// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Lightweight monotonic time helpers for state-machine timing.
 *
 * Provides monotonic time in seconds and helpers to stamp/clear CC/VC sync
 * times on `dsd_state`.
 */

#pragma once

#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/timing.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Return monotonic time in seconds when available (wall clock fallback).
 */
static inline double
dsd_time_now_monotonic_s(void) {
    return (double)dsd_time_monotonic_ns() / 1e9;
}

/* Convenience helpers to stamp/clear CC and VC sync times on state
 * with both wall-clock and monotonic values.
 */
/** @brief Stamp current time as control-channel sync (monotonic + wall clock). */
static inline void
dsd_mark_cc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

/** @brief Stamp current time as voice-channel sync (monotonic + wall clock). */
static inline void
dsd_mark_vc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
}

/** @brief Clear control-channel sync timestamps. */
static inline void
dsd_clear_cc_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
}

/** @brief Clear voice-channel sync timestamps. */
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
