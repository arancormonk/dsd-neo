// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: channel squelch zeros lowpassed when below threshold; passes when above.
 * With continuous flow model, squelch sets the flag and zeros the buffer but pipeline
 * continues to produce output (zeros) to maintain UI responsiveness. */

#include <atomic>
#include <cmath>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/mem.h>
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

/* A demod state running the AM monitor (dsd_demod_am_active()). */
static void
make_am_monitor(demod_state* s) {
    s->mode_demod = &dsd_am_demod;
    s->analog_family = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->cqpsk_enable = 0;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->rate_out = 48000;
}

/* Fill @p pairs complex samples of a carrier at 0 Hz with amplitude @p a and phase @p phi, plus a receiver DC offset
   (@p dc_i, @p dc_q). */
static void
fill_carrier(float* buf, int pairs, double a, double phi, double dc_i, double dc_q) {
    for (int n = 0; n < pairs; n++) {
        buf[(size_t)(2 * n) + 0] = (float)(a * cos(phi) + dc_i);
        buf[(size_t)(2 * n) + 1] = (float)(a * sin(phi) + dc_q);
    }
}

/* Issue #518 follow-up: an AM carrier sits at 0 Hz. mean_power() pooled I and Q and took their common mean out, so a
   steady carrier read A²(1 - sin 2φ)/4 -- nothing at φ = 45° -- and the squelch chopped the channel as the carrier's
   phase drifted. On the AM monitor the carrier's power reads A²/2 at every phase, and a receiver DC offset is measured
   with it, as the vector sum (the AM detector sees the same sum); off AM nothing changes. */
static int
am_carrier_power_is_phase_independent(demod_state* s) {
    const int pairs = 256;
    static float buf[(size_t)256 * 2];
    const double a = 0.1;
    const double want = a * a / 2.0;
    int rc = 0;
    for (int k = 0; k < 16; k++) {
        const double phi = (double)k * (M_PI / 8.0);
        fill_carrier(buf, pairs, a, phi, 0.0, 0.0);
        DSD_MEMSET(s, 0, sizeof(*s));
        make_am_monitor(s);
        s->lowpassed = buf;
        s->lp_len = pairs * 2;
        s->channel_squelch_level.store((float)dsd_squelch_level_from_sql(-40.0), std::memory_order_relaxed);
        full_demod(s);
        if (fabs((double)s->channel_pwr - want) > 1e-6 * want + 1e-12 || s->channel_squelched) {
            DSD_FPRINTF(stderr, "squelch: AM carrier at phase %.3f read %.6g (want %.6g), squelched=%d\n", phi,
                        s->channel_pwr, want, s->channel_squelched);
            rc = 1;
        }
    }
    /* Carrier C plus receiver DC D: the vector sum |C + D|²/2, phase-dependent; opposite phasors cancel. */
    const double phi = 0.3;
    const double dc_i = 0.02;
    const double dc_q = -0.01;
    fill_carrier(buf, pairs, a, phi, dc_i, dc_q);
    DSD_MEMSET(s, 0, sizeof(*s));
    make_am_monitor(s);
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    const double si = a * cos(phi) + dc_i;
    const double sq = a * sin(phi) + dc_q;
    const double want_sum = (si * si + sq * sq) / 2.0;
    if (fabs((double)s->channel_pwr - want_sum) > 1e-6 * want_sum) {
        DSD_FPRINTF(stderr, "squelch: AM carrier plus DC read %.6g (want %.6g)\n", s->channel_pwr, want_sum);
        rc = 1;
    }
    /* Off AM the pooled measurement is unchanged: the same 45° carrier still reads as no power on the FM monitor. */
    fill_carrier(buf, pairs, a, M_PI / 4.0, 0.0, 0.0);
    DSD_MEMSET(s, 0, sizeof(*s));
    s->mode_demod = &raw_demod;
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    if (fabs((double)s->channel_pwr) > 1e-9) {
        DSD_FPRINTF(stderr, "squelch: non-AM measurement changed: %.6g\n", s->channel_pwr);
        rc = 1;
    }
    return rc;
}

int
main(void) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
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
        dsd_neo_aligned_free(s);
        return 1;
    }
    // With continuous flow model, result_len should be > 0 (pipeline continues with zeros)
    if (s->result_len <= 0) {
        DSD_FPRINTF(stderr, "squelch: below threshold but result_len=%d (expected >0 for continuous flow)\n",
                    s->result_len);
        dsd_neo_aligned_free(s);
        return 1;
    }
    // Verify output is all zeros when squelched
    if (!all_zero(s->result, s->result_len)) {
        DSD_FPRINTF(stderr, "squelch: below threshold but result contains non-zero samples\n");
        dsd_neo_aligned_free(s);
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
        dsd_neo_aligned_free(s);
        return 1;
    }

    if (row_thresholds_gate_a_fixed_channel(s) != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    if (am_carrier_power_is_phase_independent(s) != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    dsd_neo_aligned_free(s);
    return 0;
}
