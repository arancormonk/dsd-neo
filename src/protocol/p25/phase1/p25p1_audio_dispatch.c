// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25p1_ldu.h>

void
p25p1_play_imbe_audio(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    } else if (opts->floating_point == 0 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    } else if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceFM(opts, state);
    } else if (opts->floating_point == 1 && opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceFS(opts, state);
    }
}
