// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Bose Cloud Replacement Endpoints
//
// Verifizierte Pflicht-Endpoints aus den AfterTouch-Live-Logs (Pi5-Migration
// Kueche 2026-05-16). Phase 1: alle 11 Endpoints mit echten Bodies, plus
// Account-Sources und Preset-Sync zwischen Speaker und ESP-Store.

#include "bose_endpoints.h"
#include "tunein_resolver.h"
#include "preset_store.h"
#include "speaker_inventory.h"
#include "event_store.h"
#include "nvs_helper.h"
#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <map>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
                  req->pathArg(0).c_str(), (int)req->contentLength());
    send200(req);
}

void handleSCMUDCBody(AsyncWebServerRequest* req,
                      uint8_t* data, size_t len, size_t index, size_t total) {
    String devId = req->pathArg(0);
    bosefix::eventStoreIngestChunk(devId, data, len, index, total);
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

// Konstante Source-IDs in unserer XML (frei vergeben, aber stabil):
//   INTERNET_RADIO (provider 2, generic radio)       -> 10002
//   LOCAL_INTERNET_RADIO (provider 11, our stream)   -> 10003
//   TUNEIN legacy (provider 3, ohne Credential)      -> 10004
//   TUNEIN account-bound (provider 25, mit Token)    -> 19989342
//                                  -^ Ueberboese-Referenz-Id, beliebig
static constexpr const char* kTuneinAcctSourceId = "19989342";
static constexpr const char* kFakeTs             = "2020-01-01T00:00:00.000+00:00";

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

// Pro-Device Preset-Block in ueberboese-Form (mit eingebettetem <source>-
// Tag, der den Account-Source-Eintrag referenziert).
static String buildDevicePresets_(const String& deviceId) {
    // Wenn der Store keine Presets fuer das Device hat, KEIN <presets>-Element
    // ausgeben — das gibt dem Speaker das Signal "Cloud sagt nichts dazu" und
    // verhindert dass er seinen Local-Cache mit unserer leeren Liste
    // ueberschreibt (siehe handleDevicePresets-Kommentar).
    if (!bosefix::PresetStore::instance().hasAnyFor(deviceId)) {
        return String();
    }
    auto presets = bosefix::PresetStore::instance().getForSpeaker(deviceId);
    String out = "<presets>";
    for (const auto& p : presets) {
        if (p.source == bosefix::PresetSource::EMPTY) continue;
        out += "<preset buttonNumber=\""; out += String(p.slot); out += "\">";
        out += "<containerArt>"; out += xmlEsc_(p.imageUrl); out += "</containerArt>";
        out += "<contentItemType>stationurl</contentItemType>";
        out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
        out += "<location>";
        if (p.source == bosefix::PresetSource::TUNEIN) {
            out += "/v1/playback/station/"; out += xmlEsc_(p.stationId);
        } else {
            out += xmlEsc_(p.streamUrl);
        }
        out += "</location>";
        out += "<name>"; out += xmlEsc_(p.name); out += "</name>";
        // Eingebetteter Source-Verweis: Provider/Id passend zum Preset-Typ
        if (p.source == bosefix::PresetSource::TUNEIN) {
            out += "<source id=\""; out += kTuneinAcctSourceId; out += "\" type=\"Audio\">";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<credential type=\"token\">bf32-tunein-token</credential>";
            out += "<name></name>";
            out += "<sourceproviderid>25</sourceproviderid>";
            out += "<sourcename></sourcename>";
            out += "<sourceSettings/>";
            out += "<updatedOn>"; out += kFakeTs; out += "</updatedOn>";
            out += "<username></username>";
            out += "</source>";
        } else {
            out += "<source id=\"10003\" type=\"Audio\">";
            out += "<createdOn>"; out += kFakeTs; out += "</createdOn>";
            out += "<credential type=\"token\">eyJzZXJpYWwiOiJsb2NhbC1pbnRlcm5ldC1yYWRpbyJ9</credential>";
            out += "<name></name>";
            out += "<sourceproviderid>11</sourceproviderid>";
            out += "<sourcename></sourcename>";
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

void handleAccountFull(AsyncWebServerRequest* req) {
    String acct = req->pathArg(0);
    String body;
    body.reserve(3072);
    body  = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    body += "<account id=\""; body += xmlEsc_(acct); body += "\">";
    body += "<accountStatus>REGISTERED</accountStatus>";

    // <devices> — alle bekannten Speaker mit gleichem accountId.
    // Fallback wenn die Liste fuer den angefragten Account leer ist:
    // dann liefern wir ALLE Speaker (akzeptieren also dass der Speaker
    // sich selbst in dem device-Block wiederfindet, auch wenn die acct-
    // Verknuepfung im Inventory noch nicht aufgebaut wurde).
    auto allSpeakers = bosefix::SpeakerInventory::instance().list();
    std::vector<bosefix::Speaker> matched;
    for (const auto& s : allSpeakers) if (s.accountId == acct) matched.push_back(s);
    if (matched.empty()) matched = allSpeakers;

    body += "<devices>";
    for (const auto& s : matched) {
        String prodCode, prodLabel;
        modelToProduct_(s.model, prodCode, prodLabel);
        body += "<device deviceid=\""; body += xmlEsc_(s.deviceId); body += "\">";
        body += "<attachedProduct product_code=\""; body += xmlEsc_(prodCode); body += "\">";
        body += "<components/>";
        body += "<productlabel>"; body += xmlEsc_(prodLabel); body += "</productlabel>";
        body += "<serialnumber>"; body += xmlEsc_(s.deviceId); body += "</serialnumber>";
        body += "</attachedProduct>";
        body += "<createdOn>"; body += kFakeTs; body += "</createdOn>";
        body += "<firmwareVersion>"; body += xmlEsc_(s.firmware); body += "</firmwareVersion>";
        body += "<ipaddress>"; body += xmlEsc_(s.ip); body += "</ipaddress>";
        body += "<name>"; body += xmlEsc_(s.name); body += "</name>";
        body += buildDevicePresets_(s.deviceId);
        body += "<recents/>";
        body += "<serialNumber>"; body += xmlEsc_(s.deviceId); body += "</serialNumber>";
        body += "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
        body += "</device>";
    }
    body += "</devices>";

    body += "<mode>global</mode>";
    body += "<preferredLanguage>de</preferredLanguage>";

    // Account-Sources. Doppel-TUNEIN: Legacy provider 3 (wie bisher) +
    // neuer account-bound provider 25 mit nicht-leerer credential.
    body += "<sources>";
    body += "<source id=\"10002\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name></name>";
    body +=   "<sourceproviderid>2</sourceproviderid>";
    body +=   "<sourcename></sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";
    body += "<source id=\"10003\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\">eyJzZXJpYWwiOiJsb2NhbC1pbnRlcm5ldC1yYWRpbyJ9</credential>";
    body +=   "<name></name>";
    body +=   "<sourceproviderid>11</sourceproviderid>";
    body +=   "<sourcename></sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";
    body += "<source id=\"10004\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\"></credential>";
    body +=   "<name>TUNEIN</name>";
    body +=   "<sourceproviderid>3</sourceproviderid>";
    body +=   "<sourcename>TuneIn Radio</sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username>TuneIn</username>";
    body += "</source>";
    body += "<source id=\""; body += kTuneinAcctSourceId; body += "\" type=\"Audio\">";
    body +=   "<createdOn>"; body += kFakeTs; body += "</createdOn>";
    body +=   "<credential type=\"token\">bf32-tunein-token</credential>";
    body +=   "<name></name>";
    body +=   "<sourceproviderid>25</sourceproviderid>";
    body +=   "<sourcename></sourcename>";
    body +=   "<sourceSettings/>";
    body +=   "<updatedOn>"; body += kFakeTs; body += "</updatedOn>";
    body +=   "<username></username>";
    body += "</source>";
    body += "</sources>";

    body += "</account>";
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleAccountSources(AsyncWebServerRequest* req) {
    // Konsistent mit handleAccountFull: doppelte TUNEIN-Liste (legacy
    // provider 3 + account-bound provider 25 mit credential).
    String body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                  "<sources>"
                  "<source id=\"10002\"><name>INTERNET_RADIO</name><sourceproviderid>2</sourceproviderid></source>"
                  "<source id=\"10003\"><name>LOCAL_INTERNET_RADIO</name><sourceproviderid>11</sourceproviderid></source>"
                  "<source id=\"10004\"><name>TUNEIN</name><sourceproviderid>3</sourceproviderid></source>"
                  "<source id=\"";
    body += kTuneinAcctSourceId;
    body += "\" type=\"Audio\"><sourceproviderid>25</sourceproviderid><credential type=\"token\">bf32-tunein-token</credential></source>"
            "</sources>";
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

// -----------------------------------------------------------------------------
// GET /streaming/sourceproviders  -- Provider-Katalog (P1, P0-Audit-Befund)
//   Speaker polled das alle ~30s pro Geraet. Antwort listet die Streaming-
//   Services die im Bose-Universum existieren. Unsere Liste = was wir tatsaechlich
//   bedienen koennen (TUNEIN, INTERNET_RADIO, LOCAL_INTERNET_RADIO). IDs decken
//   sich mit den sourceproviderids in handleAccountSources.
// -----------------------------------------------------------------------------
void handleSourceProviders(AsyncWebServerRequest* req) {
    // Ueberboese-Referenz fuehrt TUNEIN unter sourceproviderid=25, nicht 3.
    // Wir liefern beide IDs als TUNEIN aus — Speaker akzeptiert dann eine
    // davon abhaengig davon was er persistent gespeichert hat.
    static const char* body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<sourceProviders>"
        "<sourceprovider id=\"2\">"
            "<createdOn>2012-01-01T00:00:00.000+00:00</createdOn>"
            "<name>INTERNET_RADIO</name>"
            "<updatedOn>2012-01-01T00:00:00.000+00:00</updatedOn>"
        "</sourceprovider>"
        "<sourceprovider id=\"3\">"
            "<createdOn>2012-01-01T00:00:00.000+00:00</createdOn>"
            "<name>TUNEIN</name>"
            "<updatedOn>2012-01-01T00:00:00.000+00:00</updatedOn>"
        "</sourceprovider>"
        "<sourceprovider id=\"11\">"
            "<createdOn>2012-01-01T00:00:00.000+00:00</createdOn>"
            "<name>LOCAL_INTERNET_RADIO</name>"
            "<updatedOn>2012-01-01T00:00:00.000+00:00</updatedOn>"
        "</sourceprovider>"
        "<sourceprovider id=\"25\">"
            "<createdOn>2012-01-01T00:00:00.000+00:00</createdOn>"
            "<name>TUNEIN</name>"
            "<updatedOn>2012-01-01T00:00:00.000+00:00</updatedOn>"
        "</sourceprovider>"
        "</sourceProviders>";
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
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
        "<title>BoseFix32 Cloud-Mock</title></head><body>"
        "<h1>BoseFix32 Cloud-Mock</h1>"
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
    AsyncWebServerResponse* resp = req->beginResponse_P(
        200, "image/png", kPng1x1Transparent, sizeof(kPng1x1Transparent));
    resp->addHeader("Cache-Control", "public, max-age=86400");
    req->send(resp);
}

void handleProviderSettings(AsyncWebServerRequest* req) {
    req->send(200, "application/json",
              "{\"providers\":["
                "{\"id\":3,\"name\":\"TUNEIN\",\"enabled\":true},"
                "{\"id\":11,\"name\":\"LOCAL_INTERNET_RADIO\",\"enabled\":true}"
              "]}");
}

// -----------------------------------------------------------------------------
// Preset-Endpoints: Speaker fragt nach Account-/Device-Presets
// -----------------------------------------------------------------------------
void handleAccountPresets(AsyncWebServerRequest* req) {
    // Legacy /presets-Endpoint — alte Bose-Cloud-Variante.
    // Gleiche Schutzlogik wie handleDevicePresets: wenn kein bekannter Speaker
    // Presets im Store hat, 404 statt <presets/>, damit der Speaker seinen
    // Local-Cache nicht ueberschreibt.
    auto speakers = bosefix::SpeakerInventory::instance().list();
    bosefix::Speaker* withPresets = nullptr;
    for (auto& s : speakers) {
        if (bosefix::PresetStore::instance().hasAnyFor(s.deviceId)) {
            withPresets = &s; break;
        }
    }
    if (!withPresets) {
        Serial.println("[bmx][safe] /presets account-level -> 404 (alle Stores leer)");
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    String body = bosefix::PresetStore::instance().toBoseXml(withPresets->deviceId);
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

void handleDevicePresets(AsyncWebServerRequest* req) {
    String acct  = req->pathArg(0);
    String devId = req->pathArg(1);
    // Schutz gegen Preset-Verlust nach erase: wenn unser Store fuer das Device
    // noch leer ist (z.B. Mock-Server frisch geflasht, Auto-Mode hat noch
    // nicht via import-from-device gesynct), liefern wir 404 statt
    // <presets/>. Der Speaker behaelt damit seinen Local-Cache. WICHTIG:
    // dieser Endpoint ist die Schluessel-Sicherung gegen den Phase-2-Loss
    // den wir am 2026-05-19 beobachtet haben.
    if (!bosefix::PresetStore::instance().hasAnyFor(devId)) {
        Serial.printf("[bmx][safe] /presets %s -> 404 (store empty, Speaker behaelt Cache)\n",
                      devId.c_str());
        req->send(404, "application/vnd.bose.streaming-v1.2+xml", "");
        return;
    }
    String body  = bosefix::PresetStore::instance().toBoseXml(devId);
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", body);
}

// -----------------------------------------------------------------------------
// /bmx/tunein/* — Station-Resolver, Token-Mock, Report-Sink
// -----------------------------------------------------------------------------
void handleTuneInToken(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"access_token\":\"\",\"refresh_token\":\"\"}");
}

void handleTuneInStation(AsyncWebServerRequest* req) {
    String id = req->pathArg(0);
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
//   Fallback-Custom-Stream-Adapter — Speaker landet hier wenn TUNEIN-Adapter
//   fehlt. Minimal-JSON, kein realer Stream. Speaker faellt auf STANDBY zurueck.
void handleOrionStation(AsyncWebServerRequest* req) {
    req->send(200, "application/json",
              "{\"streamUrl\":\"\",\"imageUrl\":\"\",\"name\":\"\"}");
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
//   Liste der zuletzt gespielten Inhalte. BoseFix32 fuehrt keine Recent-Liste
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
//   Single-Preset-Pull. BoseFix32 liefert Presets ueber /streaming/account/
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

void groupsSave_() {
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
    bosefix::nvsSaveJson("bfx_groups", "list", doc);
}

void groupsInit_() {
    if (!g_groups_mtx) g_groups_mtx = xSemaphoreCreateMutex();
    JsonDocument doc;
    if (!bosefix::nvsLoadJson("bfx_groups", "list", doc)) return;
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
    String did = req->pathArg(1);
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
        resp = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><group/>";
    }
    Serial.printf("[group] GET device=%s -> %s\n",
                  did.c_str(), resp.indexOf("id=") > 0 ? "group" : "empty");
    req->send(200, "application/vnd.bose.streaming-v1.2+xml", resp);
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
    String gid  = req->pathArg(1);
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
    String gid = req->pathArg(1);
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

void handleDeviceAddFinalize(AsyncWebServerRequest* req) {
    String acct = req->pathArg(0);
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
    resp->addHeader("Location",
                    "http://10.10.11.168:8000/streaming/account/" + acct + "/device/" + devId);
    resp->addHeader("METHOD_NAME", "addDevice");
    req->send(resp);
    Serial.printf("[marge] addDevice acct=%s dev=%s -> 201 Bearer\n",
                  acct.c_str(), devId.c_str());
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

    req->send(404, "text/plain", "BoseFix32: endpoint not implemented");
}

} // anon

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
    server.on("^/v1/scmudc/([^/]+)$",                                 HTTP_POST,
              handleSCMUDCFinalize, nullptr, handleSCMUDCBody);
    server.on("/bmx/registry/v1/services",                            HTTP_GET,  handleBMXRegistry);
    server.on("/bmx/registry/v1/servicesAvailability",                HTTP_GET,  handleBMXServicesAvailability);
    server.on("/streaming/sourceproviders",                           HTTP_GET,  handleSourceProviders);
    server.on("^/media/bmx-icons/.+$",                                HTTP_GET,  handleBmxIconStub);
    server.on("/streaming/support/power_on",                          HTTP_POST, handlePowerOn);
    server.on("^/streaming/account/([^/]+)/full$",                    HTTP_GET,  handleAccountFull);
    server.on("^/streaming/account/([^/]+)/sources$",                 HTTP_GET,  handleAccountSources);
    server.on("^/streaming/account/([^/]+)/provider_settings$",       HTTP_GET,  handleProviderSettings);
    server.on("^/streaming/account/([^/]+)/presets$",                 HTTP_GET,  handleAccountPresets);
    server.on("^/streaming/account/([^/]+)/presets/all$",             HTTP_GET,  handleAccountPresets);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/presets$",  HTTP_GET,  handleDevicePresets);
    server.on("/bmx/tunein/v1/token",                                 HTTP_POST, handleTuneInToken);
    server.on("^/bmx/tunein/v1/playback/station/([^/]+)$",            HTTP_GET,  handleTuneInStation);
    server.on("/bmx/tunein/v1/report",                                HTTP_POST, handleTuneInReport);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/recent$",   HTTP_POST, handleRecent);
    // Multi-Room-Group-Status — vom Speaker beim Post-Pair-Boot abgefragt:
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/group/$",   HTTP_GET,  handleDeviceGroup);
    // P5 — Stereo-Pairing / Multi-Room-Group-Lifecycle (POST/PUT/DELETE):
    server.on("^/streaming/account/([^/]+)/group/$",                  HTTP_POST,
              handleGroupCreate, nullptr, handleGroupBody);
    server.on("^/streaming/account/([^/]+)/group/([^/]+)$",           HTTP_PUT,
              handleGroupUpdate, nullptr, handleGroupBody);
    server.on("^/streaming/account/([^/]+)/group/([^/]+)$",           HTTP_DELETE,
              handleGroupDelete);
    // Bose-Device-Blacklist-Check (immer 404 expected):
    server.on("^/v1/blacklist/([^/]+)$",                              HTTP_GET,  handleBlacklist);
    // P7 — defensive Speaker-aktive Gap-Filler-Stubs:
    server.on("/core02/svc-bmx-adapter-orion/prod/orion/station",     HTTP_GET,  handleOrionStation);
    server.on("^/streaming/software/update/account/([^/]+)$",         HTTP_GET,  handleSwUpdateAccount);
    server.on("^/streaming/device/([^/]+)/streaming_token$",          HTTP_GET,  handleStreamingToken);
    server.on("/streaming/support/customersupport",                   HTTP_POST, handleCustomerSupport);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/recents$",  HTTP_GET,  handleAccountRecents);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/recent/([^/]+)$",
              HTTP_GET, handleAccountRecentSingle);
    server.on("^/streaming/account/([^/]+)/device/([^/]+)/preset/([1-6])$",
              HTTP_GET, handleAccountPresetSingle);
    // Marge-Association-Bootstrap (trailing slash ist Pflicht — Speaker schickt es so):
    server.on("^/streaming/account/([^/]+)/device/$",                 HTTP_POST,
              handleDeviceAddFinalize, nullptr, handleDeviceAddBody);
    server.on("/updates/soundtouch",                                  HTTP_GET,  handleSWUpdateIndex);

    server.onNotFound(handleNotFound);
}
