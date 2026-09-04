// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <math.h>
#include <stdio.h>

enum { TEST_SAMPLE_RATE = 48000, TEST_SYMBOL_RATE = 4800, TEST_SPS = 10 };

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
    assert(modem.discriminator_peak_est > 0.0f);

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
    assert(peak > 25000.0f);
    assert(fabs(mean) < 2500.0);
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
    assert(modem.discriminator_peak_est > 0.0f);

    dsd_fsk_modem_reset(&modem);
    assert(modem.have_prev == 0);
    assert(modem.dc_est == 0.0f);
    assert(modem.discriminator_peak_est == 0.0f);
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
    assert(modem.discriminator_peak_est == 0.0f);
}

int
main(void) {
    expect_discriminator_count_sign_center_and_scale();
    expect_reset_clears_discriminator_history();
    expect_configure_preserves_or_resets_state();
    printf("DSP_FSK_MODEM: OK\n");
    return 0;
}
