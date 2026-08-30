// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * EDACS control-frame verdict (issue #391).
 *
 * edacs() consumes 240 dibits before it looks at anything, and the SPS hunt pays the profile
 * for them unless the handler says the frame validated nothing. The tuned-trunk early-out
 * deliberately forgoes acting on the message, and used to return the same zero a failed BCH
 * returns -- so "we chose not to decode" was reported as "the check failed", which is the
 * direction #391 warns about. These cases drive the real edacs() over a synthetic dibit
 * stream, with get_dibit_and_analog_signal() wrapped at link time.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/protocol/edacs/edacs_bch.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "edacs_internal.h"

/* The 240-bit control frame edacs_collect_bits() reads, one bit per call. */
static int g_frame_bits[240];
static int g_bit_pos;
static int g_dibit_calls;

// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    g_dibit_calls++;
    if (g_bit_pos >= (int)(sizeof(g_frame_bits) / sizeof(g_frame_bits[0]))) {
        return 0;
    }
    return g_frame_bits[g_bit_pos++];
}

/* One 40-bit codeword, MSB first, into bits[offset .. offset + 39]. */
static void
write_codeword(int offset, unsigned long long int codeword) {
    for (int i = 0; i < 40; i++) {
        g_frame_bits[offset + i] = (int)((codeword >> (39 - i)) & 1ULL);
    }
}

/* The on-air layout edacs_build_raw_frames()/edacs_vote_frames() expect: each message sent
 * three times, with the middle copy inverted. */
static void
build_frame(unsigned long long int cw_1, unsigned long long int cw_2) {
    write_codeword(0, cw_1);
    write_codeword(40, (~cw_1) & 0xFFFFFFFFFFULL);
    write_codeword(80, cw_1);
    write_codeword(120, cw_2);
    write_codeword(160, (~cw_2) & 0xFFFFFFFFFFULL);
    write_codeword(200, cw_2);
    g_bit_pos = 0;
    g_dibit_calls = 0;
}

static unsigned long long int
codeword_for(unsigned long long int message) {
    return edacs_bch(message) & 0xFFFFFFFFFFULL;
}

/* The vote and the two BCH re-derivations, on their own. */
static void
test_frame_bch_verdict_reads_the_voted_frames(void) {
    unsigned long long int msg_1_ec = 0;
    unsigned long long int msg_2_ec = 0;
    const unsigned long long int cw_1 = codeword_for(0x0123456ULL);
    const unsigned long long int cw_2 = codeword_for(0x0ABCDEFULL);

    build_frame(cw_1, cw_2);
    assert(edacs_frame_bch_verdict(g_frame_bits, &msg_1_ec, &msg_2_ec) == 1);
    assert(msg_1_ec == cw_1);
    assert(msg_2_ec == cw_2);
    assert((msg_1_ec >> 12) == 0x0123456ULL);
    assert((msg_2_ec >> 12) == 0x0ABCDEFULL);

    /* One flipped message bit in every copy outvotes the majority and breaks the BCH. */
    build_frame(cw_1 ^ (1ULL << 39), cw_2);
    assert(edacs_frame_bch_verdict(g_frame_bits, &msg_1_ec, &msg_2_ec) == 0);

    /* The output pointers are optional. */
    build_frame(cw_1, cw_2);
    assert(edacs_frame_bch_verdict(g_frame_bits, NULL, NULL) == 1);
}

/* The fix: a tuned trunk forgoes acting on the message, but the frame it read still either
 * checked out or did not, and edacs() reports which. Before #391's follow-up this returned 0
 * for a frame whose BCH held, telling the SPS hunt that 240 dibits of real control channel
 * validated nothing. */
static void
test_tuned_early_out_reports_the_real_verdict(void) {
    static dsd_opts opts;
    static dsd_state state;
    const unsigned long long int cw_1 = codeword_for(0x0123456ULL);
    const unsigned long long int cw_2 = codeword_for(0x0ABCDEFULL);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_is_tuned = 1;

    build_frame(cw_1, cw_2);
    assert(edacs(&opts, &state) == 1);
    /* Exactly the frame and no more: the early-out still forgoes the message itself, so no
     * source identity is taken from stale control symbols. */
    assert(g_dibit_calls == 240);

    /* A frame that really did fail its BCH still reports the failure while tuned. */
    build_frame(cw_1 ^ (1ULL << 39), cw_2);
    assert(edacs(&opts, &state) == 0);
    assert(g_dibit_calls == 240);
}

/* Untuned, the BCH failure path is unchanged: 240 dibits gone, nothing decoded. */
static void
test_failed_bch_reports_zero_untuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    const unsigned long long int cw_1 = codeword_for(0x0123456ULL);
    const unsigned long long int cw_2 = codeword_for(0x0ABCDEFULL);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_is_tuned = 0;

    build_frame(cw_1, cw_2 ^ (1ULL << 39));
    assert(edacs(&opts, &state) == 0);
    assert(g_dibit_calls == 240);
}

int
main(void) {
    test_frame_bch_verdict_reads_the_voted_frames();
    test_tuned_early_out_reports_the_real_verdict();
    test_failed_bch_reports_zero_untuned();
    printf("EDACS_FRAME_VERDICT: OK\n");
    return 0;
}
