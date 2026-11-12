// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Generic sync calibration module implementation.
 *
 * Provides protocol-agnostic symbol history and warm-start threshold
 * initialization from outer-only sync patterns.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/platform/fp_opaque.h>
#include <dsd-neo/runtime/config.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
float_compare_asc(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    if (fa < fb) {
        return -1;
    }
    if (fa > fb) {
        return 1;
    }
    return 0;
}

int
dsd_sync_warm_start_enabled(void) {
    const dsdneoRuntimeConfig* config = dsd_neo_get_config();
    if (config == NULL) {
        dsd_neo_config_init();
        config = dsd_neo_get_config();
    }
    return (config && config->sync_warmstart_enable) ? 1 : 0;
}

static float*
dsd_sync_prepare_sorted_syms(const dsd_state* state, int sync_len, float* stack_syms, int stack_count, int* free_syms) {
    float* syms = stack_syms;
    *free_syms = 0;

    if (sync_len > stack_count) {
        syms = (float*)malloc(sizeof(float) * (size_t)sync_len);
        if (syms == NULL) {
            return NULL;
        }
        *free_syms = 1;
    }

    for (int i = 0; i < sync_len; i++) {
        syms[i] = dsd_symbol_history_get_back(state, i);
    }

    qsort(syms, (size_t)sync_len, sizeof(float), float_compare_asc);
    return syms;
}

static int
dsd_sync_find_split_by_largest_gap(const float* syms, int sync_len) {
    int split = -1;
    float max_gap = -1.0f;
    for (int i = 0; i < (sync_len - 1); i++) {
        float gap = syms[i + 1] - syms[i];
        if (gap > max_gap) {
            max_gap = gap;
            split = i;
        }
    }
    return split;
}

static int
dsd_sync_compute_cluster_means(const float* syms, int sync_len, int split, float* mean_low, float* mean_high) {
    int n_low = split + 1;
    int n_high = sync_len - n_low;
    if (n_low < 2 || n_high < 2) {
        return -1;
    }

    float sum_low = 0.0f;
    for (int i = 0; i < n_low; i++) {
        sum_low += syms[i];
    }

    float sum_high = 0.0f;
    for (int i = n_low; i < sync_len; i++) {
        sum_high += syms[i];
    }

    *mean_low = sum_low / (float)n_low;
    *mean_high = sum_high / (float)n_high;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Symbol History Management
 *
 * Decoder-state initialization, reset, and cleanup own the buffer lifecycle.
 * ───────────────────────────────────────────────────────────────────────────── */

void
dsd_symbol_history_push(dsd_state* state, float symbol) {
    if (state == NULL || state->symbol_history == NULL) {
        return;
    }

    state->symbol_history[state->symbol_history_head] = symbol;
    state->symbol_history_head = (state->symbol_history_head + 1) % state->symbol_history_size;
    if (state->symbol_history_count < state->symbol_history_size) {
        state->symbol_history_count++;
    }
}

float
dsd_symbol_history_get_back(const dsd_state* state, int back) {
    if (state == NULL || state->symbol_history == NULL) {
        return 0.0f;
    }

    if (back < 0 || back >= state->symbol_history_count) {
        return 0.0f;
    }

    /* back=0 is most recent, which is at head-1 */
    int idx = state->symbol_history_head - 1 - back;
    while (idx < 0) {
        idx += state->symbol_history_size;
    }
    idx = idx % state->symbol_history_size;

    return state->symbol_history[idx];
}

int
dsd_symbol_history_count(const dsd_state* state) {
    if (state == NULL) {
        return 0;
    }
    return state->symbol_history_count;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Threshold Warm-Start
 * ───────────────────────────────────────────────────────────────────────────── */

/* The slicer thresholds from the sync word's two outer levels, and the rolling buffers prefilled with them. */
static void
dsd_sync_apply_warm_start_levels(const dsd_opts* opts, dsd_state* state, float mean_neg, float mean_pos) {
    /* Initialize thresholds */
    state->max = mean_pos;
    state->min = mean_neg;
    state->center = (state->max + state->min) / 2.0f;

    /* Warm-start mid-thresholds use a slight outer bias for noisy-sync robustness. */
    state->umid = state->center + (state->max - state->center) * DSD_WARM_START_MID_FRACTION;
    state->lmid = state->center + (state->min - state->center) * DSD_WARM_START_MID_FRACTION;

    /* Reference values (80% of extremes) */
    state->maxref = state->max * 0.80f;
    state->minref = state->min * 0.80f;

    /* The pending zero crossing was measured against the thresholds just
       replaced, so it no longer describes anything (#404). It would otherwise
       outlive the frame this sync opens -- symbol_adjust_timing_index() does not
       run while a frame is being read and nothing else clears it -- and slip the
       symbol grid once the next sync search begins, on where a crossing fell
       against a calibration and a symbol that are both long gone. */
    state->jitter = -1;

    /* Pre-fill rolling buffers to skip warmup period */
    if (opts != NULL) {
        int fill_count = opts->msize;
        if (fill_count > 1024) {
            fill_count = 1024;
        }
        for (int i = 0; i < fill_count; i++) {
            state->maxbuf[i] = state->max;
            state->minbuf[i] = state->min;
        }
        dsd_state_invalidate_minmax_sums(state);
    }
}

dsd_warm_start_result_t
dsd_sync_warm_start_thresholds_outer_only(const dsd_opts* opts, dsd_state* state, int sync_len) {
    if (!dsd_sync_warm_start_enabled()) {
        return DSD_WARM_START_DISABLED;
    }

    /* Validate state */
    if (state == NULL) {
        return DSD_WARM_START_NULL_STATE;
    }

    /* Check history availability */
    if (state->symbol_history == NULL || state->symbol_history_count < sync_len) {
        return DSD_WARM_START_NO_HISTORY;
    }

    /* Extract sync symbols from history and compute means */
    float sum_pos = 0.0f, sum_neg = 0.0f;
    int count_pos = 0, count_neg = 0;

    for (int i = 0; i < sync_len; i++) {
        /* back=0 is newest symbol (last symbol of sync)
         * We want oldest first, so back = sync_len - 1 - i for sequential order,
         * but for computing means the order doesn't matter. */
        float symbol = dsd_symbol_history_get_back(state, i);

        if (symbol > 0.0f) {
            sum_pos += symbol;
            count_pos++;
        } else {
            sum_neg += symbol;
            count_neg++;
        }
    }

    /* Require both positive and negative symbols for outer-only pattern */
    if (count_pos == 0 || count_neg == 0) {
        return DSD_WARM_START_DEGENERATE;
    }

    /* Calculate actual symbol levels from sync pattern */
    float mean_pos = sum_pos / (float)count_pos;
    float mean_neg = sum_neg / (float)count_neg;

    /* Check for degenerate span (signal too weak or invalid) */
    float span = mean_pos - mean_neg;
    if (fabsf(span) < DSD_WARM_START_MIN_SPAN) {
        return DSD_WARM_START_DEGENERATE;
    }

    dsd_sync_apply_warm_start_levels(opts, state, mean_neg, mean_pos);
    return DSD_WARM_START_OK;
}

/* Whether a value the fit computed is finite, in a form the release builds' fast-math cannot fold away: through a
   volatile round trip (fp_opaque.h), then a test of the exponent bits as an integer. Once per sync. */
static int
dsd_sync_fit_value_is_finite(double value) {
    const double opaque = dsd_fp_opaque_d(value);
    uint64_t bits = 0;
    DSD_MEMCPY(&bits, &opaque, sizeof(bits));
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

/* The outer levels of the last @p sync_len symbols, fitted by least squares to the word's own levels (symbol = centre +
   gain * level). Returns -1 for a character outside '0'..'3', a word of a single level, which places no gain, or a fit
   that is not finite (an Inf or NaN among the symbols: a float symbol replay passes what the file holds). */
static int
dsd_sync_fit_outer_levels(const dsd_state* state, const char* sync_dibits, int sync_len, float* mean_neg,
                          float* mean_pos) {
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (int i = 0; i < sync_len; i++) {
        const char c = sync_dibits[i];
        if (c < '0' || c > '3') {
            return -1;
        }
        const double x = (double)dsd_symbol_level_from_dibit((uint8_t)(c - '0'));
        /* sync_dibits is oldest first; back = 0 is the word's last symbol. */
        const double y = (double)dsd_symbol_history_get_back(state, sync_len - 1 - i);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double n = (double)sync_len;
    const double spread = (n * sum_xx) - (sum_x * sum_x);
    if (spread < 1e-9) {
        return -1;
    }
    const double gain = ((n * sum_xy) - (sum_x * sum_y)) / spread;
    const double centre = (sum_y - (gain * sum_x)) / n;
    if (!dsd_sync_fit_value_is_finite(gain) || !dsd_sync_fit_value_is_finite(centre)) {
        return -1;
    }
    *mean_pos = (float)(centre + (3.0 * gain));
    *mean_neg = (float)(centre - (3.0 * gain));
    return 0;
}

dsd_warm_start_result_t
dsd_sync_warm_start_thresholds_from_dibits(const dsd_opts* opts, dsd_state* state, const char* sync_dibits,
                                           int sync_len) {
    if (!dsd_sync_warm_start_enabled()) {
        return DSD_WARM_START_DISABLED;
    }
    if (state == NULL) {
        return DSD_WARM_START_NULL_STATE;
    }
    if (sync_dibits == NULL || sync_len < 2) {
        return DSD_WARM_START_DEGENERATE;
    }
    if (state->symbol_history == NULL || state->symbol_history_count < sync_len) {
        return DSD_WARM_START_NO_HISTORY;
    }

    float mean_neg = 0.0f;
    float mean_pos = 0.0f;
    if (dsd_sync_fit_outer_levels(state, sync_dibits, sync_len, &mean_neg, &mean_pos) != 0) {
        return DSD_WARM_START_DEGENERATE;
    }
    /* The word gives the polarity, so outer levels the wrong way round are no fit either. */
    if (!(mean_pos - mean_neg >= DSD_WARM_START_MIN_SPAN)) {
        return DSD_WARM_START_DEGENERATE;
    }

    dsd_sync_apply_warm_start_levels(opts, state, mean_neg, mean_pos);
    return DSD_WARM_START_OK;
}

dsd_warm_start_result_t
dsd_sync_warm_start_center_outer_only(dsd_state* state, int sync_len) {
    if (!dsd_sync_warm_start_enabled()) {
        return DSD_WARM_START_DISABLED;
    }

    if (state == NULL) {
        return DSD_WARM_START_NULL_STATE;
    }

    if (sync_len <= 1) {
        return DSD_WARM_START_DEGENERATE;
    }

    if (state->symbol_history == NULL || state->symbol_history_count < sync_len) {
        return DSD_WARM_START_NO_HISTORY;
    }

    float stack_syms[64];
    int free_syms = 0;
    int stack_count = (int)(sizeof(stack_syms) / sizeof(stack_syms[0]));
    float* syms = dsd_sync_prepare_sorted_syms(state, sync_len, stack_syms, stack_count, &free_syms);
    if (syms == NULL) {
        return DSD_WARM_START_DEGENERATE;
    }

    /* Split the bimodal distribution using the largest inter-sample gap. */
    int split = dsd_sync_find_split_by_largest_gap(syms, sync_len);
    if (split < 0 || split >= (sync_len - 1)) {
        if (free_syms) {
            free(syms);
        }
        return DSD_WARM_START_DEGENERATE;
    }

    float mean_low = 0.0f;
    float mean_high = 0.0f;
    if (dsd_sync_compute_cluster_means(syms, sync_len, split, &mean_low, &mean_high) != 0) {
        if (free_syms) {
            free(syms);
        }
        return DSD_WARM_START_DEGENERATE;
    }

    float span = mean_high - mean_low;
    if (fabsf(span) < DSD_WARM_START_MIN_SPAN) {
        if (free_syms) {
            free(syms);
        }
        return DSD_WARM_START_DEGENERATE;
    }

    state->center = (mean_low + mean_high) / 2.0f;

    if (free_syms) {
        free(syms);
    }

    return DSD_WARM_START_OK;
}
