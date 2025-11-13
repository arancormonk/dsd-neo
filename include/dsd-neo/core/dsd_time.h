// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lightweight monotonic time helpers for SM timing.
 * Returns seconds as double from a monotonic clock when available.
 */

#pragma once

#include <dsd-neo/core/state.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline double
dsd_time_now_monotonic_s(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&counter) && freq.QuadPart != 0) {
        return (double)counter.QuadPart / (double)freq.QuadPart;
    }
    return (double)time(NULL);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
    return (double)time(NULL);
#else
    return (double)time(NULL);
#endif
}

/* Convenience helpers to stamp/clear CC and VC sync times on state
 * with both wall-clock and monotonic values.
 */
static inline void
dsd_mark_cc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

static inline void
dsd_mark_vc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
}

static inline void
dsd_clear_cc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
}

static inline void
dsd_clear_vc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
}

#ifdef __cplusplus
}
#endif
