// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Source-Normalizer

#include "source_normalizer.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

namespace sixback {

namespace {

// Letzte Pfad-Komponente aus "/v1/playback/station/<id_or_uuid>".
String lastPathSegment(const String& location) {
    int slash = location.lastIndexOf('/');
    if (slash < 0) return location;
    return location.substring(slash + 1);
}

// radio-browser.info: UUID → Station-Metadata.
// HTTP (kein TLS noetig — verifiziert 2026-05-18 auf de1.api.radio-browser.info).
// API: /json/stations/byuuid/<uuid> → Array mit einem Objekt
//      { name, url_resolved, url, favicon, ... }
bool resolveRadioBrowserUuid(const String& uuid,
                             String& outName,
                             String& outStreamUrl,
                             String& outImage) {
    if (uuid.length() < 8) return false;
    HTTPClient http;
    http.setReuse(false);  // single-shot probe — kein TIME_WAIT-pcb-leak
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    http.setUserAgent("SixBack/1.0 (+https://github.com/tostmann/SixBack)");
    String url = "http://de1.api.radio-browser.info/json/stations/byuuid/" + uuid;
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[normalize] radio-browser HTTP %d for %s\n", code, uuid.c_str());
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.printf("[normalize] radio-browser JSON parse failed for %s\n",
                      uuid.c_str());
        return false;
    }
    if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
        Serial.printf("[normalize] radio-browser empty response for %s\n",
                      uuid.c_str());
        return false;
    }
    JsonObject st = doc[0].as<JsonObject>();
    outName      = (const char*)(st["name"]         | "");
    outStreamUrl = (const char*)(st["url_resolved"] | "");
    if (outStreamUrl.length() == 0) outStreamUrl = (const char*)(st["url"] | "");
    outImage     = (const char*)(st["favicon"]      | "");
    return outStreamUrl.length() > 0;
}

} // anon

const char* normalizeStatusToStr(NormalizeStatus s) {
    switch (s) {
        case NormalizeStatus::OK_PASSTHROUGH: return "ok-passthrough";
        case NormalizeStatus::OK_CONVERTED:   return "ok-converted";
        case NormalizeStatus::OK_OPAQUE:      return "ok-opaque";
        default:                              return "abandoned";
    }
}

NormalizeResult normalizePreset(const String& sourceStr,
                                const String& location,
                                const String& itemName,
                                const String& imageUrl,
                                Preset&       out) {
    NormalizeResult r{};
    r.originalSource = sourceStr;
    out.name     = itemName;
    out.imageUrl = imageUrl;
    out.streamUrl.clear();
    out.stationId.clear();

    if (sourceStr == "TUNEIN") {
        out.source    = PresetSource::TUNEIN;
        out.stationId = lastPathSegment(location);
        r.status      = NormalizeStatus::OK_PASSTHROUGH;
        r.reason      = "native TUNEIN";
        return r;
    }
    if (sourceStr == "LOCAL_INTERNET_RADIO" || sourceStr == "INTERNET_RADIO") {
        // Unsere eigene ORION-Form "/station?data=<base64-json>" (seit v0.8.50
        // serviert account/full LIR-Presets so, der Speaker traegt sie also
        // dauerhaft): streamUrl/name/imageUrl stehen im JSON -> zurueck zum
        // nativen LIR-Eintrag. Als OPAQUE wuerde der Slot mit leerem
        // sourceAccount aus account/full fallen und am Speaker geloescht.
        if (location.startsWith("/station?data=")) {
            String u, n, img;
            if (orionStationDecode(location, u, n, img)) {
                out.source    = PresetSource::LOCAL_INTERNET_RADIO;
                out.streamUrl = u;
                if (out.name.length() == 0)     out.name     = n;
                if (out.imageUrl.length() == 0) out.imageUrl = img;
                r.status      = NormalizeStatus::OK_PASSTHROUGH;
                r.reason      = String("ORION ") + sourceStr + " → native LIR";
                return r;
            }
        }
        // Fremde ORION-Form (Bose-App/gmuth, voller Adapter-Pfad) oder nicht
        // dekodierbar: echter streamUrl nicht verlustfrei rueckgewinnbar -> 1:1
        // als OPAQUE durchreichen (der Caller kopiert das rohe <ContentItem>,
        // toBoseXml bettet es wieder ein). Eine plain Stream-URL bleibt nativer
        // LIR-Eintrag, den der Push ueber orionStationLocation() neu verpackt.
        if (location.startsWith("/station?data=") ||
            location.indexOf("svc-bmx-adapter-orion") >= 0) {
            out.source           = PresetSource::OPAQUE;
            out.opaqueSourceName = sourceStr;
            r.status             = NormalizeStatus::OK_OPAQUE;
            r.reason             = "ORION location → opaque passthrough";
            return r;
        }
        out.source    = PresetSource::LOCAL_INTERNET_RADIO;
        out.streamUrl = location;
        r.status      = NormalizeStatus::OK_PASSTHROUGH;
        r.reason      = String("native ") + sourceStr;
        return r;
    }
    if (sourceStr == "RADIO_BROWSER") {
        String uuid = lastPathSegment(location);
        String n, u, img;
        if (resolveRadioBrowserUuid(uuid, n, u, img)) {
            out.source    = PresetSource::LOCAL_INTERNET_RADIO;
            out.streamUrl = u;
            if (out.name.length() == 0)     out.name     = n;
            if (out.imageUrl.length() == 0) out.imageUrl = img;
            r.status = NormalizeStatus::OK_CONVERTED;
            r.reason = String("RADIO_BROWSER ") + uuid + " → LOCAL_INTERNET_RADIO";
        } else {
            r.status = NormalizeStatus::ABANDONED;
            r.reason = String("RADIO_BROWSER uuid=") + uuid + " not resolvable";
        }
        return r;
    }
    // Alles andere — STORED_MUSIC, STORED_MUSIC_MEDIA_RENDERER, UPNP,
    // BLUETOOTH, SPOTIFY-Account-Preset, etc. → OPAQUE-Passthrough.
    // Speaker spricht solche Quellen direkt an (DLNA/UPnP-Server, BT-Stack,
    // Spotify-Connect-Session); die Cloud (= unser ESP) ist am Playback
    // nicht beteiligt. Wir muessen das ContentItem nur 1:1 zurueckspiegeln
    // beim sync. Caller setzt out.rawContentItem nach dem Aufruf.
    out.source           = PresetSource::OPAQUE;
    out.opaqueSourceName = sourceStr;
    r.status             = NormalizeStatus::OK_OPAQUE;
    r.reason             = String("opaque passthrough for '") + sourceStr + "'";
    return r;
}

} // namespace sixback
