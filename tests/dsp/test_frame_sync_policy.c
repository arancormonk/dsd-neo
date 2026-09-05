// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

static void
reset(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    state.carrier = 1;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    opts.trunk_enable = 1;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 1);

    state.carrier = 0;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    state.carrier = 1;
    state.lastsynctype = DSD_SYNC_YSF_POS;
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, &state) == 0);

    assert(dsd_frame_sync_suppress_p25_alt_sync(NULL, &state) == 0);
    assert(dsd_frame_sync_suppress_p25_alt_sync(&opts, NULL) == 0);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_TCP;
    assert(dsd_frame_sync_suppress_tcp_no_signal_console(&opts, &state) == 1);

    opts.audio_in_type = AUDIO_IN_PULSE;
    assert(dsd_frame_sync_suppress_tcp_no_signal_console(&opts, &state) == 0);

    opts.audio_in_type = AUDIO_IN_UDP;
    assert(dsd_frame_sync_suppress_tcp_no_signal_console(&opts, &state) == 0);

    opts.audio_in_type = AUDIO_IN_RTL;
    assert(dsd_frame_sync_suppress_tcp_no_signal_console(&opts, &state) == 0);

    assert(dsd_frame_sync_suppress_tcp_no_signal_console(NULL, &state) == 0);
    assert(dsd_frame_sync_suppress_tcp_no_signal_console(&opts, NULL) == 0);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    opts.trunk_enable = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);

    /* Combined P25 scan visits must leave room for C4FM, CQPSK and Phase 2.
     * Explicit locks, fixed inputs and normal trunking keep the original dwell. */
    opts.trunk_scan_enabled = 1;
    opts.frame_p25p2 = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&state;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);
    opts.mod_cli_lock = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);
    opts.mod_cli_lock = 0;
    opts.audio_in_type = AUDIO_IN_WAV;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    opts.trunk_scan_enabled = 0;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 5);
    opts.trunk_is_tuned = 1;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    opts.trunk_is_tuned = 0;
    opts.frame_p25p1 = 0;
    opts.frame_p25p2 = 0;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) == 3);

    assert(dsd_frame_sync_sps_hunt_dwell_passes(NULL, &state) == 3);
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, NULL) == 3);

    /* #445: while a protocol has recently proved the 2400/4 profile, the weaker matchers a
     * hunt step onto 4800/4 would offer the same transmission stand down. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;

    /* A zeroed state is not armed. The stamp cannot say so on its own -- profile index 0 is
     * 4800/4 and symbol 0 is a real symbol count -- which is what the valid flag is for. */
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 0);

    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.symbolcnt = 1000U;
    dsd_frame_sync_note_profile_proof(&state);
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 1);

    /* The span covers one full rotation of the hunt and then releases. */
    state.symbolcnt = 1000U + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS - 1U;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 1);
    state.symbolcnt = 1000U + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 0);

    /* A proof read on any other profile is inert: what it vouches for is 2400/4. */
    state.symbolcnt = 1000U;
    state.profile_proof_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 0);
    state.profile_proof_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;

    /* Gated on the protocols that can arm a 2400/4 proof at all, so a build carrying neither
     * behaves as it did before. Either one alone is enough. */
    opts.frame_nxdn48 = 0;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 0);
    opts.frame_dpmr = 1;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 1);
    opts.frame_nxdn48 = 1;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 1);
    opts.frame_dpmr = 0;

    /* Modular difference, so the span is exact across the symbol counter's rollover. */
    state.profile_proof_symbolcnt = UINT32_MAX - 100U;
    state.symbolcnt = 49U;
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 1);
    state.symbolcnt = (uint32_t)(UINT32_MAX - 100U + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS);
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, &state) == 0);

    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(NULL, &state) == 0);
    assert(dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(&opts, NULL) == 0);

    return 0;
}
