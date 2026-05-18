// SPDX-License-Identifier: GPL-3.0-or-later
#include "system_health.h"
#include "config.h"
#include "speaker_inventory.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

namespace bosefix {

namespace {

// ------ Tunables -------------------------------------------------------------
constexpr uint32_t HEALTH_TWDT_S               = 30;                   // Task-WDT-Timeout
constexpr uint32_t HEALTH_WIFI_DOWN_REBOOT_S   = 5 * 60;               // 5 Min WLAN weg -> Reboot
constexpr uint32_t HEALTH_WIFI_RECONNECT_S     = 10;                   // alle 10s WiFi.reconnect()
constexpr uint32_t HEALTH_HEAP_LOW_BYTES       = 30 * 1024;            // <30 KB free
constexpr uint32_t HEALTH_HEAP_LOW_REBOOT_S    = 5 * 60;               // 5 Min unter Schwelle -> Reboot
constexpr uint32_t HEALTH_PING_INTERVAL_S      = 5 * 60;               // alle 5 Min Speaker pingen
constexpr uint8_t  HEALTH_PING_MISS_FOR_OFFLINE = 5;                   // 5x miss -> OFFLINE (5*5min = 25min stille bevor Status flippt)
constexpr uint16_t HEALTH_PING_TIMEOUT_MS       = 800;                 // GET /info Timeout

// ------ NVS-State ------------------------------------------------------------
constexpr const char* NVS_NS = "bosefix-sys";

struct HealthState {
    uint32_t boot_count       = 0;
    uint32_t crash_count      = 0;
    uint32_t wifi_reboots     = 0;
    uint32_t heap_reboots     = 0;
    uint8_t  last_reset_raw   = 0;   // esp_reset_reason() vom letzten Boot
    bool     wdt_subscribed   = false;

    // Laufzeit-Tracking (kein NVS)
    uint32_t wifi_down_since_ms   = 0;   // 0 = WiFi up
    uint32_t wifi_last_reconnect_ms = 0;
    uint32_t heap_low_since_ms    = 0;   // 0 = ok
    uint32_t last_ping_ms         = 0;
    // Miss-Counter pro Speaker via Map (deviceId -> u8). Da wir nicht viele
    // haben, simple std::map waere ok, aber wir bleiben minimalistisch und
    // halten die Werte direkt am Speaker-Objekt nicht — wir verwalten sie
    // hier in einer kleinen statischen Tabelle (max 16 Speaker).
    struct PingMiss { String id; uint8_t miss; };
    PingMiss ping_misses[16];
} g;

const char* resetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        default:                 return "?";
    }
}

bool isCrashReason(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT;
}

uint8_t& pingMissForId(const String& id) {
    static uint8_t dummy = 0;
    // existierenden Slot finden
    for (auto& p : g.ping_misses) {
        if (p.id == id) return p.miss;
    }
    // freien Slot finden
    for (auto& p : g.ping_misses) {
        if (p.id.length() == 0) { p.id = id; p.miss = 0; return p.miss; }
    }
    return dummy;  // Tabelle voll - never mind
}

void resetPingMiss(const String& id) { pingMissForId(id) = 0; }

void persistCounters() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putUInt("boot_count",   g.boot_count);
    p.putUInt("crash_count",  g.crash_count);
    p.putUInt("wifi_reboots", g.wifi_reboots);
    p.putUInt("heap_reboots", g.heap_reboots);
    p.putUChar("last_reason", g.last_reset_raw);
    p.end();
}

void doSelfReboot(const char* reason, uint32_t& counterField) {
    Serial.printf("[health] SELF-REBOOT: %s\n", reason);
    ++counterField;
    persistCounters();
    delay(200);
    ESP.restart();
}

// Single-Speaker self-ping (synchron, 800ms timeout - billig).
// Mutiert die Kopie `s`; statusChanged=true wenn der Caller nach dem Merge
// saveToNVS aufrufen soll. saveToNVS wird hier NICHT mehr direkt gemacht —
// sonst persistieren wir den vor-merge-Zustand mit stalen Feldern.
void pingOneSpeaker(Speaker& s, bool& statusChanged) {
    statusChanged = false;
    if (s.ip.length() == 0) return;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(HEALTH_PING_TIMEOUT_MS);
    http.setTimeout(HEALTH_PING_TIMEOUT_MS);
    String url = "http://" + s.ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) return;
    int code = http.GET();
    http.end();
    if (code == 200) {
        s.lastSeenMs = millis();
        resetPingMiss(s.deviceId);
        if (s.status == MigrationStatus::OFFLINE) {
            // war offline, jetzt wieder da - status zurueck auf UNKNOWN damit
            // ein refresh-status den echten Stand holt
            s.status = MigrationStatus::UNKNOWN;
            statusChanged = true;
        }
    } else {
        uint8_t& miss = pingMissForId(s.deviceId);
        if (miss < 255) ++miss;
        if (miss >= HEALTH_PING_MISS_FOR_OFFLINE &&
            s.status != MigrationStatus::OFFLINE) {
            Serial.printf("[health] speaker %s (%s) OFFLINE after %u misses\n",
                          s.deviceId.c_str(), s.ip.c_str(), miss);
            s.status = MigrationStatus::OFFLINE;
            statusChanged = true;
        }
    }
}

void pingAllSpeakers() {
    auto& inv = SpeakerInventory::instance();
    // Snapshot OHNE Lock fuer HTTP-IO, dann unter Lock zurueck-mergen +
    // saveToNVS nur wenn sich tatsaechlich Status aenderte.
    auto snapshot = inv.list();
    bool anyChange = false;
    for (auto& s : snapshot) {
        bool changed = false;
        pingOneSpeaker(s, changed);
        SpeakerInventory::LockGuard g(inv);
        if (auto* live = inv.findById(s.deviceId)) {
            live->lastSeenMs = s.lastSeenMs;
            live->status     = s.status;
        }
        if (changed) anyChange = true;
    }
    if (anyChange) inv.saveToNVS();
}

}  // namespace

void healthInit() {
    Preferences p;
    if (p.begin(NVS_NS, false)) {
        g.boot_count     = p.getUInt("boot_count",   0);
        g.crash_count    = p.getUInt("crash_count",  0);
        g.wifi_reboots   = p.getUInt("wifi_reboots", 0);
        g.heap_reboots   = p.getUInt("heap_reboots", 0);
        g.last_reset_raw = p.getUChar("last_reason", 0);
        p.end();
    }
    esp_reset_reason_t r = esp_reset_reason();
    if (isCrashReason(r)) ++g.crash_count;
    ++g.boot_count;
    g.last_reset_raw = (uint8_t)r;
    persistCounters();

    Serial.printf("[health] boot #%u  last-reset=%s  crashes-lifetime=%u  "
                  "wifi-reboots=%u  heap-reboots=%u\n",
                  g.boot_count, resetReasonText(r),
                  g.crash_count, g.wifi_reboots, g.heap_reboots);

    // Task-WDT subscriben (Loop-Task = aktuelle Task).
    // arduino-esp32 3.x: esp_task_wdt_init() existiert bereits mit
    // Default-Config; wir adden uns nur dazu. esp_task_wdt_add(NULL)
    // wirft ESP_ERR_INVALID_STATE wenn TWDT noch nicht init'd ist —
    // in dem Fall holen wir das nach.
    esp_err_t e = esp_task_wdt_add(NULL);
    if (e == ESP_ERR_INVALID_STATE) {
        // TWDT war noch nicht initialisiert (zB CONFIG_ESP_TASK_WDT_INIT=n).
        // Wir initialisieren ihn jetzt nur fuer uns.
        esp_task_wdt_config_t cfg = {
            .timeout_ms   = HEALTH_TWDT_S * 1000,
            .idle_core_mask = 0,    // Idle-Task nicht beobachten
            .trigger_panic = true,
        };
        if (esp_task_wdt_init(&cfg) == ESP_OK) {
            e = esp_task_wdt_add(NULL);
        }
    } else if (e == ESP_OK) {
        // schon initialisiert mit fremder Config — wir koennen Timeout NICHT
        // unilateral umstellen. Default in arduino-esp32 ist meist 5s,
        // damit muessten wir < 5s ticken. Wir versuchen ein reconfigure.
        esp_task_wdt_config_t cfg = {
            .timeout_ms   = HEALTH_TWDT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        esp_task_wdt_reconfigure(&cfg);
    }
    g.wdt_subscribed = (e == ESP_OK || e == ESP_ERR_INVALID_STATE) &&
                       esp_task_wdt_status(NULL) == ESP_OK;
    Serial.printf("[health] task-wdt subscribed=%d (timeout=%us)\n",
                  g.wdt_subscribed ? 1 : 0, HEALTH_TWDT_S);
}

void healthTick() {
    uint32_t now = millis();

    // 1) Task-WDT feeden
    if (g.wdt_subscribed) esp_task_wdt_reset();

    // 2) WiFi-Watchdog
    bool wifi = (WiFi.status() == WL_CONNECTED);
    if (wifi) {
        g.wifi_down_since_ms = 0;
    } else {
        if (g.wifi_down_since_ms == 0) {
            g.wifi_down_since_ms = now;
            g.wifi_last_reconnect_ms = 0;
            Serial.println("[health] WiFi disconnected — watchdog armed");
        }
        // Periodisch reconnect anstossen (nicht im Sekundentakt - alle 10s)
        if (now - g.wifi_last_reconnect_ms > HEALTH_WIFI_RECONNECT_S * 1000) {
            g.wifi_last_reconnect_ms = now;
            Serial.printf("[health] WiFi.reconnect() (down for %lus)\n",
                          (now - g.wifi_down_since_ms) / 1000);
            WiFi.reconnect();
        }
        if (now - g.wifi_down_since_ms > HEALTH_WIFI_DOWN_REBOOT_S * 1000) {
            doSelfReboot("WiFi down > threshold", g.wifi_reboots);
        }
    }

    // 3) Heap-Watchdog
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < HEALTH_HEAP_LOW_BYTES) {
        if (g.heap_low_since_ms == 0) {
            g.heap_low_since_ms = now;
            Serial.printf("[health] free-heap %u < %u — watchdog armed\n",
                          freeHeap, HEALTH_HEAP_LOW_BYTES);
        }
        if (now - g.heap_low_since_ms > HEALTH_HEAP_LOW_REBOOT_S * 1000) {
            doSelfReboot("free-heap low > threshold", g.heap_reboots);
        }
    } else {
        if (g.heap_low_since_ms != 0) {
            Serial.printf("[health] free-heap recovered (%u) — disarm\n", freeHeap);
        }
        g.heap_low_since_ms = 0;
    }

    // 4) Self-Ping (nur wenn WiFi up)
    if (wifi && (now - g.last_ping_ms > HEALTH_PING_INTERVAL_S * 1000)) {
        g.last_ping_ms = now;
        pingAllSpeakers();
    }
}

void healthToJson(JsonObject out) {
    out["boot_count"]      = g.boot_count;
    out["crash_count"]     = g.crash_count;
    out["wifi_reboots"]    = g.wifi_reboots;
    out["heap_reboots"]    = g.heap_reboots;
    out["last_reset"]      = resetReasonText((esp_reset_reason_t)g.last_reset_raw);
    out["wdt_subscribed"]  = g.wdt_subscribed;
    out["wifi_down_for_s"] = g.wifi_down_since_ms == 0 ? 0
                              : (millis() - g.wifi_down_since_ms) / 1000;
    out["heap_low_for_s"]  = g.heap_low_since_ms == 0 ? 0
                              : (millis() - g.heap_low_since_ms) / 1000;
    out["last_ping_age_s"] = g.last_ping_ms == 0 ? -1
                              : (int)((millis() - g.last_ping_ms) / 1000);
}

const char* lastResetReasonStr() {
    return resetReasonText((esp_reset_reason_t)g.last_reset_raw);
}

}  // namespace bosefix
