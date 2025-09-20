// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Scaffold for P25p1 LSD (16,8) FEC.
 *
 * NOTE: This is a stub implementation intended for wiring and tests. Replace
 * with a spec-correct cyclic-code implementation that can correct single-bit
 * errors anywhere in the 16-bit word and detect multi-bit errors.
 */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/p25/p25_lsd.h>

int
p25_lsd_fec_16x8(uint8_t* bits16) {
    if (!bits16) {
        return 0;
    }
    // Very basic placeholder: treat data[0..7] and parity[8..15] equal => valid.
    // Real implementation will compute parity using the correct (16,8) cyclic code.
    uint8_t data = (uint8_t)ConvertBitIntoBytes(bits16, 8);
    uint8_t par = (uint8_t)ConvertBitIntoBytes(bits16 + 8, 8);
    if (data == par) {
        return 1; // pretend valid
    }
    return 0;
}
