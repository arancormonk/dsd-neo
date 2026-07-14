// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI command actions — radio domain */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <stdint.h>
#include <string.h>
#include "../command_dispatch.h"
#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
ui_modulation_demod_rate(const dsd_opts* opts, const dsd_state* state) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts && opts->audio_in_type == AUDIO_IN_RTL && state && state->rtl_ctx) {
        const int rtl_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
        if (rtl_rate > 0) {
            demod_rate = rtl_rate;
        }
    }
#else
    (void)state;
#endif
    return demod_rate;
}

#ifdef USE_RADIO
static int
ui_rtl_channel_profile(const dsd_opts* opts, int symbol_rate_hz, int cqpsk) {
    if (cqpsk) {
        return RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    if (symbol_rate_hz == 4800 && !dsd_opts_uses_wide_4800_profile(opts)) {
        return RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    }
    return RTL_STREAM_CHANNEL_PROFILE_12K5;
}

static void
ui_apply_rtl_demod_profile(const dsd_opts* opts, const dsd_state* state, int symbol_rate_hz, int sps) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    const int cqpsk = state->rf_mod == 1;
    rtl_stream_toggle_cqpsk(cqpsk);
    rtl_stream_clear_ted_sps_override();
    rtl_stream_set_ted_sps_no_override(sps);
    (void)rtl_stream_set_symbol_profile(symbol_rate_hz, 4, ui_rtl_channel_profile(opts, symbol_rate_hz, cqpsk));
}
#endif

static int
ui_handle_ppm_delta(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    int32_t d = 0;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&d, c->data, sizeof(int32_t));
    }
#ifdef USE_RADIO
    rtl_stream_adjust_ppm(opts, d);
#else
    opts->rtlsdr_ppm_error += d;
#endif
    return 1;
}

static int
ui_handle_invert_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)state;
    (void)c;
    int inv = opts->inverted_dmr ? 0 : 1;
    opts->inverted_dmr = inv;
    opts->inverted_dpmr = inv;
    opts->inverted_x2tdma = inv;
    opts->inverted_ysf = inv;
    opts->inverted_m17 = inv;
    return 1;
}

static int
ui_handle_mod_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    const int leaving_p25p2_helper = opts->mod_p25p2_c4fm == 1 || opts->mod_p25p2_profile_lock == 1;
    const int sps = dsd_opts_compute_sps_rate(opts, 4800, ui_modulation_demod_rate(opts, state));
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
    if (state->rf_mod == 0) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        state->rf_mod = 1;
    } else {
        opts->mod_c4fm = 1;
        opts->mod_qpsk = 0;
        opts->mod_gfsk = 0;
        state->rf_mod = 0;
    }
    state->samplesPerSymbol = sps;
    state->symbolCenter = dsd_opts_symbol_center(sps);
#ifdef USE_RADIO
    ui_apply_rtl_demod_profile(opts, state, 4800, sps);
#endif
    if (leaving_p25p2_helper) {
        /* Release the helper lock only after decoder timing and any RTL backend agree on profile 0. */
        opts->mod_cli_lock = 0;
    }
    return 1;
}

static int
ui_handle_mod_p2_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    // P25P2 TDMA: 6000 sym/s - compute SPS from actual demod rate
    int sps = dsd_opts_compute_sps_rate(opts, 6000, ui_modulation_demod_rate(opts, state));
    int center = dsd_opts_symbol_center(sps);
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state->sps_hunt_counter = 0;
    if (state->rf_mod == 0) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        state->rf_mod = 1;
        state->samplesPerSymbol = sps;
        state->symbolCenter = center;
    } else {
        opts->mod_c4fm = 1;
        opts->mod_qpsk = 0;
        opts->mod_gfsk = 0;
        state->rf_mod = 0;
        state->samplesPerSymbol = sps;
        state->symbolCenter = center;
    }
#ifdef USE_RADIO
    ui_apply_rtl_demod_profile(opts, state, 6000, sps);
#endif
    /* Lock only after the decoder and any RTL backend share the P25p2 profile. */
    opts->mod_cli_lock = 1;
    return 1;
}

const struct dsd_app_command_reg dsd_app_actions_radio[] = {
    {DSD_APP_CMD_PPM_DELTA, ui_handle_ppm_delta},
    {DSD_APP_CMD_INVERT_TOGGLE, ui_handle_invert_toggle},
    {DSD_APP_CMD_MOD_TOGGLE, ui_handle_mod_toggle},
    {DSD_APP_CMD_MOD_P2_TOGGLE, ui_handle_mod_p2_toggle},
    {0, NULL},
};
