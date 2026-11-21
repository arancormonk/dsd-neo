// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/telemetry.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

/* exitflag declared in runtime/exitflag.h, defined in src/runtime/exitflag.c */

static dsd_thread_t g_p25_sm_wd_thread;
static atomic_int g_p25_sm_wd_running = 0;
/* Whether g_p25_sm_wd_thread holds a started thread: only that one is joined (a handle never created is not). */
static int g_p25_sm_wd_thread_created = 0;
#ifdef DSD_NEO_TEST_HOOKS
static p25_sm_watchdog_thread_create_fn g_p25_sm_wd_create_for_test = NULL;
#endif
static atomic_int g_p25_sm_tick_lock = 0;
static atomic_int g_p25_sm_in_tick = 0;
/* A watchdog beat asked the decoder thread for a tick (p25_sm_watchdog_service()). */
static atomic_int g_p25_sm_tick_wanted = 0;
/* The tune requests the ticks began and left in flight, as the range (after, through] of request IDs (0, 0: none),
   which p25_sm_tick_tunes_in_flight() checks and retires. Written by the thread that ticks (the decoder's in a run). */
static dsd_atomic_u64 g_p25_sm_tick_tune_after = {0U};
static dsd_atomic_u64 g_p25_sm_tick_tune_through = {0U};
static int g_p25_sm_wd_ms = 0; // 0 => unset (use defaults per UI mode)

#if defined(_MSC_VER)
#define P25_SM_THREAD_LOCAL __declspec(thread)
#else
#define P25_SM_THREAD_LOCAL _Thread_local
#endif
/* Whether the calling thread holds the tick guard (p25_sm_tick_guard_held_by_caller()). */
static P25_SM_THREAD_LOCAL int s_tick_guard_held_here = 0;

int
p25_sm_tick_guard_try_enter(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_p25_sm_tick_lock, &expected, 1)) {
        return 0;
    }
    s_tick_guard_held_here = 1;
    return 1;
}

void
p25_sm_tick_guard_enter(void) {
    while (!p25_sm_tick_guard_try_enter()) {
        dsd_sleep_ms(1U);
    }
}

void
p25_sm_tick_guard_leave(void) {
    s_tick_guard_held_here = 0;
    atomic_store(&g_p25_sm_tick_lock, 0);
}

int
p25_sm_tick_guard_held_by_caller(void) {
    return s_tick_guard_held_here;
}

int
p25_sm_tick_guard_busy(void) {
    return atomic_load(&g_p25_sm_tick_lock) != 0;
}

/* Whether a recovery tick has anything to run. A startup control-channel hunt is the decoder thread's own
   (p25_sm_cold_start_tick_locked(), issue #703), which also ends it once validated control has named an owner: no tick
   touches it. */
static int
p25_sm_tick_eligible(const dsd_opts* opts, const dsd_state* state) {
    return !p25_sm_get_ctx()->cold_start_hunt && dsd_trunk_p25_recovery_allowed(opts, state);
}

/* One recovery tick, the caller holding the guard. */
static void
p25_sm_tick_locked(dsd_opts* opts, dsd_state* state) {
    if (p25_sm_tick_eligible(opts, state)) {
        /* Only one tick runs at a time across all callers. */
        atomic_store(&g_p25_sm_in_tick, 1);
        // Drive the high-level trunk SM tick
        p25_sm_tick_ctx(p25_sm_get_ctx(), opts, state);
        atomic_store(&g_p25_sm_in_tick, 0);
    }
}

/* One tick, under the guard, that notes what its tunes did, wherever the decoder runs it from (every tick runs through
   here). A tune that may have moved the receiver is a carrier boundary: a decoder that reads back before its sync
   (DMR, X2-TDMA) waits for that many of the new carrier's symbols (dsd_state_carrier_settling()). A tune left in flight
   (an RTL retune the stream applies on its own thread) is noted, and the engine starts no hunt until it lands
   (p25_sm_tick_tunes_in_flight()). One load when the tick began no request. */
static void
p25_sm_tick_noting_tunes(dsd_opts* opts, dsd_state* state) {
    const uint64_t before = dsd_trunk_tuning_last_request();
    p25_sm_tick_locked(opts, state);
    const uint64_t newest = dsd_trunk_tuning_last_request();
    if (newest == before) {
        return;
    }
    if (dsd_trunk_tuning_retuned_since(before)) {
        dsd_state_note_carrier_change(state);
    }
    if (dsd_trunk_tuning_request_pending_in(before, newest)) {
        if (dsd_atomic_u64_load_acquire(&g_p25_sm_tick_tune_through) == 0U) {
            dsd_atomic_u64_store_release(&g_p25_sm_tick_tune_after, before);
        }
        dsd_atomic_u64_store_release(&g_p25_sm_tick_tune_through, newest);
    }
}

int
p25_sm_tick_tunes_in_flight(void) {
    const uint64_t through = dsd_atomic_u64_load_acquire(&g_p25_sm_tick_tune_through);
    if (through == 0U) {
        return 0;
    }
    if (dsd_trunk_tuning_request_pending_in(dsd_atomic_u64_load_acquire(&g_p25_sm_tick_tune_after), through)) {
        return 1;
    }
    dsd_atomic_u64_store_release(&g_p25_sm_tick_tune_after, 0U);
    dsd_atomic_u64_store_release(&g_p25_sm_tick_tune_through, 0U);
    return 0;
}

void
p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (p25_sm_tick_guard_try_enter()) {
        /* This tick is the one a standing beat asked for too: it is spent here, so the next service point does not run
           a second tick for it. A beat posted while this tick runs stands for the next point. */
        atomic_store(&g_p25_sm_tick_wanted, 0);
        p25_sm_tick_noting_tunes(opts, state);
        p25_sm_tick_guard_leave();
    }
}

void
p25_sm_watchdog_request_tick(void) {
    atomic_store(&g_p25_sm_tick_wanted, 1);
}

int
p25_sm_watchdog_tick_wanted(void) {
    return atomic_load(&g_p25_sm_tick_wanted) != 0;
}

int
p25_sm_watchdog_service(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || atomic_load(&g_p25_sm_tick_wanted) == 0) {
        return 0;
    }
    /* A hold the caller already has, or anyone else's, leaves the beat standing for the next point: a holder may be
       swapping what recovery reads, so nothing of it is read without the guard. */
    if (!p25_sm_tick_guard_try_enter()) {
        return 0;
    }
    /* A beat with no recovery to run (trunking off, -Y scanning, no P25 to follow, a startup hunt) is let go: its
       tick could tune nothing, so it never hands a read back, even behind queued work. */
    if (!p25_sm_tick_eligible(opts, state)) {
        atomic_store(&g_p25_sm_tick_wanted, 0);
        p25_sm_tick_guard_leave();
        return 0;
    }
    /* Shutdown or queued work (a command, a put-off publication) comes first: the beat stands, and no tune starts on a
       receiver being stopped or replaced. The engine loop runs the work and then its own tick, which takes the beat. */
    if (dsd_exitflag_load() || dsd_runtime_controls_pending() || dsd_runtime_publish_pending()) {
        p25_sm_tick_guard_leave();
        return P25_SM_WATCHDOG_DEFERRED;
    }
    atomic_store(&g_p25_sm_tick_wanted, 0);
    p25_sm_tick_noting_tunes(opts, state);
    p25_sm_tick_guard_leave();
    return 1;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    p25_sm_watchdog_thread(void* arg) {
    dsd_runtime_fp_mode_apply_thread("p25_watchdog");
    (void)arg;
    while (atomic_load(&g_p25_sm_wd_running) && !dsd_exitflag_load()) {
        /* A beat only: the tick it asks for runs on the decoder thread, which owns dsd_state and dsd_opts and makes
           every tune (p25_sm_watchdog_service()). This thread reads and writes neither. */
        p25_sm_watchdog_request_tick();
        // Compute watchdog cadence. Prefer env override when provided; otherwise
        // tick faster whenever a frontend is watching, so a wedge is corrected
        // before anyone notices it. Gate on telemetry rather than on the frontend
        // kind: a native GUI consumes the same publishes without ever becoming the
        // terminal frontend, and it wants the same responsiveness.
        int ms = g_p25_sm_wd_ms;
        if (ms <= 0) {
            ms = dsd_telemetry_is_active() ? 200 : 400; // 200ms frontend, 400ms headless
        }
        if (ms < 20) {
            ms = 20; // clamp to sane bounds
        }
        if (ms > 2000) {
            ms = 2000; // 2s max
        }
        dsd_sleep_ms((unsigned int)ms);
    }
    DSD_THREAD_RETURN;
}

void
p25_sm_watchdog_start(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    // One-time env override for watchdog cadence (milliseconds)
    // DSD_NEO_P25_WD_MS=20 .. 2000
    if (g_p25_sm_wd_ms == 0) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->p25_wd_ms_is_set) {
            g_p25_sm_wd_ms = cfg->p25_wd_ms;
        }
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_p25_sm_wd_running, &expected, 1)) {
        return;
    }
    /* A beat a previous run left is not this run's. */
    atomic_store(&g_p25_sm_tick_wanted, 0);
#ifdef DSD_NEO_TEST_HOOKS
    const p25_sm_watchdog_thread_create_fn create = g_p25_sm_wd_create_for_test;
    const int rc = create ? create(&g_p25_sm_wd_thread, p25_sm_watchdog_thread, NULL)
                          : dsd_thread_create(&g_p25_sm_wd_thread, p25_sm_watchdog_thread, NULL);
#else
    const int rc = dsd_thread_create(&g_p25_sm_wd_thread, p25_sm_watchdog_thread, NULL);
#endif
    if (rc != 0) {
        /* No watchdog this run: the decoder's own ticks keep the state machine going. A later start tries again. */
        atomic_store(&g_p25_sm_wd_running, 0);
        LOG_WARN("P25 watchdog thread did not start (%d); trunking ticks only with the decoder.\n", rc);
        return;
    }
    g_p25_sm_wd_thread_created = 1;
}

void
p25_sm_watchdog_stop(void) {
    (void)atomic_exchange(&g_p25_sm_wd_running, 0);
    if (g_p25_sm_wd_thread_created) {
        dsd_thread_join(g_p25_sm_wd_thread);
        g_p25_sm_wd_thread_created = 0;
    }
    atomic_store(&g_p25_sm_tick_wanted, 0);
}

#ifdef DSD_NEO_TEST_HOOKS
void
p25_sm_watchdog_set_thread_create_for_test(p25_sm_watchdog_thread_create_fn create) {
    g_p25_sm_wd_create_for_test = create;
}
#endif

int
p25_sm_in_tick(void) {
    return atomic_load(&g_p25_sm_in_tick) ? 1 : 0;
}
