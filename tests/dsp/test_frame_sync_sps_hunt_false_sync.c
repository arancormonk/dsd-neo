// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SPS hunt progress across sync returns (issue #388).
 *
 * Under AUTO, permissive matchers on the 4800/4 profile return isolated syncs that no
 * protocol ever turns into a frame. Each one used to pin the hunt twice over: it discarded
 * the per-call `synctest_pos` that armed the no-sync timeout, and it raised `state->carrier`,
 * which both blocked `frame_sync_no_sync_sps_hunt()` and zeroed the dwell counter. A signal
 * sitting on any other profile could then never be found.
 *
 * These cases drive `getFrameSync()` the way `live_scanner_*()` in src/engine/engine.c does:
 * every returned sync is "handled" by consuming symbols, exactly as a protocol dispatch
 * handler consumes dibits. What separates a false sync from a real one is how many symbols
 * the handler takes, so that is what the hunt is asked to measure.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/platform/sockets.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* An exact doubled M17 preamble: what AUTO accepts as a preamble marker, and the
 * shape an alternating bit-sync run on any other protocol presents. */
#define FALSE_SYNC_MARKER M17_PRE M17_PRE

/* Symbol source: `gap` filler symbols, then the marker, repeating. A zero-length
 * marker makes the stream pure filler (the idle case). */
static const char* g_marker = "";
static int g_marker_gap = 0;
static int g_stream_pos = 0;
static long g_symbols_emitted = 0;

static void
stream_reset(const char* marker, int gap) {
    g_marker = marker ? marker : "";
    g_marker_gap = gap;
    g_stream_pos = 0;
    g_symbols_emitted = 0;
}

static char
stream_next_dibit(void) {
    const int marker_len = (int)strlen(g_marker);
    const int period = g_marker_gap + marker_len;
    char dibit = '1';
    if (period > 0) {
        const int phase = g_stream_pos % period;
        if (phase >= g_marker_gap) {
            dibit = g_marker[phase - g_marker_gap];
        }
    }
    g_stream_pos++;
    g_symbols_emitted++;
    return dibit;
}

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
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol, const dsd_dibit_soft_t* soft) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
    (void)soft;
}

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    (void)st;
    (void)sym;
    return 255;
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

/* Mirrors the production slicer's bookkeeping: history push plus the symbol counter
 * that src/dsp/dsd_symbol.c bumps on every getSymbol() path. */
float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;

    const char dibit = stream_next_dibit();
    const float symbol = (dibit == '3') ? -3.0f : 3.0f;
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    return symbol;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_soft_buf);
    free(state->symbol_history);
    state->dibit_buf = NULL;
    state->dmr_payload_buf = NULL;
    state->dmr_soft_buf = NULL;
    state->symbol_history = NULL;
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000U, sizeof(dsd_dibit_soft_t));
    state->symbol_history = (float*)calloc(DSD_SYMBOL_HISTORY_SIZE, sizeof(float));
    if (!state->dibit_buf || !state->dmr_payload_buf || !state->dmr_soft_buf || !state->symbol_history) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    state->symbol_history_size = DSD_SYMBOL_HISTORY_SIZE;
    state->symbol_history_head = 0;
    state->symbol_history_count = 0;
    return 1;
}

/* AUTO (-fa): every digital candidate enabled, C4FM pinned so the run stays
 * deterministic (the hunt then rotates among equal-timing profiles). */
static void
init_auto_opts_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    opts->wav_decimator = 48000;
    opts->mod_cli_lock = 1;
    opts->mod_c4fm = 1;
    opts->msize = 1;
    opts->ssize = 128;
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

    state->rf_mod = 0;
    state->p25_p2_active_slot = -1;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;

    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const int demod_rate = dsd_opts_current_input_timing_rate(opts);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

typedef struct {
    const char* label;
    const char* marker;  /* sync marker injected into the stream ("" = never syncs) */
    int marker_gap;      /* filler symbols between markers */
    int handler_symbols; /* symbols the "protocol handler" consumes per returned sync */
    long symbol_budget;  /* stop after this many symbols leave the source */
    int expect_step;     /* 1 = the hunt must leave its starting profile */
} HuntCase;

typedef struct {
    int stepped;
    long symbols_at_step;
    int syncs_seen;
    int final_idx;
} HuntResult;

/* The engine's shape: getFrameSync() in a loop, each sync handed to a handler that
 * consumes symbols, until the hunt steps or the symbol budget runs out. */
static HuntResult
drive_hunt(const HuntCase* tc) {
    static dsd_opts opts;
    static dsd_state state;
    HuntResult result = {0, 0, 0, 0};

    init_auto_opts_state(&opts, &state);
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "%s: failed to allocate frame-sync state buffers\n", tc->label);
        return result;
    }
    const int start_idx = state.sps_hunt_idx;
    stream_reset(tc->marker, tc->marker_gap);
    dsd_frame_sync_reset_mod_state();

    while (g_symbols_emitted < tc->symbol_budget) {
        const int sync = getFrameSync(&opts, &state);
        if (state.sps_hunt_idx != start_idx) {
            result.stepped = 1;
            result.symbols_at_step = g_symbols_emitted;
            break;
        }
        if (sync == DSD_SYNC_NONE) {
            continue;
        }
        result.syncs_seen++;
        for (int i = 0; i < tc->handler_symbols; i++) {
            (void)getSymbol(&opts, &state, 1);
        }
    }

    result.final_idx = state.sps_hunt_idx;
    free_state_buffers(&state);
    return result;
}

/* #388: isolated syncs that no handler turns into a frame must not pin the hunt. */
static void
test_false_syncs_do_not_starve_the_hunt(void) {
    /* One marker per ~300 symbols is far more often than the 1800-symbol no-sync
     * timeout, so before the fix the timeout never armed and the hunt never ran. */
    const HuntCase tc = {
        .label = "false syncs at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 284,
        .handler_symbols = 8, /* what an M17 preamble costs: dispatch_m17.c skipDibit(8) */
        .symbol_budget = 40000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 10);
    assert(r.stepped == 1);
    assert(r.final_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    /* The hunt owes the profile one full dwell before it may leave, and must not
     * need much more than that. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 2);
}

/* The other side of the contract: a signal being decoded holds its profile. */
static void
test_consumed_frames_hold_the_profile(void) {
    /* Same marker, same cadence -- only the handler's appetite differs. A protocol
     * reading real frames consumes far more than it searches (NXDN96 spends 182
     * symbols per frame, P25p1 408, M17 184). */
    const HuntCase tc = {
        .label = "decoded frames at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 84,
        .handler_symbols = 400,
        .symbol_budget = 60000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 50);
    assert(r.stepped == 0);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
}

/* No signal at all: the rotation period is the one docs/cli.md documents. */
static void
test_idle_rotation_period_is_unchanged(void) {
    const HuntCase tc = {
        .label = "idle hunt",
        .marker = "",
        .marker_gap = 0,
        .handler_symbols = 0,
        .symbol_budget = 40000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen == 0);
    assert(r.stepped == 1);
    /* Three no-sync passes of 1800 symbols, as before this change. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols + 200);
}

int
main(void) {
    test_false_syncs_do_not_starve_the_hunt();
    test_consumed_frames_hold_the_profile();
    test_idle_rotation_period_is_unchanged();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
