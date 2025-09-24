// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * End-to-end DMR Tier III trunking shim tests:
 * - Neighbor/alternate CC candidates
 * - Explicit frequency grants
 * - LPCN-derived grants with trust gating (on-CC vs off-CC)
 * - Release handling: slot activity + hangtime
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>

// Rigctl/RTL and CC-return stubs to avoid external I/O and core linkage
bool
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
SetModulation(int sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return false;
}

long int
GetCurrentFreq(int sockfd) {
    (void)sockfd;
    return 0;
}
struct RtlSdrContext;
struct RtlSdrContext* g_rtl_ctx = 0;

int
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
    }
}

// Provide local stubs to satisfy DMR SM linker deps
void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || freq <= 0) {
        return;
    }
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = time(NULL);
}

static void
init_env(dsd_opts* opts, dsd_state* state) {
    memset(opts, 0, sizeof(*opts));
    memset(state, 0, sizeof(*state));
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->use_rigctl = 0;
    opts->audio_in_type = 0;
    state->p25_cc_freq = 851000000; // mock CC
}

static void
test_neighbor_candidates(void) {
    dsd_opts opts;
    dsd_state state;
    init_env(&opts, &state);

    long cand[3] = {851012500, 852500000, 0};
    dmr_sm_on_neighbor_update(&opts, &state, cand, 3);
    assert(state.p25_cc_cand_count >= 2);

    long next = 0;
    int ok = dmr_sm_next_cc_candidate(&state, &next);
    assert(ok == 1);
    assert(next == cand[0] || next == cand[1]);
}

static void
test_explicit_grant_and_release(void) {
    dsd_opts opts;
    dsd_state state;
    init_env(&opts, &state);

    long vc = 852000000;
    dmr_sm_on_group_grant(&opts, &state, vc, /*lpcn*/ 0, /*tg*/ 1001, /*src*/ 42);
    assert(opts.p25_is_tuned == 1);
    assert(state.p25_vc_freq[0] == vc);

    // Slot activity prevents return (DMR VOICE=16)
    state.dmrburstL = 16; // active voice
    dmr_sm_on_release(&opts, &state);
    assert(opts.p25_is_tuned == 1);

    // Clear activity; hangtime defers
    state.dmrburstL = 24; // idle
    opts.trunk_hangtime = 1.0f;
    state.last_t3_tune_time = time(NULL);
    dmr_sm_on_release(&opts, &state);
    assert(opts.p25_is_tuned == 1);

    // After hangtime, return to CC
    state.last_t3_tune_time = time(NULL) - 2; // ensure elapsed > hangtime
    dmr_sm_on_release(&opts, &state);
    assert(opts.p25_is_tuned == 0);
}

static void
test_lpcn_trust_gating(void) {
    dsd_opts opts;
    dsd_state state;
    init_env(&opts, &state);

    // On CC (p25_is_tuned==0): allow tuning with untrusted LPCN mapping
    int lpcn = 0x0123;
    long f1 = 853000000;
    state.trunk_chan_map[lpcn] = f1;
    state.dmr_lcn_trust[lpcn] = 1; // unconfirmed
    opts.p25_is_tuned = 0;         // on CC
    dmr_sm_on_group_grant(&opts, &state, /*freq_hz*/ 0, lpcn, /*tg*/ 101, /*src*/ 99);
    assert(opts.p25_is_tuned == 1);
    assert(state.p25_vc_freq[0] == f1);

    // Off CC (currently tuned to VC): block tune with untrusted mapping
    int lpcn2 = 0x0124;
    long f2 = 854000000;
    state.trunk_chan_map[lpcn2] = f2;
    state.dmr_lcn_trust[lpcn2] = 1; // unconfirmed
    long prev = state.p25_vc_freq[0];
    opts.p25_is_tuned = 1; // off CC
    dmr_sm_on_group_grant(&opts, &state, /*freq_hz*/ 0, lpcn2, /*tg*/ 101, /*src*/ 99);
    assert(state.p25_vc_freq[0] == prev); // unchanged (blocked)
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    test_neighbor_candidates();
    test_explicit_grant_and_release();
    test_lpcn_trust_gating();
    printf("DMR_T3_SHIM: OK\n");
    return 0;
}
