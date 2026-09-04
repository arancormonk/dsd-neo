// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/trunk_tuning_hooks.h"
#include "p25_cc_selection.h"
#include "p25_trunk_sm_internal.h"

static p25_cc_selection*
p25_sm_prepare_cc_selection(dsd_state* state) {
    p25_cc_selection* selection = dsd_state_ext_get(state, DSD_STATE_EXT_PROTO_P25_CC_SELECTION);
    if (!selection) {
        selection = calloc(1, sizeof(*selection));
        if (selection && dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_P25_CC_SELECTION, selection, free) != 0) {
            free(selection);
            return NULL;
        }
    }
    return selection;
}

static void
p25_sm_forget_selected_site(p25_sm_ctx_t* ctx, dsd_state* state) {
    ctx->expected_cc_nac = 0;
    ctx->nac_mismatch_count = 0;
    state->nac = 0;
    if (!state->p2_hardset) {
        state->p2_cc = 0;
    }
    state->p2_rfssid = 0;
    state->p2_siteid = 0;
    state->p25_site_lra_valid = 0;
    state->p25_site_lra = 0;
    state->p25_site_network_active_valid = 0;
    state->p25_site_network_active = 0;
    state->p25_cc_prot_valid = 0;
    state->p25_cc_prot_algid = 0;
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
    state->p25_last_cc_msg_time = 0;
    state->p25_last_cc_msg_time_m = 0.0;
    state->p25_cc_eval_freq = 0;
    state->p25_cc_eval_start_m = 0.0;
    state->p25_cc_cache_loaded = 0;
    dsd_trunk_cc_candidates_reset(state);
    state->p25_nb_count = 0;
    DSD_MEMSET(state->p25_nb_entries, 0, sizeof(state->p25_nb_entries));
    state->p25_secondary_cc_count = 0;
    DSD_MEMSET(state->p25_secondary_cc_entries, 0, sizeof(state->p25_secondary_cc_entries));
    state->p25_pending_announcement_count = 0;
    DSD_MEMSET(state->p25_pending_announcements, 0, sizeof(state->p25_pending_announcements));
}

static int
p25_cc_selection_sps(const dsd_opts* opts, int is_tdma) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        const int rtl_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (rtl_rate > 0) {
            demod_rate = rtl_rate;
        }
    }
#endif
    return dsd_opts_compute_sps_rate(opts, is_tdma ? 6000 : 4800, demod_rate);
}

dsd_trunk_tune_result
p25_sm_select_control_channel(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, long hz) {
    if (!ctx || !opts || !state || hz <= 0 || opts->trunk_enable != 1 || opts->trunk_scan_enabled) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
#if LONG_MAX > UINT32_MAX
    if ((unsigned long)hz > UINT32_MAX) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
#endif
    p25_sm_tick_guard_enter();
    p25_cc_selection* selection = ctx->initialized ? p25_sm_prepare_cc_selection(state) : NULL;
    if (!selection) {
        p25_sm_tick_guard_leave();
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    // Both the hardware profile and decoder timing use the saved CC type, even
    // when this command interrupts a 6000-symbol traffic channel.
    const int is_tdma = state->p25_cc_is_tdma == 1;
    const int sps = p25_cc_selection_sps(opts, is_tdma);
    uint64_t request_id = 0;
    const dsd_trunk_tune_result result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, hz, sps, &request_id);
    if (dsd_trunk_tune_result_is_ok(result)) {
        p25_sm_clear_manual_selection_calls(ctx, opts, state);
        dsd_mbe_purge_slot_audio(state, 0);
        dsd_mbe_purge_slot_audio(state, 1);
        p25_p2_frame_reset();
        p25_sm_forget_selected_site(ctx, state);
        selection->require_site_cache = 1;
        state->p25_cc_freq = hz;
        state->trunk_cc_freq = hz;
        opts->rtlsdr_center_freq = (uint32_t)hz;
        if (!dsd_state_trunk_lcn_user_list_present(opts, state)) {
            state->trunk_lcn_freq[0] = hz;
            state->lcn_freq_count = 1;
            state->lcn_freq_roll = 0;
        }
        state->samplesPerSymbol = sps;
        state->symbolCenter = dsd_opts_symbol_center(sps);
        state->sps_hunt_idx = is_tdma ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        state->sps_hunt_counter = 0;
        dsd_frame_sync_reset_mod_state();
        if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
            (void)p25_sm_await_pending_cc_tune(ctx, opts, state, request_id, "manual-cc");
        } else {
            (void)p25_sm_restart_pending_cc_acquisition(ctx, opts, state, 0.0, "manual-cc");
        }
    }
    p25_sm_tick_guard_leave();
    return result;
}
