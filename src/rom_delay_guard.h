// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Plausibilitaets-Deckel auf esp_rom_delay_us()
#ifndef BOSEFIX32_ROM_DELAY_GUARD_H
#define BOSEFIX32_ROM_DELAY_GUARD_H

#include <stdint.h>

namespace sixback {

// Wie oft ein unplausibles Busy-Wait verworfen wurde (0 wenn der Guard nicht
// eingebaut ist). Landet in /api/status -> health.rom_delay_clamped.
uint32_t romDelayClamped();

// Das zuletzt verworfene Argument in Mikrosekunden — der Beweis, dass es der
// Underflow war und kein knapp zu grosser, aber gewollter Wert.
uint32_t romDelayLastClampedUs();

}  // namespace sixback

#endif  // BOSEFIX32_ROM_DELAY_GUARD_H
