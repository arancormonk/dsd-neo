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
#include "dsd-neo/core/safe_api.h"

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

static void
synthesize_fsk_fractional(float* iq, const float* symbols, int symbol_count, int sample_count, float actual_sps,
                          float deviation_per_level, float cfo, float noise_amp) {
    float phase = 0.41f;
    int out = 0;
    for (int sample = 0; sample < sample_count; sample++) {
        int sym = (int)floorf((float)sample / actual_sps);
        if (sym < 0) {
            sym = 0;
        } else if (sym >= symbol_count) {
            sym = symbol_count - 1;
        }
        phase += cfo + deviation_per_level * symbols[sym];
        if (phase > DSD_TEST_PI) {
            phase -= 2.0f * DSD_TEST_PI;
        } else if (phase < -DSD_TEST_PI) {
            phase += 2.0f * DSD_TEST_PI;
        }
        float amp = 0.82f + 0.18f * (float)((sample / 3) % 19) / 18.0f;
        iq[out++] = amp * cosf(phase) + noise_amp * test_rand_pm1();
        iq[out++] = amp * sinf(phase) + noise_amp * test_rand_pm1();
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
        DSD_FPRINTF(stderr, "%s: 4-level accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok,
                    checked);
        assert(0);
    }
}

static void
expect_4level_accuracy_best_shift(const char* name, const float* out, int out_count, const float* expected,
                                  int expected_count, int run_len, int warmup, int max_abs_shift, float min_accuracy) {
    float best_accuracy = 0.0f;
    int best_ok = 0;
    int best_checked = 0;
    int best_shift = 0;
    for (int shift = -max_abs_shift; shift <= max_abs_shift; shift++) {
        int checked = 0;
        int ok = 0;
        int limit = out_count < expected_count ? out_count : expected_count;
        for (int i = warmup; i < limit; i++) {
            int expected_i = i + shift;
            if (expected_i < 0 || expected_i >= expected_count) {
                continue;
            }
            int pos = expected_i % run_len;
            if (pos == 0 || pos >= run_len - 2) {
                continue;
            }
            checked++;
            if (classify4(out[i]) == (int)expected[expected_i]) {
                ok++;
            }
        }
        if (checked > 200) {
            float accuracy = (float)ok / (float)checked;
            if (accuracy > best_accuracy) {
                best_accuracy = accuracy;
                best_ok = ok;
                best_checked = checked;
                best_shift = shift;
            }
        }
    }
    if (best_accuracy < min_accuracy) {
        DSD_FPRINTF(stderr, "%s: best shifted 4-level accuracy %.3f below %.3f shift=%d (%d/%d)\n", name, best_accuracy,
                    min_accuracy, best_shift, best_ok, best_checked);
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
        DSD_FPRINTF(stderr, "%s: 2-level accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok,
                    checked);
        assert(0);
    }
}

static void
seed_tracking_boundary_window_scaled(dsd_fsk_modem_state* modem, int clock_i, int best_phase, int second_phase,
                                     float start_phase, float best_delta, float second_delta) {
    int target = clock_i * 64;
    assert(target <= DSD_FSK_MODEM_TRACK_MAX_SAMPLES);
    DSD_MEMSET(modem->track_freq, 0, sizeof(modem->track_freq));
    for (int pos = best_phase; pos < target - 1; pos += clock_i) {
        if (pos > 0) {
            modem->track_freq[pos - 1] = -1.0f;
            modem->track_freq[pos] = -1.0f + best_delta;
        }
    }
    if (second_phase >= 0) {
        for (int pos = second_phase; pos < target - 1; pos += clock_i) {
            if (pos > 0) {
                modem->track_freq[pos - 1] = -1.0f;
                modem->track_freq[pos] = -1.0f + second_delta;
            }
        }
    }
    modem->track_len = target - 1;
    modem->track_start_phase = start_phase;
}

static void
seed_tracking_boundary_window(dsd_fsk_modem_state* modem, int clock_i, int best_phase, float start_phase) {
    seed_tracking_boundary_window_scaled(modem, clock_i, best_phase, -1, start_phase, 2.0f, 0.0f);
}

static void
seed_tracking_constant_window(dsd_fsk_modem_state* modem, int clock_i, float start_phase, float value) {
    int target = clock_i * 64;
    assert(target <= DSD_FSK_MODEM_TRACK_MAX_SAMPLES);
    for (int i = 0; i < target - 1; i++) {
        modem->track_freq[i] = value;
    }
    modem->track_len = target - 1;
    modem->track_start_phase = start_phase;
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
        DSD_FPRINTF(stderr, "%s accuracy %.3f below %.3f (%d/%d)\n", name, accuracy, min_accuracy, ok, checked);
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
test_soft_metrics_track_clean_signal(void) {
    enum { SYMBOLS = 1400, SPS = 10, RUN = 13 };

    static const float levels[] = {1.0f, -1.0f, -1.0f, 1.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 16, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.035f, 0.0f, 0.0f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 2, 1};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 16);
    assert(produced >= SYMBOLS - 2);

    dsd_fsk_modem_metrics metrics;
    assert(dsd_fsk_modem_get_metrics(&modem, &metrics) == 0);
    assert(metrics.valid == 1);
    assert(metrics.levels == 2);
    assert(metrics.symbol_rate_hz == 4800);
    assert(metrics.window_symbols > 0);
    assert(metrics.window_symbols <= 256U);
    assert(metrics.symbols_total >= (uint64_t)(SYMBOLS - 2));
    assert(metrics.mean_reliability > 128U);
    assert(metrics.min_reliability <= metrics.mean_reliability);
    assert(metrics.rms_error >= 0.0f && metrics.rms_error < 0.8f);
    assert(metrics.evm_snr_db > 0.0f);
    assert(metrics.low_reliability_pct < 50.0f);
    assert(metrics.clip_pct == 0.0f);
    assert(metrics.timing_acquired == modem.timing_acquired);
    assert(metrics.track_updates == modem.track_updates);
    assert(metrics.track_skips == modem.track_skips);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_soft_metrics_invalidate_on_zero_symbols(void) {
    enum { SYMBOLS = 240, SPS = 10 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float symbols[SYMBOLS];
    float iq[SYMBOLS * SPS * 2];
    float out[SYMBOLS + 32];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = levels[i & 3];
    }
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.026f, 0.0f, 0.0f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 32);
    assert(produced > 0);

    dsd_fsk_modem_metrics metrics;
    assert(dsd_fsk_modem_get_metrics(&modem, &metrics) == 0);
    assert(metrics.valid == 1);

    int zeros = dsd_fsk_modem_zero_symbols(&modem, SPS * 8, out, SYMBOLS + 32);
    assert(zeros > 0);
    assert(dsd_fsk_modem_get_metrics(&modem, &metrics) == 0);
    assert(metrics.valid == 0);
    assert(metrics.window_symbols == 0U);
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
test_timing_tracker_clamps_phase_underflow(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.have_prev = 1;
    modem.prev_i = 1.0f;
    modem.prev_q = 0.0f;
    modem.symbol_phase = 9.1f;
    modem.symbol_accum = 0.1f;
    modem.symbol_count = 9;
    seed_tracking_boundary_window(&modem, 10, 2, 0.0f);

    float iq[] = {1.0f, 0.0f};
    float out[8];
    (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    assert(modem.track_updates > 0);
    assert(modem.symbol_phase >= 0.0f);
    assert(modem.symbol_phase < 1.0f);
}

static void
test_timing_tracker_clamps_phase_overflow(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.have_prev = 1;
    modem.prev_i = 1.0f;
    modem.prev_q = 0.0f;
    modem.symbol_phase = 8.95f;
    seed_tracking_boundary_window(&modem, 10, 0, 8.0f);

    float iq[] = {1.0f, 0.0f};
    float out[8];
    (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    assert(modem.track_updates > 0);
    assert(modem.symbol_phase > 9.0f);
    assert(modem.symbol_phase < 10.0f);
}

static void
test_timing_tracker_uses_floor_for_fractional_start_phase(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.have_prev = 1;
    modem.prev_i = 1.0f;
    modem.prev_q = 0.0f;
    modem.symbol_phase = 0.6f;
    seed_tracking_boundary_window(&modem, 10, 0, 0.6f);

    float iq[] = {1.0f, 0.0f};
    float out[8];
    (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    assert(modem.track_updates > 0);
    assert(fabsf(modem.track_last_error) < 1.0e-6f);
    assert(fabsf(modem.symbol_phase - 1.6f) < 1.0e-4f);
}

static void
test_dmr_gfsk_tracks_fractional_symbol_clock(void) {
    enum { SYMBOLS = 4200, RUN = 17 };

    const float actual_sps = 10.005f;
    int sample_count = (int)((float)SYMBOLS * actual_sps);

    static const float levels[] = {-3.0f, 3.0f, -1.0f, 1.0f, 3.0f, -3.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* iq = (float*)calloc((size_t)sample_count * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 512, sizeof(float));
    assert(symbols && iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk_fractional(iq, symbols, SYMBOLS, sample_count, actual_sps, 0.025f, 0.0015f, 0.002f);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, sample_count * 2, out, SYMBOLS + 512);
    assert(produced >= SYMBOLS - 24);
    assert(modem.track_updates > 0);
    expect_4level_accuracy("dmr-gfsk-fractional-clock-tracking", out, produced, symbols, SYMBOLS, RUN, 900, 0.88f);

    free(out);
    free(iq);
    free(symbols);
}

static void
test_timing_tracker_skips_low_confidence_windows(void) {
    enum { SYMBOLS = 128, SPS = 10, ZERO_SAMPLES = SPS * 80 };

    static const float levels[] = {-3.0f, -1.0f, 1.0f, 3.0f};
    float symbols[SYMBOLS];
    float iq[SYMBOLS * SPS * 2];
    float zeros[ZERO_SAMPLES * 2];
    float out[SYMBOLS + 128];

    for (int i = 0; i < SYMBOLS; i++) {
        symbols[i] = levels[i & 3];
    }
    synthesize_fsk(iq, symbols, SYMBOLS, SPS, 0.026f, 0.0f, 0.0f, 0);
    DSD_MEMSET(zeros, 0, sizeof(zeros));

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 4};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, iq, SYMBOLS * SPS * 2, out, SYMBOLS + 128);
    assert(produced > 0);
    assert(modem.timing_acquired == 1);

    modem.track_len = 0;
    modem.have_prev = 0;
    uint64_t skips_before = modem.track_skips;
    (void)dsd_fsk_modem_process(&modem, zeros, ZERO_SAMPLES * 2, out, SYMBOLS + 128);
    assert(modem.track_skips > skips_before);
}

static void
test_timing_tracker_applies_bounded_soft_correction(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.have_prev = 1;
    modem.prev_i = 1.0f;
    modem.prev_q = 0.0f;
    modem.symbol_phase = 5.0f;
    modem.track_consecutive_skips = 4;
    seed_tracking_boundary_window_scaled(&modem, 10, 2, 7, 0.0f, 1.05f, 1.0f);

    float iq[] = {1.0f, 0.0f};
    float out[8];
    (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    assert(modem.track_updates == 1);
    assert(modem.track_skips == 0);
    assert(modem.track_consecutive_skips == 0);
    assert(modem.symbol_phase < 6.0f);
    assert(modem.symbol_phase > 5.86f);
}

static void
test_timing_tracker_soft_accepts_do_not_mask_signal_loss(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.symbol_phase = 5.0f;
    modem.abs_est = 10.0f;

    float iq[] = {1.0f, 0.0f};
    float out[8];
    for (int i = 0; i < 32; i++) {
        modem.have_prev = 1;
        modem.prev_i = 1.0f;
        modem.prev_q = 0.0f;
        seed_tracking_boundary_window_scaled(&modem, 10, 2, 7, 8.0f, 1.05f, 1.0f);
        (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    }

    assert(modem.track_updates == 32);
    assert(modem.track_skips == 0);
    assert(modem.track_consecutive_skips == 0);
    assert(modem.timing_acquired == 0);
}

static void
test_timing_tracker_preserves_low_transition_signal(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.symbol_phase = 4.0f;
    modem.abs_est = 0.05f;

    float iq[] = {cosf(0.05f), sinf(0.05f)};
    float out[8];
    for (int i = 0; i < 40; i++) {
        modem.have_prev = 1;
        modem.prev_i = 1.0f;
        modem.prev_q = 0.0f;
        modem.dc_est = 0.0f;
        seed_tracking_constant_window(&modem, 10, 0.0f, 0.05f);
        (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
        assert(modem.timing_acquired == 1);
    }

    assert(modem.track_skips == 40);
    assert(modem.track_consecutive_skips == 0);
}

static void
test_timing_tracker_reacquires_after_consecutive_skips(void) {
    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    modem.timing_acquired = 1;
    modem.have_prev = 1;
    modem.prev_i = 1.0f;
    modem.prev_q = 0.0f;
    modem.symbol_phase = 4.0f;

    float iq[] = {1.0f, 0.0f};
    float out[8];
    for (int i = 0; i < 32; i++) {
        modem.track_len = 10 * 64 - 1;
        modem.track_start_phase = 0.0f;
        DSD_MEMSET(modem.track_freq, 0, sizeof(modem.track_freq));
        (void)dsd_fsk_modem_process(&modem, iq, 2, out, 8);
    }

    assert(modem.track_skips == 32);
    assert(modem.track_consecutive_skips == 0);
    assert(modem.timing_acquired == 0);
    assert(modem.acq_len == 0);
    assert(modem.symbol_count == 0);
    assert(modem.pending_len == 0);
}

static void
test_timing_reacquire_waits_through_long_silence(void) {
    enum { SYMBOLS = 2200, SPS = 10, RUN = 16, GAP_COMPLEX = (33 * 64 * SPS) + DSD_FSK_MODEM_ACQ_MAX_SAMPLES + 500 };

    static const float levels[] = {-3.0f, 3.0f, -1.0f, 1.0f, 3.0f, -3.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* first_iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* gap_iq = (float*)calloc((size_t)GAP_COMPLEX * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 512, sizeof(float));
    assert(symbols && first_iq && gap_iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(first_iq, symbols, SYMBOLS, SPS, 0.025f, 0.001f, 0.002f, 0);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, first_iq, SYMBOLS * SPS * 2, out, SYMBOLS + 512);
    assert(produced >= SYMBOLS - 24);
    assert(modem.timing_acquired == 1);

    (void)dsd_fsk_modem_process(&modem, gap_iq, GAP_COMPLEX * 2, out, SYMBOLS + 512);
    assert(modem.timing_acquired == 0);
    assert(modem.acq_len > 0);
    assert(modem.acq_len < DSD_FSK_MODEM_ACQ_MAX_SAMPLES);

    free(out);
    free(gap_iq);
    free(first_iq);
    free(symbols);
}

static void
test_timing_tracker_recovers_after_gap(void) {
    enum { SYMBOLS = 2200, SPS = 10, RUN = 16, GAP_COMPLEX = (33 * 64 * SPS) + 100 };

    static const float levels[] = {-3.0f, 3.0f, -1.0f, 1.0f, 3.0f, -3.0f};
    float* symbols = (float*)calloc(SYMBOLS, sizeof(float));
    float* first_iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* gap_iq = (float*)calloc((size_t)GAP_COMPLEX * 2U, sizeof(float));
    float* second_iq = (float*)calloc((size_t)SYMBOLS * SPS * 2U, sizeof(float));
    float* out = (float*)calloc(SYMBOLS + 512, sizeof(float));
    assert(symbols && first_iq && gap_iq && second_iq && out);

    fill_run_pattern(symbols, SYMBOLS, levels, (int)(sizeof(levels) / sizeof(levels[0])), RUN);
    synthesize_fsk(first_iq, symbols, SYMBOLS, SPS, 0.025f, 0.001f, 0.002f, 0);
    synthesize_fsk(second_iq, symbols, SYMBOLS, SPS, 0.025f, -0.001f, 0.002f, 7);

    dsd_fsk_modem_state modem;
    dsd_fsk_modem_config cfg = {48000, 4800, 4, 2};
    dsd_fsk_modem_init(&modem, &cfg);
    int produced = dsd_fsk_modem_process(&modem, first_iq, SYMBOLS * SPS * 2, out, SYMBOLS + 512);
    assert(produced >= SYMBOLS - 24);
    assert(modem.timing_acquired == 1);

    (void)dsd_fsk_modem_process(&modem, gap_iq, GAP_COMPLEX * 2, out, SYMBOLS + 512);
    assert(modem.timing_acquired == 0);

    produced = dsd_fsk_modem_process(&modem, second_iq, SYMBOLS * SPS * 2, out, SYMBOLS + 512);
    assert(produced >= SYMBOLS - 256);
    assert(modem.timing_acquired == 1);
    expect_4level_accuracy_best_shift("dmr-gfsk-reacquire-after-gap", out, produced, symbols, SYMBOLS, RUN, 900, 64,
                                      0.86f);

    free(out);
    free(second_iq);
    free(gap_iq);
    free(first_iq);
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
    dsd_fsk_modem_metrics metrics;
    assert(dsd_fsk_modem_get_metrics(&modem, &metrics) == 0);
    assert(metrics.valid == 1);
    modem.track_len = 11;
    modem.track_start_phase = 3.0f;
    modem.track_last_error = 2.0f;
    modem.track_last_score = 0.5f;
    modem.track_signal_ref = 0.25f;
    modem.track_updates = 7;
    modem.track_skips = 5;
    modem.track_consecutive_skips = 3;

    dsd_fsk_modem_config next = {48000, 2400, 2, 1};
    dsd_fsk_modem_configure(&modem, &next);
    assert(modem.cfg.sample_rate_hz == 48000);
    assert(modem.cfg.symbol_rate_hz == 2400);
    assert(modem.cfg.levels == 2);
    assert(modem.cfg.channel_profile == 1);
    assert(modem.have_prev == 0);
    assert(modem.symbol_count == 0);
    assert(modem.symbols_emitted == 0);
    assert(modem.track_len == 0);
    assert(modem.track_start_phase == 0.0f);
    assert(modem.track_last_error == 0.0f);
    assert(modem.track_last_score == 0.0f);
    assert(modem.track_signal_ref == 0.0f);
    assert(modem.track_updates == 0);
    assert(modem.track_skips == 0);
    assert(modem.track_consecutive_skips == 0);
    assert(dsd_fsk_modem_get_metrics(&modem, &metrics) == 0);
    assert(metrics.valid == 0);
    assert(metrics.window_symbols == 0U);
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
    test_soft_metrics_track_clean_signal();
    test_soft_metrics_invalidate_on_zero_symbols();
    test_acquisition_preserves_backlog_with_small_output();
    test_binary_fsk_4800();
    test_nxdn48_2400_at_48000();
    test_timing_tracker_clamps_phase_underflow();
    test_timing_tracker_clamps_phase_overflow();
    test_timing_tracker_uses_floor_for_fractional_start_phase();
    test_dmr_gfsk_tracks_fractional_symbol_clock();
    test_timing_tracker_skips_low_confidence_windows();
    test_timing_tracker_applies_bounded_soft_correction();
    test_timing_tracker_soft_accepts_do_not_mask_signal_loss();
    test_timing_tracker_preserves_low_transition_signal();
    test_timing_tracker_reacquires_after_consecutive_skips();
    test_timing_reacquire_waits_through_long_silence();
    test_timing_tracker_recovers_after_gap();
    test_reconfigure_resets_state();
    return 0;
}
