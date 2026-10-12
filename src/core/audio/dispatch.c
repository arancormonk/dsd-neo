// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
dsd_play_synthesized_voice(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    // Both output formats play through the same mixers; float output (-y) is only how the output writes the samples.
    if (opts->pulse_digi_out_channels == 1) {
        playSynthesizedVoiceMS(opts, state);
    } else if (opts->pulse_digi_out_channels == 2) {
        playSynthesizedVoiceSS(opts, state);
    }
}
