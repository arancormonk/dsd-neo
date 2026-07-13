// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static size_t g_symbol_index;
static const char* g_sync_pattern = P25P2_SYNC;
static int g_symbol_rate_hz = 6000;

dsd_socket_t
Connect(char* hostname, int portno) { // NOLINT(misc-use-internal-linkage)
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
openAudioInput(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return -1;
}

int
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return 0;
}

void
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out,
                          size_t out_size) { // NOLINT(misc-use-internal-linkage)
    (void)timestamp;
    (void)format;
    return out ? DSD_SNPRINTF(out, out_size, "%s", "00:00:00") >= 0 : 0;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
dsd_mark_cc_sync(dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

double
raw_pwr_f(const float* samples, int len, int step) { // NOLINT(misc-use-internal-linkage)
    (void)samples;
    (void)len;
    (void)step;
    return 1.0;
}

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    (void)mean_power;
    return 0.0;
}

void
lpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
hpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
pbf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

static float
symbol_level_for_dibit(char dibit) {
    switch (dibit) {
        case '0': return 1.0f;
        case '1': return 3.0f;
        case '2': return -1.0f;
        case '3': return -3.0f;
        default: break;
    }
    return 3.0f;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U) {
        return -1;
    }

    const size_t pattern_len = strlen(g_sync_pattern);
    for (size_t i = 0; i < count; i++) {
        char dibit = (g_symbol_index < pattern_len) ? g_sync_pattern[g_symbol_index] : '1';
        out[i] = symbol_level_for_dibit(dibit);
        g_symbol_index++;
    }
    *out_got = (int)count;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 1.0;
}

static int
fake_output_kind(void) {
    return RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    return 0;
}

static int
build_raw_pattern_for_map(const char* corrected_pattern, uint8_t map_idx, char* out, size_t out_cap) {
    size_t len = strlen(corrected_pattern);
    if (!out || out_cap <= len) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (corrected_pattern[i] < '0' || corrected_pattern[i] > '3') {
            return 0;
        }
        uint8_t corrected = (uint8_t)(corrected_pattern[i] - '0');
        out[i] = (char)('0' + dsd_p25_cqpsk_raw_dibit_for_corrected(map_idx, corrected));
    }
    out[len] = '\0';
    return 1;
}

static int
llr_matches_bit(int16_t llr, int bit) {
    return bit ? (llr > 0) : (llr < 0);
}

static void
test_symbol_window_extrema_avg2(const float* samples, int count, float* out_min, float* out_max) {
    if (!samples || !out_min || !out_max || count < 2) {
        if (out_min) {
            *out_min = 0.0f;
        }
        if (out_max) {
            *out_max = 0.0f;
        }
        return;
    }

    float min1 = samples[0];
    float min2 = samples[1];
    if (min2 < min1) {
        float tmp = min1;
        min1 = min2;
        min2 = tmp;
    }

    float max1 = samples[0];
    float max2 = samples[1];
    if (max2 > max1) {
        float tmp = max1;
        max1 = max2;
        max2 = tmp;
    }

    for (int i = 2; i < count; i++) {
        float v = samples[i];
        if (v < min1) {
            min2 = min1;
            min1 = v;
        } else if (v < min2) {
            min2 = v;
        }

        if (v > max1) {
            max2 = max1;
            max1 = v;
        } else if (v > max2) {
            max2 = v;
        }
    }

    *out_min = (min1 + min2) * 0.5f;
    *out_max = (max1 + max2) * 0.5f;
}

static void
simulate_payload_threshold_update(const dsd_opts* opts, dsd_state* state, float payload_symbol) {
    int cap = opts->ssize;
    if (cap < 0) {
        cap = 0;
    }
    if (cap > (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]))) {
        cap = (int)(sizeof(state->sbuf) / sizeof(state->sbuf[0]));
    }
    if (cap > 0) {
        state->sbuf[state->sidx] = payload_symbol;
    }

    float lmin = 0.0f;
    float lmax = 0.0f;
    test_symbol_window_extrema_avg2(state->sbuf, cap, &lmin, &lmax);
    dsd_state_push_minmax_window(state, opts->msize, lmin, lmax);
    state->center = (state->max + state->min) / 2.0f;
    state->umid = (((state->max) - state->center) * 5.0f / 8.0f) + state->center;
    state->lmid = (((state->min) - state->center) * 5.0f / 8.0f) + state->center;
}

static uint32_t
fake_stream_generation(void) {
    return 1U;
}

static int
fake_stream_active(void) {
    return 1;
}

static int
fake_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 1;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 1;
    }
    return 0;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_reliab_buf);
    free(state->dmr_soft_buf);
    dsd_symbol_history_free(state);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_reliab_buf = (uint8_t*)calloc(1000000U, sizeof(uint8_t));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000U, sizeof(dsd_dibit_soft_t));
    if (!state->dibit_buf || !state->dmr_payload_buf || !state->dmr_reliab_buf || !state->dmr_soft_buf) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    if (dsd_symbol_history_init(state, DSD_SYMBOL_HISTORY_SIZE) != 0) {
        free_state_buffers(state);
        return 0;
    }
    return 1;
}

static int
run_p25_sync_case(const char* pattern, int frame_p25p1, int frame_p25p2, int expected_sync, uint8_t expected_map,
                  float expected_center, const char* label) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    g_symbol_index = 0U;
    if (!pattern) {
        DSD_FPRINTF(stderr, "%s sync pattern is null\n", label ? label : "P25 sync");
        return 1;
    }
    const size_t pattern_len = strlen(pattern);
    char* sync_pattern_copy = (char*)malloc(pattern_len + 1U);
    if (!sync_pattern_copy) {
        DSD_FPRINTF(stderr, "%s sync pattern copy allocation failed\n", label ? label : "P25 sync");
        return 1;
    }
    DSD_MEMCPY(sync_pattern_copy, pattern, pattern_len + 1U);
    g_sync_pattern = sync_pattern_copy;
    g_symbol_rate_hz = frame_p25p2 ? 6000 : 4800;
    dsd_frame_sync_reset_mod_state();

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        g_sync_pattern = P25P2_SYNC;
        free(sync_pattern_copy);
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dmr = 1;
    opts.frame_dpmr = 1;
    opts.frame_provoice = 1;
    opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.p25_trunk = 1;
    opts.msize = 1;
    opts.ssize = 36;

    state.rf_mod = 1;
    state.sps_hunt_idx = frame_p25p2 ? 3 : (frame_p25p1 ? 0 : state.sps_hunt_idx);
    state.p25_p2_active_slot = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .cqpsk_status = fake_cqpsk_status,
        .stream_active = fake_stream_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int sync = getFrameSync(&opts, &state);
    int rc = 0;
    if (sync != expected_sync) {
        DSD_FPRINTF(stderr, "%s returned %d, expected %d\n", label, sync, expected_sync);
        rc = 1;
    }
    if (state.p25_cqpsk_dibit_map_idx != dsd_p25_cqpsk_dibit_map_index(expected_map)) {
        DSD_FPRINTF(stderr, "%s selected map %u, expected %u\n", label, state.p25_cqpsk_dibit_map_idx,
                    dsd_p25_cqpsk_dibit_map_index(expected_map));
        rc = 1;
    }
    if (fabsf(state.center - expected_center) > 0.001f) {
        DSD_FPRINTF(stderr, "%s center %.3f, expected %.3f\n", label, state.center, expected_center);
        rc = 1;
    }
    float scanner_center = (state.max + state.min) / 2.0f;
    if (fabsf(scanner_center - expected_center) > 0.001f) {
        DSD_FPRINTF(stderr, "%s scanner center %.3f, expected %.3f\n", label, scanner_center, expected_center);
        rc = 1;
    }
    simulate_payload_threshold_update(&opts, &state, symbol_level_for_dibit('1'));
    if (fabsf(state.center - expected_center) > 0.001f) {
        DSD_FPRINTF(stderr, "%s payload update center %.3f, expected %.3f\n", label, state.center, expected_center);
        rc = 1;
    }

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    free_state_buffers(&state);
    g_sync_pattern = P25P2_SYNC;
    free(sync_pattern_copy);
    return rc;
}

static int
run_p25p2_sync_case(const char* pattern, int expected_sync, uint8_t expected_map, const char* label) {
    return run_p25_sync_case(pattern, 0, 1, expected_sync, expected_map, 0.0f, label);
}

static int
run_p25p1_sync_case(const char* pattern, int expected_sync, uint8_t expected_map, const char* label) {
    return run_p25_sync_case(pattern, 1, 0, expected_sync, expected_map, 0.0f, label);
}

static int
test_negative_cqpsk_dibit_polarity(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate CQPSK dibit state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p2 = 1;
    state.rf_mod = 1;
    state.synctype = DSD_SYNC_P25P2_NEG;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_X2400;

    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .cqpsk_status = fake_cqpsk_status,
        .stream_active = fake_stream_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int got = digitize(&opts, &state, 1.0f);
    const dsd_dibit_soft_t soft = state.dmr_soft_p[-1];
    int rc = 0;
    if (got != 1) {
        DSD_FPRINTF(stderr, "negative CQPSK rotated dibit returned %d, expected 1\n", got);
        rc = 1;
    }
    if (!llr_matches_bit(soft.llr[0], 0) || !llr_matches_bit(soft.llr[1], 1)) {
        DSD_FPRINTF(stderr, "negative CQPSK rotated soft signs mismatch (%d,%d)\n", soft.llr[0], soft.llr[1]);
        rc = 1;
    }

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    free_state_buffers(&state);
    return rc;
}

static int
test_cqpsk_map_is_p25_scoped(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate non-P25 CQPSK state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    state.rf_mod = 1;
    state.synctype = DSD_SYNC_X2TDMA_DATA_POS;
    state.lastsynctype = DSD_SYNC_X2TDMA_DATA_POS;
    state.center = 0.0f;
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_X2400;

    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .cqpsk_status = fake_cqpsk_status,
        .stream_active = fake_stream_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int got = digitize(&opts, &state, 3.0f);
    int rc = 0;
    if (got != 1) {
        DSD_FPRINTF(stderr, "non-P25 CQPSK dibit returned %d, expected raw threshold dibit 1\n", got);
        rc = 1;
    }

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    char rotated[32];

    rc |= run_p25p2_sync_case(P25P2_SYNC, DSD_SYNC_P25P2_POS, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY,
                              "P25P2 RTL positive sync");
    rc |= run_p25p2_sync_case(INV_P25P2_SYNC, DSD_SYNC_P25P2_NEG, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY,
                              "P25P2 RTL inverted sync");

    if (!build_raw_pattern_for_map(P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_X2400, rotated, sizeof(rotated))) {
        return 1;
    }
    rc |=
        run_p25p2_sync_case(rotated, DSD_SYNC_P25P2_POS, DSD_P25_CQPSK_DIBIT_MAP_X2400, "P25P2 RTL X2400 rotated sync");

    if (!build_raw_pattern_for_map(INV_P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_X2400, rotated, sizeof(rotated))) {
        return 1;
    }
    rc |= run_p25p2_sync_case(rotated, DSD_SYNC_P25P2_NEG, DSD_P25_CQPSK_DIBIT_MAP_X2400,
                              "P25P2 RTL X2400 rotated inverted sync");

    if (!build_raw_pattern_for_map(P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_N1200, rotated, sizeof(rotated))) {
        return 1;
    }
    rc |=
        run_p25p2_sync_case(rotated, DSD_SYNC_P25P2_POS, DSD_P25_CQPSK_DIBIT_MAP_N1200, "P25P2 RTL N1200 rotated sync");

    if (!build_raw_pattern_for_map(P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_P1200, rotated, sizeof(rotated))) {
        return 1;
    }
    rc |=
        run_p25p2_sync_case(rotated, DSD_SYNC_P25P2_POS, DSD_P25_CQPSK_DIBIT_MAP_P1200, "P25P2 RTL P1200 rotated sync");

    if (!build_raw_pattern_for_map(P25P1_SYNC, DSD_P25_CQPSK_DIBIT_MAP_X2400, rotated, sizeof(rotated))) {
        return 1;
    }
    rc |=
        run_p25p1_sync_case(rotated, DSD_SYNC_P25P1_POS, DSD_P25_CQPSK_DIBIT_MAP_X2400, "P25P1 RTL X2400 rotated sync");
    rc |= test_negative_cqpsk_dibit_polarity();
    rc |= test_cqpsk_map_is_p25_scoped();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
