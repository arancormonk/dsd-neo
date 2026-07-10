// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static size_t g_sample_index;
static const char* g_sync_pattern = M17_PRE;
static char g_fill_symbol = '1';

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
cleanupAndExit(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
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

void
getTimeC_buf(char out[9]) { // NOLINT(misc-use-internal-linkage)
    if (out) {
        DSD_SNPRINTF(out, 9, "%s", "00:00:00");
    }
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

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
}

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    (void)st;
    (void)sym;
    return 255;
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
symbol_level_for_m17(char dibit) {
    return (dibit == '3') ? -3.0f : 3.0f;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U) {
        return -1;
    }

    const size_t pattern_len = strlen(g_sync_pattern);
    const size_t samples_per_symbol = 10U;
    for (size_t i = 0; i < count; i++) {
        size_t symbol_index = g_sample_index / samples_per_symbol;
        char dibit = (symbol_index < pattern_len) ? g_sync_pattern[symbol_index] : g_fill_symbol;
        out[i] = symbol_level_for_m17(dibit);
        g_sample_index++;
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
    return RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    return 0;
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
        *out_cqpsk_enable = 0;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 0;
    }
    return 0;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_reliab_buf);
    free(state->dmr_soft_buf);
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
    return 1;
}

static void
install_hooks(void) {
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
}

static void
clear_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static int
init_m17_sync_case(dsd_opts* opts, dsd_state* state, int* fake_rtl_context) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    if (!init_state_buffers(state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        return 0;
    }

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 1;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 1;
    opts->frame_provoice = 1;
    opts->frame_ysf = 1;
    opts->frame_m17 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_gfsk = 1;
    opts->msize = 1;
    opts->ssize = 128;

    state->rf_mod = 2;
    state->p25_p2_active_slot = -1;
    state->rtl_ctx = (struct RtlSdrContext*)fake_rtl_context;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
    return 1;
}

static int
run_one_on_state(dsd_opts* opts, dsd_state* state, const char* pattern, int expected_sync, const char* label) {
    g_sample_index = 0U;
    g_sync_pattern = pattern;
    g_fill_symbol = (expected_sync < 0) ? '3' : '1';
    state->rtl_symbol_cache_pos = 0;
    state->rtl_symbol_cache_len = 0;
    state->rtl_symbol_cache_published_pending = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int sync = getFrameSync(opts, state);
    if (sync != expected_sync) {
        DSD_FPRINTF(stderr, "%s returned %d, expected %d\n", label, sync, expected_sync);
        return 1;
    }
    return 0;
}

static int
run_m17_sync_case(int initial_last, uint8_t initial_polarity, const char* pattern, int expected_sync,
                  const char* label) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    dsd_frame_sync_reset_mod_state();
    if (!init_m17_sync_case(&opts, &state, &fake_rtl_context)) {
        return 1;
    }
    state.lastsynctype = initial_last;
    state.m17_polarity = initial_polarity;

    install_hooks();
    const int rc = run_one_on_state(&opts, &state, pattern, expected_sync, label);
    clear_hooks();
    free_state_buffers(&state);
    return rc;
}

static int
run_m17_two_step_case(const char* first_pattern, int first_expected, const char* second_pattern, int second_expected,
                      const char* label) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    dsd_frame_sync_reset_mod_state();
    if (!init_m17_sync_case(&opts, &state, &fake_rtl_context)) {
        return 1;
    }

    install_hooks();
    int rc = run_one_on_state(&opts, &state, first_pattern, first_expected, label);
    if (rc == 0) {
        rc = run_one_on_state(&opts, &state, second_pattern, second_expected, label);
    }
    clear_hooks();
    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= run_m17_two_step_case(M17_PRE, DSD_SYNC_M17_PRE_POS, M17_LSF, DSD_SYNC_M17_LSF_POS, "M17 preamble to LSF");
    rc |= run_m17_sync_case(DSD_SYNC_M17_LSF_POS, 1U, M17_STR, DSD_SYNC_M17_STR_POS, "M17 LSF to stream");
    rc |= run_m17_sync_case(DSD_SYNC_M17_LSF_POS, 1U, M17_PKT, DSD_SYNC_M17_PKT_POS, "M17 LSF to packet");
    rc |= run_m17_two_step_case(M17_PRE, DSD_SYNC_M17_PRE_POS, M17_BRT, DSD_SYNC_M17_BRT_POS, "M17 preamble to BERT");
    rc |= run_m17_sync_case(DSD_SYNC_M17_BRT_POS, 1U, M17_BRT, DSD_SYNC_M17_BRT_POS, "M17 BERT to BERT");
    rc |= run_m17_sync_case(DSD_SYNC_M17_STR_POS, 1U, M17_STR, DSD_SYNC_M17_STR_POS, "M17 stream to stream");
    rc |= run_m17_sync_case(DSD_SYNC_M17_PKT_POS, 1U, M17_PKT, DSD_SYNC_M17_PKT_POS, "M17 packet to packet");
    rc |= run_m17_sync_case(DSD_SYNC_M17_STR_POS, 1U, M17_EOT, DSD_SYNC_M17_EOT_POS, "M17 stream to EOT");
    rc |= run_m17_sync_case(DSD_SYNC_NONE, 0U, M17_EOT, -1, "M17 rejects cold EOT");
    rc |= run_m17_two_step_case(M17_PRE, DSD_SYNC_M17_PRE_POS, M17_STR, -1, "M17 rejects stream after preamble");
    rc |= run_m17_two_step_case(M17_PRE, DSD_SYNC_M17_PRE_POS, M17_PKT, -1, "M17 rejects packet after preamble");
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
