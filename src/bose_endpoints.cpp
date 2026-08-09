// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Bose Cloud Replacement Endpoints
//
// Verifizierte Pflicht-Endpoints aus den AfterTouch-Live-Logs (Pi5-Migration
// Kueche 2026-05-16). Phase 1: alle 11 Endpoints mit echten Bodies, plus
// Account-Sources und Preset-Sync zwischen Speaker und ESP-Store.

#include "bose_endpoints.h"
#include "web_router.h"
#include "tunein_resolver.h"
#include "preset_store.h"
#include "speaker_inventory.h"
#include "event_store.h"
#include "nvs_helper.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <set>
#include <WiFi.h>
#include <map>
#include <vector>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Gap-Heal-Bruecke aus api_endpoints.cpp (Wrapper am Datei-Ende dort; die
// eigentliche Import-Implementierung liegt in deren anonymem Namespace).
// Deklaration MUSS vor dem anonymen Namespace hier stehen, sonst bekommt
// sie internal linkage und der Linker findet die Definition nicht.
// Semantik: HW-Presets fuellen nur Store-Luecken, belegte Slots gewinnen.
int importPresetsFillEmptyForHeal(const String& id, int& countOk,
                                  int& countAban, int& httpCodeOut);

namespace {

void send200(AsyncWebServerRequest* r) { r->send(200, "text/plain", ""); }

// -----------------------------------------------------------------------------
// Catch-All-Logger Ringbuffer (P3) — 50 Eintraege, ueberschreibt aelteste.
// Threadsicher per Mutex (Reader/Writer aus AsyncWebServer-Callback-Tasks).
// -----------------------------------------------------------------------------
struct UnknownReq {
    uint32_t ts_ms;
    String   client_ip;
    String   method;
    String   path;   // inkl. Query-String, falls vorhanden
    String   host;
    int      content_length;
    String   content_type;
};

constexpr size_t kUnknownRingSize = 50;
UnknownReq        g_unknown[kUnknownRingSize];
size_t            g_unknown_head  = 0;
size_t            g_unknown_count = 0;
SemaphoreHandle_t g_unknown_mtx   = nullptr;

void pushUnknown(const UnknownReq& r) {
    if (!g_unknown_mtx) return;
    if (xSemaphoreTake(g_unknown_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    g_unknown[g_unknown_head] = r;
    g_unknown_head = (g_unknown_head + 1) % kUnknownRingSize;
    if (g_unknown_count < kUnknownRingSize) g_unknown_count++;
    xSemaphoreGive(g_unknown_mtx);
}

// -----------------------------------------------------------------------------
// POST /v1/scmudc/{deviceId}  -- Telemetrie/Stats-Endpoint (P2).
//   Body: {envelope, payload:{deviceInfo, events:[{type,data,time}...]}}.
//   Events: item-started, play-state-changed, art-changed, source-state-changed,
//   volume-change, preset-pressed, power-pressed, system-state-changed, etc.
//   Wir extrahieren NowPlaying + Trace pro Device (siehe event_store.h).
// -----------------------------------------------------------------------------
void handleSCMUDCFinalize(AsyncWebServerRequest* req) {
    Serial.printf("[bmx] SCMUDC POST finalize dev=%s clen=%d\n",
                  pathParam(0).c_str(), (int)req->contentLength());
    send200(req);
}

void handleSCMUDCBody(AsyncWebServerRequest* req,
                      uint8_t* data, size_t len, size_t index, size_t total) {
    String devId = pathParam(0);
    sixback::eventStoreIngestChunk(devId, data, len, index, total);
}

// -----------------------------------------------------------------------------
// GET /bmx/registry/v1/services  -- Service-Discovery
//   Liefert AfterTouch's vollstaendiges Service-Descriptor-JSON (8 KB),
//   mit Token-Replacement fuer {BMX_SERVER} und {MEDIA_SERVER}. Ohne diesen
//   Descriptor markiert der Speaker TUNEIN als UNAVAILABLE, Preset-Tasten
//   bleiben tot.
// -----------------------------------------------------------------------------
void handleBMXRegistry(AsyncWebServerRequest* req) {
    if (!LittleFS.exists("/bmx_services.json")) {
        req->send(500, "application/json", "{\"error\":\"bmx_services.json missing in LittleFS\"}");
        return;
    }
    File f = LittleFS.open("/bmx_services.json", "r");
    String body = f.readString();
    f.close();
    String base = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    body.replace("{BMX_SERVER}",   base);
    body.replace("{MEDIA_SERVER}", base + "/media");
    req->send(200, "application/json", body);
}

void handleBMXServicesAvailability(AsyncWebServerRequest* req) {
    if (!LittleFS.exists("/bmx_services_availability.json")) {
        req->send(200, "application/json", "{\"services\":[]}");
        return;
    }
    req->send(LittleFS, "/bmx_services_availability.json", "application/json");
}

void handlePowerOn(AsyncWebServerRequest* req) {
    String body;
    if (req->hasParam("plain", true)) body = req->getParam("plain", true)->value();
    Serial.printf("[marge] power_on body-bytes=%u\n", body.length());
    send200(req);
}

// -----------------------------------------------------------------------------
// GET /streaming/account/{id}/full  -- Vollst. Account-Response (P6)
//
// Form folgt julius-d/ueberboese-api streaming-account-full-6921042.xml und
// OpenAPI-Schema FullAccountResponse (Pflichtfelder: id, accountStatus,
// devices, mode, preferredLanguage, sources).
//
// Diagnose 2026-05-19: die magere alte Form (<accountID>X</accountID>,
// keine <devices>) reichte aus, dass Speaker nach Reboot keine scmudc-Events
// schickten und Greta/Emma kein /select TUNEIN mehr konnten — vermutlich
// weil der account-bound Source-Eintrag aus der Speaker-Persistenz wegfaellt
// wenn der Re-Sync ihn nicht zurueckliefert. Diese Implementierung haengt
// das XML an unsere SpeakerInventory: pro bekanntem Speaker mit gleichem
// accountId ein <device>-Block mit echten Werten (deviceid, model→
// product_code, firmware, ip, name, Presets), plus <sources> mit
// **beiden** TUNEIN-Eintraegen (Legacy provider 3 wie bisher + account-
// bound provider 25 mit nicht-leerer credential), damit speaker-seitige
// Logik die "account-bound TUNEIN"-Variante wieder findet.
// -----------------------------------------------------------------------------

// 2026-05-21 Bosman-Schema-Replay (proxy-capture-verified):
//   Bosman vergibt sequenzielle source-IDs 1,2,3,4,5,... — KEINE 10000+-Range.
//   Reihenfolge: 1=TUNEIN(25), 2=RADIO_BROWSER(39), 3=LOCAL_INTERNET_RADIO(11),
//                4..N=STORED_MUSIC(7) pro DLNA-UUID.
//   Presets referenzieren die source ueber id=N (Bosman-Preset-Block:
//     <source id="4" type="Audio"> ... <username>UUID/0</username></source>).
static constexpr const char* kSrcIdTuneIn          = "1";
static constexpr const char* kSrcIdRadioBrowser    = "2";
static constexpr const char* kSrcIdLocalIntRadio   = "3";
static constexpr int         kSrcIdStoredMusicBase = 4;  // erste UUID-Source

// Timestamps in Bosman-Form: ISO mit ms+Z. Bosman-2012-Referenz fuer
// "ewige" Records, "heute" fuer per-Speaker-Created. Wir bleiben pragmatisch
// und nehmen einen festen Stempel — Speaker akzeptiert alles ISO8601.
static constexpr const char* kFakeTs        = "2020-01-01T00:00:00.000+00:00";
static constexpr const char* kBosmanInitTs  = "2012-09-19T12:43:00.000+00:00";  // Bosman-Genesis-Stempel

// Etag muss konsistent ueber Calls auf gleichen Content sein (content-hash-
// artig). Bosman benutzt 13-stellige Unix-millis. Wir nehmen einen deterministischen
// counter pro Content-Type, der nur wechselt wenn sich content aendert.
static String etagFor_(const String& tag) {
    return String("1779") + String((uint32_t)(tag.length() * 7919 + 100000)) + "008";
}

// Bosman-XML-Decl (standalone=yes, KEIN encoding-attribute)
static constexpr const char* kXmlDeclBosman =
    "<?xml version=\"1.0\" standalone=\"yes\"?>";

// model -> attachedProduct{product_code, productlabel}
static void modelToProduct_(const String& model, String& outProductCode, String& outLabel) {
    if (model.indexOf("10") >= 0) {
        outProductCode = "SoundTouch 10 sm2"; outLabel = "soundtouch_10";
    } else if (model.indexOf("20") >= 0) {
        outProductCode = "SoundTouch 20 sm2"; outLabel = "soundtouch_20_series3";
    } else if (model.indexOf("30") >= 0) {
        outProductCode = "SoundTouch 30 sm2"; outLabel = "soundtouch_30";
    } else if (model.indexOf("300") >= 0) {
        outProductCode = "SoundTouch 300";    outLabel = "soundtouch_300";
    } else if (model.indexOf("Wireless") >= 0 || model.indexOf("Link") >= 0) {
        outProductCode = "SoundTouch Wireless Link adapter"; outLabel = "soundtouch_link";
    } else {
        outProductCode = model.length() ? model : String("SoundTouch");
        outLabel = "soundtouch_generic";
    }
}

static String xmlEsc_(const String& in) {
    String o; o.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in.charAt(i);
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default:   o += c;        break;
        }
    }
    return o;
}

// Pro-Device Preset-Block im Bosman-Schema (proxy-capture-verified
// 2026-05-21, siehe reference_bosman_cloud_schema.md). Emittiert pro Slot
// einen <preset buttonNumber="N">-Block mit eingebettetem <source>-Verweis
// auf eine source-id aus dem <sources>-Block der Account-Antwort. Fuer
// OPAQUE-Slots (STORED_MUSIC / DLNA) wird location + name aus rawContentItem
// extrahiert und die source-id ueber die UUID->ID-Map des Callers aufgeloest.
//
// uuidToSrcId: muss vom Caller (handleAccountFull) IDENTISCH zur /sources-
// Block-Allokation gebaut werden (kSrcIdStoredMusicBase + first-occurrence
// in matched-speakers). Ohne diese Map wuerden OPAQUE-Slots auf eine
// nicht-existente source-id verweisen und der Speaker wuerde den Slot
// verwerfen.
// -----------------------------------------------------------------------------
// Gap-Heal (2. Verlustpfad, am Blech bewiesen 2026-08-07): traegt der beim
// Boot geladene Slice eines Speakers einen Gap-Marker (Save-Fail vor dem
// Reboot -> moegliche Luecke gegenueber dem Geraet), liefern die Cloud-Mock-
// Endpoints 404 — eine unvollstaendige <presets>-Liste wuerde den fehlenden
// Slot AM GERAET loeschen (am Blech gemessen: binnen ~30-60 s, ohne Reboot,
// ohne dass jemand den Speaker anfasst). Der 404
// ist fuer die Bose-FW "Cloud antwortet nicht" und laesst den Cache stehen.
// Damit das kein Dauerzustand wird, stoesst der erste gegatete Poll diesen
// Task an: HW-Presets vom Geraet ziehen und die RAM-Luecken fuellen — danach
// ist der RAM vollstaendig, der naechste Poll (~30-60 s) bekommt wieder 200.
// Der persistente Marker verschwindet erst mit einem erfolgreichen NVS-Save;
// bleibt die Partition voll, wiederholt sich Boot->Verdacht->Heilung, das
// Geraet bleibt dabei durchgehend geschuetzt.
// -----------------------------------------------------------------------------
struct GapHealJob_ { String deviceId; };

static void gapHealTask_(void* arg) {
    auto* job = static_cast<GapHealJob_*>(arg);
    int countOk = 0, countAban = 0, httpCode = 0;
    int rc = importPresetsFillEmptyForHeal(job->deviceId, countOk, countAban,
                                           httpCode);
    if (rc == 200) {
        // RAM ist jetzt vollstaendig (Luecken aus HW gefuellt) — auch wenn
        // der NVS-Save weiter scheitert, darf /full wieder ausliefern.
        sixback::PresetStore::instance().finishGapHeal(job->deviceId, true);
        Serial.printf("[bmx][heal] %s: HW-Merge ok (%d Preset(s) uebernommen) "
                      "— naechster Poll liefert wieder voll\n",
                      job->deviceId.c_str(), countOk);
    } else {
        // Speaker nicht erreichbar o.ae. — Verdacht bleibt, der naechste
        // gegatete Poll versucht es erneut.
        sixback::PresetStore::instance().finishGapHeal(job->deviceId, false);
        Serial.printf("[bmx][heal] %s: HW-Pull fehlgeschlagen (rc=%d http=%d) "
                      "— Gate bleibt, Retry beim naechsten Poll\n",
                      job->deviceId.c_str(), rc, httpCode);
    }
    delete job;
    vTaskDelete(nullptr);
}

// 404-Gate-Begleiter: Heil-Task genau einmal je Verdacht anstossen
// (tryBeginGapHeal dedupliziert — die Speaker pollen alle ~30-60 s).
static void spawnGapHeal_(const String& deviceId) {
    if (!sixback::PresetStore::instance().tryBeginGapHeal(deviceId)) return;
    auto* job = new (std::nothrow) GapHealJob_{deviceId};
    if (!job) { sixback::PresetStore::instance().finishGapHeal(deviceId, false); return; }
    if (xTaskCreate(gapHealTask_, "gap-heal", 8192, job, 1, nullptr) != pdPASS) {
        sixback::PresetStore::instance().finishGapHeal(deviceId, false);
        delete job;
    }
}

static String buildDevicePresets_(const String& deviceId,
                                  const std::map<String, int>& uuidToSrcId) {
    // Wenn der Store keine Presets fuer das Device hat, KEIN <presets>-Element
    // ausgeben.
    //
    // ⚠️ KORREKTUR 2026-08-07: hier stand frueher, das weggelassene Element
    // verhindere, dass der Speaker seinen Local-Cache ueberschreibt. Das ist
    // FALSCH und hat zwei Massenverluste ueberlebt (2026-05-20, 2026-08-06).
    // Die Bose-FW liest den fehlenden Block als "Cloud-Account hat keine
    // Presets" und LEERT den Cache; nur ein 404 auf Request-Ebene schuetzt.
    // Der leere String hier ist deshalb KEINE Schutzmassnahme, sondern nur
    // die korrekte Serialisierung eines leeren Slice — der Schutz sitzt im
    // Per-Device-Gate in handleAccountFull, das so einen Block gar nicht
    // erst an den anfragenden Speaker ausliefert.
    if (!sixback::PresetStore::instance().hasAnyFor(deviceId)) {
        return String();
    }
    auto presets = sixback::PresetStore::instance().getForSpeaker(deviceId);
    int nonEmpty = 0;
    for (const auto& p : presets) {
        if (p.source != sixback::PresetSource::EMPTY) ++nonEmpty;
    }
    if (nonEmpty == 0) return String();

    String out = "<presets>";
    for (const auto& p : presets) {
        if (p.source == sixback::PresetSource::EMPTY) continue;

        // OPAQUE-Slots (STORED_MUSIC / DLNA / UPnP): Bosman-Schema-konformen
        // <preset>-Block bauen aus rawContentItem. Ohne diesen Block wuerde
        // der Speaker den Slot beim naechsten /full-Pull als "Cloud sagt
        // geloescht" interpretieren und lokal verwerfen — siehe
        // reference_bosman_cloud_schema.md Z.63-82.
        if (p.source == sixback::PresetSource::OPAQUE) {
            const String& raw = p.rawContentItem;
            int locStart = raw.indexOf("location=\"");
            int saStart  = raw.indexOf("sourceAccount=\"");
            int nmStart  = raw.indexOf("<itemName>");
            if (locStart < 0 || saStart < 0 || nmStart < 0) continue;
            int locEnd = raw.indexOf("\"", locStart + 10);
            int saEnd  = raw.indexOf("\"", saStart + 15);
            int nmEnd  = raw.indexOf("</itemName>", nmStart);
            if (locEnd < 0 || saEnd < 0 || nmEnd < 0) continue;
            String location      = raw.substring(locStart + 10, locEnd);
            String sourceAccount = raw.substring(saStart + 15, saEnd);
            String itemName      = raw.substring(nmStart + 10, nmEnd);
            // UUID ohne "/0"-Suffix fuer Lookup in uuidToSrcId
            String uuid = sourceAccount;
            int slash = uuid.indexOf('/');
            if (slash >= 0) uuid = uuid.substring(0, slash);
            auto it = uuidToSrcId.find(uuid);
            if (it == uuidToSrcId.end()) continue;  // UUID nicht in /sources — Slot skippen
            int srcId = it->second;
            out += "<preset buttonNumber=\""; out += String(p.slot); out += "\">";
            out += "<containerArt></containerArt>";
            out += "<contentItemType></contentItemType>";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<location>"; out += xmlEsc_(location); out += "</location>";
            out += "<name>"; out += xmlEsc_(itemName); out += "</name>";
            out += "<source id=\""; out += String(srcId); out += "\" type=\"Audio\">";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<credential type=\"token\"></credential>";
            out += "<name>"; out += xmlEsc_(uuid); out += "</name>";
            out += "<sourceproviderid>7</sourceproviderid>";
            out += "<sourcename>STORED_MUSIC</sourcename>";
            out += "<sourceSettings/>";
            out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
            out += "<username>"; out += xmlEsc_(sourceAccount); out += "</username>";
            out += "</source>";
            out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
            out += "</preset>";
            continue;
        }
        out += "<preset buttonNumber=\""; out += String(p.slot); out += "\">";
        out += "<containerArt>"; out += xmlEsc_(p.imageUrl); out += "</containerArt>";
        out += "<contentItemType>stationurl</contentItemType>";
        out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
        out += "<location>";
        if (p.source == sixback::PresetSource::TUNEIN) {
            out += "/v1/playback/station/"; out += xmlEsc_(p.stationId);
        } else {
            out += xmlEsc_(p.streamUrl);
        }
        out += "</location>";
        out += "<name>"; out += xmlEsc_(p.name); out += "</name>";
        // Eingebetteter Source-Verweis nach Bosman-Schema:
        //   <source id="N" type="Audio"><createdOn>../<credential type="token"></credential>
        //   <name>display</name><sourceproviderid>P</sourceproviderid>
        //   <sourcename>NAME</sourcename><sourceSettings/><updatedOn>..
        //   <username>account</username></source>
        if (p.source == sixback::PresetSource::TUNEIN) {
            out += "<source id=\""; out += kSrcIdTuneIn; out += "\" type=\"Audio\">";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<credential type=\"token\"></credential>";
            out += "<name>TuneIn</name>";
            out += "<sourceproviderid>25</sourceproviderid>";
            out += "<sourcename>TUNEIN</sourcename>";
            out += "<sourceSettings/>";
            out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
            out += "<username></username>";
            out += "</source>";
        } else {
            out += "<source id=\""; out += kSrcIdLocalIntRadio; out += "\" type=\"Audio\">";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<credential type=\"token\"></credential>";
            out += "<name>Local Internet Radio</name>";
            out += "<sourceproviderid>11</sourceproviderid>";
            out += "<sourcename>LOCAL_INTERNET_RADIO</sourcename>";
            out += "<sourceSettings/>";
            out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
            out += "<username></username>";
            out += "</source>";
        }
        out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
        out += "<username>"; out += xmlEsc_(p.name); out += "</username>";
        out += "</preset>";
    }
    out += "</presets>";
    return out;
}

// Pre-Probe: query a speaker's /sources via BMX-API, return the set of
// UUIDs (without "/0" suffix) that the speaker reports as
// <sourceItem source="STORED_MUSIC" sourceAccount="<UUID>/0" status="READY">.
// Used by handleAccountFull/handleAccountSources to AVOID emitting our own
// generic <source>-blocks for those UUIDs — empirisch verifiziert 2026-05-21:
// wenn unser account/full den UUID-source-Block enthaelt, ueberschreibt
// der Speaker seinen lokalen <sourceItem READY>-state mit Default UNAVAILABLE,
// und die DLNA-Presets brechen mit 1005 UNKNOWN_SOURCE_ERROR. Skippen wir
// den block, behaelt der Speaker den bereits eingerichteten state.
static std::set<String> probeSpeakerStoredMusicReadyUuids_(const String& speakerIp) {
    std::set<String> result;
    if (speakerIp.length() == 0) return result;
    HTTPClient http;
    String url = "http://" + speakerIp + ":8090/sources";
    if (!http.begin(url)) return result;
    http.setTimeout(1500);
    int code = http.GET();
    if (code != 200) { http.end(); return result; }
    String body = http.getString();
    http.end();
    int pos = 0;
    while ((pos = body.indexOf("<sourceItem ", pos)) >= 0) {
        int endTag = body.indexOf(">", pos);
        if (endTag < 0) break;
        String tag = body.substring(pos, endTag);
        pos = endTag + 1;
        if (tag.indexOf("source=\"STORED_MUSIC\"") < 0) continue;
        if (tag.indexOf("status=\"READY\"") < 0) continue;
        int accPos = tag.indexOf("sourceAccount=\"");
        if (accPos < 0) continue;
        int accStart = accPos + 15;
        int accEnd = tag.indexOf("\"", accStart);
        if (accEnd < 0) continue;
        String acc = tag.substring(accStart, accEnd);
        int slash = acc.indexOf("/");
        String uuid = (slash > 0) ? acc.substring(0, slash) : acc;
        if (uuid.length() > 0) result.insert(uuid);
    }
    return result;
}

void handleAccountFull(AsyncWebServerRequest* req) {
    String acct = pathParam(0);

    // <devices> — alle bekannten Speaker mit gleichem accountId.
    // Fallback wenn die Liste fuer den angefragten Account leer ist:
    // dann liefern wir ALLE Speaker (akzeptieren also dass der Speaker
    // sich selbst in dem device-Block wiederfindet, auch wenn die acct-
    // Verknuepfung im Inventory noch nicht aufgebaut wurde).
    auto allSpeakers = sixback::SpeakerInventory::instance().list();
    std::vector<sixback::Speaker> matched;
    for (const auto& s : allSpeakers) if (s.accountId == acct) matched.push_back(s);
    if (matched.empty()) matched = allSpeakers;
    // Inventory (noch) komplett leer — z.B. im fresh-erase-Fenster, bevor
    // die Discovery es wieder gefuellt hat, waehrend die Speaker bereits
    // gegen uns pollen: dann gibt es keinen Device-Block, den wir sicher
    // ausliefern koennten. Das alte flotten-weite Gate 404te hier implizit
    // (beide any*-Flags false); das Per-Device-Gate unten wuerde bei leerem
    // matched aber DURCHFALLEN (Schleife ueber null Elemente -> 200 mit
    // leerer <devices>-Liste, Verhalten der Bose-FW darauf unbekannt).
    // Deshalb expliziter Guard: 404 = Speaker behalten ihren Cache.
    // (Review-Fund 2026-08-07, vor dem ersten Push gefixt.)
    if (matched.empty()) {
        Serial.println("[bmx][safe] account/full -> 404 (inventory leer — kein Device-Block lieferbar)");
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }

    // Per-Request-Disambiguation 2026-05-22: Bose-Speaker parsen /full nicht
    // per <device deviceid=...>-Match, sondern uebernehmen den ERSTEN <device>-
    // Block als ihren eigenen State. Bei Multi-Device-Account (Emma+Küche
    // share accountId) bekam jeder Speaker den state des ersten Eintrags.
    //
    // Loesung: aus der gemeinsamen matched-Liste den anfragenden Speaker per
    // Remote-IP identifizieren und an Position 0 sortieren — sodass jeder
    // Speaker seinen eigenen Block "vorne" sieht. Schwester-Block bleibt
    // dran (Bose-App + group-orchestration brauchen das ggf.), aber der
    // anfragende Speaker greift das erste = sein eigenes.
    // requesterId wird ZUSAETZLICH zum Re-Ordering gebraucht: das
    // Wipe-Schutz-Gate unten entscheidet per-Device, und dafuer muss auch der
    // Ein-Speaker-Fall (matched.size()==1, kein Swap noetig) den Anfrager
    // kennen. Leer = per Remote-IP nicht zuzuordnen -> Gate faellt unten auf
    // die konservative Variante zurueck.
    String remoteIp = req->client() ? req->client()->remoteIP().toString() : String();
    String requesterId;
    if (!remoteIp.isEmpty()) {
        for (size_t i = 0; i < matched.size(); ++i) {
            if (matched[i].ip == remoteIp) {
                if (i != 0) std::swap(matched[0], matched[i]);
                requesterId = matched[0].deviceId;
                if (matched.size() > 1) {
                    Serial.printf("[bmx] account/full: requesting speaker ip=%s -> %s (block re-ordered to first)\n",
                                  remoteIp.c_str(), requesterId.c_str());
                }
                break;
            }
        }
    }

    // Boot-Load-Fail-Gate (FHEM 144729 #153, 2026-07-17): konnte der
    // Preset-Store beim Boot NICHT geladen werden, obwohl ein Blob in NVS
    // LIEGT (unlesbar/korrupt), ist jeder 200er hier brandgefaehrlich —
    // hasAnyFor() liest dann fuer JEDES Device false, das Per-Device-Gate
    // unten wuerde konsequent 404 liefern, aber verlassen wollen wir uns
    // darauf nicht: die Speaker wipen ihre Caches, sobald doch ein Block ohne
    // <presets>-Element rausgeht (Vorfall 2026-05-20), obwohl der User
    // Presets HAT. Dann lieber
    // pauschal 404 (Speaker behalten den Cache) — bis der erste
    // erfolgreiche Save den defekten Blob ersetzt hat (loescht das Flag;
    // auch Recovery via import-from-device bleibt so moeglich, weil die
    // Speaker-HW-Presets nie ueberschrieben wurden). Fresh-Install (KEIN
    // Blob vorhanden) setzt das Flag nicht — Migration/Kueche-DLNA-Pfad
    // (2026-05-21) bleibt unangetastet.
    if (sixback::PresetStore::instance().loadFailedFromNvs()) {
        Serial.println("[bmx][safe] account/full -> 404 (preset store unlesbar — schuetzt Speaker-Cache bis zum ersten erfolgreichen Save)");
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }

    // Wipe-Schutz, PER-DEVICE (2026-08-07). Ersetzt das fruehere flotten-weite
    // Gate `!anySeeded && !anyMediaServers`.
    //
    // Warum ueberhaupt ein Gate: ein 200 mit einem <device>-Block OHNE
    // <presets>-Element liest die Bose-FW als "Cloud-Account hat keine
    // Presets" und LEERT ihren lokalen Cache. Nur 404 ("Cloud antwortet
    // nicht") laesst den Cache stehen. buildDevicePresets_ liefert fuer ein
    // ungeseedetes Device genau so einen leeren String.
    //
    // Warum das alte Gate nicht reichte (Vorfall 2026-08-06/07, identisch zu
    // 2026-05-20): beide Bedingungen waren flotten-weit. Nach einem
    // fresh-erase des Sticks, der die Speaker besitzt, war der Store leer,
    // aber `refreshMediaServers()` hatte Kueches DLNA-UUIDs schon wieder
    // gefuellt -> anyMediaServers==true -> Gate feuerte NICHT -> 200 ohne
    // <presets> -> ALLE drei Speaker haben ihren Cache geleert. Spaeter hielt
    // umgekehrt ein einziger geseedeter Speaker (anySeeded==true) das Gate
    // fuer die beiden leeren offen. Ein Gate, das die ganze Flotte am
    // schwaechsten Glied ausrichtet, schuetzt niemanden.
    //
    // Neue Regel, symmetrisch zu handleAccountPresets + handleDevicePresets
    // (die das laengst per-Device machen): es zaehlt nur, ob DER ANFRAGENDE
    // Speaker geseedet ist — denn nur er wertet diese Antwort aus. Ein
    // ungeseedeter Schwester-Block ist ungefaehrlich, weil der Anfrager per
    // Remote-IP auf Position 0 sortiert wird und laut 05-22-Befund den
    // ERSTEN Block als seinen eigenen State uebernimmt.
    //
    // Die mediaServerUuids-Lockerung von 2026-05-21 entfaellt — als BEWUSSTER
    // Trade-off, nicht weil sie ueberfluessig gewesen waere: die
    // STORED_MUSIC-Source-Registrierung passiert nachweislich beim
    // /full-Pull (P0a-Befund 2026-05-21, Pi5-Proxy-Capture: Bosman macht
    // nichts ausser GET /full; dass ein Speaker den separaten
    // /sources-Endpoint jemals zieht, ist NICHT belegt). Ein ungeseedeter
    // Speaker mit DLNA-Servern bekommt seine UUID-Sources deshalb erst,
    // wenn sein Store-Slice das erste Preset traegt (dann 200 inkl.
    // Sources-Block).
    //
    // Kein Henne-Ei und keine Sackgasse (2026-08-07 verifiziert):
    //  - Der DLNA-BROWSE haengt NICHT am Speaker. /api/dlna/browse spricht
    //    UPnP direkt vom Stick zum Medienserver (server-scoped, s. dort) —
    //    der User kann also auch auf einem virgin Speaker DLNA durchsuchen
    //    und ein DLNA-Preset in den Store legen. Das seedet den Slice ->
    //    naechster /full-Pull liefert 200 inkl. Sources -> Speaker
    //    registriert STORED_MUSIC -> Preset spielbar.
    //  - Die Registrierung ist am Speaker PERSISTENT: Greta hat einen
    //    kompletten Ausflug auf einen fremden Stick mit ausschliesslich
    //    404-Antworten plus zwei Reboots ueberstanden und beide
    //    STORED_MUSIC-sourceItems READY behalten.
    // Rest-Effekt ist damit: beim ALLERERSTEN DLNA-Preset eines virgin
    // Speakers liegt ein /full-Zyklus (beobachtet ~30-60 s) zwischen
    // Speichern und Abspielbarkeit. Vorher /select -> 500 UNKNOWN_SOURCE
    // (Befund A). Das ist der Preis; er heilt sich selbst.
    //
    // Wipe-Schutz schlaegt Virgin-Bootstrap: ein 200 ohne <presets> ist
    // unter keinen Umstaenden sicher (Vorfaelle 2026-05-20 + 2026-08-06,
    // beide durch genau diese Lockerung).
    if (!requesterId.isEmpty()) {
        if (!sixback::PresetStore::instance().hasAnyFor(requesterId)) {
            Serial.printf("[bmx][safe] account/full %s -> 404 (store empty fuer den Anfrager — Speaker behaelt Cache)\n",
                          requesterId.c_str());
            req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
            return;
        }
        // 2. Verlustpfad (2026-08-07): Slice VORHANDEN, aber mit Gap-Marker
        // geladen — eine Liste mit fehlendem Slot loescht den Slot am
        // Geraet. 404 + Heilung anstossen (s. gapHealTask_ oben).
        if (sixback::PresetStore::instance().gapSuspect(requesterId)) {
            Serial.printf("[bmx][safe] account/full %s -> 404 (gap-marker: Slice evtl. lueckenhaft — HW-Merge angestossen)\n",
                          requesterId.c_str());
            spawnGapHeal_(requesterId);
            req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
            return;
        }
    } else {
        // Anfrager per Remote-IP NICHT zuzuordnen (IP-Wechsel, Proxy, leere
        // Client-Info): dann ist unbekannt, wer Block 0 adoptiert. Nur
        // ausliefern, wenn JEDES matched Device geseedet und lueckenfrei
        // ist — sonst koennte der Anfrager ausgerechnet den leeren bzw.
        // lueckigen Block erwischen.
        for (const auto& s : matched) {
            if (!sixback::PresetStore::instance().hasAnyFor(s.deviceId)) {
                Serial.printf("[bmx][safe] account/full -> 404 (Anfrager ip=%s unbekannt, %s ungeseedet — Speaker behalten Cache)\n",
                              remoteIp.c_str(), s.deviceId.c_str());
                req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
                return;
            }
            if (sixback::PresetStore::instance().gapSuspect(s.deviceId)) {
                Serial.printf("[bmx][safe] account/full -> 404 (Anfrager ip=%s unbekannt, %s mit gap-marker — HW-Merge angestossen)\n",
                              remoteIp.c_str(), s.deviceId.c_str());
                spawnGapHeal_(s.deviceId);
                req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
                return;
            }
        }
    }

    String body;
    body.reserve(4096);

    // Bosman-Schema 1:1 (proxy-capture-verified 2026-05-21):
    //   <?xml version="1.0" standalone="yes"?>
    //   <account><id>X</id><accountStatus>ENABLED</accountStatus>
    //     <devices>...</devices>
    //     <mode>global</mode><preferredLanguage>en</preferredLanguage>
    //     <providerSettings/>
    //     <sources>...</sources>
    //   </account>
    body  = kXmlDeclBosman;
    body += "<account><id>"; body += xmlEsc_(acct); body += "</id>";
    body += "<accountStatus>ENABLED</accountStatus>";

    // UUID -> source-id Map einmalig vorab bauen — wird sowohl von
    // buildDevicePresets_ (fuer OPAQUE-Slot-source-Verweise) als auch
    // vom /sources-Block weiter unten benutzt. Reihenfolge: erste
    // Begegnung pro UUID in matched-Iteration, beginnend bei
    // kSrcIdStoredMusicBase (=4). Konsistenz zwischen beiden Stellen
    // ist Pflicht — sonst zeigt der <preset>-source-Verweis ins Leere.
    std::map<String, int> uuidToSrcId;
    {
        int dlnaIdx = kSrcIdStoredMusicBase;
        std::set<String> seen;
        for (const auto& sp : matched) {
            for (const auto& uuid : sp.mediaServerUuids) {
                if (uuid.length() == 0) continue;
                if (!seen.insert(uuid).second) continue;
                uuidToSrcId[uuid] = dlnaIdx++;
            }
        }
    }

    body += "<devices>";
    for (const auto& s : matched) {
        String prodCode, prodLabel;
        modelToProduct_(s.model, prodCode, prodLabel);
        body += "<device deviceid=\""; body += xmlEsc_(s.deviceId); body += "\">";
        // attachedProduct.serialnumber bei Bosman LEER (Speaker hat den
        // realen serial unten als <serialnumber> nochmal lowercase).
        body += "<attachedProduct product_code=\""; body += xmlEsc_(prodCode); body += "\">";
        body += "<components/>";
        body += "<productlabel>"; body += xmlEsc_(prodLabel); body += "</productlabel>";
        body += "<serialnumber></serialnumber>";
        body += "</attachedProduct>";
        body += "<createdOn>"; body += kBosmanInitTs; body += "</createdOn>";
        body += "<firmwareVersion>"; body += xmlEsc_(s.firmware); body += "</firmwareVersion>";
        body += "<ipaddress>"; body += xmlEsc_(s.ip); body += "</ipaddress>";
        body += "<name>"; body += xmlEsc_(s.name); body += "</name>";
        body += buildDevicePresets_(s.deviceId, uuidToSrcId);
        body += "<recents/>";
        // Bosman: <serialnumber> (lowercase) mit echtem MAC/Serial
        body += "<serialnumber>"; body += xmlEsc_(s.deviceId); body += "</serialnumber>";
        body += "<updatedOn>"; body += kBosmanInitTs; body += "</updatedOn>";
        body += "</device>";
    }
    body += "</devices>";

    body += "<mode>global</mode>";
    body += "<preferredLanguage>en</preferredLanguage>";
    // Bosman: providerSettings DIREKT als leeres Element im account
    body += "<providerSettings/>";

    // Sources nach Bosman-Schema: sequenziell durchnummeriert, vollstaendige
    // Schema-Felder. Speaker zieht via sourceAccount=<username> die Verknuepfung.
    body += "<sources>";

    // id=1: TUNEIN (sourceproviderid=25, "TuneIn")
    body += "<source id=\""; body += kSrcIdTuneIn; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>TuneIn</name>";
    body +=   "<sourceproviderid>25</sourceproviderid>";
    body +=   "<sourcename>TUNEIN</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=2: RADIO_BROWSER (sourceproviderid=39)
    body += "<source id=\""; body += kSrcIdRadioBrowser; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>RadioBrowser</name>";
    body +=   "<sourceproviderid>39</sourceproviderid>";
    body +=   "<sourcename>RADIO_BROWSER</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=3: LOCAL_INTERNET_RADIO (sourceproviderid=11)
    body += "<source id=\""; body += kSrcIdLocalIntRadio; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>Local Internet Radio</name>";
    body +=   "<sourceproviderid>11</sourceproviderid>";
    body +=   "<sourcename>LOCAL_INTERNET_RADIO</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=4..N: STORED_MUSIC pro DLNA-UUID (sourceproviderid=7).
    // Bosman-Schema: <username>UUID/0</username> + <name>display</name>.
    // Wir nutzen die uuidToSrcId-Map die OBEN gebaut wurde — selbe Map
    // referenzieren die <preset>-source-id-Verweise. Ohne diese Konsistenz
    // sind <preset>-Slots auf nicht-existente source-IDs verlinkt.
    // Emission-Order: erst nach src-id sortieren (statt Map-Key) damit die
    // ID-Reihenfolge im XML monoton aufsteigt — kleinere Anomalie fuer
    // Bose-Parser-Logik.
    std::vector<std::pair<int, String>> srcIdToUuid;  // (srcId, uuid)
    srcIdToUuid.reserve(uuidToSrcId.size());
    for (const auto& kv : uuidToSrcId) srcIdToUuid.push_back({kv.second, kv.first});
    std::sort(srcIdToUuid.begin(), srcIdToUuid.end());
    for (const auto& pr : srcIdToUuid) {
        const int srcId = pr.first;
        const String& uuid = pr.second;
        body += "<source id=\""; body += String(srcId); body += "\" type=\"Audio\">";
        body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
        body +=   "<credential type=\"token\"></credential>";
        body +=   "<name>"; body += xmlEsc_(uuid); body += "</name>";
        body +=   "<sourceproviderid>7</sourceproviderid>";
        body +=   "<sourcename>STORED_MUSIC</sourcename>";
        body +=   "<sourceSettings/>";
        body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
        body +=   "<username>"; body += xmlEsc_(uuid); body += "/0</username>";
        body += "</source>";
    }

    body += "</sources>";
    body += "</account>";

    // Bosman-Response-Header: Method_name + Etag (content-derived hash).
    AsyncWebServerResponse* resp = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", body);
    resp->addHeader("Method_name", "getFullAccount");
    resp->addHeader("Etag", etagFor_(String("full:") + acct));
    req->send(resp);
}

void handleAccountSources(AsyncWebServerRequest* req) {
    // Bosman-Schema: gleiche source-Bloecke wie in account/full, sequenziell.
    // Bosman liefert bei /sources tatsaechlich dieselbe Liste wie unter
    // account/full <sources>, gleiches XML-Decl + Etag-Konsistenz.
    String body;
    body.reserve(2048);
    body  = kXmlDeclBosman;
    body += "<sources>";

    // id=1: TUNEIN
    body += "<source id=\""; body += kSrcIdTuneIn; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>TuneIn</name>";
    body +=   "<sourceproviderid>25</sourceproviderid>";
    body +=   "<sourcename>TUNEIN</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=2: RADIO_BROWSER
    body += "<source id=\""; body += kSrcIdRadioBrowser; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>RadioBrowser</name>";
    body +=   "<sourceproviderid>39</sourceproviderid>";
    body +=   "<sourcename>RADIO_BROWSER</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=3: LOCAL_INTERNET_RADIO
    body += "<source id=\""; body += kSrcIdLocalIntRadio; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>Local Internet Radio</name>";
    body +=   "<sourceproviderid>11</sourceproviderid>";
    body +=   "<sourcename>LOCAL_INTERNET_RADIO</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";

    // id=4..N: STORED_MUSIC pro UUID
    int dlnaIdx = kSrcIdStoredMusicBase;
    std::set<String> emittedUuids;
    auto allSpeakers = sixback::SpeakerInventory::instance().list();
    for (const auto& s : allSpeakers) {
        for (const auto& uuid : s.mediaServerUuids) {
            if (uuid.length() == 0) continue;
            if (!emittedUuids.insert(uuid).second) continue;
            body += "<source id=\""; body += String(dlnaIdx++); body += "\" type=\"Audio\">";
            body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
            body +=   "<credential type=\"token\"></credential>";
            body +=   "<name>"; body += xmlEsc_(uuid); body += "</name>";
            body +=   "<sourceproviderid>7</sourceproviderid>";
            body +=   "<sourcename>STORED_MUSIC</sourcename>";
            body +=   "<sourceSettings/>";
            body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
            body +=   "<username>"; body += xmlEsc_(uuid); body += "/0</username>";
            body += "</source>";
        }
    }

    body += "</sources>";

    AsyncWebServerResponse* resp = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", body);
    resp->addHeader("Method_name", "getSources");
    resp->addHeader("Etag", etagFor_(String("sources")));
    req->send(resp);
}

// -----------------------------------------------------------------------------
// GET /streaming/sourceproviders  -- Provider-Katalog (P1, P0-Audit-Befund)
//   Speaker polled das alle ~30s pro Geraet. Antwort listet die Streaming-
//   Services die im Bose-Universum existieren. Unsere Liste = was wir tatsaechlich
//   bedienen koennen (TUNEIN, INTERNET_RADIO, LOCAL_INTERNET_RADIO). IDs decken
//   sich mit den sourceproviderids in handleAccountSources.
// -----------------------------------------------------------------------------
void handleSourceProviders(AsyncWebServerRequest* req) {
    // Bosman-Schema (proxy-capture-verified 2026-05-21): volle Liste von ALLEN
    // Bose-cloud-bekannten Providern, mit id+name+createdOn+updatedOn.
    // Speaker referenziert sourceproviderid aus diesem Katalog. Unsere
    // sources-Liste benutzt nur: 7 STORED_MUSIC, 11 LOCAL_INTERNET_RADIO,
    // 25 TUNEIN, 39 RADIO_BROWSER — aber wir liefern Bosman-volle Liste
    // damit Speaker keinen "uebliche-IDs-fehlen"-Pfad triggert.
    static constexpr const char* kTs = "2012-09-19T12:43:00.000+00:00";
    String body;
    body.reserve(4096);
    body  = kXmlDeclBosman;
    body += "<sourceProviders>";
    struct Sp { const char* id; const char* name; };
    static const Sp kProviders[] = {
        {"1",  "PANDORA"},
        {"2",  "INTERNET_RADIO"},
        {"3",  "OFF"},
        {"4",  "LOCAL"},
        {"5",  "AIRPLAY"},
        {"6",  "CURRATED_RADIO"},
        {"7",  "STORED_MUSIC"},
        {"8",  "SLAVE_SOURCE"},
        {"9",  "AUX"},
        {"10", "RECOMMENDED_INTERNET_RADIO"},
        {"11", "LOCAL_INTERNET_RADIO"},
        {"12", "GLOBAL_INTERNET_RADIO"},
        {"13", "HELLO"},
        {"14", "DEEZER"},
        {"15", "SPOTIFY"},
        {"16", "IHEART"},
        {"17", "SIRIUSXM"},
        {"18", "GOOGLE_PLAY_MUSIC"},
        {"19", "QQMUSIC"},
        {"20", "AMAZON"},
        {"21", "LOCAL_MUSIC"},
        {"22", "WBMX"},
        {"23", "SOUNDCLOUD"},
        {"24", "TIDAL"},
        {"25", "TUNEIN"},
        {"39", "RADIO_BROWSER"},
    };
    for (const auto& p : kProviders) {
        body += "<sourceprovider id=\""; body += p.id; body += "\">";
        body +=   "<createdOn>"; body += kTs; body += "</createdOn>";
        body +=   "<name>"; body += p.name; body += "</name>";
        body +=   "<updatedOn>"; body += kTs; body += "</updatedOn>";
        body += "</sourceprovider>";
    }
    body += "</sourceProviders>";

    AsyncWebServerResponse* resp = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", body);
    resp->addHeader("Etag", etagFor_("sourceProviders"));
    req->send(resp);
}

// -----------------------------------------------------------------------------
// GET / auf Port 8000 (Bose-Cloud-Mock)  -- Health-Probe (P0-Audit-Befund)
//   Speaker rufen das vor jedem /streaming/sourceproviders-Polling auf.
//   Minimal-200 reicht; gleichzeitig nett fuer Menschen die direkt mit dem
//   Browser auf Port 8000 zugreifen.
// -----------------------------------------------------------------------------
void handleBoseRoot(AsyncWebServerRequest* req) {
    static const char* body =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>SixBack Cloud-Mock</title></head><body>"
        "<h1>SixBack Cloud-Mock</h1>"
        "<p>Bose-SoundTouch-Cloud-Replacement, lokal auf ESP32.</p>"
        "<p>Verwaltung: <a href=\"/\">Port 80</a></p>"
        "</body></html>";
    req->send(200, "text/html; charset=utf-8", body);
}

// -----------------------------------------------------------------------------
// GET /media/bmx-icons/*  -- BMX-Service-Icons (P0-Audit-Befund)
//   bmx_services.json referenziert {MEDIA_SERVER}/bmx-icons/{service}/...
//   Wir besitzen die Original-Bose-Icons nicht. Liefer ein 1x1-transparent-PNG
//   als Platzhalter; Speaker-UI zeigt dann einfach kein Logo (statt 404-Spam).
// -----------------------------------------------------------------------------
static const uint8_t kPng1x1Transparent[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
    0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
    0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
    0x42,0x60,0x82
};

void handleBmxIconStub(AsyncWebServerRequest* req) {
    // 2026-06-15: Uniform SixBack-Branding — JEDES BMX-Service-Icon (alle Quellen:
    // TuneIn, Custom Stations, ...) zeigt die SixBack-Wortmarke statt eines
    // fremden Marken-Logos (TM-sauber + brandet den lokalen Cloud-Ersatz am
    // Geraete-Display). PNG fuers ST20/30-Panel, SVG fuer die App; beides im
    // LittleFS unter /bmx-icons/sixback.{png,svg}. Fallback: 1x1-transparent
    // (kein 404-Spam), falls das Asset fehlt.
    bool svg = req->url().endsWith(".svg");
    const char* fsPath = svg ? "/bmx-icons/sixback.svg" : "/bmx-icons/sixback.png";
    if (LittleFS.exists(fsPath)) {
        req->send(LittleFS, fsPath, svg ? "image/svg+xml" : "image/png");
        return;
    }
    AsyncWebServerResponse* resp = req->beginResponse_P(
        200, "image/png", kPng1x1Transparent, sizeof(kPng1x1Transparent));
    resp->addHeader("Cache-Control", "public, max-age=86400");
    req->send(resp);
}

void handleProviderSettings(AsyncWebServerRequest* req) {
    // Bosman-Schema (proxy-capture-verified 2026-05-21):
    //   Content-Type: application/vnd.bose.streaming-v1.2+xml
    //   Body: <?xml version="1.0" standalone="yes"?><providerSettings/>
    //   Headers: Method_name: getProviderSettings, Etag: <stable-hash>
    String body = kXmlDeclBosman;
    body += "<providerSettings/>";
    AsyncWebServerResponse* resp = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", body);
    resp->addHeader("Method_name", "getProviderSettings");
    resp->addHeader("Etag", etagFor_("providerSettings"));
    req->send(resp);
}

// -----------------------------------------------------------------------------
// Preset-Endpoints: Speaker fragt nach Account-/Device-Presets
// -----------------------------------------------------------------------------
void handleAccountPresets(AsyncWebServerRequest* req) {
    // Legacy /streaming/account/<a>/presets — alte Bose-Cloud-Variante ohne
    // device-disambiguation im Pfad. Bug 2026-05-22: returnte First-Match
    // ueber alle Speaker, sodass bei Multi-Device-Account jeder Speaker die
    // Presets des ERSTEN Inventory-Eintrags bekam (Emma).
    //
    // Fix: anfragenden Speaker per Remote-IP identifizieren. Speaker baut
    // Cloud-HTTP von seiner eigenen IP auf, also matcht Inventory.findByIp_.
    String remoteIp = req->client() ? req->client()->remoteIP().toString() : String();
    String matchedDevId;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findByIp(remoteIp);
        if (sp) matchedDevId = sp->deviceId;
    }
    if (matchedDevId.isEmpty()) {
        Serial.printf("[bmx][safe] /presets account-level -> 404 "
                      "(kein Speaker mit ip=%s im Inventory)\n", remoteIp.c_str());
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    if (!sixback::PresetStore::instance().hasAnyFor(matchedDevId)) {
        Serial.printf("[bmx][safe] /presets %s -> 404 (store empty)\n",
                      matchedDevId.c_str());
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    // Gap-Marker: Slice evtl. lueckenhaft geladen -> 404 + HW-Merge
    // (symmetrisch zu handleAccountFull, 2. Verlustpfad 2026-08-07).
    if (sixback::PresetStore::instance().gapSuspect(matchedDevId)) {
        Serial.printf("[bmx][safe] /presets %s -> 404 (gap-marker — HW-Merge angestossen)\n",
                      matchedDevId.c_str());
        spawnGapHeal_(matchedDevId);
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    Serial.printf("[bmx] /presets account-level -> per-device-fix for %s (ip=%s)\n",
                  matchedDevId.c_str(), remoteIp.c_str());
    String body = sixback::PresetStore::instance().toBoseXml(matchedDevId);
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleDevicePresets(AsyncWebServerRequest* req) {
    String acct  = pathParam(0);
    String devId = pathParam(1);
    // Schutz gegen Preset-Verlust nach erase: wenn unser Store fuer das Device
    // noch leer ist (z.B. Mock-Server frisch geflasht, Auto-Mode hat noch
    // nicht via import-from-device gesynct), liefern wir 404 statt
    // <presets/>. Der Speaker behaelt damit seinen Local-Cache. WICHTIG:
    // dieser Endpoint ist die Schluessel-Sicherung gegen den Phase-2-Loss
    // den wir am 2026-05-19 beobachtet haben.
    if (!sixback::PresetStore::instance().hasAnyFor(devId)) {
        Serial.printf("[bmx][safe] /presets %s -> 404 (store empty, Speaker behaelt Cache)\n",
                      devId.c_str());
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    // Gap-Marker: Slice evtl. lueckenhaft geladen -> 404 + HW-Merge
    // (symmetrisch zu handleAccountFull, 2. Verlustpfad 2026-08-07).
    if (sixback::PresetStore::instance().gapSuspect(devId)) {
        Serial.printf("[bmx][safe] /presets %s -> 404 (gap-marker — HW-Merge angestossen)\n",
                      devId.c_str());
        spawnGapHeal_(devId);
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    String body  = sixback::PresetStore::instance().toBoseXml(devId);
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

// -----------------------------------------------------------------------------
// /bmx/tunein/* — Station-Resolver, Token-Mock, Report-Sink
// -----------------------------------------------------------------------------
void handleTuneInToken(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"access_token\":\"\",\"refresh_token\":\"\"}");
}

void handleTuneInStation(AsyncWebServerRequest* req) {
    String id = pathParam(0);
    String json = resolveTuneInStation(id);
    req->send(200, "application/json", json);
}

void handleTuneInReport(AsyncWebServerRequest* req)  { send200(req); }
void handleRecent(AsyncWebServerRequest* req)        { req->send(201, "text/plain", ""); }

// -----------------------------------------------------------------------------
// GET /v1/blacklist/{deviceId}   -- Bose-Device-Blacklist-Check (Post-Pair-Boot)
//   Per ueberboese-Spec: Speaker fragt das beim Boot/Re-Sync nach Pairing.
//   Erwartet 404. Bose-Cloud nutzte das vermutlich um gestohlene/gesperrte
//   Geraete-IDs auszusperren. Wir haben keine Blacklist -> alle Devices "OK".
// -----------------------------------------------------------------------------
void handleBlacklist(AsyncWebServerRequest* req) {
    req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
}

// -----------------------------------------------------------------------------
// P7 — Defensive Speaker-aktive Gap-Filler-Stubs (ueberboese-Spec).
//   Aktuell rufen Greta/Emma/Kueche keinen dieser Endpoints aktiv an
//   (unknown-requests count=0), aber falls einer im Lifecycle doch kommt,
//   liefern wir ein spec-konformes Minimum statt 404 (was nochmal kostet
//   und im Log untergehen koennte).
// -----------------------------------------------------------------------------

// GET /core02/svc-bmx-adapter-orion/prod/orion/station?data=<b64>
//   BMX-ORION-Adapter ("Custom Stations" = bmx_service hinter
//   LOCAL_INTERNET_RADIO, provider 11). Loest LIR/ORION-Presets auf: die
//   ContentItem-location ist /station?data=<base64-json {streamUrl,name,
//   imageUrl[,isRealtime,streamType]}>; wir spiegeln das im Bose-BMX-Envelope
//   zurueck (Form wie der Upstream-svc-bmx-adapter-orion). Damit spielt der
//   Speaker den Stream nativ — kein TUNEIN-Tunnel noetig. (Empirisch belegt
//   2026-06-03 auf Emma: ohne dieses Envelope (alter Leer-Stub ohne "audio"-
//   Wrapper) blieb LIR auf STANDBY = der vermeintliche "LIR-Play-Fail".)
void handleOrionStation(AsyncWebServerRequest* req) {
    String streamUrl, name, imageUrl, streamType = "liveRadio";
    bool isRealtime = true;
    String dataParam = req->hasParam("data") ? req->getParam("data")->value() : "";
    if (dataParam.length() > 0) {
        dataParam.replace('-', '+'); dataParam.replace('_', '/');   // url-safe -> std
        size_t outLen = 0;
        std::vector<unsigned char> buf(((dataParam.length() / 4) + 1) * 3 + 8);
        if (mbedtls_base64_decode(buf.data(), buf.size(), &outLen,
                (const unsigned char*)dataParam.c_str(), dataParam.length()) == 0 && outLen) {
            JsonDocument d;
            if (!deserializeJson(d, buf.data(), outLen)) {
                streamUrl = (const char*)(d["streamUrl"] | "");
                name      = (const char*)(d["name"]      | "");
                imageUrl  = (const char*)(d["imageUrl"]  | "");
                if (d["streamType"].is<const char*>()) streamType = (const char*)d["streamType"];
                if (d["isRealtime"].is<bool>())        isRealtime = d["isRealtime"].as<bool>();
            }
        }
    }
    JsonDocument out;
    JsonObject a = out["audio"].to<JsonObject>();
    a["isRealtime"]  = isRealtime;
    a["hasPlaylist"] = false;
    a["streamUrl"]   = streamUrl;       // leer falls data fehlt/ungueltig -> Speaker STANDBY
    out["imageUrl"]  = imageUrl;
    out["name"]      = name;
    out["streamType"] = streamType;
    String body; serializeJson(out, body);
    req->send(200, "application/json", body);
}

// GET /streaming/software/update/account/{aid}
//   Account-spezifischer Update-Check. Leere softwareUpdateLocation = no update.
void handleSwUpdateAccount(AsyncWebServerRequest* req) {
    req->send(200, "application/vnd.bose.streaming-v1.2+xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<software_update><softwareUpdateLocation></softwareUpdateLocation>"
              "</software_update>");
}

// GET /streaming/device/{did}/streaming_token
//   "experimental"-Endpoint — liefert Auth-Token im Header, Body leer.
void handleStreamingToken(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r = req->beginResponse(200, "text/plain", "");
    r->addHeader("Authorization", "bf32-streaming-token");
    req->send(r);
}

// POST /streaming/support/customersupport
//   CS-Diagnostics-Upload — Speaker meldet Probleme rein, wir aknowledgen nur.
void handleCustomerSupport(AsyncWebServerRequest* req) {
    Serial.printf("[bmx] customer-support upload clen=%d\n", (int)req->contentLength());
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", "");
}

// GET /streaming/account/{a}/device/{d}/recents
//   Liste der zuletzt gespielten Inhalte. SixBack fuehrt keine Recent-Liste
//   (Speaker macht das selber). Empty <recents/>.
void handleAccountRecents(AsyncWebServerRequest* req) {
    req->send(200, "application/vnd.bose.streaming-v1.2+xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><recents/>");
}

// GET /streaming/account/{a}/device/{d}/recent/{rid}
//   Einzelner Recent — wir haben keine -> spec-konformes 404-Body.
void handleAccountRecentSingle(AsyncWebServerRequest* req) {
    req->send(404, "application/vnd.bose.streaming-v1.2+xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<status><message>Not found</message><status-code>404</status-code></status>");
}

// GET /streaming/account/{a}/device/{d}/preset/{n}
//   Single-Preset-Pull. SixBack liefert Presets ueber /streaming/account/
//   {a}/device/{d}/presets als Liste; per-Slot ist redundant. 404.
void handleAccountPresetSingle(AsyncWebServerRequest* req) {
    req->send(404, "application/vnd.bose.streaming-v1.2+xml",
              "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<status><message>Not found</message><status-code>404</status-code></status>");
}

// -----------------------------------------------------------------------------
// P5 — Stereo-Pairing / Multi-Room-Group-Store
//   Endpoints per ueberboese-Spec:
//     POST   /streaming/account/{a}/group/             -> 201 + <group id=...>
//     PUT    /streaming/account/{a}/group/{gid}        -> 200 + <group id=...>
//     DELETE /streaming/account/{a}/group/{gid}        -> 200 empty
//     GET    /streaming/account/{a}/device/{d}/group/  -> 200 + <group/> oder
//                                                              <group id=...>
//   In-memory + NVS-Persistenz (Preferences-Namespace "bfx_groups", key "list").
// -----------------------------------------------------------------------------
struct GroupRole { String deviceId; String role; };  // role = LEFT/RIGHT/...
struct Group {
    String id;
    String masterDeviceId;
    String name;
    std::vector<GroupRole> roles;
};

std::vector<Group> g_groups;
SemaphoreHandle_t  g_groups_mtx = nullptr;
uint32_t           g_next_group_id = 1000000;

// Health-Flag: false = letzter bfx_groups-Save scheiterte (NVS voll trotz
// Cleanup) → Stereo-Paar NICHT reboot-persistent. Der Speaker (BMX-Client)
// bekommt trotzdem 201/200 (Gruppe lebt in g_groups/RAM diese Session) — das
// ehrliche Signal geht an Operator/WebUI via /api/status.groups_persist_ok,
// NICHT als 500 an die eingefrorene FW (BMX-Contract).
bool g_groupsPersistOk = true;

bool groupsSave_() {
    JsonDocument doc;
    doc["next_id"] = g_next_group_id;
    JsonArray arr  = doc["groups"].to<JsonArray>();
    for (const auto& g : g_groups) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]     = g.id;
        o["master"] = g.masterDeviceId;
        o["name"]   = g.name;
        JsonArray rs = o["roles"].to<JsonArray>();
        for (const auto& r : g.roles) {
            JsonObject ro = rs.add<JsonObject>();
            ro["did"]  = r.deviceId;
            ro["role"] = r.role;
        }
    }
    // Cleanup-Variante wie die anderen User-Settings-Stores: bei vollem NVS erst
    // regenerable Caches purgen + retry statt Silent-Fail (sonst geht die Stereo-
    // Paar-Persistenz ueber Reboot still verloren).
    bool ok = sixback::nvsSaveJsonWithCleanup("bfx_groups", "list", doc);
    g_groupsPersistOk = ok;
    if (!ok) {
        // KEIN 500 an den Speaker (BMX-Contract) — die Gruppe funktioniert diese
        // Session im RAM, die FW erwartet hier 201/200. Ehrliches Signal geht an
        // Operator/WebUI: lautes Serial + Health-Flag in /api/status.
        Serial.println("[group] PERSIST FAIL — Stereo-Paar NICHT reboot-persistent "
                       "(NVS voll trotz Cleanup); Speaker erhaelt weiter 201/200, "
                       "Gruppe lebt in RAM. -> /api/status groups_persist_ok=false");
    }
    return ok;
}

void groupsInit_() {
    if (!g_groups_mtx) g_groups_mtx = xSemaphoreCreateMutex();
    JsonDocument doc;
    if (!sixback::nvsLoadJson("bfx_groups", "list", doc)) return;
    if (doc["next_id"].is<uint32_t>()) g_next_group_id = doc["next_id"];
    JsonArrayConst arr = doc["groups"].as<JsonArrayConst>();
    for (JsonVariantConst v : arr) {
        Group g;
        g.id             = v["id"]     | "";
        g.masterDeviceId = v["master"] | "";
        g.name           = v["name"]   | "";
        JsonArrayConst rs = v["roles"].as<JsonArrayConst>();
        for (JsonVariantConst r : rs) {
            GroupRole rr;
            rr.deviceId = r["did"]  | "";
            rr.role     = r["role"] | "";
            if (rr.deviceId.length()) g.roles.push_back(rr);
        }
        g_groups.push_back(g);
    }
    Serial.printf("[group] NVS: %d groups loaded, next_id=%u\n",
                  (int)g_groups.size(), (unsigned)g_next_group_id);
}

String xmlTagText_(const String& xml, const char* tag) {
    String openTag = "<"; openTag += tag; openTag += ">";
    String closeTag = "</"; closeTag += tag; closeTag += ">";
    int p = xml.indexOf(openTag);
    if (p < 0) return "";
    int s = p + (int)openTag.length();
    int e = xml.indexOf(closeTag, s);
    if (e < 0) return "";
    return xml.substring(s, e);
}

String groupToXml_(const Group& g) {
    String x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    x += "<group id=\""; x += g.id; x += "\">";
    x += "<masterDeviceId>"; x += g.masterDeviceId; x += "</masterDeviceId>";
    x += "<name>"; x += g.name; x += "</name>";
    x += "<roles>";
    for (const auto& r : g.roles) {
        x += "<groupRole>";
        x += "<deviceId>"; x += r.deviceId; x += "</deviceId>";
        x += "<role>";     x += r.role;     x += "</role>";
        x += "</groupRole>";
    }
    x += "</roles>";
    x += "</group>";
    return x;
}

void parseRoles_(const String& xml, std::vector<GroupRole>& out) {
    String rolesXml = xmlTagText_(xml, "roles");
    if (rolesXml.length() == 0) return;
    int pos = 0;
    while (true) {
        int p = rolesXml.indexOf("<groupRole>", pos);
        if (p < 0) break;
        int e = rolesXml.indexOf("</groupRole>", p);
        if (e < 0) break;
        String body = rolesXml.substring(p, e + 12);
        GroupRole r;
        r.deviceId = xmlTagText_(body, "deviceId");
        r.role     = xmlTagText_(body, "role");
        if (r.deviceId.length() > 0) out.push_back(r);
        pos = e + 12;
    }
}

// Per-Request Body-Buffer fuer POST/PUT (deviceadd-Pattern wiederverwendet).
struct GroupBodyCtx { String body; };
std::map<void*, GroupBodyCtx> g_groupBodyCtx;
SemaphoreHandle_t              g_groupBody_mtx = nullptr;

void handleGroupBody(AsyncWebServerRequest* req,
                     uint8_t* data, size_t len, size_t index, size_t total) {
    if (!g_groupBody_mtx) g_groupBody_mtx = xSemaphoreCreateMutex();
    if (xSemaphoreTake(g_groupBody_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    GroupBodyCtx& ctx = g_groupBodyCtx[req];
    if (index == 0 && total > 0 && total < 4096) ctx.body.reserve(total);
    for (size_t i = 0; i < len; ++i) ctx.body += (char)data[i];
    xSemaphoreGive(g_groupBody_mtx);
}

String takeGroupBody_(AsyncWebServerRequest* req) {
    String body;
    if (g_groupBody_mtx && xSemaphoreTake(g_groupBody_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = g_groupBodyCtx.find(req);
        if (it != g_groupBodyCtx.end()) {
            body = it->second.body;
            g_groupBodyCtx.erase(it);
        }
        xSemaphoreGive(g_groupBody_mtx);
    }
    return body;
}

// GET /streaming/account/{a}/device/{d}/group/
void handleDeviceGroup(AsyncWebServerRequest* req) {
    String did = pathParam(1);
    String resp;
    if (g_groups_mtx && xSemaphoreTake(g_groups_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (const auto& g : g_groups) {
            bool hit = (g.masterDeviceId == did);
            if (!hit) {
                for (const auto& r : g.roles) if (r.deviceId == did) { hit = true; break; }
            }
            if (hit) { resp = groupToXml_(g); break; }
        }
        xSemaphoreGive(g_groups_mtx);
    }
    if (resp.length() == 0) {
        // Bosman: <?xml version="1.0" encoding="UTF-8"?><group/>
        resp = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><group/>";
    }
    Serial.printf("[group] GET device=%s -> %s\n",
                  did.c_str(), resp.indexOf("id=") > 0 ? "group" : "empty");
    AsyncWebServerResponse* r = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", resp);
    r->addHeader("Method_name", "getGroup");
    r->addHeader("Etag", etagFor_(String("group:") + did));
    req->send(r);
}

// POST /streaming/account/{a}/group/  -> 201 Created
void handleGroupCreate(AsyncWebServerRequest* req) {
    String body = takeGroupBody_(req);
    Group g;
    g.id             = String(g_next_group_id);
    g.masterDeviceId = xmlTagText_(body, "masterDeviceId");
    g.name           = xmlTagText_(body, "name");
    parseRoles_(body, g.roles);

    if (g.masterDeviceId.length() == 0) {
        Serial.println("[group] CREATE rejected: empty masterDeviceId");
        req->send(400, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }

    if (g_groups_mtx && xSemaphoreTake(g_groups_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Prune (2026-06-07, #22): die Bose-App loescht beim Stereo-Pair-
        // Trennen den Cloud-Eintrag NICHT (Feld-Report vavolio-dev); nur der
        // direkte removeGroup-Pfad raeumt auf (on-device verifiziert). Ein
        // neues Pair mit denselben Geraeten ERSETZT deshalb alle alten
        // Eintraege, die ein Mitglied teilen — sonst akkumuliert der Store
        // bei haeufigem Pair/Unpair unbegrenzt im NVS.
        size_t pruned = 0;
        for (size_t i = g_groups.size(); i-- > 0; ) {
            const Group& old = g_groups[i];
            bool shared = (old.masterDeviceId == g.masterDeviceId);
            for (const auto& orole : old.roles) {
                if (shared) break;
                if (orole.deviceId == g.masterDeviceId) { shared = true; break; }
                for (const auto& nrole : g.roles) {
                    if (orole.deviceId == nrole.deviceId) { shared = true; break; }
                }
            }
            if (shared) { g_groups.erase(g_groups.begin() + i); ++pruned; }
        }
        if (pruned) {
            Serial.printf("[group] CREATE pruned %u stale group(s) sharing members\n",
                          (unsigned)pruned);
        }
        g_groups.push_back(g);
        g_next_group_id++;
        groupsSave_();
        xSemaphoreGive(g_groups_mtx);
    }
    String resp = groupToXml_(g);
    Serial.printf("[group] CREATE id=%s master=%s roles=%d\n",
                  g.id.c_str(), g.masterDeviceId.c_str(), (int)g.roles.size());
    req->send(201, "application/vnd.bose.streaming-v1.2+xml", resp);
}

// PUT /streaming/account/{a}/group/{gid}  -> 200 OK
void handleGroupUpdate(AsyncWebServerRequest* req) {
    String gid  = pathParam(1);
    String body = takeGroupBody_(req);
    String resp;
    bool   found = false;
    if (g_groups_mtx && xSemaphoreTake(g_groups_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto& g : g_groups) {
            if (g.id != gid) continue;
            found = true;
            String m = xmlTagText_(body, "masterDeviceId");
            String n = xmlTagText_(body, "name");
            if (m.length()) g.masterDeviceId = m;
            if (n.length()) g.name           = n;
            std::vector<GroupRole> newRoles;
            parseRoles_(body, newRoles);
            if (!newRoles.empty()) g.roles = newRoles;
            groupsSave_();
            resp = groupToXml_(g);
            break;
        }
        xSemaphoreGive(g_groups_mtx);
    }
    Serial.printf("[group] PUT id=%s -> %s\n", gid.c_str(), found ? "200" : "404");
    if (!found) {
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", resp);
}

// DELETE /streaming/account/{a}/group/{gid}  -> 200 OK empty body
void handleGroupDelete(AsyncWebServerRequest* req) {
    String gid = pathParam(1);
    bool   removed = false;
    if (g_groups_mtx && xSemaphoreTake(g_groups_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto it = g_groups.begin(); it != g_groups.end(); ++it) {
            if (it->id == gid) {
                g_groups.erase(it);
                groupsSave_();
                removed = true;
                break;
            }
        }
        xSemaphoreGive(g_groups_mtx);
    }
    Serial.printf("[group] DELETE id=%s -> %s\n", gid.c_str(), removed ? "200" : "404");
    req->send(removed ? 200 : 404, "application/vnd.bose.streaming-v1.2+xml", "");
}

// -----------------------------------------------------------------------------
// POST /streaming/account/{accountId}/device/   -- Marge-Association Bootstrap
//
// Wird vom Speaker selbst getriggert sobald jemand POST /setMargeAccount
// (mit <PairDeviceWithAccount>) am Speaker-Port 8090 macht. Speaker dreht
// das in einen Round-Trip zu unserem Cloud-Mock auf diesen Endpoint:
//   POST /streaming/account/<aid>/device/
//   Content-Type: application/vnd.bose.streaming-v1.2+xml
//   Body: <device deviceid="..."><name>...</name>...</device>
//
// Antwort muss 201 + Credentials-Header (Bearer-Token) liefern. Den
// Token persistiert der Speaker und schickt ihn fortan als Bearer-
// Auth bei /v1/scmudc/... mit. **Das ist DER Marge-Association-
// Handshake** — ohne ihn bleibt MargeClient in NotAssociated und
// queue't Events nur lokal.
//
// Wir geben einen synthetischen Token zurueck. Greta+Co. verifizieren
// nichts gegen Bose-Cloud (die ja tot ist) — Token nur lokal verwendet.
// -----------------------------------------------------------------------------

// In-flight: pro Request-Pointer cachen wir den im Body extrahierten
// deviceId. Body-Callback fuellt, Finalize liest. Mutex-protected weil
// async_tcp-Task verschiedenste Verbindungen parallel haendeln kann.
struct AddDeviceCtx { String deviceId; uint32_t added_ms; };
std::map<void*, AddDeviceCtx> g_addDevCtx;
SemaphoreHandle_t g_addDevMtx = nullptr;

void handleDeviceAddBody(AsyncWebServerRequest* req,
                          uint8_t* data, size_t len, size_t index, size_t total) {
    if (index == 0) {
        Serial.printf("[marge] addDevice body start total=%d\n", (int)total);
    }
    // deviceid aus 'deviceid="XXXX"' parsen (Speaker sendet das immer auf der
    // ersten Seite des Bodys; total<=200 typisch).
    if (index == 0 && len > 0) {
        String body;
        size_t n = (len < 256) ? len : 256;
        for (size_t i = 0; i < n; ++i) body += (char)data[i];
        int p = body.indexOf("deviceid=\"");
        if (p >= 0) {
            int e = body.indexOf("\"", p + 10);
            if (e > p) {
                String did = body.substring(p + 10, e);
                if (!g_addDevMtx) g_addDevMtx = xSemaphoreCreateMutex();
                if (xSemaphoreTake(g_addDevMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_addDevCtx[req] = {did, millis()};
                    xSemaphoreGive(g_addDevMtx);
                }
                Serial.printf("[marge] addDevice deviceid=%s\n", did.c_str());
            }
        }
    }
}

// -----------------------------------------------------------------------------
// POST /streaming/account/{aid}/source  --  Speaker registriert einen
// "MusicService Account" (DLNA-MediaServer, Spotify-User-Account, etc.).
// Empirisch entdeckt 2026-05-21 via Capture: Speaker schickt nach
// `BMX-POST /setMusicServiceAccount` (von SoundTouchPlus/Bosman) einen
// Cloud-Roundtrip mit Bearer-Token + diesem Body:
//   <source><credential></credential><username>UUID/0</username>
//   <sourceproviderid>INVALID_SOURCE</sourceproviderid>
//   <sourcename>display-name</sourcename></source>
// Cloud muss 200 OK mit <source>-XML zurueck (id + sourceproviderid auf 7
// fuer STORED_MUSIC). Danach setzt der Speaker intern <sourceItem
// sourceAccount="UUID/0" status="READY"> — und /select klappt.
struct AddSourceCtx { String accountId; String body; };
std::map<void*, AddSourceCtx> g_addSrcCtx;
SemaphoreHandle_t g_addSrcMtx = nullptr;

void handleAccountSourceAddBody(AsyncWebServerRequest* req,
                                 uint8_t* data, size_t len, size_t index, size_t total) {
    if (!g_addSrcMtx) g_addSrcMtx = xSemaphoreCreateMutex();
    if (xSemaphoreTake(g_addSrcMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    auto& ctx = g_addSrcCtx[req];
    if (index == 0) {
        ctx.accountId = pathParam(0);
        ctx.body = "";
    }
    for (size_t i = 0; i < len; ++i) ctx.body += (char)data[i];
    xSemaphoreGive(g_addSrcMtx);
}

void handleAccountSourceAddFinalize(AsyncWebServerRequest* req) {
    String acct, body;
    if (g_addSrcMtx && xSemaphoreTake(g_addSrcMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = g_addSrcCtx.find(req);
        if (it != g_addSrcCtx.end()) {
            acct = it->second.accountId;
            body = it->second.body;
            g_addSrcCtx.erase(it);
        }
        xSemaphoreGive(g_addSrcMtx);
    }
    // Body parsen: <username>UUID/0</username>, <sourcename>X</sourcename>
    String userAcc, srcName;
    int p1 = body.indexOf("<username>");
    if (p1 >= 0) {
        int e = body.indexOf("</username>", p1);
        if (e > p1) userAcc = body.substring(p1 + 10, e);
    }
    int p2 = body.indexOf("<sourcename>");
    if (p2 >= 0) {
        int e = body.indexOf("</sourcename>", p2);
        if (e > p2) srcName = body.substring(p2 + 12, e);
    }
    // Eindeutige Source-ID aus UUID-Hash (deterministisch ueber reboots).
    uint32_t srcId = 25000000;
    for (size_t i = 0; i < userAcc.length(); ++i) srcId = srcId * 31 + (uint8_t)userAcc[i];
    srcId = 25000000 + (srcId % 1000000);
    // soundcork main.py configured_source_xml (Zeile 218-241):
    //   <sourcename> = display_name aus Request (z.B. "host: minidlna"),
    //   NICHT hardcoded "Mediaserver". <name> = source_key_account (= username
    //   = UUID/0). sourceproviderid wird aus PROVIDERS-index berechnet — fuer
    //   STORED_MUSIC ist das 7.
    String resp = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<source id=\""; resp += String(srcId); resp += "\" type=\"Audio\">"
                    "<createdOn>"; resp += kFakeTs; resp += "</createdOn>"
                    "<credential type=\"token\"></credential>"
                    "<name>"; resp += xmlEsc_(userAcc); resp += "</name>"
                    "<sourceproviderid>7</sourceproviderid>"
                    "<sourcename>"; resp += xmlEsc_(srcName); resp += "</sourcename>"
                    "<sourceSettings/>"
                    "<updatedOn>"; resp += kFakeTs; resp += "</updatedOn>"
                    "<username>"; resp += xmlEsc_(userAcc); resp += "</username>"
                  "</source>";
    // Ground-truth aus deborahgu/soundcork main.py:630 — `post_account_source`:
    //   status_code=HTTPStatus.CREATED  (= 201)
    //   extra_headers={"method_name": "addSource"}
    //   ETag-Header (etag_for_account, weak=False) — soundcork-Endpoint
    //   serializiert den account-state-checksum hier; ohne den verwirft
    //   Bose_Lisa die Antwort vermutlich als "stale".
    AsyncWebServerResponse* r = req->beginResponse(
        201, "application/vnd.bose.streaming-v1.2+xml", resp);
    r->addHeader("METHOD_NAME", "addSource");
    // ETag = deterministisch aus account + src-id (analog soundcork mtime-basiert).
    String etag = "\""; etag += acct; etag += "-"; etag += String(srcId); etag += "\"";
    r->addHeader("ETag", etag);
    req->send(r);
    Serial.printf("[marge] addAccountSource acct=%s user=%s name=%s -> 200 id=%u\n",
                  acct.c_str(), userAcc.c_str(), srcName.c_str(), (unsigned)srcId);
}

void handleDeviceAddFinalize(AsyncWebServerRequest* req) {
    String acct = pathParam(0);
    String devId;
    if (g_addDevMtx && xSemaphoreTake(g_addDevMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = g_addDevCtx.find(req);
        if (it != g_addDevCtx.end()) {
            devId = it->second.deviceId;
            g_addDevCtx.erase(it);
        }
        xSemaphoreGive(g_addDevMtx);
    }
    String body = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<device deviceid=\""; body += devId; body += "\">"
                    "<createdOn>2020-01-01T00:00:00.000+00:00</createdOn>"
                    "<ipaddress></ipaddress>"
                    "<name></name>"
                    "<updatedOn>2020-01-01T00:00:00.000+00:00</updatedOn>"
                  "</device>";
    AsyncWebServerResponse* resp = req->beginResponse(
        201, "application/vnd.bose.streaming-v1.2+xml", body);
    resp->addHeader("Credentials", "Bearer bf32-marge-token");
    String selfBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    resp->addHeader("Location",
                    selfBase + "/streaming/account/" + acct + "/device/" + devId);
    resp->addHeader("METHOD_NAME", "addDevice");
    req->send(resp);
    Serial.printf("[marge] addDevice acct=%s dev=%s -> 201 Bearer\n",
                  acct.c_str(), devId.c_str());
}

// -----------------------------------------------------------------------------
// PUT /streaming/account/{accountId}/device/{deviceId}  -- Rename (account-seitig)
//
// Ein marge-/account-gebundener (migrierter) Speaker wendet einen Rename NICHT
// lokal an: er feuert diesen PUT SELBST an seine margeURL (= uns), egal ob der
// User in der Bose-App oder via lokalem POST :8090/name umbenennt. Ohne Handler
// -> onNotFound/404 -> Speaker retry-loopt, Rename committed nie, /info bleibt
// auf dem alten Namen (HW-bewiesen 2026-06-10 an Emma: POST :8090/name -> Emma
// feuert PUT .../device/EC24B89C26AD, mehrfach = Retry; vgl. gesellix/AfterTouch
// issue285 — nur-POST-registriert ergab dort 502 + endlosen Bose-App-Spinner).
//   Content-Type: application/vnd.bose.streaming-v1.2+xml
//   Body: <device deviceid="DEVID"><name>NEU</name><macaddress>MAC</macaddress></device>
//   200:  <device deviceid><createdOn><ipaddress><name>NEU</name><updatedOn></device>
//   deviceid URL != Body -> 400 (mirror AfterTouch/ueberboese updateDevice).
// account/full serviert <name> bereits (s.o.), daher genuegt der Inventory-Update.
struct PutDeviceCtx { String body; };
std::map<void*, PutDeviceCtx> g_putDevCtx;
SemaphoreHandle_t g_putDevMtx = nullptr;

void handlePutDeviceBody(AsyncWebServerRequest* req,
                         uint8_t* data, size_t len, size_t index, size_t total) {
    if (!g_putDevMtx) g_putDevMtx = xSemaphoreCreateMutex();
    if (xSemaphoreTake(g_putDevMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    auto& ctx = g_putDevCtx[req];
    if (index == 0) ctx.body = "";
    for (size_t i = 0; i < len; ++i) ctx.body += (char)data[i];
    xSemaphoreGive(g_putDevMtx);
}

void handlePutDeviceFinalize(AsyncWebServerRequest* req) {
    String acct     = pathParam(0);
    String urlDevId = pathParam(1);
    String body;
    if (g_putDevMtx && xSemaphoreTake(g_putDevMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = g_putDevCtx.find(req);
        if (it != g_putDevCtx.end()) { body = it->second.body; g_putDevCtx.erase(it); }
        xSemaphoreGive(g_putDevMtx);
    }
    // Body parsen: deviceid="..."-Attr + <name>...</name>
    String bodyDevId, newName;
    int dp = body.indexOf("deviceid=\"");
    if (dp >= 0) { int e = body.indexOf("\"", dp + 10); if (e > dp) bodyDevId = body.substring(dp + 10, e); }
    int np = body.indexOf("<name>");
    if (np >= 0) { int e = body.indexOf("</name>", np); if (e > np) newName = body.substring(np + 6, e); }
    newName.trim();
    // deviceid-Mismatch URL vs Body -> 400 (wie AfterTouch HandleMargeUpdateDevice)
    if (bodyDevId.length() && urlDevId.length() && bodyDevId != urlDevId) {
        req->send(400, "application/vnd.bose.streaming-v1.2+xml", "");
        Serial.printf("[marge] putDevice deviceid mismatch url=%s body=%s -> 400\n",
                      urlDevId.c_str(), bodyDevId.c_str());
        return;
    }
    String devId = urlDevId.length() ? urlDevId : bodyDevId;
    // Inventory-Name aktualisieren. Speaker unbekannt -> trotzdem 200 echoen
    // (Speaker erwartet 200, sonst Retry-Loop); Name synct dann via account/full.
    String ip, appliedName = newName;
    bool found = false, changed = false;
    {
        auto& inv = sixback::SpeakerInventory::instance();
        sixback::SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(devId);
        if (sp) {
            found = true;
            ip = sp->ip;
            if (newName.length() && sp->name != newName) { sp->name = newName; changed = true; }
            if (newName.length() == 0) appliedName = sp->name;
        }
    }
    // saveToNVS() lockt selbst -> AUSSERHALB des LockGuard-Scopes rufen.
    if (changed) sixback::SpeakerInventory::instance().saveToNVS();
    String resp = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<device deviceid=\""; resp += xmlEsc_(devId); resp += "\">"
                    "<createdOn>"; resp += kBosmanInitTs; resp += "</createdOn>"
                    "<ipaddress>"; resp += xmlEsc_(ip); resp += "</ipaddress>"
                    "<name>"; resp += xmlEsc_(appliedName); resp += "</name>"
                    "<updatedOn>"; resp += kBosmanInitTs; resp += "</updatedOn>"
                  "</device>";
    AsyncWebServerResponse* r = req->beginResponse(
        200, "application/vnd.bose.streaming-v1.2+xml", resp);
    r->addHeader("METHOD_NAME", "updateDevice");
    req->send(r);
    Serial.printf("[marge] putDevice acct=%s dev=%s name=\"%s\" found=%d changed=%d -> 200\n",
                  acct.c_str(), devId.c_str(), appliedName.c_str(), (int)found, (int)changed);
}

// -----------------------------------------------------------------------------
// GET /updates/soundtouch  -- Replica der Bose-Firmware-Index-Liste
// -----------------------------------------------------------------------------
void handleSWUpdateIndex(AsyncWebServerRequest* req) {
    const char* body =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<INDEX REVISION=\"02.11.00\">\n"
"  <DEVICE ID=\"0x0939\" PRODUCTNAME=\"SoundTouch 10\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x093B\" PRODUCTNAME=\"SoundTouch 20\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x093C\" PRODUCTNAME=\"SoundTouch 30\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x094A\" PRODUCTNAME=\"SoundTouch Wireless Link adapter\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"  <DEVICE ID=\"0x0949\" PRODUCTNAME=\"SoundTouch 300\">\n"
"    <HARDWARE REVISION=\"00.01.00\">\n"
"      <RELEASE REVISION=\"27.0.6.46330.5043500\"/>\n"
"    </HARDWARE>\n"
"  </DEVICE>\n"
"</INDEX>\n";
    req->send(200, "application/xml", body);
}

// -----------------------------------------------------------------------------
// Catch-all - logged in Serial + Ringbuffer, dann 404. Endpoint-Forensik
// (welche Paths fragt der Speaker an die wir noch nicht beantworten).
// Body wird NICHT erfasst (AsyncWebServer ohne registrierten Body-Handler
// liefert ihn nicht durch). Content-Length signalisiert ob Body vorhanden war.
// -----------------------------------------------------------------------------
void handleNotFound(AsyncWebServerRequest* req) {
    UnknownReq r;
    r.ts_ms          = millis();
    r.client_ip      = req->client() ? req->client()->remoteIP().toString() : String("?");
    r.method         = String(req->methodToString());
    r.host           = req->host();
    r.content_length = (int)req->contentLength();
    r.content_type   = req->contentType();

    // URL inklusive GET-Query-String (POST-Body-Params auslassen).
    String pathFull = req->url();
    String qs;
    for (size_t i = 0; i < req->params(); i++) {
        const AsyncWebParameter* p = req->getParam(i);
        if (!p || p->isPost()) continue;
        qs += (qs.length() ? "&" : "?");
        qs += p->name();
        qs += "=";
        qs += p->value();
    }
    pathFull += qs;
    r.path = pathFull;

    Serial.printf("[bmx][?] %s %s host=%s clen=%d ct=%s from=%s\n",
                  r.method.c_str(), r.path.c_str(), r.host.c_str(),
                  r.content_length, r.content_type.c_str(), r.client_ip.c_str());

    pushUnknown(r);

    req->send(404, "text/plain", "SixBack: endpoint not implemented");
}

} // anon

// Getter fuer /api/status (deklariert in bose_endpoints.h): letzter
// groupsSave_-Persist-Stand. Bewusst GLOBALE Linkage (der uebrige Group-Code
// liegt im anon-NS oben) — handleStatus in api_endpoints.cpp ruft es TU-
// uebergreifend. g_groupsPersistOk lebt im anon-NS, ist aber in derselben TU
// aus dem globalen Scope sichtbar.
bool groupsPersistOk() { return g_groupsPersistOk; }

// -----------------------------------------------------------------------------
// Catch-All-Logger Public API (P3) — fuer UI-Endpoint /api/unknown-requests.
// -----------------------------------------------------------------------------
void getUnknownRequestsJson(JsonArray out) {
    if (!g_unknown_mtx) return;
    if (xSemaphoreTake(g_unknown_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    // Aelteste zuerst.
    size_t start = (g_unknown_count < kUnknownRingSize) ? 0 : g_unknown_head;
    for (size_t i = 0; i < g_unknown_count; i++) {
        size_t idx = (start + i) % kUnknownRingSize;
        const UnknownReq& r = g_unknown[idx];
        JsonObject o = out.add<JsonObject>();
        o["ts_ms"]          = r.ts_ms;
        o["ip"]             = r.client_ip;
        o["method"]         = r.method;
        o["path"]           = r.path;
        o["host"]           = r.host;
        o["content_length"] = r.content_length;
        o["content_type"]   = r.content_type;
    }
    xSemaphoreGive(g_unknown_mtx);
}

void clearUnknownRequests() {
    if (!g_unknown_mtx) return;
    if (xSemaphoreTake(g_unknown_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_unknown_head  = 0;
    g_unknown_count = 0;
    xSemaphoreGive(g_unknown_mtx);
}

void registerBoseEndpoints(AsyncWebServer& server) {
    if (!g_unknown_mtx) g_unknown_mtx = xSemaphoreCreateMutex();
    groupsInit_();

    server.on("/",                                                    HTTP_GET,  handleBoseRoot);
    // silence.mp3 — lokal hosten statt von sixback.io. Bose-Speaker (FW 2021)
    // schaffen das Cloudflare-TLS-Cert nicht und reporten INVALID_SOURCE,
    // wodurch der long-press nach /select nichts zu speichern findet und der
    // Push-To-Speaker scheitert. ESP-LittleFS serviert die ~4.5KB MP3 selber.
    server.serveStatic("/silence.mp3", LittleFS, "/silence.mp3")
          .setCacheControl("max-age=3600");
    routeT(server, "^/v1/scmudc/([^/]+)$",                                 HTTP_POST,
              handleSCMUDCFinalize, nullptr, handleSCMUDCBody);
    server.on("/bmx/registry/v1/services",                            HTTP_GET,  handleBMXRegistry);
    server.on("/bmx/registry/v1/servicesAvailability",                HTTP_GET,  handleBMXServicesAvailability);
    server.on("/streaming/sourceproviders",                           HTTP_GET,  handleSourceProviders);
    routeT(server, "^/media/bmx-icons/.+$",                                HTTP_GET,  handleBmxIconStub);
    server.on("/streaming/support/power_on",                          HTTP_POST, handlePowerOn);
    routeT(server, "^/streaming/account/([^/]+)/full$",                    HTTP_GET,  handleAccountFull);
    routeT(server, "^/streaming/account/([^/]+)/sources$",                 HTTP_GET,  handleAccountSources);
    routeT(server, "^/streaming/account/([^/]+)/provider_settings$",       HTTP_GET,  handleProviderSettings);
    routeT(server, "^/streaming/account/([^/]+)/presets$",                 HTTP_GET,  handleAccountPresets);
    routeT(server, "^/streaming/account/([^/]+)/presets/all$",             HTTP_GET,  handleAccountPresets);
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/presets$",  HTTP_GET,  handleDevicePresets);
    server.on("/bmx/tunein/v1/token",                                 HTTP_POST, handleTuneInToken);
    routeT(server, "^/bmx/tunein/v1/playback/station/([^/]+)$",            HTTP_GET,  handleTuneInStation);
    server.on("/bmx/tunein/v1/report",                                HTTP_POST, handleTuneInReport);
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/recent$",   HTTP_POST, handleRecent);
    // Multi-Room-Group-Status — vom Speaker beim Post-Pair-Boot abgefragt:
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/group/$",   HTTP_GET,  handleDeviceGroup);
    // P5 — Stereo-Pairing / Multi-Room-Group-Lifecycle (POST/PUT/DELETE):
    routeT(server, "^/streaming/account/([^/]+)/group/$",                  HTTP_POST,
              handleGroupCreate, nullptr, handleGroupBody);
    routeT(server, "^/streaming/account/([^/]+)/group/([^/]+)$",           HTTP_PUT,
              handleGroupUpdate, nullptr, handleGroupBody);
    routeT(server, "^/streaming/account/([^/]+)/group/([^/]+)$",           HTTP_DELETE,
              handleGroupDelete);
    // Bose-Device-Blacklist-Check (immer 404 expected):
    routeT(server, "^/v1/blacklist/([^/]+)$",                              HTTP_GET,  handleBlacklist);
    // P7 — defensive Speaker-aktive Gap-Filler-Stubs:
    server.on("/core02/svc-bmx-adapter-orion/prod/orion/station",     HTTP_GET,  handleOrionStation);
    routeT(server, "^/streaming/software/update/account/([^/]+)$",         HTTP_GET,  handleSwUpdateAccount);
    routeT(server, "^/streaming/device/([^/]+)/streaming_token$",          HTTP_GET,  handleStreamingToken);
    server.on("/streaming/support/customersupport",                   HTTP_POST, handleCustomerSupport);
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/recents$",  HTTP_GET,  handleAccountRecents);
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/recent/([^/]+)$",
              HTTP_GET, handleAccountRecentSingle);
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)/preset/([1-6])$",
              HTTP_GET, handleAccountPresetSingle);
    // Marge-Association-Bootstrap (trailing slash ist Pflicht — Speaker schickt es so):
    routeT(server, "^/streaming/account/([^/]+)/device/$",                 HTTP_POST,
              handleDeviceAddFinalize, nullptr, handleDeviceAddBody);
    // Rename (account-seitig): Speaker feuert diesen PUT beim Umbenennen selbst
    // (Bose-App ODER lokales :8090/name). Ohne ihn -> 404 + Retry-Loop, Rename
    // committed nie. Anchored + PUT-only -> kollidiert nicht mit .../device/{d}/*.
    routeT(server, "^/streaming/account/([^/]+)/device/([^/]+)$",          HTTP_PUT,
              handlePutDeviceFinalize, nullptr, handlePutDeviceBody);
    // POST /streaming/account/<aid>/source — MusicService-Account-Registrierung
    // (DLNA, Spotify-User, etc.). Speaker triggert dies aus BMX-POST
    // /setMusicServiceAccount und erwartet 200 OK mit <source id=...>-XML.
    routeT(server, "^/streaming/account/([^/]+)/source$",                  HTTP_POST,
              handleAccountSourceAddFinalize, nullptr, handleAccountSourceAddBody);
    server.on("/updates/soundtouch",                                  HTTP_GET,  handleSWUpdateIndex);

    server.onNotFound(handleNotFound);
}
