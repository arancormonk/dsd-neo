// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 FACCH and SACCH as shortened, punctured RS(63,35) codes (issue #804).
 *
 * FACCH leaves RS(63,35) symbols 0..8 out as known zeros (shortened) and does not send 54..62 (punctured): an
 * RS(45,26,20) code that corrects 9 symbol errors once the 9 punctured symbols are erased. SACCH leaves out 0..4 and
 * 57..62: RS(52,30,23), 11 errors. The decoders used to erase the shortened symbols as well, which cut FACCH to 5
 * errors and SACCH to 8 and let about 1.2e-3 of random FACCH words pass, each one by "correcting" a symbol that is
 * zero on air.
 *
 * Every word here is built in full RS(63,35) coordinates: index i holds RS symbol i, so the shortened symbols are
 * sym[0..pad-1], the payload sym[pad..34], the sent parity sym[35..] and the punctured parity the rest. The test
 * carries its own RS(63,35) encoder (GF(64) on x^6+x+1, roots alpha^1..alpha^28, as ezpwd's RS<63,35>) to check
 * the wrapper encoders against the unshortened code and to build words that only the unshortened code accepts.
 * The ESS (RS(63,35) shortened by 19) is checked for the same correction-count unit.
 */

#include <algorithm>
#include <dsd-neo/fec/ez.h>
#include <exception>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>
#include "dsd-neo/core/safe_api.h"

namespace {

struct AcchFormat {
    const char* name;
    int pad;         // shortened symbols, RS index 0..pad-1
    int n_payload;   // payload hexbits, RS index pad..34
    int n_parity;    // sent parity hexbits, RS index 35..35+n_parity-1
    int correctable; // symbol errors the code corrects with the punctured symbols erased
    int (*decode)(int*, int*, const int*, int);
    int (*decode_fixed)(int*, int*);
    void (*encode)(const int*, int*);
};

const AcchFormat kFacch = {"FACCH", 9, 26, 19, 9, ez_rs28_facch, ez_rs28_facch_fixed, ez_rs28_facch_encode};
const AcchFormat kSacch = {"SACCH", 5, 30, 22, 11, ez_rs28_sacch, ez_rs28_sacch_fixed, ez_rs28_sacch_encode};

const int kFacchPunctured[9] = {54, 55, 56, 57, 58, 59, 60, 61, 62};
const int kSacchPunctured[6] = {57, 58, 59, 60, 61, 62};

using Word = std::vector<uint8_t>; // 63 symbols in full RS(63,35) coordinates

/* splitmix64: the same stream for a seed on every platform. */
struct Rng {
    uint64_t state;

    uint64_t
    operator()() {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

struct Gf64 {
    uint8_t exp[126];
    uint8_t log[64];
    uint8_t gen[29]; // RS(63,35) generator polynomial, highest degree first (gen[0] == 1)
};

uint8_t
gf_mul(const Gf64& gf, uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return gf.exp[gf.log[a] + gf.log[b]];
}

const Gf64&
gf64(void) {
    static const Gf64 gf = []() {
        Gf64 g = {};
        unsigned x = 1;
        for (int i = 0; i < 63; i++) {
            g.exp[i] = (uint8_t)x;
            g.exp[i + 63] = (uint8_t)x;
            g.log[x] = (uint8_t)i;
            x <<= 1;
            if ((x & 0x40U) != 0) {
                x ^= 0x43U;
            }
        }
        g.gen[0] = 1;
        for (int root = 1; root <= 28; root++) {
            for (int k = root; k > 0; k--) {
                g.gen[k] ^= gf_mul(g, g.gen[k - 1], g.exp[root]);
            }
        }
        return g;
    }();
    return gf;
}

/* Systematic encode of the unshortened code: sym[0..34] data in, sym[35..62] parity out. */
void
reference_encode(Word* sym) {
    const Gf64& gf = gf64();
    Word work = *sym;
    std::fill(work.begin() + 35, work.end(), (uint8_t)0);
    for (int i = 0; i < 35; i++) {
        uint8_t coef = work[(size_t)i];
        for (int j = 0; coef != 0 && j <= 28; j++) {
            work[(size_t)i + (size_t)j] ^= gf_mul(gf, coef, gf.gen[j]);
        }
    }
    std::copy(work.begin() + 35, work.end(), sym->begin() + 35);
}

struct Bits {
    int payload[180];
    int parity[132];
};

int
fail(const char* tag, const AcchFormat& f, int trial, int got) {
    DSD_FPRINTF(stderr, "%s %s: trial %d got %d\n", f.name, tag, trial, got);
    return 1;
}

const int*
punctured_list(const AcchFormat& f, int* count) {
    if (f.pad == kFacch.pad) {
        *count = 9;
        return kFacchPunctured;
    }
    *count = 6;
    return kSacchPunctured;
}

void
symbols_to_bits(const uint8_t* sym, int n, int* bits) {
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < 6; b++) {
            bits[(i * 6) + b] = (sym[i] >> (5 - b)) & 1;
        }
    }
}

void
bits_to_symbols(const int* bits, int n, uint8_t* sym) {
    for (int i = 0; i < n; i++) {
        uint8_t s = 0;
        for (int b = 0; b < 6; b++) {
            s = (uint8_t)((s << 1) | (bits[(i * 6) + b] & 1));
        }
        sym[i] = s;
    }
}

Bits
word_bits(const AcchFormat& f, const Word& w) {
    Bits bits = {};
    symbols_to_bits(&w[(size_t)f.pad], f.n_payload, bits.payload);
    symbols_to_bits(&w[35], f.n_parity, bits.parity);
    return bits;
}

/* A codeword of the shortened code, built with the wrapper encoder (punctured symbols left zero). */
Word
random_codeword(const AcchFormat& f, Rng& rng) {
    Word w(63, 0);
    for (int i = f.pad; i < 35; i++) {
        w[(size_t)i] = (uint8_t)(rng() & 0x3F);
    }
    Bits bits = word_bits(f, w);
    f.encode(bits.payload, bits.parity);
    bits_to_symbols(bits.parity, f.n_parity, &w[35]);
    return w;
}

/* XOR a nonzero value into n distinct sent symbols. */
void
corrupt(const AcchFormat& f, Word* w, int n, Rng& rng) {
    std::vector<int> positions;
    for (int i = f.pad; i < 35 + f.n_parity; i++) {
        positions.push_back(i);
    }
    for (size_t i = positions.size() - 1; i > 0; i--) {
        std::swap(positions[i], positions[(size_t)(rng() % (i + 1))]);
    }
    for (int e = 0; e < n; e++) {
        (*w)[(size_t)positions[(size_t)e]] ^= (uint8_t)(1 + (rng() % 63));
    }
}

bool
payload_matches(const AcchFormat& f, const int* payload, const Word& w) {
    Bits want = word_bits(f, w);
    for (int i = 0; i < f.n_payload * 6; i++) {
        if (payload[i] != want.payload[i]) {
            return false;
        }
    }
    return true;
}

int
decode_with_list(const AcchFormat& f, const Word& rx, const int* erasures, int n_erasures, Bits* out) {
    *out = word_bits(f, rx);
    return f.decode(out->payload, out->parity, erasures, n_erasures);
}

int
decode_fixed(const AcchFormat& f, const Word& rx, Bits* out) {
    int n = 0;
    const int* list = punctured_list(f, &n);
    return decode_with_list(f, rx, list, n, out);
}

/* n errors are corrected exactly, and the return value counts exactly those n sent symbols. */
int
expect_corrects(const AcchFormat& f, int n_errors, uint64_t seed) {
    Rng rng{seed};
    int rc = 0;
    for (int trial = 0; trial < 200; trial++) {
        Word cw = random_codeword(f, rng);
        Word rx = cw;
        corrupt(f, &rx, n_errors, rng);
        Bits out;
        int ec = decode_fixed(f, rx, &out);
        if (ec != n_errors) {
            rc |= fail("correction count", f, trial, ec);
        }
        if (!payload_matches(f, out.payload, cw)) {
            rc |= fail("payload", f, trial, ec);
        }
    }
    return rc;
}

int
expect_fails(const AcchFormat& f, int n_errors, uint64_t seed) {
    Rng rng{seed};
    int rc = 0;
    for (int trial = 0; trial < 200; trial++) {
        Word rx = random_codeword(f, rng);
        corrupt(f, &rx, n_errors, rng);
        Bits out;
        int ec = decode_fixed(f, rx, &out);
        if (ec >= 0) {
            rc |= fail("beyond the code's reach", f, trial, ec);
        }
    }
    return rc;
}

/* A codeword of the unshortened code with one nonzero shortened symbol; on air that symbol reads zero. */
Word
pad_violation_word(const AcchFormat& f, Rng& rng) {
    Word full(63, 0);
    for (int i = f.pad; i < 35; i++) {
        full[(size_t)i] = (uint8_t)(rng() & 0x3F);
    }
    full[(size_t)(rng() % (uint64_t)f.pad)] = (uint8_t)(1 + (rng() % 63));
    reference_encode(&full);
    Word w(63, 0);
    std::copy(full.begin() + f.pad, full.begin() + 35 + f.n_parity, w.begin() + f.pad);
    return w;
}

int
test_encoders_match_unshortened_code(void) {
    Rng rng{0x804101ULL};
    int rc = 0;
    for (const AcchFormat* f : {&kFacch, &kSacch}) {
        for (int trial = 0; trial < 100; trial++) {
            Word cw = random_codeword(*f, rng);
            Word full = cw;
            reference_encode(&full);
            for (int i = 0; i < f->n_parity; i++) {
                if (cw[35U + (size_t)i] != full[35U + (size_t)i]) {
                    rc |= fail("encoder parity", *f, trial, i);
                    break;
                }
            }
        }
    }
    return rc;
}

int
test_clean_codeword_reports_no_corrections(void) {
    Rng rng{0x804102ULL};
    int rc = 0;
    for (const AcchFormat* f : {&kFacch, &kSacch}) {
        for (int trial = 0; trial < 50; trial++) {
            Word cw = random_codeword(*f, rng);
            Bits out;
            int ec = decode_fixed(*f, cw, &out);
            if (ec != 0 || !payload_matches(*f, out.payload, cw)) {
                rc |= fail("clean codeword", *f, trial, ec);
            }
        }
    }
    return rc;
}

int
test_facch_corrects_nine_symbol_errors(void) {
    return expect_corrects(kFacch, 9, 0x804103ULL);
}

int
test_facch_ten_errors_fail(void) {
    return expect_fails(kFacch, 10, 0x804104ULL);
}

int
test_sacch_corrects_eleven_symbol_errors(void) {
    return expect_corrects(kSacch, 11, 0x804105ULL);
}

int
test_sacch_twelve_errors_fail(void) {
    return expect_fails(kSacch, 12, 0x804106ULL);
}

int
test_pad_correction_rejected(void) {
    Rng rng{0x804107ULL};
    int rc = 0;
    for (const AcchFormat* f : {&kFacch, &kSacch}) {
        for (int trial = 0; trial < 100; trial++) {
            Word rx = pad_violation_word(*f, rng);
            Bits sent = word_bits(*f, rx);
            Bits out;
            int ec = decode_fixed(*f, rx, &out);
            if (ec >= 0) {
                rc |= fail("pad correction accepted", *f, trial, ec);
            }
            Bits fixed_out = sent;
            ec = f->decode_fixed(fixed_out.payload, fixed_out.parity);
            if (ec >= 0) {
                rc |= fail("pad correction accepted (fixed entry)", *f, trial, ec);
            }
            for (int i = 0; i < f->n_payload * 6; i++) {
                if (out.payload[i] != sent.payload[i] || fixed_out.payload[i] != sent.payload[i]) {
                    rc |= fail("failed decode changed the payload", *f, trial, i);
                    break;
                }
            }
        }
    }
    return rc;
}

/* The pre-#804 lists erased the shortened symbols too; those (and out-of-range) entries must change nothing. */
int
test_shortened_indices_in_erasure_list_ignored(void) {
    static const int kFacchLegacy[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 54, 55, 56, 57, 58, 59, 60, 61, 62, -1, 63};
    static const int kSacchLegacy[13] = {0, 1, 2, 3, 4, 57, 58, 59, 60, 61, 62, -1, 63};
    Rng rng{0x804108ULL};
    int rc = 0;
    for (const AcchFormat* f : {&kFacch, &kSacch}) {
        const int* legacy = (f == &kFacch) ? kFacchLegacy : kSacchLegacy;
        int n_legacy = (f == &kFacch) ? 20 : 13;
        for (int trial = 0; trial < 100; trial++) {
            Word cw = random_codeword(*f, rng);
            Word rx = cw;
            corrupt(*f, &rx, f->correctable, rng);
            Bits out;
            int ec = decode_with_list(*f, rx, legacy, n_legacy, &out);
            if (ec != f->correctable || !payload_matches(*f, out.payload, cw)) {
                rc |= fail("legacy list, t errors", *f, trial, ec);
            }
            ec = decode_with_list(*f, pad_violation_word(*f, rng), legacy, n_legacy, &out);
            if (ec >= 0) {
                rc |= fail("legacy list, pad violation", *f, trial, ec);
            }
        }
    }
    return rc;
}

int
test_fixed_entry_matches_vector_entry(void) {
    Rng rng{0x804109ULL};
    int rc = 0;
    for (const AcchFormat* f : {&kFacch, &kSacch}) {
        for (int trial = 0; trial < 1000; trial++) {
            Word rx = random_codeword(*f, rng);
            corrupt(*f, &rx, (int)(rng() % (uint64_t)(f->correctable + 4)), rng);
            Bits by_list;
            int ec_list = decode_fixed(*f, rx, &by_list);
            Bits by_fixed = word_bits(*f, rx);
            int ec_fixed = f->decode_fixed(by_fixed.payload, by_fixed.parity);
            if (ec_list != ec_fixed) {
                rc |= fail("fixed entry return", *f, trial, ec_fixed);
            }
            for (int i = 0; i < f->n_payload * 6; i++) {
                if (by_list.payload[i] != by_fixed.payload[i]) {
                    rc |= fail("fixed entry payload", *f, trial, i);
                    break;
                }
            }
        }
    }
    return rc;
}

/*
 * The ESS (RS(44,16,29): RS(63,35) shortened by 19, every symbol sent, indices 0..43) reports the symbols its decode
 * changed, the unit of FACCH and SACCH: an erasure that held the right value is not a correction. ezpwd's own count
 * includes every erasure.
 */
int
test_ess_counts_changed_symbols(void) {
    static const int kErrors[3] = {1, 20, 30};              // wrong, not erased
    static const int kWrongErased[2] = {8, 35};             // wrong and erased
    static const int kErasures[6] = {8, 35, 5, 10, 25, 40}; // 5, 10, 25 and 40 hold the right value
    Rng rng{0x80410AULL};
    int rc = 0;
    for (int trial = 0; trial < 50; trial++) {
        Word full(63, 0);
        for (int i = 19; i < 35; i++) {
            full[(size_t)i] = (uint8_t)(rng() & 0x3F);
        }
        reference_encode(&full);
        Word rx = full;
        for (int pos : kErrors) {
            rx[19U + (size_t)pos] ^= (uint8_t)(1 + (rng() % 63));
        }
        for (int pos : kWrongErased) {
            rx[19U + (size_t)pos] ^= (uint8_t)(1 + (rng() % 63));
        }
        int payload[96];
        int parity[168];
        symbols_to_bits(&rx[19], 16, payload);
        symbols_to_bits(&rx[35], 28, parity);
        int ec = ez_rs28_ess(payload, parity, kErasures, 6);
        int want[96];
        symbols_to_bits(&full[19], 16, want);
        bool exact = true;
        for (int i = 0; i < 96; i++) {
            exact = exact && payload[i] == want[i];
        }
        if (ec != 5 || !exact) {
            DSD_FPRINTF(stderr, "ESS changed-symbol count: trial %d got %d want 5 (payload %s)\n", trial, ec,
                        exact ? "exact" : "wrong");
            rc = 1;
        }
    }
    return rc;
}

/* Wrongly descrambled bursts look random; the full RS check now rejects nearly all of them (base: ~240 passes). */
int
test_random_words_rarely_pass(void) {
    Rng rng{0x804804ULL};
    int passes = 0;
    for (int trial = 0; trial < 200000; trial++) {
        int payload[156];
        int parity[114];
        for (int& bit : payload) {
            bit = (int)(rng() & 1);
        }
        for (int& bit : parity) {
            bit = (int)(rng() & 1);
        }
        if (ez_rs28_facch_fixed(payload, parity) >= 0) {
            passes++;
        }
    }
    if (passes > 2) {
        DSD_FPRINTF(stderr, "FACCH random words: %d of 200000 passed\n", passes);
        return 1;
    }
    return 0;
}

} // namespace

int
main(void) try {
    int rc = 0;
    rc |= test_encoders_match_unshortened_code();
    rc |= test_clean_codeword_reports_no_corrections();
    rc |= test_facch_corrects_nine_symbol_errors();
    rc |= test_facch_ten_errors_fail();
    rc |= test_sacch_corrects_eleven_symbol_errors();
    rc |= test_sacch_twelve_errors_fail();
    rc |= test_pad_correction_rejected();
    rc |= test_shortened_indices_in_erasure_list_ignored();
    rc |= test_fixed_entry_matches_vector_entry();
    rc |= test_ess_counts_changed_symbols();
    rc |= test_random_words_rarely_pass();
    return rc;
} catch (const std::exception& e) {
    DSD_FPRINTF(stderr, "Unhandled exception: %s\n", e.what());
    return 1;
} catch (...) {
    DSD_FPRINTF(stderr, "Unhandled exception\n");
    return 1;
}
