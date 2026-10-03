// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Two-level symbol reliability, kept out of dsd_dibit.c so that it can be built with IEEE semantics: it rejects
 * NaN and infinite symbols and thresholds, which the global fast-math option would otherwise compile out. It runs
 * once per ProVoice or D-STAR voice symbol, and its arithmetic compiles the same either way.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/fp_opaque.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

uint8_t
dsd_two_level_symbol_reliability(const dsd_opts* opts, const dsd_state* state, float symbol) {
    symbol = dsd_fp_opaque_f(symbol); /* keep the NaN test when LTO inlines this into fast-math code */
    if (opts == NULL || state == NULL || !isfinite(symbol)) {
        return 0U;
    }

    /* A replayed symbol whose stored amplitude was unusable stands in as 0, which is no confidence only when the
     * thresholds centre on 0. Its bit is an erasure whatever they are. */
    if ((opts->audio_in_type == AUDIO_IN_SYMBOL_BIN || opts->audio_in_type == AUDIO_IN_SYMBOL_FLT)
        && state->symbol_replay_symbol_unusable) {
        return 0U;
    }

    /* A legacy symbol capture keeps only the decided bit, which replay turns back into an ideal four-level
     * amplitude (dsd_symbol_level_from_dibit()). Those amplitudes are not confidences, so the bit keeps a hard
     * decision's weight. The soft capture format records the measured symbol and is used as it is. */
    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN && state->symbol_replay_format != DSD_SYMBOL_REPLAY_FORMAT_SOFT) {
        return 255U;
    }

    /* The two-level form of soft_metric_for_bit()'s scale: full confidence at an ideal level. For
     * two-level modes the warm start at sync sets min and max to the class means with center between
     * them. Thresholds that are not ordered that way, or whose spacing is not a usable number, carry no
     * information about confidence, so the bit keeps the weight a hard decision gives it. */
    const float center = state->center;
    const float half_spacing = 0.5f * (state->max - state->min);
    if (!(state->min < center && center < state->max) || !isfinite(half_spacing) || half_spacing < 1e-6f) {
        return 255U;
    }

    const float scaled = 255.0f * fabsf(symbol - center) / half_spacing;
    if (scaled >= 255.0f) {
        return 255U;
    }
    return (uint8_t)lrintf(scaled);
}
