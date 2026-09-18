// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <stdint.h>

namespace sixback {

// Sprungfeste Zeitmessung fuer Fristen, die abbrechen oder rebooten duerfen.
//
// WARUM: auf dem ESP32-C6 (rev v0.0) springt millis() im laufenden Betrieb.
// Beleg = 15,5-h-Serial-Mitschnitt vom 2026-08-20 an einem C6-Testboard:
// 22 Rueckwaertsspruenge der Uptime, davon 20 OHNE Reboot — die Heartbeats
// laufen bruchlos weiter, kein Boot-Banner, Heap unveraendert. Die
// Sprunghoehen sind Vielfache von 2^32 us = 4294,967 s. millis() ist
// esp_timer_get_time()/1000, es ist also die Zeitbasis selbst und nicht ein
// Zaehler darueber.
//
// Eine uint32-Differenz `millis() - start` ist gegen den 49,7-Tage-Ueberlauf
// sicher, aber gegen so einen Sprung wehrlos:
//   - rueckwaerts -> die Differenz unterlaeuft in Richtung 49 Tage. Belegt im
//     Mitschnitt: "[health] WiFi.reconnect() (down for 4292607s)" unmittelbar
//     gefolgt von "[health] SELF-REBOOT: WiFi down > threshold" — ein Reboot
//     aus dem Nichts, der dann auch noch im INT_WDT endete.
//   - vorwaerts -> die Differenz meldet bis zu ~71 min zu viel. Das passt zum
//     bis dahin ungeklaerten OTA-Wall-Clock-Fehlalarm vom 2026-08-06
//     ("exceeded 9 min" nach 26 s bei vollem Tempo). Dort NICHT bewiesen —
//     der Vorfall liegt vor diesem Mitschnitt — aber derselbe Mechanismus in
//     derselben Groessenordnung.
//
// GEGENMITTEL: nicht (jetzt - start) rechnen, sondern die Zeit aus den
// Schritten zwischen zwei Messungen aufaddieren und einen unplausiblen
// Schritt als 0 verbuchen. Ein Sprung laesst die Uhr dann stehen statt sie
// springen zu lassen. Das ist bewusst der konservative Fehler: eine Frist
// laeuft im Sprungfall langsamer ab, statt dass sie grundlos sofort feuert.
//
// BEDINGUNG: step() muss haeufiger als kMaxStepMs aufgerufen werden, sonst
// wird echte verstrichene Zeit verworfen. Alle Nutzer hier ticken im
// Millisekunden- bis Sekundenbereich.
class MonoClock {
public:
    // Groesster Schritt, der noch als echte verstrichene Zeit durchgeht.
    static constexpr uint32_t kMaxStepMs = 60u * 1000u;

    explicit MonoClock(uint32_t nowMs) : lastRaw_(nowMs) {}

    // Einmal je Durchlauf aufrufen; liefert den plausiblen Zuwachs in ms.
    // Ein Sprung (rueckwaerts oder > kMaxStepMs vorwaerts) liefert 0 und
    // erhoeht jumps().
    uint32_t step(uint32_t nowMs) {
        const int32_t d = (int32_t)(nowMs - lastRaw_);
        lastRaw_ = nowMs;
        if (d < 0 || (uint32_t)d > kMaxStepMs) { ++jumps_; return 0; }
        return (uint32_t)d;
    }

    // Zaehlt hoch bei jedem verworfenen Schritt. uint16 reicht; ein Ueberlauf
    // ist unkritisch, die Nutzer vergleichen nur auf Veraenderung.
    uint16_t jumps() const { return jumps_; }

private:
    uint32_t lastRaw_;
    uint16_t jumps_ = 0;
};

}  // namespace sixback
