// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ysf_frame.h"
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/viterbi.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum {
    DSD_YSF_MAX_DECODE_DIBITS = 244,
    DSD_YSF_MAX_DECODE_BYTES = 64,
    DSD_YSF_BITS_PER_DIBIT = 2,
    DSD_YSF_BITS_PER_BYTE = 8,
    DSD_YSF_PN95_TABLE_BITS = 512,
    DSD_YSF_PN95_SEED = 0x1C9,
};

static const uint8_t DSD_YSF_PUNCTURE_NONE[4] = {1U, 1U, 1U, 1U};
static const char DSD_YSF_EVENT_PLACEHOLDER[] = "BUMBLEBEETUNA";
static const uint8_t DSD_YSF_FR_INTERLEAVE[144] = {
    0, 7,  12, 19, 24, 31, 36, 43, 48, 55, 60, 67, 72, 79, 84, 91, 96,  103, 108, 115, 120, 127, 132, 139,
    1, 6,  13, 18, 25, 30, 37, 42, 49, 54, 61, 66, 73, 78, 85, 90, 97,  102, 109, 114, 121, 126, 133, 138,
    2, 9,  14, 21, 26, 33, 38, 45, 50, 57, 62, 69, 74, 81, 86, 93, 98,  105, 110, 117, 122, 129, 134, 141,
    3, 8,  15, 20, 27, 32, 39, 44, 51, 56, 63, 68, 75, 80, 87, 92, 99,  104, 111, 116, 123, 128, 135, 140,
    4, 11, 16, 23, 28, 35, 40, 47, 52, 59, 64, 71, 76, 83, 88, 95, 100, 107, 112, 119, 124, 131, 136, 143,
    5, 10, 17, 22, 29, 34, 41, 46, 53, 58, 65, 70, 77, 82, 89, 94, 101, 106, 113, 118, 125, 130, 137, 142,
};

static void
ysf_unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, size_t len) {
    size_t k = 0U;
    for (size_t i = 0; i < len; i++) {
        output[k++] = (input[i] >> 7) & 1U;
        output[k++] = (input[i] >> 6) & 1U;
        output[k++] = (input[i] >> 5) & 1U;
        output[k++] = (input[i] >> 4) & 1U;
        output[k++] = (input[i] >> 3) & 1U;
        output[k++] = (input[i] >> 2) & 1U;
        output[k++] = (input[i] >> 1) & 1U;
        output[k++] = input[i] & 1U;
    }
}

static void
ysf_pack_bit_array_into_byte_array(const uint8_t* input, uint8_t* output, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = 0U;
        for (size_t bit = 0; bit < DSD_YSF_BITS_PER_BYTE; bit++) {
            byte = (uint8_t)((byte << 1U) | (input[(i * DSD_YSF_BITS_PER_BYTE) + bit] & 1U));
        }
        output[i] = byte;
    }
}

static uint8_t
ysf_pn95_next(uint16_t* lfsr) {
    uint8_t bit = (uint8_t)(*lfsr & 1U);
    uint16_t feedback = (uint16_t)(((*lfsr >> 4U) ^ *lfsr) & 1U);
    *lfsr = (uint16_t)((*lfsr >> 1U) | (feedback << 8U));
    return bit;
}

uint8_t
dsd_ysf_pn95_bit(size_t bit_index) {
    uint16_t lfsr = DSD_YSF_PN95_SEED;
    bit_index %= DSD_YSF_PN95_TABLE_BITS;

    for (size_t i = 0; i < bit_index; i++) {
        (void)ysf_pn95_next(&lfsr);
    }

    return ysf_pn95_next(&lfsr);
}

void
dsd_ysf_dewhiten_bits(uint8_t* bits, size_t bit_count) {
    if (bits == NULL) {
        return;
    }

    size_t offset = 0U;
    while (offset < bit_count) {
        uint16_t lfsr = DSD_YSF_PN95_SEED;
        for (size_t i = 0U; i < DSD_YSF_PN95_TABLE_BITS && offset < bit_count; i++, offset++) {
            bits[offset] = (uint8_t)(bits[offset] ^ ysf_pn95_next(&lfsr));
        }
    }
}

static void
ysf_dibit_to_soft_costs(uint8_t dibit, uint16_t* s0, uint16_t* s1) {
    *s0 = (dibit & 0x2U) ? UINT16_MAX : 0U;
    *s1 = (dibit & 0x1U) ? UINT16_MAX : 0U;
}

uint32_t
dsd_ysf_soft_viterbi_decode(const uint8_t* dibits, size_t dibit_count, size_t decoded_bytes, size_t offset_bits,
                            size_t output_bits, uint8_t* out_bits, uint8_t* out_bytes) {
    uint16_t soft_bits[DSD_YSF_MAX_DECODE_DIBITS * DSD_YSF_BITS_PER_DIBIT];
    uint8_t decoded[DSD_YSF_MAX_DECODE_BYTES];
    uint8_t temp_bits[DSD_YSF_MAX_DECODE_BYTES * DSD_YSF_BITS_PER_BYTE];
    size_t output_bytes;
    size_t input_bits;

    if (dibits == NULL || out_bits == NULL || out_bytes == NULL || dibit_count > DSD_YSF_MAX_DECODE_DIBITS
        || decoded_bytes == 0U || decoded_bytes >= DSD_YSF_MAX_DECODE_BYTES || output_bits == 0U
        || (output_bits % DSD_YSF_BITS_PER_BYTE) != 0U) {
        return UINT32_MAX;
    }

    if (offset_bits > ((decoded_bytes + 1U) * DSD_YSF_BITS_PER_BYTE)
        || output_bits > ((decoded_bytes + 1U) * DSD_YSF_BITS_PER_BYTE) - offset_bits) {
        return UINT32_MAX;
    }
    output_bytes = output_bits / DSD_YSF_BITS_PER_BYTE;

    DSD_MEMSET(soft_bits, 0, sizeof(soft_bits));
    DSD_MEMSET(decoded, 0, sizeof(decoded));
    DSD_MEMSET(temp_bits, 0, sizeof(temp_bits));
    DSD_MEMSET(out_bits, 0, output_bits);
    DSD_MEMSET(out_bytes, 0, output_bytes);

    for (size_t i = 0; i < dibit_count; i++) {
        ysf_dibit_to_soft_costs(dibits[i], &soft_bits[i * 2U], &soft_bits[(i * 2U) + 1U]);
    }

    input_bits = dibit_count * DSD_YSF_BITS_PER_DIBIT;
    uint32_t error =
        viterbi_decode_punctured(decoded, soft_bits, DSD_YSF_PUNCTURE_NONE, (uint16_t)input_bits,
                                 (uint16_t)(sizeof(DSD_YSF_PUNCTURE_NONE) / sizeof(DSD_YSF_PUNCTURE_NONE[0])));

    ysf_unpack_byte_array_into_bit_array(decoded, temp_bits, decoded_bytes + 1U);
    DSD_MEMCPY(out_bits, temp_bits + offset_bits, output_bits);
    ysf_pack_bit_array_into_byte_array(out_bits, out_bytes, output_bytes);
    return error;
}

bool
dsd_ysf_event_text_should_print(const dsd_state* state) {
    if (state == NULL || state->event_history_s == NULL) {
        return false;
    }

    const Event_History* item = &state->event_history_s[0].Event_History_Items[0];
    if (item->text_message[0] == '\0') {
        return false;
    }

    return strncmp(item->event_string, DSD_YSF_EVENT_PLACEHOLDER, strlen(DSD_YSF_EVENT_PLACEHOLDER)) != 0;
}

void
dsd_ysf_unpack_full_rate_imbe(const uint8_t imbe_raw[144], uint8_t imbe_vch[144], char imbe_fr[8][23]) {
    int k = 0;

    if (imbe_raw == NULL || imbe_vch == NULL || imbe_fr == NULL) {
        return;
    }

    for (int j = 0; j < 144; j++) {
        imbe_vch[j] = imbe_raw[DSD_YSF_FR_INTERLEAVE[j]];
    }

    for (int n = 0; n < 4; n++) {
        for (int m = 22; m >= 0; m--) {
            imbe_fr[n][m] = (char)imbe_vch[k++];
        }
    }
    for (int n = 4; n < 7; n++) {
        for (int m = 14; m >= 0; m--) {
            imbe_fr[n][m] = (char)imbe_vch[k++];
        }
    }
    for (int m = 6; m >= 0; m--) {
        imbe_fr[7][m] = (char)imbe_vch[k++];
    }
}
