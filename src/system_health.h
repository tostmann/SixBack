// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — System-Health (Watchdogs, Crash-Counter, Self-Ping)
//
// Pflicht in loop():  sixback::healthTick()
// Pflicht in setup(): sixback::healthInit()
//
// Was es macht:
//   - Liest beim Boot esp_reset_reason() + persistente Counter aus NVS
//     (Namespace "sixback-sys": boot_count, crash_count, wifi_reboots,
//      heap_reboots, wifi_disc, last_reason).
//   - Subscribed die Loop-Task am Task-Watchdog (Timeout HEALTH_TWDT_S),
//     ruft esp_task_wdt_reset() in jedem tick().
//   - WiFi-Watchdog: "verbunden" = WL_CONNECTED UND gueltige IP (faengt den
//     stillen Drop / DHCP-Renew-Fail, bei dem WL_CONNECTED auf 0.0.0.0 haengt).
//     Jeder erkannte Abriss -> wifi_disconnects++ (auch ohne Reboot), Dauer in
//     last_wifi_down_s. Nach HEALTH_WIFI_DOWN_REBOOT_S durchgaengiger
//     Disconnect-Phase -> ESP.restart() + wifi_reboots++.
//   - Heap-Watchdog: wenn free-heap < HEALTH_HEAP_LOW_BYTES laenger als
//     HEALTH_HEAP_LOW_REBOOT_S anhaelt -> ESP.restart() + heap_reboots++.
//   - Self-Ping: alle HEALTH_PING_INTERVAL_S sec ein leichter
//     GET /info (Port 8090) an jeden bekannten Speaker. Bei Erfolg:
//     lastSeenMs aktualisieren. Bei N Misses: status=OFFLINE.
//
// Health-Snapshot fuer /api/status: sixback::healthToJson(JsonObject&).

#ifndef BOSEFIX32_SYSTEM_HEALTH_H
#define BOSEFIX32_SYSTEM_HEALTH_H

#include <ArduinoJson.h>

namespace sixback {

// In setup() einmal aufrufen.
void healthInit();

// In loop() bei jeder Iteration aufrufen.
void healthTick();

// Fuer /api/status — fuegt "health"-Object an doc[key] ein.
void healthToJson(JsonObject out);

// Sprungbereinigte Uptime in Sekunden. Auf Boards, deren Zeitbasis springt
// (ESP32-C6 rev v0.0, siehe mono_clock.h), ist das der einzige Uptime-Wert,
// der nicht rueckwaerts laufen kann — millis()/1000 tut das dort sichtbar.
// Im Sprungfall untertreibt der Wert lieber, als zurueckzuspringen.
uint32_t monoUptimeS();

// Reset-Reason vom letzten Boot als kurzer Text ("POWERON","PANIC","WDT",...)
const char* lastResetReasonStr();

// Chip-Modell als kurzer Text ("ESP32-S3","ESP32-C5",...). Gemeinsam genutzt
// von /api/status und dem Diagnostic-Snapshot, damit die Modell-Zuordnung an
// EINER Stelle lebt.
const char* chipModelStr();

}  // namespace sixback

#endif  // BOSEFIX32_SYSTEM_HEALTH_H
