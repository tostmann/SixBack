// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tunein_resolver.h"
#include "nvs_helper.h"
#include "preset_store.h"
#include "config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <vector>

namespace sixback {

namespace {

// Bis v0.8.36 lag der Resolve-Cache in DIESEM NVS-Namespace. Ein Eintrag
// (url+name+image) kostet dort ~8 der 630 NVS-Entries, ohne Obergrenze —
// bei vielen Speakern/Sendern laeuft die Partition damit voll, egal wie
// schlank die Presets sind (FHEM 144729: 9 Boxen x 6 Presets = "partition
// out of space" trotz Fresh-Install). Seit dem Umzug nach LittleFS wird
// der Namespace nur noch beim Boot geleert (Migration, gibt die Entries
// frei) — neue Eintraege landen hier NIE wieder.
constexpr const char* NVS_NS = "sixback-tune";

// Cache-Policy (unveraendert): einmal aufgeloest, ewig gueltig — ohne RTC
// kein verlaesslicher Zeitbegriff, daher kein Aging. Stale-Eintraege raeumt
// der Versions-Sprung-Auto-Clear ab (Marker-Datei unten). Ablage: eine
// kleine JSON-Datei je Sender unter /tcache/. Auf den 16-MB-Layouts sind
// dort ~9,9 MB frei; auf den 256-KB-Filesystemen der 4-MB-Targets kann ein
// Write bei vollem FS scheitern — dann faellt der Eintrag auf Cache-Miss
// zurueck (Resolver holt ihn erneut von OPML), nichts geht kaputt.
constexpr const char* CACHE_DIR    = "/tcache";
constexpr const char* CACHE_VER_FN = "/tcache/__fwver";   // kein .json-Suffix
                                                          // -> kollidiert nie
                                                          // mit Station-Files

// Sanitizing: die stationId kommt aus dem URL-Pfad des Speakers. Nur
// harmloses Zeichenmaterial wird zum Dateinamen; alles andere ist schlicht
// nicht cachebar (Resolver funktioniert trotzdem, nur eben ohne Cache).
String cacheFsPath(const String& id) {
    if (id.length() == 0 || id.length() > 48) return String();
    for (size_t i = 0; i < id.length(); ++i) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return String();
    }
    return String(CACHE_DIR) + "/" + id + ".json";
}

// Fallback-Liste fuer "Internet weg und Cache leer" (= ganz frischer Boot
// ohne je gelaufenen TuneIn-Resolve). Stark DACH-biased — Dirks lokale
// Senderauswahl. Fuer non-DE-Deployments koennen Nutzer ihre Presets
// einfach per Web-UI mit konkreten Stream-URLs setzen (source=
// LOCAL_INTERNET_RADIO), dann ist die Fallback-Liste nicht relevant.
struct Fallback { const char* id; const char* name; const char* url; };
const Fallback kFallback[] = {
    { "s24896",  "SWR3",               "http://liveradio.swr.de/tn2d2ac/swr3" },
    { "s307613", "Radio Luebeck",      "http://stream.lokalradio.nrw/luebeck" },
    { "s18353",  "R.SH",               "http://streams.rsh.de/rsh-live/mp3-192/web" },
    { "s325614", "Radio Wellenrausch", "http://stream.wellenrausch.de:8002/stream" },
    { "s25221",  "94.3 RS2",           "http://stream.rs2.de/rs2/mp3-192/web/" },
    { "s255597", "80s80s Radio",       "http://streams.80s80s.de/web/mp3-192/streema/" },
};
const size_t kFallbackCount = sizeof(kFallback) / sizeof(kFallback[0]);

bool lookupCache(const String& id, String& url, String& name, String& image) {
    const String path = cacheFsPath(id);
    if (path.length() == 0) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    JsonDocument doc;
    const bool parsed = deserializeJson(doc, f) == DeserializationError::Ok;
    f.close();
    if (!parsed) return false;               // korrupt -> Miss, wird neu geholt
    url   = (const char*)(doc["url"]   | "");
    name  = (const char*)(doc["name"]  | "");
    image = (const char*)(doc["image"] | "");
    return url.length() > 0;
}

void saveCache(const String& id, const String& url, const String& name, const String& image) {
    const String path = cacheFsPath(id);
    if (path.length() == 0) return;
    JsonDocument doc;
    doc["url"]   = url;
    doc["name"]  = name;
    doc["image"] = image;
    LittleFS.mkdir(CACHE_DIR);               // no-op wenn vorhanden
    File f = LittleFS.open(path, "w");
    if (!f) return;                          // FS voll/RO -> kein Cache, kein Drama
    const size_t want = measureJson(doc);
    const size_t got  = serializeJson(doc, f);
    f.close();
    if (got != want) LittleFS.remove(path);  // halber Write (FS voll) -> weg damit,
                                             // sonst laege dauerhaft Muell im Cache
}

// Prueft, ob eine Stream-URL erreichbar ist. GET, nur den Response-Code lesen,
// Body NICHT konsumieren (ein Icecast-200 wuerde sonst endlos streamen).
//   2xx          -> ok (direkter Stream)
//   3xx (301/302/307/308) -> ok: der Speaker folgt dem Redirect selbst
//                            (z.B. stream.srg-ssr.ch: 2x 302 -> audio/mpeg).
//                            Wir loesen ihn NICHT auf (kein setFollowRedirects)
//                            und reichen die Original-URL weiter.
//   4xx/5xx/Connect-Fehler -> tot.
// Noetig, weil TuneIn fuer manche Sender tote Varianten ZUERST listet (s.u.).
bool isStreamReachable(const String& url) {
    HTTPClient probe;
    probe.setReuse(false);
    probe.setConnectTimeout(3000);
    probe.setTimeout(3000);
    if (!probe.begin(url)) return false;
    int code = probe.GET();   // kehrt nach den Headern zurueck
    probe.end();              // schliesst sofort, Body bleibt ungelesen
    return code >= 200 && code < 400;
}

bool fetchFromOpml(const String& id, String& url, String& name, String& image) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);
    http.setTimeout(5000);

    // 1) Stream-URL via Tune.ashx.
    //    WICHTIG: OHNE explizites `formats=` liefert TuneIns OPML-API fuer
    //    AAC-only-Stationen KEINEN Stream, sondern den Platzhalter
    //    `cdn-cms.tunein.com/service/Audio/notcompatible.enUS.mp3` — der
    //    Speaker spielt dann ~12s "this station is not compatible" und faellt
    //    auf INVALID_SOURCE. Der Bose-SoundTouch dekodiert AAC-LC aber sehr
    //    wohl (verifiziert via DLNA auf Emma, 2026-06-02). Loesung: explizit
    //    `formats=mp3` bevorzugen (haelt das bisherige Verhalten fuer
    //    Dual-Format-Sender + waehlt den kompatibelsten Codec), und nur wenn
    //    der Sender kein MP3 hat auf `formats=aac` zurueckfallen.
    //    Kein `hls`/`ogg`/`wma`: die 2021er-FW hat dafuer keinen Decoder.
    auto fetchStreamUrl = [&](const char* formats) -> String {
        String tuneUrl = "http://opml.radiotime.com/Tune.ashx?id=" + id +
                         "&render=json&formats=" + formats;
        if (!http.begin(tuneUrl)) return String();
        int code = http.GET();
        if (code != 200) { http.end(); return String(); }
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) return String();
        // Alle brauchbaren Kandidaten einsammeln (notcompatible/HLS raus).
        std::vector<String> cand;
        for (JsonObject o : doc["body"].as<JsonArray>()) {
            if (String((const char*)(o["element"] | "")) != "audio") continue;
            String u = (const char*)(o["url"] | "");
            if (u.length() == 0) continue;
            if (u.indexOf("notcompatible") >= 0) continue;  // TuneIn-Platzhalter
            if (u.indexOf(".m3u8") >= 0) continue;           // HLS: kein Client in Bose-FW
            cand.push_back(u);
            if (cand.size() >= 6) break;                      // Hot-Path: Liste begrenzen
        }
        if (cand.empty()) return String();
        // TuneIns Reihenfolge ist NICHT verlaesslich "best-first": fuer manche
        // Sender stehen tote Varianten (404) trotz reliability=100 vorn
        // (s17871/Dvojka: _mp3_32 + _mp3_64 = 404, erst _mp3_128 lebt). Das
        // blinde Nehmen des ersten Elements liefert dann eine tote URL, und der
        // Speaker faellt nach ~10s Buffering auf INVALID_SOURCE. Daher den
        // ersten ERREICHBAREN Kandidaten waehlen (2xx/3xx; siehe isStreamReachable).
        for (const String& u : cand) {
            if (isStreamReachable(u)) return u;
        }
        // Keiner hat die Probe bestanden (z.B. transienter Probe-Fehler) ->
        // alte Heuristik "erster Kandidat", damit der Fix nie schlechter ist
        // als vorher (nicht-regressiv).
        Serial.printf("[tunein] %s/%s: no candidate passed reachability probe, "
                      "falling back to first (%s)\n",
                      id.c_str(), formats, cand[0].c_str());
        return cand[0];
    };

    url = fetchStreamUrl("mp3");
    if (url.length() == 0) url = fetchStreamUrl("aac");
    if (url.length() == 0) return false;

    // 2) Stations-Metadata via Describe.ashx — liefert name, slogan, logo
    String desc = "http://opml.radiotime.com/Describe.ashx?id=" + id + "&render=json";
    if (http.begin(desc)) {
        int dc = http.GET();
        if (dc == 200) {
            String dbody = http.getString();
            JsonDocument ddoc;
            if (deserializeJson(ddoc, dbody) == DeserializationError::Ok) {
                JsonArray darr = ddoc["body"].as<JsonArray>();
                for (JsonObject o : darr) {
                    if (String((const char*)(o["element"] | "")) != "station") continue;
                    name  = (const char*)(o["name"] | "");
                    image = (const char*)(o["logo"] | "");
                    break;
                }
            }
        }
        http.end();
    }
    if (image.length() == 0)
        image = tuneInLogoUrl(id);
    return true;
}

} // anon

// Kanonische Logo-URL. Deklaration + Begruendung siehe tunein_resolver.h.
String tuneInLogoUrl(const String& stationId) {
    if (stationId.length() == 0) return String();
    return "https://cdn-profiles.tunein.com/" + stationId + "/images/logoq.png";
}

// Loescht den gesamten TuneIn-Resolve-Cache (NVS-Namespace sixback-tune).
// Noetig nach einem Resolver-Verhaltenswechsel (z.B. dem formats=-Fix):
// Stations, die VORHER als notcompatible-Platzhalter aufgeloest + gecached
// wurden, sind sonst dauerhaft stale. Aufgerufen von POST /api/tunein/cache/clear.
void clearTuneInCache() {
    // Station-Files unter /tcache loeschen (Marker-Datei __fwver bleibt —
    // sonst wuerde der naechste Boot den Cache gleich noch einmal leeren).
    // Erst Namen sammeln, dann loeschen: remove() waehrend openNextFile()
    // ist auf LittleFS nicht definiert.
    std::vector<String> victims;
    File dir = LittleFS.open(CACHE_DIR);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            const String n = String(f.name());
            if (n.endsWith(".json")) victims.push_back(String(CACHE_DIR) + "/" + n);
        }
    }
    for (const auto& p : victims) LittleFS.remove(p);
    // Legacy: bis v0.8.36 lag der Cache im NVS — Namespace mit abraeumen.
    nvsEraseAllInNamespace(NVS_NS);
}

// Beim Firmware-Versionssprung den Resolve-Cache EINMAL leeren. Verhindert,
// dass ein Resolver-Verhaltenswechsel (z.B. der formats=-AAC-Fix) Stations
// dauerhaft auf einer stale notcompatible-Aufloesung haengen laesst — der
// Nutzer muss nach einem OTA nichts manuell triggern. Idempotent: feuert nur,
// wenn FW_VERSION_STRING vom zuletzt gestempelten Wert abweicht; der Marker
// ist die Datei /tcache/__fwver. Der NVS-Legacy-Namespace wird dagegen bei
// JEDEM Boot geleert (billig, wenn er leer ist) — damit werden auch Geraete
// frei, deren Marker schon auf der aktuellen Version steht.
void autoClearTuneInCacheOnVersionChange(const char* fwVersion) {
    nvsEraseAllInNamespace(NVS_NS);           // Migration: NVS-Entries freigeben
    String last;
    File m = LittleFS.open(CACHE_VER_FN, "r");
    if (m) { last = m.readString(); m.close(); }
    if (last == fwVersion) return;            // gleiche Version -> nichts tun
    clearTuneInCache();
    LittleFS.mkdir(CACHE_DIR);
    File w = LittleFS.open(CACHE_VER_FN, "w");
    if (w) { w.print(fwVersion); w.close(); }
    Serial.printf("[tunein] resolve-cache cleared on version change -> %s\n", fwVersion);
}

String buildBoseJson(const String& id, const String& name, const String& url,
                     const String& image, const String& source) {
    JsonDocument doc;
    doc["name"]       = name.length() > 0 ? name : ("Station " + id);
    doc["streamType"] = "liveRadio";
    JsonObject audio  = doc["audio"].to<JsonObject>();
    audio["hasPlaylist"]    = true;
    audio["isRealtime"]     = true;
    audio["maxTimeout"]     = 60;
    audio["streamUrl"]      = url;
    JsonArray streams       = audio["streams"].to<JsonArray>();
    JsonObject stream       = streams.add<JsonObject>();
    stream["bufferingTimeout"]  = 20;
    stream["connectingTimeout"] = 10;
    stream["hasPlaylist"]       = true;
    stream["isRealtime"]        = true;
    stream["streamUrl"]         = url;
    if (image.length() > 0) doc["imageUrl"] = image;
    JsonObject links = doc["_links"].to<JsonObject>();
    links["bmx_reporting"]["href"]   = "/v1/report?guide_id=" + id;
    // useInternalClient=ALWAYS: Speaker holt RadioText direkt von TuneIn,
    // nicht ueber uns — sonst sehen wir 'Station sXXXXX' statt echtem Namen.
    JsonObject np = links["bmx_nowplaying"].to<JsonObject>();
    np["href"]              = "/v1/now-playing/station/" + id;
    np["useInternalClient"] = "ALWAYS";
    doc["_meta"]["resolver"] = source;
    String body; serializeJson(doc, body); return body;
}

namespace {
bool findPresetOverride(const String& id, String& url, String& name, String& image) {
    // Hot-Path: jeder Preset-Druck am Speaker landet hier ueber den TuneIn-
    // Resolver. Direkter Lookup via PresetStore::findByStationId statt
    // exportJson+Parse, sonst frisst das Heap+CPU bei jedem Tastendruck.
    Preset p;
    if (!PresetStore::instance().findByStationId(id, p)) return false;
    name  = p.name;
    image = p.imageUrl;
    // Spotify-Tunnel-Sentinel (sspot1..sspot6) braucht eine TLS-freie streamUrl,
    // weil Bose-FW (2021) das Cloudflare-Cert von sixback.io nicht validiert
    // (INVALID_SOURCE-State, long-press persistiert dann nichts). Lokal vom
    // ESP serven, HTTP only — funktioniert unabhaengig von der Stored-URL.
    // Steht VOR dem Leer-Guard, weil die gespeicherte URL hier inhaltlich egal
    // ist: ein unter v0.8.36 ohne streamUrl persistierter Slot scheiterte sonst
    // am Guard und fiel auf "Unknown (sspotN)" mit leerem streams[] zurueck.
    if (id.startsWith("sspot")) {
        url = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT)
            + "/silence.mp3";
        return true;
    }
    if (p.streamUrl.length() == 0) return false;
    url = p.streamUrl;
    return true;
}

} // anon

TuneInResolution resolveTuneInStruct(const String& stationId) {
    TuneInResolution r;
    r.stationId = stationId;
    r.ok = false;
    String url, name, image;

    if (findPresetOverride(stationId, url, name, image)) {
        r.name = name; r.streamUrl = url; r.imageUrl = image;
        r.source = "preset_override"; r.ok = true;
        return r;
    }

    if (lookupCache(stationId, url, name, image) && name.length() > 0) {
        // Cache nur akzeptieren wenn er auch den Stationsnamen kennt.
        // Alte Cache-Eintraege ohne name fallen durch → fetch von OPML.
        r.name = name; r.streamUrl = url; r.imageUrl = image;
        r.source = "cache"; r.ok = true;
        return r;
    }
    url = ""; name = ""; image = "";  // Reset nach unvollstaendigem Cache-Hit

    if (fetchFromOpml(stationId, url, name, image)) {
        saveCache(stationId, url, name, image);
        r.name = name; r.streamUrl = url; r.imageUrl = image;
        r.source = "opml"; r.ok = true;
        return r;
    }

    for (size_t i = 0; i < kFallbackCount; ++i) {
        if (stationId == kFallback[i].id) {
            r.name      = kFallback[i].name;
            r.streamUrl = kFallback[i].url;
            r.imageUrl  = tuneInLogoUrl(stationId);
            r.source    = "fallback";
            r.ok        = true;
            return r;
        }
    }
    return r;
}

} // namespace sixback

String resolveTuneInStation(const String& stationId) {
    auto r = sixback::resolveTuneInStruct(stationId);
    if (!r.ok) {
        // Mock fuer unbekannte Stations - Speaker erwartet gueltiges JSON
        return String("{\"name\":\"Unknown (") + stationId + ")\","
               "\"streamType\":\"liveRadio\","
               "\"audio\":{\"streamUrl\":\"\",\"streams\":[]},"
               "\"_links\":{\"bmx_reporting\":{\"href\":\"/v1/report?guide_id=" + stationId + "\"}}}";
    }
    return sixback::buildBoseJson(r.stationId, r.name, r.streamUrl, r.imageUrl, r.source);
}
