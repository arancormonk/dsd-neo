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
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#endif

void
svc_publish_symbol_profile(const dsd_opts* opts, dsd_state* state, dsd_decode_mode_profile profile) {
    if (!opts || !state) {
        return;
    }

    state->sps_hunt_idx = (int)profile.sps_profile_index;
    state->sps_hunt_counter = 0;

#ifdef USE_RADIO
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    /* Analog monitor has no symbol clock and no channel to protect, and the front
       end already answers that case for itself with DSD_CH_LPF_PROFILE_WIDE
       (opts_channel_profile_for_rate()). dsd_decode_mode_profile_for() has no
       entry for it and falls back on 4800/4, so publishing that would ask
       rtl_stream_set_symbol_profile() for the P25 C4FM filter and narrow the
       monitor audio to a digital channel's width -- for a command that only chose
       a mode. Narrower than "no digital decode mode": that is also the shape a
       modulation change sees before any frame flag is set. */
    if (opts->analog_only) {
        return;
    }
    /* Queued for the demod thread rather than written into demod state from the
       caller's thread. The clamp mirrors the no-override setter this replaced. */
    const int mod = state->rf_mod;
    const int ted_sps = state->samplesPerSymbol < 2 ? 2 : state->samplesPerSymbol;
    (void)rtl_stream_request_demod_profile(
        mod == 1, profile.symbol_rate_hz, profile.levels,
        dsd_rtl_channel_profile_for(opts, profile.symbol_rate_hz, profile.levels, mod), ted_sps, 0);
#endif
}
