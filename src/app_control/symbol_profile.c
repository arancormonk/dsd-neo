// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Publishing a decoder symbol profile to the SPS hunt and the front end.
 *
 * Its own translation unit rather than part of menu_services.c so that the
 * hermetic unit tests over a single command-handler source can link it without
 * dragging in CSV import, rigctl and the P25 watchdog.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>

/* Whether the decoder's configured mode, not a scan row's constraint over it, is digital. A typed digital scan row on
   an analog session runs its symbol profile on the analog family's monitor output, as it always has; only a digital
   configured mode moves the front end off the analog family. */
static int
symbol_profile_configured_digital(const dsd_opts* opts, const dsd_state* state) {
    const dsd_scan_settings* configured = dsd_scan_mode_configured_view(state);
    const int analog_only = configured ? configured->analog_only : opts->analog_only;
    return (analog_only == 1 && opts->m17encoder != 1) ? 0 : 1;
}
#endif

/* Note the configured digital decode modes with the RTL front end (rtl_stream_set_digital_decode_modes()). Once a live
   switch has moved it onto the digital family, the options it opened with no longer name the modes it runs (a -fA
   session's name none), and these pick the FSK channel profile a CQPSK toggle returns to, as an open with them would.
   Only options that are the configured ones say that: outside a scan row, or while a command updates the configuration
   under one (dsd_scan_mode_updating(), before the row's constraint is reapplied); a running row's options carry the
   row's constraint, and the configured modes it runs under were noted when they were set, or are the ones the stream
   opened with. A mode change made under a row is noted here even though the front end waits for the row's leave to
   switch to it. */
void
svc_note_digital_decode_modes(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx
        || dsd_scan_mode_configured_view(state) || opts->analog_only == 1) {
        return;
    }
    rtl_stream_set_digital_decode_modes(opts);
#else
    (void)opts;
    (void)state;
#endif
}

int
svc_check_mode_receive_profile(const dsd_opts* opts, const dsd_state* state, dsdneoUserDecodeMode mode) {
    if (!opts || !state || mode != DSDCFG_MODE_ANALOG || opts->m17encoder == 1 || dsd_scan_mode_updating(state)) {
        return 0;
    }
#ifdef USE_RADIO
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return 0;
    }
    /* What svc_publish_symbol_profile() will request once the Analog preset has run: the preset selects NFM
       (dsd_apply_decode_mode_preset() sets analog_demod to FM) and keeps the configured NFM width. */
    return rtl_stream_check_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, opts->analog_nfm_bandwidth_hz);
#else
    return 0;
#endif
}

void
svc_publish_symbol_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile) {
    if (!opts || !state) {
        return;
    }

    state->sps_hunt_idx = (int)profile.sps_profile_index;
    svc_note_digital_decode_modes(opts, state);
    if (dsd_scan_mode_updating(state)) {
        /* The saved configuration needs its new profile, but acquisition and
         * frontend changes wait until the row constraint has been reapplied. */
        return;
    }
    state->sps_hunt_counter = 0;

#ifdef USE_RADIO
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    /* Analog monitor has no symbol clock, so it gets the analog receive profile
       instead of a symbol profile: dsd_decode_mode_profile_for() has no entry for
       it and falls back on 4800/4, which would ask rtl_stream_set_symbol_profile()
       for the P25 C4FM filter and narrow the monitor audio to a digital channel.
       The analog request is also what moves a live digital front end onto the
       analog family when the operator picks Analog mid-session. Its result is
       dropped: the caller's svc_check_mode_receive_profile() held the same
       profile to the rate the stream ran at before it committed, so the front
       end refuses it only when a retune moved the rate since, here or on the
       demod thread. Then the refusal is logged with the validator's text and
       the front end keeps its receive profile, as it does for a width change
       the running monitor refuses, rather than run the width without its
       channel filter, but a decoder that has committed to Analog is not told
       and stays on it. Only an explicit analog width can be refused for its
       rate; nothing in this build sets one (the Analog preset asks for the
       unset NFM default), so the split cannot happen yet, and a control that
       lets the operator set a width has to report this refusal back. */
    if (dsd_opts_is_analog_family(opts)) {
        (void)rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, opts->analog_demod,
                                                dsd_opts_analog_width_hz(opts));
        return;
    }
    /* The M17 encoder rides the analog front end without being the analog family. */
    if (opts->analog_only) {
        return;
    }
    const int mod = state->rf_mod;
    const int configured_digital = symbol_profile_configured_digital(opts, state);
    if (configured_digital && rtl_stream_analog_family_active()) {
        /* Leaving analog: the family switch lands on the demod thread after this returns, and until then the
           output rate is the analog family's (the monitor's resampled audio rate, or the rate a CQPSK toggle or typed
           row put it on), not the rate the digital stream will run at. Time the decoder, and the front end below,
           for the digital rate. */
        const unsigned int rate_hz =
            rtl_stream_output_rate_for_family(DSD_RX_FAMILY_DIGITAL, mod == 1, profile.symbol_rate_hz);
        if (rate_hz > 0U) {
            state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, profile.symbol_rate_hz, (int)rate_hz);
            state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        }
    }
    /* Queued for the demod thread rather than written into demod state from the
       caller's thread: the digital family first (a no-op unless the front end is
       analog), then the symbol profile it runs on. The clamp mirrors the
       no-override setter this replaced. */
    if (configured_digital) {
        (void)rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    }
    const int ted_sps = state->samplesPerSymbol < 2 ? 2 : state->samplesPerSymbol;
    (void)rtl_stream_request_demod_profile(
        mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, mod), ted_sps, 0);
#endif
}
