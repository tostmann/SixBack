// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Zero-Touch Auto-Migration
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
// Gated durch AutoModeConfig::enabled (Image-Default s. SIXBACK_AUTOMODE_DEFAULT
// unten: an, ausser in Probe-Builds). Hard-Limit AutoModeConfig::maxPerBoot
// (default 4) als Foot-Gun-Guard.
#ifndef BOSEFIX32_AUTO_MODE_H
#define BOSEFIX32_AUTO_MODE_H

#include <Arduino.h>

namespace sixback {

// Lab-Probe-Builds (SIXBACK_CONTIG_PROBE) starten mit auto-mode AUS: ein
// Probe-Board mit leerem NVS haengt im selben LAN wie produktiv genutzte
// Speaker und wuerde die sonst zero-touch an sich reissen.
#ifndef SIXBACK_AUTOMODE_DEFAULT
#  if SIXBACK_CONTIG_PROBE
#    define SIXBACK_AUTOMODE_DEFAULT false
#  else
#    define SIXBACK_AUTOMODE_DEFAULT true
#  endif
#endif

struct AutoModeConfig {
    bool      enabled       = SIXBACK_AUTOMODE_DEFAULT;  // Image-Default an: flash → provision → migrate zero-touch
    bool      dryRun        = false;
    // 20 s statt der frueheren 10 (FHEM 144729 msg1367658, oeffentlich
    // zugesagt): das Fenster startet NICHT beim Einschalten, sondern erst
    // wenn provisionWifi() mit stehender STA zurueckkehrt (main.cpp) — beim
    // frisch geflashten Stick also in dem Moment, in dem der Nutzer die IP
    // gerade erst sieht. 10 s reichten nicht, um die Automatik vor ihrem
    // ersten Zugriff abzuschalten. Bestandsgeraete mit gespeicherten 10000
    // werden beim Laden angehoben, s. loadAutoModeConfig().
    uint32_t  bootDelayMs   = 20000;
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
// true = in NVS persistiert. false = Write fehlgeschlagen (Partition voll trotz
// Cleanup, oder Heap-Enge beim Preferences::begin/serialize). Der PUT-Handler
// MUSS den Rueckgabewert auswerten — sonst meldet die UI faelschlich Erfolg und
// der Toggle springt nach Reboot/Re-GET zurueck (FHEM 144729, betateilchen C5).
bool           saveAutoModeConfig(const AutoModeConfig& cfg);

// Spawn der FreeRTOS-Task, die die Pipeline ausfuehrt. Idempotent —
// mehrfache Aufrufe innerhalb eines Boots starten die Pipeline nur einmal.
// Wenn config.enabled == false: Task wird gestartet, prueft, beendet sich.
void           startAutoModeTask();

// Snapshot fuer /api/auto-mode.
AutoModeStatus getAutoModeStatus();

} // namespace sixback

#endif
