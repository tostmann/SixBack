// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Plausibilitaets-Deckel auf esp_rom_delay_us()
//
// WARUM: auf dem ESP32-C6 (rev v0.0) springt esp_timer_get_time() rueckwaerts
// (Beleg: src/mono_clock.h). Der IDF-Temperatursensor-Pfad rechnet damit eine
// vorzeichenbehaftete Zeitdifferenz aus und uebergibt sie ungeprueft an eine
// unsigned-Delay-API — in components/esp_hw_support/sar_tsens_ctrl.c
// (IDF v5.5.2; in master und release/v5.5 unveraendert vorhanden):
//
//     int64_t diff = esp_timer_get_time() - timer1;
//     if (diff < TSENS_LINE_REGRESSION_US)                 // 200 us
//         esp_rom_delay_us(TSENS_LINE_REGRESSION_US - diff);
//
// Bei negativem diff wird 200-diff gross und laeuft als uint32 in ein
// Busy-Wait von bis zu ~71 Minuten — und zwar INNERHALB portENTER_CRITICAL().
// Der Tick-ISR kommt dann nicht mehr dran, der Interrupt-Watchdog feuert nach
// CONFIG_ESP_INT_WDT_TIMEOUT_MS (300 ms) und das Board paniced.
//
// Am Blech dreimal reproduziert (c6.log 2026-08-19..21, boot #31/#33/#34),
// jedes Mal mit derselben Signatur: RA = temp_sensor_get_raw_value+0x50 (die
// Ruecksprungadresse direkt hinter dem esp_rom_delay_us-Call), MEPC im C6-ROM,
// A1 = 0xffffffff (high word der Differenz = -1, also negativ) und A0 =
// 0x05c248e0 / 0xd6e76140 / 0xff0964c0 = 96,6 s / 3605,5 s / 4278,8 s
// angefordertes Delay. Einziger Aufrufer des Pfades im Image ist
// phy_get_tsens_value() — der WiFi-PHY. Deshalb faellt es bei WLAN-Stress auf.
//
// WAS DER GUARD MACHT: er faengt die Referenz auf esp_rom_delay_us beim Linken
// ab (-Wl,--wrap) und verwirft Argumente, die kein Treiber je gewollt haben
// kann. Verworfen heisst NICHT gekuerzt: im Fehlerfall ist die korrekte
// Wartezeit null (die 200 us Regressionszeit sind laengst um), und ein auf
// 100 ms gekuerztes Busy-Wait waere in einer kritischen Sektion selbst schon
// ein Problem.
//
// WAS ER NICHT MACHT: die springende Zeitbasis heilen. Das ist ein Deckel auf
// die Folge, nicht ein Fix der Ursache — die liegt im Chip bzw. upstream.
//
// GRENZE DER SCHWELLE: esp_rom_delay_us ist ein Busy-Wait mit laufender CPU;
// Treiber benutzen es fuer Mikro- bis Millisekunden-Wartezeiten. 100 ms ist
// bereits eine Groessenordnung ueber allem Plausiblen und liegt zugleich unter
// dem 300-ms-Limit des Interrupt-Watchdogs — der Deckel muss darunter liegen,
// sonst wirkt er nicht. Die beobachteten Fehlwerte sind 1000x groesser.

#include "rom_delay_guard.h"

#if SIXBACK_ROM_DELAY_GUARD

#include <esp_attr.h>

extern "C" {
void __real_esp_rom_delay_us(uint32_t us);
void __wrap_esp_rom_delay_us(uint32_t us);
}

namespace {
// Groesstes Busy-Wait, das noch als gewollt durchgeht.
constexpr uint32_t kMaxPlausibleUs = 100u * 1000u;   // 100 ms

// Beide in DRAM (.bss) — der Wrapper laeuft auch mit abgeschaltetem Flash-Cache.
volatile uint32_t s_clamped       = 0;
volatile uint32_t s_lastClampedUs = 0;
}  // namespace

// IRAM_ATTR ist Pflicht: der gewrappte Aufruf kommt aus .iram0.text und kann
// waehrend Flash-Operationen oder aus einer kritischen Sektion erfolgen, in
// der ein Sprung nach Flash den Chip zerlegen wuerde.
extern "C" void IRAM_ATTR __wrap_esp_rom_delay_us(uint32_t us) {
    if (us > kMaxPlausibleUs) {
        ++s_clamped;
        s_lastClampedUs = us;
        return;
    }
    __real_esp_rom_delay_us(us);
}

namespace sixback {
uint32_t romDelayClamped()       { return s_clamped; }
uint32_t romDelayLastClampedUs() { return s_lastClampedUs; }
}  // namespace sixback

#else   // Guard nicht eingebaut — die Zaehler bleiben ehrlich bei 0.

namespace sixback {
uint32_t romDelayClamped()       { return 0; }
uint32_t romDelayLastClampedUs() { return 0; }
}  // namespace sixback

#endif
