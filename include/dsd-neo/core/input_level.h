// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Advisory input-level health metrics and classification helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_INPUT_LEVEL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_INPUT_LEVEL_H_

#include <dsd-neo/platform/platform.h>

#include <stdint.h>
#include <time.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DSD_ATTR_PACKED dsd_input_level_status {
    DSD_INPUT_LEVEL_UNKNOWN = 0,
    DSD_INPUT_LEVEL_OK = 1,
    DSD_INPUT_LEVEL_LOW = 2,
    DSD_INPUT_LEVEL_HOT = 3,
    DSD_INPUT_LEVEL_CLIPPING = 4,
} dsd_input_level_status;

typedef enum DSD_ATTR_PACKED dsd_input_level_source {
    DSD_INPUT_LEVEL_SOURCE_UNKNOWN = 0,
    DSD_INPUT_LEVEL_SOURCE_PCM = 1,
    DSD_INPUT_LEVEL_SOURCE_RTL_CU8 = 2,
    DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8 = 3,
    DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16 = 4,
    DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32 = 5,
} dsd_input_level_source;

typedef struct dsd_input_level_snapshot {
    dsd_input_level_status status;
    dsd_input_level_source source;
    double rms_dbfs;
    double peak_dbfs;
    double clip_pct;
    uint64_t sample_count;
    time_t updated;
} dsd_input_level_snapshot;

/**
 * @brief Exact integer moments for unsigned 8-bit interleaved I/Q bytes.
 *
 * CU8 level snapshots intentionally treat every I and Q byte as one sample.
 * Keeping the raw moments in integers lets capture paths collect them while
 * widening or copying data and defer floating-point normalization until a
 * complete source block is ready to publish.
 */
typedef struct dsd_input_level_cu8_moments {
    uint64_t count;
    uint64_t sum;
    uint64_t sum_sq;
    uint64_t clipped;
    uint8_t min_sample;
    uint8_t max_sample;
} dsd_input_level_cu8_moments;

typedef enum DSD_ATTR_PACKED dsd_input_level_notify_mask {
    DSD_INPUT_LEVEL_NOTIFY_LOW = 1U << 0U,
    DSD_INPUT_LEVEL_NOTIFY_HOT = 1U << 1U,
    DSD_INPUT_LEVEL_NOTIFY_CLIPPING = 1U << 2U,
    DSD_INPUT_LEVEL_NOTIFY_ALL =
        DSD_INPUT_LEVEL_NOTIFY_LOW | DSD_INPUT_LEVEL_NOTIFY_HOT | DSD_INPUT_LEVEL_NOTIFY_CLIPPING,
    DSD_INPUT_LEVEL_NOTIFY_RF = DSD_INPUT_LEVEL_NOTIFY_HOT | DSD_INPUT_LEVEL_NOTIFY_CLIPPING,
} dsd_input_level_notify_mask;

const char* dsd_input_level_status_label(dsd_input_level_status status);
const char* dsd_input_level_display_label(dsd_input_level_source source);
int dsd_input_level_source_is_rf(dsd_input_level_source source);

void dsd_input_level_classify(dsd_input_level_snapshot* snapshot, double low_warn_db);

int dsd_input_level_metrics_from_pcm_i16(const int16_t* samples, size_t count, size_t step,
                                         dsd_input_level_source source, dsd_input_level_snapshot* out);
int dsd_input_level_metrics_from_pcm_f32_i16_scale(const float* samples, size_t count, size_t step,
                                                   dsd_input_level_source source, dsd_input_level_snapshot* out);
void dsd_input_level_cu8_moments_reset(dsd_input_level_cu8_moments* moments);
int dsd_input_level_cu8_moments_merge(dsd_input_level_cu8_moments* moments, const dsd_input_level_cu8_moments* add);
int dsd_input_level_cu8_moments_accumulate(dsd_input_level_cu8_moments* moments, const uint8_t* samples, size_t count);
int dsd_input_level_cu8_moments_finalize(const dsd_input_level_cu8_moments* moments, dsd_input_level_source source,
                                         dsd_input_level_snapshot* out);
int dsd_input_level_metrics_from_cu8_moments(const dsd_input_level_cu8_moments* moments, dsd_input_level_source source,
                                             dsd_input_level_snapshot* out);
int dsd_input_level_metrics_from_cu8(const uint8_t* samples, size_t count, dsd_input_level_source source,
                                     dsd_input_level_snapshot* out);
int dsd_input_level_metrics_from_cs16(const int16_t* samples, size_t count, dsd_input_level_source source,
                                      dsd_input_level_snapshot* out);
int dsd_input_level_metrics_from_cf32(const float* samples, size_t count, dsd_input_level_source source,
                                      dsd_input_level_snapshot* out);
int dsd_input_level_format_advisory(const dsd_input_level_snapshot* snapshot, char* out, size_t out_size);
void dsd_input_level_publish(dsd_opts* opts, dsd_state* state, const dsd_input_level_snapshot* snapshot,
                             unsigned int notify_mask);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_INPUT_LEVEL_H_ */
