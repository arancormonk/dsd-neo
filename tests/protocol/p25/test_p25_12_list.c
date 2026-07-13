// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused tests for the P25 Phase 1 1/2-rate soft-decision list decoder.
 */

#include <dsd-neo/protocol/p25/p25_12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

/* Fixed reference encoding of 96 2C 11 84 7E A0 39 55 C3 08 F1 6B. */
static const uint8_t k_reference_dibits[98] = {
    0U, 1U, 2U, 1U, 0U, 2U, 3U, 1U, 3U, 0U, 2U, 2U, 0U, 2U, 0U, 0U, 0U, 3U, 1U, 1U, 3U, 3U, 0U, 0U, 1U,
    1U, 1U, 3U, 0U, 1U, 3U, 0U, 2U, 1U, 0U, 3U, 2U, 2U, 3U, 3U, 0U, 0U, 1U, 1U, 0U, 2U, 2U, 0U, 3U, 1U,
    0U, 0U, 1U, 0U, 3U, 2U, 3U, 0U, 2U, 0U, 2U, 1U, 1U, 2U, 0U, 0U, 0U, 2U, 0U, 1U, 1U, 1U, 2U, 2U, 3U,
    1U, 1U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 0U, 2U, 1U, 3U, 0U, 0U, 3U, 3U, 2U, 1U, 3U, 0U, 1U, 0U,
};

static void
dibits_to_llr(const uint8_t dibits[98], int16_t bit_llr[196], int16_t magnitude) {
    for (int i = 0; i < 98; i++) {
        bit_llr[(i * 2) + 0] = ((dibits[i] >> 1) & 1) ? magnitude : (int16_t)-magnitude;
        bit_llr[(i * 2) + 1] = (dibits[i] & 1) ? magnitude : (int16_t)-magnitude;
    }
}

static void
set_dibit_llr_magnitude(const uint8_t dibits[98], int16_t bit_llr[196], int dibit_index, int16_t magnitude) {
    bit_llr[(dibit_index * 2) + 0] = ((dibits[dibit_index] >> 1) & 1) ? magnitude : (int16_t)-magnitude;
    bit_llr[(dibit_index * 2) + 1] = (dibits[dibit_index] & 1) ? magnitude : (int16_t)-magnitude;
}

int
main(void) {
    const uint8_t payload[12] = {0x96, 0x2C, 0x11, 0x84, 0x7E, 0xA0, 0x39, 0x55, 0xC3, 0x08, 0xF1, 0x6B};

    int16_t bit_llr[196];
    dibits_to_llr(k_reference_dibits, bit_llr, 200);

    p25_12_candidate_t list[P25_12_MAX_CANDIDATES];
    if (p25_12_soft_llr_list(k_reference_dibits, NULL, list, P25_12_MAX_CANDIDATES) != 0
        || p25_12_soft_llr_list(k_reference_dibits, bit_llr, NULL, P25_12_MAX_CANDIDATES) != 0
        || p25_12_soft_llr_list(k_reference_dibits, bit_llr, list, 0) != 0) {
        DSD_FPRINTF(stderr, "P25 1/2 list guard failed\n");
        return 1;
    }

    int list_count = p25_12_soft_llr_list(k_reference_dibits, bit_llr, list, P25_12_MAX_CANDIDATES);
    if (list_count <= 0 || memcmp(list[0].bytes, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clean P25 1/2 list decode failed count=%d\n", list_count);
        return 1;
    }

    uint8_t best[12] = {0};
    int metric = p25_12_soft_llr(k_reference_dibits, bit_llr, best);
    if (metric != 0 || memcmp(best, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clean P25 1/2 single decode failed metric=%d\n", metric);
        return 1;
    }

    DSD_MEMSET(list, 0, sizeof(list));
    list_count = p25_12_soft_llr_list(k_reference_dibits, bit_llr, list, P25_12_MAX_CANDIDATES + 3);
    if (list_count != P25_12_MAX_CANDIDATES || memcmp(list[0].bytes, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clamped P25 1/2 list decode failed count=%d\n", list_count);
        return 1;
    }

    uint8_t noisy_dibits[98];
    DSD_MEMCPY(noisy_dibits, k_reference_dibits, sizeof(noisy_dibits));
    noisy_dibits[9] ^= 1U;
    dibits_to_llr(noisy_dibits, bit_llr, 200);
    set_dibit_llr_magnitude(noisy_dibits, bit_llr, 9, 10);

    DSD_MEMSET(list, 0, sizeof(list));
    list_count = p25_12_soft_llr_list(noisy_dibits, bit_llr, list, P25_12_MAX_CANDIDATES);
    int found_original = 0;
    for (int i = 0; i < list_count; i++) {
        if (memcmp(list[i].bytes, payload, sizeof(payload)) == 0) {
            found_original = 1;
            break;
        }
    }
    if (!found_original) {
        DSD_FPRINTF(stderr, "noisy P25 1/2 list decode did not include original count=%d\n", list_count);
        return 2;
    }

    DSD_MEMSET(best, 0, sizeof(best));
    (void)p25_12_soft_llr(noisy_dibits, bit_llr, best);
    if (memcmp(best, list[0].bytes, sizeof(best)) != 0) {
        DSD_FPRINTF(stderr, "noisy P25 1/2 single/list best mismatch\n");
        return 2;
    }

    DSD_FPRINTF(stderr, "P25 1/2-rate LLR list tests OK\n");
    return 0;
}
