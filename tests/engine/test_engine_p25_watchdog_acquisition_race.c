// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* What frame acquisition finds is the decoder thread's: the frame-sync hunt writes the last sync type and the control
 * channel's sync stamp outside the P25 SM tick guard, at every accepted sync, and the no-carrier pass resets the carrier's
 * codes. The real P25 watchdog thread ticks a machine sitting on its control channel, whose tick re-learns the expected
 * NAC from the sync type and keeps the channel alive from the stamp, while this thread does what the decoder does
 * around it: the accept's writes, a frame dispatch under the guard, and now and then a no-carrier pass. The frame
 * sync's no-sync note to the machine waits for a tick that holds the guard. ThreadSanitizer checks both in its build. */
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include "engine_hooks_install.h"
#include "test_env.h"

enum { CC_NAC = 0x293, NO_CARRIER_EVERY = 64, TICK_COUNT_MS = 20, TICK_HOLD_MS = 60 };

static const double RUN_S = 1.5;
static const long CC_FREQ_HZ = 851000000L;

/* What the hunt's sync accept writes, outside the guard (frame_sync_accept_p25p1() and frame_sync_note_cc_sync()). */
static void
accept_sync(dsd_state* state, int round) {
    state->lastsynctype = (round & 1) != 0 ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    dsd_mark_cc_sync(state);
}

/* The frame dispatch that follows, under the guard: the NID names the NAC, a TSBK decodes. */
static void
dispatch_frame(dsd_state* state) {
    p25_sm_tick_guard_enter();
    state->nac = CC_NAC;
    state->p25_last_cc_msg_time = dsd_decode_time();
    state->p25_last_cc_msg_time_m = dsd_decode_now_mono_s();
    p25_sm_tick_guard_leave();
}

/* A tick in progress: it holds the guard, and some time into it starts the machine's no-sync count over (as a
   control-channel tune's reacquire tracking does), then holds on a while. */
static DSD_THREAD_RETURN_TYPE
tick_holding_guard(void* opaque) {
    p25_sm_ctx_t* ctx = opaque;
    p25_sm_tick_guard_enter();
    dsd_sleep_ms(TICK_COUNT_MS);
    ctx->cc_no_sync_passes = 0U;
    dsd_sleep_ms(TICK_HOLD_MS);
    p25_sm_tick_guard_leave();
    DSD_THREAD_RETURN;
}

/* The frame sync's pass without sync is counted by the machine after the tick that held the guard, never inside it:
   the tick's own reset stands, and the pass counts on top of it. */
static int
check_no_sync_note_waits_for_tick(dsd_opts* opts, dsd_state* state) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    ctx->state = P25_SM_ON_CC;
    ctx->cc_tune_pending = 0;
    ctx->cc_sync_pending = 1;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_RETURN;
    ctx->t_cc_tune_m = dsd_decode_now_mono_s();
    ctx->cc_no_sync_passes = 5U;
    dsd_engine_frame_sync_hooks_install();

    dsd_thread_t tick;
    if (dsd_thread_create(&tick, tick_holding_guard, ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the tick thread did not start\n");
        return 1;
    }
    while (!p25_sm_tick_guard_busy()) {
        dsd_thread_yield();
    }
    dsd_frame_sync_hook_p25_sm_vc_no_sync(opts, state);
    dsd_thread_join(tick);
    dsd_frame_sync_hooks_set(NULL);

    if (ctx->cc_no_sync_passes != 1U) {
        DSD_FPRINTF(stderr, "FAIL: the no-sync pass counted %u, not 1 after the tick's reset\n",
                    ctx->cc_no_sync_passes);
        return 1;
    }
    return 0;
}

/* The real watchdog ticks the control channel while this thread accepts syncs, dispatches frames and loses the
   carrier now and then: the tick learns the NAC and keeps the channel. */
static int
check_watchdog_beside_acquisition(dsd_opts* opts, dsd_state* state) {
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, opts, state);
    ctx->state = P25_SM_IDLE;
    accept_sync(state, 0);
    dispatch_frame(state);

    p25_sm_watchdog_start(opts, state);
    const double started_s = dsd_realtime_mono_s();
    int round = 1;
    while (dsd_realtime_mono_s() - started_s < RUN_S) {
        accept_sync(state, round);
        dispatch_frame(state);
        if (round % NO_CARRIER_EVERY == 0) {
            noCarrier(opts, state);
            accept_sync(state, round);
            dispatch_frame(state);
        }
        round++;
        dsd_thread_yield();
    }
    p25_sm_watchdog_stop();

    int failed = 0;
    if (ctx->state != P25_SM_ON_CC) {
        DSD_FPRINTF(stderr, "FAIL: the machine left its live control channel (state %d)\n", (int)ctx->state);
        failed = 1;
    }
    if (ctx->expected_cc_nac != CC_NAC) {
        DSD_FPRINTF(stderr, "FAIL: the watchdog's tick learned NAC 0x%03X, not 0x%03X\n", ctx->expected_cc_nac, CC_NAC);
        failed = 1;
    }
    return failed;
}

int
main(void) {
    if (dsd_test_setenv("DSD_NEO_P25_WD_MS", "20", 1) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not set the watchdog cadence\n");
        return 1;
    }
    dsd_neo_config_init();
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "FAIL: out of memory\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->trunk_enable = 1;
    opts->frame_p25p1 = 1;
    state->p25_cc_freq = state->trunk_cc_freq = CC_FREQ_HZ;
    dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_P25);
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_set_decoder_thread(1);

    int failed = check_no_sync_note_waits_for_tick(opts, state);
    failed |= check_watchdog_beside_acquisition(opts, state);

    dsd_engine_set_decoder_thread(0);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    freeState(state);
    free(state);
    free(opts);
    (void)dsd_test_unsetenv("DSD_NEO_P25_WD_MS");
    return failed;
}
