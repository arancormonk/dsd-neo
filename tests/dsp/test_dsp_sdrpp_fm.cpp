// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for the SDR++-style non-CQPSK FM demodulation path. */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/mem.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void
free_fm_filter_state(demod_state* s) {
    if (!s) {
        return;
    }
    if (s->fm_channel_lpf_taps) {
        dsd_neo_aligned_free(s->fm_channel_lpf_taps);
    }
    if (s->fm_channel_lpf_hist_i) {
        dsd_neo_aligned_free(s->fm_channel_lpf_hist_i);
    }
    if (s->fm_channel_lpf_hist_q) {
        dsd_neo_aligned_free(s->fm_channel_lpf_hist_q);
    }
    if (s->fm_audio_lpf_taps) {
        dsd_neo_aligned_free(s->fm_audio_lpf_taps);
    }
    if (s->fm_audio_lpf_hist) {
        dsd_neo_aligned_free(s->fm_audio_lpf_hist);
    }
    if (s->post_polydecim_taps) {
        dsd_neo_aligned_free(s->post_polydecim_taps);
    }
    if (s->post_polydecim_hist) {
        dsd_neo_aligned_free(s->post_polydecim_hist);
    }
}

static double
rms_range(const float* x, int start, int stop) {
    double acc = 0.0;
    int n = 0;
    for (int i = start; i < stop; i++) {
        acc += (double)x[i] * (double)x[i];
        n++;
    }
    return n > 0 ? std::sqrt(acc / (double)n) : 0.0;
}

static void
fill_fm_tone(float* iq, int pairs, double fs, double tone_hz, double deviation_hz) {
    double phase = 0.0;
    for (int n = 0; n < pairs; n++) {
        double audio = std::sin(2.0 * M_PI * tone_hz * (double)n / fs);
        phase += 2.0 * M_PI * deviation_hz * audio / fs;
        iq[(size_t)(n << 1) + 0] = (float)std::cos(phase);
        iq[(size_t)(n << 1) + 1] = (float)std::sin(phase);
    }
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));

    {
        const int pairs = 512;
        static float iq[(size_t)pairs * 2];
        const double fs = 50000.0;
        const double bw = 12500.0;
        const double deviation = bw * 0.5;
        const double dphi = 2.0 * M_PI * deviation / fs;
        for (int n = 0; n < pairs; n++) {
            double phase = (double)n * dphi;
            iq[(size_t)(n << 1) + 0] = (float)std::cos(phase);
            iq[(size_t)(n << 1) + 1] = (float)std::sin(phase);
        }

        s->lowpassed = iq;
        s->lp_len = pairs * 2;
        s->rate_out = (int)fs;
        s->fm_demod_bw_hz = (int)bw;
        s->fm_demod_history_valid = 0;
        s->fll_enabled = 0;
        dsd_fm_demod(s);

        if (s->result_len != pairs) {
            fprintf(stderr, "sdrpp fm norm: result_len=%d want %d\n", s->result_len, pairs);
            free(s);
            return 1;
        }
        for (int i = 1; i < s->result_len; i++) {
            if (std::fabs(s->result[i] - 1.0f) > 0.01f) {
                fprintf(stderr, "sdrpp fm norm: result[%d]=%f want ~1.0\n", i, s->result[i]);
                free_fm_filter_state(s);
                free(s);
                return 1;
            }
        }
    }

    {
        memset(s, 0, sizeof(*s));
        const int pairs = 512;
        static float iq[(size_t)pairs * 2];
        const double fs = 50000.0;
        const double bw = 12500.0;
        const double deviation = bw * 0.5;
        const double dphi = 2.0 * M_PI * deviation / fs;
        for (int n = 0; n < pairs; n++) {
            double phase = (double)n * dphi;
            iq[(size_t)(n << 1) + 0] = (float)std::cos(phase);
            iq[(size_t)(n << 1) + 1] = (float)std::sin(phase);
        }

        s->lowpassed = iq;
        s->lp_len = pairs * 2;
        s->rate_in = (int)fs;
        s->rate_out = (int)(fs / 2.0);
        s->post_downsample = 2;
        s->fm_demod_bw_hz = (int)bw;
        s->fm_demod_history_valid = 0;
        s->fll_enabled = 0;
        dsd_fm_demod(s);

        if (s->result_len != pairs) {
            fprintf(stderr, "sdrpp fm post rate norm: result_len=%d want %d\n", s->result_len, pairs);
            free(s);
            return 1;
        }
        for (int i = 1; i < s->result_len; i++) {
            if (std::fabs(s->result[i] - 1.0f) > 0.01f) {
                fprintf(stderr, "sdrpp fm post rate norm: result[%d]=%f want ~1.0\n", i, s->result[i]);
                free_fm_filter_state(s);
                free(s);
                return 1;
            }
        }
    }

    {
        memset(s, 0, sizeof(*s));
        const int pairs = 4096;
        static float iq_inband[(size_t)pairs * 2];
        static float iq_oob[(size_t)pairs * 2];
        const double fs = 50000.0;
        const double bw = 12500.0;
        fill_fm_tone(iq_inband, pairs, fs, 1000.0, 1200.0);
        fill_fm_tone(iq_oob, pairs, fs, 12000.0, 1200.0);

        s->lowpassed = iq_inband;
        s->lp_len = pairs * 2;
        s->rate_in = (int)fs;
        s->rate_out = (int)fs;
        s->rate_out2 = -1;
        s->mode_demod = &dsd_fm_demod;
        s->fm_demod_bw_hz = (int)bw;
        s->fm_audio_lpf_enable = 1;
        s->squelch_gate_open = 1;
        s->squelch_env = 1.0f;
        full_demod(s);
        double inband = rms_range(s->result, 1024, s->result_len);

        s->lowpassed = iq_oob;
        s->lp_len = pairs * 2;
        s->fm_demod_history_valid = 0;
        if (s->fm_audio_lpf_hist && s->fm_audio_lpf_taps_len > 1) {
            memset(s->fm_audio_lpf_hist, 0, (size_t)(s->fm_audio_lpf_taps_len - 1) * sizeof(float));
        }
        full_demod(s);
        double oob = rms_range(s->result, 1024, s->result_len);

        if (!(inband > 0.10)) {
            fprintf(stderr, "sdrpp fm lpf: in-band RMS too low: %.6f\n", inband);
            free_fm_filter_state(s);
            free(s);
            return 1;
        }
        if (!(oob < inband * 0.35)) {
            fprintf(stderr, "sdrpp fm lpf: oob RMS %.6f not sufficiently below in-band %.6f\n", oob, inband);
            free_fm_filter_state(s);
            free(s);
            return 1;
        }
    }

    free_fm_filter_state(s);
    memset(s, 0, sizeof(*s));

    {
        const int pairs = 4096;
        static float iq[(size_t)pairs * 2];
        const double fs = 50000.0;
        const double bw = 12500.0;
        fill_fm_tone(iq, pairs, fs, 1000.0, 1200.0);

        s->lowpassed = iq;
        s->lp_len = pairs * 2;
        s->rate_in = (int)fs;
        s->rate_out = (int)(fs / 2.0);
        s->post_downsample = 2;
        s->rate_out2 = -1;
        s->mode_demod = &dsd_fm_demod;
        s->channel_lpf_enable = 1;
        s->fm_demod_bw_hz = (int)bw;
        s->fm_audio_lpf_enable = 1;
        s->squelch_gate_open = 1;
        s->squelch_env = 1.0f;
        full_demod(s);

        if (s->fm_channel_lpf_rate_hz != (int)fs) {
            fprintf(stderr, "sdrpp fm post rate: channel LPF rate=%d want %d\n", s->fm_channel_lpf_rate_hz, (int)fs);
            free_fm_filter_state(s);
            free(s);
            return 1;
        }
        if (s->fm_audio_lpf_rate_hz != (int)fs) {
            fprintf(stderr, "sdrpp fm post rate: audio LPF rate=%d want %d\n", s->fm_audio_lpf_rate_hz, (int)fs);
            free_fm_filter_state(s);
            free(s);
            return 1;
        }
    }

    free_fm_filter_state(s);
    free(s);
    return 0;
}
