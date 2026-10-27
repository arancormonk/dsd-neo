// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
classify(dsd_input_level_snapshot* snapshot) {
    dsd_input_level_classify(snapshot, -40.0);
}

static void
test_pcm_i16_metrics_with_step_and_guards(void) {
    dsd_input_level_snapshot snapshot;
    int16_t stepped[] = {0, 111, INT16_MAX, 222, INT16_MIN, 333};

    assert(dsd_input_level_metrics_from_pcm_i16(stepped, 6U, 2U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.source == DSD_INPUT_LEVEL_SOURCE_PCM);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snapshot.sample_count == 3U);
    assert(fabs(snapshot.clip_pct - (200.0 / 3.0)) < 1e-9);
    assert(snapshot.peak_dbfs <= 0.0);
    assert(snapshot.peak_dbfs > -0.1);

    assert(dsd_input_level_metrics_from_pcm_i16(NULL, 6U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_pcm_i16(stepped, 0U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_pcm_i16(stepped, 6U, 0U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_pcm_i16(stepped, 6U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, NULL) == -1);
}

static void
test_pcm_f32_i16_scale_metrics_nonfinite_and_guards(void) {
    dsd_input_level_snapshot snapshot;
    float samples[] = {-32768.0f, 0.0f, NAN, INFINITY, -INFINITY};

    assert(dsd_input_level_metrics_from_pcm_f32_i16_scale(samples, 5U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.source == DSD_INPUT_LEVEL_SOURCE_PCM);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snapshot.sample_count == 5U);
    assert(fabs(snapshot.clip_pct - 80.0) < 1e-9);
    assert(snapshot.peak_dbfs <= 0.0);
    assert(snapshot.peak_dbfs > -0.1);

    assert(dsd_input_level_metrics_from_pcm_f32_i16_scale(NULL, 5U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_pcm_f32_i16_scale(samples, 0U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot)
           == -1);
    assert(dsd_input_level_metrics_from_pcm_f32_i16_scale(samples, 5U, 0U, DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot)
           == -1);
    assert(dsd_input_level_metrics_from_pcm_f32_i16_scale(samples, 5U, 1U, DSD_INPUT_LEVEL_SOURCE_PCM, NULL) == -1);
}

static void
test_cu8_metrics(void) {
    uint8_t normal[128];
    for (size_t i = 0U; i < 128U; i++) {
        normal[i] = (i & 1U) ? 136U : 119U;
    }

    dsd_input_level_snapshot snapshot;
    assert(dsd_input_level_metrics_from_cu8(normal, 128U, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(snapshot.status == DSD_INPUT_LEVEL_OK);
    assert(snapshot.sample_count == 128U);
    assert(snapshot.clip_pct == 0.0);

    uint8_t low[64];
    for (size_t i = 0U; i < 64U; i++) {
        low[i] = 128U;
    }
    assert(dsd_input_level_metrics_from_cu8(low, 64U, DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_LOW);

    uint8_t clipped[1000];
    for (size_t i = 0U; i < 1000U; i++) {
        clipped[i] = 128U;
    }
    clipped[17] = 0U;
    assert(dsd_input_level_metrics_from_cu8(clipped, 1000U, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(fabs(snapshot.clip_pct - 0.1) < 1e-9);

    assert(dsd_input_level_metrics_from_cu8(NULL, 128U, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cu8(normal, 0U, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cu8(normal, 128U, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, NULL) == -1);
}

static void
assert_cu8_moments_equal(const dsd_input_level_cu8_moments* lhs, const dsd_input_level_cu8_moments* rhs) {
    assert(lhs != NULL);
    assert(rhs != NULL);
    assert(lhs->count == rhs->count);
    assert(lhs->sum == rhs->sum);
    assert(lhs->sum_sq == rhs->sum_sq);
    assert(lhs->clipped == rhs->clipped);
    assert(lhs->min_sample == rhs->min_sample);
    assert(lhs->max_sample == rhs->max_sample);
}

static void
test_cu8_integer_moments(void) {
    uint8_t all_values[256];
    for (size_t i = 0U; i < sizeof(all_values); i++) {
        all_values[i] = (uint8_t)i;
    }

    dsd_input_level_cu8_moments one_shot;
    dsd_input_level_cu8_moments_reset(&one_shot);
    assert(one_shot.count == 0U);
    assert(one_shot.sum == 0U);
    assert(one_shot.sum_sq == 0U);
    assert(one_shot.clipped == 0U);
    assert(one_shot.min_sample == UINT8_MAX);
    assert(one_shot.max_sample == 0U);
    assert(dsd_input_level_cu8_moments_accumulate(&one_shot, all_values, sizeof(all_values)) == 0);
    assert(one_shot.count == 256U);
    assert(one_shot.sum == 32640U);
    assert(one_shot.sum_sq == 5559680U);
    assert(one_shot.clipped == 4U);
    assert(one_shot.min_sample == 0U);
    assert(one_shot.max_sample == 255U);

    dsd_input_level_cu8_moments incremental;
    dsd_input_level_cu8_moments_reset(&incremental);
    assert(dsd_input_level_cu8_moments_accumulate(&incremental, all_values, 73U) == 0);
    assert(dsd_input_level_cu8_moments_accumulate(&incremental, all_values + 73U, 1U) == 0);
    assert(dsd_input_level_cu8_moments_accumulate(&incremental, all_values + 74U, 182U) == 0);
    assert_cu8_moments_equal(&incremental, &one_shot);

    const uint8_t clipping_edges[] = {0U, 1U, 2U, 253U, 254U, 255U};
    dsd_input_level_cu8_moments edges;
    dsd_input_level_cu8_moments_reset(&edges);
    assert(dsd_input_level_cu8_moments_accumulate(&edges, clipping_edges, sizeof(clipping_edges)) == 0);
    assert(edges.clipped == 4U);
    assert(edges.min_sample == 0U);
    assert(edges.max_sample == 255U);

    dsd_input_level_snapshot snapshot;
    assert(dsd_input_level_cu8_moments_finalize(&one_shot, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    const double mean = 127.5;
    const double variance = 5559680.0 / 256.0 - mean * mean;
    const double expected_rms = 10.0 * log10(variance / (127.5 * 127.5));
    assert(snapshot.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    assert(snapshot.sample_count == 256U);
    assert(fabs(snapshot.rms_dbfs - expected_rms) < 1e-12);
    assert(fabs(snapshot.peak_dbfs) < 1e-12);
    assert(fabs(snapshot.clip_pct - 1.5625) < 1e-12);

    dsd_input_level_snapshot alias_snapshot;
    assert(dsd_input_level_metrics_from_cu8_moments(&one_shot, DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8, &alias_snapshot)
           == 0);
    assert(alias_snapshot.source == DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8);
    assert(alias_snapshot.sample_count == snapshot.sample_count);
    assert(alias_snapshot.rms_dbfs == snapshot.rms_dbfs);
    assert(alias_snapshot.peak_dbfs == snapshot.peak_dbfs);
    assert(alias_snapshot.clip_pct == snapshot.clip_pct);

    uint8_t constant[32];
    DSD_MEMSET(constant, 128, sizeof(constant));
    dsd_input_level_cu8_moments dc;
    dsd_input_level_cu8_moments_reset(&dc);
    assert(dsd_input_level_cu8_moments_accumulate(&dc, constant, sizeof(constant)) == 0);
    assert(dsd_input_level_cu8_moments_finalize(&dc, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    assert(snapshot.rms_dbfs == -120.0);
    assert(fabs(snapshot.peak_dbfs - (20.0 * log10(0.5 / 127.5))) < 1e-12);
    assert(snapshot.clip_pct == 0.0);

    for (size_t i = 0U; i < sizeof(constant); i++) {
        constant[i] = (i & 1U) ? 136U : 120U;
    }
    dsd_input_level_cu8_moments_reset(&dc);
    assert(dsd_input_level_cu8_moments_accumulate(&dc, constant, sizeof(constant)) == 0);
    assert(dsd_input_level_cu8_moments_finalize(&dc, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    assert(fabs(snapshot.rms_dbfs - (10.0 * log10(64.0 / (127.5 * 127.5)))) < 1e-12);
}

static void
test_cu8_moment_guards_and_overflow(void) {
    const uint8_t sample = 255U;
    dsd_input_level_cu8_moments moments;
    dsd_input_level_cu8_moments_reset(&moments);
    dsd_input_level_cu8_moments before = moments;
    dsd_input_level_snapshot snapshot;

    assert(dsd_input_level_cu8_moments_accumulate(NULL, &sample, 1U) == -1);
    assert(dsd_input_level_cu8_moments_accumulate(&moments, NULL, 1U) == -1);
    assert(dsd_input_level_cu8_moments_accumulate(&moments, &sample, 0U) == -1);
    assert_cu8_moments_equal(&moments, &before);
    assert(dsd_input_level_cu8_moments_finalize(NULL, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == -1);
    assert(dsd_input_level_cu8_moments_finalize(&moments, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == -1);
    assert(dsd_input_level_cu8_moments_finalize(&moments, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, NULL) == -1);

    moments.count = UINT64_MAX;
    moments.sum = 255U;
    moments.sum_sq = 65025U;
    moments.clipped = 1U;
    moments.min_sample = 255U;
    moments.max_sample = 255U;
    before = moments;
    assert(dsd_input_level_cu8_moments_accumulate(&moments, &sample, 1U) == -1);
    assert_cu8_moments_equal(&moments, &before);

    dsd_input_level_cu8_moments add;
    dsd_input_level_cu8_moments_reset(&add);
    assert(dsd_input_level_cu8_moments_accumulate(&add, &sample, 1U) == 0);
    moments.count = 1U;
    moments.sum = UINT64_MAX;
    moments.sum_sq = UINT64_MAX;
    moments.clipped = 1U;
    moments.min_sample = 255U;
    moments.max_sample = 255U;
    before = moments;
    assert(dsd_input_level_cu8_moments_merge(&moments, &add) == -1);
    assert_cu8_moments_equal(&moments, &before);
    assert(dsd_input_level_cu8_moments_merge(NULL, &add) == -1);
    assert(dsd_input_level_cu8_moments_merge(&moments, NULL) == -1);
}

/* Compare the bounded reductions with independent 64-bit totals, including a block whose squares exceed 32 bits. */
static void
test_cu8_moment_reduction_boundaries(void) {
    const size_t lengths[] = {15U, 16U, 17U, 65535U, 65536U, 65537U, 131073U};
    uint8_t* storage = (uint8_t*)malloc(131074U);
    assert(storage != NULL);
    uint8_t* samples = storage + 1U;
    for (size_t pattern = 0U; pattern < 3U; pattern++) {
        for (size_t i = 0U; i < 131073U; i++) {
            samples[i] = pattern == 0U ? 255U : (pattern == 1U ? 37U : (uint8_t)(i * 73U + i / 251U));
        }
        for (size_t size = 0U; size < sizeof(lengths) / sizeof(lengths[0]); size++) {
            dsd_input_level_cu8_moments expected;
            dsd_input_level_cu8_moments_reset(&expected);
            expected.count = lengths[size];
            for (size_t i = 0U; i < lengths[size]; i++) {
                const uint8_t sample = samples[i];
                expected.sum += sample;
                expected.sum_sq += (uint64_t)sample * sample;
                expected.clipped += (sample <= 1U || sample >= 254U) ? 1U : 0U;
                if (sample < expected.min_sample) {
                    expected.min_sample = sample;
                }
                if (sample > expected.max_sample) {
                    expected.max_sample = sample;
                }
            }
            dsd_input_level_cu8_moments got;
            dsd_input_level_cu8_moments_reset(&got);
            const int rc = dsd_input_level_cu8_moments_accumulate(&got, samples, lengths[size]);
            assert(rc == 0);
            assert_cu8_moments_equal(&got, &expected);
        }
    }
    free(storage);
}

static void
test_cs16_metrics(void) {
    int16_t hot[1000] = {0};
    hot[10] = 32000;
    dsd_input_level_snapshot snapshot;
    assert(dsd_input_level_metrics_from_cs16(hot, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_HOT);
    assert(snapshot.clip_pct == 0.0);

    int16_t clipped[1000] = {0};
    clipped[20] = -32760;
    assert(dsd_input_level_metrics_from_cs16(clipped, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(fabs(snapshot.clip_pct - 0.1) < 1e-9);

    assert(dsd_input_level_metrics_from_cs16(NULL, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cs16(hot, 0U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cs16(hot, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, NULL) == -1);
}

static void
test_cf32_metrics(void) {
    float hot[1000] = {0.0f};
    hot[8] = 0.95f;
    dsd_input_level_snapshot snapshot;
    assert(dsd_input_level_metrics_from_cf32(hot, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_HOT);
    assert(snapshot.clip_pct == 0.0);

    float clipped[1000] = {0.0f};
    clipped[9] = -0.98f;
    assert(dsd_input_level_metrics_from_cf32(clipped, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(fabs(snapshot.clip_pct - 0.1) < 1e-9);

    float nonfinite[] = {NAN, -INFINITY};
    assert(dsd_input_level_metrics_from_cf32(nonfinite, 2U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(snapshot.clip_pct == 100.0);

    assert(dsd_input_level_metrics_from_cf32(NULL, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cf32(hot, 0U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_cf32(hot, 1000U, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, NULL) == -1);
}

static uint64_t
double_bits(double value) {
    uint64_t bits = 0U;
    _Static_assert(sizeof(bits) == sizeof(value), "exact level comparison requires 64-bit doubles");
    DSD_MEMCPY(&bits, &value, sizeof(bits));
    return bits;
}

/* A scalar ordered reference covers CF32 reductions of short and large blocks, including non-finite samples.
 * Compare the published numeric fields bit for bit; the measurement timestamp is allowed to advance. */
static void
test_cf32_reduction_exact(void) {
    float* storage = (float*)malloc(8194U * sizeof(float));
    assert(storage != NULL);
    float* samples = storage + 1U;
    const size_t lengths[] = {1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U, 17U, 1023U, 8191U, 8192U, 8193U};
    for (size_t pattern = 0U; pattern < 4U; pattern++) {
        for (size_t i = 0U; i < 8193U; i++) {
            samples[i] = pattern == 0U ? 0.125f : (float)((int)(i * 73U % 65537U) - 32768) / 32768.0f;
            if (pattern == 3U) {
                const uint32_t bits = ((uint32_t)(i & 1U) << 31) | ((uint32_t)(105U + i % 24U) << 23)
                                      | ((uint32_t)(i * 2654435761U) & 0x7fffffU);
                _Static_assert(sizeof(bits) == sizeof(samples[i]), "CF32 requires 32-bit floats");
                DSD_MEMCPY(&samples[i], &bits, sizeof(bits));
            }
            if (pattern == 2U && i % 503U == 0U) {
                samples[i] = i % 3U == 0U ? NAN : (i % 3U == 1U ? INFINITY : -INFINITY);
            }
        }
        if (pattern == 2U) {
            samples[1] = nextafterf(0.98f, 0.0f);
            samples[2] = 0.98f;
            samples[3] = nextafterf(0.98f, INFINITY);
            samples[4] = -samples[1];
            samples[5] = -samples[2];
            samples[6] = -samples[3];
        }
        for (size_t k = 0U; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
            const size_t count = lengths[k];
            volatile double power_sum = 0.0;
            volatile double sum = 0.0;
            double peak = 0.0;
            uint64_t clipped = 0U;
            for (size_t i = 0U; i < count; i++) {
                double value = samples[i];
                if (!isfinite(value)) {
                    value = value < 0.0 ? -1.0 : 1.0;
                }
                const double a = fabs(value);
                if (a > peak) {
                    peak = a;
                }
                clipped += a >= 0.98 ? 1U : 0U;
                sum += value;
                power_sum += value * value;
            }
            double mean_power = power_sum - ((sum * sum) / (double)count);
            if (mean_power < 0.0) {
                mean_power = 0.0;
            }
            mean_power /= (double)count;
            double expected_rms = mean_power <= 0.0 ? -120.0 : 10.0 * log10(mean_power);
            expected_rms = fmax(-120.0, fmin(0.0, expected_rms));
            double expected_peak = peak <= 0.0 ? -120.0 : 20.0 * log10(peak);
            expected_peak = fmax(-120.0, fmin(0.0, expected_peak));
            const double expected_clip = (double)clipped * 100.0 / (double)count;
            dsd_input_level_snapshot got;
            const int rc = dsd_input_level_metrics_from_cf32(samples, count, DSD_INPUT_LEVEL_SOURCE_AIRSPY_CF32, &got);
            assert(rc == 0 && got.sample_count == count && got.source == DSD_INPUT_LEVEL_SOURCE_AIRSPY_CF32);
            assert(double_bits(got.rms_dbfs) == double_bits(expected_rms));
            assert(double_bits(got.peak_dbfs) == double_bits(expected_peak));
            assert(double_bits(got.clip_pct) == double_bits(expected_clip));
        }
    }
    free(storage);
}

/* Issue B6: a level is stamped on the real-time monotonic clock in milliseconds when it is measured, so a frontend
   can tell a receiver that stopped from one that is quiet. */
static void
test_levels_stamped_on_realtime_monotonic_ms(void) {
    dsd_input_level_snapshot snapshot;
    const uint8_t cu8[] = {127U, 128U, 120U, 135U};
    const uint64_t before = dsd_realtime_mono_ms();
    assert(dsd_input_level_metrics_from_cu8(cu8, sizeof cu8, DSD_INPUT_LEVEL_SOURCE_RTL_CU8, &snapshot) == 0);
    const uint64_t after = dsd_realtime_mono_ms();
    assert(snapshot.updated_ms >= before && snapshot.updated_ms <= after);
}

static void
test_stale_predicate(void) {
    dsd_input_level_snapshot snapshot;
    DSD_MEMSET(&snapshot, 0, sizeof(snapshot));
    snapshot.source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    snapshot.updated_ms = 50000U;

    /* Never received: no level at all, whatever its stamp. */
    assert(dsd_input_level_is_stale(NULL, 50000U) == 1);
    assert(dsd_input_level_is_stale(&snapshot, 50000U) == 1);

    snapshot.sample_count = 1024U;
    assert(dsd_input_level_is_stale(&snapshot, 50000U) == 0);
    assert(dsd_input_level_is_stale(&snapshot, 50000U + DSD_INPUT_LEVEL_STALE_MS) == 0);
    assert(dsd_input_level_is_stale(&snapshot, 50000U + DSD_INPUT_LEVEL_STALE_MS + 1U) == 1);
    /* A reader whose clock read came before the producer's stamp sees a fresh level, not an ancient one. */
    assert(dsd_input_level_is_stale(&snapshot, 49990U) == 0);
}

static dsd_input_level_snapshot
rf_clipping_level(uint64_t updated_ms) {
    dsd_input_level_snapshot snapshot;
    DSD_MEMSET(&snapshot, 0, sizeof(snapshot));
    snapshot.status = DSD_INPUT_LEVEL_UNKNOWN;
    snapshot.source = DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
    snapshot.rms_dbfs = -6.0;
    snapshot.peak_dbfs = -0.1;
    snapshot.clip_pct = 0.5;
    snapshot.sample_count = 4096U;
    snapshot.updated_ms = updated_ms;
    return snapshot;
}

/* A live receiver's level that is more than DSD_INPUT_LEVEL_STALE_MS old describes a receiver that stopped: the decoder
   keeps it, but does not warn about it. A fresh one still warns. */
static void
test_stale_live_warning_suppressed(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;
    opts->iq_replay_active = 0;
    dsd_decode_clock_use_test(1000ULL * 1000000000ULL);

    const uint64_t now_ms = dsd_realtime_mono_ms();
    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    dsd_input_level_snapshot stale = rf_clipping_level(now_ms - DSD_INPUT_LEVEL_STALE_MS - 500U);
    dsd_input_level_publish(opts, state, &stale, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(state->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    assert(state->input_level_last_toast_time == 0);
    assert(opts->last_input_warn_time == 0);

    dsd_input_level_snapshot fresh = rf_clipping_level(dsd_realtime_mono_ms());
    dsd_input_level_publish(opts, state, &fresh, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strstr(state->ui_msg, "RF Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == 1000);

    dsd_decode_clock_use_system();
    free(state);
    free(opts);
}

/* A replay's advisories follow capture time, so every run prints the same ones: a replay paused for longer than
   DSD_INPUT_LEVEL_STALE_MS of real time (a slow decoder, a debugger, a jittered read) still prints the warning its
   level calls for, at the decode time the capture says. */
static void
test_replay_paused_over_2s_keeps_advisories(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts != NULL && state != NULL);
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->input_warn_db = -40.0;
    opts->input_warn_cooldown_sec = 10;
    opts->iq_replay_active = 1;
    const time_t capture_s = 1262304000; /* 2010-01-01Z */
    dsd_decode_clock_use_test((uint64_t)capture_s * 1000000000ULL);

    dsd_input_level_snapshot paused = rf_clipping_level(dsd_realtime_mono_ms() - DSD_INPUT_LEVEL_STALE_MS - 500U);
    dsd_input_level_publish(opts, state, &paused, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(state->input_level.status == DSD_INPUT_LEVEL_CLIPPING);
    assert(strstr(state->ui_msg, "RF Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == capture_s);
    assert(opts->last_input_warn_time == capture_s);

    /* The repeat waits out its cooldown on capture time, however stale the real-time stamp. */
    DSD_SNPRINTF(state->ui_msg, sizeof(state->ui_msg), "%s", "sentinel");
    dsd_decode_clock_test_set_ns((uint64_t)(capture_s + 5) * 1000000000ULL);
    dsd_input_level_publish(opts, state, &paused, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strcmp(state->ui_msg, "sentinel") == 0);
    dsd_decode_clock_test_set_ns((uint64_t)(capture_s + 10) * 1000000000ULL);
    dsd_input_level_publish(opts, state, &paused, DSD_INPUT_LEVEL_NOTIFY_RF);
    assert(strstr(state->ui_msg, "RF Level CLIP") != NULL);
    assert(state->input_level_last_toast_time == capture_s + 10);

    dsd_decode_clock_use_system();
    free(state);
    free(opts);
}

int
main(void) {
    test_pcm_i16_metrics_with_step_and_guards();
    test_pcm_f32_i16_scale_metrics_nonfinite_and_guards();
    test_cu8_metrics();
    test_cu8_integer_moments();
    test_cu8_moment_reduction_boundaries();
    test_cu8_moment_guards_and_overflow();
    test_cs16_metrics();
    test_cf32_metrics();
    test_cf32_reduction_exact();
    test_levels_stamped_on_realtime_monotonic_ms();
    test_stale_predicate();
    test_stale_live_warning_suppressed();
    test_replay_paused_over_2s_keeps_advisories();
    return 0;
}
