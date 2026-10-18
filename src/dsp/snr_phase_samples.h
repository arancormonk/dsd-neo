// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_DSP_SNR_PHASE_SAMPLES_H
#define DSD_NEO_DSP_SNR_PHASE_SAMPLES_H

#include <cmath>
#include <stddef.h>

static inline bool
dsd_snr_phase_window_valid(int sps, int phase, int window) {
    return sps >= 1 && sps <= 64 && phase >= 0 && phase < sps && window >= 0 && window <= sps / 2;
}

/* The estimator validates SPS (1..64), phase (0..SPS-1), and window (0..SPS/2).
 * Visit the admitted intervals in input order, including a wrapped phase window.
 * The cap applies to finite samples, exactly as in the per-sample phase scan.
 * Compile with IEEE semantics so non-finite values never reach nth_element. */
static inline int
dsd_snr_collect_phase_samples(const float* samples, int sample_count, int sps, int phase, int window, float* vals,
                              int max_vals) {
    if (!samples || !vals || sample_count <= 0 || max_vals <= 0 || !dsd_snr_phase_window_valid(sps, phase, window)) {
        return 0;
    }
    size_t starts[2] = {};
    size_t ends[2] = {};
    const size_t center = (size_t)phase;
    const size_t radius = (size_t)window;
    size_t step = (size_t)sps;
    int spans = 1;
    if (2 * window + 1 >= sps) {
        /* Full coverage needs only one contiguous scan, even at SPS == 1. */
        step = (size_t)sample_count;
        ends[0] = (size_t)sample_count;
    } else if (phase < window) {
        spans = 2;
        ends[0] = center + radius + 1;
        starts[1] = center + step - radius;
        ends[1] = (size_t)sps;
    } else if (phase + window >= sps) {
        spans = 2;
        ends[0] = center + radius - step + 1;
        starts[1] = center - radius;
        ends[1] = (size_t)sps;
    } else {
        starts[0] = center - radius;
        ends[0] = center + radius + 1;
    }

    int count = 0;
    const size_t length = (size_t)sample_count;
    for (size_t base = 0; base < length; base += step) {
        for (int span = 0; span < spans; ++span) {
            const size_t limit = base + ends[span];
            const size_t end = (limit < length) ? limit : length;
            for (size_t i = base + starts[span]; i < end; ++i) {
                const float v = samples[i];
                if (std::isfinite(v)) {
                    vals[count++] = v;
                    if (count == max_vals) {
                        return count;
                    }
                }
            }
        }
    }
    return count;
}

#endif
