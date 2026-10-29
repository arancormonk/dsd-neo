// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The frame sync's history across a no-sync exit (dsd_frame_sync_history_carry).
 *
 * getFrameSync() ends a call after a matchless pass of DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS symbols. The windows it
 * matches are built from a dibit history that used to live only as long as the call, so the next call started blind:
 * a burst that began before the exit was matched only if its training ended at least its window's length into the new
 * call. A TETRA control uplink burst is matched on a 79-symbol window that ends with its training, 59 symbols into the
 * burst, so one that began fewer than 20 symbols before the exit was never decoded. The uplink fixture lost burst 2009
 * that way: the exit came 7 symbols before it began (tetra_uplink_bursts_72k, DECODE_IQ_TETRA_UPLINK_CONTROL_BURSTS).
 *
 * These cases drive getFrameSync() as the engine does across such an exit, with a symbol source that holds a control
 * uplink burst straddling it: the next call goes on from the history the last one kept when the stream went on unbroken,
 * and starts blind when it did not: a carrier change, a trunk retune (in the exit's own no-sync hooks, as a P25 release
 * makes one, or between the calls, on the decoder thread or off it), a completed tune, a stream flush the next read
 * finds, a symbol read between the calls, another modulation, or another hunt profile.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

enum {
    STREAM_LEN = 4000,
    /* Where the control uplink burst begins: 10 symbols before the first call's matchless pass ends. */
    BURST_AT = DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 10,
    CB_TAIL = 2,
    CB_HALF = 42,
    CB_ETS = 15,
};

static char g_stream[STREAM_LEN];
static int g_pos = 0;
/* 1: the next read finds a stream flush, as an RTL reader does after a retune (dsd_symbol.c bumps the clock's epoch). */
static int g_flush_on_next_read = 0;
static char g_block1[CB_HALF];

/* Filler that runs 0, 1, 2, 3 over and over, which no training sequence matches in any rotation (each holds a dibit
   twice in a row, which a rotation keeps and the filler never has), at all four levels so the window's levels centre
   the slicer; and one control uplink burst at BURST_AT: tail, 42 dibits of block 1, the extended training sequence,
   block 2, tail. */
static void
stream_build(void) {
    for (int i = 0; i < STREAM_LEN; i++) {
        g_stream[i] = (char)('0' + (i & 3));
    }
    uint32_t seed = 0x2009u;
    int at = BURST_AT + CB_TAIL;
    for (int i = 0; i < CB_HALF; i++) {
        seed = (seed * 1103515245u) + 12345u;
        g_block1[i] = (char)('0' + ((seed >> 16) & 3u));
        g_stream[at + i] = g_block1[i];
    }
    at += CB_HALF;
    DSD_MEMCPY(g_stream + at, TETRA_CB_SYNC, CB_ETS);
    at += CB_ETS;
    for (int i = 0; i < CB_HALF; i++) {
        seed = (seed * 1103515245u) + 12345u;
        g_stream[at + i] = (char)('0' + ((seed >> 16) & 3u));
    }
    g_pos = 0;
}

/* The CQPSK slicer's levels for each dibit (frame_sync_slice_cqpsk_dibit(): 1 at +3, 0 at +1, 2 at -1, 3 at -3). */
static float
dibit_level(char dibit) {
    switch (dibit) {
        case '1': return 3.0f;
        case '0': return 1.0f;
        case '2': return -1.0f;
        default: return -3.0f;
    }
}

/* What dsd_symbol.c's read leaves behind: the soft history, the symbol count and the stream's symbol clock. */
float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;
    const float symbol = dibit_level(g_pos < STREAM_LEN ? g_stream[g_pos] : (char)('0' + (g_pos & 3)));
    g_pos++;
    if (g_flush_on_next_read) {
        g_flush_on_next_read = 0;
        state->symbol_clock_epoch++;
    }
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    state->symbol_clock++;
    return symbol;
}

/* A TETRA uplink session on the 18 ksym/s CQPSK profile, as -fT --tetra-uplink runs it. */
static void
init_uplink(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 72000;
    opts->wav_decimator = 72000;
    opts->mod_qpsk = 1;
    opts->mod_cli_lock = 1;
    opts->frame_tetra = 1;
    opts->tetra_uplink = 1;
    opts->msize = 1024; /* the default: the slicer centre averages over many windows */
    opts->ssize = 128;
    state->rf_mod = 1;
    state->p25_p2_active_slot = -1;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_18000_4;
    state->samplesPerSymbol = 4;
    state->symbolCenter = 2;
    assert(init_state_buffers(state));
    stream_build();
    dsd_frame_sync_reset_mod_state();
}

/* The first call: a matchless pass that ends before the burst's training does. */
static void
first_call_times_out(dsd_opts* opts, dsd_state* state) {
    const int rc = getFrameSync(opts, state);
    assert(rc == -1);
    assert(g_pos > BURST_AT && g_pos < BURST_AT + CB_TAIL + CB_HALF + CB_ETS);
    assert(state->frame_sync_carry.valid == 1);
}

/* Calls until a sync or the stream's end; returns the sync. */
static int
hunt_to_end(dsd_opts* opts, dsd_state* state) {
    while (g_pos < STREAM_LEN) {
        const int sync = getFrameSync(opts, state);
        if (sync > DSD_SYNC_NONE) {
            return sync;
        }
    }
    return DSD_SYNC_NONE;
}

/* The stream went on unbroken: the burst straddling the exit locks at the end of its training, block 1 whole. */
static void
test_burst_across_the_exit_locks(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    const int sync = hunt_to_end(&opts, &state);
    assert(sync == DSD_SYNC_TETRA_NDB_POS);
    assert(state.tetra_sync.cb == 1 && state.tetra_sync.b1_valid == 1);
    assert(g_pos == BURST_AT + CB_TAIL + CB_HALF + CB_ETS);
    for (int i = 0; i < CB_HALF; i++) {
        assert(state.tetra_sync.b1_dibuf[i] == (uint8_t)(g_block1[i] - '0'));
    }
    /* A sync drops the history: the next call starts blind. */
    assert(state.frame_sync_carry.valid == 0);
    free_state_buffers(&state);
}

/* The receiver left the carrier between the calls (dsd_engine_carrier_boundary() bumps carrier_seq): what the history
   holds is the carrier left's, so the next call starts blind and the burst, half of it before, is not matched. */
static void
test_carrier_change_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    state.carrier_seq++;
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* A symbol read between the calls (a handler, a flush): the history no longer ends where the stream goes on. */
static void
test_symbol_between_calls_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    (void)getSymbol(&opts, &state, 0);
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* The stream's clock moved to another epoch (an RTL flush, an input switch, a scan target change). */
static void
test_new_epoch_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    state.symbol_clock_epoch++;
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* A trunk retune in the exit's own no-sync hooks, as a P25 voice channel's release makes at hangtime (its return to the
   control channel forgets the sync histories, dsd_engine_rxid_trunk_tune()): the history read on the carrier left is
   not kept, though the exit comes after the retune. */
static void
retune_in_hooks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    dsd_state_forget_sync_join_histories(state);
}

static void
test_retune_in_no_sync_hooks_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    dsd_frame_sync_hooks hooks = {0};
    hooks.no_carrier = retune_in_hooks;
    dsd_frame_sync_hooks_set(&hooks);
    assert(getFrameSync(&opts, &state) == -1);
    assert(state.frame_sync_carry.valid == 0);
    dsd_frame_sync_hooks_set(NULL);
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* A tagged tune that completes in the hooks (the trunk tuning generation moves) is a retune too. */
static void
tune_completes_in_hooks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    dsd_trunk_tuning_generation_advance();
}

static void
test_tune_completing_in_no_sync_hooks_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    dsd_frame_sync_hooks hooks = {0};
    hooks.no_carrier = tune_completes_in_hooks;
    dsd_frame_sync_hooks_set(&hooks);
    assert(getFrameSync(&opts, &state) == -1);
    assert(state.frame_sync_carry.valid == 0);
    dsd_frame_sync_hooks_set(NULL);
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* A trunk retune on the decoder thread between the calls (the engine loop's ticks, noCarrier's return to the control
   channel): it forgets the sync histories, and the carried one with them. */
static void
test_retune_between_calls_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    dsd_state_forget_sync_join_histories(&state);
    assert(state.frame_sync_carry.valid == 0);
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* A tune off the decoder thread (the watchdog's) between the calls: the next call applies it at its start
   (dsd_frame_sync_apply_off_thread_tunes()), before it would take the history up. */
static void
test_off_thread_tune_between_calls_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    const uint32_t drops = state.frame_sync_carry.drops;
    dsd_p25p1_decision read;
    dsd_frame_sync_p25p1_read_decision(&read);
    dsd_frame_sync_note_off_thread_tune(&read);
    assert(dsd_frame_sync_off_thread_tune_pending());
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    assert(state.frame_sync_carry.drops != drops);
    free_state_buffers(&state);
}

/* A tune that completed between the calls without forgetting anything on this thread (the generation moves). */
static void
test_completed_tune_between_calls_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    dsd_trunk_tuning_generation_advance();
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* The next call's first read finds a stream flush (an RTL retune's, which the reader notes only when it reads): the
   history taken up at the call's start goes again. */
static void
test_flush_at_first_read_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    g_flush_on_next_read = 1;
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* The modulation changed between the calls: a lock the next call applies at its start (frame_sync_apply_cli_mod_lock())
   moves rf_mod off the CQPSK the history was sliced on. */
static void
test_new_modulation_starts_blind(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    first_call_times_out(&opts, &state);
    opts.mod_qpsk = 0;
    opts.mod_c4fm = 1;
    assert(hunt_to_end(&opts, &state) == DSD_SYNC_NONE);
    free_state_buffers(&state);
}

/* The hunt steps to another profile at a no-sync exit: the history is kept with the profile it was read under, which
   is not the one the next call runs, so the next call does not take it up. */
static void
test_profile_step_keeps_the_read_profile(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_uplink(&opts, &state);
    opts.mod_cli_lock = 0;
    opts.frame_dmr = 1;
    const int start = state.sps_hunt_idx;
    int calls = 0;
    while (state.sps_hunt_idx == start && calls < 40) {
        (void)getFrameSync(&opts, &state); /* the burst may lock on the way: what matters is the exit that steps */
        calls++;
    }
    assert(state.sps_hunt_idx != start);
    /* Kept under the profile it was read under, which frame_sync_history_resume() compares with the one running. */
    assert(state.frame_sync_carry.valid == 1 && state.frame_sync_carry.sps_hunt_idx == start);
    free_state_buffers(&state);
}

int
main(void) {
    test_burst_across_the_exit_locks();
    test_carrier_change_starts_blind();
    test_symbol_between_calls_starts_blind();
    test_new_epoch_starts_blind();
    test_retune_in_no_sync_hooks_starts_blind();
    test_tune_completing_in_no_sync_hooks_starts_blind();
    test_retune_between_calls_starts_blind();
    test_off_thread_tune_between_calls_starts_blind();
    test_completed_tune_between_calls_starts_blind();
    test_flush_at_first_read_starts_blind();
    test_new_modulation_starts_blind();
    test_profile_step_keeps_the_read_profile();
    printf("FRAME_SYNC_HISTORY_CARRY: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
