// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

int
dsd_frame_sync_suppress_p25_alt_sync(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    return opts->trunk_enable == 1 && state->carrier == 1 && DSD_SYNC_IS_P25(state->lastsynctype);
}

int
dsd_frame_sync_suppress_nxdn48_sync(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    if (opts->frame_dpmr != 1 || state->dpmr_fs2_frame_valid == 0) {
        return 0;
    }
    /* Modular difference, so the span stays exact across the symbol counter's rollover. A
     * backwards jump -- nxdn_reset_after_cac_fail() and initState() both zero symbolcnt --
     * reads as a huge forward distance and simply ends the suppression early, which is the
     * safe way for it to fail. */
    const uint32_t since = state->symbolcnt - state->dpmr_fs2_frame_symbolcnt;
    return since < DSD_FRAME_SYNC_DPMR_FS2_FRAME_SYMBOLS;
}

int
dsd_frame_sync_suppress_4800_4_for_p25p1_frame(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    if (opts->frame_p25p1 != 1 || state->p25_p1_c4fm_frame_valid == 0) {
        return 0;
    }
    /* Modular difference, for the same reason and with the same failure direction as the FS2
     * span above: a backwards symbolcnt jump reads as a huge forward distance and ends the
     * suppression early rather than extending it. */
    const uint32_t since = state->symbolcnt - state->p25_p1_c4fm_frame_symbolcnt;
    return since < DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS;
}

void
dsd_frame_sync_note_profile_proof(dsd_state* state) {
    if (!state) {
        return;
    }
    /* The index current at the call, which is the one the frame was read on: this runs from the
     * protocol handler, inside the same getFrameSync() cycle that matched the sync. Re-stamped
     * per proof and never extended, so a channel that stops proving releases the guard. */
    state->profile_proof_idx = state->sps_hunt_idx;
    state->profile_proof_symbolcnt = state->symbolcnt;
    state->profile_proof_valid = 1;
}

int
dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    /* Gated on the protocols that can prove 2400/4 at all -- the pair
     * frame_sync_sps_profile_has_candidate() names for that profile -- so a build carrying
     * neither behaves exactly as it did before. The arming is verdict-based rather than
     * matcher-based, so the gate is their union rather than one protocol's flag. */
    if (opts->frame_nxdn48 != 1 && opts->frame_dpmr != 1) {
        return 0;
    }
    if (state->profile_proof_valid == 0 || state->profile_proof_idx != DSD_FRAME_SYNC_SPS_PROFILE_2400_4) {
        return 0;
    }
    /* Modular difference, with the same failure direction as the two spans above: a backwards
     * symbolcnt jump reads as a huge forward distance and ends the suppression early. */
    const uint32_t since = state->symbolcnt - state->profile_proof_symbolcnt;
    return since < DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS;
}

int
dsd_frame_sync_suppress_tcp_no_signal_console(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    return opts->audio_in_type == AUDIO_IN_TCP;
}

int
frame_sync_trunk_scan_p25p1_trial(const dsd_opts* opts, const dsd_state* state) {
    return opts && state && opts->trunk_scan_enabled == 1 && !opts->mod_cli_lock && opts->frame_p25p1 == 1
           && opts->frame_p25p2 == 1 && opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx
           && state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
}

int
dsd_frame_sync_sps_hunt_dwell_passes(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return 3;
    }
    if (opts->trunk_enable == 1 && opts->trunk_is_tuned == 0 && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1)) {
        if (frame_sync_trunk_scan_p25p1_trial(opts, state)) {
            /* C4FM and CQPSK each get 5400 symbols (1125 ms); Phase 2 then
             * starts at 2250 ms, also inside the default 3000 ms target visit. */
            return 3;
        }
        return 5;
    }
    return 3;
}
