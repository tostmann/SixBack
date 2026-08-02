// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Verwaltungs-REST-API (Port 80)
//
// Vollstaendige Speaker-/Preset-/Gruppen-Verwaltung. Frontend (Web-UI)
// nutzt diese Endpoints und bekommt damit alle Funktionalitaet:
//   - Speaker entdecken + listen
//   - Pro Speaker: Migration / Revert / Reboot
//   - Pro Speaker: 6 Presets verwalten
//   - Gruppen-Sync (Preset-Set von Speaker A auf B,C,...)
//   - TuneIn-Lookup + Suche

#include "api_endpoints.h"
#include "web_router.h"
#include "version.h"
#include "config.h"
#include "speaker_telnet.h"
#include "wifi_provisioning.h"
#include "speaker_inventory.h"
#include "zone_manager.h"
#include "preset_store.h"
#include "tunein_resolver.h"
#include "system_health.h"
#include "captive_portal.h"
#include "ota_pull.h"
#include "dlna_browse.h"
#include "auto_mode.h"
#include "ui_auth.h"
#include "source_normalizer.h"
#include "bose_endpoints.h"
#include "event_store.h"
#include "ip_failsafe.h"
#include "nvs_helper.h"
#include "gabbo_watcher.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include "speaker_diagnostic.h"
#include "diag_settings.h"
#include "host_settings.h"
#include "spotify_player.h"
#include "stream_library.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <LittleFS.h>
#include <Update.h>
#include <Preferences.h>
#include <atomic>

// -----------------------------------------------------------------------------
// Heap-/Concurrency-Guard fuer synchrone Outbound-Handler (FHEM 144729 #121).
//
// handleGetHardwarePresets + handleNowPlayingLive holen /presets bzw.
// /now_playing SYNCHRON per HTTPClient.GET() im AsyncWebServer-Callback (blockt
// den einzelnen async_tcp-Task bis zu ~5s) und akkumulieren den XML-Body als
// String. Beim Oeffnen der WebUI feuert der Browser diese Reads fuer JEDEN
// gefundenen Speaker gleichzeitig -> auf dem no-PSRAM-C5 (~250KB Heap) stauen
// sich TCP-Backlog + HTTPClient-Puffer + String-Bodies -> Heap-Kollaps -> Crash
// -> WLAN weg. (Lab-verifiziert 2026-07-04: min_free 52KB->22.9KB, crash++.)
//
// Fix: vor der teuren Outbound-Arbeit Heap-Boden + max-Inflight pruefen; darunter
// sofort 503 "busy" (die WebUI retryt ohnehin). So bleibt der async_tcp-Task
// frei zum Draenen des Connection-Backlogs statt zu crashen. PSRAM-Boards (S3)
// haben Reserve -> grosszuegig; no-PSRAM (C5/C6/C3) -> streng.
// -----------------------------------------------------------------------------
namespace {
std::atomic<int>      g_outboundInflight{0};
std::atomic<uint32_t> g_outboundRejects{0};   // 503-Zaehler (runtime, KEIN NVS) —
                                              // forensisch via /api/status ablesbar
// Per-Ursache-Aufschluesselung (FHEM 144729 #155): "low heap" im UI kann
// total-Floor, largest-Block ODER Inflight-Cap sein — ohne Aufschluesselung
// ist ein Feld-Report wie betateilchens 4.-Box-Fall nicht diagnostizierbar.
std::atomic<uint32_t> g_rejFloorTotal{0};
std::atomic<uint32_t> g_rejFloorBlock{0};
std::atomic<uint32_t> g_rejInflight{0};

// -----------------------------------------------------------------------------
// SIXBACK_CONTIG_PROBE — Lab-Instrumentierung, NICHT im Release-Build (eigene
// pio-Envs *-probe). Hintergrund: outboundBlockFloor() ist mit 16000 fest
// verdrahtet und der EINZIGE Guard-Zweig, der nicht mit PSRAM skaliert; der
// Check laeuft auf MALLOC_CAP_INTERNAL, PSRAM hilft dort per Konstruktion
// nicht. betateilchens C5-N16R8-Dump (FHEM 144729 msg1367262) zeigt 27 von 27
// Rejects aus genau diesem Zweig bei largest_block 15348. Diese Sonde misst,
// wieviel zusammenhaengenden INTERNAL-Heap die zwei Live-Handler real
// brauchen, damit die Schwelle gegen Zahlen statt gegen Bauchgefuehl steht.
#if SIXBACK_CONTIG_PROBE
#include <vector>
std::atomic<uint32_t> g_probeRuns{0};
std::atomic<uint32_t> g_probeFails{0};
std::atomic<uint32_t> g_probeLbWorst{0xFFFFFFFFu};  // kleinster je gesehener largest_block
std::atomic<uint32_t> g_probeLbDropMax{0};          // groesster (entry-min) ueber alle Laeufe
std::atomic<uint32_t> g_probeFreeDropMax{0};
std::atomic<uint32_t> g_probeBodyMax{0};
std::atomic<uint32_t> g_probeFloor{16000};          // laufzeit-verstellbar (Floor-Sweep)

// Ein Sampler-Objekt pro Handler-Lauf, auf dem Stack. Die Live-Handler laufen
// alle im EINEN async_tcp-Task, es gibt also keine Verschachtelung; nur die
// Aggregate unten sind atomar.
struct ContigSampler {
    uint32_t lbEntry, lbMin, freeEntry, freeMin;
    static uint32_t lb() { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); }
    static uint32_t fr() { return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
    ContigSampler() { lbEntry = lbMin = lb(); freeEntry = freeMin = fr(); }
    void mark() {
        uint32_t a = lb(); if (a < lbMin) lbMin = a;
        uint32_t b = fr(); if (b < freeMin) freeMin = b;
    }
    void done(uint32_t bodyLen, bool ok) {
        mark();
        g_probeRuns.fetch_add(1, std::memory_order_relaxed);
        if (!ok) g_probeFails.fetch_add(1, std::memory_order_relaxed);
        uint32_t prev = g_probeLbWorst.load(std::memory_order_relaxed);
        while (lbMin < prev && !g_probeLbWorst.compare_exchange_weak(prev, lbMin)) {}
        uint32_t drop = lbEntry > lbMin ? lbEntry - lbMin : 0;
        prev = g_probeLbDropMax.load(std::memory_order_relaxed);
        while (drop > prev && !g_probeLbDropMax.compare_exchange_weak(prev, drop)) {}
        uint32_t fdrop = freeEntry > freeMin ? freeEntry - freeMin : 0;
        prev = g_probeFreeDropMax.load(std::memory_order_relaxed);
        while (fdrop > prev && !g_probeFreeDropMax.compare_exchange_weak(prev, fdrop)) {}
        prev = g_probeBodyMax.load(std::memory_order_relaxed);
        while (bodyLen > prev && !g_probeBodyMax.compare_exchange_weak(prev, bodyLen)) {}
    }
};
// Ballast: schneidet den groessten freien INTERNAL-Block gezielt klein, damit
// sich die Feldlage (betateilchen: largest_block 15348) im Lab herstellen
// laesst. Ohne das testet man nur "Floor niedrig, Heap riesig" = nichtssagend.
std::vector<void*> g_ballast;
void ballastFree() {
    for (void* p : g_ballast) heap_caps_free(p);
    g_ballast.clear();
}
void ballastTo(uint32_t target) {
    ballastFree();
    if (target == 0) return;
    for (int i = 0; i < 64; ++i) {
        uint32_t lb = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (lb <= target) break;
        size_t take = lb - target;
        if (take < 128) break;
        // Harte Untergrenze: unter ~30 KB freiem INTERNAL-Heap stirbt der
        // WiFi-/TCP-Stack mit. Lab-Vorfall 2026-07-30: ballast=8000 hat das
        // Board getoetet und beim Reboot das NVS mitgenommen. Lieber das
        // lb-Ziel verfehlen als das Board verlieren.
        uint32_t freeNow = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (freeNow < 30000u || freeNow - take < 30000u) break;
        void* p = heap_caps_malloc(take, MALLOC_CAP_INTERNAL);
        if (!p) break;
        g_ballast.push_back(p);
    }
}

#define PROBE_SAMPLER(nm) ContigSampler nm
#define PROBE_MARK(nm)    nm.mark()
#define PROBE_DONE(nm, len, ok) nm.done((uint32_t)(len), (ok))
#else
#define PROBE_SAMPLER(nm) do {} while (0)
#define PROBE_MARK(nm)    do {} while (0)
#define PROBE_DONE(nm, len, ok) do {} while (0)
#endif

inline bool     outboundHasPsram()    { return ESP.getPsramSize() > 0; }
// no-PSRAM-Floor 45K -> 30K (Lab .58 2026-07-17): 45K war auf die Vor-.32-Lage
// ohne Outbound-Timeouts kalibriert. Seit .32 ist der Drain pro Handler auf
// 600/800 ms + inflight<=2 gebunden; gemessener Normalbetrieb C5 dippt auf
// ~61K (UI-Burst) bzw. min_free 28.7K ueber 8.7h ohne Kollaps. Der harte
// Accept-Storm-Kollaps (#121-Klasse) passiert bei free >=48K und wird von
// KEINEM Handler-Floor verhindert — 45K blockierte also nur den Normalbetrieb
// (betateilchen #155: Baseline 45-62K = Dauer-Reject).
inline uint32_t outboundHeapFloor()   { return outboundHasPsram() ? 20000u : 30000u; }
inline int      outboundMaxInflight() { return outboundHasPsram() ? 8 : 2; }
// Contiguous-Bedarf der zwei leichten Live-Handler (HTTPClient-RX-Puffer +
// getString()-Body von /presets bzw. /now_playing = wenige KB) — bewusst
// GETRENNT vom total-free-Floor oben, siehe Guard-Kommentar (FHEM 144729 #153).
//
// 16000 -> 8000 (v0.8.38, Lab-gemessen 2026-07-30 auf c5-16mb gegen die echten
// Speaker; Anlass: betateilchens C5-N16R8-Dump FHEM 144729 msg1367262 mit 27
// von 27 Rejects aus GENAU diesem Zweig bei largest_block 15348):
//   * Die Speaker antworten chunked OHNE Content-Length -> getString() kann
//     nicht vorab reservieren, der String waechst per realloc. Groesster Body
//     im Lab: /presets 1883 B, /now_playing 158 B.
//   * Groesster gemessener contiguous ENTZUG pro Handler-Lauf: 6144 B
//     (96 Laeufe seriell + 6er-Burst, 0 Fehler). Im engen Heap sogar nur
//     0-2048 B — der Allokator bedient dann aus kleineren Bloecken, statt den
//     groessten anzuschneiden.
//   * Floor-Sweep bei kuenstlich auf 15348 gedruecktem largest_block:
//     Floor 16000 -> 36/36 = 503 (VOLLSTAENDIGER Dauer-Reject, keine Spitze);
//     Floor 8000/6000/4000 -> 60/60 = 200, null Fehler, null kaputte Bodies.
//   Also: 16000 hat nicht die Allokation geschuetzt, sondern den Normalbetrieb
//   auf fragmentierten PSRAM-Boards blockiert — dieselbe Fehlerklasse wie die
//   .32-Regression, nur eine Stufe tiefer. 8000 deckt den 6144er-Worst-Case
//   mit Luft und ist der einzige Guard-Wert, der bewusst NICHT mit PSRAM
//   skaliert (der Check laeuft auf MALLOC_CAP_INTERNAL, PSRAM hilft dort
//   per Konstruktion nicht — der Bedarf ist chip-unabhaengig, er haengt am
//   Speaker-Body).
//   NICHT gemessen: die Bruchstelle unterhalb largest_block ~15 K. Wer hier
//   weiter senken will, misst das erst (Envs *-probe, /api/dbg/contig).
#if SIXBACK_CONTIG_PROBE
inline uint32_t outboundBlockFloor()  { return g_probeFloor.load(std::memory_order_relaxed); }
#else
inline uint32_t outboundBlockFloor()  { return 8000u; }
#endif

// FHEM 144729 (v0.8.32): kurze, empirisch abgesicherte Timeouts fuer die zwei
// synchronen Live-Outbound-Handler. Lab-Messung (Kueche/Emma/Greta, wach):
// /presets + /now_playing antworten in <60 ms; der alte 3s-Read (bzw. FEHLENDE
// Timeout in now-playing-live -> HTTPClient-Default ~5s) hielt bei einem
// nicht-antwortenden Standby-Speaker den EINEN async_tcp-Task sekundenlang fest
// -> Connection-Backlog + Heap-Drain -> Stick "taub" ohne Disassoc (lab-repro).
// 600/800 ms = >13x Marge ueber gemessenem Max, kappt die worst-case-Blockade.
constexpr uint16_t LIVE_OUT_CONNECT_MS = 600;
constexpr uint16_t LIVE_OUT_READ_MS    = 800;

struct OutboundGuard {
    bool acquired = false;
    OutboundGuard() {
        // Doppel-Bedingung (v0.8.33-Fix, FHEM 144729 #153):
        // (1) total-free-Floor wie v0.8.30/31 — Kollaps-Schutz gegen den
        //     #121-Heap-Absturz unter Page-Load-Concurrency.
        // (2) largest-block-Floor fuer die contiguous HTTPClient/String-
        //     Allokation der zwei leichten Handler — Fragmentierungs-Check.
        // REGRESSION v0.8.32: dort wurde largest_free_block gegen den ALTEN
        // total-Floor (45K no-PSRAM) gemessen. Der war fuer die getFreeHeap-
        // SUMME kalibriert; largest block liegt auf dem no-PSRAM-C5 schon
        // beim normalen WebUI-Lade-Burst (6 gequeueste Connections) darunter
        // -> Guard lehnte praktisch JEDEN Per-Speaker-Read ab, WebUI dauerhaft
        // ohne Presets ("device busy"), obwohl free ~70-80K gesund war
        // (Lab-Repro 2026-07-17: seriell 10/10 200er, UI-Burst 17/18 503er).
        // Die Handler brauchen contiguous nur wenige KB (RX-Puffer + XML-Body)
        // -> eigener Floor 16K statt 45K; 45K-total bleibt als Kollaps-Netz.
        if (ESP.getFreeHeap() < outboundHeapFloor()) {
            g_rejFloorTotal.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < outboundBlockFloor()) {
            g_rejFloorBlock.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        int n = g_outboundInflight.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > outboundMaxInflight()) {
            g_outboundInflight.fetch_sub(1, std::memory_order_relaxed);
            g_rejInflight.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        acquired = true;
    }
    ~OutboundGuard() {
        if (acquired) g_outboundInflight.fetch_sub(1, std::memory_order_relaxed);
    }
};

// true = abgelehnt (503 gesendet), Handler soll sofort returnen.
inline bool outboundReject(AsyncWebServerRequest* req) {
    g_outboundRejects.fetch_add(1, std::memory_order_relaxed);
    req->send(503, "application/json",
              "{\"error\":\"device busy (low heap) \\u2014 retry\",\"retry\":true}");
    return true;
}

// -----------------------------------------------------------------------------
// Micro-Cache fuer die zwei synchronen Live-Outbound-Handler (FHEM 144729,
// v0.8.32). ALLE AsyncWebServer-Handler laufen im selben async_tcp-Task
// (serialisiert) -> KEIN Mutex noetig. Ein kurzes TTL-Fenster kollabiert den
// Cold-Load-Fan-out (je 1 Request pro Speaker), die kickPoll-Bursts (0/800/
// 2500 ms) und neuer-Tab-Reloads auf max EINEN echten Outbound pro
// (Speaker x Endpoint) pro TTL -> weniger Blockaden, weniger Heap-Druck.
// Nur erfolgreiche (200) Bodies werden gecached; Fehler bleiben un-gecached.
// -----------------------------------------------------------------------------
constexpr uint32_t OUT_CACHE_TTL_MS = 2500;
constexpr int      OUT_CACHE_N      = 12;   // 6 Speaker x 2 Endpoints
struct OutCacheEntry {
    String   key;
    String   body;
    uint32_t expiresMs = 0;
};
OutCacheEntry g_outCache[OUT_CACHE_N];

bool outCacheGet(const String& key, String& body) {
    uint32_t now = millis();
    for (auto& e : g_outCache) {
        if (e.expiresMs > now && e.key == key) { body = e.body; return true; }
    }
    return false;
}
void outCachePut(const String& key, const String& body) {
    uint32_t now = millis();
    OutCacheEntry* victim = nullptr;
    uint32_t oldest = UINT32_MAX;
    for (auto& e : g_outCache) {
        if (e.key == key)          { victim = &e; break; }   // gleicher key -> ersetzen
        if (e.expiresMs <= now)    { victim = &e; break; }   // freier/abgelaufener Slot
        if (e.expiresMs < oldest)  { oldest = e.expiresMs; victim = &e; }
    }
    if (!victim) victim = &g_outCache[0];
    victim->key = key;
    victim->body = body;
    victim->expiresMs = now + OUT_CACHE_TTL_MS;
}
} // namespace

// Setzt einen TUNEIN/LIR-Sender per /select an einen Speaker (plain HTTP :8090).
// ACHTUNG: doPush_ haelt eine bewusst getrennte, BYTE-IDENTISCHE Kopie dieses
// /select-ContentItem-Schemas (braucht den HTTP-Code fuer seine Abort-Logik +
// macht danach waitForPlayState_ + Long-Press). Bei Schema-Aenderung (type/
// sourceAccount/location/Attribute) BEIDE Stellen anpassen: hier UND doPush_.
// Markiert gabboMarkSelfSelect
// VOR dem Push, damit das resultierende nowSelectionUpdated nicht als HW-Press
// re-armed wird (gabbo-Loop-Guard). type="stationurl" Pflicht (type="url" ->
// Speaker STANDBY); sourceAccount="" (empirisch Emma: "TuneIn" -> 500 1005).
// MUSS am globalen Scope stehen (NICHT im anon-namespace unten), sonst laesst
// sich der sixback::-qualifizierte Name nicht definieren.
int sixback::selectStationOnSpeaker(const String& spIp, const sixback::Preset& p) {
    sixback::gabboMarkSelfSelect(spIp);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(2000);   // Watcher-Task nicht lange blockieren (Re-Arm-Kontext)
    http.setTimeout(3000);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/select";
    int code = -1;
    if (http.begin(url)) {
        const char* ciType = "stationurl";
        String ci = "<ContentItem source=\"";
        ci += sixback::presetSourceToStr(p.source);
        ci += "\" type=\""; ci += ciType;
        ci += "\" location=\"";
        if (p.source == sixback::PresetSource::TUNEIN) {
            ci += "/v1/playback/station/" + p.stationId;
        } else {
            ci += sixback::orionStationLocation(p.streamUrl, p.name, p.imageUrl);
        }
        ci += "\" sourceAccount=\"\" isPresetable=\"true\"><itemName>" + sixback::escapeXml(p.name) + "</itemName></ContentItem>";
        http.addHeader("Content-Type", "text/xml");
        code = http.POST(ci);
        http.end();
    }
    return code;
}

namespace {

// -----------------------------------------------------------------------------
// JSON-Body-Akkumulator fuer POST/PUT — Helper
// -----------------------------------------------------------------------------
using JsonHandler = std::function<void(AsyncWebServerRequest*, JsonDocument&)>;

void routeJsonBody(AsyncWebServer& s, const String& uri, WebRequestMethodComposite m,
                   JsonHandler cb) {
    routeT(s, uri.c_str(), m,
        [](AsyncWebServerRequest* req){ /* finalized by body handler */ },
        nullptr,
        [cb](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t idx, size_t total){
            if (!req->_tempObject) {
                req->_tempObject = new String();
                ((String*)req->_tempObject)->reserve(total + 1);
                // Framework-Destructor ruft free() auf _tempObject; das kennt
                // den String-Heap-Buffer nicht und leakt ihn bei Abbruch.
                // onDisconnect ueberbruecken: korrekt deleten + nullen, damit
                // free(nullptr) im Destructor harmlos ist.
                req->onDisconnect([req]() {
                    if (req->_tempObject) {
                        delete static_cast<String*>(req->_tempObject);
                        req->_tempObject = nullptr;
                    }
                });
            }
            String* buf = (String*)req->_tempObject;
            buf->concat((const char*)data, len);
            if (idx + len == total) {
                JsonDocument doc;
                auto err = deserializeJson(doc, *buf);
                delete buf;
                req->_tempObject = nullptr;
                if (err) { req->send(400, "application/json",
                    String("{\"error\":\"bad json: ") + err.c_str() + "\"}"); return; }
                cb(req, doc);
            }
        });
}

String myBaseUrl() {
    return "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
}

// RFC3986-percent-encode fuer Query-Parameter. Nur die unreservierten
// ASCII-Zeichen bleiben unverwandelt; alles andere wird %XX. Speziell
// '&' muss escaped werden, sonst splittet ein User-Sucher wie "Bob & Friends"
// die TuneIn-Search-URL in zwei Query-Parameter.
String urlEncodeQuery_(const String& in) {
    String out;
    out.reserve(in.length() * 3);
    for (size_t i = 0; i < in.length(); ++i) {
        unsigned char c = (unsigned char)in.charAt(i);
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// GET /api/status  — System-Health
// -----------------------------------------------------------------------------
void handleStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["name"]       = FW_NAME;
    doc["version"]    = FW_VERSION_STRING;
    doc["build"]      = FW_BUILD_DATE;
    doc["license"]    = "PolyForm-Noncommercial-1.0.0";
    doc["copyright"]  = "Copyright (c) 2026 Dirk Tostmann";
    doc["uptime_s"]   = millis() / 1000;

    JsonObject wifi   = doc["wifi"].to<JsonObject>();
    wifi["connected"] = WiFi.status() == WL_CONNECTED;
    wifi["ssid"]      = WiFi.SSID();
    wifi["ip"]        = WiFi.localIP().toString();
    wifi["rssi"]      = WiFi.RSSI();
    // Kanal + Band-Sichtbarkeit (v0.8.28, ESP32-C5-Dualband): Kanal 1-14 = 2.4 GHz,
    // >=36 = 5 GHz. WiFi.getBand() ist im Framework per #if SOC_WIFI_SUPPORT_5G
    // gekapselt -> liefert auf 2.4-only-Chips (esp32/s3/c3/c6) immer WIFI_BAND_2G,
    // also cross-target sicher. Beantwortet "auf welchem Band haengt das Geraet?".
    wifi["channel"]   = WiFi.channel();
    wifi["band"]      = (WiFi.getBand() == WIFI_BAND_5G) ? "5GHz" : "2.4GHz";
    wifi["mac"]       = WiFi.macAddress();
    wifi["hostname"]  = sixback::hostname() + ".local";
    wifi["improv_active"]   = sixback::improvIsActive();
    wifi["improv_window_s"] = sixback::improvWindowRemainingS();
    wifi["captive_active"]   = sixback::captiveIsActive();
    wifi["captive_window_s"] = sixback::captiveWindowRemainingS();

    JsonObject heap   = doc["heap"].to<JsonObject>();
    heap["free"]      = ESP.getFreeHeap();
    heap["min_free"]  = ESP.getMinFreeHeap();
    heap["total"]     = ESP.getHeapSize();
    // FHEM 144729 #153: Fragmentierungs-Sicht + Guard-Forensik. largest_block
    // ist die Groesse, gegen die der OutboundGuard prueft (internal RAM);
    // outbound_rejects zaehlt gesendete 503er seit Boot (runtime-only).
    heap["largest_block"]    = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    heap["outbound_rejects"] = g_outboundRejects.load(std::memory_order_relaxed);
    // Aufschluesselung nach Reject-Ursache (FHEM 144729 #155) — nur wenn
    // ueberhaupt rejected wurde, sonst Status-Noise fuer alle.
    if (g_outboundRejects.load(std::memory_order_relaxed) > 0) {
        heap["rej_floor_total"] = g_rejFloorTotal.load(std::memory_order_relaxed);
        heap["rej_floor_block"] = g_rejFloorBlock.load(std::memory_order_relaxed);
        heap["rej_inflight"]    = g_rejInflight.load(std::memory_order_relaxed);
    }
    JsonObject psram  = doc["psram"].to<JsonObject>();
    psram["free"]     = ESP.getFreePsram();
    psram["total"]    = ESP.getPsramSize();
    esp_chip_info_t info; esp_chip_info(&info);
    JsonObject chip   = doc["chip"].to<JsonObject>();
    chip["model"]     = sixback::chipModelStr();
    chip["cores"]     = info.cores;
    chip["revision"]  = info.revision;
    // Layout-Identitaet (Issue #23): unterscheidet die 16-MB- von der
    // 8-MB-S3-Variante — fuer Support-Faelle ("welchen Webflasher-Button?").
    // getFlashChipSize() liest die Image-Header-Konfiguration = geflashtes Layout.
    chip["flash_size"] = ESP.getFlashChipSize();
    JsonObject cloud  = doc["cloud_replacement"].to<JsonObject>();
    cloud["port"]     = BOSE_HTTP_PORT;
    cloud["base_url"] = myBaseUrl();
    doc["speakers_count"]    = sixback::SpeakerInventory::instance().list().size();
    doc["scan_in_progress"]  = sixback::SpeakerInventory::instance().isScanRunning();
    // Stereo-Paar-Persistenz-Health (Diagnostik): false = letzter bfx_groups-Save
    // scheiterte (NVS voll trotz Cleanup) → Gruppe nur im RAM. Der Speaker bekommt
    // weiterhin 201/200 (BMX-Contract), der Fehlschlag ist NUR hier sichtbar.
    doc["groups_persist_ok"] = groupsPersistOk();

    // Preset-Store-Forensik (FHEM 144729 #153): load_ok=false = beim Boot lag
    // ein Store-Blob in NVS, war aber unlesbar (Store startete leer, /full
    // gated auf 404); save_fails = fehlgeschlagene NVS-Saves seit Boot
    // (inkl. JsonDocument-Overflow); speakers = geladene Store-Slices.
    {
        auto& ps = sixback::PresetStore::instance();
        JsonObject pstore = doc["preset_store"].to<JsonObject>();
        pstore["load_ok"]    = !ps.loadFailedFromNvs();
        pstore["save_fails"] = ps.saveFails();
        pstore["speakers"]   = (uint32_t)ps.speakerCount();
        // Ursachen-Aufschluesselung nur wenn ueberhaupt was schiefging —
        // sonst Status-Noise fuer alle (gleiche Regel wie bei rej_floor_*).
        // heap_aborts = transient (NVS intakt), nvs_fails = Kapazitaetskante.
        if (ps.saveFails() > 0) {
            pstore["save_heap_aborts"] = ps.saveHeapAborts();
            pstore["save_nvs_fails"]   = ps.saveNvsFails();
        }
    }
    // Analog fuer das Speaker-Inventory (hidden/order/ownership-Writes):
    // die Callsites antworten 200 auch bei NVS-Fail — der Zaehler macht das
    // im Status + UI-Badge sichtbar.
    doc["inventory"]["save_fails"] = sixback::SpeakerInventory::instance().saveFails();

    // Health-Snapshot: boot/crash-counter, last reset reason, watchdog state
    JsonObject health = doc["health"].to<JsonObject>();
    sixback::healthToJson(health);

    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Speaker-Liste + Discovery
// -----------------------------------------------------------------------------
void emitSpeakers(AsyncWebServerRequest* req, JsonDocument& doc) {
    JsonArray arr = doc["speakers"].to<JsonArray>();
    auto& inv = sixback::SpeakerInventory::instance();
    auto& ps  = sixback::PresetStore::instance();
    for (auto& s : inv.list()) {
        JsonObject o = arr.add<JsonObject>();
        o["device_id"] = s.deviceId;
        o["name"]      = s.name;
        o["model"]     = s.model;
        o["firmware"]  = s.firmware;
        o["module_type"] = s.moduleType;   // sm2/scm — HW-Revision (Issue-Triage, STR-Adopt)
        o["variant"]     = s.variant;      // rhino/mojo/spotty
        o["ip"]        = s.ip;
        o["account_id"]= s.accountId;
        o["status"]      = sixback::migrationStatusToStr(s.status);
        o["cloud_url"]   = s.cloudUrl;
        o["owned_by_us"] = s.ownedByUs;
        o["sources_ready"] = s.sourcesReady;  // Issue #10: false = migriert aber
                                              // TUNEIN-Source fehlt -> Re-Sync noetig
        o["group_id"]    = s.groupId;
        o["hidden"]      = s.hidden;   // WebUI-Ausblendung (Anzeige-Semantik)
        // Anzahl belegter Preset-Slots
        int n = 0;
        for (auto& p : ps.getForSpeaker(s.deviceId)) {
            if (p.source != sixback::PresetSource::EMPTY) ++n;
        }
        o["preset_count"] = n;
        // Spotify-Accounts (READY) — Stufe 0 Diagnose, leerer Array wenn kein Link.
        if (!s.spotifyAccounts.empty()) {
            JsonArray spa = o["spotify_accounts"].to<JsonArray>();
            for (const auto& sa : s.spotifyAccounts) {
                JsonObject sp = spa.add<JsonObject>();
                sp["source_account"] = sa.sourceAccount;
                sp["display_name"]   = sa.displayName;
            }
        }
    }
    doc["scan_in_progress"] = inv.isScanRunning();
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handleSpeakersList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    emitSpeakers(req, doc);
}

void discoverWorker_(void* /*arg*/) {
    // Expliziter User-Discover: IMMER den vollen /24-Active-Scan fahren — der
    // User will neue/umgezogene Speaker finden. (Der Boot-Auto-Mode-Pass ruft
    // discover() ohne force und ueberspringt den Scan, wenn alles erreichbar
    // + klassifiziert ist; siehe speaker_inventory.h discover().)
    sixback::SpeakerInventory::instance().discover(/*forceActiveScan=*/true);
    // Stack-Headroom messen, bevor sich der Task selbst loescht. ESP-IDF gibt
    // den High-Water-Mark in Bytes zurueck (= kleinster je freier Stack-Rest).
    // Diente zur Verifikation des bg-discover-Stack-Overflow-Fixes (v0.8.4);
    // bleibt als Telemetrie fuer grosse Setups (viele Speaker = tiefere Probes).
    UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(nullptr);
    Serial.printf("[inv] bg-discover done — stack high-water-mark=%u bytes free (of 8192)\n",
                  (unsigned)freeBytes);
    vTaskDelete(nullptr);
}

void handleDiscover(AsyncWebServerRequest* req) {
    // Discover als Background-Task: knownIpProbe + SSDP-listen + refreshMigrationStatus
    // brauchen zusammen ~5 s, danach laeuft der /24-Active-Scan weitere 30s+.
    // Wir returnen sofort den aktuellen Snapshot — scan_in_progress=true signal-
    // isiert dem UI dass es weiter pollen soll. Wenn discover() bereits laeuft,
    // gibt's keinen weiteren Worker (compare_exchange in discover()) — wir
    // returnen einfach den aktuellen Stand.
    auto& inv = sixback::SpeakerInventory::instance();
    if (!inv.isScanRunning()) {
        // 8192 statt 4096: discover() faehrt knownIpProbe_ + ssdpMSearch_ +
        // refreshMigrationStatus, alle mit HTTPClient/Telnet (lwIP-Tiefe ~1.5-2 KB)
        // ueber alle Speaker. Mit 4 KB lief der Task auf grossen Setups (9 Speaker)
        // ueber -> "Stack canary watchpoint triggered (bg-discover)" in v0.8.4.
        // 8192 entspricht den anderen netz-I/O-Tasks (auto-mode, ota-pull).
        BaseType_t r = xTaskCreate(discoverWorker_, "bg-discover", 8192,
                                    nullptr, tskIDLE_PRIORITY + 1, nullptr);
        if (r != pdPASS) {
            req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
            return;
        }
    }
    JsonDocument doc;
    emitSpeakers(req, doc);
}

void handleSpeakerAdd(AsyncWebServerRequest* req, JsonDocument& body) {
    String ip = (const char*)(body["ip"] | "");
    if (ip.length() == 0) { req->send(400, "application/json", "{\"error\":\"ip required\"}"); return; }
    sixback::ProbeFailure fail;
    bool ok = sixback::SpeakerInventory::instance().addByIp(ip, &fail);
    if (ok) {
        req->send(200, "application/json", "{\"ok\":true}");
        return;
    }
    // Strukturierter Fehler — UI kann reason+detail darstellen statt nur "failed"
    String out = "{\"ok\":false,\"error\":\"probe_failed\",\"reason\":\"";
    out += sixback::probeFailReasonStr(fail.reason);
    out += "\",\"detail\":\"";
    String d = fail.detail; d.replace("\"", "'"); d.replace("\n", " ");
    out += d;
    out += "\",\"hint\":\"";
    switch (fail.reason) {
        case sixback::ProbeFailReason::CONNECT_FAILED:
            out += "Speaker antwortet nicht auf Port 8090. Pruefe ob die IP "
                   "korrekt ist und der Speaker eingeschaltet ist. Wenn ein "
                   "SoundTouch Portable im Standby ist, weckt ihn ein "
                   "kurzer Button-Druck.";
            break;
        case sixback::ProbeFailReason::HTTP_NOT_200:
            out += "Port 8090 antwortet, liefert aber nicht HTTP 200. Ist die IP "
                   "wirklich ein SoundTouch, oder etwas anderes?";
            break;
        case sixback::ProbeFailReason::WRONG_BODY:
        case sixback::ProbeFailReason::EMPTY_BODY:
            out += "Antwort kein Bose-/info-XML. Pruefe: curl http://" + ip +
                   ":8090/info — was kommt zurueck?";
            break;
        case sixback::ProbeFailReason::NO_DEVICE_ID:
            out += "<info>-Tag vorhanden, aber kein deviceID-Attribut. Sehr "
                   "ungewoehnlich — bitte XML-Body vom curl-Test mitschicken.";
            break;
        default:
            out += "Unbekannter Fehler — bitte serielles Log mitschicken.";
    }
    out += "\"}";
    req->send(404, "application/json", out);
}

void handleSpeakerDelete(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    bool ok = sixback::SpeakerInventory::instance().remove(id);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"not found\"}");
}

// POST /api/speakers/order  body {"order":["<deviceId>", ...]}
// Setzt die persistente Anzeige-Reihenfolge der Speaker (UI-Drag-to-Reorder).
// Device-seitig (NVS) statt Browser-localStorage, damit die Sortierung
// browseruebergreifend + reboot-fest ist (analog zur Stream-Library, #8).
void handleSpeakersOrder(AsyncWebServerRequest* req, JsonDocument& body) {
    JsonArray arr = body["order"].as<JsonArray>();
    if (arr.isNull()) {
        req->send(400, "application/json", "{\"error\":\"order array required\"}");
        return;
    }
    std::vector<String> ids;
    for (JsonVariant v : arr) {
        const char* s = v.as<const char*>();
        if (s && *s) ids.push_back(String(s));
    }
    sixback::SpeakerInventory::instance().reorder(ids);
    req->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/speakers/hide  body {"device_id":"...","hidden":true|false}
// Blendet einen Speaker in der WebUI aus (fremde/Nachbar-Boxen, FHEM 144729).
// Reine Anzeige-Semantik: Auto-Mode, Cloud-Mock und BMX bedienen den Speaker
// unveraendert weiter; das Flag lebt im Inventory (reboot- + re-discovery-fest,
// mergeSpeaker_ fasst es nicht an).
void handleSpeakerHide(AsyncWebServerRequest* req, JsonDocument& body) {
    if (!body["device_id"].is<const char*>() || !body["hidden"].is<bool>()) {
        req->send(400, "application/json",
                  "{\"error\":\"device_id string + hidden bool required\"}");
        return;
    }
    bool ok = sixback::SpeakerInventory::instance().setHidden(
        body["device_id"].as<String>(), body["hidden"].as<bool>());
    req->send(ok ? 200 : 404, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"not found\"}");
}

// -----------------------------------------------------------------------------
// Speaker-Aktionen: migrate / revert / reboot / refresh-status
// -----------------------------------------------------------------------------
// Worker-Pattern fuer Long-IO-Handler:
// Telnet auf TCP:17000 zum Speaker dauert 5-10 s. ESPAsyncWebServer laeuft
// auf einem einzigen async_tcp-Task — solange er in einem Handler haengt,
// blockieren ALLE anderen Requests (auch port 8000 Cloud-Mock). Deshalb:
// Handler spawnt FreeRTOS-Task und returnt sofort 202+queued; das UI sieht
// einen schnellen Erfolg und re-fetch'ed die speaker-Liste, was den
// finalen Status zeigt sobald der Worker durch ist.
struct MigrateJob_ {
    String deviceId;
    String ip;
    String baseUrl;
    bool   doMigrate;  // true=migrate, false=revert
};

// Forward-decl: Import-Helper liegt weiter unten (vor handleImportFromDevice).
// handleMigrate ruft ihn vor migrate-trigger auf, damit der Speaker NIE eine
// Migration auf einen leeren Preset-Store erlebt — sonst verliert er beim
// ersten Sync seinen Local-Cache.
int importPresetsFromSpeaker_(const String& id, int& countOk, int& countAban,
                              int& httpCodeOut, JsonArray imported,
                              JsonArray abandoned, bool* persistedOut = nullptr);

void migrateRevertWorker_(void* arg) {
    auto* job = static_cast<MigrateJob_*>(arg);
    auto& inv = sixback::SpeakerInventory::instance();
    if (job->doMigrate) {
        auto r = migrateSpeaker(job->ip, job->baseUrl);
        Serial.printf("[bg:migrate] %s -> %s ok=%d msg=%s\n",
                      job->deviceId.c_str(), job->baseUrl.c_str(),
                      (int)r.ok, r.message.c_str());
        if (r.ok) {
            sixback::SpeakerInventory::LockGuard g(inv);
            if (auto* sp = inv.findById(job->deviceId)) {
                sp->status    = sixback::MigrationStatus::MIGRATED;
                sp->cloudUrl  = job->baseUrl;
                sp->ownedByUs = true;
                inv.saveToNVS();
            }
        }
    } else {
        auto r = revertSpeaker(job->ip);
        Serial.printf("[bg:revert] %s ok=%d msg=%s\n",
                      job->deviceId.c_str(), (int)r.ok, r.message.c_str());
        if (r.ok) {
            sixback::SpeakerInventory::LockGuard g(inv);
            if (auto* sp = inv.findById(job->deviceId)) {
                sp->status    = sixback::MigrationStatus::NOT_MIGRATED;
                sp->cloudUrl  = "https://streaming.bose.com";
                sp->ownedByUs = false;
                inv.saveToNVS();
            }
        }
    }
    delete job;
    vTaskDelete(nullptr);
}

void handleMigrate(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String ip;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ip = sp->ip;
    }

    // SCHUTZ gegen Preset-Verlust (2026-05-19): wenn unser Store fuer das
    // Device noch leer ist, MUSS vorher import-from-device passieren — sonst
    // antwortet handleDevicePresets dem Speaker mit <presets/> und der
    // ueberschreibt seinen Local-Cache. handleDevicePresets selbst schickt
    // jetzt zwar 404 bei leerem Store, aber wir wollen den Migrate sowieso
    // erst freischalten, wenn die Presets im Store sind.
    if (!sixback::PresetStore::instance().hasAnyFor(id)) {
        int countOk = 0, countAban = 0, httpCode = 0;
        int status = importPresetsFromSpeaker_(id, countOk, countAban, httpCode,
                                                JsonArray(), JsonArray());
        Serial.printf("[migrate-safe] %s pre-import status=%d ok=%d aban=%d "
                      "speakerhttp=%d store-now-has=%d\n",
                      id.c_str(), status, countOk, countAban, httpCode,
                      (int)sixback::PresetStore::instance().hasAnyFor(id));
        if (status != 200) {
            String err = "{\"error\":\"pre-migrate preset-import failed (status ";
            err += status; err += ")\",\"speaker_http\":"; err += httpCode; err += "}";
            req->send(409, "application/json", err);
            return;
        }
        // Auch ohne Error koennten 0 Presets gefunden worden sein (Speaker hat
        // selber keinen). Das ist OK — Mock liefert dann zwar 404 weiter, aber
        // damit ist nichts verloren. Speaker behaelt was er hat (= nichts).
    }

    sixback::persistPreMigrateSnapshot(id, /*force=*/false);

    // DLNA-Server-UUIDs vom Speaker cachen, damit handleAccountFull spaeter
    // pro UUID eine dedizierte STORED_MUSIC-Source deklarieren kann.
    // Ohne diese sieht der Speaker STORED_MUSIC-Presets als
    // UNKNOWN_SOURCE_ERROR (1005) nach Migration zu SixBack.
    sixback::SpeakerInventory::instance().refreshMediaServers(id);
    sixback::SpeakerInventory::instance().refreshSpotifyAccounts(id);

    auto* job = new MigrateJob_{id, ip, myBaseUrl(), /*doMigrate=*/true};
    BaseType_t r = xTaskCreate(migrateRevertWorker_, "bg-migrate", 4096, job,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        delete job;
        req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"ok\":true,\"queued\":true,"
              "\"message\":\"migration started in background — refresh speaker card in ~90s to see result (Bose-Reboot + Telnet-Settle)\"}");
}

void handleReboot(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String ip;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ip = sp->ip;
    }
    bool ok = rebootSpeaker(ip);
    req->send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true,\"message\":\"speaker reboot triggered\"}"
                 : "{\"error\":\"telnet failed\"}");
}

void handleRefreshStatus(AsyncWebServerRequest* req) {
    // Die Probe-Arbeit (refreshMigrationStatus: Telnet/BMX/Peer-Probe je Speaker
    // + stale-view-Retry mit delay() bis 30 s, dann Spotify-/sources je Speaker)
    // NICHT synchron im AsyncTCP-Request-Handler ausfuehren — das blockiert den
    // einzigen Async-TCP-Task und fror bei grossen Zonen die ganze WebUI ein
    // (FHEM 144729 #86). Wie handleDiscover: als Background-Task starten und
    // sofort den aktuellen Snapshot mit scan_in_progress=true zurueckgeben; das
    // UI pollt dann /api/speakers bis scan_in_progress==false und faehrt erst
    // danach die Per-Speaker-Phasen (hardware-presets / discard).
    auto& inv = sixback::SpeakerInventory::instance();
    // FHEM 144729 #121: den schweren Probe-Fan-out (Telnet+BMX+Spotify+sources je
    // Speaker, HTTPClient-Puffer + String-Bodies) NICHT anwerfen, wenn der Heap
    // auf dem no-PSRAM-C5 schon knapp ist (z.B. gleichzeitiges Bundle-/Per-Speaker-
    // Serving beim Page-Load) — das war der Haupt-Heap-Treiber ins Kollaps-Fenster.
    // Snapshot wird trotzdem geliefert; die WebUI kann spaeter erneut refreshen.
    if (ESP.getFreeHeap() >= outboundHeapFloor()) {
        inv.refreshStatusesAsync();
    }
    JsonDocument doc;
    emitSpeakers(req, doc);   // setzt scan_in_progress = isScanRunning()
}

// -----------------------------------------------------------------------------
// Preset-Verwaltung
// -----------------------------------------------------------------------------
void handleGetPresets(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    JsonDocument doc;
    JsonArray arr = doc["presets"].to<JsonArray>();
    for (auto& p : sixback::PresetStore::instance().getForSpeaker(id)) {
        JsonObject o = arr.add<JsonObject>();
        o["slot"]      = p.slot;
        o["source"]    = sixback::presetSourceToStr(p.source);
        o["name"]      = p.name;
        o["stationId"] = p.stationId;
        o["streamUrl"] = p.streamUrl;
        o["imageUrl"]  = p.imageUrl;
        if (p.source == sixback::PresetSource::OPAQUE) {
            o["opaqueSourceName"] = p.opaqueSourceName;
            o["rawContentItemBytes"] = (uint16_t)p.rawContentItem.length();
            // containerArt-URL aus rawContentItem extrahieren, falls vorhanden.
            // DLNA-Slots (z.B. forum-user OG_Bad_ST10weiss slot 1) haben oft
            // `<containerArt>http://192.168.x.y/...</containerArt>` mit dem
            // Cover-Art-Link des lokalen DLNA-Servers. Wenn der Browser sich
            // im selben Netz befindet, laedt das Bild direkt.
            int caStart = p.rawContentItem.indexOf("<containerArt>");
            if (caStart >= 0) {
                int caEnd = p.rawContentItem.indexOf("</containerArt>", caStart);
                if (caEnd > caStart + 14) {
                    String ca = p.rawContentItem.substring(caStart + 14, caEnd);
                    if (ca.startsWith("http")) o["containerArt"] = ca;
                }
            }
        }
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handlePutPreset(AsyncWebServerRequest* req, JsonDocument& body) {
    String id  = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    sixback::Preset p;
    p.slot      = slot;
    // source-Validierung an der Grenze: presetSourceFromStr returnt EMPTY
    // fuer unbekannte Strings (silent default), was sonst zu einem nicht
    // abspielbaren Preset-Slot fuehrt der den User irritiert. Lieber 400.
    String srcStr = String((const char*)(body["source"] | "TUNEIN"));
    p.source    = sixback::presetSourceFromStr(srcStr);
    if (p.source == sixback::PresetSource::EMPTY && srcStr != "EMPTY") {
        req->send(400, "application/json",
                  String("{\"error\":\"unknown source\",\"got\":\"") + srcStr +
                  "\",\"expected\":\"TUNEIN | LOCAL_INTERNET_RADIO\"}");
        return;
    }
    p.name      = (const char*)(body["name"]      | "");
    p.stationId = (const char*)(body["stationId"] | "");
    p.streamUrl = (const char*)(body["streamUrl"] | "");
    p.imageUrl  = (const char*)(body["imageUrl"]  | "");
    bool ok = sixback::PresetStore::instance().set(id, p);
    if (!ok) {
        // NVS-Save fehlgeschlagen. Seit dem Blob-Umbau (2026-06-07) heisst
        // das: Partition WIRKLICH voll (das fruehere 4000-B-String-Limit,
        // das ab ~5 Speakern jeden Save scheitern liess, ist weg). Der
        // letzte gute Stand bleibt erhalten (Pass-3-Restore).
        // UI sieht 500 + error-Detail, statt fakem "ok":true.
        req->send(500, "application/json",
                  "{\"ok\":false,\"error\":\"NVS save failed (partition out of space) — "
                  "remove unused speakers or try POST /api/nvs/cleanup\"}");
        return;
    }
    // Echtes (nicht-Sentinel-)Preset ueber einen Spotify-gebundenen Slot:
    // die Bindung waere ab jetzt verwaist (tote Sub-Row in der UI, Playlist-
    // Name in Fremd-Clients wie FHEM BOSEST — 144729 #151). Hier mitraeumen,
    // damit die Invariante "Nicht-Sentinel-Preset => keine Bindung" fuer ALLE
    // Clients gilt, nicht nur die WebUI. Drei Ausnahmen: sspot{N}-Writes
    // (applySpotifyDrop Schritt 2), Imports mit spotify-Feld (Block unten
    // setzt die Bindung gewollt neu) und EMPTY-Writes (leerer Slot + Bindung
    // = Click-to-Play-Feature). getSlot-Vorabcheck, weil setSlot("") sonst
    // bei jedem Preset-PUT grundlos NVS schreiben wuerde (Flash-Wear).
    if (p.source != sixback::PresetSource::EMPTY &&
        p.stationId != String("sspot") + String((int)slot) &&
        !body["spotify"].is<JsonObject>()) {
        sixback::spotify::SlotMapping stale;
        if (sixback::spotify::getSlot(id, slot, stale) && stale.spotifyUri.length() > 0) {
            sixback::spotify::setSlot(id, slot, "", "");
            Serial.printf("[spot] slot %s/%d: stale binding cleared by preset overwrite\n",
                          id.c_str(), slot);
        }
    }

    // Single-Preset-Import via File-Drop: optionales spotify-Feld mitnehmen,
    // sonst geht beim Re-Import die Track/Album-Bindung verloren. Wird hier
    // ZUSAETZLICH zur PresetStore-Schreibung gemacht (idempotent zum 2-Step-
    // Pattern in applySpotifyDrop).
    if (body["spotify"].is<JsonObject>()) {
        JsonObject sp = body["spotify"].as<JsonObject>();
        String uri    = (const char*)(sp["uri"]     | "");
        String spName = (const char*)(sp["name"]    | "");
        bool shuffle  = sp["shuffle"] | false;
        String repeat = (const char*)(sp["repeat"]  | "off");
        if (uri.length() > 0) {
            sixback::spotify::setSlot(id, slot, uri, spName, shuffle, repeat);
        }
    }
    req->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"set failed\"}");
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/preset/{n}/export
//   Liefert ein einzelnes Preset als JSON-Datei mit Content-Disposition
//   "attachment", damit Browser einen File-Download triggern (für drag-to-
//   desktop-Workflow). Schema ist kompatibel zum PUT-Endpoint:
//     { source, name, stationId, streamUrl, imageUrl, sixback_preset:1, exported_from }
// -----------------------------------------------------------------------------
void handleExportPreset(AsyncWebServerRequest* req) {
    String id  = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    auto p = sixback::PresetStore::instance().get(id, slot);
    JsonDocument doc;
    doc["sixback_preset"] = 1;     // schema marker for the importer
    doc["exported_from"]  = id;
    doc["slot"]           = slot;
    doc["source"]         = sixback::presetSourceToStr(p.source);
    doc["name"]           = p.name;
    doc["stationId"]      = p.stationId;
    doc["streamUrl"]      = p.streamUrl;
    doc["imageUrl"]       = p.imageUrl;
    if (p.source == sixback::PresetSource::OPAQUE) {
        doc["opaqueSourceName"] = p.opaqueSourceName;
    }
    // Spotify-Mapping mitexportieren — sonst geht beim Re-Import die
    // Track/Album/Playlist-Information verloren und der TUNEIN-Tunnel-Slot
    // triggert nichts. Schema kompatibel zu /api/spotify/slot.
    sixback::spotify::SlotMapping sm;
    if (sixback::spotify::getSlot(id, slot, sm) && sm.spotifyUri.length() > 0) {
        JsonObject sp = doc["spotify"].to<JsonObject>();
        sp["uri"]     = sm.spotifyUri;
        sp["name"]    = sm.displayName;
        sp["shuffle"] = sm.shuffle;
        sp["repeat"]  = sm.repeatMode;
    }
    String body; serializeJson(doc, body);

    // Filename: sanitized "<speaker-name>-slot-<n>.json" if we have a name in inventory,
    // else "preset-<dev>-<n>.json".
    String fname = "preset-" + id + "-" + String(slot) + ".json";
    {
        sixback::SpeakerInventory::LockGuard g(sixback::SpeakerInventory::instance());
        auto* sp = sixback::SpeakerInventory::instance().findById(id);
        if (sp && sp->name.length() > 0) {
            String safe;
            for (size_t i = 0; i < sp->name.length() && safe.length() < 32; ++i) {
                char c = sp->name[i];
                safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
            }
            if (safe.length() > 0) fname = safe + "-slot-" + String(slot) + ".json";
        }
    }
    auto* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    req->send(resp);
}

void handleDeletePreset(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    bool ok = sixback::PresetStore::instance().clear(id, slot);
    req->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"unknown\"}");
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/presets/export-set
//   Komplettes 6-Slot-Preset-Set eines Speakers als JSON-File.
//   Schema kompatibel zum Import-Endpunkt (siehe handleImportPresetsSet).
// -----------------------------------------------------------------------------
void handleExportPresetsSet(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto slots = sixback::PresetStore::instance().getForSpeaker(id);

    JsonDocument doc;
    doc["sixback_preset_set"] = 1;
    doc["exported_from"]      = id;
    String spName;
    {
        sixback::SpeakerInventory::LockGuard g(sixback::SpeakerInventory::instance());
        auto* sp = sixback::SpeakerInventory::instance().findById(id);
        if (sp) spName = sp->name;
    }
    doc["exported_speaker_name"] = spName;
    JsonArray arr = doc["presets"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) {
        const sixback::Preset& p = (i < (int)slots.size()) ? slots[i] : sixback::Preset();
        JsonObject pj = arr.add<JsonObject>();
        pj["slot"]      = i + 1;
        pj["source"]    = sixback::presetSourceToStr(p.source);
        pj["name"]      = p.name;
        pj["stationId"] = p.stationId;
        pj["streamUrl"] = p.streamUrl;
        pj["imageUrl"]  = p.imageUrl;
        if (p.source == sixback::PresetSource::OPAQUE) {
            pj["rawContentItem"]   = p.rawContentItem;
            pj["opaqueSourceName"] = p.opaqueSourceName;
        }
        // Spotify-Slot-Mapping mitexportieren (siehe handleExportPreset).
        sixback::spotify::SlotMapping sm;
        if (sixback::spotify::getSlot(id, i + 1, sm) && sm.spotifyUri.length() > 0) {
            JsonObject sp = pj["spotify"].to<JsonObject>();
            sp["uri"]     = sm.spotifyUri;
            sp["name"]    = sm.displayName;
            sp["shuffle"] = sm.shuffle;
            sp["repeat"]  = sm.repeatMode;
        }
    }
    String body; serializeJson(doc, body);

    String fname = "presets-" + id + ".json";
    if (spName.length() > 0) {
        String safe;
        for (size_t i = 0; i < spName.length() && safe.length() < 32; ++i) {
            char c = spName[i];
            safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
        }
        if (safe.length() > 0) fname = safe + "-presets.json";
    }
    auto* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    req->send(resp);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/presets/import-set
//   Ersetzt das komplette 6-Slot-Set des Speakers durch das uebergebene JSON.
//   Akzeptiert sowohl das Export-Set-Schema (sixback_preset_set:1 mit
//   presets[]) als auch direkt ein presets[]-Array. Slots ohne Eintrag im
//   JSON werden gelöscht (=EMPTY). Push zur Hardware passiert NICHT
//   automatisch — User klickt danach den per-Slot- oder Push-All-Button.
// -----------------------------------------------------------------------------
void handleImportPresetsSet(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        if (!inv.findById(id)) {
            req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}");
            return;
        }
    }

    JsonArray arr;
    if (body["presets"].is<JsonArray>())       arr = body["presets"].as<JsonArray>();
    else if (body.as<JsonArray>().size() > 0)  arr = body.as<JsonArray>();
    else {
        req->send(400, "application/json",
                  "{\"error\":\"expected {presets:[...]} or [...]\"}");
        return;
    }

    std::vector<sixback::Preset> slots(6);
    for (int i = 0; i < 6; ++i) {
        slots[i].slot   = i + 1;
        slots[i].source = sixback::PresetSource::EMPTY;
    }
    int n = 0;
    int spImported = 0;
    for (JsonObject pj : arr) {
        uint8_t slot = pj["slot"].as<uint8_t>();
        if (slot < 1 || slot > 6) continue;
        sixback::Preset& p = slots[slot - 1];
        p.slot      = slot;
        p.source    = sixback::presetSourceFromStr(String((const char*)(pj["source"] | "EMPTY")));
        p.name      = (const char*)(pj["name"]      | "");
        p.stationId = (const char*)(pj["stationId"] | "");
        p.streamUrl = (const char*)(pj["streamUrl"] | "");
        p.imageUrl  = (const char*)(pj["imageUrl"]  | "");
        p.rawContentItem   = (const char*)(pj["rawContentItem"]   | "");
        p.opaqueSourceName = (const char*)(pj["opaqueSourceName"] | "");
        if (p.source != sixback::PresetSource::EMPTY) ++n;
        // Spotify-Slot-Mapping wiederherstellen wenn im Export-File enthalten.
        // Upsert-Semantik: bestehende Mappings auf demselben Slot werden
        // ersetzt. Wenn das Import-File KEIN spotify-Feld hat (Legacy oder
        // bewusst), bleibt das existierende Mapping unangetastet.
        if (pj["spotify"].is<JsonObject>()) {
            JsonObject sp = pj["spotify"].as<JsonObject>();
            String uri    = (const char*)(sp["uri"]     | "");
            String spName = (const char*)(sp["name"]    | "");
            bool shuffle  = sp["shuffle"] | false;
            String repeat = (const char*)(sp["repeat"]  | "off");
            if (uri.length() > 0) {
                sixback::spotify::setSlot(id, slot, uri, spName, shuffle, repeat);
                ++spImported;
            }
        }
    }
    bool ok = sixback::PresetStore::instance().setSlots(id, slots);

    JsonDocument resp;
    resp["ok"] = ok;
    resp["imported"] = n;
    resp["spotify_imported"] = spImported;
    String b; serializeJson(resp, b);
    req->send(ok ? 200 : 500, "application/json", b);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/presets/import-from-device
//   Liest /presets-XML vom echten Speaker (BMX-API Port 8090) und uebernimmt
//   die bereits dort gespeicherten Presets in den ESP-PresetStore. So muss
//   der User nicht haendisch jede TuneIn-ID raussuchen.
//   Funktioniert AUCH wenn der Speaker noch nicht migriert ist (BMX-API ist
//   immer offen, unabhaengig von der Cloud-URL-Config).
// -----------------------------------------------------------------------------
String xmlExtractAttr(const String& xml, int start, int end, const String& attr) {
    String key = attr + "=\"";
    int a = xml.indexOf(key, start);
    if (a < 0 || a >= end) return "";
    a += key.length();
    int b = xml.indexOf('"', a);
    if (b < 0 || b >= end) return "";
    return xml.substring(a, b);
}

String xmlExtractTag(const String& xml, int start, int end, const String& tag) {
    String open = "<" + tag + ">";
    String close = "</" + tag + ">";
    int a = xml.indexOf(open, start);
    if (a < 0 || a >= end) return "";
    a += open.length();
    int b = xml.indexOf(close, a);
    if (b < 0 || b > end) return "";
    // Tag-Text-Content ist XML-escaped (itemName/containerArt) -> zurueck zur
    // logischen Form dekodieren, sonst "&amp;" in Store/UI/Diff.
    return sixback::unescapeXml(xml.substring(a, b));
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/preset/{n}/revert
//   Setzt EINEN Store-Slot zurueck auf den aktuellen Hardware-Stand des
//   Speakers. Genutzt vom UI als "✕ undo" auf einer noch nicht gepushten
//   (unsaved) Slot-Karte: der User hat eine Aenderung vorbereitet, will
//   sie aber doch nicht — Klick auf ✕ macht den Store wieder synchron
//   ohne dass die Hardware angefasst wird.
//
//   Verfahren: /presets-XML vom Speaker holen, Slot N extrahieren,
//   normalisieren (inkl. OPAQUE rawContentItem), Store-Slot ersetzen.
//   Wenn der Speaker den Slot leer hat → Store-Slot loeschen.
// -----------------------------------------------------------------------------
void handleRevertPresetToHw(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(2000); http.setTimeout(3000);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!http.begin(url)) {
        req->send(500, "application/json", "{\"error\":\"http begin failed\"}");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        JsonDocument doc; doc["error"] = "speaker /presets http"; doc["http"] = code;
        String b; serializeJson(doc, b);
        req->send(502, "application/json", b);
        return;
    }
    String xml = http.getString();
    http.end();

    String openTag = "<preset id=\"" + String(slot) + "\"";
    int presetOpen = xml.indexOf(openTag);
    if (presetOpen < 0) {
        sixback::PresetStore::instance().clear(id, slot);
        JsonDocument doc; doc["ok"] = true; doc["action"] = "cleared"; doc["slot"] = slot;
        String b; serializeJson(doc, b);
        req->send(200, "application/json", b);
        return;
    }
    int idStart = presetOpen + openTag.length();
    int presetClose = xml.indexOf("</preset>", idStart);
    if (presetClose < 0) {
        req->send(502, "application/json", "{\"error\":\"malformed /presets xml\"}");
        return;
    }

    String src  = xmlExtractAttr(xml, idStart, presetClose, "source");
    String loc  = xmlExtractAttr(xml, idStart, presetClose, "location");
    String name = xmlExtractTag (xml, idStart, presetClose, "itemName");
    String img  = xmlExtractTag (xml, idStart, presetClose, "containerArt");

    sixback::Preset p;
    p.slot = slot;
    auto nr = sixback::normalizePreset(src, loc, name, img, p);

    if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
        int ciOpen  = xml.indexOf("<ContentItem", idStart);
        int ciClose = xml.indexOf("</ContentItem>", ciOpen);
        if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
            p.rawContentItem = xml.substring(ciOpen, ciClose + 14);
        }
        if (p.name.length() == 0) p.name = String("[") + src + String("] preset");
    }
    if (nr.status == sixback::NormalizeStatus::ABANDONED) {
        sixback::PresetStore::instance().clear(id, slot);
        JsonDocument doc; doc["ok"] = true; doc["action"] = "cleared"; doc["slot"] = slot;
        doc["note"] = "speaker source not representable in store";
        String b; serializeJson(doc, b);
        req->send(200, "application/json", b);
        return;
    }

    bool ok = sixback::PresetStore::instance().set(id, p);
    JsonDocument doc;
    doc["ok"]     = ok;
    doc["action"] = "reverted";
    doc["slot"]   = slot;
    doc["source"] = sixback::presetSourceToStr(p.source);
    doc["name"]   = p.name;
    String b; serializeJson(doc, b);
    req->send(ok ? 200 : 500, "application/json", b);
}

// Pure-Helper: holt /presets-XML vom Speaker, normalisiert + persistiert.
// JsonDocument&-Outputs optional (nullptr fuer "kein Detail-Report noetig",
// nur Status reicht). Return-Wert:
//   200 = OK (countOk + countAban gesetzt)
//   404 = unknown deviceId
//   500 = http begin failed
//   502 = speaker http != 200 (httpCodeOut gefuellt)
int importPresetsFromSpeaker_(const String& id, int& countOk, int& countAban,
                              int& httpCodeOut, JsonArray imported,
                              JsonArray abandoned, bool* persistedOut) {
    countOk = countAban = 0;
    httpCodeOut = 0;
    if (persistedOut) *persistedOut = true;   // "nichts zu schreiben" gilt als ok
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) return 404;
        spIp = sp->ip;
    }
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    http.setConnectTimeout(2000); http.setTimeout(3000);
    if (!http.begin(url)) return 500;
    int code = http.GET();
    httpCodeOut = code;
    if (code != 200) { http.end(); return 502; }
    String xml = http.getString();
    http.end();

    std::vector<sixback::Preset> toSet;
    toSet.reserve(6);
    int pos = 0;
    while (true) {
        int presetOpen = xml.indexOf("<preset id=\"", pos);
        if (presetOpen < 0) break;
        int idStart = presetOpen + 12;
        int idEnd = xml.indexOf('"', idStart);
        if (idEnd < 0) break;
        int presetClose = xml.indexOf("</preset>", idEnd);
        if (presetClose < 0) break;
        uint8_t slot = xml.substring(idStart, idEnd).toInt();
        if (slot < 1 || slot > 6) { pos = presetClose; continue; }

        String src  = xmlExtractAttr(xml, idEnd, presetClose, "source");
        String loc  = xmlExtractAttr(xml, idEnd, presetClose, "location");
        String name = xmlExtractTag (xml, idEnd, presetClose, "itemName");
        String img  = xmlExtractTag (xml, idEnd, presetClose, "containerArt");

        sixback::Preset p;
        p.slot = slot;
        auto nr = sixback::normalizePreset(src, loc, name, img, p);

        if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
            // Vollstaendiges <ContentItem>...</ContentItem> extrahieren —
            // wird beim Sync 1:1 ans Speaker zurueckgeschickt. Speaker spricht
            // DLNA/UPnP/Bluetooth selbst an, unsere Cloud ist nicht
            // beteiligt am Playback.
            int ciOpen  = xml.indexOf("<ContentItem", idEnd);
            int ciClose = xml.indexOf("</ContentItem>", ciOpen);
            if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
                p.rawContentItem = xml.substring(ciOpen, ciClose + 14);
            }
            if (p.name.length() == 0) {
                p.name = String("[") + src + String("] preset");
            }
        }

        if (nr.status == sixback::NormalizeStatus::ABANDONED) {
            if (!abandoned.isNull()) {
                JsonObject o = abandoned.add<JsonObject>();
                o["slot"]   = slot;
                o["source"] = src;
                o["reason"] = nr.reason;
            }
            ++countAban;
        } else {
            toSet.push_back(p);
            if (!imported.isNull()) {
                JsonObject o = imported.add<JsonObject>();
                o["slot"]            = p.slot;
                o["name"]            = p.name;
                o["source"]          = sixback::presetSourceToStr(p.source);
                o["stationId"]       = p.stationId;
                o["streamUrl"]       = p.streamUrl;
                o["normalize"]       = sixback::normalizeStatusToStr(nr.status);
                if (nr.status == sixback::NormalizeStatus::OK_CONVERTED) {
                    o["converted_from"] = nr.originalSource;
                    o["reason"]         = nr.reason;
                }
                if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
                    o["opaque_source"]  = p.opaqueSourceName;
                    o["raw_bytes"]      = (uint16_t)p.rawContentItem.length();
                }
            }
            ++countOk;
        }
        pos = presetClose;
    }
    // Batched-Save: alle erfolgreich normalisierten Presets in einem
    // NVS-Write statt einer pro Slot. ABER: wenn der Speaker grad leer ist
    // (z.B. eben durch eine voraus-fehlgeschlagene Migration), setSlots-mit-
    // leerem-vector wuerde unseren Store auch leer machen. Deshalb nur dann
    // schreiben wenn was gefunden wurde — sonst Store unveraendert lassen.
    //
    // Der Rueckgabewert von setSlots() wurde bis 2026-08-02 verworfen: ein an
    // der NVS-Kante gescheiterter Save meldete dem Aufrufer trotzdem 200, der
    // Store hielt die Presets nur noch im RAM (weg beim naechsten Boot) — und
    // die UI wusste nichts davon (FHEM 144729 #196). Jetzt durchgereicht.
    if (!toSet.empty()) {
        bool saved = sixback::PresetStore::instance().setSlots(id, toSet);
        if (persistedOut) *persistedOut = saved;
        if (!saved) {
            Serial.printf("[import] %s: %u Presets im RAM, aber NVS-Persist "
                          "FEHLGESCHLAGEN\n", id.c_str(), (unsigned)toSet.size());
        }
    }
    return 200;
}

// Auto-Import-Backoff (FHEM 144729 #196, 2026-08-02)
// ---------------------------------------------------
// Der First-Contact-Auto-Import der WebUI feuert bei JEDEM frischen Seiten-
// aufbau fuer jeden Speaker, dessen Store-Slice leer ist. saveToNVS() schreibt
// dabei IMMER den kompletten Store-Blob (alle Slices) — bei einem Nutzer mit
// vielen Boxen also einen grossen Write, und wenn der an der NVS-Kante
// scheitert, bleibt der Slice leer => beim naechsten Reload derselbe
// Voll-Blob-Write, samt Cleanup-Kaskade, die dabei regenerable Namespaces
// (sixback-tune/-sys) purgt. Ein Reload-Zyklus wurde so zum Write-Verstaerker.
// Deshalb: gescheiterter Persist sperrt weitere AUTO-Importe fuer dieses
// Device — bis irgendein Save wieder durchkommt (saveOkCount aendert sich,
// z.B. weil der User Platz geschaffen hat) oder bis zum Reboot.
namespace {
std::vector<String> g_importPersistFailed;
uint32_t            g_importFailedAtSaveOk = 0;

// Sperrliste verwerfen, sobald wieder ein Save durchkam.
void importBackoffSync_() {
    uint32_t okNow = sixback::PresetStore::instance().saveOkCount();
    if (okNow != g_importFailedAtSaveOk) {
        if (!g_importPersistFailed.empty()) {
            Serial.printf("[import] NVS-Save wieder erfolgreich — Auto-Import-Sperre "
                          "fuer %u Device(s) aufgehoben\n",
                          (unsigned)g_importPersistFailed.size());
        }
        g_importPersistFailed.clear();
        g_importFailedAtSaveOk = okNow;
    }
}
bool importBackoffBlocked_(const String& id) {
    importBackoffSync_();
    for (const auto& d : g_importPersistFailed) if (d == id) return true;
    return false;
}
void importBackoffMark_(const String& id) {
    for (const auto& d : g_importPersistFailed) if (d == id) return;
    g_importPersistFailed.push_back(id);
    g_importFailedAtSaveOk = sixback::PresetStore::instance().saveOkCount();
}
}  // namespace

void handleImportFromDevice(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    // Bereits gescheitert und seither kein erfolgreicher Save: gar nicht erst
    // versuchen. 200 (nicht 5xx), damit der Auto-Pfad in der UI still bleibt —
    // die Diagnose steht in preset_store.save_fails + im Serial-Log.
    if (importBackoffBlocked_(id)) {
        req->send(200, "application/json",
                  "{\"ok\":false,\"count\":0,\"persisted\":false,"
                  "\"skipped\":\"backoff\","
                  "\"error\":\"previous import could not be persisted (NVS) — "
                  "not retried until a save succeeds\"}");
        return;
    }
    JsonDocument resp;
    JsonArray imported  = resp["imported"].to<JsonArray>();
    JsonArray abandoned = resp["abandoned"].to<JsonArray>();
    int countOk = 0, countAban = 0, httpCode = 0;
    bool persisted = true;
    int status = importPresetsFromSpeaker_(id, countOk, countAban, httpCode,
                                            imported, abandoned, &persisted);
    if (status == 404) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    if (status == 500) { req->send(500, "application/json", "{\"error\":\"http begin\"}");      return; }
    if (status == 502) {
        req->send(502, "application/json",
                  String("{\"error\":\"speaker http ") + httpCode + "\"}");
        return;
    }
    if (!persisted) importBackoffMark_(id);
    resp["ok"]              = persisted;
    resp["persisted"]       = persisted;
    resp["count"]           = countOk;
    resp["abandoned_count"] = countAban;
    if (!persisted) {
        resp["error"] = "imported into RAM but NVS save failed — "
                        "presets are not reboot-safe yet";
    }
    String body; serializeJson(resp, body);
    req->send(200, "application/json", body);
}

// POST /api/speaker/{id}/preset/{slot}/push-to-device
//   speichert das aktuelle Preset am echten Speaker (Long-Press-Simulation):
//   /select mit ContentItem → /key press PRESET_n → 2.5s → /key release PRESET_n
//
// 4 s + 2.5 s = 6.5 s sind hartes Bose-Timing, nicht verkuerzbar — daher
// als Background-Task wie Migrate/Revert, damit der async_tcp-Thread frei
// bleibt.
struct PushPresetJob_ {
    String spIp;
    sixback::Preset p;
};

// Persistenter Single-Worker mit FreeRTOS-Queue. Vorgaenger-Designs hatten
// pro Push einen eigenen Task spawned + Mutex serialisiert. Bei Burst >6
// Pushes ging das gelegentlich kaputt — entweder xTaskCreate fail (Heap-
// Druck mit N×4KB-Stacks parallel) oder Race im Mutex-Lazy-Init.
//
// Jetzt: eine Worker-Task wird lazy beim ersten Push-Handle erzeugt und
// laeuft fuer immer. Handler enqueued nur `PushPresetJob_*` in die Queue,
// Worker holt FIFO ab. Queue-Full -> 503 (statt verlorenen Tasks).
static QueueHandle_t g_pushQueue = nullptr;
static TaskHandle_t  g_pushTask  = nullptr;
constexpr UBaseType_t PUSH_QUEUE_DEPTH = 16;  // ≥ 3 Speaker × 6 Slots

// Wartet bis der Speaker den gerade ge-selecteten Sender wirklich abspielt
// (now_playing: passende source + playStatus=PLAY_STATE), max timeoutMs.
// Ersetzt das blinde delay(8000) in den Long-Press-Pfaden: auf langsamen
// Netzen/Resolutions startet der Stream erst nach >8s, und ein Long-Press
// waehrend BUFFERING/INVALID_SOURCE speichert nichts (Issue #10 "Mode B" —
// migrierter Speaker, /select=200, aber verify-fail "hardware did not save").
// Rueckgabe: true = PLAY_STATE erreicht; false = INVALID_SOURCE oder Timeout.
static bool waitForPlayState_(const String& spIp, const String& expectSource,
                              uint32_t timeoutMs) {
    const String srcNeedle = "source=\"" + expectSource + "\"";
    delay(1500);  // kurze Anlauf-Latenz, damit now_playing vom Vorzustand weg ist
    uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        HTTPClient h;
        h.setReuse(false);
        bool play = false, invalid = false;
        if (h.begin("http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/now_playing")) {
            if (h.GET() == 200) {
                String np = h.getString();
                if (np.indexOf("INVALID_SOURCE") >= 0) invalid = true;
                else if (np.indexOf(srcNeedle) >= 0 && np.indexOf("PLAY_STATE") >= 0) play = true;
            }
        }
        h.end();
        if (play)    return true;
        if (invalid) return false;
        delay(1000);
    }
    return false;
}

static void doPush_(const PushPresetJob_& job) {
    // Defensive /sources-Vorpruefung (Issue #10/#11): der Handler gated bereits
    // auf MigrationStatus, aber der kann veraltet sein (cron-tick alle 30 min)
    // oder der account-bound TUNEIN-Source kann trotz "migrated" weggefallen sein
    // (siehe bose_endpoints.cpp account-bound-source-drop). Ist TUNEIN am Speaker
    // nicht READY, liefe /select in 500 UNKNOWN_SOURCE_ERROR — hier vorher sauber
    // abbrechen mit klarem Log statt Fehl-Long-Press. Nur bei eindeutigem
    // /sources=200-ohne-READY abbrechen; ein /sources-Fehler faellt durch (das
    // /select unten hat eigenes Error-Handling).
    if (job.p.source == sixback::PresetSource::TUNEIN) {
        HTTPClient hs;
        hs.setReuse(false);
        if (hs.begin("http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/sources")) {
            if (hs.GET() == 200) {
                String srcs = hs.getString();
                bool ready = false;
                int pos = srcs.indexOf("source=\"TUNEIN\"");
                while (pos >= 0) {
                    int tagEnd = srcs.indexOf('>', pos);
                    if (tagEnd > pos &&
                        srcs.substring(pos, tagEnd).indexOf("status=\"READY\"") >= 0) {
                        ready = true; break;
                    }
                    pos = srcs.indexOf("source=\"TUNEIN\"", pos + 15);
                }
                if (!ready) {
                    hs.end();
                    Serial.printf("[bg:push-preset] %s slot %u ABORT — TUNEIN source not READY in /sources (not migrated / source dropped) — no /select\n",
                                  job.spIp.c_str(), job.p.slot);
                    return;
                }
            }
            hs.end();
        }
    }
    sixback::gabboMarkSelfSelect(job.spIp);   // gabbo-Loop-Guard: Eigen-/select markieren
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/select";
    int selectCode = -1;
    if (http.begin(url)) {
        // sourceAccount: empirisch 2026-05-23 mit Emma:
        //   "TuneIn" -> HTTP 500 1005 UNKNOWN_SOURCE_ERROR
        //   ""       -> HTTP 200 + now_playing schaltet auf TUNEIN
        // Aeltere Memory-Notiz (reference_bose_select_sourceaccount.md) sagt
        // TUNEIN braeuchte "TuneIn" — gilt nicht auf der aktuellen Bose-FW
        // dieser Speaker (Emma SoundTouch 10 FW 27.0.3). Empty fuer beide
        // Source-Typen scheint zuverlaessig zu funktionieren.
        const char* srcAcct = "";
        // type="stationurl" fuer BEIDE adapter-aufgeloesten Quellen:
        //   TUNEIN               -> location /v1/playback/station/<sid> (tunein-Adapter)
        //   LOCAL_INTERNET_RADIO -> location /station?data=…            (ORION-Adapter)
        // Frueher nutzte LIR type="url" mit roher Stream-URL — der Speaker
        // echo't das nur, spielt aber nicht. Mit der ORION-location MUSS es
        // "stationurl" sein, sonst interpretiert der Speaker /station?data= als
        // Direkt-URL und bleibt STANDBY (on-device verifiziert 2026-06-03 an Emma:
        // type="url" -> STANDBY, type="stationurl" -> PLAY_STATE).
        const char* ciType = "stationurl";
        String ci = "<ContentItem source=\"";
        ci += sixback::presetSourceToStr(job.p.source);
        ci += "\" type=\""; ci += ciType;
        ci += "\" location=\"";
        if (job.p.source == sixback::PresetSource::TUNEIN) {
            ci += "/v1/playback/station/" + job.p.stationId;
        } else {
            // LOCAL_INTERNET_RADIO: native ORION-Adapter-location statt roher
            // Stream-URL. Letztere wird vom Speaker nur ge-echo't, aber nicht
            // gespielt (kein <playStatus>); der ORION-Pfad spielt nativ
            // (handleOrionStation, empirisch v0.8.11 auf Emma).
            ci += sixback::orionStationLocation(job.p.streamUrl, job.p.name, job.p.imageUrl);
        }
        ci += "\" sourceAccount=\""; ci += srcAcct;
        ci += "\" isPresetable=\"true\"><itemName>" + sixback::escapeXml(job.p.name) + "</itemName></ContentItem>";
        http.addHeader("Content-Type", "text/xml");
        selectCode = http.POST(ci);
        http.end();
    }
    if (selectCode != 200) {
        // /select failed (network glitch, speaker STANDBY/busy, UNKNOWN_SOURCE,
        // etc.). Long-Press would save whatever happens to be playing at that
        // moment to this slot — almost guaranteed to be wrong. Abort.
        const char* hint = (selectCode == 500)
            ? " [500 = UNKNOWN_SOURCE: speaker source not bound — is it migrated?]" : "";
        Serial.printf("[bg:push-preset] %s slot %u ABORT — /select=%d (no long-press attempted)%s\n",
                      job.spIp.c_str(), job.p.slot, selectCode, hint);
        return;
    }
    // Statt blind 8s: auf echtes PLAY_STATE warten (Fix Issue #10 "Mode B").
    // Bose-FW quittiert /select 200 bevor now_playing umgeschaltet ist; auf
    // langsamen Netzen/Resolutions laeuft der Sender erst nach >8s an, und ein
    // Long-Press waehrend BUFFERING/INVALID_SOURCE speichert nichts -> verify-
    // fail. waitForPlayState_ pollt bis der ge-selectete Sender PLAY_STATE
    // meldet (max 18s); sonst abbrechen statt einen Fehl-Long-Press zu machen.
    if (!waitForPlayState_(job.spIp, sixback::presetSourceToStr(job.p.source), 18000)) {
        Serial.printf("[bg:push-preset] %s slot %u ABORT — station never reached PLAY_STATE within 18s (slow stream resolve / INVALID_SOURCE) — no long-press, nothing saved\n",
                      job.spIp.c_str(), job.p.slot);
        return;
    }
    // Long-Press PRESET_n
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + job.spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(job.p.slot) + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int pressRc = sendKey("press");
    delay(2500);  // > 2s = Long-Press
    int releaseRc = sendKey("release");
    Serial.printf("[bg:push-preset] %s slot %u select=%d press=%d release=%d done\n",
                  job.spIp.c_str(), job.p.slot, selectCode, pressRc, releaseRc);
}

void pushPresetWorker_(void* /*arg*/) {
    PushPresetJob_* job = nullptr;
    while (true) {
        if (xQueueReceive(g_pushQueue, &job, portMAX_DELAY) == pdTRUE && job) {
            doPush_(*job);
            delete job;
            job = nullptr;
        }
    }
}

// Lazy-init push queue+worker on demand. Single-threaded AsyncWebServer
// caller, so no init race. Returns 0=ready, -1=queue alloc, -2=task spawn.
static int ensurePushQueueReady_() {
    if (g_pushQueue) return 0;
    g_pushQueue = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(PushPresetJob_*));
    if (!g_pushQueue) return -1;
    BaseType_t tr = xTaskCreate(pushPresetWorker_, "push-worker", 4096,
                                 nullptr, tskIDLE_PRIORITY + 1, &g_pushTask);
    if (tr != pdPASS) {
        vQueueDelete(g_pushQueue);
        g_pushQueue = nullptr;
        return -2;
    }
    return 0;
}

// Enqueue one push-job. Caller owns the Preset content (we copy by value).
// Returns 0=enqueued, -1=queue full. Caller must have ensurePushQueueReady_
// returned 0 already.
static int enqueuePushJob_(const String& spIp, const sixback::Preset& p) {
    auto* job = new PushPresetJob_{spIp, p};
    if (xQueueSend(g_pushQueue, &job, 0) != pdTRUE) {
        delete job;
        return -1;
    }
    return 0;
}

void handlePushPresetToDevice(AsyncWebServerRequest* req) {
    int qr = ensurePushQueueReady_();
    if (qr == -1) { req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}"); return; }
    if (qr == -2) { req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}"); return; }

    String id = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    sixback::MigrationStatus spStatus = sixback::MigrationStatus::UNKNOWN;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp     = sp->ip;
        spStatus = sp->status;
    }
    // Push-Guard (Issue #10/#11, Repro 2026-06-01): ein nicht-migrierter Speaker
    // hat keinen gebundenen TUNEIN-/Cloud-Source in /sources -> /select liefert
    // HTTP 500 UNKNOWN_SOURCE_ERROR (1005), doPush_ bricht ab, und die UI meldet
    // das irrefuehrende "hardware did not save ... (Bose-side / name-normalization)".
    // Statt einen aussichtslosen Push zu enqueuen: hier mit handlungsweisender
    // Meldung abweisen. api() reicht body.error als Step-Fehlertext an die UI durch.
    if (spStatus != sixback::MigrationStatus::MIGRATED) {
        const char* msg;
        switch (spStatus) {
            case sixback::MigrationStatus::NOT_MIGRATED:
                msg = "speaker not migrated to SixBack yet — migrate it first, then push presets";
                break;
            case sixback::MigrationStatus::SETTLING:
                msg = "speaker is settling after a reboot — retry in ~90 s";
                break;
            case sixback::MigrationStatus::OFFLINE:
                msg = "speaker is offline / unreachable";
                break;
            default:
                msg = "speaker not ready for preset push (migrate it first)";
                break;
        }
        req->send(409, "application/json", String("{\"error\":\"") + msg + "\"}");
        return;
    }
    auto p = sixback::PresetStore::instance().get(id, slot);
    if (p.source == sixback::PresetSource::EMPTY) {
        req->send(400, "application/json", "{\"error\":\"empty slot\"}"); return;
    }
    if (enqueuePushJob_(spIp, p) != 0) {
        req->send(503, "application/json",
                  "{\"error\":\"push queue full (>=16 pending) — retry in a few seconds\"}");
        return;
    }
    UBaseType_t depth = uxQueueMessagesWaiting(g_pushQueue);
    String body = "{\"ok\":true,\"queued\":true,\"queue_depth\":";
    body += String((unsigned)depth);
    body += ",\"message\":\"preset push enqueued — sequential push-worker, ~7s per slot\"}";
    req->send(202, "application/json", body);
}

// -----------------------------------------------------------------------------
// Pre-Migration Restore (Issue #3) — recovery for users who overwrote slots
// while experimenting with SixBack. Reads the snapshot captured at first
// migration (/snapshots/<dev>.json, snapshot_kind=pre_migrate), reconstructs
// the 6 original presets, writes them to PresetStore, and pushes the
// non-OPAQUE slots via doPush_. OPAQUE (STORED_MUSIC/DLNA) slots get picked
// up by the speaker on its next /full poll (~30s).
// -----------------------------------------------------------------------------

// Parse <presets><preset id="N">...</preset></presets> XML out of the
// snapshot's bmx.presets.body field. Populates outPresets (normalized,
// ready for PresetStore::setSlots) and outInfo (per-slot summary for
// the UI preview / response report). Identical parsing logic to
// importPresetsFromSpeaker_ — kept inline rather than refactored because
// the live-import path uses xmlExtractAttr/Tag with positional offsets
// against a different buffer (speaker HTTP response vs snapshot field).
struct RestoreSlotInfo_ {
    uint8_t slot;
    String  source;        // raw, as in snapshot
    String  name;
    String  stationId;
    String  streamUrl;
    bool    abandoned;
    bool    opaque;
    String  reason;        // for abandoned
};

static void parseSnapshotPresetsXml_(const String& presetsXml,
                                     std::vector<sixback::Preset>& outPresets,
                                     std::vector<RestoreSlotInfo_>& outInfo) {
    int pos = 0;
    while (true) {
        int presetOpen = presetsXml.indexOf("<preset id=\"", pos);
        if (presetOpen < 0) break;
        int idStart = presetOpen + 12;
        int idEnd = presetsXml.indexOf('"', idStart);
        if (idEnd < 0) break;
        int presetClose = presetsXml.indexOf("</preset>", idEnd);
        if (presetClose < 0) break;
        uint8_t slot = presetsXml.substring(idStart, idEnd).toInt();
        if (slot < 1 || slot > 6) { pos = presetClose; continue; }

        String src  = xmlExtractAttr(presetsXml, idEnd, presetClose, "source");
        String loc  = xmlExtractAttr(presetsXml, idEnd, presetClose, "location");
        String name = xmlExtractTag (presetsXml, idEnd, presetClose, "itemName");
        String img  = xmlExtractTag (presetsXml, idEnd, presetClose, "containerArt");

        sixback::Preset p;
        p.slot = slot;
        auto nr = sixback::normalizePreset(src, loc, name, img, p);

        RestoreSlotInfo_ info;
        info.slot = slot; info.source = src; info.name = name;
        info.stationId = p.stationId; info.streamUrl = p.streamUrl;
        info.abandoned = false; info.opaque = false;

        if (nr.status == sixback::NormalizeStatus::OK_OPAQUE) {
            int ciOpen  = presetsXml.indexOf("<ContentItem", idEnd);
            int ciClose = presetsXml.indexOf("</ContentItem>", ciOpen);
            if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
                p.rawContentItem = presetsXml.substring(ciOpen, ciClose + 14);
            }
            if (p.name.length() == 0) p.name = String("[") + src + String("] preset");
            info.opaque = true;
        }
        if (nr.status == sixback::NormalizeStatus::ABANDONED) {
            info.abandoned = true;
            info.reason    = nr.reason;
        } else {
            outPresets.push_back(p);
        }
        outInfo.push_back(info);
        pos = presetClose;
    }
}

// Common: load + parse the stored snapshot for a deviceId.
// Returns 0=ok / 404=no snapshot / 500=parse fail. On 0, fills outDoc.
//
// Accepts any snapshot_kind ("pre_migrate" written at first migrate,
// "manual" written from `/diagnostic-snapshot?save=1`, "live" from
// live-capture-without-save). User sees the kind in the preview and
// decides whether to restore. The original spec was pre_migrate-only;
// in practice a manual snapshot is equally valid recovery material —
// the *user* knows whether they captured it before or after the bad
// edits.
static int loadPreMigrateSnapshotDoc_(const String& id, JsonDocument& outDoc,
                                       String& errOut) {
    String snapJson;
    if (!sixback::loadStoredSnapshot(id, snapJson)) {
        errOut = "no stored snapshot for this speaker";
        return 404;
    }
    if (deserializeJson(outDoc, snapJson)) {
        errOut = "snapshot JSON parse failed";
        return 500;
    }
    return 0;
}

// GET /api/speaker/{id}/restore-pre-migration/preview
//   Returns the slot-by-slot view of what restore WOULD do, without pushing.
//   UI uses this to render the confirm dialog ("you're about to overwrite
//   these N slots — proceed?"). No state change.
void handleRestorePreMigrationPreview(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    JsonDocument snap;
    String err;
    int rc = loadPreMigrateSnapshotDoc_(id, snap, err);
    if (rc != 0) {
        JsonDocument e; e["error"] = err;
        String b; serializeJson(e, b);
        req->send(rc, "application/json", b);
        return;
    }
    String presetsXml = snap["bmx"]["presets"]["body"] | "";
    if (presetsXml.length() == 0) {
        req->send(500, "application/json", "{\"error\":\"snapshot has no bmx.presets.body\"}");
        return;
    }
    std::vector<sixback::Preset>   presets;
    std::vector<RestoreSlotInfo_>  info;
    parseSnapshotPresetsXml_(presetsXml, presets, info);

    JsonDocument out;
    out["ok"] = true;
    out["captured_at_ms"]    = snap["captured_at_ms"] | (uint32_t)0;
    out["snapshot_kind"]     = snap["snapshot_kind"] | "";
    out["speaker_name"]      = snap["speaker"]["name"]      | "";
    out["speaker_firmware"]  = snap["speaker"]["firmware"]  | "";
    out["restorable_count"]  = (int)presets.size();
    out["abandoned_count"]   = (int)(info.size() - presets.size());
    JsonArray slots = out["slots"].to<JsonArray>();
    for (const auto& s : info) {
        JsonObject o = slots.add<JsonObject>();
        o["slot"]      = s.slot;
        o["source"]    = s.source;
        o["name"]      = s.name;
        o["stationId"] = s.stationId;
        o["streamUrl"] = s.streamUrl;
        o["opaque"]    = s.opaque;
        o["abandoned"] = s.abandoned;
        if (s.abandoned) o["reason"] = s.reason;
        o["will_push"] = !s.abandoned && !s.opaque;
        o["sync_via"]  = s.abandoned ? "n/a (abandoned source — speaker can't play it)"
                       : s.opaque    ? "speaker /full poll (~30s)"
                                     : "doPush_ queue (~7s)";
    }
    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// POST /api/speaker/{id}/restore-pre-migration
//   Commit the restore: write all reconstructed presets to PresetStore +
//   enqueue doPush_ for non-OPAQUE/non-abandoned slots. OPAQUE slots are
//   speaker-managed via /full poll, so we only update the store and trust
//   the speaker to pick them up on its next poll cycle.
void handleRestorePreMigration(AsyncWebServerRequest* req) {
    int qr = ensurePushQueueReady_();
    if (qr == -1) { req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}"); return; }
    if (qr == -2) { req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}"); return; }

    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    JsonDocument snap;
    String err;
    int rc = loadPreMigrateSnapshotDoc_(id, snap, err);
    if (rc != 0) {
        JsonDocument e; e["error"] = err;
        String b; serializeJson(e, b);
        req->send(rc, "application/json", b);
        return;
    }
    String presetsXml = snap["bmx"]["presets"]["body"] | "";
    if (presetsXml.length() == 0) {
        req->send(500, "application/json", "{\"error\":\"snapshot has no bmx.presets.body\"}");
        return;
    }
    std::vector<sixback::Preset>   presets;
    std::vector<RestoreSlotInfo_>  info;
    parseSnapshotPresetsXml_(presetsXml, presets, info);
    if (presets.empty()) {
        req->send(422, "application/json",
                  "{\"error\":\"no restorable presets in snapshot (all 6 slots abandoned)\"}");
        return;
    }

    // Write to PresetStore in one batch. The speaker will pick up OPAQUE
    // slots automatically on its next /full poll.
    sixback::PresetStore::instance().setSlots(id, presets);

    // Push the non-OPAQUE / non-abandoned slots via the existing pipeline.
    int pushedCount = 0, opaqueCount = 0, abandonedCount = 0, queueFull = 0;
    for (const auto& p : presets) {
        if (p.source == sixback::PresetSource::OPAQUE) { ++opaqueCount; continue; }
        if (enqueuePushJob_(spIp, p) == 0) ++pushedCount;
        else                                ++queueFull;
    }
    for (const auto& s : info) if (s.abandoned) ++abandonedCount;

    JsonDocument out;
    out["ok"]              = true;
    out["restored_count"]  = (int)presets.size();
    out["pushed_count"]    = pushedCount;
    out["opaque_count"]    = opaqueCount;
    out["abandoned_count"] = abandonedCount;
    out["queue_full"]      = queueFull;
    out["message"]         = String("restored ") + presets.size() +
                             " slot(s); pushing " + pushedCount +
                             " via /select+long-press (~7s each), " + opaqueCount +
                             " OPAQUE slot(s) will sync on speaker's next /full poll";
    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/key/{KEY}
//   Proxy fuer Speaker-Side /key: kurzer Press + Release. KEY ist eines der
//   Bose-Tasten-Tokens (POWER, PRESET_1..6, AUX, BLUETOOTH, PLAY, PAUSE,
//   NEXT_TRACK, …). Erlaubt der WebUI die Hardware-Tasten zu simulieren ohne
//   CORS-Probleme mit dem Speaker-Endpoint.
// -----------------------------------------------------------------------------
void handleSpeakerKey(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String key = pathParam(1);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">" + key + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int p = sendKey("press");
    delay(150);  // Short press
    int r = sendKey("release");
    JsonDocument out;
    out["ok"] = (p == 200 && r == 200);
    out["key"] = key;
    out["press"] = p;
    out["release"] = r;
    String b; serializeJson(out, b);
    req->send(out["ok"] ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/play-preset/{n}
//   Short-press eines Preset-Slots am Speaker — spielt den Slot wie wenn man
//   physisch die Taste 1..6 drueckt. KEIN Long-Press, KEIN /select-Update.
//   Wenn Slot leer ist, macht der Speaker meistens nichts.
// -----------------------------------------------------------------------------
void handlePlayPreset(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    int slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) { req->send(400, "application/json", "{\"error\":\"slot 1..6\"}"); return; }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" + String(slot) + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int p = sendKey("press");
    delay(150);
    int r = sendKey("release");
    JsonDocument out;
    out["ok"] = (p == 200 && r == 200);
    out["slot"] = slot;
    String b; serializeJson(out, b);
    req->send(out["ok"] ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/play-source
//   Spielt eine Media-Source ad-hoc ab, OHNE einen Preset-Slot anzufassen.
//   Verwendet das gleiche /select-ContentItem wie doPush_ aber ohne den
//   nachfolgenden Long-Press. Use-case: Library-Tile-Preview in der WebUI —
//   User clickt ▶ auf einer Sidebar-Tile, hoert sofort den Stream im
//   Speaker, kann dann mit D&D dauerhaft in einen Slot legen.
// Body JSON:
//   { "source": "TUNEIN" | "LOCAL_INTERNET_RADIO",
//     "name":   "<itemName>",
//     "stationId": "s24896",       (TUNEIN only)
//     "streamUrl": "http://…"      (LOCAL_INTERNET_RADIO only) }
// -----------------------------------------------------------------------------
void handlePlaySource(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    String srcStr = String((const char*)(body["source"] | ""));
    sixback::PresetSource src = sixback::presetSourceFromStr(srcStr);
    if (src != sixback::PresetSource::TUNEIN &&
        src != sixback::PresetSource::LOCAL_INTERNET_RADIO) {
        req->send(400, "application/json",
                  "{\"error\":\"source must be TUNEIN or LOCAL_INTERNET_RADIO\"}");
        return;
    }
    String name      = (const char*)(body["name"]      | "");
    String stationId = (const char*)(body["stationId"] | "");
    String streamUrl = (const char*)(body["streamUrl"] | "");
    String imageUrl  = (const char*)(body["imageUrl"]  | "");
    if (src == sixback::PresetSource::TUNEIN && stationId.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"stationId required for TUNEIN\"}"); return;
    }
    if (src == sixback::PresetSource::LOCAL_INTERNET_RADIO && streamUrl.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"streamUrl required for LOCAL_INTERNET_RADIO\"}"); return;
    }
    sixback::Preset tmp;
    tmp.slot      = 0;
    tmp.source    = src;
    tmp.name      = name;
    tmp.stationId = stationId;
    tmp.streamUrl = streamUrl;
    tmp.imageUrl  = imageUrl;
    int code = sixback::selectStationOnSpeaker(spIp, tmp);
    JsonDocument out;
    out["ok"]     = (code == 200);
    out["select"] = code;
    out["name"]   = name;
    out["source"] = sixback::presetSourceToStr(src);
    String b; serializeJson(out, b);
    req->send(code == 200 ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// ZoneManager — device-direct Multiroom (BMX :8090 /setZone etc.)
//   Stateless: liest die Live-Wahrheit aus /getZone des Masters. Eigene
//   Schicht, getrennt vom cloud-seitigen Group-Store (handlePutGroup/Sync).
// -----------------------------------------------------------------------------

// Serialisiert eine ZoneView nach JSON (geteilt von handleGetZone +
// den POST-Handlern fuer instant feedback). isMaster wird relativ zur
// abgefragten deviceId abgeleitet.
void emitZoneView_(JsonDocument& out, const sixback::ZoneView& zv, const String& forId) {
    out["inZone"]   = zv.inZone;
    out["masterId"] = zv.masterId;
    out["isMaster"] = zv.inZone && (zv.masterId == forId);
    JsonArray arr = out["members"].to<JsonArray>();
    for (const auto& m : zv.members) {
        JsonObject o = arr.add<JsonObject>();
        o["deviceId"] = m.deviceId;
        o["ip"]       = m.ip;
        o["name"]     = m.name;
    }
}

// GET /api/speaker/{id}/zone — ZoneView des Masters fuer die angefragte id.
void handleGetZone(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        if (!inv.findById(id)) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    }
    sixback::ZoneView zv = sixback::zoneStatus(id);
    JsonDocument out;
    emitZoneView_(out, zv, id);
    String b; serializeJson(out, b);
    req->send(200, "application/json", b);
}

// POST /api/zone/create  { master, members:[deviceId,...] }
void handleZoneCreate(AsyncWebServerRequest* req, JsonDocument& body) {
    String master = String((const char*)(body["master"] | ""));
    if (master.length() == 0) { req->send(400, "application/json", "{\"error\":\"master required\"}"); return; }
    std::vector<String> slaves;
    if (body["members"].is<JsonArray>()) {
        for (JsonVariant v : body["members"].as<JsonArray>()) {
            String sid = String((const char*)(v | ""));
            if (sid.length() && sid != master) slaves.push_back(sid);
        }
    }
    if (slaves.empty()) { req->send(400, "application/json", "{\"error\":\"members required\"}"); return; }
    int rc = sixback::zoneCreate(master, slaves);
    if (rc == -1) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    JsonDocument out;
    out["ok"] = (rc == 0);
    sixback::ZoneView zv = sixback::zoneStatus(master);
    emitZoneView_(out, zv, master);
    String b; serializeJson(out, b);
    req->send(rc == 0 ? 200 : 502, "application/json", b);
}

// POST /api/zone/dissolve  { master }
void handleZoneDissolve(AsyncWebServerRequest* req, JsonDocument& body) {
    String master = String((const char*)(body["master"] | ""));
    if (master.length() == 0) { req->send(400, "application/json", "{\"error\":\"master required\"}"); return; }
    int rc = sixback::zoneDissolve(master);
    if (rc == -1) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    JsonDocument out;
    out["ok"] = (rc == 0);
    String b; serializeJson(out, b);
    req->send(rc == 0 ? 200 : 502, "application/json", b);
}

// POST /api/zone/member  { master, slave, op:"add"|"remove" }
void handleZoneMember(AsyncWebServerRequest* req, JsonDocument& body) {
    String master = String((const char*)(body["master"] | ""));
    String slave  = String((const char*)(body["slave"]  | ""));
    String op     = String((const char*)(body["op"]     | ""));
    if (master.length() == 0 || slave.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"master and slave required\"}"); return;
    }
    int rc;
    if (op == "add")         rc = sixback::zoneAdd(master, slave);
    else if (op == "remove") rc = sixback::zoneRemove(master, slave);
    else { req->send(400, "application/json", "{\"error\":\"op must be add or remove\"}"); return; }
    if (rc == -1) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
    JsonDocument out;
    out["ok"] = (rc == 0);
    out["op"] = op;
    sixback::ZoneView zv = sixback::zoneStatus(master);
    emitZoneView_(out, zv, master);
    String b; serializeJson(out, b);
    req->send(rc == 0 ? 200 : 502, "application/json", b);
}

// -----------------------------------------------------------------------------
// Stereo-Pair (ST10) — device-direct via BMX :8090 /addGroup + /removeGroup.
//   Live-Wahrheit aus /getGroup des Speakers (stateless, analog ZoneManager).
//   On-device verifiziert 2026-06-07 (2x ST10): addGroup am Master (= LEFT)
//   bildet das Paar sofort (status GROUP_OK, kein Reboot, Geraetenamen
//   bleiben); der Speaker registriert die Gruppe selbst im cloud-seitigen
//   Group-Store und loescht sie dort beim removeGroup wieder. removeGroup
//   ist ein GET und MUSS an den Master gehen (thlucas1/bosesoundtouchapi,
//   RemoveGroupStereoPair). Stereo-Pair ist laut derselben Quelle ST10-only;
//   supportedURLs taugt NICHT als Gate (auch die ST30 advertised /addGroup —
//   verifiziert 2026-06-07): das Modell-Gating macht die UI, das Backend
//   gated bewusst nicht.
// -----------------------------------------------------------------------------
struct StereoView {
    bool   paired = false;
    String groupId, name, masterId;
    struct Role { String deviceId, role, ip; };
    std::vector<Role> roles;
};

// GET /getGroup eines Speakers + Parse. false nur bei HTTP-/Netz-Fehler;
// "kein Paar" ist ein Erfolgsfall (paired=false).
static bool fetchStereoView_(const String& spIp, StereoView& out) {
    HTTPClient h;
    h.setReuse(false);
    h.setConnectTimeout(2000);
    h.setTimeout(4000);
    String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/getGroup";
    if (!h.begin(u)) return false;
    int code = h.GET();
    if (code != 200) { h.end(); return false; }
    String xml = h.getString();
    h.end();

    auto attr = [&](const char* tag, const char* a) -> String {
        int t = xml.indexOf(String("<") + tag);
        if (t < 0) return String();
        int e = xml.indexOf('>', t);
        if (e < 0) return String();
        String hdr = xml.substring(t, e);
        String needle = String(a) + "=\"";
        int s = hdr.indexOf(needle);
        if (s < 0) return String();
        s += needle.length();
        int q = hdr.indexOf('"', s);
        return q > s ? hdr.substring(s, q) : String();
    };
    auto tagIn = [](const String& scope, const char* tag) -> String {
        String open = String("<") + tag + ">", close = String("</") + tag + ">";
        int s = scope.indexOf(open);
        if (s < 0) return String();
        s += open.length();
        int e = scope.indexOf(close, s);
        return e > s ? scope.substring(s, e) : String();
    };

    out = StereoView();
    out.groupId = attr("group", "id");
    if (out.groupId.length() == 0) return true;   // <group /> = kein Paar
    out.paired   = true;
    out.name     = sixback::unescapeXml(tagIn(xml, "name"));
    out.masterId = tagIn(xml, "masterDeviceId");
    int pos = 0;
    while (true) {
        int p = xml.indexOf("<groupRole>", pos);
        if (p < 0) break;
        int e = xml.indexOf("</groupRole>", p);
        if (e < 0) break;
        String body = xml.substring(p, e);
        StereoView::Role r;
        r.deviceId = tagIn(body, "deviceId");
        r.role     = tagIn(body, "role");
        r.ip       = tagIn(body, "ipAddress");
        if (r.deviceId.length()) out.roles.push_back(r);
        pos = e + 12;
    }
    return true;
}

static void emitStereoView_(JsonDocument& out, const StereoView& sv, const String& forId) {
    out["paired"]   = sv.paired;
    out["groupId"]  = sv.groupId;
    out["name"]     = sv.name;
    out["masterId"] = sv.masterId;
    out["isMaster"] = sv.paired && (sv.masterId == forId);
    JsonArray arr = out["roles"].to<JsonArray>();
    for (const auto& r : sv.roles) {
        JsonObject o = arr.add<JsonObject>();
        o["deviceId"] = r.deviceId;
        o["role"]     = r.role;
        o["ip"]       = r.ip;
    }
}

// GET /api/speaker/{id}/stereo-pair — Live-Paar-Status vom Speaker.
void handleStereoPairGet(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String spIp;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    StereoView sv;
    if (!fetchStereoView_(spIp, sv)) {
        req->send(502, "application/json", "{\"error\":\"speaker unreachable\"}");
        return;
    }
    JsonDocument out;
    emitStereoView_(out, sv, id);
    String b; serializeJson(out, b);
    req->send(200, "application/json", b);
}

// POST /api/speaker/{id}/stereo-pair  { partner:"<deviceId>", name?:"..." }
//   {id} wird Master + LEFT, partner wird RIGHT (Konvention aus dem
//   on-device-Test + Feld-Report #22). Default-Name: "<left> + <right>".
void handleStereoPairCreate(AsyncWebServerRequest* req, JsonDocument& body) {
    String id      = pathParam(0);
    String partner = String((const char*)(body["partner"] | ""));
    String name    = String((const char*)(body["name"]    | ""));
    if (partner.length() == 0) { req->send(400, "application/json", "{\"error\":\"partner required\"}"); return; }
    if (partner == id)         { req->send(400, "application/json", "{\"error\":\"partner must differ\"}"); return; }

    String ipL, ipR, nameL, nameR;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* l = inv.findById(id);
        auto* r = inv.findById(partner);
        if (!l || !r) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        ipL = l->ip; nameL = l->name;
        ipR = r->ip; nameR = r->name;
    }

    // Pre-Check: keiner der beiden darf schon in einem Paar stecken.
    StereoView pre;
    if (fetchStereoView_(ipL, pre) && pre.paired) {
        req->send(409, "application/json", "{\"error\":\"speaker already in a stereo pair — unpair first\"}");
        return;
    }
    if (fetchStereoView_(ipR, pre) && pre.paired) {
        req->send(409, "application/json", "{\"error\":\"partner already in a stereo pair — unpair first\"}");
        return;
    }

    if (name.length() == 0) name = nameL + " + " + nameR;

    // WICHTIG: addGroup darf NICHT synchron aus dem Handler laufen. Der
    // Speaker ruft waehrend der addGroup-Verarbeitung SYNCHRON zu unserem
    // BMX-Server (:8000) zurueck (Group-Registrierung); beide Server teilen
    // sich den async_tcp-Task — ein blockierender Handler verhungert den
    // Callback -> Deadlock bis Timeout (-11), on-device beobachtet
    // 2026-06-07. Deshalb fire-and-forget-Task + 202; die UI re-pollt den
    // Paar-Status sowieso (kickPollStereo).
    struct Ctx { String xml, ipL, id, partner; };
    Ctx* ctx = new Ctx();
    ctx->ipL = ipL; ctx->id = id; ctx->partner = partner;
    ctx->xml.reserve(512);
    ctx->xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><group>";
    ctx->xml += "<name>"; ctx->xml += sixback::escapeXml(name); ctx->xml += "</name>";
    ctx->xml += "<masterDeviceId>"; ctx->xml += id; ctx->xml += "</masterDeviceId>";
    ctx->xml += "<roles>";
    ctx->xml += "<groupRole><deviceId>"; ctx->xml += id;      ctx->xml += "</deviceId><role>LEFT</role><ipAddress>";  ctx->xml += ipL; ctx->xml += "</ipAddress></groupRole>";
    ctx->xml += "<groupRole><deviceId>"; ctx->xml += partner; ctx->xml += "</deviceId><role>RIGHT</role><ipAddress>"; ctx->xml += ipR; ctx->xml += "</ipAddress></groupRole>";
    ctx->xml += "</roles></group>";

    BaseType_t rc = xTaskCreate([](void* arg) {
        Ctx* c = (Ctx*)arg;
        HTTPClient h;
        h.setReuse(false);
        h.setConnectTimeout(2000);
        h.setTimeout(10000);
        String u = "http://" + c->ipL + ":" + String(BOSE_BMX_PORT) + "/addGroup";
        int code = -1; String resp;
        if (h.begin(u)) {
            h.addHeader("Content-Type", "application/xml");
            code = h.POST(c->xml);
            if (code > 0) resp = h.getString();
            h.end();
        }
        bool ok = (code == 200) && (resp.indexOf("<status>GROUP_OK</status>") >= 0);
        Serial.printf("[stereo] addGroup %s+%s -> HTTP %d %s\n",
                      c->id.c_str(), c->partner.c_str(), code, ok ? "GROUP_OK" : "FAIL");
        delete c;
        vTaskDelete(nullptr);
    }, "stereo-pair", 8192, ctx, 1, nullptr);
    if (rc != pdPASS) {
        delete ctx;
        req->send(500, "application/json", "{\"error\":\"task create failed\"}");
        return;
    }
    req->send(202, "application/json", "{\"ok\":true,\"state\":\"pairing\"}");
}

// DELETE /api/speaker/{id}/stereo-pair — trennt das Paar. Funktioniert von
// beiden Karten aus: removeGroup wird immer an den MASTER geschickt
// (Pflicht laut bosesoundtouchapi; on-device bestaetigt). Idempotent.
void handleStereoPairRemove(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String spIp;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    StereoView sv;
    if (!fetchStereoView_(spIp, sv)) {
        req->send(502, "application/json", "{\"error\":\"speaker unreachable\"}");
        return;
    }
    if (!sv.paired) {
        req->send(200, "application/json", "{\"ok\":true,\"paired\":false}");
        return;
    }
    // Master-IP aufloesen: aus den Rollen (tragen ipAddress), sonst Inventory.
    String masterIp = spIp;
    if (sv.masterId != id) {
        masterIp = "";
        for (const auto& r : sv.roles) {
            if (r.deviceId == sv.masterId && r.ip.length()) { masterIp = r.ip; break; }
        }
        if (masterIp.length() == 0) {
            auto& inv = sixback::SpeakerInventory::instance();
            sixback::SpeakerInventory::LockGuard g(inv);
            auto* m = inv.findById(sv.masterId);
            if (m) masterIp = m->ip;
        }
        if (masterIp.length() == 0) {
            req->send(502, "application/json", "{\"error\":\"master speaker not resolvable\"}");
            return;
        }
    }
    // Wie beim Pairing: der Speaker loescht waehrend removeGroup synchron
    // den Eintrag in unserem Group-Store (:8000) — removeGroup deshalb
    // ebenfalls als fire-and-forget-Task statt blockierend im Handler
    // (async_tcp-Starvation, s.o.). 202 + UI-Re-Poll.
    struct Ctx { String masterIp, masterId; };
    Ctx* ctx = new Ctx();
    ctx->masterIp = masterIp;
    ctx->masterId = sv.masterId;
    BaseType_t rc = xTaskCreate([](void* arg) {
        Ctx* c = (Ctx*)arg;
        HTTPClient h;
        h.setReuse(false);
        h.setConnectTimeout(2000);
        h.setTimeout(10000);
        String u = "http://" + c->masterIp + ":" + String(BOSE_BMX_PORT) + "/removeGroup";
        int code = -1;
        if (h.begin(u)) { code = h.GET(); h.end(); }
        Serial.printf("[stereo] removeGroup master=%s -> HTTP %d\n",
                      c->masterId.c_str(), code);
        delete c;
        vTaskDelete(nullptr);
    }, "stereo-unpair", 8192, ctx, 1, nullptr);
    if (rc != pdPASS) {
        delete ctx;
        req->send(500, "application/json", "{\"error\":\"task create failed\"}");
        return;
    }
    req->send(202, "application/json", "{\"ok\":true,\"state\":\"unpairing\"}");
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/name  { name: "..." } — Speaker umbenennen (Issue #25).
//   Device-direkt: POST :8090/name mit <name>...</name>. Ground Truth
//   bosesoundtouchapi::SetName -> Put(SoundTouchNodes.name, SimpleConfig('name'))
//   — und deren Put() ist laut Docstring ein HTTP POST. Der Name lebt AUF dem
//   Speaker (reboot-persistent dort); wir spiegeln ihn nach 200 in den
//   Inventory-Cache + NVS, der periodische /info-Refresh haelt ihn danach synchron.
//   Fire-and-forget wie stereo-pair: ob /name speaker-seitig einen synchronen
//   Callback zu unserem BMX-Server ausloest, ist nicht verifiziert — das
//   Worker-Pattern kostet nichts und schliesst die async_tcp-Starvation
//   (Lesson 2026-06-07, #22) kategorisch aus.
//   ⚠ ZWEITER ST10-Quirk (on-device, FW 27.0.3): der Speaker wendet nur EINEN
//   Rename pro Boot-Session an — jeder weitere noopt still (HTTP 200 + altes
//   <info>). Reboot-persistent ist der Name aber. Die Body-Verify unten
//   erkennt den Noop (Cache bleibt korrekt); die UI zeigt dann den
//   Reboot-Hinweis. KEIN Auto-Reboot — Speaker koennte gerade spielen.
// -----------------------------------------------------------------------------
void handleSpeakerRename(AsyncWebServerRequest* req, JsonDocument& body) {
    String id   = pathParam(0);
    String name = String((const char*)(body["name"] | ""));
    name.trim();
    if (name.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"name required\"}");
        return;
    }
    // Konservatives Limit: laengster bekannter Feld-Name liegt weit darunter;
    // schuetzt NVS-Inventory-Eintrag + Speaker-XML vor Ausreissern.
    if (name.length() > 64) {
        req->send(400, "application/json", "{\"error\":\"name too long (max 64 chars)\"}");
        return;
    }
    String spIp;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    struct Ctx { String ip, id, name, xml; };
    Ctx* ctx = new Ctx();
    ctx->ip = spIp; ctx->id = id; ctx->name = name;
    ctx->xml += "<name>"; ctx->xml += sixback::escapeXml(name); ctx->xml += "</name>";

    BaseType_t rc = xTaskCreate([](void* arg) {
        Ctx* c = (Ctx*)arg;
        HTTPClient h;
        h.setReuse(false);
        h.setConnectTimeout(2000);
        h.setTimeout(10000);
        String u = "http://" + c->ip + ":" + String(BOSE_BMX_PORT) + "/name";
        int code = -1; String resp;
        if (h.begin(u)) {
            // ⚠ ST10-Quirk (on-device 2026-06-07, FW 27.0.3): /name verlangt
            // Content-Type "text/xml" — mit "application/xml" antwortet der
            // Speaker 200 + ALTEM <info>-Dokument und wendet NICHTS an
            // (silent noop). addGroup dagegen akzeptiert application/xml;
            // die Picky-ness ist endpoint-spezifisch.
            h.addHeader("Content-Type", "text/xml");
            code = h.POST(c->xml);
            if (code > 0) resp = h.getString();
            h.end();
        }
        // 200 allein beweist nichts (siehe Quirk) — der Speaker echot bei
        // Erfolg das <info>-Dokument mit dem NEUEN Namen. Darauf pruefen.
        bool ok = (code == 200) && (resp.indexOf(c->xml) >= 0);
        if (ok) {
            // Cache aktualisieren; saveToNVS() lockt selbst -> NICHT im Scope rufen.
            auto& inv = sixback::SpeakerInventory::instance();
            {
                sixback::SpeakerInventory::LockGuard g(inv);
                auto* sp = inv.findById(c->id);
                if (sp) sp->name = c->name;
            }
            inv.saveToNVS();
        }
        Serial.printf("[rename] %s -> \"%s\" HTTP %d %s\n",
                      c->id.c_str(), c->name.c_str(), code, ok ? "OK" : "FAIL");
        delete c;
        vTaskDelete(nullptr);
    }, "sp-rename", 8192, ctx, 1, nullptr);
    if (rc != pdPASS) {
        delete ctx;
        req->send(500, "application/json", "{\"error\":\"task create failed\"}");
        return;
    }
    req->send(202, "application/json", "{\"ok\":true,\"state\":\"renaming\"}");
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/now-playing-live
//   Proxy fuer Speaker /now_playing (Live-Pull, nicht aus dem SCMUDC-Event-
//   Cache). Gibt getrimmte JSON-Sicht zurueck mit location-derived
//   station_id und stream_url — das braucht der WebUI-Highlight um Slots
//   per stationId/streamUrl zu matchen, was die SCMUDC-Variante nicht kennt.
// -----------------------------------------------------------------------------
void handleNowPlayingLive(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String cacheKey = "np#" + id;                          // FHEM 144729 v0.8.32
    String cached;
    if (outCacheGet(cacheKey, cached)) { req->send(200, "application/json", cached); return; }
    OutboundGuard guard;                                   // FHEM 144729 #121
    if (!guard.acquired) { outboundReject(req); return; }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    PROBE_SAMPLER(pr);
    HTTPClient h;
    h.setReuse(false);
    h.setConnectTimeout(LIVE_OUT_CONNECT_MS);              // FHEM 144729: hatte KEIN
    h.setTimeout(LIVE_OUT_READ_MS);                        // Timeout -> Default ~5s Blockade
    String u = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/now_playing";
    if (!h.begin(u)) { PROBE_DONE(pr, 0, false); req->send(502, "application/json", "{\"error\":\"http begin failed\"}"); return; }
    PROBE_MARK(pr);
    int code = h.GET();
    PROBE_MARK(pr);
    if (code != 200) {
        h.end();
        PROBE_DONE(pr, 0, false);
        req->send(502, "application/json", String("{\"error\":\"speaker HTTP\",\"code\":") + code + "}");
        return;
    }
    String xml = h.getString();
    PROBE_MARK(pr);
    h.end();
    PROBE_DONE(pr, xml.length(), true);

    auto extractAttr = [&](const String& tag, const String& attr) -> String {
        int t = xml.indexOf("<" + tag);
        if (t < 0) return String();
        int e = xml.indexOf('>', t);
        if (e < 0) return String();
        String hdr = xml.substring(t, e);
        String needle = attr + "=\"";
        int s = hdr.indexOf(needle);
        if (s < 0) return String();
        s += needle.length();
        int q = hdr.indexOf('"', s);
        return q > s ? hdr.substring(s, q) : String();
    };
    auto extractTag = [&](const String& tag) -> String {
        String open  = "<" + tag + ">";
        String close = "</" + tag + ">";
        int s = xml.indexOf(open);
        if (s < 0) return String();
        s += open.length();
        int e = xml.indexOf(close, s);
        return e > s ? xml.substring(s, e) : String();
    };

    String src      = extractAttr("ContentItem", "source");
    String srcAcct  = extractAttr("ContentItem", "sourceAccount");
    String location = extractAttr("ContentItem", "location");
    String itemName = sixback::unescapeXml(extractTag("itemName"));
    String trackName = sixback::unescapeXml(extractTag("track"));
    String playStatus = extractTag("playStatus");
    String art = extractTag("art");

    // location für TUNEIN ist /v1/playback/station/<id> -> nimm den letzten Pfad-Teil als stationId
    String stationId;
    if (src == "TUNEIN" && location.startsWith("/v1/playback/station/")) {
        stationId = location.substring(21);
    }

    JsonDocument out;
    out["source"] = src;
    out["source_account"] = srcAcct;
    out["name"] = itemName;
    out["track"] = trackName;
    out["station_id"] = stationId;
    out["stream_url"] = (src == "LOCAL_INTERNET_RADIO") ? location : "";
    out["art_url"] = art;
    out["play_status"] = playStatus;
    String body; serializeJson(out, body);
    outCachePut(cacheKey, body);                           // FHEM 144729 v0.8.32
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Clock-Display (Uhr-/Standby-Anzeige auf ST20/ST30) — device-direct :8090
//   Am echten ST30 verifiziert 2026-06-14: GET/POST /clockDisplay, Body
//   verschachtelt <clockDisplay><clockConfig timezoneInfo userEnable
//   timeFormat userOffsetMinute brightnessLevel userUtcTime /></clockDisplay>,
//   Content-Type text/xml. Die Uhr-Anzeige ist AUS bei userEnable="false"
//   ODER brightnessLevel="0" (beide schalten ab, userEnable uebersteuert
//   die Helligkeit). Gate: nur Geraete mit Display (capabilities.xml
//   <clockDisplay>true</clockDisplay>; ST20/ST30 true, ST10 false).
// -----------------------------------------------------------------------------
// Extrahiert ein Attribut aus dem self-closing <clockConfig .../>-Tag
// (eindeutig im Dokument -> simples indexOf genuegt). "" wenn nicht da.
static String clockConfigAttr_(const String& xml, const char* attr) {
    String needle = String(attr) + "=\"";
    int s = xml.indexOf(needle);
    if (s < 0) return String();
    s += needle.length();
    int e = xml.indexOf('"', s);
    return e > s ? xml.substring(s, e) : String();
}

// GET /api/speaker/{id}/clock-display -> {has_clock_display, enabled, brightness}
void handleClockDisplayGet(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    String prefix = "http://" + spIp + ":" + String(BOSE_BMX_PORT);

    // (1) Capability-Gate: hat das Geraet ueberhaupt eine Uhr-Anzeige?
    bool hasDisplay = false;
    {
        HTTPClient h; h.setReuse(false); h.setConnectTimeout(2000); h.setTimeout(4000);
        if (h.begin(prefix + "/capabilities")) {
            if (h.GET() == 200) {
                String caps = h.getString();
                hasDisplay = (caps.indexOf("<clockDisplay>true</clockDisplay>") >= 0);
            }
            h.end();
        }
    }

    JsonDocument out;
    out["has_clock_display"] = hasDisplay;
    // (2) aktuellen clockConfig nur lesen, wenn das Geraet ein Display hat
    if (hasDisplay) {
        HTTPClient h; h.setReuse(false); h.setConnectTimeout(2000); h.setTimeout(4000);
        if (h.begin(prefix + "/clockDisplay")) {
            if (h.GET() == 200) {
                String xml = h.getString();
                String ue = clockConfigAttr_(xml, "userEnable");
                String bl = clockConfigAttr_(xml, "brightnessLevel");
                out["enabled"]    = (ue == "true");
                out["brightness"] = bl.length() ? bl.toInt() : 0;
            }
            h.end();
        }
    }
    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// POST /api/speaker/{id}/clock-display  body {enabled?:bool, brightness?:int}
//   Read-modify-write: liest erst den vollen clockConfig vom Speaker,
//   ueberschreibt NUR die gesetzten Felder und sendet den KOMPLETTEN
//   Envelope zurueck (text/xml) — sonst fallen timezoneInfo/timeFormat/
//   userOffsetMinute auf Defaults. Fire-and-forget-Worker (async_tcp-safe,
//   Lesson #22), 202 sofort.
void handleClockDisplaySet(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = pathParam(0);
    bool hasEnabled    = !body["enabled"].isNull();
    bool hasBrightness = !body["brightness"].isNull();
    if (!hasEnabled && !hasBrightness) {
        req->send(400, "application/json", "{\"error\":\"need enabled and/or brightness\"}");
        return;
    }
    bool enabled    = body["enabled"] | true;
    int  brightness = body["brightness"] | 0;
    if (brightness < 0)   brightness = 0;
    if (brightness > 100) brightness = 100;

    String spIp;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    struct Ctx { String ip, id; bool setEn, setBr, en; int br; };
    Ctx* ctx = new Ctx();
    ctx->ip = spIp; ctx->id = id;
    ctx->setEn = hasEnabled; ctx->setBr = hasBrightness;
    ctx->en = enabled; ctx->br = brightness;

    BaseType_t rc = xTaskCreate([](void* arg) {
        Ctx* c = (Ctx*)arg;
        HTTPClient h;
        h.setReuse(false); h.setConnectTimeout(2000); h.setTimeout(10000);
        String base = "http://" + c->ip + ":" + String(BOSE_BMX_PORT) + "/clockDisplay";

        // read: aktuellen clockConfig holen (read-modify-write)
        String cur;
        if (h.begin(base)) { if (h.GET() == 200) cur = h.getString(); h.end(); }
        String tz  = clockConfigAttr_(cur, "timezoneInfo");
        String ue  = clockConfigAttr_(cur, "userEnable");
        String tf  = clockConfigAttr_(cur, "timeFormat");
        String uom = clockConfigAttr_(cur, "userOffsetMinute");
        String bl  = clockConfigAttr_(cur, "brightnessLevel");
        String uut = clockConfigAttr_(cur, "userUtcTime");
        // Defaults falls der GET fehlschlug (Felder vom echten ST30)
        if (ue.length()  == 0) ue  = "true";
        if (tf.length()  == 0) tf  = "TIME_FORMAT_24HOUR_ID";
        if (uom.length() == 0) uom = "0";
        if (bl.length()  == 0) bl  = "70";
        if (uut.length() == 0) uut = "0";
        // modify: nur die angeforderten Felder
        if (c->setEn) ue = c->en ? "true" : "false";
        if (c->setBr) bl = String(c->br);

        String xml = "<clockDisplay><clockConfig";
        if (tz.length()) { xml += " timezoneInfo=\""; xml += tz; xml += "\""; }
        xml += " userEnable=\"";       xml += ue;  xml += "\"";
        xml += " timeFormat=\"";       xml += tf;  xml += "\"";
        xml += " userOffsetMinute=\""; xml += uom; xml += "\"";
        xml += " brightnessLevel=\"";  xml += bl;  xml += "\"";
        xml += " userUtcTime=\"";      xml += uut; xml += "\"";
        xml += " /></clockDisplay>";

        int code = -1;
        if (h.begin(base)) {
            h.addHeader("Content-Type", "text/xml");  // ST-Endpoints picky (vgl. /name)
            code = h.POST(xml);
            h.end();
        }
        Serial.printf("[clock] %s userEnable=%s brightness=%s HTTP %d\n",
                      c->id.c_str(), ue.c_str(), bl.c_str(), code);
        delete c;
        vTaskDelete(nullptr);
    }, "sp-clock", 8192, ctx, 1, nullptr);
    if (rc != pdPASS) {
        delete ctx;
        req->send(500, "application/json", "{\"error\":\"task create failed\"}");
        return;
    }
    req->send(202, "application/json", "{\"ok\":true,\"state\":\"clock-set\"}");
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/dlna/preset/{slot}
//   Records an OPAQUE STORED_MUSIC preset by emulating what a user would do
//   at the speaker (long-press): /select the DLNA ContentItem -> wait for
//   stream stabilization -> /key press PRESET_N + sleep + release. After
//   the long-press completes the speaker has the new preset locally, so
//   we re-pull /presets via importPresetsFromSpeaker_ to capture the
//   rawContentItem into PresetStore.
//
// Body JSON:
//   { "uuid":        "4d696e69-444c-...",        // DLNA server UUID
//     "object_id":   "1$4",                       // DIDL ObjectID
//     "name":        "All Music",                 // <itemName>
//     "type":        "dir" | "track" }            // container vs item
//
// Runs in a one-shot FreeRTOS task; the handler returns 202 immediately
// because the long-press alone burns ~12 s and would block AsyncWebServer.
// -----------------------------------------------------------------------------
struct RecordDlnaJob_ {
    String deviceId;
    String spIp;
    String uuid;
    String objectId;
    String itemName;
    String itemType;     // "dir" / "track"
    uint8_t slot;
};

static void recordDlnaWorker_(void* arg) {
    RecordDlnaJob_* job = (RecordDlnaJob_*)arg;

    // /select the STORED_MUSIC ContentItem so the speaker switches source +
    // starts streaming the container/track. Long-press would otherwise save
    // whatever was previously playing.
    {
        HTTPClient http;
        http.setReuse(false);
        String url = "http://" + job->spIp + ":" + String(BOSE_BMX_PORT) + "/select";
        int selectCode = -1;
        if (http.begin(url)) {
            String ci = "<ContentItem source=\"STORED_MUSIC\" sourceAccount=\"";
            ci += job->uuid; ci += "/0\" location=\"";
            ci += job->objectId;
            ci += "\" isPresetable=\"true\" type=\"";
            ci += job->itemType.length() ? job->itemType : String("dir");
            ci += "\"><itemName>";
            ci += sixback::escapeXml(job->itemName);
            ci += "</itemName></ContentItem>";
            http.addHeader("Content-Type", "text/xml");
            selectCode = http.POST(ci);
            http.end();
        }
        if (selectCode != 200) {
            Serial.printf("[bg:dlna-record] %s slot %u ABORT — /select=%d\n",
                          job->spIp.c_str(), job->slot, selectCode);
            delete job;
            vTaskDelete(nullptr);
            return;
        }
    }

    // Auf echtes PLAY_STATE warten statt blind 8s — mirrors doPush_ rationale;
    // DLNA stream-start can be slow (server has to seek + open file). Bei Timeout
    // konservativ trotzdem fortfahren (kein Regress des bestehenden Verhaltens).
    if (!waitForPlayState_(job->spIp, "STORED_MUSIC", 18000)) {
        Serial.printf("[bg:dlna-record] %s slot %u — no PLAY_STATE within 18s, long-press anyway\n",
                      job->spIp.c_str(), job->slot);
    }

    auto sendKey = [&](const String& state) {
        HTTPClient h;
        h.setReuse(false);
        String u = "http://" + job->spIp + ":" + String(BOSE_BMX_PORT) + "/key";
        if (!h.begin(u)) return -1;
        h.addHeader("Content-Type", "text/xml");
        String body = "<key state=\"" + state + "\" sender=\"Gabbo\">PRESET_" +
                      String(job->slot) + "</key>";
        int rc = h.POST(body);
        h.end();
        return rc;
    };
    int pressRc   = sendKey("press");
    delay(2500);  // > 2 s = long-press, < 0.5 s = short-press (PLAY)
    int releaseRc = sendKey("release");
    Serial.printf("[bg:dlna-record] %s slot %u select=200 press=%d release=%d\n",
                  job->spIp.c_str(), job->slot, pressRc, releaseRc);

    // Speaker needs a moment to flush /presets after the long-press recording.
    delay(1500);

    // Pull /presets back into PresetStore — this captures the new OPAQUE slot
    // with its rawContentItem. We pass null JsonArrays because we have no
    // response to enrich (background task).
    int countOk = 0, countAban = 0, httpCode = 0;
    int rc = importPresetsFromSpeaker_(job->deviceId, countOk, countAban,
                                        httpCode, JsonArray(), JsonArray());
    Serial.printf("[bg:dlna-record] %s import-after-record rc=%d ok=%d aban=%d\n",
                  job->spIp.c_str(), rc, countOk, countAban);

    delete job;
    vTaskDelete(nullptr);
}

void handleDlnaRecordPreset(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = pathParam(0);
    uint8_t slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    String uuid     = (const char*)(body["uuid"]      | "");
    String objectId = (const char*)(body["object_id"] | "");
    String name     = (const char*)(body["name"]      | "");
    String type     = (const char*)(body["type"]      | "dir");
    if (uuid.length() == 0 || objectId.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"uuid and object_id required\"}");
        return;
    }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    bool ownedByUs = false;
    String cloudUrl;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
        ownedByUs = sp->ownedByUs;
        cloudUrl = sp->cloudUrl;
    }
    // Peer-aware: a non-owner stick would record OPAQUE at the speaker, only
    // for the owner's auto-mode tick to overwrite the slot from its store
    // moments later (verified 2026-05-27 on C6 vs. S3 + Emma). Refuse with
    // a hint so the user knows which stick to use or to migrate the speaker.
    if (!ownedByUs) {
        String e = "{\"error\":\"speaker not owned by this SixBack\",\"hint\":\"";
        e += "Slot would be overwritten by the owning SixBack moments after recording. ";
        e += "Migrate the speaker to this SixBack, or use the owner stick. ";
        e += "Speaker currently polls: ";
        e += cloudUrl.length() ? cloudUrl : String("(unknown)");
        e += "\"}";
        req->send(409, "application/json", e);
        return;
    }

    auto* job = new RecordDlnaJob_{id, spIp, uuid, objectId, name, type, slot};
    BaseType_t r = xTaskCreate(recordDlnaWorker_, "bg-dlna-rec", 4096, job,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        delete job;
        req->send(503, "application/json", "{\"error\":\"task spawn failed\"}");
        return;
    }
    req->send(202, "application/json",
              "{\"ok\":true,\"queued\":true,"
              "\"message\":\"recording DLNA preset on speaker — refresh slot in ~14s\"}");
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/dlna/servers
//   Fresh-Pull /listMediaServers vom Speaker und gibt die discovered DLNA-
//   Server als JSON zurueck. Die UI braucht hier mehr als die in
//   SpeakerInventory gecachten UUIDs — sie braucht auch location (zur
//   MediaServerDevDesc.xml) und friendly_name. Browse selbst macht dann
//   der sixback-dlna-Proxy (Port 8790).
// -----------------------------------------------------------------------------
void handleDlnaServers(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/listMediaServers";
    if (!http.begin(url)) {
        req->send(502, "application/json", "{\"error\":\"http begin failed\"}");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        http.end();
        String e = "{\"error\":\"speaker HTTP\",\"code\":"; e += code; e += "}";
        req->send(502, "application/json", e);
        return;
    }
    String xml = http.getString();
    http.end();

    JsonDocument out;
    out["ok"] = true;
    JsonArray arr = out["servers"].to<JsonArray>();

    auto getAttr = [](const String& chunk, const char* attr) -> String {
        String needle = String(attr) + "=\"";
        int s = chunk.indexOf(needle);
        if (s < 0) return String();
        s += needle.length();
        int e = chunk.indexOf('"', s);
        return e > s ? chunk.substring(s, e) : String();
    };
    int pos = 0;
    while (true) {
        int msStart = xml.indexOf("<media_server", pos);
        if (msStart < 0) break;
        int msEnd = xml.indexOf("/>", msStart);
        if (msEnd < 0) msEnd = xml.indexOf(">", msStart);
        if (msEnd < 0) break;
        String chunk = xml.substring(msStart, msEnd);
        String uuid = getAttr(chunk, "id");
        String name = getAttr(chunk, "friendly_name");
        String manu = getAttr(chunk, "manufacturer");
        String model = getAttr(chunk, "model_name");
        String location = getAttr(chunk, "location");
        if (uuid.length() > 0 && location.length() > 0) {
            JsonObject o = arr.add<JsonObject>();
            o["uuid"] = uuid;
            o["name"] = name;
            o["manufacturer"] = manu;
            o["model"] = model;
            o["location"] = location;
        }
        pos = msEnd + 1;
    }

    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// GET /api/dlna/describe?location=<MediaServerDevDesc-URL>
// GET /api/dlna/browse?control_url=<CDS-controlURL>&object_id=&start=&count=
//   Same-origin replacement for the old LAN-bound sixback-dlna proxy. The
//   firmware talks UPnP ContentDirectory directly to the media server (plain
//   HTTP on the LAN). Browse uses a streaming parser so peak heap depends on
//   the page size, not the folder size. Target+location are RFC1918-guarded in
//   dlna_browse.cpp. These are server-scoped (not speaker-scoped): the URL
//   from /dlna/servers fully identifies the target.
// -----------------------------------------------------------------------------
void handleDlnaDescribe(AsyncWebServerRequest* req) {
    if (!req->hasParam("location")) {
        req->send(400, "application/json", "{\"ok\":false,\"detail\":\"location required\"}");
        return;
    }
    String out;
    int code = sixback::dlnaDescribe(req->getParam("location")->value(), out);
    req->send(code, "application/json", out);
}

void handleDlnaBrowse(AsyncWebServerRequest* req) {
    if (!req->hasParam("control_url")) {
        req->send(400, "application/json", "{\"ok\":false,\"detail\":\"control_url required\"}");
        return;
    }
    String control = req->getParam("control_url")->value();
    String objectId = req->hasParam("object_id") ? req->getParam("object_id")->value() : String("0");
    long start = req->hasParam("start") ? req->getParam("start")->value().toInt() : 0;
    long count = req->hasParam("count") ? req->getParam("count")->value().toInt() : 50;
    String out;
    int code = sixback::dlnaBrowse(control, objectId, start, count, out);
    req->send(code, "application/json", out);
}

// -----------------------------------------------------------------------------
// Helper: Speaker-Side /presets-XML fetchen + grob parsen.
// Returnt true wenn Fetch+Parse erfolgreich. Slot-Slots ohne preset bleiben
// mit leerem source. Synchroner HTTP-Call (~100-300 ms) — akzeptabel weil
// nur 1× pro push-all User-Klick aufgerufen.
// -----------------------------------------------------------------------------
struct HwSlotInfo_ {
    String source;     // "TUNEIN" / "LOCAL_INTERNET_RADIO" / "STORED_MUSIC" / "" (empty)
    String location;   // raw "location" attr value
    String name;       // <itemName>
};

static bool fetchHardwarePresets_(const String& spIp, HwSlotInfo_ out[6]) {
    PROBE_SAMPLER(pr);
    HTTPClient h; h.setReuse(false);
    h.setConnectTimeout(LIVE_OUT_CONNECT_MS);   // FHEM 144729: Connect bounden
    h.setTimeout(LIVE_OUT_READ_MS);             // (toter Standby-Speaker -> kurz raus)
    String url = "http://" + spIp + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!h.begin(url)) { PROBE_DONE(pr, 0, false); return false; }
    PROBE_MARK(pr);
    int code = h.GET();
    PROBE_MARK(pr);
    if (code != 200) { h.end(); PROBE_DONE(pr, 0, false); return false; }
    String xml = h.getString();
    PROBE_MARK(pr);
    h.end();
    PROBE_DONE(pr, xml.length(), true);
    for (int slot = 1; slot <= 6; ++slot) {
        String tag = "<preset id=\"" + String(slot) + "\"";
        int idx = xml.indexOf(tag);
        if (idx < 0) continue;
        int end = xml.indexOf("</preset>", idx);
        if (end < 0) continue;
        String block = xml.substring(idx, end);
        int ci = block.indexOf("<ContentItem");
        if (ci < 0) continue;
        int ciEnd = block.indexOf('>', ci);
        if (ciEnd < 0) continue;
        String hdr = block.substring(ci, ciEnd);
        auto extract = [&](const String& attr) -> String {
            String n = attr + "=\"";
            int s = hdr.indexOf(n);
            if (s < 0) return String();
            s += n.length();
            int e = hdr.indexOf('"', s);
            return e > s ? hdr.substring(s, e) : String();
        };
        out[slot-1].source   = extract("source");
        out[slot-1].location = extract("location");
        int nm = block.indexOf("<itemName>");
        if (nm >= 0) {
            int ne = block.indexOf("</itemName>", nm);
            if (ne > nm) out[slot-1].name = sixback::unescapeXml(block.substring(nm + 10, ne));
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// GET /api/speaker/{id}/hardware-presets
//   Liest /presets vom Speaker live aus (kein Store-Touch) und liefert
//   strukturierte JSON zurueck. Wird von der WebUI fuer die "on speaker:"-
//   Zeile in der Slot-Karte gebraucht, parallel zum Store-Endpoint.
// -----------------------------------------------------------------------------
void handleGetHardwarePresets(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String cacheKey = "hp#" + id;                          // FHEM 144729 v0.8.32
    String cached;
    if (outCacheGet(cacheKey, cached)) { req->send(200, "application/json", cached); return; }
    OutboundGuard guard;                                   // FHEM 144729 #121
    if (!guard.acquired) { outboundReject(req); return; }
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }
    HwSlotInfo_ hw[6];
    bool ok = fetchHardwarePresets_(spIp, hw);
    JsonDocument doc;
    doc["ok"] = ok;
    JsonArray slots = doc["slots"].to<JsonArray>();
    for (uint8_t s = 1; s <= 6; ++s) {
        JsonObject o = slots.add<JsonObject>();
        o["slot"]     = s;
        o["source"]   = hw[s-1].source;
        o["location"] = hw[s-1].location;
        o["name"]     = hw[s-1].name;
        if (hw[s-1].source == "TUNEIN" && hw[s-1].location.startsWith("/v1/playback/station/")) {
            o["station_id"] = hw[s-1].location.substring(strlen("/v1/playback/station/"));
        }
    }
    String body; serializeJson(doc, body);
    if (ok) outCachePut(cacheKey, body);                   // nur Erfolg cachen
    req->send(ok ? 200 : 502, "application/json", body);
}

// -----------------------------------------------------------------------------
// POST /api/speaker/{id}/push-all
//   Pusht nur die Slots wo Store-Stand != Hardware-Stand. Holt initial das
//   Speaker-/presets-XML als Snapshot, vergleicht jeden Slot, und enqueued
//   nur die Diff-Slots. Beispiel: bei einem Slot 1↔2 Swap werden 2 von 6
//   Slots gepushed, nicht alle 6 (~30s vs ~75s).
//   Skipt: EMPTY (store), OPAQUE (store), und Slots die schon korrekt am
//   Speaker liegen ("synced" im Response).
//   Reihenfolge im Push: slot 1..6 in ascending order.
//
//   Warum manueller Trigger statt auto-push pro PUT/swap: siehe Comment in
//   handleSwapPresets — auto-push war flaky.
// -----------------------------------------------------------------------------
void handleBulkPushPresets(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto& inv = sixback::SpeakerInventory::instance();
    String spIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(id);
        if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown deviceId\"}"); return; }
        spIp = sp->ip;
    }

    if (!g_pushQueue) {
        g_pushQueue = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(PushPresetJob_*));
        if (!g_pushQueue) {
            req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}"); return;
        }
        BaseType_t tr = xTaskCreate(pushPresetWorker_, "push-worker", 4096,
                                     nullptr, tskIDLE_PRIORITY + 1, &g_pushTask);
        if (tr != pdPASS) {
            vQueueDelete(g_pushQueue);
            g_pushQueue = nullptr;
            req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}"); return;
        }
    }

    HwSlotInfo_ hw[6];
    bool hwOk = fetchHardwarePresets_(spIp, hw);

    auto& store = sixback::PresetStore::instance();
    int enqueued = 0, synced = 0, skippedOpaque = 0, skippedEmpty = 0;
    for (uint8_t slot = 1; slot <= 6; ++slot) {
        auto p = store.get(id, slot);
        if (p.source == sixback::PresetSource::EMPTY)  { skippedEmpty++;  continue; }
        if (p.source == sixback::PresetSource::OPAQUE) { skippedOpaque++; continue; }

        if (hwOk) {
            const auto& h = hw[slot-1];
            bool already = false;
            if (p.source == sixback::PresetSource::TUNEIN && h.source == "TUNEIN") {
                String wantLoc = "/v1/playback/station/" + p.stationId;
                if (h.location == wantLoc && h.name == p.name) already = true;
            } else if (p.source == sixback::PresetSource::LOCAL_INTERNET_RADIO
                       && h.source == "LOCAL_INTERNET_RADIO") {
                // Der Speaker meldet die LIR-location in /presets je nach Zustand
                // verschieden: nach dem Long-Press-Save als ge-"backene" rohe
                // streamUrl (der Speaker loest den ORION-Adapter auf und merkt
                // sich die resolved URL), frisch aus account/full (Reboot) als
                // ORION-Adapter-Pfad /station?data=…. BEIDE gelten als synced,
                // sonst wuerde push-all den Slot bei jedem Lauf neu pushen.
                // (on-device 2026-06-03 an Emma: /presets -> rohe streamUrl.)
                String orionLoc = sixback::orionStationLocation(p.streamUrl, p.name, p.imageUrl);
                if ((h.location == p.streamUrl || h.location == orionLoc)
                    && h.name == p.name) already = true;
            }
            if (already) { synced++; continue; }
        }

        auto* job = new PushPresetJob_{spIp, p};
        if (xQueueSend(g_pushQueue, &job, 0) != pdTRUE) {
            delete job;
            req->send(503, "application/json",
                      "{\"error\":\"push queue full mid-bulk — retry in ~1 min\"}");
            return;
        }
        enqueued++;
    }

    UBaseType_t depth = uxQueueMessagesWaiting(g_pushQueue);
    JsonDocument out;
    out["ok"]            = true;
    out["enqueued"]      = enqueued;
    out["synced"]        = synced;        // slots already matching the hardware
    out["skipped_empty"] = skippedEmpty;
    out["skipped_opaque"]= skippedOpaque;
    out["hw_fetched"]    = hwOk;
    out["queue_depth"]   = (unsigned)depth;
    out["eta_seconds"]   = enqueued * 13;
    if (enqueued == 0) {
        out["message"] = hwOk
            ? "all slots already in sync with speaker — nothing to push"
            : "no slots needed pushing (speaker unreachable for diff, all slots EMPTY/OPAQUE)";
    } else {
        out["message"] = hwOk
            ? "pushing only the diff — " + String(enqueued) + " of 6 slots"
            : "pushing all non-empty slots (speaker /presets unreachable for diff)";
    }
    String body; serializeJson(out, body);
    req->send(202, "application/json", body);
}

// -----------------------------------------------------------------------------
// POST /api/preset/swap
//   body: { "src": {"dev":"...", "slot":N}, "dst": {"dev":"...", "slot":M} }
//   Tauscht zwei Slots (zwischen oder innerhalb von Speakern). Beide Stores
//   atomic update, beide Pushes in die existierende push-queue enqueued.
//   OPAQUE als Source oder Target verboten (rawContentItem ist DLNA-source-
//   spezifisch, Cross-Speaker-Replay nicht garantiert). Empty-Source ist
//   erlaubt = effektiv MOVE.
// -----------------------------------------------------------------------------
void handleSwapPresets(AsyncWebServerRequest* req, JsonDocument& body) {
    if (!body["src"].is<JsonObject>() || !body["dst"].is<JsonObject>()) {
        req->send(400, "application/json",
                  "{\"error\":\"src/dst required as objects\"}");
        return;
    }
    String srcDev = (const char*)(body["src"]["dev"] | "");
    String dstDev = (const char*)(body["dst"]["dev"] | "");
    int srcSlot = body["src"]["slot"] | 0;
    int dstSlot = body["dst"]["slot"] | 0;
    if (srcDev.isEmpty() || dstDev.isEmpty() ||
        srcSlot < 1 || srcSlot > 6 || dstSlot < 1 || dstSlot > 6) {
        req->send(400, "application/json",
                  "{\"error\":\"src.dev/dst.dev required, slots 1..6\"}");
        return;
    }
    if (srcDev == dstDev && srcSlot == dstSlot) {
        req->send(200, "application/json", "{\"ok\":true,\"noop\":true}");
        return;
    }

    auto& inv = sixback::SpeakerInventory::instance();
    String srcIp, dstIp;
    {
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp1 = inv.findById(srcDev);
        auto* sp2 = inv.findById(dstDev);
        if (!sp1) { req->send(404, "application/json", "{\"error\":\"unknown src.dev\"}"); return; }
        if (!sp2) { req->send(404, "application/json", "{\"error\":\"unknown dst.dev\"}"); return; }
        srcIp = sp1->ip;
        dstIp = sp2->ip;
    }

    auto& store = sixback::PresetStore::instance();
    auto pSrc = store.get(srcDev, srcSlot);
    auto pDst = store.get(dstDev, dstSlot);

    if (pSrc.source == sixback::PresetSource::OPAQUE ||
        pDst.source == sixback::PresetSource::OPAQUE) {
        req->send(400, "application/json",
            "{\"error\":\"OPAQUE preset cannot be swapped via WebUI — "
            "re-record at speaker via long-press\"}");
        return;
    }

    if (!g_pushQueue) {
        g_pushQueue = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(PushPresetJob_*));
        if (!g_pushQueue) {
            req->send(503, "application/json", "{\"error\":\"push queue alloc failed\"}");
            return;
        }
        BaseType_t tr = xTaskCreate(pushPresetWorker_, "push-worker", 4096,
                                     nullptr, tskIDLE_PRIORITY + 1, &g_pushTask);
        if (tr != pdPASS) {
            vQueueDelete(g_pushQueue);
            g_pushQueue = nullptr;
            req->send(503, "application/json", "{\"error\":\"push worker spawn failed\"}");
            return;
        }
    }

    sixback::Preset newDst = pSrc; newDst.slot = (uint8_t)dstSlot;
    sixback::Preset newSrc = pDst; newSrc.slot = (uint8_t)srcSlot;

    bool ok1 = (newDst.source == sixback::PresetSource::EMPTY)
               ? store.clear(dstDev, dstSlot)
               : store.set(dstDev, newDst);
    bool ok2 = (newSrc.source == sixback::PresetSource::EMPTY)
               ? store.clear(srcDev, srcSlot)
               : store.set(srcDev, newSrc);
    if (!ok1 || !ok2) {
        req->send(500, "application/json", "{\"error\":\"store update failed\"}");
        return;
    }

    // Swap aktualisiert NUR den Store. Push zum Speaker passiert ueber den
    // expliziten "Push to speaker"-Button (handleBulkPushPresets), nicht
    // automatisch — siehe Forum-Feedback 2026-05-23: auto-push war flaky
    // (Speaker STANDBY, /select transient errors, timing-races) und tauschte
    // hardware-Slots oft nicht zuverlaessig. Manueller Trigger erlaubt User
    // die Slots in Ruhe zu komponieren + bewusst zu commit'n.

    JsonDocument out;
    out["ok"] = true;
    out["pushed"] = false;
    out["message"] = "store updated — click 'Push 1-6 to speaker' on each affected speaker to write to hardware";
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// -----------------------------------------------------------------------------
// Gruppen
// -----------------------------------------------------------------------------
void handlePutGroup(AsyncWebServerRequest* req, JsonDocument& body) {
    String id = pathParam(0);
    String groupId = (const char*)(body["group_id"] | "");
    auto& inv = sixback::SpeakerInventory::instance();
    sixback::SpeakerInventory::LockGuard g(inv);
    auto* sp = inv.findById(id);
    if (!sp) { req->send(404, "application/json", "{\"error\":\"unknown\"}"); return; }
    sp->groupId = groupId;
    inv.saveToNVS();
    req->send(200, "application/json", "{\"ok\":true}");
}

void handleSyncGroup(AsyncWebServerRequest* req, JsonDocument& body) {
    String src = (const char*)(body["source_device_id"] | "");
    std::vector<String> targets;
    if (body["target_device_ids"].is<JsonArray>()) {
        for (JsonVariant v : body["target_device_ids"].as<JsonArray>()) {
            targets.push_back(String((const char*)v));
        }
    } else if (body["group_id"].is<const char*>()) {
        String gid = (const char*)body["group_id"];
        for (auto& s : sixback::SpeakerInventory::instance().list()) {
            if (s.groupId == gid && s.deviceId != src) targets.push_back(s.deviceId);
        }
    }
    int n = sixback::PresetStore::instance().syncToGroup(src, targets);
    JsonDocument doc; doc["ok"] = true; doc["synced_to"] = n;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// TuneIn — Resolve fuer UI-Preview
// -----------------------------------------------------------------------------
void handleTuneInResolve(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    auto r = sixback::resolveTuneInStruct(id);
    JsonDocument doc;
    doc["ok"]        = r.ok;
    doc["stationId"] = r.stationId;
    doc["name"]      = r.name;
    doc["streamUrl"] = r.streamUrl;
    doc["imageUrl"]  = r.imageUrl;
    doc["source"]    = r.source;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// POST /api/tunein/cache/clear — verwirft den Resolve-Cache komplett
// (LittleFS /tcache/*.json + Legacy-NVS-Namespace aus Zeiten vor dem
// FS-Umzug). Noetig nach einem Resolver-Verhaltenswechsel, damit Stations,
// die vorher als notcompatible-Platzhalter gecached wurden, neu aufgeloest
// werden.
void handleTuneInCacheClear(AsyncWebServerRequest* req) {
    sixback::clearTuneInCache();
    req->send(200, "application/json", "{\"ok\":true,\"cleared\":\"tunein-cache\"}");
}

// GET /api/tunein/search?q=...
//   Proxy zu http://opml.radiotime.com/Search.ashx?query=<q>&render=json
void handleTuneInSearch(AsyncWebServerRequest* req) {
    String q;
    if (req->hasParam("q")) q = req->getParam("q")->value();
    if (q.length() == 0) { req->send(400, "application/json", "{\"error\":\"q required\"}"); return; }
    if (WiFi.status() != WL_CONNECTED) { req->send(503, "application/json", "{\"error\":\"offline\"}"); return; }
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000); http.setTimeout(5000);
    String url = "http://opml.radiotime.com/Search.ashx?query=" + urlEncodeQuery_(q) + "&render=json";
    if (!http.begin(url)) { req->send(500, "application/json", "{\"error\":\"begin\"}"); return; }
    int code = http.GET();
    if (code != 200) { http.end(); req->send(502, "application/json", "{\"error\":\"upstream\"}"); return; }
    String body = http.getString();
    http.end();
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// System
// -----------------------------------------------------------------------------
void handleFactoryResetWifi(AsyncWebServerRequest* req) {
    sixback::factoryResetWifi();
    req->send(200, "application/json", "{\"ok\":true,\"reboot_in_ms\":500}");
    delay(500); ESP.restart();
}

void handleSelfReboot(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true,\"reboot_in_ms\":500}");
    delay(500); ESP.restart();
}

// -----------------------------------------------------------------------------
// OTA — App-Image (POST /api/ota) und LittleFS-Image (POST /api/ota/fs)
//   Beide multipart/octet-stream firmware upload, Reboot nach Finalize.
//   Unterschied: Update.begin(..., U_FLASH) vs U_SPIFFS — bei arduino-esp32
//   ist U_SPIFFS auch der Pfad fuer LittleFS (gleiche Partition).
// Nur auf Builds mit SIXBACK_OTA_ENABLED (ESP32-classic + S3) — auf C3/C6
// hat die partition keinen A/B-Slot, Update.h kann nicht schreiben.
// -----------------------------------------------------------------------------
#ifdef SIXBACK_OTA_ENABLED
void handleOtaFinalize(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"]    = !Update.hasError();
    doc["error"] = Update.errorString();
    doc["kind"]  = "app";
    String body; serializeJson(doc, body);
    req->send(Update.hasError() ? 500 : 200, "application/json", body);
    if (!Update.hasError()) { delay(500); ESP.restart(); }
}

void handleOtaUpload(AsyncWebServerRequest* req, String filename,
                     size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[ota] APP start %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
        }
    }
    if (Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (Update.end(true)) Serial.printf("[ota] APP done (%u bytes)\n",
                                            (unsigned)(index+len));
        else                  Update.printError(Serial);
    }
}

void handleOtaFsFinalize(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"]    = !Update.hasError();
    doc["error"] = Update.errorString();
    doc["kind"]  = "fs";
    String body; serializeJson(doc, body);
    req->send(Update.hasError() ? 500 : 200, "application/json", body);
    if (!Update.hasError()) { delay(500); ESP.restart(); }
}

void handleOtaFsUpload(AsyncWebServerRequest* req, String filename,
                       size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) {
        Serial.printf("[ota] FS start %s\n", filename.c_str());
        // LittleFS hat keine eigene Update-Konstante - U_SPIFFS funktioniert,
        // weil arduino-esp32 die Daten-Partition unabhaengig vom FS-Type
        // schreibt. WICHTIG: vor dem Schreiben unmounten, sonst frisch
        // geschriebene Daten werden vom alten Mount nicht gesehen.
        LittleFS.end();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
            Update.printError(Serial);
        }
    }
    if (Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (Update.end(true)) Serial.printf("[ota] FS done (%u bytes)\n",
                                            (unsigned)(index+len));
        else                  Update.printError(Serial);
    }
}

// -----------------------------------------------------------------------------
// Online-Update (HTTPS-Pull von sixback.io) — Inspired by
// tul-knx-gateway. Drei Endpoints:
//   GET  /api/update/check    — Manifest holen + Version vergleichen.
//   POST /api/update/install  — Background-Task startet Firmware-Pull + Flash.
//   GET  /api/update/status   — JSON-Snapshot fuer UI-Polling.
// -----------------------------------------------------------------------------
namespace {
const char* otaStateName_(sixback::ota::State s) {
    using S = sixback::ota::State;
    switch (s) {
        case S::IDLE:       return "idle";
        case S::CHECKING:   return "checking";
        case S::AVAILABLE:  return "available";
        case S::INSTALLING: return "installing";
        case S::DONE:       return "done";
        case S::ERROR_:     return "error";
    }
    return "?";
}

void writeOtaStatus_(AsyncWebServerRequest* req) {
    auto st = sixback::ota::getStatus();
    JsonDocument doc;
    doc["state"]    = otaStateName_(st.state);
    doc["current"]  = st.current;
    doc["latest"]   = st.latest;
    doc["progress"] = st.progress;
    doc["total"]    = st.total;
    doc["phase"]    = st.phase;
    doc["phase_idx"]= st.phaseIdx;
    doc["phase_n"]  = st.phaseN;
    doc["error"]    = st.error;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}
} // anon

#ifdef SIXBACK_OTA_SELFUPDATE_UNSUPPORTED
// ESP32-C5: der Online-OTA-Pull braucht einen TLS-Handshake, dessen mbedtls- +
// esp-aes-DMA-Puffer auf dem no-PSRAM-C5 (~252 KB Heap, WiFi-6-Dual-Band-Stack)
// nicht allozierbar sind ("esp-aes: Failed to allocate memory" ->
// ssl_starttls_handshake ERROR). Verifiziert 2026-07-03, 6/6 reproduzierbar.
// Der lokale Multipart-Upload (/api/ota) bleibt nutzbar; nur der Online-Pull
// (Check + Install) wird sauber als "nicht unterstuetzt" gemeldet statt
// kryptischem "HTTP -1". Siehe ~/.claude/docs/esp32-c5-ota-tls-heap.md
static constexpr const char* kOtaSelfUpdateUnsupportedBody =
    "{\"state\":\"error\","
    "\"error\":\"Online OTA is not available on the ESP32-C5 (not enough RAM for the TLS download) \\u2014 update via the Web-Flasher at https://sixback.io/\","
    "\"current\":\"\",\"latest\":\"\",\"progress\":0,\"total\":0,"
    "\"phase\":\"\",\"phase_idx\":0,\"phase_n\":0,"
    "\"updater\":\"https://sixback.io/\"}";
#endif

void handleOtaUpdateCheck(AsyncWebServerRequest* req) {
#ifdef SIXBACK_OTA_SELFUPDATE_UNSUPPORTED
    req->send(200, "application/json", kOtaSelfUpdateUnsupportedBody);
    return;
#endif
    sixback::ota::checkOnline();
    writeOtaStatus_(req);
}

void handleOtaUpdateInstall(AsyncWebServerRequest* req) {
#ifdef SIXBACK_OTA_SELFUPDATE_UNSUPPORTED
    req->send(200, "application/json", kOtaSelfUpdateUnsupportedBody);
    return;
#endif
    // Optional: ?force=1 erlaubt Re-Install des gleichen oder eines aelteren
    // Versions-Standes (z.B. "ich will von sixback.io denselben Build
    // nochmal pullen", oder Demo des Progress-Bars wenn current >= latest).
    bool force = req->hasParam("force") && req->getParam("force")->value() == "1";
    bool ok = force ? sixback::ota::installOnlineForceAsync()
                    : sixback::ota::installOnlineAsync();
    if (ok) { writeOtaStatus_(req); return; }

    // Nicht gestartet. installOnlineAsync() hat ggf. frisch nach-gecheckt, also
    // spiegelt der State den echten Grund — praezise, handlungsleitende Meldung
    // statt pauschalem "no update available" (UX-Bug #6). current/latest mit-
    // liefern, damit das UI-Panel sich am echten Stand ausrichten kann.
    auto st = sixback::ota::getStatus();
    using S = sixback::ota::State;
    JsonDocument doc;
    int httpCode;
    if (st.state == S::INSTALLING) {
        httpCode = 409;
        doc["error"] = "an update is already installing";
    } else if (st.state == S::ERROR_) {
        httpCode = 503;  // transient: Manifest/Netz/TLS — "nochmal versuchen", NICHT "kein Update"
        doc["error"] = "update server unreachable — check Wi-Fi and try again";
        if (st.error.length()) doc["detail"] = st.error;
    } else if (st.state == S::IDLE) {
        httpCode = 409;  // wirklich aktuell
        doc["error"] = String("already up-to-date (") + st.latest +
                       ") — use Force re-install to flash this version anyway";
    } else {
        httpCode = 500;
        doc["error"] = "failed to start update";
    }
    doc["state"]   = otaStateName_(st.state);
    doc["current"] = st.current;
    doc["latest"]  = st.latest;
    String body; serializeJson(doc, body);
    req->send(httpCode, "application/json", body);
}

void handleOtaUpdateStatus(AsyncWebServerRequest* req) {
    writeOtaStatus_(req);
}
#else // SIXBACK_OTA_ENABLED
// Single-app builds (c3/c6): kein OTA-Pull moeglich, Partition zu eng.
// Antwort enthaelt `state:"error"` damit die UI's error-Branch greift und
// dem User die Web-Serial-Updater-URL zeigt (statt leerer Zeile, weil ein
// fehlendes state-Feld den UI-Switch nicht matched — Bug-Report 2026-05-27).
namespace {
constexpr const char* kOtaDisabledBody =
    "{\"state\":\"error\","
    "\"error\":\"OTA disabled on this chip — use the Web-Serial Updater at https://sixback.io/\","
    "\"current\":\"\",\"latest\":\"\",\"progress\":0,\"total\":0,"
    "\"phase\":\"\",\"phase_idx\":0,\"phase_n\":0,"
    "\"updater\":\"https://sixback.io/\"}";
}
void handleOtaUpdateCheck(AsyncWebServerRequest* req) {
    req->send(200, "application/json", kOtaDisabledBody);
}
void handleOtaUpdateInstall(AsyncWebServerRequest* req) {
    req->send(200, "application/json", kOtaDisabledBody);
}
void handleOtaUpdateStatus(AsyncWebServerRequest* req) {
    req->send(200, "application/json", kOtaDisabledBody);
}
#endif // SIXBACK_OTA_ENABLED

// Lab-Befund 2026-07-17 (FHEM 144729 #155): ZWEI gleichzeitige Transfers des
// 75-KB-Bundles machen den no-PSRAM-C5 in ~1-2 s dauerhaft netz-taub — der
// Heap-Sturz passiert WAEHREND der parallelen chunked-Sends, ein Heap-Check am
// Request-Anfang (beide sehen noch ~78K free) greift prinzipiell nicht. Ein
// einzelner Transfer ist auch im Dauerfeuer stabil (256 Loads in Folge, 0
// Loss). Deshalb harte Parallel-Grenze. Decrement via onDisconnect ist hier
// zuverlaessig transfer-genau: diese Lib-Version (ESPAsyncWebServer 3.x)
// schliesst die Verbindung nach JEDER fertigen Response (_onAck ->
// _client->close()), es gibt keinen Keep-Alive-Pfad der den Zaehler pinnen
// koennte.
std::atomic<int> g_indexInflight{0};
inline int indexMaxInflight() { return outboundHasPsram() ? 3 : 1; }

void handleRoot(AsyncWebServerRequest* req) {
    // FHEM 144729 #121: unter Heap-Druck (Page-Load-Burst / mehrere Tabs auf dem
    // no-PSRAM-C5) das schwere 74-KB-Bundle-Serving kurz mit 503 abweisen statt
    // einen Heap-Kollaps zu riskieren (der crasht das Board + wirft WLAN ab).
    // Der Floor ist so hoch, dass ein normaler Einzel-Load nie triggert; die
    // Auto-Refresh-Seite holt sich das Bundle nach ~2 s selbst. Nur no-PSRAM
    // gated (PSRAM-S3 hat Reserve -> outboundHeapFloor() dort niedrig).
    if (ESP.getFreeHeap() < outboundHeapFloor()) {
        req->send(503, "text/html",
                  "<!doctype html><meta http-equiv=\"refresh\" content=\"2\">"
                  "<body style=\"font-family:sans-serif\">SixBack is busy \xE2\x80\x94 retrying\xE2\x80\xA6</body>");
        return;
    }
    int n = g_indexInflight.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > indexMaxInflight()) {
        g_indexInflight.fetch_sub(1, std::memory_order_relaxed);
        req->send(503, "text/html",
                  "<!doctype html><meta http-equiv=\"refresh\" content=\"1\">"
                  "<body style=\"font-family:sans-serif\">SixBack is busy \xE2\x80\x94 retrying\xE2\x80\xA6</body>");
        return;
    }
    req->onDisconnect([]() {
        g_indexInflight.fetch_sub(1, std::memory_order_relaxed);
    });
    // index.html ist >100 KB; AsyncWebServer schafft das im uncompressed-Pfad
    // nicht zuverlaessig (Truncation/Timeouts unter Last). Wenn index.html.gz
    // im LittleFS liegt -> serve mit Content-Encoding: gzip. Browser entpackt.
    if (LittleFS.exists("/index.html.gz")) {
        auto* resp = req->beginResponse(LittleFS, "/index.html.gz", "text/html");
        resp->addHeader("Content-Encoding", "gzip");
        resp->addHeader("Content-Security-Policy", SIXBACK_CSP_HEADER);
        resp->addHeader("X-Content-Type-Options", "nosniff");
        req->send(resp);
        return;
    }
    if (LittleFS.exists("/index.html")) {
        req->send(LittleFS, "/index.html", "text/html");
        return;
    }
    String ip = WiFi.localIP().toString();
    String html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<title>SixBack " FW_VERSION_STRING "</title>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:-apple-system,Segoe UI,sans-serif;max-width:40em;margin:3em auto;padding:0 1em}"
        "code{background:#fee;padding:.1em .3em;border-radius:3px}</style></head><body>"
        "<h1>SixBack</h1><p>Web UI not flashed. Use the JSON API directly:</p>"
        "<ul>"
          "<li><code>GET /api/status</code></li>"
          "<li><code>GET /api/speakers</code></li>"
          "<li><code>POST /api/speakers/discover</code></li>"
          "<li><code>POST /api/speaker/&lt;id&gt;/migrate</code></li>"
          "<li><code>GET/PUT /api/speaker/&lt;id&gt;/preset/&lt;1..6&gt;</code></li>"
          "<li><code>POST /api/group/sync</code></li>"
          "<li><code>GET /api/tunein/resolve/&lt;s24896&gt;</code></li>"
          "<li><code>POST /api/ota</code> (multipart firmware.bin)</li>"
          "<li><code>POST /api/ota/fs</code> (multipart littlefs.bin)</li>"
        "</ul>"
        "<p>Version <code>" FW_VERSION_STRING "</code> &middot; Build " FW_BUILD_DATE
        " &middot; <a href=\"https://github.com/tostmann/SixBack\">github.com/tostmann/SixBack</a></p>"
        "</body></html>";
    req->send(200, "text/html; charset=utf-8", html);
}

} // anon

// -----------------------------------------------------------------------------
// IP-Failsafe Test-Endpoint
//   Setzt NVS `sixback-net.last_ip` auf einen anderen Wert, sodass beim
//   naechsten Boot `ipFailsafeCheck()` einen IP-Wechsel erkennt und alle
//   owned-Speaker per Telnet re-migriert. Reine Test-Hilfe, kein Cleanup
//   noetig — der Failsafe persistiert beim Lauf die aktuelle IP wieder.
//   Mit `trigger_now: true` wird zusaetzlich der Runtime-Recheck gearmt —
//   ipFailsafeTick() laeuft dann SOFORT (ohne Reboot), gleicher Pfad wie
//   ein GOT_IP-Event mit neuer IP.
// -----------------------------------------------------------------------------
void handleTestForceIpChange(AsyncWebServerRequest* req, JsonDocument& body) {
    String fakeIp     = (const char*)(body["fake_ip"] | "10.10.99.99");
    bool   triggerNow = body["trigger_now"] | false;
    Preferences p;
    if (!p.begin("sixback-net", false)) {
        req->send(500, "application/json", "{\"error\":\"nvs open failed\"}"); return;
    }
    String oldStored = p.getString("last_ip", "");
    p.putString("last_ip", fakeIp);
    p.end();
    if (triggerNow) sixback::ipFailsafeArmRecheck();
    JsonDocument doc;
    doc["ok"]                = true;
    doc["nvs_last_ip_was"]   = oldStored;
    doc["nvs_last_ip_now"]   = fakeIp;
    doc["esp_actual_ip"]     = WiFi.localIP().toString();
    doc["next"]              = triggerNow
        ? "armed — ipFailsafeTick() runs within ~1s, watch serial log"
        : "POST /api/reboot to trigger ip_failsafe";
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// Auto-Mode (Zero-Touch Migration beim Boot)
// -----------------------------------------------------------------------------
void handleGetAutoMode(AsyncWebServerRequest* req) {
    auto cfg = sixback::loadAutoModeConfig();
    auto st  = sixback::getAutoModeStatus();
    JsonDocument doc;
    doc["config"]["enabled"]         = cfg.enabled;
    doc["config"]["dry_run"]         = cfg.dryRun;
    doc["config"]["boot_delay_ms"]   = cfg.bootDelayMs;
    doc["config"]["max_per_boot"]    = cfg.maxPerBoot;
    doc["config"]["cron_interval_s"] = cfg.cronIntervalS;
    doc["status"]["ran"]                  = st.ran;
    doc["status"]["running"]              = st.running;
    doc["status"]["state"]                = st.state;
    doc["status"]["current_device"]       = st.currentDeviceId;
    doc["status"]["speakers_seen"]        = st.speakersSeen;
    doc["status"]["speakers_eligible"]    = st.speakersEligible;
    doc["status"]["speakers_migrated"]    = st.speakersMigrated;
    doc["status"]["slots_normalized"]     = st.slotsNormalized;
    doc["status"]["slots_converted"]      = st.slotsConverted;
    doc["status"]["slots_abandoned"]      = st.slotsAbandoned;
    doc["status"]["last_error"]           = st.lastError;
    doc["status"]["started_ms"]           = st.startedMs;
    doc["status"]["finished_ms"]          = st.finishedMs;
    doc["status"]["tick_count"]           = st.tickCount;
    doc["status"]["last_tick_finished_ms"]= st.lastTickFinishedMs;
    doc["status"]["next_tick_in_s"]       = st.nextTickInS;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

void handlePutAutoMode(AsyncWebServerRequest* req, JsonDocument& body) {
    auto cfg = sixback::loadAutoModeConfig();
    if (body["enabled"].is<bool>())            cfg.enabled       = body["enabled"].as<bool>();
    if (body["dry_run"].is<bool>())            cfg.dryRun        = body["dry_run"].as<bool>();
    if (body["boot_delay_ms"].is<uint32_t>())  cfg.bootDelayMs   = body["boot_delay_ms"].as<uint32_t>();
    if (body["max_per_boot"].is<uint32_t>())   cfg.maxPerBoot    = body["max_per_boot"].as<uint32_t>();
    if (body["cron_interval_s"].is<uint32_t>())cfg.cronIntervalS = body["cron_interval_s"].as<uint32_t>();

    bool saved = sixback::saveAutoModeConfig(cfg);
    // Bei Fehlschlag NICHT den angeforderten Wert echoen — das war der Bug: die UI
    // meldete Erfolg (ok:true + Wunschwert), obwohl NVS nichts persistierte, sodass
    // der Toggle nach Reboot/Re-GET zuruecksprang (FHEM 144729, betateilchen C5).
    // Stattdessen den TATSAECHLICH persistierten Stand zuruecklesen und mit 500
    // melden, damit toggleAutoMode()->loadAutoMode() den echten Zustand zeigt.
    auto actual = saved ? cfg : sixback::loadAutoModeConfig();
    JsonDocument resp;
    resp["ok"]                         = saved;
    if (!saved) resp["error"]          = "nvs write failed — setting not persisted";
    resp["config"]["enabled"]          = actual.enabled;
    resp["config"]["dry_run"]          = actual.dryRun;
    resp["config"]["boot_delay_ms"]    = actual.bootDelayMs;
    resp["config"]["max_per_boot"]     = actual.maxPerBoot;
    resp["config"]["cron_interval_s"]  = actual.cronIntervalS;
    String b; serializeJson(resp, b);
    req->send(saved ? 200 : 500, "application/json", b);
}

// -----------------------------------------------------------------------------
// Diagnostic-Sharing — Opt-In fuer Snapshot-Upload zum Maintainer-Receiver
// (Issue #4). Default OFF. Bei Aktivierung wird einmalig der retroaktive
// Upload-Worker getriggert, der existierende /snapshots/*.json hochlaedt.
// -----------------------------------------------------------------------------
void handleGetDiagShare(AsyncWebServerRequest* req) {
    auto cfg = sixback::loadDiagShareConfig();
    JsonDocument doc;
    doc["upload_enabled"] = cfg.uploadEnabled;
    doc["receiver"]       = "https://sixback.io/snapshots/bosefix/snapshot";
    doc["notice"]         = "Snapshot enthaelt: Speaker-Identitaet (MAC, Modell, "
                            "Firmware), Presets + Sources + Now-Playing, "
                            "Speaker-System-Konfiguration. Wird nur bei Debug-"
                            "Anfragen vom Maintainer verwendet.";
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handlePutDiagShare(AsyncWebServerRequest* req, JsonDocument& body) {
    auto cfg = sixback::loadDiagShareConfig();
    bool wasEnabled = cfg.uploadEnabled;
    if (body["upload_enabled"].is<bool>()) {
        cfg.uploadEnabled = body["upload_enabled"].as<bool>();
    }
    sixback::saveDiagShareConfig(cfg);
    // Just-now-aktiviert → retroaktiven Upload der lokal gesammelten
    // Snapshots starten (best-effort, async).
    if (!wasEnabled && cfg.uploadEnabled) {
        sixback::uploadAllStoredSnapshots();
    }
    JsonDocument resp;
    resp["ok"]             = true;
    resp["upload_enabled"] = cfg.uploadEnabled;
    String b; serializeJson(resp, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// Konfigurierbarer mDNS-/DHCP-Hostname (Zwei-Stick-Betrieb, FHEM 144729).
// PUT persistiert nur — mDNS + DHCP-Hostname greifen beim naechsten Boot
// (reboot_required:true), Live-Restart des mDNS-Stacks ist bewusst nicht
// implementiert (ungetestet gegen laufende AsyncWebServer-Verbindungen).
// -----------------------------------------------------------------------------
void handleGetHostname(AsyncWebServerRequest* req) {
    // hostname = persistierter NVS-Wert (nach PUT ggf. noch nicht aktiv),
    // active = was seit Boot laeuft. Ohne die Trennung springt das UI-Feld
    // nach einem Save auf den Boot-Wert zurueck (Review-Fund v0.8.35).
    JsonDocument doc;
    String persisted = sixback::persistedHostname();
    doc["hostname"]       = persisted;
    doc["active"]         = sixback::hostname();
    doc["pending_reboot"] = (persisted != sixback::hostname());
    doc["default"]        = MDNS_HOSTNAME;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handlePutHostname(AsyncWebServerRequest* req, JsonDocument& body) {
    if (!body["hostname"].is<const char*>()) {
        req->send(400, "application/json", "{\"error\":\"hostname string required\"}");
        return;
    }
    String h = body["hostname"].as<String>();
    JsonDocument resp;
    if (h.length()) {                      // Leerstring = Reset auf Default
        String err;
        if (!sixback::isValidHostname(h, err)) {
            resp["error"] = err;
            String b; serializeJson(resp, b);
            req->send(400, "application/json", b);
            return;
        }
    }
    bool saved = sixback::saveHostname(h);
    resp["ok"] = saved;
    resp["hostname"] = h.length() ? h : String(MDNS_HOSTNAME);
    resp["reboot_required"] = true;
    if (!saved) resp["error"] = "nvs write failed — setting not persisted";
    String b; serializeJson(resp, b);
    req->send(saved ? 200 : 500, "application/json", b);
}

// -----------------------------------------------------------------------------
// Issue #31 — optionaler Web-UI-Zugriffsschutz (Digest, default AUS).
// Gated wird ueber die Middleware am uiServer; diese Endpoints verwalten nur
// die Konfiguration. GET gibt NIE den Passwort-Hash heraus.
// -----------------------------------------------------------------------------
void handleGetUiAuth(AsyncWebServerRequest* req) {
    auto c = sixback::uiAuthLoad();
    JsonDocument doc;
    doc["enabled"]    = c.enabled;
    doc["username"]   = c.username;
    doc["configured"] = (c.username.length() > 0 && c.ha1.length() > 0);
    doc["notice"]     = "Digest auth for the web UI only (:80). On plain-HTTP LAN the "
                        "password is never sent in cleartext, but page content and the "
                        "speaker's own :8090 API stay open. Deterrence, not isolation.";
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handlePutUiAuth(AsyncWebServerRequest* req, JsonDocument& body) {
    JsonDocument resp;
    // Variante A: {username,password} → Credentials setzen (impliziert enable).
    if (body["password"].is<const char*>()) {
        String user = body["username"] | "";
        String pass = body["password"] | "";
        user.trim();
        if (!sixback::uiAuthSetCredentials(user, pass)) {
            resp["ok"]    = false;
            resp["error"] = "username required and password must be at least 4 characters";
            String b; serializeJson(resp, b);
            req->send(400, "application/json", b);
            return;
        }
    // Variante B: {enabled:bool} → nur an/aus ohne Credential-Aenderung.
    } else if (body["enabled"].is<bool>()) {
        if (!sixback::uiAuthSetEnabled(body["enabled"].as<bool>())) {
            resp["ok"]    = false;
            resp["error"] = "set a username and password before enabling protection";
            String b; serializeJson(resp, b);
            req->send(400, "application/json", b);
            return;
        }
    } else {
        resp["ok"]    = false;
        resp["error"] = "provide {username,password} to set credentials, or {enabled:bool} to toggle";
        String b; serializeJson(resp, b);
        req->send(400, "application/json", b);
        return;
    }
    auto c = sixback::uiAuthLoad();
    resp["ok"]         = true;
    resp["enabled"]    = c.enabled;
    resp["username"]   = c.username;
    resp["configured"] = (c.username.length() > 0 && c.ha1.length() > 0);
    String b; serializeJson(resp, b);
    req->send(200, "application/json", b);
}

void handlePostUiAuthReset(AsyncWebServerRequest* req) {
    sixback::uiAuthReset();
    JsonDocument resp;
    resp["ok"]      = true;
    resp["enabled"] = false;
    String b; serializeJson(resp, b);
    req->send(200, "application/json", b);
}

// -----------------------------------------------------------------------------
// P2 — Event-Capture / Now-Playing-UI.
//   GET  /api/events                       — all devices, latest now-playing per
//   GET  /api/speaker/{deviceId}/now-playing  — single device
//   GET  /api/speaker/{deviceId}/events       — last 20 event-types per device
//   DELETE /api/events                     — clear all
// -----------------------------------------------------------------------------
void handleEventsAll(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    sixback::eventStoreAllDevicesJson(arr);
    doc["count"] = arr.size();
    sixback::EventStoreStats st = sixback::eventStoreStats();
    JsonObject stats = doc["stats"].to<JsonObject>();
    stats["chunks_seen"]      = st.chunks_seen;
    stats["bodies_completed"] = st.bodies_completed;
    stats["parse_errors"]     = st.parse_errors;
    stats["events_ingested"]  = st.events_ingested;
    stats["last_chunk_ms"]    = st.last_chunk_ms;
    stats["last_body_ms"]     = st.last_body_ms;
    stats["last_chunk_age_ms"] = st.last_chunk_ms ? (long)(millis() - st.last_chunk_ms) : -1L;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleNowPlaying(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    JsonDocument doc;
    JsonObject now = doc["now"].to<JsonObject>();
    sixback::eventStoreNowPlayingJson(id, now);
    doc["device_id"] = id;
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleSpeakerEvents(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    JsonDocument doc;
    JsonArray arr = doc["events"].to<JsonArray>();
    sixback::eventStoreEventsJson(id, arr);
    doc["device_id"] = id;
    doc["count"]     = arr.size();
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleEventsClear(AsyncWebServerRequest* req) {
    sixback::eventStoreClearAll();
    req->send(200, "application/json", "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// GET /api/unknown-requests — Catch-All-Logger Ringbuffer-Inhalt (P3).
// DELETE /api/unknown-requests — leert den Ringbuffer.
// -----------------------------------------------------------------------------
void handleUnknownRequestsGet(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["requests"].to<JsonArray>();
    getUnknownRequestsJson(arr);
    doc["count"]    = arr.size();
    doc["capacity"] = 50;
    doc["uptime_ms"] = millis();
    String b; serializeJson(doc, b);
    req->send(200, "application/json", b);
}

void handleUnknownRequestsClear(AsyncWebServerRequest* req) {
    clearUnknownRequests();
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/speaker/{id}/diagnostic-snapshot
//   ?source=stored  → liefert den persistierten Pre-Migrate-Snapshot (404 wenn keiner da)
//   ?source=live    → erzeugt einen frischen Live-Snapshot vom Speaker (default)
//   ?save=1         → bei live: zusaetzlich als manueller Snapshot speichern (force)
void handleDiagnosticSnapshot(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String src = req->hasParam("source") ? req->getParam("source")->value() : String("live");

    if (src == "stored") {
        String stored;
        if (!sixback::loadStoredSnapshot(id, stored)) {
            req->send(404, "application/json",
                      "{\"error\":\"no stored snapshot for this device\"}");
            return;
        }
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", stored);
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"sixback-snapshot-" + id + "-stored.json\"");
        req->send(resp);
        return;
    }

    JsonDocument doc;
    if (!sixback::captureLiveSnapshot(id, doc)) {
        req->send(502, "application/json",
                  "{\"error\":\"speaker unreachable or unknown deviceId\"}");
        return;
    }
    doc["snapshot_kind"] = "live";

    if (req->hasParam("save") && req->getParam("save")->value() == "1") {
        sixback::persistPreMigrateSnapshot(id, /*force=*/true);
    }

    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* resp = req->beginResponse(200, "application/json", body);
    resp->addHeader("Content-Disposition",
                    "attachment; filename=\"sixback-snapshot-" + id + "-live.json\"");
    req->send(resp);
}

void handleDiagnosticSnapshotMeta(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    JsonDocument doc;
    doc["device_id"] = id;
    doc["has_stored"] = sixback::hasStoredSnapshot(id);
    String body;
    serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// -----------------------------------------------------------------------------
// Spotify slot-mapping API (S3-only, siehe SIXBACK_SPOTIFY_ENABLED in
// platformio.ini)
// -----------------------------------------------------------------------------
#ifdef SIXBACK_SPOTIFY_ENABLED

// GET /api/spotify/slots  — alle aktuellen Mappings
void handleSpotifySlotsList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (const auto& m : sixback::spotify::listSlots()) {
        JsonObject o = arr.add<JsonObject>();
        o["device_id"]    = m.deviceId;
        o["slot"]         = m.slot;
        o["spotify_uri"]  = m.spotifyUri;
        o["display_name"] = m.displayName;
        o["shuffle"]      = m.shuffle;
        o["repeat"]       = m.repeatMode;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// PUT /api/spotify/slot/{deviceId}/{slot}
// Body: {"uri":"spotify:...","name":"...","shuffle":true|false,"repeat":"off|track|context"}
// Empty uri -> Mapping loeschen.
void handleSpotifySlotPut(AsyncWebServerRequest* req, JsonDocument& body) {
    String deviceId = pathParam(0);
    int slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    String uri  = (const char*)(body["uri"]  | "");
    String name = (const char*)(body["name"] | "");
    if (uri.length() > 0 && !uri.startsWith("spotify:")) {
        req->send(400, "application/json",
                  "{\"error\":\"uri must start with spotify:\"}");
        return;
    }
    bool shuffle  = body["shuffle"] | false;
    String repeat = (const char*)(body["repeat"] | "off");
    sixback::spotify::setSlot(deviceId, slot, uri, name, shuffle, repeat);
    req->send(200, "application/json", "{\"ok\":true}");
}

// PUT /api/spotify/auth  Body: {clientId, clientSecret, refreshToken, [accessToken], [expiresInSec]}
// Phase-A/B-stop-gap: User holt manuell einen Token via curl-Setup und stopft
// ihn hier rein. Phase-B-OAuth-Flow wird das ersetzen.
void handleSpotifyAuthPut(AsyncWebServerRequest* req, JsonDocument& body) {
    sixback::spotify::AuthState s;
    s.clientId     = (const char*)(body["clientId"]     | "");
    s.clientSecret = (const char*)(body["clientSecret"] | "");
    s.refreshToken = (const char*)(body["refreshToken"] | "");
    s.accessToken  = (const char*)(body["accessToken"]  | "");
    int expiresIn  = body["expiresInSec"] | 3600;
    s.expiresAtMs  = (uint64_t)millis() + (uint64_t)expiresIn * 1000;
    s.spotifyUserId      = (const char*)(body["spotifyUserId"]      | "");
    s.spotifyDisplayName = (const char*)(body["spotifyDisplayName"] | "");
    if (s.clientId.length() == 0 || s.clientSecret.length() == 0
        || s.refreshToken.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"clientId+clientSecret+refreshToken required\"}");
        return;
    }
    sixback::spotify::setAuth(s);
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/spotify/auth — Status ohne Secrets.
void handleSpotifyAuthGet(AsyncWebServerRequest* req) {
    auto s = sixback::spotify::getAuth();
    JsonDocument doc;
    doc["configured"]      = sixback::spotify::isAuthConfigured();
    doc["client_id"]       = s.clientId;          // public part nur
    doc["user_id"]         = s.spotifyUserId;
    doc["display_name"]    = s.spotifyDisplayName;
    int64_t rem = (int64_t)s.expiresAtMs - (int64_t)millis();
    doc["expires_in_sec"]  = rem / 1000;
    doc["has_access_token"]= s.accessToken.length() > 0;
    doc["has_refresh_token"]= s.refreshToken.length() > 0;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// GET /api/spotify/auth/url?client_id=X&redirect_uri=Y
// Liefert die Spotify-OAuth-URL fuer den Browser-Auth-Flow.
// redirect_uri default: http://127.0.0.1:8888/callback (User kopiert URL aus
// 404-Page).
void handleSpotifyAuthUrl(AsyncWebServerRequest* req) {
    // Fallback-Kette für clientId:
    //   1. ?client_id= Param
    //   2. stored AppCreds (NVS)
    String clientId = req->hasParam("client_id")
        ? req->getParam("client_id")->value()
        : sixback::spotify::getAppCreds().clientId;
    // Default redirect_uri: öffentliche sixback.io/proxy/ Bridge (HTTPS,
    // Spotify-konform). Bridge liest ?state= und navigiert zurück ins LAN.
    // ?redirect_uri= override für Test/Custom-Setup.
    String redirectUri = req->hasParam("redirect_uri")
        ? req->getParam("redirect_uri")->value()
        : "https://sixback.io/proxy/";
    // state = lokaler SixBack-Callback (wo Bridge den User zurückleitet).
    String state = "http://" + req->host() + "/spotify-callback";
    if (clientId.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"client_id required (no stored AppCreds)\"}");
        return;
    }
    String authUrl = sixback::spotify::genAuthUrl(clientId, redirectUri, state);
    JsonDocument doc;
    doc["auth_url"]     = authUrl;
    doc["redirect_uri"] = redirectUri;
    doc["state"]        = state;
    doc["client_id"]    = clientId;
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// POST /api/spotify/auth/exchange
//   Body: {code, clientId, clientSecret, redirectUri}
// Tauscht Code gegen Tokens + fetcht /v1/me + persistiert via setAuth().
void handleSpotifyAuthExchange(AsyncWebServerRequest* req, JsonDocument& body) {
    String code         = (const char*)(body["code"]         | "");
    String clientId     = (const char*)(body["clientId"]     | "");
    String clientSecret = (const char*)(body["clientSecret"] | "");
    String redirectUri  = (const char*)(body["redirectUri"]  | "");
    // Fallback auf stored AppCreds wenn nicht im Body übergeben.
    if (clientId.length() == 0 || clientSecret.length() == 0) {
        auto stored = sixback::spotify::getAppCreds();
        if (clientId.length()     == 0) clientId     = stored.clientId;
        if (clientSecret.length() == 0) clientSecret = stored.clientSecret;
    }
    // Default redirect_uri: muss EXAKT matchen was in /authorize gesendet
    // wurde — also sixback.io/proxy/ aus dem default-Flow.
    if (redirectUri.length() == 0) {
        redirectUri = "https://sixback.io/proxy/";
    }
    if (code.length() == 0 || clientId.length() == 0
        || clientSecret.length() == 0 || redirectUri.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"code required (clientId+clientSecret from body or stored AppCreds)\"}");
        return;
    }
    String err;
    if (!sixback::spotify::exchangeAuthCode(code, clientId, clientSecret,
                                            redirectUri, err)) {
        JsonDocument out;
        out["ok"]    = false;
        out["error"] = err;
        String s; serializeJson(out, s);
        req->send(400, "application/json", s);
        return;
    }
    auto st = sixback::spotify::getAuth();
    JsonDocument out;
    out["ok"]              = true;
    out["user_id"]         = st.spotifyUserId;
    out["display_name"]    = st.spotifyDisplayName;
    out["expires_in_sec"]  = ((int64_t)st.expiresAtMs - (int64_t)millis()) / 1000;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// GET /api/spotify/last-trigger — letzte 8 Trigger-Versuche fuer Debug.
void handleSpotifyLastTrigger(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["triggers"].to<JsonArray>();
    uint32_t now = millis();
    for (auto& e : sixback::spotify::listTriggerLog()) {
        JsonObject o = arr.add<JsonObject>();
        o["ts_ms"]            = e.ts_ms;
        o["age_ms"]           = (int32_t)(now - e.ts_ms);
        o["device_id"]        = e.deviceId;
        o["slot"]             = e.slot;
        o["uri"]              = e.uri;
        o["speaker_name"]     = e.speakerName;
        o["spotify_device_id"]= e.spotifyDeviceId;
        o["play_http_code"]   = e.playHttpCode;
        o["result"]           = e.result;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// DELETE /api/spotify/auth — clear auth (Disconnect Spotify).
// App-Creds bleiben erhalten (siehe handleSpotifyAppCredsDelete für vollen Wipe).
void handleSpotifyAuthDelete(AsyncWebServerRequest* req) {
    sixback::spotify::AuthState empty;
    sixback::spotify::setAuth(empty);
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/spotify/debug/probe-api?url=/v1/me
// Macht GET-Request an api.spotify.com mit FRISCH allokiertem WiFiClientSecure
// (kein Singleton-Reuse). Returnt HTTP-Code + body. Für Diagnose bei HTTP -1.
void handleSpotifyDebugProbe(AsyncWebServerRequest* req) {
    String path = req->hasParam("url") ? req->getParam("url")->value() : "/v1/me";
    if (!path.startsWith("/")) path = "/" + path;
    String tok = sixback::spotify::getValidAccessToken();
    if (tok.length() == 0) {
        req->send(401, "application/json", "{\"error\":\"no-token\"}");
        return;
    }
    uint32_t t0 = millis();
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(8);
    HTTPClient http;
    String url = "https://api.spotify.com" + path;
    if (!http.begin(tls, url)) {
        req->send(500, "application/json", "{\"error\":\"http.begin failed\"}");
        return;
    }
    http.addHeader("Authorization", "Bearer " + tok);
    http.setTimeout(8000);
    int code = http.GET();
    String body = http.getString();
    http.end();
    uint32_t dt = millis() - t0;
    JsonDocument doc;
    doc["url"]       = url;
    doc["http_code"] = code;
    doc["body"]      = body.substring(0, 1000);
    doc["dt_ms"]     = dt;
    doc["body_len"]  = body.length();
    String s; serializeJson(doc, s);
    req->send(200, "application/json", s);
}

// PUT /api/spotify/app-creds  Body: {clientId, clientSecret}
// Persistiert App-Creds separat von Tokens. Disconnect löscht sie NICHT.
void handleSpotifyAppCredsPut(AsyncWebServerRequest* req, JsonDocument& body) {
    sixback::spotify::AppCreds c;
    c.clientId     = (const char*)(body["clientId"]     | "");
    c.clientSecret = (const char*)(body["clientSecret"] | "");
    if (c.clientId.length() == 0 || c.clientSecret.length() == 0) {
        req->send(400, "application/json",
                  "{\"error\":\"clientId+clientSecret required\"}");
        return;
    }
    sixback::spotify::setAppCreds(c);
    req->send(200, "application/json", "{\"ok\":true}");
}

// GET /api/spotify/app-creds — Status ohne Secret.
void handleSpotifyAppCredsGet(AsyncWebServerRequest* req) {
    auto c = sixback::spotify::getAppCreds();
    JsonDocument doc;
    doc["client_id"]  = c.clientId;
    doc["has_secret"] = c.clientSecret.length() > 0;
    doc["configured"] = sixback::spotify::hasAppCreds();
    String s; serializeJson(doc, s);
    req->send(200, "application/json", s);
}

// DELETE /api/spotify/app-creds — voller Wipe (App-Creds + Auth).
void handleSpotifyAppCredsDelete(AsyncWebServerRequest* req) {
    sixback::spotify::AppCreds empty;
    sixback::spotify::setAppCreds(empty);
    sixback::spotify::AuthState emptyAuth;
    sixback::spotify::setAuth(emptyAuth);
    req->send(200, "application/json", "{\"ok\":true}");
}


// DELETE /api/spotify/slot/{deviceId}/{slot}
void handleSpotifySlotDelete(AsyncWebServerRequest* req) {
    String deviceId = pathParam(0);
    int slot = pathParam(1).toInt();
    if (slot < 1 || slot > 6) {
        req->send(400, "application/json", "{\"error\":\"slot 1..6\"}");
        return;
    }
    sixback::spotify::setSlot(deviceId, slot, "", "");
    req->send(200, "application/json", "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// Library-Endpoints (Sidebar-Tile-Grid, D&D-Source)
// -----------------------------------------------------------------------------

// GET /api/spotify/library — alle gespeicherten Library-Items
void handleSpotifyLibraryList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    for (const auto& it : sixback::spotify::listLibrary()) {
        JsonObject o = arr.add<JsonObject>();
        o["uri"]        = it.uri;
        o["name"]       = it.name;
        o["image_url"]  = it.imageUrl;
        o["shuffle"]    = it.shuffle;
        o["repeat"]     = it.repeatMode;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// POST /api/spotify/library
// Body: {"uri":"spotify:...","name":"...","image_url":"https://...",
//        "shuffle":true|false,"repeat":"off|track|context"}
// Upsert per uri. Returns {ok:true, created:bool}.
void handleSpotifyLibraryAdd(AsyncWebServerRequest* req, JsonDocument& body) {
    sixback::spotify::LibraryItem it;
    it.uri        = (const char*)(body["uri"]        | "");
    it.name       = (const char*)(body["name"]       | "");
    it.imageUrl   = (const char*)(body["image_url"]  | "");
    it.shuffle    = body["shuffle"]                   | false;
    it.repeatMode = (const char*)(body["repeat"]     | "off");
    if (it.uri.length() == 0 || !it.uri.startsWith("spotify:")) {
        req->send(400, "application/json",
                  "{\"error\":\"uri must start with spotify:\"}");
        return;
    }
    if (it.name.length() == 0) it.name = it.uri;
    bool created = sixback::spotify::addLibraryItem(it);
    JsonDocument out;
    out["ok"]      = true;
    out["created"] = created;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// DELETE /api/spotify/library?uri=spotify:album:XYZ
// Per Query-Param statt Path-Component weil `spotify:` ein Colon enthaelt
// und URL-encoding in Path-Args mit ASYNCWEBSERVER_REGEX fragwuerdig waere.
void handleSpotifyLibraryDelete(AsyncWebServerRequest* req) {
    if (!req->hasParam("uri")) {
        req->send(400, "application/json", "{\"error\":\"uri query-param required\"}");
        return;
    }
    String uri = req->getParam("uri")->value();
    bool removed = sixback::spotify::removeLibraryItem(uri);
    JsonDocument out;
    out["ok"]      = true;
    out["removed"] = removed;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

#endif // SIXBACK_SPOTIFY_ENABLED

// -----------------------------------------------------------------------------
// Stream-Library — device-side storage for custom radio-stream tiles (the
// "Stream" sidebar tab). Mirrors the Spotify-Library API but is NOT gated on
// Spotify — every build has it. stream_url is the primary key. Drag-onto-slot
// behaviour is unchanged (frontend still creates a TUNEIN-tunnel preset).
// -----------------------------------------------------------------------------
// GET /api/streams — all stored stream tiles
void handleStreamsList(AsyncWebServerRequest* req) {
    JsonDocument doc;
    JsonArray arr = doc["items"].to<JsonArray>();
    for (const auto& it : sixback::streams::listStreams()) {
        JsonObject o = arr.add<JsonObject>();
        o["name"]         = it.name;
        o["stream_url"]   = it.streamUrl;
        o["image_url"]    = it.imageUrl;
        o["icy_name"]     = it.icyName;
        o["content_type"] = it.contentType;
        o["bitrate"]      = it.bitrate;
    }
    String body; serializeJson(doc, body);
    req->send(200, "application/json", body);
}

// POST /api/streams
// Body: {name, stream_url, image_url, icy_name?, content_type?, bitrate?}
// Upsert per stream_url. Returns {ok:true, created:bool}.
void handleStreamsAdd(AsyncWebServerRequest* req, JsonDocument& body) {
    sixback::streams::StreamItem it;
    it.name        = (const char*)(body["name"]         | "");
    it.streamUrl   = (const char*)(body["stream_url"]   | "");
    it.imageUrl    = (const char*)(body["image_url"]    | "");
    it.icyName     = (const char*)(body["icy_name"]     | "");
    it.contentType = (const char*)(body["content_type"] | "");
    it.bitrate     = (const char*)(body["bitrate"]      | "");
    if (it.streamUrl.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"stream_url required\"}");
        return;
    }
    if (it.name.length() == 0) it.name = it.streamUrl;
    bool created = sixback::streams::addStreamItem(it);
    JsonDocument out;
    out["ok"]      = true;
    out["created"] = created;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// DELETE /api/streams?url=<stream_url>
// Query-param (not path) because a stream URL contains '/' and ':'.
void handleStreamsDelete(AsyncWebServerRequest* req) {
    if (!req->hasParam("url")) {
        req->send(400, "application/json", "{\"error\":\"url query-param required\"}");
        return;
    }
    bool removed = sixback::streams::removeStreamItem(req->getParam("url")->value());
    JsonDocument out;
    out["ok"]      = true;
    out["removed"] = removed;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

// POST /api/streams/import  Body: {items:[{name, stream_url, ...}, ...]}
// Bulk merge — upserts each item by stream_url. Returns {ok:true, imported:N}.
void handleStreamsImport(AsyncWebServerRequest* req, JsonDocument& body) {
    int imported = 0;
    for (JsonObject o : body["items"].as<JsonArray>()) {
        sixback::streams::StreamItem it;
        it.name        = (const char*)(o["name"]         | "");
        it.streamUrl   = (const char*)(o["stream_url"]   | "");
        it.imageUrl    = (const char*)(o["image_url"]    | "");
        it.icyName     = (const char*)(o["icy_name"]     | "");
        it.contentType = (const char*)(o["content_type"] | "");
        it.bitrate     = (const char*)(o["bitrate"]      | "");
        if (it.streamUrl.length() == 0) continue;
        if (it.name.length() == 0) it.name = it.streamUrl;
        sixback::streams::addStreamItem(it);
        imported++;
    }
    JsonDocument out;
    out["ok"]       = true;
    out["imported"] = imported;
    String s; serializeJson(out, s);
    req->send(200, "application/json", s);
}

#if SIXBACK_CONTIG_PROBE
// GET /api/dbg/contig[?floor=N][&reset=1] — Lab-Sonde (nur *-probe-Envs).
//   floor=N  setzt outboundBlockFloor() zur Laufzeit (Floor-Sweep)
//   reset=1  setzt die Aggregate zurueck (vor jedem Messlauf)
void handleDbgContig(AsyncWebServerRequest* req) {
    if (req->hasParam("floor")) {
        uint32_t v = (uint32_t)req->getParam("floor")->value().toInt();
        g_probeFloor.store(v, std::memory_order_relaxed);
    }
    if (req->hasParam("ballast")) {
        // ballast=N -> groessten freien INTERNAL-Block auf ~N Bytes druecken,
        // ballast=0 -> alles wieder freigeben.
        ballastTo((uint32_t)req->getParam("ballast")->value().toInt());
    }
    if (req->hasParam("reset")) {
        g_probeRuns.store(0);        g_probeFails.store(0);
        g_probeLbWorst.store(0xFFFFFFFFu);
        g_probeLbDropMax.store(0);   g_probeFreeDropMax.store(0);
        g_probeBodyMax.store(0);
        g_outboundRejects.store(0);  g_rejFloorTotal.store(0);
        g_rejFloorBlock.store(0);    g_rejInflight.store(0);
    }
    JsonDocument d;
    d["floor"]          = g_probeFloor.load();
    d["runs"]           = g_probeRuns.load();
    d["fails"]          = g_probeFails.load();
    uint32_t w = g_probeLbWorst.load();
    d["lb_worst"]       = (w == 0xFFFFFFFFu) ? 0 : w;   // kleinster largest_block im Handler
    d["lb_drop_max"]    = g_probeLbDropMax.load();      // groesster contiguous-Entzug
    d["free_drop_max"]  = g_probeFreeDropMax.load();
    d["body_max"]       = g_probeBodyMax.load();
    d["lb_now"]         = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    d["free_now"]       = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    JsonObject rej      = d["rejects"].to<JsonObject>();
    rej["total"]        = g_outboundRejects.load();
    rej["floor_total"]  = g_rejFloorTotal.load();
    rej["floor_block"]  = g_rejFloorBlock.load();
    rej["inflight"]     = g_rejInflight.load();
    String s; serializeJson(d, s);
    req->send(200, "application/json", s);
}
#endif

void registerApiEndpoints(AsyncWebServer& ui) {
    // Statische Assets aus LittleFS (CSS, JS, etc.)
    ui.serveStatic("/assets/", LittleFS, "/assets/").setCacheControl("max-age=600");

    ui.on("/",                        HTTP_GET,    handleRoot);
    ui.on("/api/status",              HTTP_GET,    handleStatus);
#if SIXBACK_CONTIG_PROBE
    ui.on("/api/dbg/contig",          HTTP_GET,    handleDbgContig);
#endif
    ui.on("/api/speakers",            HTTP_GET,    handleSpeakersList);
    ui.on("/api/speakers/discover",   HTTP_POST,   handleDiscover);
    routeJsonBody(ui, "/api/speakers/add", HTTP_POST, handleSpeakerAdd);
    routeJsonBody(ui, "/api/speakers/order", HTTP_POST, handleSpeakersOrder);
    routeJsonBody(ui, "/api/speakers/hide", HTTP_POST, handleSpeakerHide);
    routeT(ui, "^/api/speakers/([^/]+)$",  HTTP_DELETE, handleSpeakerDelete);

    // Stream-Library (custom radio-stream tiles) — device-side, not Spotify-gated.
    routeT(ui, "^/api/streams$",         HTTP_GET,    handleStreamsList);
    routeJsonBody(ui, "^/api/streams$",  HTTP_POST,   handleStreamsAdd);
    routeT(ui, "^/api/streams$",         HTTP_DELETE, handleStreamsDelete);
    routeJsonBody(ui, "^/api/streams/import$", HTTP_POST, handleStreamsImport);
    routeT(ui, "^/api/speaker/([^/]+)/migrate$",        HTTP_POST, handleMigrate);
    routeT(ui, "^/api/speaker/([^/]+)/reboot$",         HTTP_POST, handleReboot);
    ui.on("/api/speakers/refresh-status",          HTTP_POST, handleRefreshStatus);

    routeT(ui, "^/api/speaker/([^/]+)/presets$",        HTTP_GET,    handleGetPresets);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_PUT, handlePutPreset);
    routeT(ui, "^/api/speaker/([^/]+)/preset/([1-6])$", HTTP_DELETE, handleDeletePreset);
    routeT(ui, "^/api/speaker/([^/]+)/preset/([1-6])/export$", HTTP_GET, handleExportPreset);
    routeT(ui, "^/api/speaker/([^/]+)/preset/([1-6])/push-to-device$",
          HTTP_POST, handlePushPresetToDevice);
    routeT(ui, "^/api/speaker/([^/]+)/preset/([1-6])/revert$",
          HTTP_POST, handleRevertPresetToHw);
    routeT(ui, "^/api/speaker/([^/]+)/presets/import-from-device$",
          HTTP_POST, handleImportFromDevice);
    routeT(ui, "^/api/speaker/([^/]+)/presets/export-set$",
          HTTP_GET, handleExportPresetsSet);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/presets/import-set$",
                  HTTP_POST, handleImportPresetsSet);
    routeJsonBody(ui, "/api/preset/swap", HTTP_POST, handleSwapPresets);
    routeT(ui, "^/api/speaker/([^/]+)/push-all$", HTTP_POST, handleBulkPushPresets);
    routeT(ui, "^/api/speaker/([^/]+)/key/([A-Z_0-9]+)$", HTTP_POST, handleSpeakerKey);
    routeT(ui, "^/api/speaker/([^/]+)/play-preset/([1-6])$", HTTP_POST, handlePlayPreset);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/play-source$", HTTP_POST, handlePlaySource);
    routeT(ui, "^/api/speaker/([^/]+)/hardware-presets$", HTTP_GET, handleGetHardwarePresets);
    routeT(ui, "^/api/speaker/([^/]+)/now-playing-live$", HTTP_GET, handleNowPlayingLive);

    // Clock-Display-Toggle (ST20/ST30) — device-direct GET/POST /clockDisplay
    routeT(ui, "^/api/speaker/([^/]+)/clock-display$", HTTP_GET, handleClockDisplayGet);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/clock-display$", HTTP_POST, handleClockDisplaySet);

    // ZoneManager — device-direct Multiroom (BMX :8090). Eigene Schicht,
    // getrennt vom cloud-Group-Store (handlePutGroup/handleSyncGroup unten).
    routeT(ui, "^/api/speaker/([^/]+)/zone$", HTTP_GET,  handleGetZone);
    routeJsonBody(ui, "^/api/zone/create$",   HTTP_POST, handleZoneCreate);
    routeJsonBody(ui, "^/api/zone/dissolve$", HTTP_POST, handleZoneDissolve);
    routeJsonBody(ui, "^/api/zone/member$",   HTTP_POST, handleZoneMember);

    // Stereo-Pair (ST10, #22) — device-direct addGroup/removeGroup
    routeT(ui, "^/api/speaker/([^/]+)/stereo-pair$", HTTP_GET,    handleStereoPairGet);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/stereo-pair$", HTTP_POST, handleStereoPairCreate);
    routeT(ui, "^/api/speaker/([^/]+)/stereo-pair$", HTTP_DELETE, handleStereoPairRemove);

    // Speaker umbenennen (#25) — device-direct POST /name
    routeJsonBody(ui, "^/api/speaker/([^/]+)/name$", HTTP_POST, handleSpeakerRename);

    routeT(ui, "^/api/speaker/([^/]+)/dlna/servers$", HTTP_GET, handleDlnaServers);
    routeT(ui, "/api/dlna/describe", HTTP_GET, handleDlnaDescribe);
    routeT(ui, "/api/dlna/browse",   HTTP_GET, handleDlnaBrowse);
    routeJsonBody(ui, "^/api/speaker/([^/]+)/dlna/preset/([1-6])$",
                  HTTP_POST, handleDlnaRecordPreset);

    routeT(ui, "^/api/speaker/([^/]+)/diagnostic-snapshot$",
          HTTP_GET, handleDiagnosticSnapshot);
    routeT(ui, "^/api/speaker/([^/]+)/diagnostic-snapshot/meta$",
          HTTP_GET, handleDiagnosticSnapshotMeta);

    // Issue #3: Pre-Migration Restore — preview (diff) + commit.
    routeT(ui, "^/api/speaker/([^/]+)/restore-pre-migration/preview$",
          HTTP_GET,  handleRestorePreMigrationPreview);
    routeT(ui, "^/api/speaker/([^/]+)/restore-pre-migration$",
          HTTP_POST, handleRestorePreMigration);

    routeJsonBody(ui, "^/api/speaker/([^/]+)/group$", HTTP_PUT, handlePutGroup);
    routeJsonBody(ui, "/api/group/sync",              HTTP_POST, handleSyncGroup);

    routeT(ui, "^/api/tunein/resolve/([^/]+)$", HTTP_GET, handleTuneInResolve);
    routeT(ui, "^/api/tunein/cache/clear$",     HTTP_POST, handleTuneInCacheClear);
    ui.on("/api/tunein/search",            HTTP_GET, handleTuneInSearch);

    ui.on("/api/auto-mode",          HTTP_GET,  handleGetAutoMode);
    routeJsonBody(ui, "/api/auto-mode", HTTP_PUT, handlePutAutoMode);

    ui.on("/api/diag-share",         HTTP_GET,  handleGetDiagShare);
    routeJsonBody(ui, "/api/diag-share", HTTP_PUT, handlePutDiagShare);

    ui.on("/api/hostname",           HTTP_GET,  handleGetHostname);
    routeJsonBody(ui, "/api/hostname", HTTP_PUT, handlePutHostname);

    ui.on("/api/ui-auth",            HTTP_GET,  handleGetUiAuth);
    routeJsonBody(ui, "/api/ui-auth", HTTP_PUT, handlePutUiAuth);
    ui.on("/api/ui-auth/reset",      HTTP_POST, handlePostUiAuthReset);
    routeJsonBody(ui, "/api/test/force-ip-change", HTTP_POST, handleTestForceIpChange);

    ui.on("/api/factory_reset_wifi", HTTP_POST, handleFactoryResetWifi);
    ui.on("/api/reboot",             HTTP_POST, handleSelfReboot);

    ui.on("/api/unknown-requests",   HTTP_GET,    handleUnknownRequestsGet);
    ui.on("/api/unknown-requests",   HTTP_DELETE, handleUnknownRequestsClear);

    ui.on("/api/events",                                   HTTP_GET,    handleEventsAll);
    ui.on("/api/events",                                   HTTP_DELETE, handleEventsClear);
    routeT(ui, "^/api/speaker/([^/]+)/now-playing$",            HTTP_GET,    handleNowPlaying);
    routeT(ui, "^/api/speaker/([^/]+)/events$",                 HTTP_GET,    handleSpeakerEvents);

#ifdef SIXBACK_SPOTIFY_ENABLED
    ui.on("/api/spotify/slots",                            HTTP_GET,    handleSpotifySlotsList);
    routeJsonBody(ui, "^/api/spotify/slot/([^/]+)/([1-6])$", HTTP_PUT,  handleSpotifySlotPut);
    routeT(ui, "^/api/spotify/slot/([^/]+)/([1-6])$",           HTTP_DELETE, handleSpotifySlotDelete);
    // ASYNCWEBSERVER_REGEX=1 — Path-strings ohne ^$ matchen als Prefix.
    // /api/spotify/auth wuerde sonst /api/spotify/auth/url + /exchange
    // mitschlucken. Explizit anchored:
    routeT(ui, "^/api/spotify/auth$",                           HTTP_GET,    handleSpotifyAuthGet);
    routeJsonBody(ui, "^/api/spotify/auth$",               HTTP_PUT,    handleSpotifyAuthPut);
    routeT(ui, "^/api/spotify/auth$",                           HTTP_DELETE, handleSpotifyAuthDelete);
    routeT(ui, "^/api/spotify/auth/url$",                       HTTP_GET,    handleSpotifyAuthUrl);
    routeJsonBody(ui, "^/api/spotify/auth/exchange$",      HTTP_POST,   handleSpotifyAuthExchange);
    routeT(ui, "^/api/spotify/last-trigger$",                   HTTP_GET,    handleSpotifyLastTrigger);
    routeT(ui, "^/api/spotify/debug/probe-api$",                HTTP_GET,    handleSpotifyDebugProbe);
    // Calls lookupSpotifyDeviceId INLINE from handler thread (no xTaskCreate).
    routeT(ui, "^/api/spotify/debug/lookup-inline$", HTTP_GET, [](AsyncWebServerRequest* r) {
        String name = r->hasParam("name") ? r->getParam("name")->value() : "Emma";
        uint32_t t0 = millis();
        String devId = sixback::spotify::lookupSpotifyDeviceId(name);
        uint32_t dt = millis() - t0;
        JsonDocument doc;
        doc["name"] = name;
        doc["device_id"] = devId;
        doc["dt_ms"] = dt;
        String s; serializeJson(doc, s);
        r->send(200, "application/json", s);
    });
    // POST /api/spotify/simulate/{devId}/{slot} — feuert preset-pressed-Event
    // wie ein physischer Tastendruck. Trigger Spotify-/play OHNE Bose-Source-
    // Switch. Nützlich wenn Bose-Slot leer aber Spotify gemappt ist.
    routeT(ui, "^/api/spotify/simulate/([^/]+)/([1-6])$", HTTP_POST,
          [](AsyncWebServerRequest* r) {
        String devId = pathParam(0);
        int slot = pathParam(1).toInt();
        sixback::spotify::onPresetPressed(devId, slot, "ui-simulate");
        r->send(200, "application/json", "{\"ok\":true,\"simulated\":true}");
    });
    routeT(ui, "^/api/spotify/app-creds$",                      HTTP_GET,    handleSpotifyAppCredsGet);
    routeJsonBody(ui, "^/api/spotify/app-creds$",          HTTP_PUT,    handleSpotifyAppCredsPut);
    routeT(ui, "^/api/spotify/app-creds$",                      HTTP_DELETE, handleSpotifyAppCredsDelete);
    // Library — D&D-Source-Tile-Storage (Sidebar)
    routeT(ui, "^/api/spotify/library$",                        HTTP_GET,    handleSpotifyLibraryList);
    routeJsonBody(ui, "^/api/spotify/library$",            HTTP_POST,   handleSpotifyLibraryAdd);
    routeT(ui, "^/api/spotify/library$",                        HTTP_DELETE, handleSpotifyLibraryDelete);
    // OAuth-Callback-Page: fängt ?code aus Spotify-Redirect ab + macht
    // Exchange auto im Browser. Self-Redirect → kein localhost-Detour nötig.
    routeT(ui, "^/spotify-callback$", HTTP_GET, [](AsyncWebServerRequest* r) {
        static const char kCallbackHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>SixBack – Spotify Callback</title>
<style>body{font-family:system-ui;background:#1a1a1a;color:#eee;padding:2em;
text-align:center}code{background:#333;padding:.2em .4em;border-radius:3px}</style>
</head><body>
<h1>Spotify Callback</h1><div id="msg">Tausche Code gegen Tokens…</div>
<script>
const p = new URLSearchParams(window.location.search);
const code = p.get('code'), err = p.get('error');
const msg = document.getElementById('msg');
if (err) { msg.innerHTML = '<span style="color:#f88">Fehler: ' + err + '</span>'; }
else if (!code) { msg.innerHTML = '<span style="color:#f88">Kein Code in URL</span>'; }
else {
  fetch('/api/spotify/auth/exchange', {method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({code})    // redirectUri vom Server-Default (sixback.io/proxy/)
  }).then(r => r.json()).then(j => {
    if (j.ok) {
      msg.innerHTML = '<span style="color:#9d8">✓ Verbunden als <code>' +
                     (j.display_name || j.user_id || '?') + '</code></span>' +
                     '<p>Weiterleitung zur Hauptseite in 2s…</p>';
      setTimeout(() => { window.location.href = '/'; }, 2000);
    } else {
      msg.innerHTML = '<span style="color:#f88">Exchange failed: ' +
                     (j.error || 'unknown') + '</span>';
    }
  }).catch(e => {
    msg.innerHTML = '<span style="color:#f88">Network error: ' + e.message + '</span>';
  });
}
</script></body></html>)HTML";
        r->send(200, "text/html", kCallbackHtml);
    });
    routeT(ui, "^/api/nvs/stats$", HTTP_GET, [](AsyncWebServerRequest* r) {
        JsonDocument doc;
        sixback::nvsGetStatsJson(doc);
        String body; serializeJson(doc, body);
        r->send(200, "application/json", body);
    });
    routeT(ui, "^/api/nvs/cleanup$", HTTP_POST, [](AsyncWebServerRequest* r) {
        bool a = sixback::nvsEraseAllInNamespace("sixback-tune");
        bool b = sixback::nvsEraseAllInNamespace("sixback-sys");
        JsonDocument doc;
        doc["tune_erased"] = a;
        doc["sys_erased"]  = b;
        JsonDocument stats;
        sixback::nvsGetStatsJson(stats);
        doc["stats_after"] = stats;
        String body; serializeJson(doc, body);
        r->send(200, "application/json", body);
    });
#endif

#ifdef SIXBACK_OTA_ENABLED
    // OTA — multipart upload. Mit ASYNCWEBSERVER_REGEX=1 binden plain
    // Path-Strings ohne `^...$` als Prefix — /api/ota wuerde dann auch
    // /api/ota/fs schlucken. Mit explizitem Anchor matched jede Route
    // exakt ihren Pfad.
    routeT(ui, "^/api/ota$",    HTTP_POST, handleOtaFinalize,   handleOtaUpload);
    routeT(ui, "^/api/ota/fs$", HTTP_POST, handleOtaFsFinalize, handleOtaFsUpload);

    // Online-Update — HTTPS-Pull von sixback.io
    sixback::ota::init(String(FW_VERSION_STRING));
#endif
    // /api/update/* sind immer registriert — auf no-OTA-builds returnen
    // sie HTTP 410 mit Hinweis auf Web-Serial-Webflasher.
    ui.on("/api/update/check",   HTTP_GET,  handleOtaUpdateCheck);
    ui.on("/api/update/install", HTTP_POST, handleOtaUpdateInstall);
    ui.on("/api/update/status",  HTTP_GET,  handleOtaUpdateStatus);
}
