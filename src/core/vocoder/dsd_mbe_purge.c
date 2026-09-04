// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <mbelib-neo/mbelib.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
dsd_mbe_reset_slot_parameters(dsd_state* state, int slot) {
    if (slot == 0) {
        if (state->cur_mp && state->prev_mp && state->prev_mp_enhanced) {
            mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
        }
        return;
    }
    if (state->cur_mp2 && state->prev_mp2 && state->prev_mp_enhanced2) {
        mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);
    }
}

void
dsd_mbe_purge_slot_audio(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }

    p25_p2_audio_ring_reset(state, slot);
    dsd_mbe_reset_slot_parameters(state, slot);
    state->voice_counter[slot] = 0;

    if (slot == 0) {
        DSD_MEMSET(state->audio_out_temp_buf, 0, sizeof(state->audio_out_temp_buf));
        DSD_MEMSET(state->f_l, 0, sizeof(state->f_l));
        DSD_MEMSET(state->f_l4, 0, sizeof(state->f_l4));
        DSD_MEMSET(state->s_l, 0, sizeof(state->s_l));
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
        DSD_MEMSET(state->s_lu, 0, sizeof(state->s_lu));
        DSD_MEMSET(state->s_l4u, 0, sizeof(state->s_l4u));
        state->audio_out_temp_buf_p = state->audio_out_temp_buf;
        state->audio_out_idx = 0;
        state->audio_out_idx2 = 0;
        if (state->audio_out_float_buf) {
            DSD_MEMSET(state->audio_out_float_buf, 0, 100U * sizeof(*state->audio_out_float_buf));
            state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
        }
        if (state->audio_out_buf) {
            DSD_MEMSET(state->audio_out_buf, 0, 100U * sizeof(*state->audio_out_buf));
            state->audio_out_buf_p = state->audio_out_buf + 100;
        }
        return;
    }

    DSD_MEMSET(state->audio_out_temp_bufR, 0, sizeof(state->audio_out_temp_bufR));
    DSD_MEMSET(state->f_r, 0, sizeof(state->f_r));
    DSD_MEMSET(state->f_r4, 0, sizeof(state->f_r4));
    DSD_MEMSET(state->s_r, 0, sizeof(state->s_r));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    DSD_MEMSET(state->s_ru, 0, sizeof(state->s_ru));
    DSD_MEMSET(state->s_r4u, 0, sizeof(state->s_r4u));
    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    state->audio_out_idxR = 0;
    state->audio_out_idx2R = 0;
    if (state->audio_out_float_bufR) {
        DSD_MEMSET(state->audio_out_float_bufR, 0, 100U * sizeof(*state->audio_out_float_bufR));
        state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
    }
    if (state->audio_out_bufR) {
        DSD_MEMSET(state->audio_out_bufR, 0, 100U * sizeof(*state->audio_out_bufR));
        state->audio_out_buf_pR = state->audio_out_bufR + 100;
    }
}
