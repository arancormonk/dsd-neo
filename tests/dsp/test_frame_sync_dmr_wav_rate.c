// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/sockets.h>
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

static size_t g_symbol_index;
static const char* g_sync_pattern = DMR_BS_DATA_SYNC;

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
symbol_level_for_dibit(char dibit) {
    return (dibit == '3') ? -3.0f : 3.0f;
}

float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;

    const size_t pattern_len = strlen(g_sync_pattern);
    char dibit = (g_symbol_index < pattern_len) ? g_sync_pattern[g_symbol_index] : '1';
    g_symbol_index++;

    float symbol = symbol_level_for_dibit(dibit);
    dmr_sample_history_push(state, symbol);
    return symbol;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_reliab_buf);
    free(state->dmr_soft_buf);
    dmr_sample_history_free(state);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_reliab_buf = (uint8_t*)calloc(1000000U, sizeof(uint8_t));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000U, sizeof(dsd_dibit_soft_t));
    if (dmr_sample_history_init(state) != 0 || !state->dibit_buf || !state->dmr_payload_buf || !state->dmr_reliab_buf
        || !state->dmr_soft_buf) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    return 1;
}

static int
run_dmr_wav_rate_case(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.wav_decimator = 48000;
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_gfsk = 1;
    opts.msize = 1;
    opts.ssize = 128;

    state.rf_mod = 2;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 9;
    state.p25_p2_active_slot = -1;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;
    state.lmid = -2.0f;
    state.umid = 2.0f;
    state.minref = -2.4f;
    state.maxref = 2.4f;

    g_symbol_index = 0U;
    g_sync_pattern = DMR_BS_DATA_SYNC;
    dsd_frame_sync_reset_mod_state();

    int sync = getFrameSync(&opts, &state);
    int rc = 0;
    if (sync != DSD_SYNC_DMR_BS_DATA_POS) {
        DSD_FPRINTF(stderr, "DMR WAV sync returned %d, expected %d\n", sync, DSD_SYNC_DMR_BS_DATA_POS);
        rc = 1;
    }
    if (state.samplesPerSymbol != 20 || state.symbolCenter != 9) {
        DSD_FPRINTF(stderr, "DMR WAV timing changed to sps=%d center=%d, expected 20/9\n", state.samplesPerSymbol,
                    state.symbolCenter);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    return run_dmr_wav_rate_case();
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
