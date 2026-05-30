// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t code;
    int32_t color;
} dpmr_color_code_entry;

static const dpmr_color_code_entry k_dpmr_color_code_map[] = {
    {0x575F77u, 0},  {0x577577u, 1},  {0x57DD75u, 2},  {0x57F775u, 3},  {0x55577Du, 4},  {0x557D7Du, 5},
    {0x55D57Fu, 6},  {0x55FF7Fu, 7},  {0x5F555Fu, 8},  {0x5F7F5Fu, 9},  {0x5FD75Du, 10}, {0x5FFD5Du, 11},
    {0x5D5D55u, 12}, {0x5D7755u, 13}, {0x5DDF57u, 14}, {0x5DF557u, 15}, {0x775DD7u, 16}, {0x7777D7u, 17},
    {0x77DFD5u, 18}, {0x77F5D5u, 19}, {0x7555DDu, 20}, {0x757FDDu, 21}, {0x75D7DFu, 22}, {0x75FDDFu, 23},
    {0x7F57FFu, 24}, {0x7F7DFFu, 25}, {0x7FD5FDu, 26}, {0x7FFFFDu, 27}, {0x7D5FF5u, 28}, {0x7D75F5u, 29},
    {0x7DDDF7u, 30}, {0x7DF7F7u, 31}, {0xD755F7u, 32}, {0xD77FF7u, 33}, {0xD7D7F5u, 34}, {0xD7FDF5u, 35},
    {0xD55DFDu, 36}, {0xD577FDu, 37}, {0xD5DFFFu, 38}, {0xD5F5FFu, 39}, {0xDF5FDFu, 40}, {0xDF75DFu, 41},
    {0xDFDDDDu, 42}, {0xDFF7DDu, 43}, {0xDD57D5u, 44}, {0xDD7DD5u, 45}, {0xDDD5D7u, 46}, {0xDDFFD7u, 47},
    {0xF75757u, 48}, {0xF77D57u, 49}, {0xF7D555u, 50}, {0xF7FF55u, 51}, {0xF55F5Du, 52}, {0xF5755Du, 53},
    {0xF5DD5Fu, 54}, {0xF5F75Fu, 55}, {0xFF5D7Fu, 56}, {0xFF777Fu, 57}, {0xFFDF7Du, 58}, {0xFFF57Du, 59},
    {0xFD5575u, 60}, {0xFD7F75u, 61}, {0xFDD777u, 62}, {0xFDFD77u, 63},
};

/* Convert the Channel code (24 bit) into a valid
 * dPMR color code [0..63]
 * Return -1 on error */
int32_t
GetdPmrColorCode(uint8_t ChannelCodeBit[24]) {
    uint32_t channel_code = 0;

    /* Reconstitute the 24 bit channel code into a 3 bytes integer */
    for (uint32_t i = 0; i < 24; i++) {
        channel_code <<= 1;
        channel_code |= (uint32_t)(ChannelCodeBit[i] & 1u);
    }

    /* By analyzing all channel code on the
   * dPMR standard, I observe that if we decompose the
   * 24 bit into 12 dibit, all dibit's LSB are always
   * equal to "1", so apply a mask to correct
   * flipped LSB dibit.
   *
   * For more details see ETSI TS 102 658 chapter
   * 6.1.5.2.2 "Channel Code Determined by Frequency
   * and System Identity Code".
   *
   * Apply the mask 0x555555
   * 0x555555 = 0b010101010101010101010101 => All dibit's LSB
   * are equal to "1".
   */
    channel_code |= 0x555555u;

    for (uint32_t i = 0; i < (uint32_t)(sizeof(k_dpmr_color_code_map) / sizeof(k_dpmr_color_code_map[0])); i++) {
        if (k_dpmr_color_code_map[i].code == channel_code) {
            return k_dpmr_color_code_map[i].color;
        }
    }
    return -1;
}

void
dpmr_scrambled_pmr_bits(uint32_t* lfsr_value, const uint8_t* input, uint8_t* output, uint32_t bit_count) {
    if (lfsr_value == NULL || input == NULL || output == NULL) {
        return;
    }

    uint8_t shift[9] = {0};
    uint32_t value = *lfsr_value;

    for (uint32_t i = 0; i < 9U; i++) {
        shift[i] = (uint8_t)(value & 1U);
        value >>= 1U;
    }

    for (uint32_t i = 0; i < bit_count; i++) {
        output[i] = (uint8_t)((input[i] ^ shift[0]) & 0x01U);

        const uint8_t feedback = (uint8_t)(shift[4] ^ shift[0]);
        shift[0] = shift[1];
        shift[1] = shift[2];
        shift[2] = shift[3];
        shift[3] = shift[4];
        shift[4] = shift[5];
        shift[5] = shift[6];
        shift[6] = shift[7];
        shift[7] = shift[8];
        shift[8] = feedback;
    }

    value = 0;
    for (uint32_t i = 9U; i > 0U; i--) {
        value <<= 1U;
        value |= (uint32_t)(shift[i - 1U] & 1U);
    }

    *lfsr_value = value;
}

/* End of file */
