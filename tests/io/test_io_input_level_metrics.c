// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/input_level.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

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

static void
test_fsk_symbol_clip_metric_is_not_rf_clipping(void) {
    dsd_input_level_snapshot snapshot;
    assert(dsd_input_level_metrics_from_fsk_clip(12.5f, 256U, &snapshot) == 0);
    classify(&snapshot);
    assert(snapshot.source == DSD_INPUT_LEVEL_SOURCE_FSK_SYMBOL);
    assert(snapshot.status == DSD_INPUT_LEVEL_UNKNOWN);
    assert(dsd_input_level_source_is_rf(snapshot.source) == 0);
    assert(snapshot.sample_count == 256U);
    assert(fabs(snapshot.clip_pct - 12.5) < 1e-6);

    assert(dsd_input_level_metrics_from_fsk_clip(-2.0f, 12U, &snapshot) == 0);
    assert(snapshot.clip_pct == 0.0);
    assert(dsd_input_level_metrics_from_fsk_clip(12.5f, 0U, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_fsk_clip(NAN, 12U, &snapshot) == -1);
    assert(dsd_input_level_metrics_from_fsk_clip(12.5f, 12U, NULL) == -1);
}

int
main(void) {
    test_pcm_i16_metrics_with_step_and_guards();
    test_pcm_f32_i16_scale_metrics_nonfinite_and_guards();
    test_cu8_metrics();
    test_cs16_metrics();
    test_cf32_metrics();
    test_fsk_symbol_clip_metric_is_not_rf_clipping();
    return 0;
}
