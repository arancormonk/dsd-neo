// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/dsp/sync_hamming.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/telemetry.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/timing.h"
#include "frame_sync_internal.h"
#include "frame_sync_level.h"
#ifdef DSD_NEO_TEST_HOOKS
#include "frame_sync_test_support.h"
#endif

#ifdef USE_RADIO
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif

enum {
    FRAME_SYNC_HISTORY_CAPACITY = 48,
};

static int
frame_sync_opts_has_4800_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1
            || opts->frame_m17 == 1);
}

static int frame_sync_current_demod_rate(const dsd_opts* opts, const dsd_state* state);

#ifdef USE_RADIO
static int
dmr_best_sync_hamming(const char* window, const char** out_name) {
    static const char* names[] = {"bs_data",     "bs_voice",    "ms_data",      "ms_voice",
                                  "dm_ts1_data", "dm_ts2_data", "dm_ts1_voice", "dm_ts2_voice"};
    static const char* patterns[] = {DMR_BS_DATA_SYNC,
                                     DMR_BS_VOICE_SYNC,
                                     DMR_MS_DATA_SYNC,
                                     DMR_MS_VOICE_SYNC,
                                     DMR_DIRECT_MODE_TS1_DATA_SYNC,
                                     DMR_DIRECT_MODE_TS2_DATA_SYNC,
                                     DMR_DIRECT_MODE_TS1_VOICE_SYNC,
                                     DMR_DIRECT_MODE_TS2_VOICE_SYNC};
    int best = 24;
    int best_idx = 0;

    if (!window) {
        if (out_name) {
            *out_name = "none";
        }
        return best;
    }

    for (int p = 0; p < 8; p++) {
        int ham = 0;
        for (int k = 0; k < 24; k++) {
            if (window[k] != patterns[p][k]) {
                ham++;
            }
        }
        if (ham < best) {
            best = ham;
            best_idx = p;
        }
    }
    if (out_name) {
        *out_name = names[best_idx];
    }
    return best;
}

static int
rtl_profile_for_sps_profile(const dsd_opts* opts, const dsd_state* state, const frame_sync_sps_profile* profile) {
    if (!profile) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE;
    }
    /* Shared with the operator-facing modulation control, which reaches the same
     * front end by a different route: two copies of this drift, and then the
     * filter changes under the user every time the hunt re-runs. */
    return dsd_rtl_channel_profile_for(opts, profile->symbol_rate_hz, profile->levels, state ? state->rf_mod : 0);
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_frame_sync_test_rtl_profile_for_sps_index(const dsd_opts* opts, const dsd_state* state, int profile_index) {
    return rtl_profile_for_sps_profile(opts, state, frame_sync_sps_profile_for_index(profile_index));
}
#endif

static void
rtl_maybe_apply_demod_profile(const dsd_opts* opts, const dsd_state* state, const frame_sync_sps_profile* profile) {
    if (!opts || !state || !profile || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    const int ted_sps =
        dsd_opts_compute_sps_rate(opts, profile->symbol_rate_hz, frame_sync_current_demod_rate(opts, state));
    (void)dsd_rtl_stream_metrics_hook_apply_demod_profile(state->rf_mod == 1, profile->symbol_rate_hz, profile->levels,
                                                          rtl_profile_for_sps_profile(opts, state, profile), ted_sps);
}

static void
rtl_maybe_apply_active_demod_profile(const dsd_opts* opts, const dsd_state* state) {
    rtl_maybe_apply_demod_profile(opts, state, frame_sync_sps_profile_for_index(state->sps_hunt_idx));
}
#endif

static int
frame_sync_current_demod_rate(const dsd_opts* opts, const dsd_state* state) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts && state && opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        int rtl_demod_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (rtl_demod_rate > 0) {
            demod_rate = rtl_demod_rate;
        }
    }
#else
    UNUSED(state);
#endif
    return demod_rate;
}

static inline void
dmr_set_symbol_timing(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    int demod_rate = frame_sync_current_demod_rate(opts, state);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
#ifdef USE_RADIO
    rtl_maybe_apply_active_demod_profile(opts, state);
#endif
}

/* Modulation auto-detect state (file scope for reset access).
 * Vote counters and Hamming distance tracking for C4FM/QPSK/GFSK switching.
 * These are atomic because engine retune requests reset them from the tuning
 * thread while getFrameSync() reads/writes them on the DSP thread. */
static atomic_int g_vote_qpsk = 0;
static atomic_int g_vote_c4fm = 0;
static atomic_int g_vote_gfsk = 0;
static atomic_int g_ham_c4fm_recent = 24;
static atomic_int g_ham_qpsk_recent = 24;
static atomic_int g_ham_gfsk_recent = 24;
static atomic_int g_qpsk_dwell_enter_ms = 0;
static dsd_atomic_u64 g_frame_sync_ui_last_publish_ms = {0};

enum { DSD_FRAME_SYNC_UI_PUBLISH_INTERVAL_MS = 50 };

static void
frame_sync_publish_ui_throttled(const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_telemetry_is_active()) {
        return;
    }

    const uint64_t now_ms = dsd_time_monotonic_ms();
    const uint64_t last_ms = dsd_atomic_u64_load_relaxed(&g_frame_sync_ui_last_publish_ms);
    if (last_ms != 0 && (now_ms - last_ms) < DSD_FRAME_SYNC_UI_PUBLISH_INTERVAL_MS) {
        return;
    }

    dsd_atomic_u64_store_relaxed(&g_frame_sync_ui_last_publish_ms, now_ms);
    dsd_telemetry_publish_both_and_redraw(opts, state);
}

static void
p25p2_note_sync_activity(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    const int voice_tuned = (opts && opts->trunk_enable == 1 && (opts->trunk_is_tuned == 1)) ? 1 : 0;

    /*
     * Exact P25P2 sync means the channel is present, but while following a VC it
     * does not necessarily mean voice is still active. TDMA VCs can keep sending
     * LCCH/idle after a call ends; refreshing last_vc_sync_time here holds the
     * trunk release path open and delays return to the CC. Voice/MAC handlers
     * update last_vc_sync_time when the call is actually active.
    */
    if (voice_tuned) {
        dsd_frame_sync_hook_p25_sm_vc_sync(opts, state);
        return;
    }

    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
}

void
dsd_frame_sync_reset_mod_state(void) {
    atomic_store(&g_vote_qpsk, 0);
    atomic_store(&g_vote_c4fm, 0);
    atomic_store(&g_vote_gfsk, 0);
    atomic_store(&g_ham_c4fm_recent, 24);
    atomic_store(&g_ham_qpsk_recent, 24);
    atomic_store(&g_ham_gfsk_recent, 24);
    atomic_store(&g_qpsk_dwell_enter_ms, 0);
}

/*
 * P25 CQPSK handling follows OP25's frame-sync dibit maps. OP25 detects raw
 * sync variants and applies the selected map at slicer output; dsd-neo also
 * keeps a separate center estimate for RTL symbol streams, so rotated sync
 * acceptance must calibrate that center from the raw symbol window.
 */

void
printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
               const char* modulation) {
    UNUSED3(state, offset, modulation);

    char timestr[9];
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    if (opts->verbose > 0) {
        DSD_FPRINTF(stderr, "%s ", timestr);
        DSD_FPRINTF(stderr, "Sync: %s ", frametype);
    }

    //oops, that made a nested if-if-if-if statement,
    //causing a memory leak

    // if (opts->verbose > 2)
    //DSD_FPRINTF(stderr,"o: %4i ", offset);
    // if (opts->verbose > 1)
    //DSD_FPRINTF(stderr,"mod: %s ", modulation);
    // if (opts->verbose > 2)
    //DSD_FPRINTF(stderr,"g: %f ", state->aout_gain);

    /* stack buffer; no free */
}

enum {
    FRAME_SYNC_WINDOW_8 = 1u << 0,
    FRAME_SYNC_WINDOW_10 = 1u << 1,
    FRAME_SYNC_WINDOW_12 = 1u << 2,
    FRAME_SYNC_WINDOW_16 = 1u << 3,
    FRAME_SYNC_WINDOW_20 = 1u << 4,
    FRAME_SYNC_WINDOW_24 = 1u << 5,
    FRAME_SYNC_WINDOW_32 = 1u << 6,
    FRAME_SYNC_WINDOW_48 = 1u << 7,
};

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    time_t now;
    double nowm;
    int synctest_pos;
    float lmax;
    float lmin;
    unsigned int ready_windows;
    char* modulation;
    char* synctest;
    char* synctest8;
    char* synctest10;
    char* synctest12;
    char* synctest16;
    char* synctest20;
    char* synctest32;
    char* synctest48;
} frame_sync_match_ctx;

static unsigned int
frame_sync_window_flag(int length) {
    switch (length) {
        case 8: return FRAME_SYNC_WINDOW_8;
        case 10: return FRAME_SYNC_WINDOW_10;
        case 12: return FRAME_SYNC_WINDOW_12;
        case 16: return FRAME_SYNC_WINDOW_16;
        case 20: return FRAME_SYNC_WINDOW_20;
        case 24: return FRAME_SYNC_WINDOW_24;
        case 32: return FRAME_SYNC_WINDOW_32;
        case 48: return FRAME_SYNC_WINDOW_48;
        default: return 0;
    }
}

static int
frame_sync_match_window_ready(const frame_sync_match_ctx* ctx, int length) {
    const unsigned int flag = frame_sync_window_flag(length);
    return ctx && flag != 0 && (ctx->ready_windows & flag) != 0;
}

static int
frame_sync_match_profile_active(const frame_sync_match_ctx* ctx, int profile_index) {
    if (!ctx || !ctx->opts || !ctx->state) {
        return 0;
    }
    /* Stored symbols cannot be revisited after an SPS hunt profile advances. */
    if (ctx->opts->audio_in_type == AUDIO_IN_SYMBOL_BIN || ctx->opts->audio_in_type == AUDIO_IN_SYMBOL_FLT) {
        return 1;
    }
    return ctx->state->sps_hunt_idx == profile_index;
}

static int
frame_sync_p25p2_profile_active(const frame_sync_match_ctx* ctx) {
    if (frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4)) {
        return 1;
    }
    if (!ctx || !ctx->opts || !ctx->state) {
        return 0;
    }
    /* -m3 deliberately retains 10-SPS C4FM timing, which otherwise maps to profile 0. */
    return ctx->opts->mod_p25p2_c4fm == 1 && ctx->opts->mod_cli_lock && ctx->opts->mod_c4fm == 1
           && ctx->opts->mod_qpsk == 0 && ctx->opts->mod_gfsk == 0 && ctx->state->rf_mod == 0;
}

static int frame_sync_cqpsk_4level_enabled(const dsd_opts* opts, const dsd_state* state);
static int frame_sync_profile_uses_gfsk_exclusively(const dsd_opts* opts, int profile_index);

static inline void
frame_sync_set_basic_lock(frame_sync_match_ctx* ctx) {
    dsd_state* state = ctx->state;
    state->carrier = 1;
    state->offset = ctx->synctest_pos;
    state->max = ((state->max) + ctx->lmax) / 2;
    state->min = ((state->min) + ctx->lmin) / 2;
}

static inline void
frame_sync_note_cc_sync(frame_sync_match_ctx* ctx) {
    ctx->state->last_cc_sync_time = ctx->now;
    ctx->state->last_cc_sync_time_m = ctx->nowm;
}

static void
frame_sync_set_p25_cqpsk_dibit_map(frame_sync_match_ctx* ctx, uint8_t map_idx) {
    if (!ctx || !ctx->state) {
        return;
    }
    ctx->state->p25_cqpsk_dibit_map_idx = dsd_p25_cqpsk_dibit_map_index(map_idx);
}

typedef enum {
    FRAME_SYNC_P25_CENTER_AUTO = 0,
    FRAME_SYNC_P25_CENTER_FORCE = 1,
    FRAME_SYNC_P25_CENTER_SKIP = 2,
} frame_sync_p25_center_mode_t;

#ifdef USE_RADIO
typedef struct {
    float center;
    float gain;
} frame_sync_p25_cqpsk_raw_fit_t;

static int
frame_sync_cqpsk_raw_level_unit(uint8_t raw_dibit) {
    static const int units[4] = {1, 3, -1, -3};
    return units[raw_dibit & 0x3u];
}

static dsd_warm_start_result_t
frame_sync_accumulate_p25_cqpsk_raw_sync(const dsd_state* state, const char* raw_window, int sync_len, float sum[4],
                                         int count[4]) {
    for (int i = 0; i < sync_len; i++) {
        char c = raw_window[i];
        if (c < '0' || c > '3') {
            return DSD_WARM_START_DEGENERATE;
        }
        int raw_dibit = c - '0';
        int back = sync_len - 1 - i;
        sum[raw_dibit] += dsd_symbol_history_get_back(state, back);
        count[raw_dibit]++;
    }
    return DSD_WARM_START_OK;
}

static dsd_warm_start_result_t
frame_sync_fit_p25_cqpsk_raw_levels(const float sum[4], const int count[4], frame_sync_p25_cqpsk_raw_fit_t* out_fit) {
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_xx = 0.0f;
    float sum_xy = 0.0f;
    int n = 0;
    for (uint8_t raw = 0; raw < 4u; raw++) {
        if (count[raw] == 0) {
            continue;
        }
        float x = (float)frame_sync_cqpsk_raw_level_unit(raw);
        float y = sum[raw] / (float)count[raw];
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        n++;
    }
    if (n < 2) {
        return DSD_WARM_START_DEGENERATE;
    }

    float denom = ((float)n * sum_xx) - (sum_x * sum_x);
    if (fabsf(denom) < 1.0e-6f) {
        return DSD_WARM_START_DEGENERATE;
    }
    float gain = (((float)n * sum_xy) - (sum_x * sum_y)) / denom;
    if (fabsf(gain) * 2.0f < DSD_WARM_START_MIN_SPAN) {
        return DSD_WARM_START_DEGENERATE;
    }
    if (!out_fit) {
        return DSD_WARM_START_NULL_STATE;
    }
    out_fit->center = (sum_y - (gain * sum_x)) / (float)n;
    out_fit->gain = gain;
    return DSD_WARM_START_OK;
}

static dsd_warm_start_result_t
frame_sync_fit_p25_cqpsk_raw_sync(const frame_sync_match_ctx* ctx, const char* raw_window, int sync_len,
                                  frame_sync_p25_cqpsk_raw_fit_t* out_fit) {
    if (!dsd_sync_warm_start_enabled()) {
        return DSD_WARM_START_DISABLED;
    }
    if (!ctx || !ctx->state || !raw_window) {
        return DSD_WARM_START_NULL_STATE;
    }
    const dsd_state* state = ctx->state;
    if (sync_len <= 1 || strlen(raw_window) < (size_t)sync_len) {
        return DSD_WARM_START_DEGENERATE;
    }
    if (state->symbol_history == NULL || dsd_symbol_history_count(state) < sync_len) {
        return DSD_WARM_START_NO_HISTORY;
    }

    float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int count[4] = {0, 0, 0, 0};
    dsd_warm_start_result_t result = frame_sync_accumulate_p25_cqpsk_raw_sync(state, raw_window, sync_len, sum, count);
    if (result != DSD_WARM_START_OK) {
        return result;
    }
    return frame_sync_fit_p25_cqpsk_raw_levels(sum, count, out_fit);
}

static void
frame_sync_seed_p25_cqpsk_level_windows(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    int minmax_count = dsd_state_minmax_window_size(opts->msize);
    for (int i = 0; i < minmax_count; i++) {
        state->minbuf[i] = state->min;
        state->maxbuf[i] = state->max;
    }
    dsd_state_invalidate_minmax_sums(state);

    int symbol_count = opts->ssize;
    if (symbol_count < 0) {
        symbol_count = 0;
    }
    if (symbol_count > (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]))) {
        symbol_count = (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]));
    }
    for (int i = 0; i < symbol_count; i++) {
        state->sbuf[i] = (i & 1) ? state->max : state->min;
    }
}

static void
frame_sync_apply_p25_cqpsk_raw_fit(frame_sync_match_ctx* ctx, const frame_sync_p25_cqpsk_raw_fit_t* fit) {
    if (!ctx || !ctx->state || !fit) {
        return;
    }

    dsd_state* state = ctx->state;
    float half_span = fabsf(fit->gain) * 3.0f;
    state->center = fit->center;
    state->min = fit->center - half_span;
    state->max = fit->center + half_span;
    state->umid = state->center + (state->max - state->center) * DSD_WARM_START_MID_FRACTION;
    state->lmid = state->center + (state->min - state->center) * DSD_WARM_START_MID_FRACTION;
    state->maxref = state->max * 0.80f;
    state->minref = state->min * 0.80f;
    frame_sync_seed_p25_cqpsk_level_windows(ctx->opts, state);
}

static int
frame_sync_p25_cqpsk_map_requires_center_fit(uint8_t map_idx) {
    map_idx = dsd_p25_cqpsk_dibit_map_index(map_idx);
    return map_idx == DSD_P25_CQPSK_DIBIT_MAP_N1200 || map_idx == DSD_P25_CQPSK_DIBIT_MAP_P1200;
}

static int
frame_sync_cqpsk_window_matches_map(const char* raw_window, const char* expected, uint8_t map_idx) {
    if (!raw_window || !expected) {
        return 0;
    }

    size_t expected_len = strlen(expected);
    for (size_t i = 0; i < expected_len; i++) {
        if (raw_window[i] < '0' || raw_window[i] > '3') {
            return 0;
        }
        uint8_t raw_dibit = (uint8_t)(raw_window[i] - '0');
        uint8_t corrected = dsd_p25_cqpsk_correct_dibit(map_idx, raw_dibit);
        if ((char)('0' + corrected) != expected[i]) {
            return 0;
        }
    }
    return raw_window[expected_len] == '\0';
}

static int
frame_sync_find_rotated_p25_cqpsk_map(const char* raw_window, const char* expected, uint8_t* out_map_idx) {
    static const uint8_t rotation_maps[] = {
        DSD_P25_CQPSK_DIBIT_MAP_X2400,
        DSD_P25_CQPSK_DIBIT_MAP_N1200,
        DSD_P25_CQPSK_DIBIT_MAP_P1200,
    };
    for (size_t i = 0; i < sizeof(rotation_maps) / sizeof(rotation_maps[0]); i++) {
        if (frame_sync_cqpsk_window_matches_map(raw_window, expected, rotation_maps[i])) {
            if (out_map_idx) {
                *out_map_idx = rotation_maps[i];
            }
            return 1;
        }
    }
    return 0;
}
#endif

static inline void
frame_sync_maybe_force_dmr_gfsk(const dsd_opts* opts, dsd_state* state) {
    if (!opts->mod_cli_lock || opts->mod_gfsk) {
        state->rf_mod = 2;
    }
}

static void
frame_sync_accept_p25p1(frame_sync_match_ctx* ctx, int synctype, const char* label,
                        frame_sync_p25_center_mode_t center_mode) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
#ifdef USE_RADIO
    /* Hold the CQPSK chain the way P25p2 does, without claiming it: a P25p1 sync word is the
     * same on both modulations, so it can only re-assert a decision already made, never make
     * one. Re-pushing on every sync corrects front-end drift left by an engine retune. */
    if (!opts->mod_cli_lock && state->rf_mod == 1) {
        rtl_maybe_apply_active_demod_profile(opts, state);
    }
#endif
    frame_sync_set_basic_lock(ctx);
    state->dmrburstR = 17;
    state->payload_algidR = 0;
    state->dmr_stereo = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "P25 Phase 1");
    if (opts->errorbars == 1) {
        printFrameSync(opts, state, label, ctx->synctest_pos + 1, ctx->modulation);
    }
    state->lastsynctype = synctype;
    frame_sync_note_cc_sync(ctx);
    if (center_mode != FRAME_SYNC_P25_CENTER_SKIP
        && (center_mode == FRAME_SYNC_P25_CENTER_FORCE || state->rf_mod == 1)) {
        dsd_sync_warm_start_center_outer_only(state, 24);
    } else if (state->rf_mod == 0) {
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
    }
}

static void
frame_sync_report_p25p2_params(dsd_opts* opts, dsd_state* state) {
    if (state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0) {
        printFrameInfo(opts, state);
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " P2 Missing Parameters            ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
frame_sync_accept_p25p2(frame_sync_match_ctx* ctx, int synctype, int inverted, const char* label,
                        int set_last_before_info, frame_sync_p25_center_mode_t center_mode) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (!opts->mod_cli_lock) {
        state->rf_mod = 1;
#ifdef USE_RADIO
        rtl_maybe_apply_active_demod_profile(opts, state);
#endif
    }
    frame_sync_set_basic_lock(ctx);
    opts->inverted_p2 = inverted;
    if (set_last_before_info) {
        state->lastsynctype = synctype;
    }
    if (opts->errorbars == 1) {
        printFrameSync(opts, state, label, ctx->synctest_pos + 1, ctx->modulation);
    }
    frame_sync_report_p25p2_params(opts, state);
    if (!set_last_before_info) {
        state->lastsynctype = synctype;
    }
    p25p2_note_sync_activity(opts, state);
    if (center_mode != FRAME_SYNC_P25_CENTER_SKIP
        && (center_mode == FRAME_SYNC_P25_CENTER_FORCE || state->rf_mod == 1)) {
        dsd_sync_warm_start_center_outer_only(state, 20);
    }
}

#ifdef USE_RADIO
static int
frame_sync_try_rotated_p25(frame_sync_match_ctx* ctx, const char* symbols, const char* pattern, int pattern_len,
                           int synctype, int inverted, const char* label, int phase2, int set_last_before_info) {
    const dsd_opts* opts = ctx->opts;
    const dsd_state* state = ctx->state;
    uint8_t map_idx = DSD_P25_CQPSK_DIBIT_MAP_IDENTITY;
    if (!frame_sync_cqpsk_4level_enabled(opts, state)
        || !frame_sync_find_rotated_p25_cqpsk_map(symbols, pattern, &map_idx)) {
        return DSD_SYNC_NONE;
    }

    frame_sync_p25_cqpsk_raw_fit_t fit = {0.0f, 0.0f};
    dsd_warm_start_result_t center_result = frame_sync_fit_p25_cqpsk_raw_sync(ctx, symbols, pattern_len, &fit);
    if (frame_sync_p25_cqpsk_map_requires_center_fit(map_idx) && center_result != DSD_WARM_START_OK) {
        return DSD_SYNC_NONE;
    }

    frame_sync_set_p25_cqpsk_dibit_map(ctx, map_idx);
    if (phase2) {
        frame_sync_accept_p25p2(ctx, synctype, inverted, label, set_last_before_info, FRAME_SYNC_P25_CENTER_SKIP);
    } else {
        frame_sync_accept_p25p1(ctx, synctype, label, FRAME_SYNC_P25_CENTER_SKIP);
    }
    if (center_result == DSD_WARM_START_OK) {
        frame_sync_apply_p25_cqpsk_raw_fit(ctx, &fit);
    }
    return synctype;
}
#endif

static int
frame_sync_try_p25p1(frame_sync_match_ctx* ctx) {
    if (ctx->opts->frame_p25p1 != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4)
        || !frame_sync_match_window_ready(ctx, 24)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest, P25P1_SYNC) == 0) {
        frame_sync_set_p25_cqpsk_dibit_map(ctx, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY);
        frame_sync_accept_p25p1(ctx, DSD_SYNC_P25P1_POS, "+P25p1", FRAME_SYNC_P25_CENTER_AUTO);
        return DSD_SYNC_P25P1_POS;
    }

    if (strcmp(ctx->synctest, INV_P25P1_SYNC) == 0) {
        frame_sync_set_p25_cqpsk_dibit_map(ctx, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY);
        frame_sync_accept_p25p1(ctx, DSD_SYNC_P25P1_NEG, "-P25p1 ", FRAME_SYNC_P25_CENTER_AUTO);
        return DSD_SYNC_P25P1_NEG;
    }

#ifdef USE_RADIO
    int sync_type =
        frame_sync_try_rotated_p25(ctx, ctx->synctest, P25P1_SYNC, 24, DSD_SYNC_P25P1_POS, 0, "+P25p1", 0, 0);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_rotated_p25(ctx, ctx->synctest, INV_P25P1_SYNC, 24, DSD_SYNC_P25P1_NEG, 0, "-P25p1 ", 0, 0);
#else
    return DSD_SYNC_NONE;
#endif
}

static int
frame_sync_accept_x2tdma(frame_sync_match_ctx* ctx, int synctype, const char* label, int marks_first_frame) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    frame_sync_set_basic_lock(ctx);
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "X2-TDMA");
    if (opts->errorbars == 1) {
        printFrameSync(opts, state, label, ctx->synctest_pos + 1, ctx->modulation);
    }
    if (marks_first_frame && state->lastsynctype != synctype) {
        state->firstframe = 1;
    }
    state->lastsynctype = synctype;
    dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
    return synctype;
}

static int
frame_sync_try_x2tdma(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    if (opts->frame_x2tdma != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_6000_4)
        || !frame_sync_match_window_ready(ctx, 24)) {
        return DSD_SYNC_NONE;
    }

    if ((strcmp(ctx->synctest, X2TDMA_BS_DATA_SYNC) == 0) || (strcmp(ctx->synctest, X2TDMA_MS_DATA_SYNC) == 0)) {
        if (opts->inverted_x2tdma == 0) {
            return frame_sync_accept_x2tdma(ctx, DSD_SYNC_X2TDMA_DATA_POS, "+X2-TDMA ", 0);
        }
        return frame_sync_accept_x2tdma(ctx, DSD_SYNC_X2TDMA_VOICE_NEG, "-X2-TDMA ", 1);
    }

    if ((strcmp(ctx->synctest, X2TDMA_BS_VOICE_SYNC) == 0) || (strcmp(ctx->synctest, X2TDMA_MS_VOICE_SYNC) == 0)) {
        if (opts->inverted_x2tdma == 0) {
            return frame_sync_accept_x2tdma(ctx, DSD_SYNC_X2TDMA_VOICE_POS, "+X2-TDMA ", 1);
        }
        return frame_sync_accept_x2tdma(ctx, DSD_SYNC_X2TDMA_DATA_NEG, "-X2-TDMA ", 0);
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_ysf(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_ysf != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4)
        || !frame_sync_match_window_ready(ctx, 20) || dsd_frame_sync_suppress_p25_alt_sync(opts, state)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest20, FUSION_SYNC) == 0) {
        printFrameSync(opts, state, "+YSF ", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        opts->inverted_ysf = 0;
        state->lastsynctype = DSD_SYNC_YSF_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 20);
        return DSD_SYNC_YSF_POS;
    }

    if (strcmp(ctx->synctest20, INV_FUSION_SYNC) == 0) {
        printFrameSync(opts, state, "-YSF ", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        opts->inverted_ysf = 1;
        state->lastsynctype = DSD_SYNC_YSF_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 20);
        return DSD_SYNC_YSF_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_p25p2(frame_sync_match_ctx* ctx) {
    if (ctx->opts->frame_p25p2 != 1 || !frame_sync_p25p2_profile_active(ctx)
        || !frame_sync_match_window_ready(ctx, 20)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest20, P25P2_SYNC) == 0) {
        frame_sync_set_p25_cqpsk_dibit_map(ctx, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY);
        frame_sync_accept_p25p2(ctx, DSD_SYNC_P25P2_POS, 0, "+P25p2", 1, FRAME_SYNC_P25_CENTER_AUTO);
        return DSD_SYNC_P25P2_POS;
    }

    if (strcmp(ctx->synctest20, INV_P25P2_SYNC) == 0) {
        frame_sync_set_p25_cqpsk_dibit_map(ctx, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY);
        frame_sync_accept_p25p2(ctx, DSD_SYNC_P25P2_NEG, 1, "-P25p2", 0, FRAME_SYNC_P25_CENTER_AUTO);
        return DSD_SYNC_P25P2_NEG;
    }

#ifdef USE_RADIO
    int sync_type =
        frame_sync_try_rotated_p25(ctx, ctx->synctest20, P25P2_SYNC, 20, DSD_SYNC_P25P2_POS, 0, "+P25p2", 1, 1);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_rotated_p25(ctx, ctx->synctest20, INV_P25P2_SYNC, 20, DSD_SYNC_P25P2_NEG, 1, "-P25p2", 1, 0);
#else
    return DSD_SYNC_NONE;
#endif
}

static int
frame_sync_try_dpmr(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_dpmr != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_2400_4)
        || !frame_sync_match_window_ready(ctx, 12)) {
        return DSD_SYNC_NONE;
    }

    if (opts->inverted_dpmr == 0 && strcmp(ctx->synctest12, DPMR_FRAME_SYNC_2) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "dPMR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+dPMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DPMR_FS2_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 12);
        return DSD_SYNC_DPMR_FS2_POS;
    }

    if (opts->inverted_dpmr == 1 && strcmp(ctx->synctest12, INV_DPMR_FRAME_SYNC_2) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "dPMR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-dPMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DPMR_FS2_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 12);
        return DSD_SYNC_DPMR_FS2_NEG;
    }

    return DSD_SYNC_NONE;
}

/**
 * @brief Latch an M17 preamble as a candidate rather than accepting it as a sync.
 *
 * M17's preamble is `31313131` -- a pure alternating run with no structure to check. Any signal
 * that alternates at 4800 baud presents one: D-STAR's bit sync does, and so does noise on an
 * open squelch. Accepting it as a sync therefore said nothing about the signal, while demanding
 * more of it -- an exact marker, or two exact markers in a row -- did not discriminate either,
 * because doubling an alternating run leaves an alternating run. What that rule did do was
 * reject real M17, whose markers arrive with the odd symbol error, and M17 has no other way into
 * its sync chain: everything downstream is gated on `lastsynctype` (issue #399).
 *
 * So a preamble is treated as what it is -- a hint that a sync word may be about to arrive --
 * and the decision waits for the LSF or BERT word behind it, which is eight symbols of real
 * structure. Nothing is printed, no lock is taken and no thresholds move here; a signal that
 * never produces the sync word costs a lapsed candidate and nothing else.
 */
static void
frame_sync_note_m17_pre_candidate(frame_sync_match_ctx* ctx, int ham_pre, int ham_piv) {
    dsd_state* state = ctx->state;

    if (ham_pre > 1 && ham_piv > 1) {
        state->m17_pre_run = 0;
        return;
    }

    /* One matching window is not a preamble. A preamble is 192 symbols of alternating outer
     * rails, so every window across it matches -- at one polarity or the other, since the run
     * reads as its own inversion one symbol over -- and a real transmission has some 180 of
     * them to spare. Noise supplies a single window at this tolerance often enough to matter,
     * and supplies eight in a row essentially never. Counting the run is what separates the two
     * without asking any single window to be cleaner than real M17 delivers (#399). */
    if (state->m17_pre_run < DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS) {
        state->m17_pre_run++;
    }
    if (state->m17_pre_run < DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS) {
        return;
    }

    /* The other half of what the old accept did here, and the half worth keeping: a run of
     * symbols alternating between the outer rails is the best level reference M17 ever offers,
     * and the sync word behind it cannot be sliced without it. Carrier is deliberately not
     * raised and nothing is printed -- an alternating run is not yet a lock. */
    state->offset = ctx->synctest_pos;
    state->max = ((state->max) + ctx->lmax) / 2;
    state->min = ((state->min) + ctx->lmin) / 2;
    dsd_sync_warm_start_thresholds_outer_only(ctx->opts, state, 8);

    state->m17_pre_candidate = (ham_pre <= 1) ? 1 : 2;
    state->m17_pre_candidate_ttl = DSD_FRAME_SYNC_M17_CANDIDATE_TTL;
}

/** @brief Age a latched candidate by one evaluation, dropping it when it lapses. */
static void
frame_sync_age_m17_pre_candidate(dsd_state* state) {
    if (state->m17_pre_candidate == 0) {
        return;
    }
    if (state->m17_pre_candidate_ttl > 0) {
        state->m17_pre_candidate_ttl--;
    }
    if (state->m17_pre_candidate_ttl == 0) {
        state->m17_pre_candidate = 0;
    }
}

/**
 * @brief Whether the M17 chain in progress has produced anything that checked out.
 *
 * Frame sync reads the protocol layer's evidence rather than re-deriving any of it. A chain that
 * has proved nothing is not allowed to keep extending itself frame after frame, which is how a
 * single false sync used to occupy the profile for the rest of a capture (#399).
 *
 * A pending weak streak counts, not just full confirmation: a real stream whose LSF arrived too
 * damaged to clear its CRC rebuilds the link setup from the LICH carried by six consecutive
 * stream frames, and demanding confirmation before the second of them would stop the chain that
 * produces it. One clean LICH is enough to earn the next frame; noise does not supply even that.
 */
static int
frame_sync_m17_chain_has_evidence(const dsd_state* state) {
    return state->m17_confirmed != 0 || state->m17_confirm_weak_streak != 0;
}

static int
frame_sync_m17_eot_allowed_after(int lastsynctype) {
    return lastsynctype == DSD_SYNC_M17_LSF_POS || lastsynctype == DSD_SYNC_M17_LSF_NEG
           || lastsynctype == DSD_SYNC_M17_STR_POS || lastsynctype == DSD_SYNC_M17_STR_NEG
           || lastsynctype == DSD_SYNC_M17_PKT_POS || lastsynctype == DSD_SYNC_M17_PKT_NEG
           || lastsynctype == DSD_SYNC_M17_BRT_POS || lastsynctype == DSD_SYNC_M17_BRT_NEG;
}

static int
frame_sync_try_m17_eot(frame_sync_match_ctx* ctx, int ham_eot, int ham_eot_inv, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    const int ham = is_inverted ? ham_eot_inv : ham_eot;
    if (ham > 1 || !frame_sync_m17_eot_allowed_after(state->lastsynctype)
        || !frame_sync_m17_chain_has_evidence(state)) {
        return DSD_SYNC_NONE;
    }

    if (is_inverted) {
        printFrameSync(opts, state, "-M17 EOT", ctx->synctest_pos + 1, ctx->modulation);
        state->lastsynctype = DSD_SYNC_M17_EOT_NEG;
    } else {
        printFrameSync(opts, state, "+M17 EOT", ctx->synctest_pos + 1, ctx->modulation);
        state->lastsynctype = DSD_SYNC_M17_EOT_POS;
    }
    frame_sync_set_basic_lock(ctx);
    state->m17_polarity = 0;
    DSD_FPRINTF(stderr, "\n");
    return state->lastsynctype;
}

static int
frame_sync_try_m17_packet(frame_sync_match_ctx* ctx, int ham_pkt, int ham_brt, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;

    if (ham_pkt <= 1 && !is_inverted) {
        if (state->lastsynctype != DSD_SYNC_M17_LSF_POS
            && !(state->lastsynctype == DSD_SYNC_M17_PKT_POS && frame_sync_m17_chain_has_evidence(state))) {
            return DSD_SYNC_NONE;
        }
        printFrameSync(opts, state, "+M17 PKT", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_PKT_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_PKT_POS;
    }

    if (ham_brt <= 1 && is_inverted) {
        if (state->lastsynctype != DSD_SYNC_M17_LSF_NEG
            && !(state->lastsynctype == DSD_SYNC_M17_PKT_NEG && frame_sync_m17_chain_has_evidence(state))) {
            return DSD_SYNC_NONE;
        }
        printFrameSync(opts, state, "-M17 PKT", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_PKT_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_PKT_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17_stream(frame_sync_match_ctx* ctx, int ham_str, int ham_lsf, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;

    if (ham_str <= 1 && !is_inverted) {
        if (state->lastsynctype != DSD_SYNC_M17_LSF_POS
            && !(state->lastsynctype == DSD_SYNC_M17_STR_POS && frame_sync_m17_chain_has_evidence(state))) {
            return DSD_SYNC_NONE;
        }
        printFrameSync(opts, state, "+M17 STR", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_STR_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_STR_POS;
    }

    if (ham_lsf <= 1 && is_inverted) {
        if (state->lastsynctype != DSD_SYNC_M17_LSF_NEG
            && !(state->lastsynctype == DSD_SYNC_M17_STR_NEG && frame_sync_m17_chain_has_evidence(state))) {
            return DSD_SYNC_NONE;
        }
        printFrameSync(opts, state, "-M17 STR", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_STR_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_STR_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_accept_m17(frame_sync_match_ctx* ctx, const char* label, int synctype) {
    printFrameSync(ctx->opts, ctx->state, label, ctx->synctest_pos + 1, ctx->modulation);
    frame_sync_set_basic_lock(ctx);
    ctx->state->lastsynctype = synctype;
    dsd_sync_warm_start_thresholds_outer_only(ctx->opts, ctx->state, 8);
    return synctype;
}

/**
 * @brief Spend a candidate on the sync word that confirmed it, at the polarity that word named.
 */
static void
frame_sync_consume_m17_pre_candidate(dsd_state* state, int polarity) {
    state->m17_polarity = (uint8_t)polarity;
    state->m17_pre_candidate = 0;
    state->m17_pre_candidate_ttl = 0;
}

static int
frame_sync_after_m17_bert(const dsd_state* state, int is_inverted) {
    /* A BERT frame carries no CRC and its sync word is eight symbols, so one accepted on noise
     * used to hand the next frame the same permission and the chain sustained itself for the
     * rest of the capture, crowding out every other protocol on the profile. Continuing it
     * costs a PRBS9 lock -- the one thing a real bit-error-rate test has and noise does not.
     * The session's first frame still arrives on a preamble candidate (#399). */
    if (state->m17_bert_locked == 0) {
        return 0;
    }
    return (!is_inverted && state->lastsynctype == DSD_SYNC_M17_BRT_POS)
           || (is_inverted && state->lastsynctype == DSD_SYNC_M17_BRT_NEG);
}

static int
frame_sync_try_m17_bert_after_context(frame_sync_match_ctx* ctx, int ham_brt, int ham_pkt, int is_inverted) {
    const int hamming = is_inverted ? ham_pkt : ham_brt;
    if (hamming > 1) {
        return DSD_SYNC_NONE;
    }

    return is_inverted ? frame_sync_accept_m17(ctx, "-M17 BRT", DSD_SYNC_M17_BRT_NEG)
                       : frame_sync_accept_m17(ctx, "+M17 BRT", DSD_SYNC_M17_BRT_POS);
}

static int
frame_sync_try_m17_lsf_or_bert(frame_sync_match_ctx* ctx, int ham_lsf, int ham_str, int ham_brt, int ham_pkt,
                               int is_inverted) {
    dsd_state* state = ctx->state;

    if (state->m17_pre_candidate != 0) {
        /* An alternating run is symmetric: read one symbol later, the marker is its own
         * inversion, so a preamble can never say which polarity the transmission has -- the old
         * code's answer was whichever phase it happened to accept on. The sync word behind it
         * can say, because M17_LSF and M17_STR are not shifts of each other. Try both readings
         * and let the one that matches name the polarity the rest of the chain locks to. */
        for (int pass = 0; pass < 2; pass++) {
            const int inverted = (ctx->opts->inverted_m17 == 0) ? pass : !pass;
            if ((inverted ? ham_str : ham_lsf) <= 1) {
                frame_sync_consume_m17_pre_candidate(state, inverted ? 2 : 1);
                return inverted ? frame_sync_accept_m17(ctx, "-M17 LSF", DSD_SYNC_M17_LSF_NEG)
                                : frame_sync_accept_m17(ctx, "+M17 LSF", DSD_SYNC_M17_LSF_POS);
            }
        }
        for (int pass = 0; pass < 2; pass++) {
            const int inverted = (ctx->opts->inverted_m17 == 0) ? pass : !pass;
            if ((inverted ? ham_pkt : ham_brt) <= 1) {
                frame_sync_consume_m17_pre_candidate(state, inverted ? 2 : 1);
                return inverted ? frame_sync_accept_m17(ctx, "-M17 BRT", DSD_SYNC_M17_BRT_NEG)
                                : frame_sync_accept_m17(ctx, "+M17 BRT", DSD_SYNC_M17_BRT_POS);
            }
        }
    }

    /* A BERT session repeats its own sync word without a fresh preamble between frames. */
    if (frame_sync_after_m17_bert(state, is_inverted)) {
        return frame_sync_try_m17_bert_after_context(ctx, ham_brt, ham_pkt, is_inverted);
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_m17 != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4)
        || !frame_sync_match_window_ready(ctx, 8)) {
        return DSD_SYNC_NONE;
    }

    int ham_pre = dsd_sync_hamming_distance(ctx->synctest8, M17_PRE, 8);
    int ham_piv = dsd_sync_hamming_distance(ctx->synctest8, M17_PIV, 8);
    int ham_lsf = dsd_sync_hamming_distance(ctx->synctest8, M17_LSF, 8);
    int ham_str = dsd_sync_hamming_distance(ctx->synctest8, M17_STR, 8);
    int ham_pkt = dsd_sync_hamming_distance(ctx->synctest8, M17_PKT, 8);
    int ham_brt = dsd_sync_hamming_distance(ctx->synctest8, M17_BRT, 8);
    int ham_eot = dsd_sync_hamming_distance(ctx->synctest8, M17_EOT, 8);
    int ham_eot_inv = dsd_sync_hamming_distance(ctx->synctest8, M17_EOT_INV, 8);
    int is_inverted = opts->inverted_m17;
    if (!opts->inverted_m17) {
        /* Before any frame has been accepted the polarity of the run in hand is all there is to
         * go on, so a pending candidate speaks for it. */
        const int polarity = state->m17_polarity != 0 ? state->m17_polarity : state->m17_pre_candidate;
        if (polarity == 2) {
            is_inverted = 1;
        }
    }

    frame_sync_age_m17_pre_candidate(state);
    frame_sync_note_m17_pre_candidate(ctx, ham_pre, ham_piv);

    int sync_type = frame_sync_try_m17_eot(ctx, ham_eot, ham_eot_inv, is_inverted);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_m17_lsf_or_bert(ctx, ham_lsf, ham_str, ham_brt, ham_pkt, is_inverted);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_m17_stream(ctx, ham_str, ham_lsf, is_inverted);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_m17_packet(ctx, ham_pkt, ham_brt, is_inverted);
}

static inline void
frame_sync_prepare_dmr_sync(frame_sync_match_ctx* ctx) {
    frame_sync_set_basic_lock(ctx);
    frame_sync_maybe_force_dmr_gfsk(ctx->opts, ctx->state);
    dmr_set_symbol_timing(ctx->opts, ctx->state);
}

static int
frame_sync_try_dmr_ms_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_MS_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR MS");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_ms_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_MS_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR MS");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr_bs_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_BS_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 0;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_BS_DATA_POS;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_NEG;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_BS_VOICE_NEG;
}

static int
frame_sync_try_dmr_dm_ts1_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS1_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_dm_ts2_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS2_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_bs_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_BS_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 0;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_BS_VOICE_POS;
    }

    if (opts->errorbars == 1) {
        printFrameSync(opts, state, "-DMR ", ctx->synctest_pos + 1, ctx->modulation);
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_NEG;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_BS_DATA_NEG;
}

static int
frame_sync_try_dmr_dm_ts1_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS1_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr_dm_ts2_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS2_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr_rc_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    /* The RC sync has no voice/data complement partner: its symbol-wise
     * complement is the ETSI-reserved pattern, so it maps to RC only when the
     * input polarity is inverted and is never claimed at normal polarity. */
    const char* pattern = (opts->inverted_dmr == 0) ? DMR_MS_RC_SYNC : DMR_MS_RC_SYNC_INV;
    if (strcmp(ctx->synctest, pattern) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DMR RC");
    state->lastsynctype = DSD_SYNC_DMR_RC_DATA;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_RC_DATA;
}

static int
frame_sync_try_dmr(frame_sync_match_ctx* ctx) {
    if (ctx->opts->frame_dmr != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4)
        || !frame_sync_match_window_ready(ctx, 24)) {
        return DSD_SYNC_NONE;
    }

    int sync_type = frame_sync_try_dmr_ms_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_ms_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_bs_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts1_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts2_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_bs_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts1_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts2_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_dmr_rc_data(ctx);
}

static int
frame_sync_symbols_match_either(const char* symbols, const char* pattern_a, const char* pattern_b) {
    return strcmp(symbols, pattern_a) == 0 || strcmp(symbols, pattern_b) == 0;
}

static int
frame_sync_accept_provoice(frame_sync_match_ctx* ctx, int synctype, const char* label, int always_print) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    frame_sync_note_cc_sync(ctx);
    frame_sync_set_basic_lock(ctx);
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "ProVoice ");
    if (always_print || opts->errorbars == 1) {
        printFrameSync(opts, state, label, ctx->synctest_pos + 1, ctx->modulation);
    }
    state->lastsynctype = synctype;
    dsd_sync_warm_start_thresholds_outer_only(opts, state, 32);
    return synctype;
}

static int
frame_sync_accept_edacs(frame_sync_match_ctx* ctx, int synctype, const char* label) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    dsd_mark_cc_sync(state);
    frame_sync_set_basic_lock(ctx);
    printFrameSync(opts, state, label, ctx->synctest_pos + 1, ctx->modulation);
    state->lastsynctype = synctype;
    dsd_sync_warm_start_thresholds_outer_only(opts, state, 48);
    return synctype;
}

static void
frame_sync_handle_edacs_dotting(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->trunk_enable == 1 && opts->trunk_is_tuned == 1) {
        printFrameSync(opts, state, " EDACS  DOTTING SEQUENCE: ", ctx->synctest_pos + 1, ctx->modulation);
        dsd_frame_sync_hook_eot_cc(opts, state);
    }
}

static int
frame_sync_try_provoice(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    if (opts->frame_provoice != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_9600_2)) {
        return DSD_SYNC_NONE;
    }

    if (frame_sync_match_window_ready(ctx, 32)) {
        if (frame_sync_symbols_match_either(ctx->synctest32, PROVOICE_SYNC, PROVOICE_EA_SYNC)) {
            return frame_sync_accept_provoice(ctx, DSD_SYNC_PROVOICE_POS, "+PV   ", 0);
        }
        if (frame_sync_symbols_match_either(ctx->synctest32, INV_PROVOICE_SYNC, INV_PROVOICE_EA_SYNC)) {
            return frame_sync_accept_provoice(ctx, DSD_SYNC_PROVOICE_NEG, "-PV   ", 1);
        }
    }

    if (frame_sync_match_window_ready(ctx, 48)) {
        if (strcmp(ctx->synctest48, EDACS_SYNC) == 0) {
            return frame_sync_accept_edacs(ctx, DSD_SYNC_EDACS_NEG, "-EDACS");
        }
        if (strcmp(ctx->synctest48, INV_EDACS_SYNC) == 0) {
            return frame_sync_accept_edacs(ctx, DSD_SYNC_EDACS_POS, "+EDACS");
        }
        if (frame_sync_symbols_match_either(ctx->synctest48, DOTTING_SEQUENCE_A, DOTTING_SEQUENCE_B)) {
            frame_sync_handle_edacs_dotting(ctx);
        }
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_dstar(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_dstar != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_2)
        || !frame_sync_match_window_ready(ctx, 24) || dsd_frame_sync_suppress_p25_alt_sync(opts, state)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest, DSTAR_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DSTAR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DSTAR VOICE ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_VOICE_POS;
    }

    if (strcmp(ctx->synctest, INV_DSTAR_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DSTAR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-DSTAR VOICE ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_VOICE_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_VOICE_NEG;
    }

    if (strcmp(ctx->synctest, DSTAR_HD) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "DSTAR_HD ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DSTAR HEADER", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_HD_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_HD_POS;
    }

    if (strcmp(ctx->synctest, INV_DSTAR_HD) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " DSTAR_HD");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-DSTAR HEADER", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_HD_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_HD_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_nxdn_sync_type(const char* symbols) {
    static const char* const positive_patterns[] = {"3131331131", "3331331131", "3131331111", "3331331111",
                                                    "3131311131"};
    static const char* const negative_patterns[] = {"1313113313", "1113113313", "1313113333", "1113113333",
                                                    "1313133313"};
    for (size_t i = 0; i < sizeof(positive_patterns) / sizeof(positive_patterns[0]); i++) {
        if (strcmp(symbols, positive_patterns[i]) == 0) {
            return DSD_SYNC_NXDN_POS;
        }
        if (strcmp(symbols, negative_patterns[i]) == 0) {
            return DSD_SYNC_NXDN_NEG;
        }
    }
    return DSD_SYNC_NONE;
}

static int
frame_sync_try_nxdn(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    int nxdn_profile_enabled = 0;
    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN || opts->audio_in_type == AUDIO_IN_SYMBOL_FLT) {
        /* Symbol captures carry no rate metadata, so an enabled variant must be unambiguous. */
        nxdn_profile_enabled = (opts->frame_nxdn48 == 1) != (opts->frame_nxdn96 == 1);
    } else {
        nxdn_profile_enabled =
            (opts->frame_nxdn96 == 1 && frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_4800_4))
            || (opts->frame_nxdn48 == 1 && frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_2400_4));
    }
    if (!nxdn_profile_enabled || !frame_sync_match_window_ready(ctx, 10)) {
        return DSD_SYNC_NONE;
    }

    const int synctype = frame_sync_nxdn_sync_type(ctx->synctest10);
    if (synctype == DSD_SYNC_NONE) {
        return DSD_SYNC_NONE;
    }

    state->offset = ctx->synctest_pos;
    state->max = ((state->max) + ctx->lmax) / 2;
    state->min = ((state->min) + ctx->lmin) / 2;
    if (state->lastsynctype == synctype) {
        frame_sync_note_cc_sync(ctx);
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 10);
        return synctype;
    }
    state->lastsynctype = synctype;
    return DSD_SYNC_NONE;
}

#ifdef PVCONVENTIONAL
static void
frame_sync_pvconv_decode_addrs(const frame_sync_match_ctx* ctx, char one_symbol, uint8_t* tx_addr, uint8_t* rx_addr) {
    *tx_addr = 0;
    *rx_addr = 0;
    for (int bit = 0; bit < 8; bit++) {
        *tx_addr = (uint8_t)(*tx_addr << 1);
        *rx_addr = (uint8_t)(*rx_addr << 1);
        if (ctx->synctest16[bit] == one_symbol) {
            *tx_addr = (uint8_t)(*tx_addr + 1);
        }
        if (ctx->synctest16[8 + bit] == one_symbol) {
            *rx_addr = (uint8_t)(*rx_addr + 1);
        }
    }
}

static int
frame_sync_try_provoice_conventional(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_provoice != 1 || !frame_sync_match_profile_active(ctx, DSD_FRAME_SYNC_SPS_PROFILE_9600_2)
        || !frame_sync_match_window_ready(ctx, 32)) {
        return DSD_SYNC_NONE;
    }

    if (strncmp(ctx->synctest32, INV_PROVOICE_CONV_SHORT, 16) == 0) {
        if (state->lastsynctype == DSD_SYNC_PROVOICE_NEG) {
            frame_sync_set_basic_lock(ctx);
            DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "ProVoice ");
            uint8_t pvc_txa = 0;
            uint8_t pvc_rxa = 0;
            frame_sync_pvconv_decode_addrs(ctx, '1', &pvc_txa, &pvc_rxa);
            printFrameSync(opts, state, "-PV_C ", ctx->synctest_pos + 1, ctx->modulation);
            DSD_FPRINTF(stderr, "TX: %d ", pvc_txa);
            DSD_FPRINTF(stderr, "RX: %d ", pvc_rxa);
            if (pvc_txa == 172) {
                DSD_FPRINTF(stderr, "ALL CALL ");
            }
            state->lastsynctype = DSD_SYNC_PROVOICE_NEG;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 16);
            return DSD_SYNC_PROVOICE_NEG;
        }
        state->lastsynctype = DSD_SYNC_PROVOICE_NEG;
        return DSD_SYNC_NONE;
    }

    if (strncmp(ctx->synctest32, PROVOICE_CONV_SHORT, 16) == 0) {
        if (state->lastsynctype == DSD_SYNC_PROVOICE_POS) {
            frame_sync_set_basic_lock(ctx);
            DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "ProVoice ");
            uint8_t pvc_txa = 0;
            uint8_t pvc_rxa = 0;
            frame_sync_pvconv_decode_addrs(ctx, '3', &pvc_txa, &pvc_rxa);
            printFrameSync(opts, state, "+PV_C ", ctx->synctest_pos + 1, ctx->modulation);
            DSD_FPRINTF(stderr, "TX: %d ", pvc_txa);
            DSD_FPRINTF(stderr, "RX: %d ", pvc_rxa);
            if (pvc_txa == 172) {
                DSD_FPRINTF(stderr, "ALL CALL ");
            }
            state->lastsynctype = DSD_SYNC_PROVOICE_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 16);
            return DSD_SYNC_PROVOICE_POS;
        }
        state->lastsynctype = DSD_SYNC_PROVOICE_POS;
    }

    return DSD_SYNC_NONE;
}
#else
static int
frame_sync_try_provoice_conventional(frame_sync_match_ctx* ctx) {
    UNUSED(ctx);
    return DSD_SYNC_NONE;
}
#endif

static int
frame_sync_try_protocol_matches(frame_sync_match_ctx* ctx) {
    int sync_type = frame_sync_try_p25p1(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_x2tdma(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_ysf(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_m17(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_p25p2(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_dpmr(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_dmr(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_provoice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_dstar(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_nxdn(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    return frame_sync_try_provoice_conventional(ctx);
}

static time_t g_p25_trunk_tick_last_tick = 0;
static time_t g_p25_trunk_tick_last_p25_seen = 0;

void
frame_sync_maybe_tick_p25_trunk_sm(dsd_opts* opts, dsd_state* state, time_t now) {
    if (now == g_p25_trunk_tick_last_tick) {
        return;
    }

    int p25_by_sync = DSD_SYNC_IS_P25(state->lastsynctype) ? 1 : 0;
    if (p25_by_sync) {
        g_p25_trunk_tick_last_p25_seen = now;
    }
    int p25_recent = (g_p25_trunk_tick_last_p25_seen != 0 && (now - g_p25_trunk_tick_last_p25_seen) <= 3) ? 1 : 0;
    int p25_active = p25_by_sync || p25_recent || (state->p25_p2_active_slot != -1);
    if (opts->trunk_enable == 1 && p25_active) {
        dsd_frame_sync_hook_p25_sm_try_tick(opts, state);
    }
    g_p25_trunk_tick_last_tick = now;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_frame_sync_test_reset_p25_trunk_tick_state(void) {
    g_p25_trunk_tick_last_tick = 0;
    g_p25_trunk_tick_last_p25_seen = 0;
}

#endif

static inline void
frame_sync_apply_cli_mod_lock(const dsd_opts* opts, dsd_state* state) {
    if (opts->mod_cli_lock) {
        int forced = opts->mod_qpsk ? 1 : (opts->mod_gfsk ? 2 : 0);
        state->rf_mod = forced;
    }
}

static int
frame_sync_select_t_max(const dsd_opts* opts, const dsd_state* state) {
    switch (state->sps_hunt_idx) {
        case DSD_FRAME_SYNC_SPS_PROFILE_2400_4: return 12;
        case DSD_FRAME_SYNC_SPS_PROFILE_6000_4:
            if (state->rf_mod == 1 && opts->frame_p25p2 == 1) {
                return 19;
            }
            return 24;
        case DSD_FRAME_SYNC_SPS_PROFILE_4800_4:
            if (DSD_SYNC_IS_YSF(state->lastsynctype)) {
                return 20;
            }
            return 24;
        default: return 24;
    }
}

static inline void
frame_sync_update_symbol_ring(const dsd_opts* opts, dsd_state* state, float symbol, float* lbuf, int* lidx,
                              int* level_count, int t_max) {
    lbuf[*lidx] = symbol;
    if (*level_count < t_max) {
        (*level_count)++;
    }
    state->sbuf[state->sidx] = symbol;
    if (*lidx == (t_max - 1)) {
        *lidx = 0;
    } else {
        (*lidx)++;
    }
    if (state->sidx == (opts->ssize - 1)) {
        state->sidx = 0;
    } else {
        state->sidx++;
    }
}

#ifdef USE_RADIO
static int
frame_sync_should_bypass_c4fm_qpsk_snr_bias(const dsd_opts* opts, const dsd_state* state, int want_mod) {
    return want_mod == 2 && frame_sync_profile_uses_gfsk_exclusively(opts, state->sps_hunt_idx);
}
#endif

static int
frame_sync_bias_want_mod_with_snr(const dsd_opts* opts, const dsd_state* state, int want_mod) {
#ifdef USE_RADIO
    if (frame_sync_should_bypass_c4fm_qpsk_snr_bias(opts, state, want_mod)) {
        return want_mod;
    }
    double snr_c = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
    double snr_q = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    if (snr_c <= -50.0) {
        snr_c = dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db();
    }
    if (snr_q <= -50.0) {
        snr_q = dsd_rtl_stream_metrics_hook_snr_qpsk_const_db();
    }
    if (snr_c <= -50.0) {
        if (snr_q <= -50.0) {
            return want_mod;
        }
        if (state->rf_mod == 1) {
            return 1;
        }
        return want_mod;
    }
    if (snr_q <= -50.0) {
        return want_mod;
    }

    const double kQpskOffsetDb = 6.0;
    double normalized_delta = (snr_q - kQpskOffsetDb) - snr_c;
    uint32_t now_ms = (uint32_t)dsd_time_monotonic_ms();
    uint32_t dwell_enter_ms = (uint32_t)atomic_load(&g_qpsk_dwell_enter_ms);
    int in_qpsk_dwell = (state->rf_mod == 1 && dwell_enter_ms != 0 && (uint32_t)(now_ms - dwell_enter_ms) < 2000U);
    if (normalized_delta >= 2.0) {
        return 1;
    }
    if (normalized_delta <= -3.0 && !in_qpsk_dwell) {
        return 0;
    }
    if (state->rf_mod == 1) {
        return 1;
    }
#else
    UNUSED(opts);
    UNUSED(state);
#endif
    return want_mod;
}

static int
frame_sync_decay_recent_ham(atomic_int* ham_counter) {
    int ham = atomic_load(ham_counter);
    if (ham < 24) {
        ham++;
        atomic_store(ham_counter, ham);
    }
    return ham;
}

static int
frame_sync_ham_for_mod(int mod, int ham_c4fm, int ham_qpsk, int ham_gfsk) {
    if (mod == 1) {
        return ham_qpsk;
    }
    if (mod == 2) {
        return ham_gfsk;
    }
    return ham_c4fm;
}

static int
frame_sync_c4fm_ham_candidate_enabled(const dsd_opts* opts, const dsd_state* state) {
    return state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && opts->frame_p25p1 == 1;
}

static int
frame_sync_qpsk_ham_candidate_enabled(const dsd_opts* opts, const dsd_state* state) {
    return (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && opts->frame_p25p1 == 1)
           || (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4 && opts->frame_p25p2 == 1);
}

static int
frame_sync_gfsk_ham_candidate_enabled(const dsd_opts* opts, const dsd_state* state) {
    return (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4
            && (opts->frame_dmr == 1 || opts->frame_nxdn96 == 1))
           || (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4
               && (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1))
           || frame_sync_sps_profile_for_index(state->sps_hunt_idx)->levels == 2;
}

static int
frame_sync_override_want_mod_with_hamming(const dsd_opts* opts, const dsd_state* state, int want_mod) {
    int ham_c4fm = frame_sync_decay_recent_ham(&g_ham_c4fm_recent);
    int ham_qpsk = frame_sync_decay_recent_ham(&g_ham_qpsk_recent);
    int ham_gfsk = frame_sync_decay_recent_ham(&g_ham_gfsk_recent);

    int best_mod = want_mod;
    int best_ham = frame_sync_ham_for_mod(want_mod, ham_c4fm, ham_qpsk, ham_gfsk);
    if (frame_sync_c4fm_ham_candidate_enabled(opts, state) && ham_c4fm < best_ham) {
        best_ham = ham_c4fm;
        best_mod = 0;
    }
    if (frame_sync_qpsk_ham_candidate_enabled(opts, state) && ham_qpsk < best_ham) {
        best_ham = ham_qpsk;
        best_mod = 1;
    }
    if (frame_sync_gfsk_ham_candidate_enabled(opts, state) && ham_gfsk < best_ham) {
        best_ham = ham_gfsk;
        best_mod = 2;
    }

    if (best_ham <= 3) {
        return best_mod;
    }
    if (best_ham >= 24) {
        return want_mod;
    }
    int current_ham = frame_sync_ham_for_mod(want_mod, ham_c4fm, ham_qpsk, ham_gfsk);
    if (current_ham >= 24 || best_ham + 4 <= current_ham) {
        return best_mod;
    }
    return want_mod;
}

/**
 * @brief Keep the CQPSK chain while it is the one decoding P25p1 frames.
 *
 * Both inputs to the modulation vote are indirect: the SNR bias compares an estimate the
 * inactive chain cannot make honestly, and the sync-hamming candidates score patterns a strong
 * signal matches on more than one modulation. A P25p1 frame whose 63-bit NID BCH decoded is
 * direct evidence instead -- it says this demodulator is working right now -- so while such
 * frames keep arriving the guesses do not get to move off it.
 *
 * This matters because CQPSK is the side that cannot defend itself. C4FM is the profile
 * default and what every rotation falls back to, whereas entering GFSK costs a single vote, so
 * without a hold the chain is taken back within a few dozen symbols and an LSM control channel
 * never gets to keep the demodulator that decodes it. The hold lapses as soon as the frames
 * stop, so it can never strand a profile on a modulation that has gone quiet.
 */
static int
frame_sync_hold_validated_p25p1_modulation(const dsd_opts* opts, const dsd_state* state, int want_mod) {
    if (opts->frame_p25p1 != 1 || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4) {
        return want_mod;
    }
    /* A trial the votes can revoke before it has decoded a single frame is not a trial, so the
     * probe's own dwell is protected whichever chain it selected. */
    if (state->p25_p1_mod_probe_active && (state->rf_mod == 0 || state->rf_mod == 1)) {
        return state->rf_mod;
    }
    if (state->rf_mod != 1 || state->p25_p1_validated_rf_mod != 1) {
        return want_mod;
    }
    if ((uint32_t)(state->symbolcnt - state->p25_p1_validated_symbolcnt)
        >= DSD_FRAME_SYNC_P25P1_VALIDATED_HOLD_SYMBOLS) {
        return want_mod;
    }
    return 1;
}

static void
frame_sync_update_mod_votes(int want_mod) {
    if (want_mod == 1) {
        atomic_fetch_add(&g_vote_qpsk, 1);
        atomic_store(&g_vote_c4fm, 0);
        atomic_store(&g_vote_gfsk, 0);
    } else if (want_mod == 2) {
        atomic_fetch_add(&g_vote_gfsk, 1);
        atomic_store(&g_vote_qpsk, 0);
        atomic_store(&g_vote_c4fm, 0);
    } else {
        atomic_fetch_add(&g_vote_c4fm, 1);
        atomic_store(&g_vote_qpsk, 0);
        atomic_store(&g_vote_gfsk, 0);
    }
}

static int
frame_sync_decide_mod_switch(const dsd_state* state, int want_mod) {
    uint32_t now_ms = (uint32_t)dsd_time_monotonic_ms();
    uint32_t dwell_enter_ms = (uint32_t)atomic_load(&g_qpsk_dwell_enter_ms);
    int in_qpsk_dwell2 = (state->rf_mod == 1 && dwell_enter_ms != 0 && (uint32_t)(now_ms - dwell_enter_ms) < 2000U);
    int req_c4_votes = (state->rf_mod == 1) ? (in_qpsk_dwell2 ? 5 : 3) : 2;
    int vote_qpsk = atomic_load(&g_vote_qpsk);
    int vote_gfsk = atomic_load(&g_vote_gfsk);
    int vote_c4fm = atomic_load(&g_vote_c4fm);
    if (want_mod == 1 && vote_qpsk >= 2 && state->rf_mod != 1) {
        return 1;
    }
    if (want_mod == 2 && vote_gfsk >= 1 && state->rf_mod != 2) {
        return 2;
    }
    if (want_mod == 0 && vote_c4fm >= req_c4_votes && state->rf_mod != 0) {
        return 0;
    }
    return -1;
}

static void
frame_sync_apply_mod_switch(const dsd_opts* opts, dsd_state* state, int do_switch) {
    if (do_switch < 0) {
        return;
    }
    if (do_switch == 1) {
        atomic_store(&g_qpsk_dwell_enter_ms, (int)(uint32_t)dsd_time_monotonic_ms());
    } else if (state->rf_mod == 1) {
        atomic_store(&g_qpsk_dwell_enter_ms, 0);
    }
    state->rf_mod = do_switch;
#ifdef USE_RADIO
    rtl_maybe_apply_active_demod_profile(opts, state);
#else
    UNUSED(opts);
#endif
    atomic_store(&g_ham_c4fm_recent, 24);
    atomic_store(&g_ham_qpsk_recent, 24);
    atomic_store(&g_ham_gfsk_recent, 24);
}

void
frame_sync_maybe_auto_switch_modulation(const dsd_opts* opts, dsd_state* state, int t_max, int* lastt) {
    if (*lastt < t_max) {
        (*lastt)++;
    }
    if (*lastt < t_max) {
        return;
    }

    *lastt = 0;
    if (opts->mod_cli_lock) {
        return;
    }

    int want_mod = frame_sync_active_profile_modulation(opts, state);
    want_mod = frame_sync_bias_want_mod_with_snr(opts, state, want_mod);
    want_mod = frame_sync_override_want_mod_with_hamming(opts, state, want_mod);
    want_mod = frame_sync_hold_validated_p25p1_modulation(opts, state, want_mod);
    frame_sync_update_mod_votes(want_mod);
    frame_sync_apply_mod_switch(opts, state, frame_sync_decide_mod_switch(state, want_mod));
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_frame_sync_test_set_recent_hamming(int ham_c4fm, int ham_qpsk, int ham_gfsk) {
    atomic_store(&g_ham_c4fm_recent, ham_c4fm);
    atomic_store(&g_ham_qpsk_recent, ham_qpsk);
    atomic_store(&g_ham_gfsk_recent, ham_gfsk);
}

int
dsd_frame_sync_test_qpsk_dwell_armed(void) {
    return atomic_load(&g_qpsk_dwell_enter_ms) != 0;
}

void
dsd_frame_sync_test_get_mod_votes(int* out_c4fm, int* out_qpsk, int* out_gfsk) {
    if (out_c4fm) {
        *out_c4fm = atomic_load(&g_vote_c4fm);
    }
    if (out_qpsk) {
        *out_qpsk = atomic_load(&g_vote_qpsk);
    }
    if (out_gfsk) {
        *out_gfsk = atomic_load(&g_vote_gfsk);
    }
}

#endif

static void
frame_sync_debug_symbol_stats(float symbol) {
#ifdef USE_RADIO
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init();
        cfg_dbg = dsd_neo_get_config();
    }
    if (cfg_dbg && cfg_dbg->debug_sync_enable) {
        static int sym_count = 0;
        static int pos_count = 0, neg_count = 0;
        static float sym_min = 1e9f, sym_max = -1e9f, sym_sum = 0.0f;
        if (symbol < sym_min) {
            sym_min = symbol;
        }
        if (symbol > sym_max) {
            sym_max = symbol;
        }
        sym_sum += symbol;
        if (symbol > 0) {
            pos_count++;
        } else {
            neg_count++;
        }
        if (++sym_count >= 4800) {
            float dc = sym_sum / (float)sym_count;
            DSD_FPRINTF(stderr, "[SYNC] range:[%.1f,%.1f] dc:%.2f ratio(1:3)=%d:%d\n", sym_min, sym_max, dc, pos_count,
                        neg_count);
            sym_min = 1e9f;
            sym_max = -1e9f;
            sym_sum = 0.0f;
            pos_count = neg_count = 0;
            sym_count = 0;
        }
    }
#else
    UNUSED(symbol);
#endif
}

static int
frame_sync_cqpsk_4level_enabled(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (state->rf_mod == 1 && opts->audio_in_type == AUDIO_IN_RTL
        && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1)) {
        int dsp_cqpsk = 0;
        int dsp_timing = 0;
        dsd_rtl_stream_metrics_hook_cqpsk_status(&dsp_cqpsk, &dsp_timing);
        if (dsp_cqpsk && dsp_timing) {
            return 1;
        }
    }
#else
    UNUSED2(opts, state);
#endif
    return 0;
}

static int
frame_sync_slice_cqpsk_dibit(const dsd_state* state, float symbol) {
    float sym = symbol - state->center;
    int d = 0;
    if (sym >= 2.0f) {
        d = 1;
    } else if (sym >= 0.0f) {
        d = 0;
    } else if (sym >= -2.0f) {
        d = 2;
    } else {
        d = 3;
    }

#ifdef USE_RADIO
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init();
        cfg_dbg = dsd_neo_get_config();
    }
    if (cfg_dbg && cfg_dbg->debug_cqpsk_enable) {
        static int sample_count = 0;
        static int hist[4] = {0, 0, 0, 0};
        static float sym_sum = 0.0f;
        static float sym_min = 1000.0f, sym_max = -1000.0f;
        hist[d]++;
        sym_sum += sym;
        if (sym < sym_min) {
            sym_min = sym;
        }
        if (sym > sym_max) {
            sym_max = sym;
        }
        if (++sample_count >= 4800) {
            float sym_avg = sym_sum / sample_count;
            DSD_FPRINTF(stderr, "[SLICER] d0:%.1f%% d1:%.1f%% d2:%.1f%% d3:%.1f%% avg:%.2f range:[%.2f,%.2f] (n=%d)\n",
                        100.0f * hist[0] / sample_count, 100.0f * hist[1] / sample_count,
                        100.0f * hist[2] / sample_count, 100.0f * hist[3] / sample_count, sym_avg, sym_min, sym_max,
                        sample_count);
            hist[0] = hist[1] = hist[2] = hist[3] = 0;
            sample_count = 0;
            sym_sum = 0.0f;
            sym_min = 1000.0f;
            sym_max = -1000.0f;
        }
    }
#endif
    return d;
}

static int
frame_sync_symbol_to_dibit(dsd_state* state, float symbol, int cqpsk_4level) {
    if (cqpsk_4level) {
        int d = frame_sync_slice_cqpsk_dibit(state, symbol);
        *state->dibit_buf_p = d;
        state->dibit_buf_p++;
        return '0' + d;
    }

    if (symbol > 0) {
        *state->dibit_buf_p = 1;
        state->dibit_buf_p++;
        return '1';
    }
    *state->dibit_buf_p = 3;
    state->dibit_buf_p++;
    return '3';
}

static void
frame_sync_capture_symbol(dsd_opts* opts, dsd_state* state, int dibit, float symbol, int cqpsk_4level) {
    if (!opts->symbol_out_f || dibit == 0) {
        return;
    }
#ifndef USE_RADIO
    UNUSED(cqpsk_4level);
#endif
    int csymbol = 0;
#ifdef USE_RADIO
    if (cqpsk_4level) {
        csymbol = dibit - '0';
    } else
#endif
        if (dibit == '1') {
        csymbol = 1;
    } else if (dibit == '3') {
        csymbol = 3;
    }
    write_symbol_capture_record(opts, state, csymbol, symbol, NULL);
}

static void
frame_sync_reset_dmr_payload_ptrs(dsd_state* state) {
    if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
        state->dmr_payload_p = state->dmr_payload_buf + 200;
    }
    if (state->dmr_soft_p && state->dmr_soft_p > state->dmr_soft_buf + 900000) {
        state->dmr_soft_p = state->dmr_soft_buf + 200;
    }
}

static void
frame_sync_store_dmr_payload_symbol(dsd_state* state, float symbol, int cqpsk_4level) {
    int d = 0;
    if (cqpsk_4level) {
        float sym = symbol - state->center;
        if (sym >= 2.0f) {
            d = 1;
        } else if (sym >= 0.0f) {
            d = 0;
        } else if (sym >= -2.0f) {
            d = 2;
        } else {
            d = 3;
        }
    } else if (symbol > state->center) {
        d = (symbol > state->umid) ? 1 : 0;
    } else {
        d = (symbol < state->lmid) ? 3 : 2;
    }

    *state->dmr_payload_p = d;
    uint8_t rel = dmr_compute_reliability(state, symbol);
    if (state->dmr_soft_p) {
        state->dmr_soft_p->reliability = rel;
        state->dmr_soft_p->llr[0] = (int16_t)(((d >> 1) & 1) ? rel : -(int)rel);
        state->dmr_soft_p->llr[1] = (int16_t)((d & 1) ? rel : -(int)rel);
        state->dmr_soft_p++;
    }
    state->dmr_payload_p++;
}

static int
frame_sync_process_dibit_and_payload(dsd_opts* opts, dsd_state* state, float symbol) {
    if (state->dibit_buf_p > state->dibit_buf + 900000) {
        state->dibit_buf_p = state->dibit_buf + 200;
    }
    frame_sync_debug_symbol_stats(symbol);
    int cqpsk_4level = frame_sync_cqpsk_4level_enabled(opts, state);
    int dibit = frame_sync_symbol_to_dibit(state, symbol, cqpsk_4level);
    frame_sync_capture_symbol(opts, state, dibit, symbol, cqpsk_4level);
    frame_sync_reset_dmr_payload_ptrs(state);
    frame_sync_store_dmr_payload_symbol(state, symbol, cqpsk_4level);
    return dibit;
}

typedef struct {
    int t;
    int dibit;
    int synctest_pos;
    int lastt;
    int lidx;
    int level_count;
    int t_max;
    int history_head;
    int history_count;
    unsigned int ready_windows;
    float symbol;
    float lmin;
    float lmax;
    char synctest[25];
    char synctest12[13];
    char synctest10[11];
    char synctest32[33];
    char synctest20[21];
    char synctest48[49];
    char synctest8[9];
    char synctest16[17];
    char modulation[8];
    char symbol_history[FRAME_SYNC_HISTORY_CAPACITY];
    float lbuf[48];
    float lbuf2[48];
} frame_sync_runtime_ctx;

static void
frame_sync_runtime_init(frame_sync_runtime_ctx* rt, const dsd_opts* opts, const dsd_state* state) {
    DSD_MEMSET(rt, 0, sizeof(*rt));
    rt->t_max = frame_sync_select_t_max(opts, state);
    if (rt->t_max < 1 || rt->t_max > (int)(sizeof(rt->lbuf) / sizeof(rt->lbuf[0]))) {
        rt->t_max = 24;
    }
    rt->lmin = state->min;
    rt->lmax = state->max;
    rt->synctest[24] = 0;
    rt->synctest12[12] = 0;
    rt->synctest10[10] = 0;
    rt->synctest32[32] = 0;
    rt->synctest20[20] = 0;
    rt->synctest48[48] = 0;
    rt->synctest8[8] = 0;
    rt->synctest16[16] = 0;
    rt->modulation[7] = 0;
}

static void
frame_sync_history_push(frame_sync_runtime_ctx* rt, char symbol) {
    rt->symbol_history[rt->history_head] = symbol;
    rt->history_head = (rt->history_head + 1) % FRAME_SYNC_HISTORY_CAPACITY;
    if (rt->history_count < FRAME_SYNC_HISTORY_CAPACITY) {
        rt->history_count++;
    }
}

static int
frame_sync_history_materialize(const frame_sync_runtime_ctx* rt, int length, char* out, size_t out_size) {
    if (!rt || !out || length <= 0 || length > FRAME_SYNC_HISTORY_CAPACITY || out_size <= (size_t)length
        || rt->history_count < length) {
        return 0;
    }

    int index = rt->history_head - length;
    if (index < 0) {
        index += FRAME_SYNC_HISTORY_CAPACITY;
    }
    for (int i = 0; i < length; i++) {
        out[i] = rt->symbol_history[index];
        index = (index + 1) % FRAME_SYNC_HISTORY_CAPACITY;
    }
    out[length] = '\0';
    return 1;
}

static void
frame_sync_materialize_ready_windows(frame_sync_runtime_ctx* rt) {
    rt->ready_windows = 0;
    if (frame_sync_history_materialize(rt, 8, rt->synctest8, sizeof(rt->synctest8))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_8;
    }
    if (frame_sync_history_materialize(rt, 10, rt->synctest10, sizeof(rt->synctest10))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_10;
    }
    if (frame_sync_history_materialize(rt, 12, rt->synctest12, sizeof(rt->synctest12))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_12;
    }
    if (frame_sync_history_materialize(rt, 16, rt->synctest16, sizeof(rt->synctest16))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_16;
    }
    if (frame_sync_history_materialize(rt, 20, rt->synctest20, sizeof(rt->synctest20))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_20;
    }
    if (frame_sync_history_materialize(rt, 24, rt->synctest, sizeof(rt->synctest))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_24;
    }
    if (frame_sync_history_materialize(rt, 32, rt->synctest32, sizeof(rt->synctest32))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_32;
    }
    if (frame_sync_history_materialize(rt, 48, rt->synctest48, sizeof(rt->synctest48))) {
        rt->ready_windows |= FRAME_SYNC_WINDOW_48;
    }
}

static int
frame_sync_compare_float(const void* left, const void* right) {
    const float a = *(const float*)left;
    const float b = *(const float*)right;
    return (a > b) - (a < b);
}

static void
frame_sync_window_levels(const dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt) {
    const int level_count = rt->level_count;
    for (int i = 0; i < level_count; i++) {
        rt->lbuf2[i] = rt->lbuf[i];
    }
    qsort(rt->lbuf2, level_count, sizeof(float), frame_sync_compare_float);
    dsd_frame_sync_estimate_sorted_window_levels(rt->lbuf2, level_count, &rt->lmin, &rt->lmax);

    if (frame_sync_active_profile_modulation(opts, state) == 1) {
        dsd_state_push_minmax_window(state, opts->msize, rt->lmin, rt->lmax);
        state->center = ((state->max) + (state->min)) / 2.0f;
        state->maxref = (state->max) * 0.80F;
        state->minref = (state->min) * 0.80F;
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }
}

static int
frame_sync_profile_uses_gfsk_exclusively(const dsd_opts* opts, int profile_index) {
    if (dsd_frame_sync_profile_levels_force_gfsk(frame_sync_sps_profile_for_index(profile_index)->levels)) {
        return 1;
    }
    if (!opts) {
        return 0;
    }

    switch (profile_index) {
        case DSD_FRAME_SYNC_SPS_PROFILE_4800_4: {
            const int has_gfsk = opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_m17 == 1;
            const int has_other_modulation = opts->frame_p25p1 == 1 || opts->frame_ysf == 1;
            return has_gfsk && !has_other_modulation;
        }
        case DSD_FRAME_SYNC_SPS_PROFILE_2400_4: return opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1;
        default: return 0;
    }
}

int
frame_sync_active_profile_modulation(const dsd_opts* opts, const dsd_state* state) {
    int profile_index = state ? state->sps_hunt_idx : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    if (profile_index < 0 || profile_index >= DSD_FRAME_SYNC_SPS_PROFILE_COUNT) {
        profile_index = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    }
    if ((!opts || !opts->mod_cli_lock) && frame_sync_profile_uses_gfsk_exclusively(opts, profile_index)) {
        return 2;
    }
    if (state && state->rf_mod == 1) {
        return 1;
    }
    if (state && state->rf_mod == 2) {
        return 2;
    }
    return 0;
}

#ifdef USE_RADIO
double
frame_sync_active_profile_snr_db(const dsd_opts* opts, const dsd_state* state) {
    switch (frame_sync_active_profile_modulation(opts, state)) {
        case 1: return dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
        case 2: return dsd_rtl_stream_metrics_hook_snr_gfsk_db();
        default: return dsd_rtl_stream_metrics_hook_snr_c4fm_db();
    }
}
#endif

int
frame_sync_should_skip_snr_or_power_gate(const dsd_opts* opts, const dsd_state* state) {
    const int active_modulation = frame_sync_active_profile_modulation(opts, state);
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->snr_sql_is_set) {
            double snr_db = frame_sync_active_profile_snr_db(opts, state);
            if (snr_db > -150.0 && snr_db < (double)cfg->snr_sql_db) {
                return 1;
            }
        }
    }
#endif
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->rtl_pwr < opts->rtl_squelch_level && active_modulation == 2) {
        return 1;
    }
    return 0;
}

int
frame_sync_hamming_distance_pattern(const char* symbols, const char* pattern, int len) {
    int ham = 0;
    for (int k = 0; k < len; k++) {
        int d = (unsigned char)symbols[k] - '0';
        int expect = pattern[k] - '0';
        if (d != expect) {
            ham++;
        }
    }
    return ham;
}

static void
frame_sync_update_c4fm_hamming(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if (!(opts->frame_p25p1 == 1 && !opts->mod_cli_lock) || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4
        || (rt->ready_windows & FRAME_SYNC_WINDOW_24) == 0) {
        return;
    }
    int ham_norm = 0;
    int ham_inv = 0;
    for (int k = 0; k < 24; k++) {
        int d = (unsigned char)rt->synctest[k] - '0';
        int expect_n = P25P1_SYNC[k] - '0';
        int expect_i = INV_P25P1_SYNC[k] - '0';
        if (d != expect_n) {
            ham_norm++;
        }
        if (d != expect_i) {
            ham_inv++;
        }
    }
    int c4fm_ham = (ham_norm < ham_inv) ? ham_norm : ham_inv;
    int ham_c4fm_cur = atomic_load(&g_ham_c4fm_recent);
    if (c4fm_ham < ham_c4fm_cur) {
        atomic_store(&g_ham_c4fm_recent, c4fm_ham);
    }
}

static void
frame_sync_update_qpsk_hamming(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if (!((opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1) && !opts->mod_cli_lock)) {
        return;
    }

    int best_qpsk_ham = 24;
    int compared = 0;
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && opts->frame_p25p1 == 1
        && (rt->ready_windows & FRAME_SYNC_WINDOW_24) != 0) {
        best_qpsk_ham = dsd_qpsk_sync_hamming_with_remaps(rt->synctest, P25P1_SYNC, INV_P25P1_SYNC, 24);
        compared = 1;
    }
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4 && opts->frame_p25p2 == 1
        && (rt->ready_windows & FRAME_SYNC_WINDOW_20) != 0) {
        int ham_p2 = dsd_qpsk_sync_hamming_with_remaps(rt->synctest20, P25P2_SYNC, INV_P25P2_SYNC, 20);
        int ham_p2_scaled = (ham_p2 * 24 + 19) / 20;
        if (ham_p2_scaled < best_qpsk_ham || !compared) {
            best_qpsk_ham = ham_p2_scaled;
        }
        compared = 1;
    }
    if (!compared) {
        return;
    }
    int ham_qpsk_cur = atomic_load(&g_ham_qpsk_recent);
    if (best_qpsk_ham < ham_qpsk_cur) {
        atomic_store(&g_ham_qpsk_recent, best_qpsk_ham);
    }
}

int
frame_sync_best_ham_for_patterns(const char* symbols, const char* const patterns[], int pattern_count, int pattern_len,
                                 int best_start) {
    int best = best_start;
    for (int p = 0; p < pattern_count; p++) {
        int ham = frame_sync_hamming_distance_pattern(symbols, patterns[p], pattern_len);
        if (ham < best) {
            best = ham;
        }
    }
    return best;
}

int
frame_sync_best_nxdn_scaled_ham(const char* symbols10, int best_start) {
    const char* nxdn_patterns[] = {"3131331131", "1313113313"};
    int best = best_start;
    for (int p = 0; p < 2; p++) {
        int ham = frame_sync_hamming_distance_pattern(symbols10, nxdn_patterns[p], 10);
        int scaled_ham = (ham * 24 + 9) / 10;
        if (scaled_ham < best) {
            best = scaled_ham;
        }
    }
    return best;
}

static int
frame_sync_dmr_gfsk_ham(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if (state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4 || opts->frame_dmr != 1
        || (rt->ready_windows & FRAME_SYNC_WINDOW_24) == 0) {
        return 24;
    }
    const char* dmr_patterns[] = {DMR_BS_DATA_SYNC, DMR_BS_VOICE_SYNC, DMR_MS_DATA_SYNC, DMR_MS_VOICE_SYNC};
    return frame_sync_best_ham_for_patterns(rt->synctest, dmr_patterns, 4, 24, 24);
}

static int
frame_sync_dpmr_gfsk_ham(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if (state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_2400_4 || opts->frame_dpmr != 1
        || (rt->ready_windows & FRAME_SYNC_WINDOW_24) == 0) {
        return 24;
    }
    const char* dpmr_patterns[] = {DPMR_FRAME_SYNC_1, DPMR_FRAME_SYNC_4, INV_DPMR_FRAME_SYNC_1, INV_DPMR_FRAME_SYNC_4};
    return frame_sync_best_ham_for_patterns(rt->synctest, dpmr_patterns, 4, 24, 24);
}

static int
frame_sync_nxdn_gfsk_ham(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if ((rt->ready_windows & FRAME_SYNC_WINDOW_10) == 0) {
        return 24;
    }
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && opts->frame_nxdn96 == 1) {
        return frame_sync_best_nxdn_scaled_ham(rt->synctest10, 24);
    }
    if (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4 && opts->frame_nxdn48 == 1) {
        return frame_sync_best_nxdn_scaled_ham(rt->synctest10, 24);
    }
    return 24;
}

static void
frame_sync_update_gfsk_hamming(const dsd_opts* opts, const dsd_state* state, const frame_sync_runtime_ctx* rt) {
    if (opts->mod_cli_lock) {
        return;
    }

    int best_gfsk_ham = frame_sync_dmr_gfsk_ham(opts, state, rt);
    int candidate_ham = frame_sync_dpmr_gfsk_ham(opts, state, rt);
    if (candidate_ham < best_gfsk_ham) {
        best_gfsk_ham = candidate_ham;
    }
    candidate_ham = frame_sync_nxdn_gfsk_ham(opts, state, rt);
    if (candidate_ham < best_gfsk_ham) {
        best_gfsk_ham = candidate_ham;
    }
    int ham_gfsk_cur = atomic_load(&g_ham_gfsk_recent);
    if (best_gfsk_ham < ham_gfsk_cur) {
        atomic_store(&g_ham_gfsk_recent, best_gfsk_ham);
    }
}

#ifdef USE_RADIO
static void
frame_sync_debug_sync_dmr(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt) {
    DSD_FPRINTF(stderr, "[SYNC] pattern=%s expect=%s\n", rt->synctest, P25P1_SYNC);
    if (opts->frame_dmr != 1) {
        return;
    }

    const char* best_name = NULL;
    int best_ham = dmr_best_sync_hamming(rt->synctest, &best_name);
    int rtl_sym_rate = 0;
    int rtl_levels = 0;
    (void)dsd_rtl_stream_metrics_hook_symbol_profile(&rtl_sym_rate, &rtl_levels, NULL);
    double snr_gfsk = dsd_rtl_stream_metrics_hook_snr_gfsk_db();
    DSD_FPRINTF(stderr,
                "[DMRDBG] best=%s ham=%d rf_mod=%d lock=%d mods(c/q/g)=%d/%d/%d "
                "rtl_profile=%d/%d pwr=%.1fdB sql=%.1fdB snr_gfsk=%.1fdB win=%.*s\n",
                best_name ? best_name : "none", best_ham, state->rf_mod, opts->mod_cli_lock, opts->mod_c4fm,
                opts->mod_qpsk, opts->mod_gfsk, rtl_sym_rate, rtl_levels, pwr_to_dB(opts->rtl_pwr),
                pwr_to_dB(opts->rtl_squelch_level), snr_gfsk, 24, rt->synctest);
}

static void
frame_sync_debug_sync_cqpsk(const frame_sync_runtime_ctx* rt) {
    static const int d_rot_map[4] = {1, 3, 0, 2};
    int ham_norm = 0, ham_inv = 0, ham_ident = 0, ham_invert = 0, ham_swap = 0, ham_xor3 = 0, ham_rot = 0;
    for (int k = 0; k < 24; k++) {
        int d = (unsigned char)rt->synctest[k];
        if (d >= '0' && d <= '3') {
            d -= '0';
        }
        int expect_n = P25P1_SYNC[k] - '0';
        int expect_i = INV_P25P1_SYNC[k] - '0';
        ham_norm += (d != expect_n);
        ham_ident += (d != expect_n);
        ham_inv += (d != expect_i);
        int d_inv = (d == 0) ? 2 : (d == 1) ? 3 : (d == 2) ? 0 : 1;
        int d_swap = ((d & 1) << 1) | ((d & 2) >> 1);
        int d_xor3 = d ^ 0x3;
        int d_rot = d_rot_map[d & 0x3];
        ham_invert += (d_inv != expect_n);
        ham_swap += (d_swap != expect_n);
        ham_xor3 += (d_xor3 != expect_n);
        ham_rot += (d_rot != expect_n);
    }
    static int dbg_win = 0;
    if ((++dbg_win % 1200) == 0) {
        DSD_FPRINTF(stderr, "[SYNCDBG] ham(norm=%d inv=%d ident=%d inv2=%d swap=%d xor3=%d rot=%d) win=%.*s\n",
                    ham_norm, ham_inv, ham_ident, ham_invert, ham_swap, ham_xor3, ham_rot, 24, rt->synctest);
    }
}
#endif

static void
frame_sync_debug_sync_window(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt) {
#ifdef USE_RADIO
    static int debug_count = 0;
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init();
        cfg_dbg = dsd_neo_get_config();
    }
    int debug_sync = (cfg_dbg && cfg_dbg->debug_sync_enable) ? 1 : 0;
    int debug_cqpsk = (cfg_dbg && cfg_dbg->debug_cqpsk_enable) ? 1 : 0;

    if (debug_sync && (++debug_count % 4800) == 0) {
        frame_sync_debug_sync_dmr(opts, state, rt);
    }

    if (debug_cqpsk && state->rf_mod == 1) {
        frame_sync_debug_sync_cqpsk(rt);
    }
#else
    UNUSED2(opts, state);
    UNUSED(rt);
#endif
}

static int
frame_sync_eval_window(dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt, time_t now, double nowm) {
    /* Some matchers accept windows shorter than the profile's level ring. Estimate
     * from every sample gathered so far before one of those matchers can return. */
    if (rt->level_count > 0) {
        frame_sync_window_levels(opts, state, rt);
    }
    frame_sync_materialize_ready_windows(rt);
    if (frame_sync_should_skip_snr_or_power_gate(opts, state)) {
        return DSD_SYNC_NONE;
    }

    if ((rt->ready_windows & FRAME_SYNC_WINDOW_24) != 0) {
        frame_sync_debug_sync_window(opts, state, rt);
    }
    frame_sync_update_c4fm_hamming(opts, state, rt);
    frame_sync_update_qpsk_hamming(opts, state, rt);
    frame_sync_update_gfsk_hamming(opts, state, rt);

    frame_sync_match_ctx match_ctx = {
        .opts = opts,
        .state = state,
        .now = now,
        .nowm = nowm,
        .synctest_pos = rt->synctest_pos,
        .lmax = rt->lmax,
        .lmin = rt->lmin,
        .ready_windows = rt->ready_windows,
        .modulation = rt->modulation,
        .synctest = rt->synctest,
        .synctest8 = rt->synctest8,
        .synctest10 = rt->synctest10,
        .synctest12 = rt->synctest12,
        .synctest16 = rt->synctest16,
        .synctest20 = rt->synctest20,
        .synctest32 = rt->synctest32,
        .synctest48 = rt->synctest48,
    };
    return frame_sync_try_protocol_matches(&match_ctx);
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_frame_sync_test_history_window(const char* symbols, int symbol_count, int window_length, char* out, int out_size) {
    if (!symbols || symbol_count < 0 || !out || out_size < 0) {
        return 0;
    }
    static dsd_opts opts;
    static dsd_state state;
    frame_sync_runtime_ctx rt;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    frame_sync_runtime_init(&rt, &opts, &state);
    for (int i = 0; i < symbol_count; i++) {
        frame_sync_history_push(&rt, symbols[i]);
    }
    return frame_sync_history_materialize(&rt, window_length, out, (size_t)out_size);
}

int
dsd_frame_sync_test_try_protocol_matches(dsd_opts* opts, dsd_state* state, const char* symbols, int symbol_count) {
    if (!opts || !state || !symbols || symbol_count < 0) {
        return DSD_SYNC_NONE;
    }
    frame_sync_runtime_ctx rt;
    frame_sync_runtime_init(&rt, opts, state);
    for (int i = 0; i < symbol_count; i++) {
        frame_sync_history_push(&rt, symbols[i]);
    }
    frame_sync_materialize_ready_windows(&rt);
    frame_sync_match_ctx match_ctx = {
        .opts = opts,
        .state = state,
        .now = 0,
        .nowm = 0.0,
        .synctest_pos = symbol_count > 0 ? symbol_count - 1 : 0,
        .lmax = state->max,
        .lmin = state->min,
        .ready_windows = rt.ready_windows,
        .modulation = rt.modulation,
        .synctest = rt.synctest,
        .synctest8 = rt.synctest8,
        .synctest10 = rt.synctest10,
        .synctest12 = rt.synctest12,
        .synctest16 = rt.synctest16,
        .synctest20 = rt.synctest20,
        .synctest32 = rt.synctest32,
        .synctest48 = rt.synctest48,
    };
    return frame_sync_try_protocol_matches(&match_ctx);
}

int
dsd_frame_sync_test_eval_window(dsd_opts* opts, dsd_state* state, const char* symbols, const float* levels,
                                int symbol_count) {
    if (!opts || !state || !symbols || !levels || symbol_count < 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_runtime_ctx rt;
    frame_sync_runtime_init(&rt, opts, state);
    for (int i = 0; i < symbol_count; i++) {
        rt.lbuf[rt.lidx] = levels[i];
        if (rt.level_count < rt.t_max) {
            rt.level_count++;
        }
        rt.lidx = (rt.lidx + 1) % rt.t_max;
        frame_sync_history_push(&rt, symbols[i]);
    }
    rt.synctest_pos = symbol_count > 0 ? symbol_count - 1 : 0;
    return frame_sync_eval_window(opts, state, &rt, 0, 0.0);
}

#endif

static void
frame_sync_advance_sync_window(dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt) {
    /* One more symbol spent looking for a sync on this profile. Unlike synctest_pos this
     * lives in dsd_state, so returning a sync does not hand the profile a fresh budget. */
    if (state->sps_hunt_counter < INT_MAX) {
        state->sps_hunt_counter++;
    }
    if (rt->synctest_pos < 10200) {
        rt->synctest_pos++;
        return;
    }
    rt->synctest_pos = 0;
    dsd_frame_sync_hook_no_carrier(opts, state);
}

static int
frame_sync_sps_profile_has_candidate(const dsd_opts* opts, int profile_index) {
    if (!opts) {
        return 0;
    }
    switch (profile_index) {
        case DSD_FRAME_SYNC_SPS_PROFILE_4800_4: return frame_sync_opts_has_4800_four_level_mode(opts);
        case DSD_FRAME_SYNC_SPS_PROFILE_2400_4: return opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1;
        case DSD_FRAME_SYNC_SPS_PROFILE_9600_2: return opts->frame_provoice == 1;
        case DSD_FRAME_SYNC_SPS_PROFILE_6000_4: return opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1;
        case DSD_FRAME_SYNC_SPS_PROFILE_4800_2: return opts->frame_dstar == 1;
        default: return 0;
    }
}

int
frame_sync_sps_hunt_next_index(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    }
    int current = state->sps_hunt_idx;
    if (current < 0 || current >= DSD_FRAME_SYNC_SPS_PROFILE_COUNT) {
        current = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    }
    int next_idx = (current + 1) % DSD_FRAME_SYNC_SPS_PROFILE_COUNT;
    for (int tries = 0; tries < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; tries++) {
        if (frame_sync_sps_profile_has_candidate(opts, next_idx)) {
            return next_idx;
        }
        next_idx = (next_idx + 1) % DSD_FRAME_SYNC_SPS_PROFILE_COUNT;
    }
    return current;
}

static int
frame_sync_sps_hunt_next_index_matching_timing(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    }

    int current = state->sps_hunt_idx;
    if (current < 0 || current >= DSD_FRAME_SYNC_SPS_PROFILE_COUNT) {
        current = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    }
    if (state->samplesPerSymbol <= 0) {
        return current;
    }

    const int demod_rate = frame_sync_current_demod_rate(opts, state);
    int next_idx = (current + 1) % DSD_FRAME_SYNC_SPS_PROFILE_COUNT;
    for (int tries = 0; tries < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; tries++) {
        const frame_sync_sps_profile* profile = frame_sync_sps_profile_for_index(next_idx);
        const int expected_sps = dsd_opts_compute_sps_rate(opts, profile->symbol_rate_hz, demod_rate);
        if (frame_sync_sps_profile_has_candidate(opts, next_idx) && expected_sps == state->samplesPerSymbol) {
            return next_idx;
        }
        next_idx = (next_idx + 1) % DSD_FRAME_SYNC_SPS_PROFILE_COUNT;
    }
    return current;
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_frame_sync_test_sps_hunt_profile_count(void) {
    return DSD_FRAME_SYNC_SPS_PROFILE_COUNT;
}

int
dsd_frame_sync_test_sps_hunt_profile_rate(int profile_index) {
    return frame_sync_sps_profile_for_index(profile_index)->symbol_rate_hz;
}

int
dsd_frame_sync_test_sps_hunt_profile_levels(int profile_index) {
    return frame_sync_sps_profile_for_index(profile_index)->levels;
}

int
dsd_frame_sync_test_sps_hunt_profile_has_candidate(const dsd_opts* opts, int profile_index) {
    return frame_sync_sps_profile_has_candidate(opts, profile_index);
}

#endif

static void
frame_sync_apply_sps_profile_timing(const dsd_opts* opts, dsd_state* state, const frame_sync_sps_profile* profile) {
    /* Locked modulation modes may also carry manual timing, notably the experimental -m3 path. */
    if (opts->mod_cli_lock && state->samplesPerSymbol > 0 && state->symbolCenter >= 0
        && state->symbolCenter < state->samplesPerSymbol) {
        return;
    }

    int demod_rate = frame_sync_current_demod_rate(opts, state);
    int sym_rate = profile->symbol_rate_hz;
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    if (opts->verbose > 1 && !dsd_frame_sync_suppress_tcp_no_signal_console(opts, state)) {
        DSD_FPRINTF(stderr, "SPS hunt: trying %d sps (sym=%d, demod=%d)\n", state->samplesPerSymbol, sym_rate,
                    demod_rate);
    }
}

/**
 * @brief Modulation a profile comes up on when the hunt normalises it.
 *
 * Four-level profiles default to C4FM, which is the right guess for a profile the hunt has
 * learned nothing about. Once a P25p1 NID has decoded through the CQPSK chain, though, the
 * 4800/4-level profile has been told which modulation the signal actually uses, and rotating
 * away and back must not throw that away -- otherwise an LSM control channel loses the chain
 * it just decoded on every time the hunt visits another protocol's profile.
 */
static int
frame_sync_sps_profile_normalized_modulation(const dsd_opts* opts, const dsd_state* state,
                                             const frame_sync_sps_profile* profile, int profile_index) {
    const int requested = (profile_index == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && opts->frame_p25p1 == 1
                           && state->p25_p1_validated_rf_mod == 1)
                              ? 1
                              : 0;
    return dsd_frame_sync_profile_modulation(profile->levels, requested);
}

/**
 * @brief Adopt a profile's modulation and start the vote state over for it.
 *
 * The votes and hamming counters belong to the acquisition that just ended, so they are always
 * cleared. The QPSK dwell is different: it is the grace period a modulation gets before the
 * votes may argue with it, and clearing it under a restored CQPSK would let the SNR bias and
 * the three-vote C4FM re-entry evict that modulation before the first sync of the new dwell
 * could even arrive.
 */
static void
frame_sync_normalize_profile_modulation(dsd_state* state, int normalize, int modulation) {
    if (normalize) {
        state->rf_mod = modulation;
    }
    dsd_frame_sync_reset_mod_state();
    if (normalize && modulation == 1) {
        atomic_store(&g_qpsk_dwell_enter_ms, (int)(uint32_t)dsd_time_monotonic_ms());
    }
}

void
frame_sync_apply_sps_hunt_profile(const dsd_opts* opts, dsd_state* state, int next_idx, int preserve_modulation) {
    if (!opts || !state || next_idx < 0 || next_idx >= DSD_FRAME_SYNC_SPS_PROFILE_COUNT) {
        return;
    }

    const frame_sync_sps_profile* profile = frame_sync_sps_profile_for_index(next_idx);
    const int profile_changed = next_idx != state->sps_hunt_idx;
    const int profile_default_modulation = frame_sync_sps_profile_normalized_modulation(opts, state, profile, next_idx);
    const int normalize_profile_modulation = !preserve_modulation && !opts->mod_cli_lock
                                             && state->rf_mod != profile_default_modulation
                                             && (profile_changed || profile->levels == 2);
    if (!profile_changed && !normalize_profile_modulation) {
        return;
    }

    state->sps_hunt_idx = next_idx;
    if (profile_changed) {
        /* The dwell belongs to the profile, so adopting one starts its budget over. That is
         * the defect: left alone, the incoming profile inherits whatever the outgoing one had
         * already spent, and one that arrives a symbol short of the dwell is stepped off again
         * on the very next symbol, because frame_sync_advance_sync_window() bills one symbol
         * per symbol (#394).
         *
         * The measurement anchor moves with the budget so the pair stays consistent, not
         * because it is stale today: every path that reaches here already has a current mark.
         * getFrameSync() runs frame_sync_sps_hunt_note_handler_consumption() -- which
         * re-anchors -- immediately before frame_sync_ensure_enabled_sps_profile(), and the
         * hunt's own call site leaves getFrameSync() through frame_sync_sps_hunt_mark_return()
         * at the same symbolcnt. A fresh budget paired with an anchor taken under the outgoing
         * profile is what would let frame_sync_sps_hunt_note_handler_consumption() credit the
         * new profile for symbols a handler consumed before it was selected, so the two are
         * written together rather than left to each caller to keep in step.
         *
         * This sits here rather than at the call sites so it covers every path through the
         * helper: the hunt itself (which zeroes the counter just before calling), both adopt
         * paths in frame_sync_ensure_enabled_sps_profile(), and any future caller. The guard is
         * load-bearing -- a call that leaves the index alone can still get past the early-out
         * above to normalise a two-level profile's modulation, and that is the same profile
         * spending the same dwell, so its budget must survive. */
        state->sps_hunt_counter = 0;
        state->sps_hunt_symbolcnt_mark = state->symbolcnt;
    }
    frame_sync_normalize_profile_modulation(state, normalize_profile_modulation, profile_default_modulation);

    if (profile_changed) {
        frame_sync_apply_sps_profile_timing(opts, state, profile);
    }

#ifdef USE_RADIO
    rtl_maybe_apply_demod_profile(opts, state, profile);
#endif
}

static int
frame_sync_sps_profile_matching_timing(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || state->samplesPerSymbol <= 0) {
        return -1;
    }

    const int demod_rate = frame_sync_current_demod_rate(opts, state);
    int matching_profile = -1;
    int matching_level_profile = -1;
    const int current_levels = frame_sync_sps_profile_for_index(state->sps_hunt_idx)->levels;
    for (int profile_index = 0; profile_index < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; profile_index++) {
        if (!frame_sync_sps_profile_has_candidate(opts, profile_index)) {
            continue;
        }
        const frame_sync_sps_profile* profile = frame_sync_sps_profile_for_index(profile_index);
        const int expected_sps = dsd_opts_compute_sps_rate(opts, profile->symbol_rate_hz, demod_rate);
        if (expected_sps != state->samplesPerSymbol) {
            continue;
        }
        if (profile_index == state->sps_hunt_idx) {
            return profile_index;
        }
        if (matching_profile < 0) {
            matching_profile = profile_index;
        }
        if (matching_level_profile < 0 && profile->levels == current_levels) {
            /* Shared-rate profiles can have identical timing; retain the current symbol-level selection. */
            matching_level_profile = profile_index;
        }
    }
    return matching_level_profile >= 0 ? matching_level_profile : matching_profile;
}

void
frame_sync_ensure_enabled_sps_profile(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    const int timing_profile = frame_sync_sps_profile_matching_timing(opts, state);
    if (timing_profile >= 0 && timing_profile != state->sps_hunt_idx) {
        /* Presets may select both timing and modulation before frame sync starts. */
        frame_sync_apply_sps_hunt_profile(opts, state, timing_profile, 1);
    }
    if (frame_sync_sps_profile_has_candidate(opts, state->sps_hunt_idx)) {
        frame_sync_apply_sps_hunt_profile(opts, state, state->sps_hunt_idx, 0);
        return;
    }
    for (int profile_index = 0; profile_index < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; profile_index++) {
        if (frame_sync_sps_profile_has_candidate(opts, profile_index)) {
            frame_sync_apply_sps_hunt_profile(opts, state, profile_index, 0);
            return;
        }
    }
}

/** @brief Symbols a profile is owed before the hunt may leave it. */
static int
frame_sync_sps_hunt_dwell_symbols(const dsd_opts* opts, const dsd_state* state) {
    int passes = dsd_frame_sync_sps_hunt_dwell_passes(opts, state);
    if (passes < 1) {
        passes = 1;
    }
    return passes * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
}

/**
 * @brief Debit what the last frame handler consumed from the hunt's budget.
 *
 * Called on entry to getFrameSync(), where dsd_state::symbolcnt has advanced by exactly the
 * symbols one protocol handler took since the previous return. The budget is a net count of
 * symbols the search burned that no frame handler claimed. A profile carrying traffic sits
 * at zero -- its handlers take a frame's worth between syncs that arrive a few symbols
 * apart -- while a profile matching nothing spends its dwell at one symbol per symbol.
 *
 * Credit is per sync and floored at DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS. A handler that returns
 * inside that did not decode a frame; it recognised a marker and bailed, and buys nothing.
 * The floor is what keeps the rule independent of how often an unproductive matcher fires.
 * Crediting an accumulator instead made the dwell a function of that cadence -- a matcher
 * firing every few symbols, the shape an alternating bit-sync run on any other protocol
 * presents, banked a refund faster than the search could spend the budget and pinned the
 * profile for good (#388).
 *
 * Size alone cannot separate a decoded frame from a block skipped on a sync no CRC would
 * accept, so a handler that ran a check and failed it says so: processFrame() leaves its
 * dsd_frame_verdict in dsd_state::sps_hunt_last_frame_verdict, and an unproductive one is
 * refused the debit however much it took (#391). Where the check is per transmission rather
 * than per frame the protocol reports a sticky verdict instead, so a frame that fails its own
 * check inside a confirmed transmission still counts -- D-STAR voice and ProVoice were the
 * last two with no verdict at any point and now confirm this way (#421). The verdict is
 * default-productive, so every handler that still reports nothing -- DMR, P25 Phase 2, dPMR
 * and X2-TDMA, which is the whole of the set that does not -- buys dwell in proportion to
 * what it swallowed, as before.
 *
 * Consumption credit answers "how much of this profile's time went somewhere", which is not
 * the same question as "is this the right profile". A P25p1 control channel reads 134
 * symbols of a ~180-symbol TSDU slot -- it stops on the standard's last-block bit, not on a
 * budget -- so a decoded frame cannot get ahead of the slot it sat in, and the floor at zero
 * denies it a reserve to spend on the failures between. Runs of failing NIDs then rotated
 * the hunt off a control channel it was decoding (#400). A handler whose own check proves
 * the profile says so with DSD_FRAME_VERDICT_PROFILE_PROVEN, and the dwell restarts outright
 * -- the same thing a profile change does (#415), and for the same reason: this profile is
 * starting its dwell over, not being paid for a frame.
 *
 * Restarting cannot re-open #388 the way an accumulator did. Zero is the floor the credit
 * path already has, so nothing banks: holding a profile takes evidence recurring inside every
 * dwell, and one false proof buys exactly one dwell. Values are compared as literals because
 * the DSP layer includes no engine headers; anything this does not recognise is refused
 * credit, so an unhandled verdict degrades toward rotating rather than toward pinning.
 */
static void
frame_sync_sps_hunt_note_handler_consumption(dsd_state* state) {
    /* DSD_FRAME_VERDICT_* (engine/protocol_dispatch.h): 0 productive, 1 unproductive,
     * 2 profile proven. Kept in step by FRAME_SYNC_SPS_HUNT_FALSE_SYNC, which drives this
     * through getFrameSync() with the enumerators themselves. */
    const int verdict = state->sps_hunt_last_frame_verdict;
    const int proven = verdict == 2;
    const int unproductive = verdict != 0 && !proven;
    /* One verdict answers for one handler call. processFrame() already re-stamps the field
     * on every dispatch, so this is belt and braces for the entries no handler precedes --
     * a no-sync return, or a frame the retune generation made undispatchable -- where a
     * stale verdict would be read against consumption that is not the handler's. */
    state->sps_hunt_last_frame_verdict = 0;

    /* Both operands wrap at 2^32, so this modular difference stays exact when the free-running
     * symbol counter rolls over mid-measurement. */
    uint32_t consumed = state->symbolcnt - state->sps_hunt_symbolcnt_mark;
    if (consumed > (uint32_t)INT_MAX) {
        /* symbolcnt went backwards, so an unrelated subsystem zeroed it rather than a
         * handler having consumed 4 billion symbols: nxdn_reset_after_cac_fail(),
         * initState() on an in-process restart, and print_datascope() on each refresh.
         * Credit nothing; the mark below re-anchors the measurement on the next call.
         *
         * The datascope is the only one of the three that can zero it faster than handlers
         * consume -- once every opts->ssize symbols after the count passes
         * 4800/opts->scoperate, so a few hundred symbols apart -- which means an interval
         * that really did decode a frame can straddle a reset and be credited nothing.
         * That is accepted here rather than worked around: nothing in the tree ever sets
         * opts->datascope to 1, so the display is unreachable and the overlap is latent,
         * and a missed credit costs only the debit, leaving the hunt on the undebited
         * dwell it had before #390 rather than mis-crediting anything. Wiring the
         * datascope back to a switch means giving its reset a form this can tell apart
         * from a rollover. */
        consumed = 0;
    }
    if (proven) {
        /* The handler's check, not the measurement, is what carries this: a proof holds
         * however few symbols the frame took to read, so the size floor and the backwards-
         * jump guard above -- both of which exist to keep consumption honest -- have no say
         * in it. */
        state->sps_hunt_counter = 0;
    } else if (!unproductive && consumed >= (uint32_t)DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS) {
        /* dsd_state::sps_hunt_counter only ever counts up from zero, so this is exact.
         * Sites outside the hunt park a profile by zeroing it; the floor at zero means
         * consumption measured across such a reset cannot drive it negative. */
        const uint32_t counter = (uint32_t)state->sps_hunt_counter;
        state->sps_hunt_counter = (int)(consumed >= counter ? 0U : counter - consumed);
    }
    state->sps_hunt_symbolcnt_mark = state->symbolcnt;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_frame_sync_test_sps_hunt_note_handler_consumption(dsd_state* state) {
    frame_sync_sps_hunt_note_handler_consumption(state);
}
#endif

/** @brief Remember where the handler starts consuming, so the next call can measure it. */
static void
frame_sync_sps_hunt_mark_return(dsd_state* state) {
    state->sps_hunt_symbolcnt_mark = state->symbolcnt;
}

/**
 * @brief Spend alternate 4800/4-level dwells watching for P25p1 on the CQPSK chain.
 *
 * A P25p1 FDMA signal cannot argue its way onto CQPSK passively. Both passive routes are
 * structurally closed: while the CQPSK chain is off the front end runs the FM discriminator, so
 * the QPSK SNR estimate comes from a constellation ring fed raw un-derotated, un-timed IQ and
 * never clears the entry margin; and the sync-hamming candidate scores a superset of the C4FM
 * hypotheses, so an LSM signal -- which decodes its sync words unrotated through the
 * discriminator -- ties rather than wins, and the override only moves on a strict improvement.
 *
 * So the decision is made by trying it. This runs only where a dwell has already expired with
 * nothing productive decoded, which is exactly where the hunt was going to leave this profile
 * anyway: a control channel that is decoding never gets here, because its handlers keep the
 * dwell counter spent. Whichever chain then starts decoding P25p1 frames keeps the profile,
 * because frame_sync_hold_validated_p25p1_modulation() stops the votes from arguing with it.
 */
static void
frame_sync_maybe_probe_p25p1_cqpsk(const dsd_opts* opts, dsd_state* state, int preserve_modulation,
                                   int expired_dwell_idx, int expired_dwell_rf_mod) {
    if (preserve_modulation || opts->mod_cli_lock || opts->frame_p25p1 != 1) {
        return;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    /* The dwell that just expired decoded nothing. If that dwell was this profile running on
     * the CQPSK chain then whatever evidence put it there has been disproved, so withdraw it
     * rather than let the restore keep handing the profile back to a chain producing nothing.
     * This judges the dwell that ended, not the one about to start. */
    if (expired_dwell_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && expired_dwell_rf_mod == 1
        && state->p25_p1_validated_rf_mod == 1) {
        state->p25_p1_validated_rf_mod = -1;
    }
    if (state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4) {
        return;
    }
    if (state->p25_p1_validated_rf_mod == 1) {
        return;
    }

    /* Own both directions. A profile the hunt re-enters at its own index is not normalised --
     * frame_sync_apply_sps_hunt_profile() returns early when neither the index nor a binary
     * profile forces its hand -- so without handing the chain back here a probe that decoded
     * nothing would keep it for good. */
    const int probe_qpsk = state->p25_p1_mod_probe_next_qpsk ? 1 : 0;
    state->p25_p1_mod_probe_next_qpsk = !probe_qpsk;
    if (state->rf_mod == probe_qpsk) {
        return;
    }

    state->rf_mod = probe_qpsk;
    state->p25_p1_mod_probe_active = 1;
    if (probe_qpsk) {
        atomic_store(&g_qpsk_dwell_enter_ms, (int)(uint32_t)dsd_time_monotonic_ms());
    }
#ifdef USE_RADIO
    rtl_maybe_apply_active_demod_profile(opts, state);
#endif
}

int
frame_sync_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state) {
    if (state->sps_hunt_counter < frame_sync_sps_hunt_dwell_symbols(opts, state)) {
        return 0;
    }
    state->sps_hunt_counter = 0;
    /* Reaching here is the trial's verdict: its dwell expired with nothing decoded. */
    state->p25_p1_mod_probe_active = 0;
    const int preserve_modulation = opts->mod_cli_lock ? 1 : 0;

    /* Generic modulation locks retain their demodulator while rotating equal-timing protocol gates. A P25p2-specific
     * lock retains whichever P25 profile was selected explicitly by the helper or by a CC retune. This keeps a known
     * FDMA return on profile 0 even when 4800 and 6000 symbols/s round to the same timing. */
    const int pin_selected_p25_profile = preserve_modulation && opts->mod_p25p2_profile_lock == 1
                                         && opts->frame_p25p2 == 1
                                         && (state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4
                                             || state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4)
                                         && frame_sync_sps_profile_has_candidate(opts, state->sps_hunt_idx);
    int next_idx = pin_selected_p25_profile
                       ? state->sps_hunt_idx
                       : (preserve_modulation ? frame_sync_sps_hunt_next_index_matching_timing(opts, state)
                                              : frame_sync_sps_hunt_next_index(opts, state));
    const int previous_idx = state->sps_hunt_idx;
    const int previous_mod = state->rf_mod;
    frame_sync_apply_sps_hunt_profile(opts, state, next_idx, preserve_modulation);
    frame_sync_maybe_probe_p25p1_cqpsk(opts, state, preserve_modulation, previous_idx, previous_mod);
    /* A repeated binary profile is a step too: frame_sync_apply_sps_hunt_profile() normalises
     * dsd_state::rf_mod and resets the modulation vote state without the index moving, and the
     * caller's sync window -- its level ring, its min/max, its history -- was captured under the
     * old modulation. Say so, so the window is rebuilt rather than matched across the change.
     * The two comparisons are exact: the helper mutates nothing else once it is past its own
     * no-op guard. */
    return state->sps_hunt_idx != previous_idx || state->rf_mod != previous_mod;
}

double
frame_sync_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp) {
    if (mono_stamp > 0.0) {
        return nowm - mono_stamp;
    }
    if (wall_stamp != 0) {
        return (double)(now - wall_stamp);
    }
    return 1e9;
}

void
frame_sync_p25_slot_activity(const dsd_opts* opts, const dsd_state* state, time_t now, double nowm, double mac_hold,
                             double ring_hold, double dt, int* left_active, int* right_active) {
    double l_dmac =
        frame_sync_elapsed_seconds(nowm, now, state->p25_p2_last_mac_active_m[0], state->p25_p2_last_mac_active[0]);
    double r_dmac =
        frame_sync_elapsed_seconds(nowm, now, state->p25_p2_last_mac_active_m[1], state->p25_p2_last_mac_active[1]);
    int l_ring = (state->p25_p2_audio_ring_count[0] > 0) && (l_dmac <= ring_hold);
    int r_ring = (state->p25_p2_audio_ring_count[1] > 0) && (r_dmac <= ring_hold);
    int left_has_audio = state->p25_p2_audio_allowed[0] || l_ring;
    int right_has_audio = state->p25_p2_audio_allowed[1] || r_ring;
    if (dt >= opts->trunk_hangtime) {
        left_has_audio = l_ring;
        right_has_audio = r_ring;
    }
    *left_active = left_has_audio || (l_dmac <= mac_hold);
    *right_active = right_has_audio || (r_dmac <= mac_hold);
}

static void
frame_sync_no_sync_try_p25_release(dsd_opts* opts, dsd_state* state, time_t now) {
    if (!(opts->trunk_enable == 1 && opts->trunk_is_tuned == 1)) {
        return;
    }
    double fallback_nowm = dsd_time_now_monotonic_s();
    double dt = frame_sync_elapsed_seconds(fallback_nowm, now, state->last_vc_sync_time_m, state->last_vc_sync_time);
    double dt_since_tune =
        frame_sync_elapsed_seconds(fallback_nowm, now, state->p25_last_vc_tune_time_m, state->p25_last_vc_tune_time);

    const dsdneoRuntimeConfig* cfg_hold = dsd_neo_get_config();
    if (!cfg_hold) {
        dsd_neo_config_init();
        cfg_hold = dsd_neo_get_config();
    }
    double vc_grace = cfg_hold ? cfg_hold->p25_vc_grace_s : 0.75;
    int is_p2_vc = (state->p25_p2_active_slot != -1);
    double ring_hold = cfg_hold ? cfg_hold->p25_ring_hold_s : 0.75;
    double mac_hold = cfg_hold ? cfg_hold->p25_mac_hold_s : 0.75;
    int left_active = 0;
    int right_active = 0;
    frame_sync_p25_slot_activity(opts, state, now, fallback_nowm, mac_hold, ring_hold, dt, &left_active, &right_active);
    int both_slots_idle = (!is_p2_vc) ? 1 : !(left_active || right_active);
    if (dt >= opts->trunk_hangtime && both_slots_idle && dt_since_tune >= vc_grace) {
        state->p25_sm_force_release = 1;
        dsd_frame_sync_hook_p25_sm_release(opts, state);
    }
}

/**
 * @brief The accounting every no-sync return from getFrameSync() owes, in contract order.
 *
 * The P25 trunk SM counts VC no-sync passes and decides releases off these, and expects
 * the VC no-sync note before the no-carrier teardown (the ordering is asserted in
 * tests/dsp/test_frame_sync_internal_helpers.c). Both no-sync exits -- the timeout and
 * the SPS hunt's budget exit -- go through here so they cannot drift apart (#393).
 */
static void
frame_sync_no_sync_run_hooks(dsd_opts* opts, dsd_state* state, time_t now) {
    dsd_frame_sync_hook_p25_sm_vc_no_sync(opts, state);
    frame_sync_no_sync_try_p25_release(opts, state, now);
    dsd_frame_sync_hook_no_carrier(opts, state);
}

/**
 * @brief Give up on this call once a whole matchless pass has gone by.
 *
 * The dwell is the only gate: no protocol gets a carve-out here. A
 * `lastsynctype == DSD_SYNC_P25P1_NEG` term used to short-circuit the whole timeout,
 * inherited unexplained from upstream DSD, which let an inverted P25p1 sync hold off
 * this exit -- and only this one -- until the sync window's 10200-symbol wrap fired
 * no_carrier and cleared lastsynctype (#389).
 */
static int
frame_sync_handle_no_sync_timeout(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt, time_t now) {
    if (rt->synctest_pos < DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS) {
        return 0;
    }

    if ((opts->errorbars == 1) && (opts->verbose > 1) && (state->carrier == 1)
        && !dsd_frame_sync_suppress_tcp_no_signal_console(opts, state)) {
        DSD_FPRINTF(stderr, "Sync: no sync\n");
    }

    (void)frame_sync_no_sync_sps_hunt(opts, state);
    frame_sync_no_sync_run_hooks(opts, state, now);
    return 1;
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_frame_sync_test_handle_no_sync_timeout(dsd_opts* opts, dsd_state* state, int synctest_pos) {
    frame_sync_runtime_ctx rt = {0};
    rt.synctest_pos = synctest_pos;
    return frame_sync_handle_no_sync_timeout(opts, state, &rt, time(NULL));
}
#endif

/* Symbols seen since the last sync or unsynced dump; only the DSP thread
 * touches it (same pattern as the g_vote_* diagnostics). */
static unsigned int g_unsynced_dmr_dump_symbols = 0;

/* --dmr-debug-unsynced: while hunting, print the trailing 144 dibits from the
 * rolling DMR payload buffer as non-overlapping "Debug Demod -Sync" chunks.
 * Chunk boundaries are arbitrary and thresholds may be uncalibrated; this is a
 * best-effort raw view of demod output that never achieved sync. The first
 * chunk after the rolling buffer rewinds its write pointer can also span
 * stale pre-rewind dibits. */
static void
frame_sync_maybe_dump_unsynced_dmr(const dsd_opts* opts, const dsd_state* state) {
    if (opts->dmr_debug_unsynced == 0 || opts->frame_dmr != 1) {
        return;
    }
    if (opts->audio_in_type != AUDIO_IN_SYMBOL_BIN && opts->audio_in_type != AUDIO_IN_SYMBOL_FLT
        && state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4) {
        return;
    }
    if (++g_unsynced_dmr_dump_symbols < 144U) {
        return;
    }
    g_unsynced_dmr_dump_symbols = 0;

    if (state->dmr_payload_buf == NULL || state->dmr_payload_p == NULL
        || state->dmr_payload_p - state->dmr_payload_buf < 144) {
        return;
    }
    char line[192];
    if (dmr_debug_format_unsynced(line, sizeof(line), state->dmr_payload_p - 144, 144U) != 0U) {
        DSD_FPRINTF(stderr, "%s\n", line);
    }
}

int
getFrameSync(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();
    frame_sync_maybe_tick_p25_trunk_sm(opts, state, now);
    frame_sync_apply_cli_mod_lock(opts, state);
    frame_sync_sps_hunt_note_handler_consumption(state);
    frame_sync_ensure_enabled_sps_profile(opts, state);

    frame_sync_runtime_ctx rt;
    frame_sync_runtime_init(&rt, opts, state);

    frame_sync_publish_ui_throttled(opts, state);
    dsd_event_sync_slot(opts, state, 0);
    dsd_event_sync_slot(opts, state, 1);

    for (;;) {
        rt.t++;
        if ((rt.t % 300) == 0) {
            frame_sync_publish_ui_throttled(opts, state);
        }

        rt.symbol = getSymbol(opts, state, 0);
        frame_sync_update_symbol_ring(opts, state, rt.symbol, rt.lbuf, &rt.lidx, &rt.level_count, rt.t_max);
        frame_sync_maybe_auto_switch_modulation(opts, state, rt.t_max, &rt.lastt);
        rt.dibit = frame_sync_process_dibit_and_payload(opts, state, rt.symbol);
        frame_sync_history_push(&rt, (char)('0' + (rt.dibit & 0x3)));

        if (rt.history_count >= 8) {
            int sync_type = frame_sync_eval_window(opts, state, &rt, now, nowm);
            if (sync_type != DSD_SYNC_NONE) {
                g_unsynced_dmr_dump_symbols = 0;
                frame_sync_sps_hunt_mark_return(state);
                return sync_type;
            }
        }
        frame_sync_maybe_dump_unsynced_dmr(opts, state);

        if (dsd_exitflag_load() == 1) {
            dsd_request_shutdown(opts, state);
            frame_sync_sps_hunt_mark_return(state);
            return DSD_SYNC_NONE;
        }

        frame_sync_advance_sync_window(opts, state, &rt);
        if (frame_sync_handle_no_sync_timeout(opts, state, &rt, now)) {
            frame_sync_sps_hunt_mark_return(state);
            return -1;
        }

        /* The timeout above is the usual trigger, but it needs a whole matchless pass
         * inside one call. Syncs arriving more often than that -- the permissive 4800/4
         * matchers do, on signals belonging to another profile entirely -- would otherwise
         * keep the hunt from ever running. The budget spans calls, so they cannot.
         *
         * One pass is the smallest dwell frame_sync_sps_hunt_dwell_symbols() can return, so
         * the compare below is a necessary condition for the step and keeps the policy call
         * out of the per-symbol path.
         *
         * This is a no-sync exit like the timeout above and owes the P25 SM the same
         * accounting, in the same order. */
        if (state->sps_hunt_counter >= DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS
            && frame_sync_no_sync_sps_hunt(opts, state)) {
            frame_sync_no_sync_run_hooks(opts, state, now);
            frame_sync_sps_hunt_mark_return(state);
            return -1;
        }
    }
}
