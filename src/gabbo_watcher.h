// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — gabbo-WS-Watcher (#15: Hardware-Tasten-Re-Arm kalter LIR-Slots)
//
// Haelt je owned+migrated Speaker (mit >=1 LOCAL_INTERNET_RADIO-Preset) eine
// persistente gabbo-WebSocket-Verbindung (GabboWsClient). Beobachtet, ob ein
// physischer Tastendruck (nowSelectionUpdated <preset id=N>) auf einer KALTEN
// LIR-Quelle landet (Folge-Signal INVALID_SOURCE / errorUpdate) — und armt
// den Slot dann ueber den nativen ORION-/select neu (selectStationOnSpeaker),
// genau wie der v0.8.20-UI-Play-Reroute, nur hardware-getriggert.
//
// Loop-Guard (gegen Endlos-Re-Select durch unsere EIGENEN /select):
//   1. Trigger nur bei INVALID_SOURCE oder ohne PLAY_STATE binnen 5 s, nicht bei jeder Selektion.
//   2. Self-Select-Suppress-Fenster: jedes eigene /select (selectStationOnSpeaker,
//      doPush_) markiert gabboMarkSelfSelect(ip) -> resultierendes
//      nowSelectionUpdated wird ~20s ignoriert.
//   3. Per-Slot-Versuchs-Cap (Backstop gegen genuin tote Quelle).
//
// Nachfassen: bringt ein Re-Arm binnen kVerifyMs (bei BUFFERING kVerifyBufferingMs) kein
// PLAY_STATE, folgt ein weiteres /select (bis zum Versuchs-Cap). Abbruch bei anderem Tastendruck
// (dieselbe Taste erneut laesst es weiterlaufen), bei STANDBY/anderer Quelle, bei WS-Reconnect
// und wenn inzwischen WebUI/Push selbst selektiert hat.
//
// Feature-Gate SIXBACK_GABBO_WATCHER_ENABLED: nur PSRAM-Targets (S3/S3-8MB).
// Auf esp32/c3/c6 (kein PSRAM, 30KB-Heap-Watchdog) sind die Symbole inline-no-op.

#ifndef SIXBACK_GABBO_WATCHER_H
#define SIXBACK_GABBO_WATCHER_H

#include <Arduino.h>

namespace sixback {

#ifdef SIXBACK_GABBO_WATCHER_ENABLED
void startGabboWatcher();
// Markiert ein selbst-initiiertes /select an spIp (Loop-Guard-Suppress-Fenster).
// Aus jedem Kontext aufrufbar (mutex-geschuetzt). Wird in selectStationOnSpeaker
// und doPush_ vor dem /select aufgerufen.
void gabboMarkSelfSelect(const String& spIp);
#else
inline void startGabboWatcher() {}
inline void gabboMarkSelfSelect(const String&) {}
#endif

} // namespace sixback

#endif // SIXBACK_GABBO_WATCHER_H
