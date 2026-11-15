// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

enum { TEST_SAMPLE_RATE = 48000, TEST_SYMBOL_RATE = 4800, TEST_SPS = 10 };

#define TEST_TWO_PI 6.28318530717958647692

static void
synthesize_fsk_iq(float* out_iq, const float* symbols, int symbol_count, int sps, float deviation_per_level) {
    float phase = 0.17f;
    int out = 0;
    for (int sym = 0; sym < symbol_count; sym++) {
        for (int k = 0; k < sps; k++) {
            phase += deviation_per_level * symbols[sym];
            if (phase > 3.14159265358979323846f) {
                phase -= 6.28318530717958647692f;
            } else if (phase < -3.14159265358979323846f) {
                phase += 6.28318530717958647692f;
            }
            out_iq[out++] = 0.85f * cosf(phase);
            out_iq[out++] = 0.85f * sinf(phase);
        }
    }
}

static void
expect_discriminator_count_sign_center_and_scale(void) {
    enum { SYMBOLS = 160, SAMPLES = SYMBOLS * TEST_SPS };

    static const float cycle[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float symbols[SYMBOLS];
    float iq[SAMPLES * 2];
    float out[SAMPLES];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = cycle[i & 3];
    }
    synthesize_fsk_iq(iq, symbols, SYMBOLS, TEST_SPS, 0.026f);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {TEST_SAMPLE_RATE, TEST_SYMBOL_RATE, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);

    int produced = dsd_fsk_modem_discriminator_process(&modem, iq, SAMPLES * 2, out, SAMPLES);
    assert(produced == SAMPLES);
    assert(out[0] == 0.0f);
    assert(modem.have_prev == 1);

    int checked = 0;
    int correct_sign = 0;
    float peak = 0.0f;
    double mean = 0.0;
    const int stable_start = TEST_SPS * 24;
    for (int i = stable_start; i < produced; i++) {
        int sym = i / TEST_SPS;
        if (sym >= SYMBOLS) {
            sym = SYMBOLS - 1;
        }
        float sample = out[i];
        assert(sample >= -32768.0f);
        assert(sample <= 32767.0f);
        mean += (double)sample;
        if (fabsf(sample) > peak) {
            peak = fabsf(sample);
        }
        if (fabsf(symbols[sym]) >= 2.0f) {
            checked++;
            if ((sample > 0.0f) == (symbols[sym] > 0.0f)) {
                correct_sign++;
            }
        }
    }
    mean /= (double)(produced - stable_start);
    assert(checked > 400);
    assert(correct_sign >= (checked * 95) / 100);
    /* The outer symbols' phase step, 3 x 0.026 rad, is 596 Hz at 48 kHz: 2980 counts on the fixed scale. */
    const float outer_counts =
        (float)(3.0 * 0.026 * TEST_SAMPLE_RATE / TEST_TWO_PI * DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ);
    assert(fabsf(peak - outer_counts) < 0.01f * outer_counts);
    assert(fabs(mean) < 0.01 * outer_counts);
}

static void
expect_reset_clears_discriminator_history(void) {
    enum { SYMBOLS = 24, SAMPLES = SYMBOLS * TEST_SPS };

    static const float cycle[] = {-3.0f, 3.0f, -1.0f, 1.0f};
    float symbols[SYMBOLS];
    float iq[SAMPLES * 2];
    float out[SAMPLES];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = cycle[i & 3];
    }
    synthesize_fsk_iq(iq, symbols, SYMBOLS, TEST_SPS, 0.03f);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {TEST_SAMPLE_RATE, TEST_SYMBOL_RATE, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);
    assert(dsd_fsk_modem_discriminator_process(&modem, iq, SAMPLES * 2, out, SAMPLES) == SAMPLES);
    assert(modem.have_prev == 1);
    const float scale = modem.scale;

    dsd_fsk_modem_reset(&modem);
    assert(modem.have_prev == 0);
    assert(modem.dc_est == 0.0f);
    assert(fabsf(modem.scale - scale) <= 1e-6f * fabsf(scale));
    assert(modem.cfg.sample_rate_hz == TEST_SAMPLE_RATE);
    assert(modem.cfg.symbol_rate_hz == TEST_SYMBOL_RATE);
    assert(modem.cfg.levels == 4);

    int produced = dsd_fsk_modem_discriminator_process(&modem, iq, SAMPLES * 2, out, SAMPLES);
    assert(produced == SAMPLES);
    assert(out[0] == 0.0f);
}

static void
expect_configure_preserves_or_resets_state(void) {
    enum { SYMBOLS = 16, SAMPLES = SYMBOLS * TEST_SPS };

    float symbols[SYMBOLS];
    float iq[SAMPLES * 2];
    float out[SAMPLES];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = (i & 1) ? 3.0f : -3.0f;
    }
    synthesize_fsk_iq(iq, symbols, SYMBOLS, TEST_SPS, 0.025f);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {TEST_SAMPLE_RATE, TEST_SYMBOL_RATE, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    assert(dsd_fsk_modem_discriminator_process(&modem, iq, SAMPLES * 2, out, SAMPLES) == SAMPLES);
    assert(modem.have_prev == 1);

    dsd_fsk_modem_configure(&modem, &cfg);
    assert(modem.have_prev == 1);

    dsd_fsk_modem_config next = {0, 0, 99, 1};
    dsd_fsk_modem_configure(&modem, &next);
    assert(modem.cfg.sample_rate_hz == TEST_SAMPLE_RATE);
    assert(modem.cfg.symbol_rate_hz == TEST_SYMBOL_RATE);
    assert(modem.cfg.levels == 4);
    assert(modem.cfg.channel_profile == 1);
    assert(modem.have_prev == 0);
    assert(modem.dc_est == 0.0f);
    assert(modem.scale > 0.0f);
}

/* A carrier held at @p freq_hz off centre for @p count samples at @p rate_hz, its phase continuing from *@p phase. */
static void
synthesize_tone_iq(float* out_iq, int count, double freq_hz, int rate_hz, double* phase) {
    for (int n = 0; n < count; n++) {
        *phase += TEST_TWO_PI * freq_hz / (double)rate_hz;
        out_iq[(size_t)n * 2U] = (float)(0.85 * cos(*phase));
        out_iq[((size_t)n * 2U) + 1U] = (float)(0.85 * sin(*phase));
    }
}

/* The scale is fixed in hertz: a deviation reads the same count at any sample rate, the PCM range (+/-32767) holds
   6553 Hz, clear of every FSK protocol's peaks, and beyond it the output is not clipped. */
static void
expect_fixed_scale_in_hertz(void) {
    enum { SAMPLES = 64 };

    static const int rates[] = {24000, 48000, 50000, 96000};
    static const double freqs[] = {1800.0, -2400.0, 4900.0, 7000.0};
    float iq[SAMPLES * 2];
    float out[SAMPLES];
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
            dsd_fsk_modem_state modem;
            dsd_fsk_modem_config cfg = {rates[r], TEST_SYMBOL_RATE, 4, 2};
            dsd_fsk_modem_init(&modem, &cfg);
            double phase = 0.3;
            synthesize_tone_iq(iq, SAMPLES, freqs[f], rates[r], &phase);
            assert(dsd_fsk_modem_discriminator_process(&modem, iq, SAMPLES * 2, out, SAMPLES) == SAMPLES);
            const double want = freqs[f] * DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ;
            /* The carrier centering has moved by at most SAMPLES / 4000 of the tone by the second sample. */
            const double got = (double)out[1];
            if (!(fabs(got - want) < 0.02 * fabs(want))) {
                DSD_FPRINTF(stderr, "%d Hz rate, %.0f Hz: %.1f counts, want %.1f\n", rates[r], freqs[f], got, want);
                assert(0);
            }
        }
    }
    assert(fabsf(DSD_FSK_DISCRIMINATOR_COUNTS_PER_HZ * 3000.0f - 15000.0f) < 1e-3f);
}

/* No gain follows the signal: a word read just after loud noise, or just after a reset (a squelched stretch), reads
   at the level it reads after a long stretch of the same signal. A peak normaliser put the first outer symbols of a
   transmission at up to twice the level of the frame behind them. */
static void
expect_level_independent_of_history(void) {
    enum { LEAD = 4800, BODY = 480 };

    static float iq[(LEAD + BODY) * 2];
    static float out[LEAD + BODY];
    static float ref[LEAD + BODY];
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (int lead_kind = 0; lead_kind < 3; lead_kind++) {
        dsd_fsk_modem_state modem;
        dsd_fsk_modem_config cfg = {TEST_SAMPLE_RATE, TEST_SYMBOL_RATE, 4, 2};
        dsd_fsk_modem_init(&modem, &cfg);
        double phase = 0.0;
        for (int n = 0; n < LEAD; n++) {
            if (lead_kind == 0) {
                /* the same signal: alternating outer symbols at 1800 Hz */
                phase += TEST_TWO_PI * (((n / TEST_SPS) & 1) ? 1800.0 : -1800.0) / TEST_SAMPLE_RATE;
            } else {
                /* noise: a phase that jumps anywhere every sample */
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                phase = TEST_TWO_PI * (double)(rng >> 11) / 9007199254740992.0;
            }
            iq[(size_t)n * 2U] = (float)(0.85 * cos(phase));
            iq[((size_t)n * 2U) + 1U] = (float)(0.85 * sin(phase));
        }
        for (int n = LEAD; n < LEAD + BODY; n++) {
            phase += TEST_TWO_PI * (((n / TEST_SPS) & 1) ? 1800.0 : -1800.0) / TEST_SAMPLE_RATE;
            iq[(size_t)n * 2U] = (float)(0.85 * cos(phase));
            iq[((size_t)n * 2U) + 1U] = (float)(0.85 * sin(phase));
        }
        if (lead_kind == 2) {
            /* noise, then a reset (the front end resets the modem across a squelched stretch) */
            assert(dsd_fsk_modem_discriminator_process(&modem, iq, LEAD * 2, out, LEAD) == LEAD);
            dsd_fsk_modem_reset(&modem);
            assert(dsd_fsk_modem_discriminator_process(&modem, iq + ((size_t)LEAD * 2U), BODY * 2, out + LEAD, BODY)
                   == BODY);
        } else {
            assert(dsd_fsk_modem_discriminator_process(&modem, iq, (LEAD + BODY) * 2, out, LEAD + BODY) == LEAD + BODY);
        }
        if (lead_kind == 0) {
            DSD_MEMCPY(ref, out, sizeof(ref));
            continue;
        }
        /* The body's held symbols, away from the transitions and the reset's first sample. */
        for (int n = LEAD + TEST_SPS; n < LEAD + BODY; n++) {
            if ((n % TEST_SPS) < 2) {
                continue;
            }
            const float want = ref[n];
            assert(fabsf(want) > 8000.0f);
            /* The carrier centering learned some of the noise's mean: a few percent at most. */
            if (!(fabsf(out[n] - want) < 0.05f * fabsf(want))) {
                DSD_FPRINTF(stderr, "lead %d sample %d: %.1f, want %.1f\n", lead_kind, n, (double)out[n], (double)want);
                assert(0);
            }
        }
    }
}

int
main(void) {
    expect_discriminator_count_sign_center_and_scale();
    expect_reset_clears_discriminator_history();
    expect_configure_preserves_or_resets_state();
    expect_fixed_scale_in_hertz();
    expect_level_independent_of_history();
    printf("DSP_FSK_MODEM: OK\n");
    return 0;
}
