// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief P25 Phase 2 frame-scrambler sequence generation.
 */

#include "p25p2_frame_internal.h"

#include <stddef.h>
#include <stdint.h>

void
p25p2_generate_scramble_bits(uint64_t wacn, uint64_t sysid, uint64_t nac, uint8_t* out_bits, size_t bit_count) {
    if (out_bits == NULL) {
        return;
    }

    uint64_t seed = (wacn * 16777216ULL) + (sysid * 4096ULL) + nac;
    for (size_t i = 0; i < bit_count; i++) {
        /* External 44-bit Fibonacci LFSR per TIA-102 BBAC Fig. 7.1:
         * x^44 + x^34 + x^20 + x^15 + x^9 + x^4 + 1. */
        out_bits[i] = (uint8_t)((seed >> 43) & 0x1U);
        uint64_t bit = ((seed >> 33) ^ (seed >> 19) ^ (seed >> 14) ^ (seed >> 8) ^ (seed >> 3) ^ (seed >> 43)) & 0x1U;
        seed = (seed << 1) | bit;
    }
}
