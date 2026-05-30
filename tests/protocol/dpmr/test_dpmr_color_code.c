// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for dPMR channel-code to color-code mapping.
 */

#include <assert.h>
#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t code;
    int32_t color;
} dpmr_color_case;

static const dpmr_color_case k_color_cases[] = {
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

static void
bits_from_u24(uint32_t code, uint8_t bits[24]) {
    for (uint32_t i = 0; i < 24; i++) {
        bits[i] = (uint8_t)((code >> (23U - i)) & 1U);
    }
}

static void
test_all_reference_color_codes(void) {
    uint8_t bits[24];
    for (uint32_t i = 0; i < (uint32_t)(sizeof(k_color_cases) / sizeof(k_color_cases[0])); i++) {
        bits_from_u24(k_color_cases[i].code, bits);
        assert(GetdPmrColorCode(bits) == k_color_cases[i].color);
    }
}

static void
test_dibit_lsb_mask_is_preserved(void) {
    uint8_t bits[24];
    bits_from_u24(0x575F77u & ~0x555555u, bits);
    assert(GetdPmrColorCode(bits) == 0);
}

static void
test_invalid_color_code_rejected(void) {
    uint8_t bits[24];
    bits_from_u24(0x000000u, bits);
    assert(GetdPmrColorCode(bits) == -1);
}

int
main(void) {
    test_all_reference_color_codes();
    test_dibit_lsb_mask_is_preserved();
    test_invalid_color_code_rejected();
    printf("DPMR_COLOR_CODE: OK\n");
    return 0;
}
