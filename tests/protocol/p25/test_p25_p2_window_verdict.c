// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 Phase 2 window verdict (p25p2_window_verdict.h): what a collected window's own ISCHs say about where it starts
 * in the superframe, and the verdict on a window a tolerant sync opened. Windows are built as a site sends them
 * (p25p2_burst_builder.h), whose ISCH placement encodes the superframe layout independently of the verdict's.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/fec/p25p2_isch.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "p25p2_burst_builder.h"
#include "p25p2_frame_internal.h"
#include "p25p2_window_verdict.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];

#define TEST_CODE 0xBEE00164161ULL

enum {
    WINDOW_BITS = 1440,
    COLLECTED_BITS = 1400,
    STRONG_LLR = 200,
    DUID_4V = 0,
    DUID_SACCH_S = 3,
    DUID_FACCH_S = 9,
    DUID_SACCH = 12,
};

static const int k_starts[6] = {2, 3, 6, 7, 10, 11};

static int g_failures;

static void
check(int cond, const char* what, int a, int b) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s (%d, %d)\n", what, a, b);
        g_failures++;
    }
}

/* The window (as int bits, the p2bit layout) and its LLRs: every bit read with a strong LLR. */
static int g_bits[WINDOW_BITS];
static int16_t g_llr[COLLECTED_BITS];

static void
load(const uint8_t built[WINDOW_BITS]) {
    for (int i = 0; i < WINDOW_BITS; i++) {
        g_bits[i] = built[i];
    }
    for (int i = 0; i < COLLECTED_BITS; i++) {
        g_llr[i] = (int16_t)(g_bits[i] ? STRONG_LLR : -STRONG_LLR);
    }
}

static void
build(int t0) {
    p25p2_bb_slot slots[4];
    DSD_MEMSET(slots, 0, sizeof(slots));
    slots[0].duid = DUID_4V;
    slots[1].duid = DUID_SACCH_S;
    slots[1].fill_crc = 1;
    slots[2].duid = DUID_FACCH_S;
    slots[2].fill_crc = 1;
    slots[3].duid = DUID_SACCH;
    slots[3].fill_crc = 1;
    uint8_t built[WINDOW_BITS];
    p25p2_bb_window(TEST_CODE, t0, slots, built);
    load(built);
}

static void
set_isch(int fc, uint64_t word) {
    for (int i = 0; i < 40; i++) {
        g_bits[320 + (360 * fc) + i] = (int)((word >> (39 - i)) & 1U);
        g_llr[320 + (360 * fc) + i] = (int16_t)(g_bits[320 + (360 * fc) + i] ? STRONG_LLR : -STRONG_LLR);
    }
}

/* The all-zero word lies at least 10 bits from every ISCH codeword: nothing decodes it, hard or soft. */
static void
erase_isch(int fc) {
    set_isch(fc, 0U);
}

static uint64_t
i_isch(int chan, int loc) {
    return dsd_p25p2_i_isch_codewords[(chan << 5) | (loc << 3)];
}

/* The window's four DUIDs as dispatch reads them. */
static void
window_duids(int duids[4]) {
    for (int i = 0; i < COLLECTED_BITS; i++) {
        p2bit[i] = g_bits[i];
        p2llr[i] = g_llr[i];
    }
    for (int ts = 0; ts < 4; ts++) {
        duids[ts] = p25p2_window_duid(ts);
    }
}

static p25p2_window_verdict
verdict(int start) {
    int duids[4];
    window_duids(duids);
    return p25p2_tolerant_window_verdict(g_bits, g_llr, start, 1, duids);
}

static uint64_t
flip_bits(uint64_t word, int count, int first) {
    for (int i = 0; i < count; i++) {
        word ^= 1ULL << ((first + (i * 7)) % 40);
    }
    return word;
}

/* The ISCH code: every word decodes to itself within 3 bit errors (and further: the unique radius is 6), the words lie
   at least 14 bits apart, and the all-zero word at least 10 from every one, which is why the fourth ISCH a window
   never collects (always zero in p2bit) decodes to nothing. */
static void
test_isch_code(void) {
    uint64_t words[DSD_P25P2_I_ISCH_CODEWORDS + 1];
    for (int i = 0; i < DSD_P25P2_I_ISCH_CODEWORDS; i++) {
        words[i] = dsd_p25p2_i_isch_codewords[i];
    }
    words[DSD_P25P2_I_ISCH_CODEWORDS] = DSD_P25P2_S_ISCH;
    int min_distance = 40;
    for (int i = 0; i <= DSD_P25P2_I_ISCH_CODEWORDS; i++) {
        for (int j = i + 1; j <= DSD_P25P2_I_ISCH_CODEWORDS; j++) {
            const int d = dsd_popcount64(words[i] ^ words[j]);
            min_distance = d < min_distance ? d : min_distance;
        }
        for (int errors = 0; errors <= 6; errors++) {
            int bits = -1;
            const int got = dsd_p25p2_isch_nearest(flip_bits(words[i], errors, i), &bits);
            check(got == i && bits == errors, "codeword decodes to itself", i, errors);
        }
    }
    check(min_distance == 14, "minimum distance", min_distance, 14);
    int zero_bits = 0;
    (void)dsd_p25p2_isch_nearest(0U, &zero_bits);
    check(zero_bits >= 10, "zero word is far from every codeword", zero_bits, 10);
    for (int i = 0; i < WINDOW_BITS; i++) {
        g_bits[i] = 0;
    }
    for (int start = 0; start < 12; start++) {
        for (int fc = 0; fc < 3; fc++) {
            check(p25p2_window_isch_check(g_bits, start, fc) == P25P2_ISCH_NONE, "zero word is no evidence", start, fc);
        }
    }
}

/* The superframe layout the verdict predicts is the one the builder sends, at every start a sync can open (an S-ISCH
   before it), and both I-ISCHs and the S-ISCH of each such window match. */
static void
test_layout(void) {
    for (size_t s = 0; s < 6; s++) {
        const int start = k_starts[s];
        build(start);
        p25p2_window_evidence evidence;
        p25p2_window_isch_evidence(g_bits, start, &evidence);
        check(evidence.i_isch_matches == 2 && evidence.s_isch_matches == 1 && evidence.contradictions == 0,
              "a clean window matches its start", start, evidence.i_isch_matches);
        for (int fc = 0; fc < 3; fc++) {
            int chan = 0;
            int loc = 0;
            const int expected_i = p25p2_window_isch_expected(start, fc, &chan, &loc);
            int bits = 0;
            const int got = dsd_p25p2_isch_nearest(p25p2_window_isch_word(g_bits, fc), &bits);
            check(bits == 0, "builder ISCH is a codeword", start, fc);
            if (expected_i) {
                check(got == ((chan << 5) | (loc << 3)), "builder I-ISCH names the predicted channel and location",
                      start, fc);
            } else {
                check(got == DSD_P25P2_ISCH_NEAREST_S_ISCH, "builder S-ISCH where predicted", start, fc);
            }
        }
        /* The S-ISCH sits at fc 0 for a window starting on slot 0's timeslot, at fc 2 for slot 1's. */
        int chan = 0;
        int loc = 0;
        check(p25p2_window_isch_expected(start, (start & 1) ? 2 : 0, &chan, &loc) == 0, "S-ISCH position", start, 0);
        /* The soft read processP2() positions an exact window by agrees. */
        check(p25p2_window_soft_offset(g_bits, g_llr) % 12 == start, "soft read locates the start", start,
              p25p2_window_soft_offset(g_bits, g_llr));
        /* Every other start is contradicted. */
        for (int other = 0; other < 12; other++) {
            if (other != start) {
                p25p2_window_isch_evidence(g_bits, other, &evidence);
                check(evidence.contradictions > 0, "another start is contradicted", start, other);
            }
        }
    }
}

/* The verdicts on a window a tolerant sync opened. */
static void
test_verdicts(void) {
    for (size_t s = 0; s < 6; s++) {
        const int start = k_starts[s];
        const int s_fc = (start & 1) ? 2 : 0;
        const int i_fc[2] = {(start & 1) ? 0 : 1, (start & 1) ? 1 : 2};

        build(start);
        check(verdict(start) == P25P2_WINDOW_PASS, "clean window passes", start, 0);
        int clean_duids[4];
        window_duids(clean_duids);
        check(p25p2_tolerant_window_verdict(g_bits, g_llr, start, 0, clean_duids) == P25P2_WINDOW_FAIL_CONTEXT,
              "context gone fails", start, 0);
        /* The anchor predicting another start: contradicted. */
        check(verdict((start + 4) % 12) == P25P2_WINDOW_FAIL_CONTRADICTED, "wrong predicted start fails", start, 0);

        /* Only the S-ISCH; only one I-ISCH (either): corroborated. */
        build(start);
        erase_isch(i_fc[0]);
        erase_isch(i_fc[1]);
        check(verdict(start) == P25P2_WINDOW_PASS, "S-ISCH alone passes", start, 0);
        for (int k = 0; k < 2; k++) {
            build(start);
            erase_isch(s_fc);
            erase_isch(i_fc[1 - k]);
            check(verdict(start) == P25P2_WINDOW_PASS, "one I-ISCH alone passes", start, k);
        }
        /* Corroboration within the evidence limits: an I-ISCH 3 bits off and an S-ISCH 4 off count, 4 and 5 do not. */
        build(start);
        erase_isch(s_fc);
        erase_isch(i_fc[1]);
        int chan = 0;
        int loc = 0;
        (void)p25p2_window_isch_expected(start, i_fc[0], &chan, &loc);
        set_isch(i_fc[0], flip_bits(i_isch(chan, loc), 3, 1));
        check(verdict(start) == P25P2_WINDOW_PASS, "I-ISCH 3 bits off corroborates", start, 0);
        build(start);
        erase_isch(i_fc[0]);
        erase_isch(i_fc[1]);
        set_isch(s_fc, flip_bits(DSD_P25P2_S_ISCH, 4, 2));
        check(verdict(start) == P25P2_WINDOW_PASS, "S-ISCH 4 bits off corroborates", start, 0);
        set_isch(s_fc, flip_bits(DSD_P25P2_S_ISCH, 5, 2));
        check(verdict(start) == P25P2_WINDOW_FAIL_UNCORROBORATED, "S-ISCH 5 bits off does not", start, 0);

        /* Nothing: uncorroborated. */
        build(start);
        erase_isch(0);
        erase_isch(1);
        erase_isch(2);
        check(verdict(start) == P25P2_WINDOW_FAIL_UNCORROBORATED, "no ISCH fails", start, 0);

        /* Contradictions: another location, the other channel, an I-ISCH where the S-ISCH belongs and the S-ISCH
           where an I-ISCH belongs. */
        build(start);
        (void)p25p2_window_isch_expected(start, i_fc[0], &chan, &loc);
        set_isch(i_fc[0], i_isch(chan, (loc + 1) % 3));
        check(verdict(start) == P25P2_WINDOW_FAIL_CONTRADICTED, "another location fails", start, 0);
        build(start);
        set_isch(i_fc[0], i_isch(1 - chan, loc));
        check(verdict(start) == P25P2_WINDOW_FAIL_CONTRADICTED, "the other channel fails", start, 0);
        build(start);
        set_isch(s_fc, i_isch(0, 1));
        check(verdict(start) == P25P2_WINDOW_FAIL_CONTRADICTED, "I-ISCH at the S-ISCH fails", start, 0);
        build(start);
        set_isch(i_fc[1], DSD_P25P2_S_ISCH);
        check(verdict(start) == P25P2_WINDOW_FAIL_CONTRADICTED, "S-ISCH at an I-ISCH fails", start, 0);

        /* The soft read reaches further than the hard evidence: a channel-1 I-ISCH of another location 5 bits off is
           no hard evidence either way, but would position the window elsewhere. */
        build(start);
        (void)p25p2_window_isch_expected(start, i_fc[1], &chan, &loc);
        set_isch(i_fc[1], flip_bits(i_isch(1, (loc + 1) % 3), 5, 3));
        check(p25p2_window_isch_check(g_bits, start, i_fc[1]) == P25P2_ISCH_NONE, "5 bits off is no hard evidence",
              start, 0);
        check(verdict(start) == P25P2_WINDOW_FAIL_CONTRADICTED, "soft-located start disagreeing fails", start, 0);

        /* DUIDs: one undecodable passes, two fail. */
        build(start);
        int duids[4];
        window_duids(duids);
        duids[3] = -1;
        check(p25p2_tolerant_window_verdict(g_bits, g_llr, start, 1, duids) == P25P2_WINDOW_PASS,
              "one DUID error passes", start, 0);
        duids[1] = 7;
        check(p25p2_tolerant_window_verdict(g_bits, g_llr, start, 1, duids) == P25P2_WINDOW_FAIL_DUIDS,
              "two DUID errors fail", start, 0);
    }
}

/* A window read 1 to 5 dibits off where it starts (a slipped sync accepted within the tolerance): its ISCHs are then
   the real ones shifted, which name nothing at the predicted start, and the verdict fails, in both start classes. */
static void
test_misread_windows(void) {
    for (size_t s = 0; s < 6; s++) {
        const int start = k_starts[s];
        /* Three windows in a row as the carrier sends them, each after its 40-bit sync. */
        static uint8_t stream[3 * WINDOW_BITS];
        for (int w = 0; w < 3; w++) {
            p25p2_bb_slot slots[4];
            DSD_MEMSET(slots, 0, sizeof(slots));
            slots[0].duid = DUID_4V;
            slots[1].duid = DUID_FACCH_S;
            slots[1].fill_crc = 1;
            slots[2].duid = DUID_4V;
            slots[3].duid = DUID_SACCH_S;
            slots[3].fill_crc = 1;
            uint8_t built[WINDOW_BITS];
            p25p2_bb_window(TEST_CODE, (start + (4 * (w - 1)) + 12) % 12, slots, built);
            /* The window's bits follow the sync before it, which is the previous window's last 40. */
            DSD_MEMCPY(stream + (size_t)w * WINDOW_BITS, built, WINDOW_BITS);
        }
        const int at = WINDOW_BITS; /* the middle window's first bit */
        for (int k = -5; k <= 5; k++) {
            if (k == 0) {
                continue;
            }
            uint8_t shifted[WINDOW_BITS];
            const ptrdiff_t from = (ptrdiff_t)at + ((ptrdiff_t)2 * k);
            DSD_MEMCPY(shifted, stream + from, WINDOW_BITS);
            load(shifted);
            check(verdict(start) != P25P2_WINDOW_PASS, "misread window fails", start, k);
        }
        uint8_t aligned[WINDOW_BITS];
        DSD_MEMCPY(aligned, stream + at, WINDOW_BITS);
        load(aligned);
        check(verdict(start) == P25P2_WINDOW_PASS, "aligned window passes", start, 0);
    }
}

/* The fourth ISCH (bits 1400-1439, the next sync) is never collected: no read returns it, whatever it holds. */
static void
test_fourth_isch_never_read(void) {
    build(3);
    for (int i = 0; i < 40; i++) {
        g_bits[1400 + i] = (int)((i_isch(1, 0) >> (39 - i)) & 1U);
    }
    check(p25p2_window_isch_word(g_bits, 3) == 0U, "fourth ISCH word is not read", 0, 0);
    check(p25p2_window_isch_check(g_bits, 3, 3) == P25P2_ISCH_NONE, "fourth ISCH is no evidence", 0, 0);
    check(p25p2_window_isch_soft_decode(g_bits, g_llr, 3) == -2, "fourth ISCH does not decode", 0, 0);
    check(p25p2_window_soft_offset(g_bits, g_llr) % 12 == 3, "fourth ISCH moves nothing", 0, 0);
}

/* The DUIDs the burst dispatch handles (p25p2_duid_dispatch()); anything else counts toward aborting a window. */
static void
test_duid_dispatch_set(void) {
    static const int handled[] = {0, 3, 4, 6, 9, 10, 12, 13, 15};
    int expected[16] = {0};
    for (size_t i = 0; i < sizeof(handled) / sizeof(handled[0]); i++) {
        expected[handled[i]] = 1;
    }
    for (int duid = 0; duid < 16; duid++) {
        check(p25p2_window_duid_dispatches(duid) == expected[duid], "DUID dispatch set", duid, expected[duid]);
    }
    check(!p25p2_window_duid_dispatches(-1) && !p25p2_window_duid_dispatches(-2), "no DUID is not dispatched", 0, 0);
}

int
main(void) {
    test_isch_code();
    test_layout();
    test_verdicts();
    test_misread_windows();
    test_fourth_isch_never_read();
    test_duid_dispatch_set();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("P25_P2_WINDOW_VERDICT: OK\n");
    return 0;
}
