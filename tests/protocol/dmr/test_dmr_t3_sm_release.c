// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// DMR SM release gating: defer when either slot is active; then return to CC.

#include <assert.h>
#include <string.h>

#define MBELIB_NO_HEADERS 1
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>

// Stubs
void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)freq;
    if (opts) {
        opts->p25_is_tuned = opts->trunk_is_tuned = 1;
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

int
main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);
    opts.p25_trunk = 1;
    opts.trunk_enable = 1;
    state.p25_cc_freq = 851000000;
    opts.p25_is_tuned = 1;

    // Mark slot bursts as active voice on left (DMR VOICE=16), idle on right
    state.dmrburstL = 16;
    state.dmrburstR = 24; // one idle, one active

    // Invoke release: should defer (stay tuned)
    dmr_sm_on_release(&opts, &state);
    assert(opts.p25_is_tuned == 1);

    // Clear activity, honor hangtime
    opts.trunk_hangtime = 0.0f;
    state.dmrburstL = 24;
    state.dmrburstR = 24;
    state.last_t3_tune_time = 0; // no debounce
    dmr_sm_on_release(&opts, &state);
    assert(opts.p25_is_tuned == 0);

    return 0;
}
