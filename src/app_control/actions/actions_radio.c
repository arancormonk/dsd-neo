// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI command actions — radio domain */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <stdint.h>
#include <string.h>
#include "../command_dispatch.h"
#include "../services.h"
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
 * @brief The symbol profile the modulation control's choices run at.
 *
 * The control switches the demodulator inside whatever decode set is enabled, so
 * the timing has to come from that set rather than from a constant — and from the
 * same authority the SPS hunt and the front-end filter read, or a modulation
 * picked here lands the decoder on one protocol's symbol clock and the hunt on
 * another's. A mode set that spans several symbol rates (AUTO, or a hand-rolled
 * combination) answers 4800/4, which is where the hunt starts anyway.
 */
static dsd_decode_mode_profile
ui_modulation_profile(const dsd_opts* opts) {
    return dsd_decode_mode_profile_for(dsd_infer_decode_mode_preset(opts));
}

/**
 * @brief The modulation a request for @p mod actually lands on.
 *
 * The control offers three modulations, but the decode set decides how many of
 * them exist: a two-level profile — EDACS/ProVoice at 9600/2, D-STAR at 4800/2 —
 * runs GFSK and nothing else. Honouring a C4FM request there would write
 * @c mod_c4fm and @c rf_mod together only for frame_sync_apply_sps_hunt_profile()
 * to normalise @c rf_mod straight back to GFSK on the next pass, leaving the two
 * permanently disagreeing: the control reads C4FM off the flags, the decoder runs
 * GFSK, and because the two disagree the idempotence check in ui_handle_mod_set()
 * can never fire, so every tap rebuilds the timing for nothing.
 */
static int
ui_effective_modulation(const dsd_opts* opts, int mod) {
    return dsd_frame_sync_profile_modulation(ui_modulation_profile(opts).levels, mod);
}

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
    const dsd_decode_mode_profile profile = ui_modulation_profile(opts);
    const int sps = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, ui_modulation_demod_rate(opts, state));
    /* Through the profile, so the flags and rf_mod written below are a modulation
       the decode set can actually run and the SPS hunt will not move off. */
    const int applied = dsd_frame_sync_profile_modulation(profile.levels, mod);
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    opts->mod_c4fm = (applied == 0) ? 1 : 0;
    opts->mod_qpsk = (applied == 1) ? 1 : 0;
    opts->mod_gfsk = (applied == 2) ? 1 : 0;
    state->rf_mod = applied;
    state->samplesPerSymbol = sps;
    state->symbolCenter = dsd_opts_symbol_center(sps);
    /* After rf_mod and the timing, both of which the published profile reads. */
    svc_publish_symbol_profile(opts, state, profile);
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
    if (c->n < (int)sizeof(int32_t)) {
        /* A truncated payload is not a request for C4FM. Falling through with want
         * still 0 would force the demodulator, reset the sps hunt and release the
         * P25p2 helper lock on a command nobody sent. */
        return 1;
    }
    DSD_MEMCPY(&want, c->data, sizeof(int32_t));
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
     * request from there is a real change and has to apply.
     *
     * Both readings have to agree before the request is a no-op. The sps hunt moves
     * state->rf_mod on its own without touching the mod_* flags
     * (frame_sync_maybe_force_dmr_gfsk(), frame_sync_accept_p25p2()), while the
     * reading a segmented control binds to is published from the flags
     * (MetricsModel::fillDecoderView). Skipping on rf_mod alone therefore drops
     * exactly the tap that would resynchronise them: the control shows C4FM, asking
     * for QPSK on a session the hunt already moved to QPSK does nothing, and the
     * control snaps back on the next poll and reads as broken. Through the same
     * helper that publishes the reading, so the two cannot answer differently.
     *
     * Compared against what the request lands on rather than what it asked for:
     * on a two-level decode set every modulation lands on GFSK, so asking for
     * C4FM there is already satisfied and rebuilding the timing for it would be
     * the same pointless churn on every tap. */
    const int mod_in_effect = dsd_opts_modulation(opts);
    const int mod_effective = ui_effective_modulation(opts, (int)want);
    if (state->rf_mod == mod_effective && mod_in_effect == mod_effective && opts->mod_p25p2_c4fm == 0
        && opts->mod_p25p2_profile_lock == 0) {
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
    const dsd_decode_mode_profile profile = dsd_decode_mode_profile_for(DSDCFG_MODE_P25P2);
    int sps = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, ui_modulation_demod_rate(opts, state));
    int center = dsd_opts_symbol_center(sps);
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 1;
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
    svc_publish_symbol_profile(opts, state, profile);
    /* Lock only after the decoder and any RTL backend share the P25p2 profile. */
    opts->mod_cli_lock = 1;
    return 1;
}

const struct dsd_app_command_reg dsd_app_actions_radio[] = {
    {DSD_APP_CMD_PPM_DELTA, ui_handle_ppm_delta},         {DSD_APP_CMD_INVERT_TOGGLE, ui_handle_invert_toggle},
    {DSD_APP_CMD_MOD_TOGGLE, ui_handle_mod_toggle},       {DSD_APP_CMD_MOD_SET, ui_handle_mod_set},
    {DSD_APP_CMD_MOD_P2_TOGGLE, ui_handle_mod_p2_toggle}, {0, NULL},
};
