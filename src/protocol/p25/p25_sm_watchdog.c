// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

extern volatile uint8_t exitflag;

static pthread_t g_p25_sm_wd_thread;
static atomic_int g_p25_sm_wd_running = 0;
static atomic_int g_p25_sm_tick_lock = 0;
static atomic_int g_p25_sm_in_tick = 0;
static dsd_opts* g_opts = NULL;
static dsd_state* g_state = NULL;
static int g_p25_sm_wd_ms = 0; // 0 => unset (use defaults per UI mode)

void
p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_p25_sm_tick_lock, &expected, 1)) {
        /* Only one tick runs at a time across all callers. */
        atomic_store(&g_p25_sm_in_tick, 1);
        // Drive the high-level trunk SM tick
        p25_sm_tick(opts, state);
        // Drive the minimal P25p2 follower tick so grant→voice and hang
        // timers are enforced independently of frame processing cadence.
        dsd_p25p2_min_tick(dsd_p25p2_min_get(), opts, state);
        atomic_store(&g_p25_sm_in_tick, 0);
        atomic_store(&g_p25_sm_tick_lock, 0);
    }
}

static void*
p25_sm_watchdog_thread(void* arg) {
    (void)arg;
    struct timespec ts;
    while (atomic_load(&g_p25_sm_wd_running) && !exitflag) {
        if (g_opts && g_state && g_opts->p25_trunk == 1) {
            p25_sm_try_tick(g_opts, g_state);
        }
        // Compute watchdog cadence. Prefer env override when provided;
        // otherwise, tick faster under ncurses to reduce perceived wedges.
        int ms = g_p25_sm_wd_ms;
        if (ms <= 0) {
            ms = (g_opts && g_opts->use_ncurses_terminal == 1) ? 200 : 400; // 200ms UI, 400ms headless
        }
        if (ms < 20) {
            ms = 20; // clamp to sane bounds
        }
        if (ms > 2000) {
            ms = 2000; // 2s max
        }
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void
p25_sm_watchdog_start(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    g_opts = opts;
    g_state = state;
    // One-time env override for watchdog cadence (milliseconds)
    // DSD_NEO_P25_WD_MS=100 .. 2000
    if (g_p25_sm_wd_ms == 0) {
        const char* s = getenv("DSD_NEO_P25_WD_MS");
        if (s && s[0] != '\0') {
            int v = atoi(s);
            if (v >= 20 && v <= 2000) {
                g_p25_sm_wd_ms = v;
            } else if (v > 0) {
                g_p25_sm_wd_ms = (v < 20) ? 20 : 2000;
            }
        }
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_p25_sm_wd_running, &expected, 1)) {
        (void)pthread_create(&g_p25_sm_wd_thread, NULL, p25_sm_watchdog_thread, NULL);
    }
}

void
p25_sm_watchdog_stop(void) {
    int was = atomic_exchange(&g_p25_sm_wd_running, 0);
    if (was != 0) {
        pthread_join(g_p25_sm_wd_thread, NULL);
    }
}

int
p25_sm_in_tick(void) {
    return atomic_load(&g_p25_sm_in_tick) ? 1 : 0;
}
