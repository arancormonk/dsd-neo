// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// Minimal DMR SM smoke test: grant tunes to VC; release returns to CC.

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    // Minimal behavior needed by the test
    if (opts) {
        opts->p25_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
    }
}

static void
init_opts_state(dsd_opts* opts, dsd_state* state) {
    memset(opts, 0, sizeof(*opts));
    memset(state, 0, sizeof(*state));
    opts->p25_trunk = 1;     // enable trunking logic
    opts->trunk_enable = 1;  // protocol-agnostic alias
    opts->use_rigctl = 0;    // avoid IO during test
    opts->audio_in_type = 0; // avoid RTL path
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 0.0f;    // no hangtime delay for this test
    state->p25_cc_freq = 851000000; // pretend CC
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_opts opts;
    dsd_state state;
    init_opts_state(&opts, &state);

    // Before grant, not tuned
    assert(opts.p25_is_tuned == 0);
    assert(state.p25_vc_freq[0] == 0);

    // Group grant with explicit frequency
    long vc = 852000000;
    dmr_sm_on_group_grant(&opts, &state, vc, /*lpcn*/ 0, /*tg*/ 101, /*src*/ 1234);

    // Expect tuned
    assert(opts.p25_is_tuned == 1);
    assert(state.p25_vc_freq[0] == vc);
    assert(state.p25_vc_freq[1] == vc);

    // Release (P_CLEAR equivalent)
    dmr_sm_on_release(&opts, &state);

    // Expect back on CC
    assert(opts.p25_is_tuned == 0);
    assert(state.p25_vc_freq[0] == 0);
    assert(state.p25_vc_freq[1] == 0);

    printf("DMR_T3_SM_BASIC: OK\n");
    return 0;
}
