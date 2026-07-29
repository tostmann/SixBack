// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — TuneIn-Live-Resolver mit Cache + User-Override
//
// Resolver-Pipeline:
//   1. PresetStore: gibt es einen Override (streamUrl) fuer diese stationId?
//      -> Bose-JSON-Wrapper, kein TuneIn-Call.
//   2. NVS-Cache (Namespace sixback-tune): TTL 7 Tage.
//      -> Cache-Hit -> JSON sofort aus Cache.
//   3. HTTP-GET zu http://opml.radiotime.com/Tune.ashx?id=<id>&render=json
//      -> Stream-URL extrahieren, in NVS cachen, JSON bauen.
//   4. Hardcoded-Fallback fuer die 6 Dirk-Stations (Notnagel wenn
//      Internet weg oder TuneIn-API tot ist).
#ifndef BOSEFIX32_TUNEIN_RESOLVER_H
#define BOSEFIX32_TUNEIN_RESOLVER_H

#include <Arduino.h>

namespace sixback {

struct TuneInResolution {
    String stationId;
    String name;
    String streamUrl;
    String imageUrl;
    String source;        // "preset_override" / "cache" / "opml" / "fallback"
    bool   ok;
};

TuneInResolution resolveTuneInStruct(const String& stationId);

// Kanonische TuneIn-Logo-URL zu einer stationId — EINZIGE Quelle fuer dieses
// Muster. PresetStore::saveToNVS laesst eine imageUrl, die exakt hier heraus-
// kommt, beim Persistieren WEG und rekonstruiert sie beim Laden (spart NVS).
// Wird das Muster hier geaendert, aendert sich damit auch die Rekonstruktion
// bereits gespeicherter Presets — nur mit Bedacht anfassen.
String tuneInLogoUrl(const String& stationId);

// Loescht den kompletten Resolve-Cache (NVS-Namespace sixback-tune).
void clearTuneInCache();

// Leert den Resolve-Cache EINMAL pro Firmware-Versionssprung (idempotent).
// Beim Boot aufrufen mit FW_VERSION_STRING.
void autoClearTuneInCacheOnVersionChange(const char* fwVersion);

} // namespace sixback

// Legacy/compat — Phase-0-API.
String resolveTuneInStation(const String& stationId);

#endif
