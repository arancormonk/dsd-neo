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
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <stddef.h>
#endif

void
svc_publish_symbol_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile) {
    if (!opts || !state) {
        return;
    }

    state->sps_hunt_idx = (int)profile.sps_profile_index;
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
       analog family when the operator picks Analog mid-session. */
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
    if (rtl_stream_get_analog_profile(NULL, NULL, NULL) > 0) {
        /* Leaving analog: the family switch lands on the demod thread after this returns, and until then the
           output rate is the analog monitor's resampled audio rate, not the rate the digital stream will run at.
           Time the decoder, and the front end below, for the digital rate. */
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
    (void)rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0);
    const int ted_sps = state->samplesPerSymbol < 2 ? 2 : state->samplesPerSymbol;
    (void)rtl_stream_request_demod_profile(
        mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, mod), ted_sps, 0);
#endif
}
