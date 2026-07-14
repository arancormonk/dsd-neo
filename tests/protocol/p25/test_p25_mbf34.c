// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25p1_mbf34.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

/* Fixed P25 MBF 3/4 reference vector for DBSN 0x2A and payload A0..AF. */
static const uint8_t k_reference_block[18] = {0x55U, 0xE1U, 0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U,
                                              0xA7U, 0xA8U, 0xA9U, 0xAAU, 0xABU, 0xACU, 0xADU, 0xAEU, 0xAFU};

static const uint8_t k_reference_dibits[98] = {
    3U, 2U, 1U, 3U, 2U, 0U, 1U, 1U, 3U, 0U, 1U, 2U, 3U, 1U, 3U, 3U, 1U, 0U, 0U, 0U, 1U, 1U, 3U, 0U, 2U,
    3U, 0U, 0U, 2U, 3U, 0U, 3U, 1U, 3U, 3U, 0U, 0U, 3U, 3U, 0U, 0U, 3U, 2U, 2U, 3U, 3U, 1U, 1U, 0U, 2U,
    1U, 1U, 0U, 2U, 0U, 1U, 3U, 3U, 0U, 1U, 0U, 2U, 3U, 1U, 0U, 0U, 0U, 0U, 3U, 1U, 0U, 0U, 0U, 0U, 2U,
    2U, 3U, 3U, 3U, 3U, 3U, 0U, 1U, 3U, 1U, 2U, 0U, 2U, 3U, 0U, 2U, 2U, 1U, 2U, 3U, 3U, 0U, 0U,
};

static void
dibits_to_llr(const uint8_t dibits[98], int16_t bit_llr[196], int16_t magnitude) {
    for (int index = 0; index < 98; index++) {
        bit_llr[(index * 2) + 0] = ((dibits[index] >> 1) & 1U) ? magnitude : (int16_t)-magnitude;
        bit_llr[(index * 2) + 1] = (dibits[index] & 1U) ? magnitude : (int16_t)-magnitude;
    }
}

static void
set_dibit_llr_magnitude(const uint8_t dibits[98], int16_t bit_llr[196], int dibit_index, int16_t magnitude) {
    bit_llr[(dibit_index * 2) + 0] = ((dibits[dibit_index] >> 1) & 1U) ? magnitude : (int16_t)-magnitude;
    bit_llr[(dibit_index * 2) + 1] = (dibits[dibit_index] & 1U) ? magnitude : (int16_t)-magnitude;
}

int
main(void) {
    int16_t bit_llr[196];
    dibits_to_llr(k_reference_dibits, bit_llr, 200);

    uint8_t decoded[18] = {0};
    int rc = p25_mbf34_decode_soft(k_reference_dibits, bit_llr, decoded);
    if (rc != 0 || memcmp(k_reference_block, decoded, sizeof decoded) != 0) {
        DSD_FPRINTF(stderr, "soft decoder clean block mismatch rc=%d\n", rc);
        return 1;
    }

    p25_mbf34_candidate_t list[P25_MBF34_MAX_CANDIDATES];
    int list_count = p25_mbf34_decode_soft_list(k_reference_dibits, bit_llr, list, P25_MBF34_MAX_CANDIDATES);
    if (list_count <= 0 || memcmp(k_reference_block, list[0].bytes, sizeof k_reference_block) != 0) {
        DSD_FPRINTF(stderr, "soft list decoder clean block mismatch count=%d\n", list_count);
        return 2;
    }

    uint8_t noisy_dibits[98];
    DSD_MEMCPY(noisy_dibits, k_reference_dibits, sizeof noisy_dibits);
    noisy_dibits[7] ^= 1U;
    dibits_to_llr(noisy_dibits, bit_llr, 200);
    set_dibit_llr_magnitude(noisy_dibits, bit_llr, 7, 10);

    DSD_MEMSET(decoded, 0, sizeof decoded);
    rc = p25_mbf34_decode_soft(noisy_dibits, bit_llr, decoded);
    if (rc != 0 || memcmp(k_reference_block, decoded, sizeof decoded) != 0) {
        DSD_FPRINTF(stderr, "soft decoder failed low-confidence dibit correction rc=%d\n", rc);
        return 3;
    }

    list_count = p25_mbf34_decode_soft_list(noisy_dibits, bit_llr, list, P25_MBF34_MAX_CANDIDATES);
    for (int candidate = 0; candidate < list_count; candidate++) {
        if (memcmp(k_reference_block, list[candidate].bytes, sizeof k_reference_block) == 0) {
            DSD_FPRINTF(stderr, "P25 MBF 3/4 fixed-vector decode passed\n");
            return 0;
        }
    }

    DSD_FPRINTF(stderr, "soft list decoder omitted low-confidence correction count=%d\n", list_count);
    return 4;
}
