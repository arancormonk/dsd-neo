// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: channel squelch zeros lowpassed when below threshold; passes when above.
 * With continuous flow model, squelch sets the flag and zeros the buffer but pipeline
 * continues to produce output (zeros) to maintain UI responsiveness. */

#include <atomic>
#include <cstdlib>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
all_zero(const float* x, int n) {
    for (int i = 0; i < n; i++) {
        if (x[i] != 0.0f) {
            return 0;
        }
    }
    return 1;
}

/* Issue #521: a scan row's --squelch-db reaches the demod as dsd_squelch_level_from_sql() of
 * its whole-dB value. With the channel held at -50 dB, a -60 dB row passes it, a -40 dB row
 * closes on it, and a row that switches the squelch off (0) passes it again. */
static int
row_thresholds_gate_a_fixed_channel(demod_state* s) {
    const int pairs = 200;
    static float buf[(size_t)200 * 2];
    const float amplitude = 0.0031622776f; /* amplitude^2 = 1e-5 = -50 dB */

    const struct {
        int db;
        int squelched;
    } rows[] = {{-60, 0}, {-40, 1}, {0, 0}, {-100, 0}};

    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        for (int n = 0; n < pairs; n++) {
            float sign = (n & 1) ? -1.0f : 1.0f;
            buf[(size_t)(2 * n) + 0] = sign * amplitude;
            buf[(size_t)(2 * n) + 1] = sign * amplitude;
        }
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        s->channel_pwr = 0.0f;
        s->channel_squelch_level.store((float)dsd_squelch_level_from_sql((double)rows[r].db),
                                       std::memory_order_relaxed);
        full_demod(s);
        if (s->channel_squelched != rows[r].squelched) {
            DSD_FPRINTF(stderr, "squelch: row %d dB against a -50 dB channel: squelched=%d, want %d (pwr=%.3g)\n",
                        rows[r].db, s->channel_squelched, rows[r].squelched, s->channel_pwr);
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    const int pairs = 200;
    static float buf[(size_t)pairs * 2];
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    s->mode_demod = &raw_demod; // copy lowpassed -> result

    // Below threshold: set small magnitude on normalized float IQ
    // mean_power of 0.01^2 + 0.01^2 = 0.0002 per pair
    for (int n = 0; n < pairs; n++) {
        buf[(size_t)(2 * n) + 0] = 0.01f;
        buf[(size_t)(2 * n) + 1] = -0.01f;
    }
    // Set threshold above the signal power so squelch triggers
    s->channel_squelch_level = 0.001f; // threshold higher than signal power (~0.0002)

    full_demod(s);
    if (!s->channel_squelched) {
        DSD_FPRINTF(stderr, "squelch: below threshold but channel_squelched not set\n");
        free(s);
        return 1;
    }
    // With continuous flow model, result_len should be > 0 (pipeline continues with zeros)
    if (s->result_len <= 0) {
        DSD_FPRINTF(stderr, "squelch: below threshold but result_len=%d (expected >0 for continuous flow)\n",
                    s->result_len);
        free(s);
        return 1;
    }
    // Verify output is all zeros when squelched
    if (!all_zero(s->result, s->result_len)) {
        DSD_FPRINTF(stderr, "squelch: below threshold but result contains non-zero samples\n");
        free(s);
        return 1;
    }

    // Above threshold: larger magnitude with zero DC should pass
    // Use alternating signs so DC is zero but power is high
    for (int n = 0; n < pairs; n++) {
        float sign = (n & 1) ? -1.0f : 1.0f;
        buf[(size_t)(2 * n) + 0] = sign * 0.3f;
        buf[(size_t)(2 * n) + 1] = sign * 0.3f;
    }
    // DC-corrected mean_power ~ 0.3^2 = 0.09 per sample, well above 0.001 threshold
    s->lowpassed = buf; // reset pointer (may have been modified by full_demod)
    s->lp_len = pairs * 2;
    s->channel_pwr = 0.0f; // reset
    full_demod(s);
    if (s->channel_squelched) {
        DSD_FPRINTF(stderr, "squelch: above threshold but channel_squelched is set (pwr=%.6f, thr=%.6f)\n",
                    s->channel_pwr, s->channel_squelch_level.load(std::memory_order_relaxed));
        free(s);
        return 1;
    }

    if (row_thresholds_gate_a_fixed_channel(s) != 0) {
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
