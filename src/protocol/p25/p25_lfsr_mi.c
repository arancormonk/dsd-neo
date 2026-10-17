// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The P25 MI successor on its own, so code that only needs it does not take the LFSR keystream helpers with it. */

#include <dsd-neo/protocol/p25/p25_lfsr.h>

unsigned long long
p25_lfsr_next_mi(unsigned long long mi) {
    unsigned long long lfsr = mi;
    for (int cnt = 0; cnt < 64; cnt++) {
        // Polynomial is C(x) = x^64 + x^62 + x^46 + x^38 + x^27 + x^15 + 1
        unsigned long long bit =
            ((lfsr >> 63) ^ (lfsr >> 61) ^ (lfsr >> 45) ^ (lfsr >> 37) ^ (lfsr >> 26) ^ (lfsr >> 14)) & 0x1;
        lfsr = (lfsr << 1) | bit;
    }
    return lfsr;
}
