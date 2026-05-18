// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Zero-Touch Auto-Migration
//
// Wenn aktiviert (NVS-Flag), laeuft beim Boot eine Pipeline:
//   1. Wait bootDelayMs nach WiFi-Connect, damit Server up sind.
//   2. SpeakerInventory::discover() (SSDP + known-IP-Probe + /24-Scan).
//   3. Pro Speaker mit (cloudUrl != myBase && !ownedByUs && Modell/FW-Whitelist):
//      a) GET /presets → Source-Normalizer → PresetStore (Snapshot vor Migration).
//      b) Telnet migrateSpeaker() → 6 sys-configuration-Kommandos + reboot.
//      c) Warten bis /info wieder antwortet (<= 180 s).
//      d) ownedByUs=true, NVS-persistent.
//   4. handleAccountFull embedded jetzt die normalisierten Presets im
//      account/full-XML → Speaker sync't sie an, kein Long-Press noetig
//      (Lessons-Item 7).
//
// Gated durch AutoModeConfig::enabled (default false). Hard-Limit
// AutoModeConfig::maxPerBoot (default 1) als Foot-Gun-Guard.
#ifndef BOSEFIX32_AUTO_MODE_H
#define BOSEFIX32_AUTO_MODE_H

#include <Arduino.h>

namespace bosefix {

struct AutoModeConfig {
    bool      enabled       = true;   // Image-Default an: flash → provision → migrate zero-touch
    bool      dryRun        = false;
    uint32_t  bootDelayMs   = 10000;
    uint32_t  maxPerBoot    = 4;      // typischer Haushalt 1-4 SoundTouch — alle in einem Boot durch
    uint32_t  cronIntervalS = 1800;   // periodischer Check alle 30 min wenn enabled
                                       //   - light discovery (SSDP + knownIpProbe, kein /24-Sweep)
                                       //   - refreshMigrationStatus (Auto-Claim/Release-Symmetrie)
                                       //   - migrate neu erschienene eligible Speaker bis maxPerBoot
                                       // Set to 0 to disable the periodic loop entirely.
};

struct AutoModeStatus {
    bool      ran                = false;
    bool      running            = false;
    String    state              = "idle";  // idle/discovering/cron-discovering/import-presets/migrate-telnet/wait-reboot/done/cron-idle
    String    currentDeviceId    = "";
    int       speakersSeen       = 0;
    int       speakersEligible   = 0;
    int       speakersMigrated   = 0;
    int       slotsNormalized    = 0;
    int       slotsConverted     = 0;
    int       slotsAbandoned     = 0;
    String    lastError          = "";
    uint32_t  startedMs          = 0;
    uint32_t  finishedMs         = 0;
    uint32_t  tickCount          = 0;       // 1 = initial boot pass, +1 per cron tick
    uint32_t  lastTickFinishedMs = 0;       // for UI countdown to next tick
    uint32_t  nextTickInS        = 0;       // computed snapshot, seconds until next cron tick
    // Anker fuer die nextTickInS-Ableitung in getAutoModeStatus.  Wird beim
    // Start eines Sleep-Intervalls einmal gesetzt; getAutoModeStatus rechnet
    // remaining = (intervalMs - (millis() - sleepStartMs)) / 1000 — kein
    // per-Sekunden-Write mehr unter Lock.
    uint32_t  sleepStartMs       = 0;
    uint32_t  sleepDurMs         = 0;
};

AutoModeConfig loadAutoModeConfig();
void           saveAutoModeConfig(const AutoModeConfig& cfg);

// Spawn der FreeRTOS-Task, die die Pipeline ausfuehrt. Idempotent —
// mehrfache Aufrufe innerhalb eines Boots starten die Pipeline nur einmal.
// Wenn config.enabled == false: Task wird gestartet, prueft, beendet sich.
void           startAutoModeTask();

// Snapshot fuer /api/auto-mode.
AutoModeStatus getAutoModeStatus();

} // namespace bosefix

#endif
