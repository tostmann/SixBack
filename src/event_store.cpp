// SPDX-License-Identifier: GPL-3.0-or-later
#include "event_store.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>

namespace bosefix {
namespace {

struct NowPlaying {
    String   title;
    String   artist;
    String   album;
    String   art_url;
    String   source;        // SPOTIFY, TUNEIN, INTERNET_RADIO, STANDBY, ...
    String   play_state;    // PLAY, PAUSE, STOP, BUFFERING
    String   sw_version;    // softwareVersion aus deviceInfo
    String   device_type;   // SoundTouch 10, 20, 30, 300
    int      volume     = -1;
    uint32_t updated_ms = 0;
};

constexpr size_t kTraceLen = 20;
struct TraceEntry {
    uint32_t ts_ms = 0;
    char     type[40] = {0};
};

struct DeviceState {
    NowPlaying  now;
    TraceEntry  trace[kTraceLen];
    size_t      trace_head  = 0;
    size_t      trace_count = 0;
};

// In-memory partial-body accumulator pro Request-Quelle. SCMUDC-Bodies sind
// ueblicherweise <8 KB; ESP32-S3 hat Heap genug.
struct BodyAccumulator {
    String   buffer;
    size_t   total = 0;
};

std::map<String, DeviceState>        g_devices;
std::map<uint32_t, BodyAccumulator>  g_inflight;   // key: AsyncWebServerRequest*-cast
SemaphoreHandle_t                    g_mtx = nullptr;
EventStoreStats                      g_stats;

void pushTrace(DeviceState& d, const String& type) {
    TraceEntry& e = d.trace[d.trace_head];
    e.ts_ms = millis();
    strncpy(e.type, type.c_str(), sizeof(e.type) - 1);
    e.type[sizeof(e.type) - 1] = 0;
    d.trace_head = (d.trace_head + 1) % kTraceLen;
    if (d.trace_count < kTraceLen) d.trace_count++;
}

// Setze einen NowPlaying-Slot, falls JsonObject das Feld enthaelt.
// Bose-JSON nutzt teils camelCase teils kebab-case — wir schauen beide an.
const char* firstStr(JsonObjectConst o, std::initializer_list<const char*> keys) {
    for (auto k : keys) {
        JsonVariantConst v = o[k];
        if (v.is<const char*>()) return v.as<const char*>();
    }
    return nullptr;
}

// Bose nowPlaying-Felder sind ueblich {"track": {"text": "Song"}}. Lies
// entweder direkt den String oder das verschachtelte .text. Liefert nullptr
// wenn nichts da.
const char* textField(JsonObjectConst parent, const char* key) {
    JsonVariantConst v = parent[key];
    if (v.is<const char*>()) return v.as<const char*>();
    JsonObjectConst sub = v.as<JsonObjectConst>();
    if (!sub.isNull()) {
        JsonVariantConst t = sub["text"];
        if (t.is<const char*>()) return t.as<const char*>();
    }
    return nullptr;
}

void applyEvent(DeviceState& d, const String& type, JsonObjectConst data) {
    // Spec: julius-d/ueberboese-api scmudc-Endpoint, alle Beispiele dort.
    if (type == "item-started") {
        // play-state ist direkt unter data
        if (auto s = firstStr(data, {"play-state"}))                    d.now.play_state = s;
        // Track/Artist/Album/Art liegen unter data.nowPlaying als {text:"..."}
        JsonObjectConst np = data["nowPlaying"].as<JsonObjectConst>();
        if (!np.isNull()) {
            if (auto s = textField(np, "track"))    d.now.title   = s;
            if (auto s = textField(np, "artist"))   d.now.artist  = s;
            if (auto s = textField(np, "album"))    d.now.album   = s;
            if (auto s = textField(np, "art"))      d.now.art_url = s;
            if (auto s = firstStr(np, {"source"}))  d.now.source  = s;
            // playStatus aus nowPlaying ist Backup falls play-state oben fehlt
            if (d.now.play_state.length() == 0) {
                if (auto s = firstStr(np, {"playStatus"})) d.now.play_state = s;
            }
        }
    } else if (type == "art-changed") {
        // data.art-status SHOW_DEFAULT_IMAGE | IMAGE_PRESENT | "" ; data.art-uri url
        if (auto s = firstStr(data, {"art-uri"})) d.now.art_url = s;
    } else if (type == "play-state-changed") {
        if (auto s = firstStr(data, {"play-state"})) d.now.play_state = s;
    } else if (type == "source-state-changed") {
        if (auto s = firstStr(data, {"source-state"})) d.now.source = s;
    } else if (type == "volume-change") {
        // data.volume-change ist Array [v0,v1,..,final]. Letztes Element nehmen.
        JsonArrayConst arr = data["volume-change"].as<JsonArrayConst>();
        if (!arr.isNull() && arr.size() > 0) {
            JsonVariantConst last = arr[arr.size() - 1];
            if (last.is<int>()) d.now.volume = last.as<int>();
        }
    } else if (type == "system-state-changed") {
        // data.system-state ist "On" oder "Standby" (Mixed-Case, nicht UPPER).
        if (auto s = firstStr(data, {"system-state"})) {
            d.now.play_state = s;
        }
        JsonObjectConst np = data["nowPlaying"].as<JsonObjectConst>();
        if (!np.isNull()) {
            if (auto s = firstStr(np, {"source"})) d.now.source = s;
        }
    }
    d.now.updated_ms = millis();
}

// Body komplett verarbeiten.
void ingestBody(const String& devIdHint, const String& body) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        g_stats.parse_errors++;
        Serial.printf("[evt] JSON parse error: %s (body=%u bytes, first200=%s)\n",
                      err.c_str(), (unsigned)body.length(),
                      body.substring(0, 200).c_str());
        return;
    }

    JsonObjectConst envelope = doc["envelope"].as<JsonObjectConst>();
    JsonObjectConst payload  = doc["payload"].as<JsonObjectConst>();
    JsonObjectConst devInfo  = payload["deviceInfo"].as<JsonObjectConst>();
    JsonArrayConst  events   = payload["events"].as<JsonArrayConst>();

    String devId;
    if (auto s = firstStr(envelope, {"uniqueId"}))                       devId = s;
    if (devId.length() == 0)
        if (auto s = firstStr(devInfo, {"deviceID"}))                     devId = s;
    if (devId.length() == 0)                                              devId = devIdHint;
    if (devId.length() == 0) {
        Serial.println("[evt] no deviceId in body or hint, drop");
        return;
    }

    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    DeviceState& d = g_devices[devId];

    if (auto s = firstStr(devInfo, {"softwareVersion"})) d.now.sw_version = s;
    if (auto s = firstStr(devInfo, {"deviceType"}))      d.now.device_type = s;

    size_t n_events = 0;
    if (!events.isNull()) {
        for (JsonVariantConst evt : events) {
            JsonObjectConst eo = evt.as<JsonObjectConst>();
            const char* type = eo["type"].as<const char*>();
            if (!type) continue;
            String t(type);
            JsonObjectConst data = eo["data"].as<JsonObjectConst>();
            applyEvent(d, t, data);
            pushTrace(d, t);
            n_events++;
        }
    }
    g_stats.events_ingested += (uint32_t)n_events;
    xSemaphoreGive(g_mtx);

    Serial.printf("[evt] %s: %u event(s)\n", devId.c_str(), (unsigned)n_events);
}

} // anon

void eventStoreInit() {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
}

bool eventStoreIngestChunk(const String& deviceIdHint,
                           uint8_t* data, size_t len,
                           size_t index, size_t total) {
    if (!g_mtx) eventStoreInit();
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    g_stats.chunks_seen++;
    g_stats.last_chunk_ms = millis();
    if (index == 0) {
        Serial.printf("[evt] chunk(0) dev=%s total=%u len=%u\n",
                      deviceIdHint.c_str(), (unsigned)total, (unsigned)len);
    }

    // Wir indexieren in-flight body-Buffer per deviceIdHint+total — gut genug
    // fuer ESP32 (1-2 parallele Speaker schicken nicht denselben deviceId
    // gleichzeitig).
    uint32_t key = (uint32_t)deviceIdHint.length() * 1009u
                 + (uint32_t)(total & 0xFFFFFFFFu);
    BodyAccumulator& acc = g_inflight[key];
    if (index == 0) {
        acc.buffer = "";
        acc.buffer.reserve(total + 1);
        acc.total = total;
    }
    acc.buffer.concat((const char*)data, len);

    bool done = (index + len) >= total;
    String body;
    if (done) {
        body = acc.buffer;
        g_inflight.erase(key);
        g_stats.bodies_completed++;
        g_stats.last_body_ms = millis();
    }
    xSemaphoreGive(g_mtx);

    if (done) {
        ingestBody(deviceIdHint, body);
        return true;
    }
    return false;
}

EventStoreStats eventStoreStats() {
    EventStoreStats s;
    if (!g_mtx) return s;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return s;
    s = g_stats;
    xSemaphoreGive(g_mtx);
    return s;
}

void eventStoreNowPlayingJson(const String& deviceId, JsonObject out) {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    auto it = g_devices.find(deviceId);
    if (it != g_devices.end()) {
        const NowPlaying& n = it->second.now;
        out["title"]       = n.title;
        out["artist"]      = n.artist;
        out["album"]       = n.album;
        out["art_url"]     = n.art_url;
        out["source"]      = n.source;
        out["play_state"]  = n.play_state;
        out["volume"]      = n.volume;
        out["sw_version"]  = n.sw_version;
        out["device_type"] = n.device_type;
        out["updated_ms"]  = n.updated_ms;
        out["age_ms"]      = n.updated_ms ? (long)(millis() - n.updated_ms) : -1L;
    }
    xSemaphoreGive(g_mtx);
}

void eventStoreEventsJson(const String& deviceId, JsonArray out) {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    auto it = g_devices.find(deviceId);
    if (it != g_devices.end()) {
        const DeviceState& d = it->second;
        size_t start = (d.trace_count < kTraceLen) ? 0 : d.trace_head;
        for (size_t i = 0; i < d.trace_count; i++) {
            size_t idx = (start + i) % kTraceLen;
            JsonObject o = out.add<JsonObject>();
            o["ts_ms"] = d.trace[idx].ts_ms;
            o["type"]  = d.trace[idx].type;
        }
    }
    xSemaphoreGive(g_mtx);
}

void eventStoreAllDevicesJson(JsonArray out) {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    for (const auto& kv : g_devices) {
        JsonObject row = out.add<JsonObject>();
        row["device_id"] = kv.first;
        JsonObject n    = row["now"].to<JsonObject>();
        const NowPlaying& np = kv.second.now;
        n["title"]      = np.title;
        n["artist"]     = np.artist;
        n["source"]     = np.source;
        n["play_state"] = np.play_state;
        n["volume"]     = np.volume;
        n["updated_ms"] = np.updated_ms;
        n["age_ms"]     = np.updated_ms ? (long)(millis() - np.updated_ms) : -1L;
        n["events_seen"] = (int)kv.second.trace_count;
    }
    xSemaphoreGive(g_mtx);
}

void eventStoreClear(const String& deviceId) {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_devices.erase(deviceId);
    xSemaphoreGive(g_mtx);
}

void eventStoreClearAll() {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
    g_devices.clear();
    g_inflight.clear();
    xSemaphoreGive(g_mtx);
}

} // namespace bosefix
