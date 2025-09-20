// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 RS parity regen smoke tests:
 * - RS(24,12,13) and RS(36,20,17) via proto wrappers
 * - RS(63,35) via ezpwd directly
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include "ezpwd/rs"

static void
sym6_to_bits(const std::vector<uint8_t>& syms, std::vector<char>& bits_out) {
    bits_out.resize(syms.size() * 6);
    for (size_t i = 0; i < syms.size(); i++) {
        uint8_t s = syms[i] & 0x3F;
        for (int b = 0; b < 6; b++) {
            bits_out[i * 6 + b] = (char)((s >> (5 - b)) & 1);
        }
    }
}

static void
bits_to_sym6(const std::vector<char>& bits, std::vector<uint8_t>& syms_out) {
    size_t n = bits.size() / 6;
    syms_out.resize(n);
    for (size_t i = 0; i < n; i++) {
        uint8_t s = 0;
        for (int b = 0; b < 6; b++) {
            s = (uint8_t)((s << 1) | (bits[i * 6 + b] & 1));
        }
        syms_out[i] = s;
    }
}

int
main(void) {
    // RS(24,12,13): 12 data symbols, 12 parity symbols (6-bit each)
    {
        std::vector<uint8_t> data_syms(12);
        for (size_t i = 0; i < data_syms.size(); i++) {
            data_syms[i] = (uint8_t)((0x15 * i + 3) & 0x3F);
        }
        std::vector<char> data_bits;
        sym6_to_bits(data_syms, data_bits);
        std::vector<char> parity_bits(12 * 6);
        encode_reedsolomon_24_12_13(data_bits.data(), parity_bits.data());

        // Decode should pass round-trip
        std::vector<char> data_copy = data_bits;
        int irr = check_and_fix_reedsolomon_24_12_13(data_copy.data(), parity_bits.data());
        if (irr != 0) {
            fprintf(stderr, "RS(24,12,13) decode returned %d\n", irr);
            return 10;
        }
        if (memcmp(data_copy.data(), data_bits.data(), data_bits.size()) != 0) {
            fprintf(stderr, "RS(24,12,13) data changed unexpectedly\n");
            return 11;
        }
    }

    // RS(36,20,17): 20 data symbols, 16 parity symbols (6-bit each)
    {
        std::vector<uint8_t> data_syms(20);
        for (size_t i = 0; i < data_syms.size(); i++) {
            data_syms[i] = (uint8_t)((0x2B * i + 7) & 0x3F);
        }
        std::vector<char> data_bits;
        sym6_to_bits(data_syms, data_bits);
        std::vector<char> parity_bits(16 * 6);
        encode_reedsolomon_36_20_17(data_bits.data(), parity_bits.data());

        std::vector<char> data_copy = data_bits;
        int irr = check_and_fix_redsolomon_36_20_17(data_copy.data(), parity_bits.data());
        if (irr != 0) {
            fprintf(stderr, "RS(36,20,17) decode returned %d\n", irr);
            return 20;
        }
        if (memcmp(data_copy.data(), data_bits.data(), data_bits.size()) != 0) {
            fprintf(stderr, "RS(36,20,17) data changed unexpectedly\n");
            return 21;
        }
    }

    // RS(63,35): 35 data symbols, 28 parity symbols; direct ezpwd
    {
        ezpwd::RS<63, 35> rs28;
        std::vector<uint8_t> data(35), parity(28);
        for (size_t i = 0; i < data.size(); i++) {
            data[i] = (uint8_t)((i * 11 + 5) & 0x3F);
        }
        rs28.encode(data, parity); // parity regen

        // Build full codeword (systematic): data || parity
        std::vector<uint8_t> cw;
        cw.reserve(63);
        cw.insert(cw.end(), data.begin(), data.end());
        cw.insert(cw.end(), parity.begin(), parity.end());

        // Decode should require no corrections (>=0) and leave cw unchanged
        int corrected = rs28.decode(cw);
        if (corrected < 0) {
            fprintf(stderr, "RS(63,35) decode failed (%d)\n", corrected);
            return 30;
        }
        if (cw.size() != 63 || !std::equal(cw.begin(), cw.begin() + 35, data.begin())) {
            fprintf(stderr, "RS(63,35) data mismatch after decode\n");
            return 31;
        }
    }

    fprintf(stderr, "RS parity regen smoke passed\n");
    return 0;
}
