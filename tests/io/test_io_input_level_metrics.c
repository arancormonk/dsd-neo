// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

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

int
main(void) {
    test_pcm_i16_metrics_with_step_and_guards();
    test_pcm_f32_i16_scale_metrics_nonfinite_and_guards();
    test_cu8_metrics();
    test_cu8_integer_moments();
    test_cu8_moment_guards_and_overflow();
    test_cs16_metrics();
    test_cf32_metrics();
    return 0;
}
