// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/crypto/dmr_keystream.h>
#include <stdint.h>

uint32_t
dmr_mi_advance32(uint32_t mi) {
    uint64_t lfsr = mi;
    for (unsigned int i = 0; i < 32U; i++) {
        const uint64_t bit = ((lfsr >> 31U) ^ (lfsr >> 3U) ^ (lfsr >> 1U)) & 1U;
        lfsr = (lfsr << 1U) | bit;
    }
    return (uint32_t)lfsr;
}
