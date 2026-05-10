// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/**
 * @file
 * @brief Input ring watermark system for adaptive IQ buffering.
 *
 * Implements watermark-based flow control for the input ring buffer.
 * The demod thread consults this module before consuming samples:
 * when the fill level drops below the low watermark, consumption is
 * paused until the ring refills to the target watermark.
 *
 * The target watermark adapts over time: frequent low-watermark events
 * increase it (more buffering), while quiet periods decrease it (lower
 * latency).
 *
 * When disabled (non-TCP modes), all checks return "consume" immediately,
 * preserving existing behavior for USB and other backends.
 *
 * See input_ring_watermark.cpp for the implementation.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare to avoid pulling in input_ring.h (C++ atomics) */
struct input_ring_state;

/*============================================================================
 * Constants — watermark thresholds and adaptive tuning parameters
 *============================================================================*/

/** Low watermark default: 200ms of I/Q data triggers demod pause. */
#define WATERMARK_LOW_DEFAULT_MS     200

/** Target watermark default: 500ms of I/Q data required to resume. */
#define WATERMARK_TARGET_DEFAULT_MS  500

/** Maximum adaptive target: 1500ms (upper bound for target growth). */
#define WATERMARK_TARGET_MAX_MS      1500

/** Minimum adaptive target: 300ms (lower bound for target shrink). */
#define WATERMARK_TARGET_MIN_MS      300

/** Adaptive increase step: add 100ms when low events are frequent. */
#define WATERMARK_ADAPT_UP_MS        100

/** Adaptive decrease step: subtract 50ms during quiet periods. */
#define WATERMARK_ADAPT_DOWN_MS      50

/** Low-event count threshold: >2 events in window triggers increase. */
#define WATERMARK_ADAPT_UP_THRESHOLD 2

/** Window for counting low-watermark events (seconds). */
#define WATERMARK_ADAPT_UP_WINDOW_S  60

/** Quiet period before decreasing target (seconds). */
#define WATERMARK_ADAPT_DOWN_QUIET_S 30

/*============================================================================
 * Types
 *============================================================================*/

/**
 * @brief Watermark system state.
 *
 * Owned by the demod thread.  Not shared across threads.
 */
struct input_ring_watermark {
    int enabled;             /**< 0 = disabled (non-TCP modes, passthrough). */
    size_t low_watermark;    /**< Low threshold in float elements.           */
    size_t target_watermark; /**< Target threshold in float elements.        */
    int paused;              /**< 1 = demod paused waiting for refill.       */

    /* EMA of fill level for smooth tracking */
    float fill_ema;  /**< 0.0–1.0 fraction of capacity.              */
    float ema_alpha; /**< Smoothing factor (default 0.1).            */

    /* Adaptive target adjustment */
    uint32_t low_event_count;           /**< Low-watermark events in current window.    */
    uint64_t low_event_window_start_ns; /**< Start of 60s event counting window.  */
    uint64_t last_low_event_ns;         /**< Timestamp of most recent low event.        */

    /* Configuration */
    uint32_t sample_rate; /**< For ms→elements conversion.                */
    uint32_t target_ms;   /**< Current target in ms (adaptive).           */
};

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * @brief Initialise the watermark system.
 *
 * Converts ms thresholds to element counts using the formula:
 *   elements = ms × sample_rate × 2 / 1000
 * (factor of 2 for interleaved I/Q float pairs).
 *
 * If sample_rate is 0, the watermark is force-disabled regardless of
 * the enabled parameter.
 *
 * @param wm          Watermark state to initialise (must not be NULL).
 * @param enabled     1 for TCP mode (watermark active), 0 for passthrough.
 * @param sample_rate Current sample rate in Hz (e.g. 1536000).
 */
void watermark_init(struct input_ring_watermark* wm, int enabled, uint32_t sample_rate);

/**
 * @brief Check whether the demod thread should consume from the input ring.
 *
 * Call this before each input_ring_read_block().  When disabled, always
 * returns 1 (consume).  When enabled, implements hysteresis:
 * - Not paused, fill < low_watermark → pause, return 0
 * - Paused, fill >= target_watermark → resume, return 1
 * - Paused, fill < target_watermark  → stay paused, return 0
 * - Not paused, fill >= low_watermark → continue, return 1
 *
 * @param wm            Watermark state.
 * @param ring_used     Number of elements currently in the ring.
 * @param ring_capacity Total ring capacity in elements.
 * @return 1 if consumption is allowed, 0 if demod should wait.
 */
int watermark_should_consume(struct input_ring_watermark* wm, size_t ring_used, size_t ring_capacity);

/**
 * @brief Notify the watermark system that a low-watermark event occurred.
 *
 * Counts events in a 60-second window.  When the count exceeds
 * WATERMARK_ADAPT_UP_THRESHOLD, increases target_ms by ADAPT_UP_MS
 * (capped at TARGET_MAX_MS) and recalculates target_watermark elements.
 *
 * @param wm     Watermark state.
 * @param now_ns Current monotonic timestamp in nanoseconds.
 */
void watermark_on_low_event(struct input_ring_watermark* wm, uint64_t now_ns);

/**
 * @brief Periodic check for quiet-period target decrease.
 *
 * If no low-watermark events have occurred for ADAPT_DOWN_QUIET_S
 * seconds, decreases target_ms by ADAPT_DOWN_MS (floored at
 * TARGET_MIN_MS) and recalculates target_watermark elements.
 *
 * Call approximately once per second from the demod thread.
 *
 * @param wm     Watermark state.
 * @param now_ns Current monotonic timestamp in nanoseconds.
 */
void watermark_periodic_adjust(struct input_ring_watermark* wm, uint64_t now_ns);

#ifdef __cplusplus
}
#endif
