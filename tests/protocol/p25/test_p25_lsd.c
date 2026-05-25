// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25_lsd.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

extern uint8_t lsd_parity[256];

static void
byte_to_bits_msbf(uint8_t b, uint8_t out8[8]) {
    for (int i = 0; i < 8; i++) {
        out8[i] = (uint8_t)((b >> (7 - i)) & 1);
    }
}

static void
make_codeword(uint8_t data, uint8_t out_bits16[16]) {
    uint8_t p = lsd_parity[data];
    byte_to_bits_msbf(data, out_bits16 + 0);
    byte_to_bits_msbf(p, out_bits16 + 8);
}

static int
eq_bits(const uint8_t* a, const uint8_t* b, int n) {
    for (int i = 0; i < n; i++) {
        if ((a[i] & 1) != (b[i] & 1)) {
            return 0;
        }
    }
    return 1;
}

static void
make_llr(const uint8_t bits16[16], int16_t llr16[16], int weak0, int weak1) {
    for (int i = 0; i < 16; i++) {
        int mag = (i == weak0 || i == weak1) ? 10 : 200;
        llr16[i] = bits16[i] ? (int16_t)mag : (int16_t)-mag;
    }
}

int
main(void) {
    uint8_t datas[] = {0x00, 0x12, 0xA5, 0xFF};
    uint8_t cw[16];
    uint8_t tmp[16];
    int16_t llr[16];

    for (unsigned int t = 0; t < sizeof(datas); t++) {
        uint8_t d = datas[t];
        make_codeword(d, cw);

        // 1) Valid word passes and remains unchanged
        DSD_MEMCPY(tmp, cw, 16);
        int rc = p25_lsd_fec_16x8(tmp);
        if (rc != 1 || !eq_bits(tmp, cw, 16)) {
            DSD_FPRINTF(stderr, "valid cw failed: d=%02X rc=%d\n", d, rc);
            return 10;
        }

        // 2) Single-bit flips across all 16 positions are corrected
        for (int i = 0; i < 16; i++) {
            DSD_MEMCPY(tmp, cw, 16);
            tmp[i] ^= 1; // flip bit
            rc = p25_lsd_fec_16x8(tmp);
            if (rc != 1 || !eq_bits(tmp, cw, 16)) {
                DSD_FPRINTF(stderr, "single-bit correction failed at pos %d for d=%02X rc=%d\n", i, d, rc);
                return 20 + i;
            }
        }

        // 3) Two-bit flips should be detected as uncorrectable
        DSD_MEMCPY(tmp, cw, 16);
        tmp[0] ^= 1;
        tmp[8] ^= 1;
        rc = p25_lsd_fec_16x8(tmp);
        if (rc != 0) {
            DSD_FPRINTF(stderr, "two-bit error not detected for d=%02X rc=%d\n", d, rc);
            return 40;
        }
        DSD_MEMCPY(tmp, cw, 16);
        tmp[3] ^= 1;
        tmp[5] ^= 1;
        rc = p25_lsd_fec_16x8(tmp);
        if (rc != 0) {
            DSD_FPRINTF(stderr, "two-bit error(2) not detected for d=%02X rc=%d\n", d, rc);
            return 41;
        }

        // 4) Soft LSD uses low-confidence per-bit LLRs to repair a two-bit word
        DSD_MEMCPY(tmp, cw, 16);
        tmp[0] ^= 1;
        tmp[8] ^= 1;
        make_llr(tmp, llr, 0, 8);
        rc = p25_lsd_fec_16x8_soft(tmp, llr);
        if (rc != 1 || !eq_bits(tmp, cw, 16)) {
            DSD_FPRINTF(stderr, "soft two-bit correction failed for d=%02X rc=%d\n", d, rc);
            return 50;
        }

        // 5) High-confidence two-bit errors remain rejected
        DSD_MEMCPY(tmp, cw, 16);
        tmp[3] ^= 1;
        tmp[5] ^= 1;
        make_llr(tmp, llr, -1, -1);
        rc = p25_lsd_fec_16x8_soft(tmp, llr);
        if (rc != 0) {
            DSD_FPRINTF(stderr, "soft high-confidence two-bit error not rejected for d=%02X rc=%d\n", d, rc);
            return 51;
        }
    }

    return 0;
}
