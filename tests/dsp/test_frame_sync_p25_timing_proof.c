// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The frame sync's side of the P25 proof the CQPSK timing loop's post-lock gain follows
 * (dsd_rtl_stream_metrics_hook_forget_p25_proof()): every acquisition the frame sync starts over withdraws it (a tune's
 * or a scan step's modulation reset, an acquisition reset, a symbol profile change), and so does every move of the
 * decoder's modulation (a vote's switch, a P25p1 chain step, a TETRA profile's CQPSK), and every other protocol's sync
 * the hunt accepts. A P25 sync on the chain the decoder already runs withdraws nothing, so a carrier decoding P25 keeps
 * its proof from frame to frame.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"
#include "frame_sync_test_support.h"

/* What reached the timing loop: the calls, and the last value (0 = withdrawn). */
static int g_proof_calls;
static uint64_t g_proof_last;

static void
capture_p25_proof(uint64_t proof) {
    g_proof_calls++;
    g_proof_last = proof;
}

/* A proof standing, as a validated frame leaves it, and nothing reported since. */
static void
proven(void) {
    g_proof_calls = 0;
    dsd_rtl_stream_metrics_hook_note_p25_proven(7U);
    assert(g_proof_calls == 1 && g_proof_last == 8U);
    g_proof_calls = 0;
}

static int
withdrawn(void) {
    return g_proof_calls > 0 && g_proof_last == 0U;
}

/* A P25p1 4800/4 stream on the CQPSK chain, the modulation unlocked so the frame sync may move it. */
static void
setup(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(init_state_buffers(state));
    opts->frame_p25p1 = 1;
    opts->msize = 1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->min = -3.0f;
    state->max = 3.0f;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.p25_timing_proof = capture_p25_proof;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_proof_calls = 0;
    g_proof_last = 1U;
}

static void
teardown(dsd_state* state) {
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free_state_buffers(state);
}

/* Every path that starts the acquisition over: the modulation reset every accepted tune runs (trunking, a manual or
   menu tune, a CC change), and the acquisition reset of a scan row or target, an input boundary and a decode-mode
   switch. */
static void
test_acquisition_restarts_withdraw(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    proven();
    dsd_frame_sync_reset_mod_state();
    assert(withdrawn());

    proven();
    dsd_frame_sync_reset_acquisition(&opts, &state, 1);
    assert(withdrawn());
    teardown(&state);
}

/* The hunt adopting another symbol profile withdraws it; re-entering the profile it is on changes nothing. */
static void
test_profile_change_withdraws(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    opts.frame_p25p2 = 1;
    proven();
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(g_proof_calls == 0);
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(withdrawn());
    teardown(&state);
}

/* A P25p1 chain step to the other chain (a CQPSK probe, a chain comparison) withdraws it; a step to the chain the
   decoder runs does not. So does the TETRA profile putting CQPSK on a stream that ran another modulation. */
static void
test_modulation_moves_withdraw(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    proven();
    frame_sync_p25p1_set_chain(&opts, &state, 1);
    assert(g_proof_calls == 0);
    frame_sync_p25p1_set_chain(&opts, &state, 0);
    assert(state.rf_mod == 0);
    assert(withdrawn());

    proven();
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_18000_4;
    int lastt = 0;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 1, &lastt);
    assert(state.rf_mod == 1);
    assert(withdrawn());
    proven();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 1, &lastt);
    assert(g_proof_calls == 0);
    teardown(&state);
}

/* A carrier decoding P25 keeps the proof from frame to frame: an exact P25p1 sync on the CQPSK chain the decoder runs
   (which re-asserts that chain) withdraws nothing. */
static void
test_sync_on_the_running_chain_keeps_it(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    proven();
    state.symbol_clock = 1000U;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(state.rf_mod == 1);
    assert(g_proof_calls == 0);
    teardown(&state);
}

/* Final review: another protocol's sync withdraws it, also when its accept moves neither the modulation nor the profile.
   Under -fa -mq a continuous carrier that goes from a proven P25 control channel to X2-TDMA (also 4800/4 on the locked
   CQPSK chain) kept the proof, and the locked X2-TDMA blocks ran the P25-only gain. So do DMR and NXDN96 there, which
   the locked modulation keeps on CQPSK too. The P25p1 and P25p2 syncs keep it (test_sync_on_the_running_chain_keeps_it()
   and below): a change between the phases is a profile step, which withdraws it (test_profile_change_withdraws()). */
static void
test_other_protocol_sync_withdraws(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const struct {
        const char* window;
        int frame;
        int sync;
    } k_syncs[] = {
        {X2TDMA_BS_DATA_SYNC, 0, DSD_SYNC_X2TDMA_DATA_POS},
        {DMR_BS_DATA_SYNC, 1, DSD_SYNC_DMR_BS_DATA_POS},
        {"11111111111111" INV_NXDN_FSW, 2, DSD_SYNC_NXDN_NEG},
    };

    for (size_t i = 0; i < sizeof(k_syncs) / sizeof(k_syncs[0]); i++) {
        setup(&opts, &state);
        opts.mod_cli_lock = 1;
        opts.mod_qpsk = 1;
        opts.frame_x2tdma = k_syncs[i].frame == 0;
        opts.frame_dmr = k_syncs[i].frame == 1;
        opts.frame_nxdn96 = k_syncs[i].frame == 2;
        state.symbol_clock = 1000U;
        state.symbolcnt = 5000;
        state.nxdn96_sync.lock_type = (uint8_t)DSD_SYNC_NXDN_NEG;
        state.nxdn96_sync.lock_symbolcnt = 5000U - DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
        proven();
        state.symbol_clock = 2000U;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, k_syncs[i].window, 24) == k_syncs[i].sync);
        assert(state.rf_mod == 1 && state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
        assert(withdrawn());
        teardown(&state);
    }

    /* An exact P25p2 sync on the 6000/4 chain it already runs keeps a proof given there. */
    setup(&opts, &state);
    opts.frame_p25p1 = 0;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    proven();
    state.symbol_clock = 1000U;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(state.rf_mod == 1);
    assert(g_proof_calls == 0);
    teardown(&state);
}

int
main(void) {
    test_acquisition_restarts_withdraw();
    test_profile_change_withdraws();
    test_modulation_moves_withdraw();
    test_sync_on_the_running_chain_keeps_it();
    test_other_protocol_sync_withdraws();
    DSD_FPRINTF(stderr, "FRAME_SYNC_P25_TIMING_PROOF: OK\n");
    return 0;
}
