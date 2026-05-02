// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Input ring watermark system implementation.
 *
 * Provides adaptive watermark-based flow control for the input ring.
 * The demod thread pauses consumption when the ring drains below the
 * low watermark and resumes when it refills to the target watermark.
 *
 * The target watermark adapts: frequent low-watermark events increase
 * it (more buffering for bursty connections), while quiet periods
 * decrease it (lower latency when the connection is stable).
 *
 * See input_ring_watermark.h for the full API contract.
 */

#include <dsd-neo/runtime/input_ring_watermark.h>

/*============================================================================
 * Internal helpers
 *============================================================================*/

/** Nanoseconds per second. */
static const uint64_t NS_PER_S = 1000000000ULL;

/**
 * @brief Convert milliseconds to interleaved I/Q float element count.
 *
 * Formula: elements = (uint64_t)ms × sample_rate × 2 / 1000
 * The factor of 2 accounts for interleaved I (in-phase) and Q (quadrature)
 * float samples stored in the ring buffer.
 */
static size_t
ms_to_elements(uint32_t ms, uint32_t sample_rate) {
    return (size_t)((uint64_t)ms * (uint64_t)sample_rate * 2ULL / 1000ULL);
}

/*============================================================================
 * Public API
 *============================================================================*/

void
watermark_init(struct input_ring_watermark* wm, int enabled, uint32_t sample_rate) {
    if (!wm) {
        return;
    }

    *wm = (struct input_ring_watermark){0};

    /* If sample_rate is 0, force-disable to avoid division issues and
     * nonsensical element counts. */
    if (sample_rate == 0) {
        wm->enabled = 0;
        return;
    }

    wm->enabled = enabled;
    wm->sample_rate = sample_rate;
    wm->target_ms = WATERMARK_TARGET_DEFAULT_MS;
    wm->ema_alpha = 0.1f;

    wm->low_watermark = ms_to_elements(WATERMARK_LOW_DEFAULT_MS, sample_rate);
    wm->target_watermark = ms_to_elements(WATERMARK_TARGET_DEFAULT_MS, sample_rate);
}

int
watermark_should_consume(struct input_ring_watermark* wm, size_t ring_used, size_t ring_capacity) {
    if (!wm || !wm->enabled) {
        return 1; /* Disabled → always consume (passthrough). */
    }

    /* Update EMA of fill fraction for smooth tracking.
     * fill_ratio is 0.0–1.0 representing how full the ring is. */
    if (ring_capacity > 0) {
        float fill_ratio = (float)ring_used / (float)ring_capacity;
        wm->fill_ema = wm->ema_alpha * fill_ratio + (1.0f - wm->ema_alpha) * wm->fill_ema;
    }

    if (!wm->paused) {
        /* Normal mode: check if we should pause. */
        if (ring_used < wm->low_watermark) {
            wm->paused = 1;
            return 0; /* Pause — tell demod to wait. */
        }
        return 1; /* Continue consuming. */
    }

    /* Paused mode: check if we should resume. */
    if (ring_used >= wm->target_watermark) {
        wm->paused = 0;
        return 1; /* Resume — ring has refilled to target. */
    }

    return 0; /* Stay paused — not yet at target. */
}

void
watermark_on_low_event(struct input_ring_watermark* wm, uint64_t now_ns) {
    if (!wm || !wm->enabled) {
        return;
    }

    /* Start a new 60-second window if this is the first event or the
     * current window has expired. */
    uint64_t window_ns = (uint64_t)WATERMARK_ADAPT_UP_WINDOW_S * NS_PER_S;
    if (wm->low_event_count == 0 || (now_ns - wm->low_event_window_start_ns) >= window_ns) {
        wm->low_event_count = 0;
        wm->low_event_window_start_ns = now_ns;
    }

    wm->low_event_count++;
    wm->last_low_event_ns = now_ns;

    /* If we've exceeded the threshold, bump the target up. */
    if (wm->low_event_count > WATERMARK_ADAPT_UP_THRESHOLD) {
        uint32_t new_target = wm->target_ms + WATERMARK_ADAPT_UP_MS;
        if (new_target > WATERMARK_TARGET_MAX_MS) {
            new_target = WATERMARK_TARGET_MAX_MS;
        }
        wm->target_ms = new_target;
        wm->target_watermark = ms_to_elements(wm->target_ms, wm->sample_rate);

        /* Reset the event counter so we don't keep bumping every call. */
        wm->low_event_count = 0;
        wm->low_event_window_start_ns = now_ns;
    }
}

void
watermark_periodic_adjust(struct input_ring_watermark* wm, uint64_t now_ns) {
    if (!wm || !wm->enabled) {
        return;
    }

    /* If no low events have ever occurred, nothing to decrease from. */
    if (wm->last_low_event_ns == 0) {
        return;
    }

    uint64_t quiet_ns = (uint64_t)WATERMARK_ADAPT_DOWN_QUIET_S * NS_PER_S;
    if ((now_ns - wm->last_low_event_ns) >= quiet_ns) {
        if (wm->target_ms > WATERMARK_TARGET_MIN_MS) {
            uint32_t decrease = WATERMARK_ADAPT_DOWN_MS;
            if (wm->target_ms - WATERMARK_TARGET_MIN_MS < decrease) {
                decrease = wm->target_ms - WATERMARK_TARGET_MIN_MS;
            }
            wm->target_ms -= decrease;
            wm->target_watermark = ms_to_elements(wm->target_ms, wm->sample_rate);
        }

        /* Reset last_low_event_ns to now so we don't keep decreasing
         * every call — require another full quiet period. */
        wm->last_low_event_ns = now_ns;
    }
}
