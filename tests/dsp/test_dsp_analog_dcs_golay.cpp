// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DCS words against the project's Golay (23,12) implementation (issue #523).
 *
 * runtime/analog_tones.c builds each DCS word with its own encoder, because the DSP detector may
 * not link the FEC module. This test checks every word it generates against Golay24.hpp, which
 * P25 decoding already relies on, through an explicit mapping between the two layouts:
 *
 *   DCS word W (dsd_dcs_word): bit 0 is the first bit sent; bits 0-8 are the code, bits 9-11
 *     are 0, 0, 1 and bits 12-22 the check bits; W(x), with bit i the coefficient of x^i, is a
 *     multiple of g(x) = 0xC75 = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1.
 *   Golay24.hpp: LSB-first division by POLY 0xAE3, the reciprocal of 0xC75, and code words laid
 *     out [check bits (11) | data (12)].
 *
 * Reversing the 23 bits of a word turns a multiple of g(x) into a multiple of its reciprocal, so
 * reverse23(W) is a Golay24.hpp code word; W itself generally is not. And because the code is
 * cyclic, reverse23(W) is Golay24.hpp's own encoding of the reversed 12 data bits, rotated left
 * by 11:
 *
 *   reverse23(W) == rotate_left23(Golay24::encode(reverse12(W & 0xFFF)) & 0x7FFFFF, 11)
 *
 * The reference words (onfreq's DPL/DCS page, printed most significant bit first) are pinned
 * before and after the mapping.
 */

#include <cstdint>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/Golay24.hpp>
#include <dsd-neo/runtime/analog_tones.h>
#include <stdio.h>

namespace {

constexpr uint32_t kMask = (1U << DSD_DCS_WORD_BITS) - 1U;

int g_failures = 0;

/* Every check runs and reports; main() fails if any did. */
void
check(bool ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "DSP_ANALOG_DCS_GOLAY_XCHECK: %s\n", what);
        ++g_failures;
    }
}

uint32_t
reverse_bits(uint32_t value, int bits) {
    uint32_t out = 0U;
    for (int i = 0; i < bits; i++) {
        if (((value >> i) & 1U) != 0U) {
            out |= 1U << (bits - 1 - i);
        }
    }
    return out;
}

uint32_t
rotate_left23(uint32_t word, int k) {
    k %= DSD_DCS_WORD_BITS;
    if (k == 0) {
        return word & kMask;
    }
    return ((word << k) | (word >> (DSD_DCS_WORD_BITS - k))) & kMask;
}

/* A 23-bit word Golay24.hpp accepts as a code word: correct() leaves it alone and reports
   nothing wrong. */
bool
golay24_accepts(uint32_t word) {
    int errs = -1;
    unsigned int detected = 1U;
    const unsigned int corrected = Golay24::correct(word, &errs, &detected);
    return corrected == word && errs == 0 && detected == 0U;
}

/* The DCS word in Golay24.hpp's layout. */
uint32_t
to_golay24(uint32_t dcs_word) {
    return reverse_bits(dcs_word, DSD_DCS_WORD_BITS);
}

/* Golay24.hpp's own encoding of a DCS word's data bits, in its layout. */
uint32_t
golay24_encoding_of(uint32_t dcs_word) {
    const uint32_t data = reverse_bits(dcs_word & 0xFFFU, 12);
    return rotate_left23(Golay24::encode(data) & kMask, 11);
}

void
test_every_word_is_a_golay24_word() {
    int not_valid_unmapped = 0;
    for (int code = 0; code <= DSD_DCS_CODE_MAX; code++) {
        for (int inverted = 0; inverted < 2; inverted++) {
            const uint32_t word = dsd_dcs_word(code, inverted);
            check(word != 0U, "word != 0U");
            const uint32_t mapped = to_golay24(word);
            check(golay24_accepts(mapped), "golay24_accepts(mapped)");
            if (!golay24_accepts(word)) {
                not_valid_unmapped++;
            }
            /* One bit wrong anywhere: Golay24.hpp corrects it back to the mapped word. */
            for (int bit = 0; bit < DSD_DCS_WORD_BITS; bit++) {
                int errs = 0;
                unsigned int detected = 0U;
                check(Golay24::correct(mapped ^ (1U << bit), &errs, &detected) == mapped,
                      "Golay24::correct(mapped ^ (1U << bit), &errs, &detected) == mapped");
                check(errs == 1, "errs == 1");
            }
        }
        /* The layout mapping, stated exactly, for the normal word (the inverted one is its
           complement, and the all-ones word is in the code). */
        const uint32_t word = dsd_dcs_word(code, 0);
        check(to_golay24(word) == golay24_encoding_of(word), "to_golay24(word) == golay24_encoding_of(word)");
    }
    /* Without the reversal the check means nothing: most words would fail it. */
    check(not_valid_unmapped > DSD_DCS_CODE_MAX, "not_valid_unmapped > DSD_DCS_CODE_MAX");
}

/*
 * The cited words, before and after the mapping:
 *   023  11101100011-100-000/010/011 = 0x763813  ->  Golay24.hpp 0x640E37
 *   023 inverted  00010011100-011-111/101/100 = 0x09C7EC  ->  0x1BF1C8
 *   000  11000111010-100-000/000/000 = 0x63A800  ->  0x000AE3, POLY itself
 */
void
test_reference_words() {
    check(dsd_dcs_word(0023, 0) == 0x763813U, "dsd_dcs_word(0023, 0) == 0x763813U");
    check(to_golay24(0x763813U) == 0x640E37U, "to_golay24(0x763813U) == 0x640E37U");
    check(golay24_accepts(0x640E37U), "golay24_accepts(0x640E37U)");
    check(!golay24_accepts(0x763813U), "!golay24_accepts(0x763813U)");

    check(dsd_dcs_word(0023, 1) == 0x09C7ECU, "dsd_dcs_word(0023, 1) == 0x09C7ECU");
    check(to_golay24(0x09C7ECU) == 0x1BF1C8U, "to_golay24(0x09C7ECU) == 0x1BF1C8U");
    check(golay24_accepts(0x1BF1C8U), "golay24_accepts(0x1BF1C8U)");

    check(dsd_dcs_word(0, 0) == 0x63A800U, "dsd_dcs_word(0, 0) == 0x63A800U");
    check(to_golay24(0x63A800U) == 0x000AE3U, "to_golay24(0x63A800U) == 0x000AE3U");
    check(to_golay24(0x63A800U) == static_cast<unsigned int>(POLY), "to_golay24(0x63A800U) == POLY");
    check(golay24_accepts(0x000AE3U), "golay24_accepts(0x000AE3U)");
}

/* dsd_dcs_match() names only Golay24.hpp code words: a window that is not one (one bit off a
   real signal) never matches, however it is rotated. */
void
test_match_needs_a_code_word() {
    for (int i = 0; i < DSD_DCS_CODE_COUNT; i++) {
        const uint32_t word = dsd_dcs_word(dsd_dcs_code(i), 0);
        for (int bit = 0; bit < DSD_DCS_WORD_BITS; bit++) {
            const uint32_t damaged = word ^ (1U << bit);
            check(!golay24_accepts(to_golay24(damaged)), "!golay24_accepts(to_golay24(damaged))");
            check(dsd_dcs_match(damaged, nullptr, nullptr) == 0, "dsd_dcs_match(damaged, nullptr, nullptr) == 0");
        }
    }
}

} // namespace

int
main() {
    test_reference_words();
    test_every_word_is_a_golay24_word();
    test_match_needs_a_code_word();
    if (g_failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stdout, "DSP_ANALOG_DCS_GOLAY_XCHECK: OK\n");
    return 0;
}
