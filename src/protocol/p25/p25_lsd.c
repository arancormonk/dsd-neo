// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25p1 Low Speed Data FEC (16,8) — single-bit correction across full codeword.
 *
 * Implements correctness for the (16,8) cyclic code used by LSD. We compute the
 * 8-bit syndrome s = parity_rx XOR f(data_rx) using the existing parity map
 * (equivalent to generator-division). If s==0, the word is valid. If s matches
 * exactly one parity bit, we flip that parity bit. If s matches the syndrome of
 * a single data-bit error (preimage via f(2^j)), we flip that data bit. All
 * other cases are treated as uncorrectable.
 *
 * bits16 layout (MSB-first as 0/1 values):
 *   bits16[0..7]   = data bits (bit 0 is MSB of byte)
 *   bits16[8..15]  = parity bits (bit 8 is MSB of parity byte)
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25_lsd.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <string.h>

// Parity lookup from legacy implementation in p25_crc.c
extern uint8_t lsd_parity[256];

int
p25_lsd_fec_16x8(uint8_t* bits16) {
    if (!bits16) {
        return 0;
    }

    // Assemble bytes (MSB-first) from bit array
    uint8_t data = (uint8_t)ConvertBitIntoBytes(bits16, 8);
    uint8_t parity = (uint8_t)ConvertBitIntoBytes(bits16 + 8, 8);

    uint8_t expected = lsd_parity[data];
    uint8_t synd = (uint8_t)(parity ^ expected);

    // Case 1: already valid
    if (synd == 0) {
        return 1;
    }

    // Case 2: single-bit error in parity half — syndrome equals one-hot parity bit
    // bits16[8+pos] corresponds to bit (7-pos) in the parity byte
    if ((synd & (uint8_t)(synd - 1)) == 0) {
        // power-of-two test passed
        int bit_index_from_msb = 0;
        // find index of the single set bit (0..7), MSB is bit7
        for (int b = 7; b >= 0; b--) {
            if (synd & (1u << b)) {
                bit_index_from_msb = b;
                break;
            }
        }
        int pos = 7 - bit_index_from_msb; // convert to [0..7] position in bits16 array
        bits16[8 + pos] = (uint8_t)(1 - (bits16[8 + pos] & 1));
        return 1;
    }

    // Case 3: single-bit error in data half —
    // syndrome must match f(2^j) where j is data bit index (0..7 from MSB)
    for (int pos = 0; pos < 8; pos++) {
        uint8_t mask = (uint8_t)(1u << (7 - pos));
        uint8_t s_pos = lsd_parity[mask];
        if (s_pos == synd) {
            // Flip the erroneous data bit in place
            bits16[pos] = (uint8_t)(1 - (bits16[pos] & 1));
            return 1;
        }
    }

    // Uncorrectable (likely multi-bit error)
    return 0;
}

static int
lsd_abs_i16(int16_t v) {
    return v < 0 ? -(int)v : (int)v;
}

static void
select_low_reliability_bits(const int16_t llr16[16], int* out, int* out_count) {
    int threshold = p25p1_get_erasure_threshold();
    int count = 0;
    for (int i = 0; i < 16; i++) {
        int r = lsd_abs_i16(llr16[i]);
        if (r < threshold) {
            out[count++] = i;
        }
    }
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            int ri = lsd_abs_i16(llr16[out[i]]);
            int rj = lsd_abs_i16(llr16[out[j]]);
            if (rj < ri || (rj == ri && out[j] < out[i])) {
                int tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    if (count > 6) {
        count = 6;
    }
    *out_count = count;
}

int
p25_lsd_fec_16x8_soft(uint8_t* bits16, const int16_t llr16[16]) {
    if (!bits16) {
        return 0;
    }
    if (p25_lsd_fec_16x8(bits16)) {
        return 1;
    }
    if (!llr16) {
        return 0;
    }

    int candidates[16];
    int candidate_count = 0;
    select_low_reliability_bits(llr16, candidates, &candidate_count);
    if (candidate_count <= 0) {
        return 0;
    }

    uint8_t best[16];
    int best_penalty = 999999;
    int found = 0;
    int combinations = 1 << candidate_count;
    for (int mask = 1; mask < combinations; mask++) {
        uint8_t tmp[16];
        memcpy(tmp, bits16, 16);
        int penalty = 0;
        for (int b = 0; b < candidate_count; b++) {
            if ((mask & (1 << b)) != 0) {
                int pos = candidates[b];
                tmp[pos] ^= 1U;
                penalty += lsd_abs_i16(llr16[pos]);
            }
        }
        if (penalty >= best_penalty) {
            continue;
        }
        if (p25_lsd_fec_16x8(tmp)) {
            memcpy(best, tmp, 16);
            best_penalty = penalty;
            found = 1;
        }
    }

    if (!found) {
        return 0;
    }
    memcpy(bits16, best, 16);
    return 1;
}
