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

#ifdef USE_RADIO
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
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
    if (state->rf_mod == 0) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        state->rf_mod = 1;
        // P25P1 QPSK: 4800 sym/s - compute SPS from actual demod rate
#ifdef USE_RADIO
        int demod_rate = 0;
        if (state->rtl_ctx) {
            demod_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
        }
#else
        int demod_rate = 0;
#endif
        state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
        state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    } else {
        opts->mod_c4fm = 1;
        opts->mod_qpsk = 0;
        opts->mod_gfsk = 0;
        state->rf_mod = 0;
        // Keep current symbol timing unless other code adjusts it
    }
    return 1;
}

static int
ui_handle_mod_p2_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    // P25P2 TDMA: 6000 sym/s - compute SPS from actual demod rate
#ifdef USE_RADIO
    int demod_rate = 0;
    if (state->rtl_ctx) {
        demod_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
    }
#else
    int demod_rate = 0;
#endif
    int sps = dsd_opts_compute_sps_rate(opts, 6000, demod_rate);
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
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        int cqpsk = state->rf_mod == 1;
        int channel_profile = cqpsk ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : RTL_STREAM_CHANNEL_PROFILE_12K5;
        rtl_stream_toggle_cqpsk(cqpsk);
        rtl_stream_clear_ted_sps_override();
        rtl_stream_set_ted_sps_no_override(sps);
        (void)rtl_stream_set_symbol_profile(6000, 4, channel_profile);
    }
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
