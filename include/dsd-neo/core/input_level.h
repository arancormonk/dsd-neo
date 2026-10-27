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

#include <stddef.h>
#include <stdint.h>

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
    DSD_INPUT_LEVEL_SOURCE_AIRSPY_CF32 = 6,
} dsd_input_level_source;

typedef struct dsd_input_level_snapshot {
    dsd_input_level_status status;
    dsd_input_level_source source;
    double rms_dbfs;
    double peak_dbfs;
    double clip_pct;
    uint64_t sample_count;
    /* When the level was measured, on the real-time monotonic clock (dsd_realtime_mono_ms()): what says whether it is
       still the input's (dsd_input_level_is_stale()). The warning cooldown runs on decode time instead. */
    uint64_t updated_ms;
} dsd_input_level_snapshot;

/** @brief Real time after its measurement that a level stops describing the input (dsd_input_level_is_stale()). */
#define DSD_INPUT_LEVEL_STALE_MS 2000U

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
    DSD_INPUT_LEVEL_NOTIFY_ALL = DSD_INPUT_LEVEL_NOTIFY_LOW | DSD_INPUT_LEVEL_NOTIFY_HOT
        | DSD_INPUT_LEVEL_NOTIFY_CLIPPING,
    DSD_INPUT_LEVEL_NOTIFY_RF = DSD_INPUT_LEVEL_NOTIFY_HOT | DSD_INPUT_LEVEL_NOTIFY_CLIPPING,
} dsd_input_level_notify_mask;

const char* dsd_input_level_status_label(dsd_input_level_status status);
const char* dsd_input_level_display_label(dsd_input_level_source source);
int dsd_input_level_source_is_rf(dsd_input_level_source source);

void dsd_input_level_classify(dsd_input_level_snapshot* snapshot, double low_warn_db);

/**
 * @brief Whether @p snapshot no longer describes its input at real-time monotonic @p now_ms.
 *
 * Stale when there is no level (NULL, no samples) or it was measured more than DSD_INPUT_LEVEL_STALE_MS before
 * @p now_ms: a receiver that stopped delivering samples (a stalled USB dongle, an rtl_tcp server gone quiet) keeps its
 * last level, and nothing but its age tells it from one that is quiet. A stamp after @p now_ms (a reader whose clock
 * read came before the producer's) is fresh. Frontends show a stale or missing level as "No input"; warnings
 * require a fresh live measurement (dsd_input_level_publish()).
 */
int dsd_input_level_is_stale(const dsd_input_level_snapshot* snapshot, uint64_t now_ms);

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
/**
 * @brief Classify @p snapshot into @p state, and print and toast an advisory when @p notify_mask allows its status.
 *
 * A repeat of a warning waits out `opts->input_warn_cooldown_sec` of decode time, measured from the decode time of the
 * last one (`state->input_level_last_toast_time`), unless the status worsens. The decoder prints it, so a replay
 * prints it at the same capture time in every run. The toast stays up for real time (`state->ui_msg_expire`).
 *
 * A stale live RF level is replaced by a fresh producer measurement when available. Without one it is kept
 * but warns about nothing: it is the last level of a receiver that stopped. An I/Q replay (`opts->iq_replay_active`)
 * warns as its capture says whatever the real-time age, so a replay that pauses still prints the same warnings in
 * every run.
 */
void dsd_input_level_publish(dsd_opts* opts, dsd_state* state, const dsd_input_level_snapshot* snapshot,
                             unsigned int notify_mask);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_INPUT_LEVEL_H_ */
