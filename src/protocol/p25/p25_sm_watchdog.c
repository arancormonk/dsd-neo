// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

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

void
p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_p25_sm_tick_lock, &expected, 1)) {
        /* Only one tick runs at a time across all callers. */
        atomic_store(&g_p25_sm_in_tick, 1);
        p25_sm_tick(opts, state);
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
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
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
