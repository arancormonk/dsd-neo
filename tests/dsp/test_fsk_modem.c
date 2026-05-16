// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef DSD_TEST_PI
#define DSD_TEST_PI 3.14159265358979323846f
#endif

static uint32_t g_rng = 0x12345678u;

static float
test_rand_pm1(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return ((float)((g_rng >> 8) & 0xFFFFu) / 32767.5f) - 1.0f;
}

static void
fill_run_pattern(float* symbols, int count, const float* levels, int level_count, int run_len) {
    for (int i = 0; i < count; i++) {
        int idx = (i / run_len) % level_count;
        symbols[i] = levels[idx];
    }
}

static void
synthesize_fsk(float* iq, const float* symbols, int symbol_count, int sps, float deviation_per_level, float cfo,
               float noise_amp, int timing_offset_samples) {
    float phase = 0.37f;
    int first_symbol = 0;
    for (int k = 0; k < timing_offset_samples; k++) {
        phase += cfo + deviation_per_level * symbols[first_symbol];
        if (phase > DSD_TEST_PI) {
            phase -= 2.0f * DSD_TEST_PI;
        } else if (phase < -DSD_TEST_PI) {
            phase += 2.0f * DSD_TEST_PI;
        }
    }

    int out = 0;
    for (int sym = 0; sym < symbol_count; sym++) {
        for (int k = 0; k < sps; k++) {
            phase += cfo + deviation_per_level * symbols[sym];
            if (phase > DSD_TEST_PI) {
                phase -= 2.0f * DSD_TEST_PI;
            } else if (phase < -DSD_TEST_PI) {
                phase += 2.0f * DSD_TEST_PI;
            }
            float amp = 0.72f + 0.36f * (float)((sym + k) % 29) / 28.0f;
            iq[out++] = amp * cosf(phase) + noise_amp * test_rand_pm1();
            iq[out++] = amp * sinf(phase) + noise_amp * test_rand_pm1();
        }
    }
}

static void
synthesize_fsk_capture_offset(float* iq, const float* symbols, int sample_count, int sps, float deviation_per_level,
                              int capture_offset_samples) {
    float phase = 0.29f;
    int out = 0;
    for (int sample = 0; sample < sample_count; sample++) {
        int absolute_sample = sample + capture_offset_samples;
        int sym = absolute_sample / sps;
        phase += deviation_per_level * symbols[sym];
        if (phase > DSD_TEST_PI) {
            phase -= 2.0f * DSD_TEST_PI;
        } else if (phase < -DSD_TEST_PI) {
            phase += 2.0f * DSD_TEST_PI;
        }
        iq[out++] = cosf(phase);
        iq[out++] = sinf(phase);
    }
}

static int
classify4(float sym) {
    if (sym < -2.0f) {
        return -3;
    }
    if (sym < 0.0f) {
        return -1;
    }
    if (sym < 2.0f) {
        return 1;
    }
    return 3;
}

static int
classify2(float sym) {
    return sym >= 0.0f ? 1 : -1;
}

static void
expect_4level_accuracy(const char* name, const float* out, int out_count, const float* expected, int expected_count,
                       int run_len, int warmup, float min_accuracy) {
    int checked = 0;
    int ok = 0;
    int limit = out_count < expected_count ? out_count : expected_count;
    for (int i = warmup; i < limit; i++) {
        int pos = i % run_len;
        if (pos == 0 || pos >= run_len - 2) {
            continue;
        }
        checked++;
        if (classify4(out[i]) == (int)expected[i]) {
            ok++;
        }
    }
    assert(checked > 200);
    float accuracy = (float)ok / (float)checked;
    if (accuracy < min_accuracy) {
        fprintf(stderr, "%s: 4-level accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok, checked);
        assert(0);
    }
}

static void
expect_2level_accuracy(const char* name, const float* out, int out_count, const float* expected, int expected_count,
                       int run_len, int warmup, float min_accuracy) {
    int checked = 0;
    int ok = 0;
    int limit = out_count < expected_count ? out_count : expected_count;
    for (int i = warmup; i < limit; i++) {
        int pos = i % run_len;
        if (pos == 0 || pos >= run_len - 2) {
            continue;
        }
        checked++;
        if (classify2(out[i]) == (int)expected[i]) {
            ok++;
        }
    }
    assert(checked > 200);
    float accuracy = (float)ok / (float)checked;
    if (accuracy < min_accuracy) {
        fprintf(stderr, "%s: 2-level accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok, checked);
        assert(0);
    }
}

static void
test_p25_c4fm_4800_at_48000(void) {
    enum { SYMBOLS = 1600, SPS = 10, RUN = 12 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f, 1.0f, -1.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.026f, 0.0f, 0.0f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 2);
    expect_4level_accuracy("p25-c4fm-4800", out, produced, symbols, SYMBOLS, RUN, 160, 0.96f);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_dmr_gfsk_4800_with_offsets_noise_and_cfo(void) {
    enum { SYMBOLS = 2600, SPS = 10, RUN = 14 };

    static const float levels[] = {-3.0f, 3.0f, -1.0f, 1.0f, 3.0f, -3.0f, 1.0f, -1.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.024f, 0.0025f, 0.004f, 3);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 2);
    expect_4level_accuracy("dmr-gfsk-offset-cfo-noise", out, produced, symbols, SYMBOLS, RUN, 500, 0.90f);

    free(out);
    free(iq);
    free(symbols);
}

static void
run_mid_symbol_start_case(const char* name, const float* levels, int level_count, int cfg_levels, int channel_profile,
                          int symbol_rate, int sps, int offset, float deviation_per_level, int use_4level,
                          float min_accuracy) {
    enum { SYMBOLS = 1800 };

    int expected_shift = offset > 0 ? 1 : 0;

    float* symbols = (float*)calloc(SYMBOLS + expected_shift + 8, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * (size_t)sps * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    for (int i = 0; i < SYMBOLS + expected_shift + 8; i++) {
        g_rng = g_rng * 1664525u + 1013904223u;
        symbols[i] = levels[(g_rng >> 29) % (uint32_t)level_count];
    }
    synthesize_fsk_capture_offset(iq, symbols, SYMBOLS * sps, sps, deviation_per_level, offset);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, symbol_rate, cfg_levels, channel_profile};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * sps * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 8);

    int checked = 0;
    int ok = 0;
    int limit = produced < SYMBOLS - expected_shift ? produced : SYMBOLS - expected_shift;
    for (int i = 220; i < limit; i++) {
        checked++;
        int got = use_4level ? classify4(out[i]) : classify2(out[i]);
        if (got == (int)symbols[i + expected_shift]) {
            ok++;
        }
    }
    assert(checked > 500);
    float accuracy = (float)ok / (float)checked;
    if (accuracy < min_accuracy) {
        fprintf(stderr, "%s accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok, checked);
        assert(0);
    }

    free(out);
    free(iq);
    free(symbols);
}

static void
test_dmr_gfsk_acquires_mid_symbol_start(void) {
    static const float levels[] = {-3.0f, 3.0f, 1.0f, -1.0f};
    run_mid_symbol_start_case("dmr-gfsk-mid-symbol-start", levels, 4, 4, 2, 4800, 10, 7, 0.026f, 1, 0.90f);
}

static void
test_p25_c4fm_acquires_mid_symbol_start(void) {
    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    run_mid_symbol_start_case("p25-c4fm-mid-symbol-start", levels, 4, 4, 4, 4800, 10, 6, 0.026f, 1, 0.90f);
}

static void
test_binary_fsk_acquires_mid_symbol_start(void) {
    static const float levels[] = {-1.0f, 1.0f};
    run_mid_symbol_start_case("binary-fsk-mid-symbol-start", levels, 2, 2, 1, 4800, 10, 7, 0.035f, 0, 0.94f);
}

static void
test_provoice_fsk_acquires_mid_symbol_start(void) {
    static const float levels[] = {-1.0f, 1.0f};
    run_mid_symbol_start_case("provoice-fsk-mid-symbol-start", levels, 2, 2, 3, 9600, 5, 3, 0.035f, 0, 0.94f);
}

static void
test_squelch_clears_partial_acquisition(void) {
    enum { SYMBOLS = 24, SPS = 10 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float symbols[SYMBOLS];
    float iq[SYMBOLS * SPS * 2];
    float out[64];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = levels[i & 3];
    }
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.026f, 0.0f, 0.0f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, 64);
    assert(produced == 0);
    assert(modem.acq_len > 0);
    assert(modem.timing_acquired == 0);
    assert(modem.have_prev == 1);

    int zeros = dsd_fsk_modem_zero_symbols(&modem, SYMBOLS * SPS, out, 64);
    assert(zeros > 0);
    assert(modem.acq_len == 0);
    assert(modem.timing_acquired == 0);
    assert(modem.have_prev == 0);
}

static void
test_acquisition_preserves_backlog_with_small_output(void) {
    enum { SYMBOLS = 3600, SPS = 10, RUN = 13, CHUNK = 7 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f, 1.0f, -1.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    float chunk[CHUNK];
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.026f, 0.0f, 0.0f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);

    int total = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, CHUNK);
    assert(total == CHUNK);
    assert(modem.pending_heap != NULL);
    assert(modem.pending_cap > DSD_FSK_MODEM_PENDING_INLINE_SYMBOLS);
    while (total < SYMBOLS && modem.pending_pos < modem.pending_len) {
        int produced = dsd_fsk_modem_process(&modem, NULL, 0, chunk, CHUNK);
        assert(produced > 0);
        for (int i = 0; i < produced; i++) {
            out[total++] = chunk[i];
        }
    }

    assert(total >= SYMBOLS - 2);
    assert(modem.pending_heap == NULL);
    expect_4level_accuracy("small-output-acquisition-backlog", out, total, symbols, SYMBOLS, RUN, 160, 0.96f);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_binary_fsk_4800(void) {
    enum { SYMBOLS = 1600, SPS = 10, RUN = 11 };

    static const float levels[] = {1.0f, -1.0f, -1.0f, 1.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.035f, -0.001f, 0.001f, 2);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 2, 1};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 2);
    expect_2level_accuracy("binary-fsk-4800", out, produced, symbols, SYMBOLS, RUN, 250, 0.94f);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_nxdn48_2400_at_48000(void) {
    enum { SYMBOLS = 1200, SPS = 20, RUN = 10 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.018f, 0.0f, 0.002f, 5);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 2400, 4, 1};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 2);
    expect_4level_accuracy("nxdn48-2400", out, produced, symbols, SYMBOLS, RUN, 180, 0.94f);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_reconfigure_resets_state(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);

    float iq[128 * 10 * 2];
    float out[160];
    float symbols[128];
    for (int i = 0; i < 128; i++) {
        symbols[i] = 3.0f;
    }
    synthesize_fsk(iq, symbols, 128, 10, 0.02f, 0.0f, 0.0f, 0);
    (void)dsd_fsk_modem_process(&modem, iq, 128 * 10 * 2, out, 160);
    assert(modem.have_prev == 1);
    assert(modem.symbols_emitted > 0);

    dsd_fsk_modem_config next = {48000, 2400, 2, 1};
    dsd_fsk_modem_configure(&modem, &next);
    assert(modem.cfg.sample_rate_hz == 48000);
    assert(modem.cfg.symbol_rate_hz == 2400);
    assert(modem.cfg.levels == 2);
    assert(modem.cfg.channel_profile == 1);
    assert(modem.have_prev == 0);
    assert(modem.symbol_count == 0);
    assert(modem.symbols_emitted == 0);
}

int
main(void) {
    test_p25_c4fm_4800_at_48000();
    test_dmr_gfsk_4800_with_offsets_noise_and_cfo();
    test_dmr_gfsk_acquires_mid_symbol_start();
    test_p25_c4fm_acquires_mid_symbol_start();
    test_binary_fsk_acquires_mid_symbol_start();
    test_provoice_fsk_acquires_mid_symbol_start();
    test_squelch_clears_partial_acquisition();
    test_acquisition_preserves_backlog_with_small_output();
    test_binary_fsk_4800();
    test_nxdn48_2400_at_48000();
    test_reconfigure_resets_state();
    return 0;
}
