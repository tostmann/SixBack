// SPDX-License-Identifier: GPL-3.0-or-later
#include "tunein_resolver.h"
#include "nvs_helper.h"
#include "preset_store.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace bosefix {

namespace {

constexpr const char* NVS_NS = "bosefix-tune";

// NVS-Cache-Policy: einmal aufgeloest, ewig gueltig.
// Ohne RTC haben wir keinen verlaesslichen Zeitbegriff (millis() resettet
// bei jedem Boot, NTP-Anbindung ist optional). Daher: kein automatisches
// Aging — Stale-Eintraege werden durch User-Reset/Re-Migration ersetzt.
// Wenn ein Sender den Stream-URL wechselt, muss der User entweder Preset
// neu setzen oder NVS-Namespace `bosefix-tune` per OTA-Reset loeschen.
// TODO: optionaler `POST /api/tunein/cache/clear`-Endpoint waere nuetzlich.

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
    JsonDocument doc;
    if (!nvsLoadJson(NVS_NS, id.c_str(), doc)) return false;
    url   = (const char*)(doc["url"]   | "");
    name  = (const char*)(doc["name"]  | "");
    image = (const char*)(doc["image"] | "");
    return url.length() > 0;
}

void saveCache(const String& id, const String& url, const String& name, const String& image) {
    JsonDocument doc;
    doc["url"]   = url;
    doc["name"]  = name;
    doc["image"] = image;
    nvsSaveJson(NVS_NS, id.c_str(), doc);
}

bool fetchFromOpml(const String& id, String& url, String& name, String& image) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);
    http.setTimeout(5000);

    // 1) Stream-URL via Tune.ashx
    String tuneUrl = "http://opml.radiotime.com/Tune.ashx?id=" + id + "&render=json";
    if (!http.begin(tuneUrl)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
    JsonArray arr = doc["body"].as<JsonArray>();
    for (JsonObject o : arr) {
        if (String((const char*)(o["element"] | "")) != "audio") continue;
        url = (const char*)(o["url"] | "");
        if (url.length() > 0) break;
    }
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
        image = "https://cdn-profiles.tunein.com/" + id + "/images/logoq.png";
    return true;
}

} // anon

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
    if (p.streamUrl.length() == 0) return false;
    url   = p.streamUrl;
    name  = p.name;
    image = p.imageUrl;
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
            r.imageUrl  = "https://cdn-profiles.tunein.com/" + stationId + "/images/logoq.png";
            r.source    = "fallback";
            r.ok        = true;
            return r;
        }
    }
    return r;
}

} // namespace bosefix

String resolveTuneInStation(const String& stationId) {
    auto r = bosefix::resolveTuneInStruct(stationId);
    if (!r.ok) {
        // Mock fuer unbekannte Stations - Speaker erwartet gueltiges JSON
        return String("{\"name\":\"Unknown (") + stationId + ")\","
               "\"streamType\":\"liveRadio\","
               "\"audio\":{\"streamUrl\":\"\",\"streams\":[]},"
               "\"_links\":{\"bmx_reporting\":{\"href\":\"/v1/report?guide_id=" + stationId + "\"}}}";
    }
    return bosefix::buildBoseJson(r.stationId, r.name, r.streamUrl, r.imageUrl, r.source);
}
