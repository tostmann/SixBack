// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gabbo_watcher.h"

#ifdef SIXBACK_GABBO_WATCHER_ENABLED

#include "gabbo_ws.h"
#include "speaker_inventory.h"
#include "preset_store.h"
#include "api_endpoints.h"   // selectStationOnSpeaker
#include "config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace sixback {
namespace {

constexpr uint32_t kInitialWaitMs     = 20000;   // nach Boot: WiFi + Inventory warm
constexpr uint32_t kReconcileMs       = 15000;   // Soll/Ist-Abgleich Inventory<->Conns
constexpr uint32_t kReconnectMs       = 10000;   // pro Conn Reconnect-Throttle
constexpr uint32_t kSuppressMs        = 20000;   // Self-Select-Echo-Fenster (doPush_ haengt bis 18s)
constexpr uint32_t kPendingTimeoutMs  = 5000;    // Frist nach Selection: INVALID_SOURCE ODER kein PLAY_STATE -> Re-Arm
constexpr uint8_t  kMaxAttempts       = 3;       // Re-Arm + bis zu 2 Nachfass-Versuche je Slot pro Cooldown
constexpr uint32_t kAttemptCooldownMs = 60000;   // Cooldown fuer den Versuchs-Cap
// Nachfassen (2026-09-21, Test auf SoundTouch 10 / FW 27.0.6): ein kalter LIR-Tastendruck aus
// Standby startet dort NIE selbst und haengt die Box ohne Re-Arm auf (7/8). Der Re-Arm rettet meist
// (8/8), in einem beobachteten Feldfall blieb aber genau der EINE Versuch wirkungslos -> Box
// eingefroren. Ein spaeteres /select loeste haengende Boxen auf Testhardware zuverlaessig. Bisher gab
// es keinen zweiten Versuch: reArm_ setzte pendingSlot=-1, und das eigene /select meldet
// preset id="0" -> kein neuer Pending.
constexpr uint32_t kVerifyMs          = 10000;   // nach Re-Arm: so lange auf PLAY_STATE warten, dann nachfassen
constexpr uint32_t kVerifyBufferingMs = 20000;   // Frist ab Re-Arm, sobald die Box BUFFERING meldet (sie arbeitet)
constexpr uint32_t kPingMs            = 30000;   // periodischer WS-Ping (Liveness)
constexpr uint32_t kIdleMs            = 90000;   // kein Frame so lange -> Socket tot -> reconnect

// Mid-Stream-Rescue (2026-09-22, Test auf SoundTouch 10 / FW 27.0.6): mitten in einem laufenden
// LIR-Stream registriert die Box gelegentlich ihre Presets/Quellen neu (Flut von sourcesUpdated,
// Presetliste kurz leer) und meldet danach nowPlaying INVALID_SOURCE — der Stream ist weg, die Box
// bleibt stumm. Beobachtet im Feld und auf Testhardware, Ausloeser unbekannt. Ein erneutes /select
// derselben Station holt den Stream zurueck. Nur wenn die Box vorher auf LIR spielte und dazwischen
// weder Taste noch STANDBY/andere Quelle kam: derselbe Burst trifft auch Boxen im Standby.
constexpr uint32_t kRescueMinPlayMs    = 20000;    // so lange muss LIR vorher gespielt haben (Kaltstart = Re-Arm)
constexpr uint32_t kRescueDelayMs      = 6000;     // nach INVALID_SOURCE: Burst ausklingen lassen
constexpr uint32_t kRescueQuietMs      = 3000;     // weitere Burst-Frames schieben die Frist so weit hinaus ...
constexpr uint32_t kRescueMaxDelayMs   = 20000;    // ... aber hoechstens bis hierhin nach INVALID_SOURCE
constexpr uint32_t kRescueVerifyMs     = 20000;    // nach Rescue-/select: so lange auf PLAY_STATE warten
constexpr uint8_t  kRescueMaxAttempts  = 2;        // je Abbruch: Rescue + ein Nachfassen
constexpr uint8_t  kRescueMaxPerWindow = 4;        // Backstop gegen genuin tote Station ...
constexpr uint32_t kRescueWindowMs     = 1800000;  // ... je 30 min

// v0.8.24 #15-Hotfix: Bisher hielt der Watcher PRO Speaker eine persistente gabbo-TCP-
// Socket offen. Bei Grossinstallationen (~10 Boxen) erschoepfen 9-10 Dauer-Sockets das
// geteilte lwIP-Budget (~16 BSD-Sockets + globaler TCP-PCB-Pool) — zusammen mit einem
// Discovery-Sweep (je ein WiFiClient/Speaker), Cloud-/Spotify-TLS und OTA. Folge:
// AsyncWebServer/OTA bekommen keine Verbindung mehr -> "SixBack nicht erreichbar",
// obwohl das Geraet laeuft. Schutz: hoechstens kMaxActiveConns gabbo-Sockets GLEICHZEITIG
// offen. Hat ein Geraet mehr LIR-Speaker als das Cap, rotiert das aktive Fenster fair
// (Best-Effort-Abdeckung statt Erreichbarkeits-Verlust). Bis kMaxActiveConns Speaker:
// volle Abdeckung wie bisher, keine Rotation. 4 laesst >=12 Sockets fuer Web/OTA/Cloud frei.
constexpr uint8_t  kMaxActiveConns    = 4;       // max. gleichzeitig offene gabbo-Sockets
constexpr uint32_t kRotateMs          = 45000;   // Fenster-Rotation bei mehr LIR-Speakern als Cap

// ---------- Loop-Guard Suppress-Map (von mehreren Tasks beschrieben) ----------
SemaphoreHandle_t g_supMtx = nullptr;
std::map<String, uint32_t> g_suppress;   // spIp -> lastSelfSelectMs

// millis() des letzten eigenen /select an ip (0 = keins). Unterscheidet beim Nachfassen
// "unser Re-Arm war das letzte /select" von "WebUI/Push hat inzwischen selbst selektiert".
uint32_t lastSelfSelectMs_(const String& ip) {
    if (!g_supMtx) return 0;
    uint32_t t = 0;
    xSemaphoreTake(g_supMtx, portMAX_DELAY);
    auto it = g_suppress.find(ip);
    if (it != g_suppress.end()) t = it->second;
    xSemaphoreGive(g_supMtx);
    return t;
}

bool suppressed_(const String& ip) {
    if (!g_supMtx) return false;
    bool s = false;
    xSemaphoreTake(g_supMtx, portMAX_DELAY);
    auto it = g_suppress.find(ip);
    if (it != g_suppress.end() && (millis() - it->second) < kSuppressMs) s = true;
    xSemaphoreGive(g_supMtx);
    return s;
}

// ---------- Per-Connection State (NUR im Watcher-Task -> kein Mutex) ----------
struct Conn {
    String         deviceId;
    String         ip;
    GabboWsClient  ws;
    int            pendingSlot = -1;          // Slot aus letztem nowSelectionUpdated
    uint32_t       pendingTs = 0;
    int            verifySlot = -1;           // Slot, dessen Re-Arm noch kein PLAY_STATE gebracht hat
    uint32_t       verifyStartMs = 0;         // Zeitpunkt des letzten Re-Arm-/select
    uint32_t       verifyDeadline = 0;        // danach nachfassen
    uint32_t       verifySelMark = 0;         // g_suppress-Marke unseres /select (Fremd-Select-Erkennung)
    uint32_t       lastConnectTry = 0;
    uint32_t       lastRxMs = 0;              // letzter empfangener Frame (Liveness)
    uint32_t       lastPingMs = 0;            // letzter gesendeter WS-Ping
    uint8_t        attempts[7]   = {0,0,0,0,0,0,0};   // [slot] Versuchs-Cap
    uint32_t       attemptWin[7] = {0,0,0,0,0,0,0};
    // Mid-Stream-Rescue
    bool           lirPlaying = false;        // letzter bekannter Stand: LIR in PLAY_STATE
    uint32_t       lirSinceMs = 0;            // seit wann dieser Stream spielt
    String         lirLoc;                    // ContentItem-location des laufenden LIR-Streams
    String         lirName;                   // itemName dazu (XML-escaped wie im Frame)
    uint8_t        rescueStage = 0;           // 0 aus · 1 Abbruch erkannt, wartet · 2 /select gesendet, wartet auf PLAY_STATE
    uint32_t       rescueDropMs = 0;          // Zeitpunkt des INVALID_SOURCE
    uint32_t       rescueDue = 0;             // naechster Schritt (senden bzw. nachfassen)
    uint32_t       rescueSelMark = 0;         // g_suppress-Marke zum Vergleich (Fremd-Select-Erkennung)
    uint8_t        rescueAttempts = 0;        // /select je Abbruch
    uint8_t        rescueWinCount = 0;        // /select im laufenden Fenster
    uint32_t       rescueWinStart = 0;
    String         rescueUrl;                 // Stream-URL, die der Rescue gerade zurueckholt
    bool           seedPending = false;       // nach Connect: erst gepufferte Frames, dann /now_playing
};
std::map<String, Conn> g_conns;   // deviceId -> Conn (Knoten stabil, kein realloc-copy)

// v0.8.24: Rotations-Fenster fuer den Socket-Cap (nur Watcher-Task -> kein Mutex).
uint32_t g_rotOffset  = 0;        // Start-Index des aktiven Fensters in g_conns (geordnet)
uint32_t g_lastRotate = 0;        // millis() der letzten Fenster-Rotation

// nowPlayingUpdated -> source-Attribut von <nowPlaying ...>, sonst "".
String nowPlayingSource_(const String& f) {
    int p = f.indexOf("<nowPlaying ");
    if (p < 0) return String();
    int q = f.indexOf("source=\"", p);
    if (q < 0) return String();
    q += 8;
    int e = f.indexOf('"', q);
    return (e > q) ? f.substring(q, e) : String();
}

// nowSelectionUpdated -> preset id (1..6), sonst -1.
int parsePresetId_(const String& f) {
    int p = f.indexOf("preset id=\"");
    if (p < 0) return -1;
    p += 11;                                  // hinter das oeffnende Quote
    if (p >= (int)f.length()) return -1;
    char c = f.charAt(p);
    return (c >= '1' && c <= '6') ? (c - '0') : -1;
}

bool hasLirPreset_(const String& deviceId) {
    for (const auto& p : PresetStore::instance().getForSpeaker(deviceId))
        if (p.source == PresetSource::LOCAL_INTERNET_RADIO) return true;
    return false;
}

// location-Attribut des ersten ContentItem ab Position from (from < 0 -> ""), sonst "".
String contentItemLocation_(const String& f, int from) {
    if (from < 0) return String();
    int p = f.indexOf("<ContentItem ", from);
    if (p < 0) return String();
    int e = f.indexOf('>', p);
    int q = f.indexOf("location=\"", p);
    if (q < 0 || (e >= 0 && q > e)) return String();
    q += 10;
    int end = f.indexOf('"', q);
    return (end > q) ? f.substring(q, end) : String();
}

String nowPlayingLocation_(const String& f) { return contentItemLocation_(f, f.indexOf("<nowPlaying ")); }
String selectionLocation_(const String& f)  { return contentItemLocation_(f, f.indexOf("<preset ")); }

// nowPlaying -> <itemName> des ContentItem, sonst "".
String nowPlayingItemName_(const String& f) {
    int ci = f.indexOf("<ContentItem ", f.indexOf("<nowPlaying "));
    if (ci < 0) return String();
    int p = f.indexOf("<itemName>", ci);
    if (p < 0) return String();
    p += 10;
    int e = f.indexOf("</itemName>", p);
    return (e > p) ? f.substring(p, e) : String();
}

// Stream-URL hinter einer ContentItem-location: ORION-Envelope dekodiert, sonst die Roh-URL.
// Vergleichsbasis fuer "dieselbe Station" — unser eigenes /select-Echo traegt die ORION-Form,
// ein Tastendruck die im Preset gespeicherte Roh-URL.
String streamUrlOf_(const String& location) {
    String u, n, i;
    if (orionStationDecode(location, u, n, i)) return u;
    return unescapeXml(location);
}

// true = Box steckt in einer Multiroom-Zone (Master oder Mitglied). Was ein /select dort mit
// der Zone macht, ist ungemessen -> der Rescue haelt sich raus.
bool inZone_(const String& ip) {
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    if (!http.begin("http://" + ip + ":" + String(BOSE_BMX_PORT) + "/getZone")) return false;
    bool zone = false;
    if (http.GET() == 200) zone = http.getString().indexOf("<member") >= 0;
    http.end();
    return zone;
}

void cancelRescue_(Conn& c, const char* why) {
    if (!c.rescueStage) return;
    Serial.printf("[gabbo] %s rescue cancelled (%s)\n", c.deviceId.c_str(), why);
    c.rescueStage = 0;
}

// Frames aus einer Reconnect-Luecke fehlen (z.B. STANDBY) -> Stand verwerfen, nie weitertragen.
void resetRescue_(Conn& c) {
    c.lirPlaying  = false;
    c.rescueStage = 0;
}

// Haelt fest, ob die Box gerade LIR spielt, und erkennt den Stream-Verlust mitten im Lauf.
// Quelle: gabbo nowPlayingUpdated oder (nach Connect) BMX /now_playing — gleiches Schema.
void noteNowPlaying_(Conn& c, const String& f) {
    String src = nowPlayingSource_(f);
    if (src.length() == 0) return;
    uint32_t now = millis();
    if (src == "LOCAL_INTERNET_RADIO") {
        if (f.indexOf("STOP_STATE") >= 0 || f.indexOf("PAUSE_STATE") >= 0) {
            // Nutzer hat angehalten (Play/Pause an der Box, App): nichts retten.
            c.lirPlaying = false;
            cancelRescue_(c, "stopped");
            return;
        }
        if (f.indexOf("PLAY_STATE") < 0) return;   // BUFFERING / ohne playStatus: neutral
        String loc = nowPlayingLocation_(f);
        // Nach gelungenem Rescue desselben Streams zaehlt die Spielzeit weiter (ein zweiter
        // Abbruch kurz danach ist wieder rettbar; den Loop begrenzt der Fenster-Cap).
        if (loc != c.lirLoc || (!c.lirPlaying && c.rescueStage != 2)) c.lirSinceMs = now;
        c.lirPlaying = true;
        c.lirLoc     = loc;
        c.lirName    = nowPlayingItemName_(f);
        if (c.rescueStage == 2) {
            Serial.printf("[gabbo] %s rescue OK: PLAY_STATE %lu ms after the drop\n",
                          c.deviceId.c_str(), (unsigned long)(now - c.rescueDropMs));
        }
        c.rescueStage = 0;
        return;
    }
    if (src == "INVALID_SOURCE") {
        bool armed = c.lirPlaying && (now - c.lirSinceMs) >= kRescueMinPlayMs;
        c.lirPlaying = false;
        if (!armed || c.rescueStage != 0) return;
        if (c.pendingSlot >= 1 || c.verifySlot >= 1) return;   // Kaltstart -> Re-Arm ist zustaendig
        c.rescueStage    = 1;
        c.rescueDropMs   = now;
        c.rescueDue      = now + kRescueDelayMs;
        c.rescueSelMark  = lastSelfSelectMs_(c.ip);
        c.rescueAttempts = 0;
        Serial.printf("[gabbo] %s LIR stream lost after %lus (INVALID_SOURCE) -> rescue pending\n",
                      c.deviceId.c_str(), (unsigned long)((now - c.lirSinceMs) / 1000));
        return;
    }
    // STANDBY oder andere Quelle: der Nutzer hat aus- bzw. umgeschaltet -> nichts retten.
    c.lirPlaying = false;
    cancelRescue_(c, src.c_str());
}

// followUp=false: erster Re-Arm nach einem Tastendruck. followUp=true: Nachfassen, weil der
// vorige Re-Arm binnen kVerifyMs kein PLAY_STATE brachte — hier darf das Suppress-Fenster nicht
// greifen, denn es stammt von unserem EIGENEN Re-Arm (Fremd-Selects prueft der Aufrufer).
// true = /select wurde gesendet.
bool reArm_(Conn& c, int slot, bool followUp) {
    if (slot < 1 || slot > 6) return false;
    if (!followUp && suppressed_(c.ip)) {
        Serial.printf("[gabbo] %s slot %d INVALID_SOURCE -> suppressed (own recent /select)\n",
                      c.deviceId.c_str(), slot);
        return false;
    }
    uint32_t now = millis();
    if (now - c.attemptWin[slot] > kAttemptCooldownMs) { c.attemptWin[slot] = now; c.attempts[slot] = 0; }
    if (c.attempts[slot] >= kMaxAttempts) {
        Serial.printf("[gabbo] %s slot %d -> re-arm cap reached (source likely dead), giving up\n",
                      c.deviceId.c_str(), slot);
        return false;
    }
    Preset p = PresetStore::instance().get(c.deviceId, (uint8_t)slot);
    if (p.source != PresetSource::LOCAL_INTERNET_RADIO) {
        // Nur kalte LIR/ORION re-armen. TUNEIN-INVALID_SOURCE = Migrations-/account-
        // full-Thema (#10/#11), nicht #15. OPAQUE/STORED_MUSIC = nicht re-armbar.
        return false;
    }
    c.attempts[slot]++;
    if (followUp) {
        Serial.printf("[gabbo] %s slot %d no PLAY_STATE after re-arm -> FOLLOW-UP via ORION /select (attempt %u)\n",
                      c.deviceId.c_str(), slot, c.attempts[slot]);
    } else {
        Serial.printf("[gabbo] %s slot %d cold LIR -> RE-ARM via ORION /select (attempt %u)\n",
                      c.deviceId.c_str(), slot, c.attempts[slot]);
    }
    uint32_t t0 = millis();
    int code = selectStationOnSpeaker(c.ip, p);   // ruft intern gabboMarkSelfSelect(ip) VOR dem POST
    uint32_t mark = lastSelfSelectMs_(c.ip);
    Serial.printf("[gabbo] %s slot %d re-arm /select -> HTTP %d\n", c.deviceId.c_str(), slot, code);
    // Unsere Marke liegt direkt bei t0. Liegt die letzte Marke deutlich spaeter, hat waehrend
    // unseres POST ein anderer (WebUI play-source / doPush_) selektiert -> nicht nachfassen.
    if ((uint32_t)(mark - t0) > 50) {
        Serial.printf("[gabbo] %s slot %d no verify: /select by UI/push during re-arm\n",
                      c.deviceId.c_str(), slot);
        c.verifySlot = -1;
        return true;
    }
    // Nachfass-Frist auch bei HTTP-Fehler starten (Box evtl. kurz nicht erreichbar).
    c.verifySlot     = slot;
    c.verifyStartMs  = millis();
    c.verifyDeadline = c.verifyStartMs + kVerifyMs;
    c.verifySelMark  = mark;
    return true;
}

// Mid-Stream-Rescue faellig (Stage 1: Abbruch erkannt; Stage 2: Rescue ohne PLAY_STATE): dieselbe
// Station erneut per ORION-/select, solange weder WebUI/Push dazwischen selektiert hat noch ein Cap greift.
void rescueStep_(Conn& c) {
    if (lastSelfSelectMs_(c.ip) != c.rescueSelMark) {
        Serial.printf("[gabbo] %s rescue dropped: newer /select by UI/push\n", c.deviceId.c_str());
        c.rescueStage = 0;
        return;
    }
    if (c.rescueAttempts >= kRescueMaxAttempts) {
        Serial.printf("[gabbo] %s rescue gave up: no PLAY_STATE after %u attempt(s)\n",
                      c.deviceId.c_str(), c.rescueAttempts);
        c.rescueStage = 0;
        return;
    }
    uint32_t now = millis();
    if (now - c.rescueWinStart > kRescueWindowMs) { c.rescueWinStart = now; c.rescueWinCount = 0; }
    if (c.rescueWinCount >= kRescueMaxPerWindow) {
        // Zaehlt bewusst auch gelungene Rescues: falls ein Abbruch Folge unseres eigenen /select
        // waere, bricht nur dieser Cap die Schleife.
        Serial.printf("[gabbo] %s rescue cap reached (%u rescues in 30 min), pausing until the window resets\n",
                      c.deviceId.c_str(), c.rescueWinCount);
        c.rescueStage = 0;
        return;
    }
    Preset p;
    p.slot   = 0;
    p.source = PresetSource::LOCAL_INTERNET_RADIO;
    if (!orionStationDecode(c.lirLoc, p.streamUrl, p.name, p.imageUrl)) {
        // Roh-URL als location (nicht ueber ORION gestartet): selectStationOnSpeaker verpackt sie.
        String loc = unescapeXml(c.lirLoc);
        if (!loc.startsWith("http")) {
            Serial.printf("[gabbo] %s rescue skipped: no stream URL in location\n", c.deviceId.c_str());
            c.rescueStage = 0;
            return;
        }
        p.streamUrl = loc;
    }
    if (p.name.length() == 0) p.name = unescapeXml(c.lirName);
    if (c.rescueAttempts == 0 && inZone_(c.ip)) {
        Serial.printf("[gabbo] %s rescue skipped: speaker is in a multiroom zone\n", c.deviceId.c_str());
        c.rescueStage = 0;
        return;
    }
    c.rescueUrl = p.streamUrl;
    c.rescueAttempts++;
    c.rescueWinCount++;
    Serial.printf("[gabbo] %s LIR stream lost -> RESCUE via ORION /select (attempt %u)\n",
                  c.deviceId.c_str(), c.rescueAttempts);
    uint32_t t0 = millis();
    int code = selectStationOnSpeaker(c.ip, p);   // markiert gabboMarkSelfSelect(ip) VOR dem POST
    uint32_t mark = lastSelfSelectMs_(c.ip);
    Serial.printf("[gabbo] %s rescue /select -> HTTP %d (stack free %u)\n", c.deviceId.c_str(), code,
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if ((uint32_t)(mark - t0) > 50) {   // waehrend unseres POST hat WebUI/Push selektiert
        Serial.printf("[gabbo] %s rescue: /select by UI/push during rescue, stop\n", c.deviceId.c_str());
        c.rescueStage = 0;
        return;
    }
    c.rescueStage   = 2;
    c.rescueSelMark = mark;
    c.rescueDue     = millis() + kRescueVerifyMs;
}

// Nach (Re)Connect den Stand einmal per BMX holen, statt einen alten "spielt LIR"-Stand
// weiterzutragen: ein Burst auf einer inzwischen ausgeschalteten Box darf sie nicht einschalten.
void seedFromNowPlaying_(Conn& c) {
    resetRescue_(c);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    if (!http.begin("http://" + c.ip + ":" + String(BOSE_BMX_PORT) + "/now_playing")) return;
    if (http.GET() == 200) {
        noteNowPlaying_(c, http.getString());
        if (c.lirPlaying) Serial.printf("[gabbo] %s seeded: LIR playing\n", c.deviceId.c_str());
    }
    http.end();
}

void handleFrame_(Conn& c, const String& f) {
    if (f.indexOf("nowSelectionUpdated") >= 0) {
        int slot = parsePresetId_(f);
        c.lirPlaying = false;
        if (c.rescueStage == 2) {
            // Dieselbe Station: Echo unseres eigenen Rescue-/select oder erneuter Druck auf ihre
            // Taste (natuerliche Reaktion auf Stille) -> Nachfassen weiterlaufen lassen; ein neuer
            // Pending liefe ins Suppress-Fenster und bliebe stumm. Andere Station (auch per /select
            // ohne SixBack, z.B. aus einer Hausautomation) -> abbrechen.
            String u = streamUrlOf_(selectionLocation_(f));
            if (u.length() && u == c.rescueUrl) return;
            cancelRescue_(c, "new selection");
        } else if (c.rescueStage == 1) {
            cancelRescue_(c, "new selection");
        }
        if (slot >= 1) {
            // Nochmal DIESELBE Taste waehrend des Nachfassens (natuerliche Reaktion auf eine
            // haengende Box; auch POWER auf haengender Box meldet den letzten Slot): Rettung
            // weiterlaufen lassen — ein neuer Pending liefe ins Suppress-Fenster und bliebe stumm.
            if (slot == c.verifySlot) return;
            // Andere Taste ersetzt ein laufendes Nachfassen.
            c.pendingSlot = slot; c.pendingTs = millis(); c.verifySlot = -1;
        }
        return;
    }
    if (f.indexOf("nowPlayingUpdated") >= 0) {
        noteNowPlaying_(c, f);
    } else if (c.rescueStage == 1 &&
               (f.indexOf("<sourcesUpdated") >= 0 || f.indexOf("<presetsUpdated") >= 0)) {
        // Burst laeuft noch -> Rescue erst, wenn die Box sich beruhigt hat (mit Obergrenze).
        uint32_t due = millis() + kRescueQuietMs;
        uint32_t cap = c.rescueDropMs + kRescueMaxDelayMs;
        if ((int32_t)(due - cap) > 0) due = cap;
        if ((int32_t)(due - c.rescueDue) > 0) c.rescueDue = due;
    }
    if (f.indexOf("PLAY_STATE") >= 0) { c.pendingSlot = -1; c.verifySlot = -1; return; }   // Erfolg -> nichts tun
    // Box ist auf etwas anderem als unserer LIR-Quelle (STANDBY = Nutzer hat ausgeschaltet,
    // AUX/BT/App-Wahl ...): weder re-armen noch nachfassen — unser /select wuerde die Box sonst
    // wieder einschalten bzw. die Wahl des Nutzers ueberfahren. (Zwischen Tastendruck und Re-Arm
    // sendet die Box kein nowPlayingUpdated, gemessen 09-21 — ein solcher Frame dort ist echt.)
    if (f.indexOf("nowPlayingUpdated") >= 0 && (c.pendingSlot >= 1 || c.verifySlot >= 1)) {
        String src = nowPlayingSource_(f);
        if (src.length() && src != "LOCAL_INTERNET_RADIO" && src != "INVALID_SOURCE") {
            Serial.printf("[gabbo] %s source %s -> cancel re-arm/follow-up\n",
                          c.deviceId.c_str(), src.c_str());
            c.pendingSlot = -1; c.verifySlot = -1; return;
        }
    }
    // Box puffert nach unserem Re-Arm -> sie arbeitet; Frist einmalig bis kVerifyBufferingMs strecken.
    if (c.verifySlot >= 1 && f.indexOf("BUFFERING_STATE") >= 0) {
        c.verifyDeadline = c.verifyStartMs + kVerifyBufferingMs;
        return;
    }
    if (f.indexOf("INVALID_SOURCE") >= 0 || f.indexOf("<errorUpdate") >= 0) {
        if (c.pendingSlot >= 1 && (millis() - c.pendingTs) < kPendingTimeoutMs) {
            int slot = c.pendingSlot;
            c.pendingSlot = -1;
            reArm_(c, slot, false);
        }
        return;
    }
}

void reconcile_() {
    auto snap = SpeakerInventory::instance().list();   // copy-by-value, lock-frei
    std::map<String, String> desired;                  // deviceId -> ip
    for (const auto& sp : snap) {
        if (!sp.ownedByUs) continue;
        if (sp.status == MigrationStatus::OFFLINE) continue;
        if (sp.ip.length() == 0) continue;
        if (!hasLirPreset_(sp.deviceId)) continue;     // nur Speaker mit kalt-faehigen LIR-Slots
        desired[sp.deviceId] = sp.ip;
    }
    // entfernen, was nicht mehr gewuenscht ist
    for (auto it = g_conns.begin(); it != g_conns.end(); ) {
        if (desired.find(it->first) == desired.end()) {
            Serial.printf("[gabbo] drop %s (not owned/migrated or no LIR preset)\n", it->first.c_str());
            it->second.ws.close();
            it = g_conns.erase(it);
        } else {
            ++it;
        }
    }
    // hinzufuegen / IP-Drift behandeln
    for (auto& d : desired) {
        auto it = g_conns.find(d.first);
        if (it == g_conns.end()) {
            Conn& c = g_conns[d.first];
            c.deviceId = d.first;
            c.ip = d.second;
            c.lastConnectTry = 0;
            Serial.printf("[gabbo] track %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
        } else if (it->second.ip != d.second) {
            Serial.printf("[gabbo] %s ip %s -> %s (reconnect)\n",
                          it->first.c_str(), it->second.ip.c_str(), d.second.c_str());
            it->second.ws.close();
            resetRescue_(it->second);
            it->second.ip = d.second;
            it->second.lastConnectTry = 0;
        }
    }
}

void watcherTask_(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(kInitialWaitMs));
    Serial.println("[gabbo] watcher started");
    uint32_t lastReconcile = 0;
    while (true) {
        uint32_t now = millis();
        if (now - lastReconcile > kReconcileMs) { reconcile_(); lastReconcile = now; }
        // v0.8.24 Socket-Cap: aktives Fenster bestimmen (+ ggf. rotieren), damit nie mehr
        // als kMaxActiveConns gabbo-Sockets gleichzeitig offen sind.
        const size_t nConns = g_conns.size();
        if (nConns) g_rotOffset %= nConns;          // normalisieren (nConns kann via reconcile schrumpfen)
        if (nConns > kMaxActiveConns && millis() - g_lastRotate > kRotateMs) {
            g_lastRotate = millis();
            g_rotOffset  = (g_rotOffset + kMaxActiveConns) % nConns;
        }
        size_t connIdx = 0;
        for (auto& kv : g_conns) {
            Conn& c = kv.second;
            // Bis zum Cap alle aktiv (keine Rotation); darueber nur das rotierende Fenster
            // [g_rotOffset, g_rotOffset+kMaxActiveConns) modulo nConns.
            bool active = (nConns <= kMaxActiveConns) ||
                          (((connIdx + nConns - g_rotOffset) % nConns) < kMaxActiveConns);
            ++connIdx;
            if (!active) {
                // Inaktiv: Socket freigeben -> lwIP-Budget bleibt fuer Web/OTA/Cloud frei.
                // pendingSlot loeschen, sonst feuert beim Wiedereintritt ein stale Re-Arm.
                if (c.ws.connected()) c.ws.close();
                c.pendingSlot = -1;
                c.verifySlot = -1;
                resetRescue_(c);
                continue;
            }
            if (!c.ws.connected()) {
                if (millis() - c.lastConnectTry > kReconnectMs) {
                    c.lastConnectTry = millis();
                    if (c.ws.connect(c.ip, BOSE_GABBO_WS_PORT, 1500)) {
                        c.lastRxMs = millis();
                        c.lastPingMs = millis();
                        Serial.printf("[gabbo] connected %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
                        c.seedPending = true;   // erst gepufferte Frames (aelter), dann /now_playing (neuer)
                    } else {
                        Serial.printf("[gabbo] connect failed %s @ %s\n", c.deviceId.c_str(), c.ip.c_str());
                    }
                }
                continue;
            }
            String frame;
            int budget = 32;                  // pro Conn max. 32 Frames je Runde
            while (budget-- > 0 && c.ws.poll(frame)) { c.lastRxMs = millis(); handleFrame_(c, frame); }
            if (c.seedPending) { c.seedPending = false; seedFromNowPlaying_(c); }
            // Liveness: ein still verschwundener Speaker (kein FIN/RST) laesst
            // connected() ewig true -> sonst verpasst der Watcher JEDEN Press.
            // gabbo sendet im Normalbetrieb regelmaessig Frames; periodischer Ping
            // erzwingt Antwort/Schreibfehler, kein Frame fuer kIdleMs = Socket tot
            // -> close -> Reconnect ueber den !connected()-Pfad.
            if (millis() - c.lastPingMs > kPingMs) {
                c.lastPingMs = millis();
                if (!c.ws.ping()) { c.ws.close(); c.verifySlot = -1; resetRescue_(c); continue; }
            }
            if (millis() - c.lastRxMs > kIdleMs) {
                Serial.printf("[gabbo] %s idle -> reconnect\n", c.deviceId.c_str());
                c.ws.close();
                c.verifySlot = -1;   // Frames im Reconnect-Loch (z.B. STANDBY) waeren verpasst
                resetRescue_(c);
                continue;
            }
            // Timeout-Trigger: LIR-Slot selektiert, aber kein PLAY_STATE in der Frist.
            // Deckt den "live-but-stuck"-Modus ab: eine erreichbare Roh-URL liefert
            // HTTP 200/Audio statt eines Station-Deskriptors -> der Speaker wirft KEIN
            // INVALID_SOURCE, steckt aber ohne playStatus fest. Das ist der REALE
            // #15-Fall (echte LIR-Streams sind live). On-device verifiziert 06-11.
            if (c.pendingSlot >= 1 && (millis() - c.pendingTs) > kPendingTimeoutMs) {
                int slot = c.pendingSlot;
                c.pendingSlot = -1;
                reArm_(c, slot, false);
            }
            // Nachfassen: Re-Arm gesendet, aber binnen Frist kein PLAY_STATE. Nur wenn unser Re-Arm
            // noch das letzte /select an diese Box ist (sonst hat WebUI/Push uebernommen).
            if (c.verifySlot >= 1 && (int32_t)(millis() - c.verifyDeadline) >= 0) {
                int slot = c.verifySlot;
                c.verifySlot = -1;
                if (lastSelfSelectMs_(c.ip) != c.verifySelMark) {
                    Serial.printf("[gabbo] %s slot %d no follow-up: newer /select by UI/push\n",
                                  c.deviceId.c_str(), slot);
                } else {
                    reArm_(c, slot, true);
                }
            }
            // Mid-Stream-Rescue: Abbruch erkannt und Burst ausgeklungen bzw. Rescue ohne PLAY_STATE.
            if (c.rescueStage && (int32_t)(millis() - c.rescueDue) >= 0) rescueStep_(c);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} // anon

void gabboMarkSelfSelect(const String& spIp) {
    if (!g_supMtx) return;
    xSemaphoreTake(g_supMtx, portMAX_DELAY);
    g_suppress[spIp] = millis();
    xSemaphoreGive(g_supMtx);
}

void startGabboWatcher() {
    static bool started = false;
    if (started) return;
    started = true;
    // Der s3-Build (qio_opi, BOARD_HAS_PSRAM) laeuft im Feld auch auf Boards
    // OHNE bestuecktes PSRAM (S3-R2/FN8). Dort degradieren Spotify/mbedtls-
    // Hook/Outbound-Floors bereits runtime — gabbo zieht als letztes nach:
    // keine 4 Dauer-Sockets + 8KB-Task auf dem knappen internen Heap.
    // g_supMtx bleibt nullptr -> gabboMarkSelfSelect() ist sauberes no-op.
    if (ESP.getPsramSize() == 0) {
        Serial.println("[gabbo] no PSRAM -> watcher disabled");
        return;
    }
    g_supMtx = xSemaphoreCreateMutex();
    BaseType_t r = xTaskCreate(watcherTask_, "gabbo-watch", 8192,
                               nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        Serial.println("[gabbo] watcher spawn FAILED");
        started = false;
    }
}

} // namespace sixback

#endif // SIXBACK_GABBO_WATCHER_ENABLED
