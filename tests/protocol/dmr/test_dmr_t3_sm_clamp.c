// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// DMR SM clamp test: deny untrusted LPCN mapping off-CC; allow when on-CC.

#include <assert.h>
#include <string.h>

#define MBELIB_NO_HEADERS 1
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>

// Stubs to avoid linking IO/rigctl
void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (opts) {
        opts->p25_is_tuned = 1;
    }
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->p25_vc_freq[0] = freq;
        state->trunk_vc_freq[0] = freq;
    }
}

void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->trunk_vc_freq[0] = 0;
    }
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
init_opts_state(dsd_opts* opts, dsd_state* state) {
    memset(opts, 0, sizeof(*opts));
    memset(state, 0, sizeof(*state));
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->trunk_hangtime = 0.0f;
}

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_opts opts;
    dsd_state state;
    init_opts_state(&opts, &state);

    // Map LCN 100 -> 851.0125 MHz but mark as untrusted (1)
    int lcn = 100;
    long freq = 851012500;
    state.trunk_chan_map[lcn] = freq;
    state.dmr_lcn_trust[lcn] = 1; // learned off-CC

    // Off-CC: should NOT tune
    opts.p25_is_tuned = 1;         // simulate on a VC
    state.p25_cc_freq = 851000000; // known CC
    dmr_sm_on_group_grant(&opts, &state, /*freq_hz*/ 0, /*lpcn*/ lcn, /*tg*/ 1234, /*src*/ 0);
    assert(opts.p25_is_tuned == 1); // unchanged
    assert(state.p25_vc_freq[0] == 0);

    // On-CC: allow tuning with untrusted mapping
    opts.p25_is_tuned = 0; // on CC
    dmr_sm_on_group_grant(&opts, &state, /*freq_hz*/ 0, /*lpcn*/ lcn, /*tg*/ 1234, /*src*/ 0);
    assert(opts.p25_is_tuned == 1);
    assert(state.p25_vc_freq[0] == freq);

    return 0;
}
