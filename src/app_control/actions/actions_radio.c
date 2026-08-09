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

/**
 * @brief The symbol rate the modulation control's choices run at, in Hz.
 *
 * The control switches the demodulator inside whatever decode set is enabled, so
 * the timing has to come from that set rather than from a constant. Everything it
 * applies to is 4800 symbols/s except ProVoice, which is 9600 — and only when
 * ProVoice is the mode being decoded. AUTO enables ProVoice alongside the 4800
 * modes, so the flag on its own says nothing.
 */
static int
ui_modulation_symbol_rate(const dsd_opts* opts) {
    if (opts->frame_provoice != 1) {
        return 4800;
    }
    const int other_modes = opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_dmr == 1
                            || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1
                            || opts->frame_m17 == 1 || opts->frame_dstar == 1 || opts->frame_x2tdma == 1
                            || opts->frame_dpmr == 1;
    return other_modes ? 4800 : 9600;
}

#ifdef USE_RADIO
/**
 * @brief The channel filter that goes with a symbol rate and modulation.
 *
 * Deliberately the same mapping the DSP's SPS hunt applies in
 * rtl_profile_for_sps_profile(): a modulation picked here and one the hunt lands
 * on must ask the backend for the same filter, or the two disagree about what the
 * front end is doing every time the hunt re-runs.
 */
static int
ui_rtl_channel_profile(const dsd_opts* opts, int symbol_rate_hz, int mod) {
    if (symbol_rate_hz == 9600) {
        return RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    }
    if (mod == 1) {
        return RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    if (symbol_rate_hz == 6000 || mod == 2 || dsd_opts_uses_wide_4800_profile(opts)) {
        return RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    return RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
}

static void
ui_apply_rtl_demod_profile(const dsd_opts* opts, const dsd_state* state, int symbol_rate_hz, int levels, int sps) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    const int mod = state->rf_mod;
    /* Queue the whole profile for the demod thread instead of mutating demod
     * state from the UI thread (sps clamp mirrors the old no-override setter). */
    (void)rtl_stream_request_demod_profile(mod == 1, symbol_rate_hz, levels,
                                           ui_rtl_channel_profile(opts, symbol_rate_hz, mod), sps < 2 ? 2 : sps, 0);
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

/**
 * @brief Put the demodulator on C4FM, QPSK or GFSK, with the timing and RTL
 *        profile that go with it.
 *
 * @param mod 0 for C4FM, 1 for QPSK, 2 for GFSK — the values @c dsd_state::rf_mod
 *            already uses, so the caller's request and the readback agree.
 *
 * Shared by the cycle-to-the-other-one hotkey and the pick-this-one command: the
 * SPS recompute, the sps-hunt reset, the P25p2 helper release and the backend
 * profile request all have to happen together, and having two copies of that is
 * how one of them ends up half right.
 */
static void
ui_apply_modulation(dsd_opts* opts, dsd_state* state, int mod) {
    const int leaving_p25p2_helper = opts->mod_p25p2_c4fm == 1 || opts->mod_p25p2_profile_lock == 1;
    const int symbol_rate = ui_modulation_symbol_rate(opts);
    const int sps = dsd_opts_compute_sps_rate(opts, symbol_rate, ui_modulation_demod_rate(opts, state));
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    state->sps_hunt_idx = (symbol_rate == 9600) ? DSD_FRAME_SYNC_SPS_PROFILE_9600_2 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
    opts->mod_c4fm = (mod == 0) ? 1 : 0;
    opts->mod_qpsk = (mod == 1) ? 1 : 0;
    opts->mod_gfsk = (mod == 2) ? 1 : 0;
    state->rf_mod = mod;
    state->samplesPerSymbol = sps;
    state->symbolCenter = dsd_opts_symbol_center(sps);
#ifdef USE_RADIO
    /* ProVoice is the two-level one; everything else this control reaches is four. */
    ui_apply_rtl_demod_profile(opts, state, symbol_rate, (symbol_rate == 9600) ? 2 : 4, sps);
#endif
    if (leaving_p25p2_helper) {
        /* Release the helper lock only after decoder timing and any RTL backend agree on profile 0. */
        opts->mod_cli_lock = 0;
    }
}

static int
ui_handle_mod_toggle(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    (void)c;
    ui_apply_modulation(opts, state, state->rf_mod == 0 ? 1 : 0);
    return 1;
}

static int
ui_handle_mod_set(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* c) {
    int32_t want = 0;
    if (c->n >= (int)sizeof(int32_t)) {
        DSD_MEMCPY(&want, c->data, sizeof(int32_t));
    }
    if (want < 0 || want > 2) {
        /* A modulation that does not exist. Nothing sane to apply, and guessing at
         * one would move the demodulator somewhere the caller never asked for. */
        return 1;
    }
    /* Idempotent on purpose: a segmented control re-asserts its own state after a
     * frame it did not cause, and re-applying the same modulation must not disturb
     * timing the decoder has already settled on. Compared against the modulation
     * actually in effect rather than as a boolean, because rf_mod has three values
     * and GFSK (2) is one the DMR and EDACS/ProVoice presets select — a C4FM
     * request from there is a real change and has to apply. */
    if (state->rf_mod == (int)want && opts->mod_p25p2_c4fm == 0 && opts->mod_p25p2_profile_lock == 0) {
        return 1;
    }
    /* The P25p2 helper is the exception to the skip: it holds mod_cli_lock and the
     * 6000 sym/s profile, and it is entered from a session whose modulation reads
     * the same as the one being asked for -- so a request that looks idempotent is
     * the only way out of it from a control that only offers three modulations. */
    ui_apply_modulation(opts, state, (int)want);
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
    ui_apply_rtl_demod_profile(opts, state, 6000, 4, sps);
#endif
    /* Lock only after the decoder and any RTL backend share the P25p2 profile. */
    opts->mod_cli_lock = 1;
    return 1;
}

const struct dsd_app_command_reg dsd_app_actions_radio[] = {
    {DSD_APP_CMD_PPM_DELTA, ui_handle_ppm_delta},         {DSD_APP_CMD_INVERT_TOGGLE, ui_handle_invert_toggle},
    {DSD_APP_CMD_MOD_TOGGLE, ui_handle_mod_toggle},       {DSD_APP_CMD_MOD_SET, ui_handle_mod_set},
    {DSD_APP_CMD_MOD_P2_TOGGLE, ui_handle_mod_p2_toggle}, {0, NULL},
};
