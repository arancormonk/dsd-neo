// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-enum-size)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include "dsd-neo/core/safe_api.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

static void
reset_demod(demod_state* s) {
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = 48000;
    s->rate_out = 48000;
    s->rate_out2 = 12000;
    s->post_downsample = 4;
    s->deemph = 1;
    s->dc_block = 1;
    s->audio_lpf_enable = 1;
    s->audio_lpf_alpha = 0.2f;
    s->mode_demod = &dsd_fm_demod;
    s->lowpassed = s->input_cb_buf;
    s->channel_lpf_enable = 0;
}

static void
synthesize_fsk_iq(float* out_iq, const float* symbols, int symbol_count, int sps, float deviation_per_level) {
    float phase = 0.19f;
    int out = 0;
    for (int sym = 0; sym < symbol_count; sym++) {
        for (int k = 0; k < sps; k++) {
            phase += deviation_per_level * symbols[sym];
            if (phase > kPi) {
                phase -= 2.0f * kPi;
            } else if (phase < -kPi) {
                phase += 2.0f * kPi;
            }
            out_iq[out++] = 0.85f * std::cos(phase);
            out_iq[out++] = 0.85f * std::sin(phase);
        }
    }
}

static int
check_fsk_discriminator_output_contract(demod_state* s) {
    enum : unsigned short { SYMBOLS = 256, SPS = 10 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float expected[SYMBOLS];

    reset_demod(s);
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->lp_len = SYMBOLS * SPS * 2;

    for (int i = 0; i < SYMBOLS; i++) {
        expected[i] = levels[(i / 8) & 3];
    }
    synthesize_fsk_iq(s->input_cb_buf, expected, SYMBOLS, SPS, 0.026f);

    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = s->rate_out;
    cfg.symbol_rate_hz = s->symbol_rate_hz;
    cfg.levels = s->symbol_levels;
    cfg.channel_profile = DSD_CH_LPF_PROFILE_12K5;
    dsd_fsk_modem_init(&s->fsk_modem_state, &cfg);

    full_demod(s);

    if (s->result_len != SYMBOLS * SPS) {
        DSD_FPRINTF(stderr, "FSK discriminator result_len=%d, expected %d samples\n", s->result_len, SYMBOLS * SPS);
        return 1;
    }
    if (s->result[0] != 0.0f) {
        DSD_FPRINTF(stderr, "FSK discriminator first sample %.3f, expected 0\n", s->result[0]);
        return 1;
    }

    int checked = 0;
    int ok = 0;
    float peak = 0.0f;
    double mean = 0.0;
    const int stable_start = SPS * 32;
    for (int i = stable_start; i < s->result_len; i++) {
        int sym = i / SPS;
        if (sym >= SYMBOLS) {
            sym = SYMBOLS - 1;
        }
        float sample = s->result[i];
        mean += (double)sample;
        peak = std::max(peak, std::fabs(sample));
        if (std::fabs(expected[sym]) >= 2.0f) {
            checked++;
            if ((sample > 0.0f) == (expected[sym] > 0.0f)) {
                ok++;
            }
        }
        if (sample < -32768.0f || sample > 32767.0f) {
            DSD_FPRINTF(stderr, "FSK discriminator sample[%d]=%.1f outside PCM range\n", i, sample);
            return 1;
        }
    }
    mean /= (double)(s->result_len - stable_start);
    if (checked < 1000 || ok < (checked * 95) / 100 || peak < 25000.0f || std::fabs(mean) > 1500.0) {
        DSD_FPRINTF(stderr, "FSK discriminator sign=%d/%d peak=%.1f mean=%.1f\n", ok, checked, peak, mean);
        return 1;
    }

    return 0;
}

static void
synthesize_dqpsk_iq(float* out_iq, const float* differential_symbols, int symbol_count) {
    float phase = 0.0f;
    for (int i = 0; i < symbol_count; i++) {
        phase += differential_symbols[i] * (kPi / 4.0f);
        while (phase > kPi) {
            phase -= 2.0f * kPi;
        }
        while (phase < -kPi) {
            phase += 2.0f * kPi;
        }
        out_iq[(i << 1) + 0] = std::cos(phase);
        out_iq[(i << 1) + 1] = std::sin(phase);
    }
}

static int
check_cqpsk_symbol_output_contract(demod_state* s) {
    enum : unsigned short { SYMBOLS = 256 };

    static const float expected_cycle[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float expected[SYMBOLS];

    reset_demod(s);
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->cqpsk_enable = 1;
    s->symbol_rate_hz = 4800;
    s->symbol_levels = 4;
    s->lp_len = SYMBOLS * 2;
    s->mode_demod = &raw_demod;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;

    for (int i = 0; i < SYMBOLS; i++) {
        expected[i] = expected_cycle[i & 3];
    }
    synthesize_dqpsk_iq(s->input_cb_buf, expected, SYMBOLS);

    full_demod(s);

    if (s->result_len != SYMBOLS) {
        DSD_FPRINTF(stderr, "CQPSK symbol path result_len=%d, expected %d\n", s->result_len, SYMBOLS);
        return 1;
    }
    for (int i = 0; i < SYMBOLS; i++) {
        if (std::fabs(s->result[i] - expected[i]) > 0.02f) {
            DSD_FPRINTF(stderr, "CQPSK symbol[%d]=%.4f, expected %.1f\n", i, s->result[i], expected[i]);
            return 1;
        }
    }
    return 0;
}

static int
check_cqpsk_phase_extractor_accuracy(demod_state* s) {
    enum : unsigned short { SAMPLES = 721 };

    reset_demod(s);
    s->lowpassed = s->input_cb_buf;
    s->lp_len = SAMPLES * 2;

    const float k4_over_pi = 4.0f / kPi;
    for (int i = 0; i < SAMPLES; i++) {
        float phase = -kPi + (2.0f * kPi * (float)i) / (float)(SAMPLES - 1);
        s->input_cb_buf[(size_t)(i << 1) + 0] = std::cos(phase);
        s->input_cb_buf[(size_t)(i << 1) + 1] = std::sin(phase);
    }

    qpsk_differential_demod(s);
    if (s->result_len != SAMPLES) {
        DSD_FPRINTF(stderr, "CQPSK phase extractor result_len=%d, expected %d\n", s->result_len, SAMPLES);
        return 1;
    }

    for (int i = 0; i < SAMPLES; i++) {
        float i_sample = s->input_cb_buf[(size_t)(i << 1) + 0];
        float q_sample = s->input_cb_buf[(size_t)(i << 1) + 1];
        float expected = std::atan2(q_sample, i_sample) * k4_over_pi;
        if (std::fabs(s->result[i] - expected) > 0.004f) {
            DSD_FPRINTF(stderr, "CQPSK phase extractor[%d]=%.6f, expected %.6f\n", i, s->result[i], expected);
            return 1;
        }
    }

    return 0;
}

static int
check_cqpsk_squelch_emits_zero_symbols(demod_state* s) {
    enum : unsigned short { PAIRS = 20, SPS = 7 };

    reset_demod(s);
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->cqpsk_enable = 1;
    s->ted_sps = SPS;
    s->lp_len = PAIRS * 2;
    s->mode_demod = &raw_demod;
    s->channel_squelch_level = 0.001f;
    for (int i = 0; i < s->lp_len; i++) {
        s->input_cb_buf[i] = 0.0f;
    }

    full_demod(s);

    const int expected_symbols = (PAIRS + SPS - 1) / SPS;
    if (!s->channel_squelched || s->squelch_gate_open) {
        DSD_FPRINTF(stderr, "CQPSK squelch state squelched=%d gate=%d\n", s->channel_squelched, s->squelch_gate_open);
        return 1;
    }
    if (s->result_len != expected_symbols) {
        DSD_FPRINTF(stderr, "CQPSK squelch result_len=%d, expected %d\n", s->result_len, expected_symbols);
        return 1;
    }
    for (int i = 0; i < s->result_len; i++) {
        if (s->result[i] != 0.0f) {
            DSD_FPRINTF(stderr, "CQPSK squelch result[%d]=%.3f, expected zero\n", i, s->result[i]);
            return 1;
        }
    }
    return 0;
}

} // namespace

int
main(void) {
    demod_state* s = static_cast<demod_state*>(std::malloc(sizeof(*s)));
    if (!s) {
        return 1;
    }

    int rc = 0;
    rc |= check_fsk_discriminator_output_contract(s);
    rc |= check_cqpsk_symbol_output_contract(s);
    rc |= check_cqpsk_phase_extractor_accuracy(s);
    rc |= check_cqpsk_squelch_emits_zero_symbols(s);

    std::free(s);
    return rc;
}

// NOLINTEND(performance-enum-size)
