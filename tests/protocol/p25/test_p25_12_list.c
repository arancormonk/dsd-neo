// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused tests for the P25 Phase 1 1/2-rate soft-decision list decoder.
 */

#include <dsd-neo/protocol/p25/p25_12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static const uint8_t test_p25_interleave[98] = {
    0,  1,  8,  9,  16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96,
    97, 2,  3,  10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
    4,  5,  12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93, 6,
    7,  14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};

static const uint8_t test_p25_dtm[16] = {2, 12, 1, 15, 14, 0, 13, 3, 9, 7, 10, 4, 5, 11, 6, 8};

static void
bytes_to_tdibits(const uint8_t bytes[12], uint8_t tdibits[49]) {
    for (int i = 0; i < 48; i++) {
        tdibits[i] = (uint8_t)((bytes[i / 4] >> (6 - (2 * (i % 4)))) & 3U);
    }
    tdibits[48] = 0;
}

static void
encode_12_to_dibits(const uint8_t bytes[12], uint8_t dibits[98]) {
    uint8_t tdibits[49];
    uint8_t deint[98];
    bytes_to_tdibits(bytes, tdibits);

    uint8_t prev = 0;
    for (int i = 0; i < 49; i++) {
        uint8_t next = tdibits[i] & 3U;
        uint8_t nibble = test_p25_dtm[(prev << 2) | next] & 0xFU;
        deint[(i * 2) + 0] = (uint8_t)((nibble >> 2) & 3U);
        deint[(i * 2) + 1] = (uint8_t)(nibble & 3U);
        prev = next;
    }

    for (int i = 0; i < 98; i++) {
        dibits[i] = deint[test_p25_interleave[i]];
    }
}

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
    uint8_t dibits[98];
    encode_12_to_dibits(payload, dibits);

    int16_t bit_llr[196];
    dibits_to_llr(dibits, bit_llr, 200);

    p25_12_candidate_t list[P25_12_MAX_CANDIDATES];
    int list_count = p25_12_soft_llr_list(dibits, bit_llr, list, P25_12_MAX_CANDIDATES);
    if (list_count <= 0 || memcmp(list[0].bytes, payload, sizeof(payload)) != 0) {
        DSD_FPRINTF(stderr, "clean P25 1/2 list decode failed count=%d\n", list_count);
        return 1;
    }

    uint8_t noisy_dibits[98];
    DSD_MEMCPY(noisy_dibits, dibits, sizeof(noisy_dibits));
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

    DSD_FPRINTF(stderr, "P25 1/2-rate LLR list tests OK\n");
    return 0;
}
