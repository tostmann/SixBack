// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Event-Store fuer SCMUDC-Telemetrie (P2)
//
// Speaker POSTen jede UI-/Playback-Zustandsaenderung als JSON an
// `POST /v1/scmudc/{deviceId}` (Bose-interner "stats" / scmudc-Stream).
// Wir nahmen die Requests bisher entgegen und verwarfen den Body — dieser
// Store extrahiert "Now Playing"-Information (Track, Artist, Art, Volume,
// Play-State) plus einen Mini-Trace der letzten Event-Typen pro Device.
//
// Quelle Body-Spec: julius-d/ueberboese-api `ueberboese-api.yaml` §
// DeviceEventsRequest / DeviceEvent. Eventtypen die wir tracken:
//   item-started, play-state-changed, art-changed, source-state-changed,
//   volume-change, preset-pressed, power-pressed, system-state-changed
// (alle anderen Typen werden nur als Trace-Entry geloggt).

#ifndef BOSEFIX32_EVENT_STORE_H
#define BOSEFIX32_EVENT_STORE_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace bosefix {

void eventStoreInit();

// Body-Akkumulator fuer den AsyncWebServer-Body-Callback. Sammelt Chunks bis
// total erreicht und ingestiert dann. Returns true wenn Ingestion erfolgte.
bool eventStoreIngestChunk(const String& deviceIdHint,
                           uint8_t* data, size_t len,
                           size_t index, size_t total);

// JSON-Render fuer Web-UI. NowPlaying = letzte bekannte Track-/Volume-/State-Info.
// Events = Liste der letzten 20 Event-Typen mit Timestamps (Debug/Trace).
void eventStoreNowPlayingJson(const String& deviceId, JsonObject out);
void eventStoreEventsJson    (const String& deviceId, JsonArray  out);
void eventStoreAllDevicesJson(JsonArray out);  // [{deviceId, now:{...}}, ...]

void eventStoreClear(const String& deviceId);
void eventStoreClearAll();

// Debug-Counter (P2): wie oft kam ein scmudc-Body rein, wie viele Chunks,
// wie viele Parse-Fehler? Hilft beim Diagnose ob Speaker uns kontaktiert.
struct EventStoreStats {
    uint32_t chunks_seen        = 0;
    uint32_t bodies_completed   = 0;
    uint32_t parse_errors       = 0;
    uint32_t events_ingested    = 0;
    uint32_t last_chunk_ms      = 0;
    uint32_t last_body_ms       = 0;
};
EventStoreStats eventStoreStats();

} // namespace bosefix

#endif // BOSEFIX32_EVENT_STORE_H
