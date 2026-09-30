// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_CLOCK_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_CLOCK_H_

/**
 * @file
 * @brief Injectable decode clock plus explicit real-time clock reads.
 *
 * Two domains are kept apart on purpose:
 *
 * - Decode domain (`dsd_decode_*`): decode decisions and decoded-output
 *   timestamps. SYSTEM source (default) reads the platform clocks exactly as
 *   before. REPLAY source reads `anchor + media time`, so a replayed capture
 *   decodes the same way on every run. TEST source reads a settable value.
 * - Real-time domain (`dsd_realtime_*`): sleeps, condvar deadlines, pacing,
 *   socket and device timeouts, UI throttles and metrics. Always the platform
 *   clocks, regardless of the decode source.
 *
 * One comparison must never mix the two domains.
 *
 * State is process-wide atomics. Reads are loads and are safe from any
 * thread. Only the decoder thread should advance media time.
 */

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decode clock source. */
typedef enum dsd_decode_clock_source {
    DSD_DECODE_CLOCK_SYSTEM = 0, /**< Platform clocks (default, live). */
    DSD_DECODE_CLOCK_REPLAY = 1, /**< anchor + media time. */
    DSD_DECODE_CLOCK_TEST = 2,   /**< Settable value (tests). */
} dsd_decode_clock_source_t;

/* ---- Decode domain ---- */

/** @brief Decode-domain replacement for `time(NULL)`. */
time_t dsd_decode_time(void);

/** @brief Decode-domain monotonic seconds (same formula as `dsd_time_now_monotonic_s`). */
double dsd_decode_now_mono_s(void);

/** @brief Decode-domain monotonic milliseconds (replaces `dsd_time_monotonic_ms`). */
uint64_t dsd_decode_now_mono_ms(void);

/** @brief Decode-domain monotonic nanoseconds (replaces `dsd_time_monotonic_ns`). */
uint64_t dsd_decode_now_mono_ns(void);

/** @brief Decode-domain wall-clock seconds with sub-second precision (same formula as `dsd_time_now_realtime_s`). */
double dsd_decode_now_realtime_s(void);

/* ---- Real-time domain (never affected by the decode source) ---- */

/** @brief Real monotonic milliseconds. */
uint64_t dsd_realtime_mono_ms(void);

/** @brief Real monotonic nanoseconds. */
uint64_t dsd_realtime_mono_ns(void);

/** @brief Real monotonic seconds (same formula as `dsd_time_now_monotonic_s`). */
double dsd_realtime_mono_s(void);

/** @brief Real `time(NULL)`. */
time_t dsd_realtime_time(void);

/** @brief Real wall-clock seconds with sub-second precision (`dsd_time_realtime_ns() / 1e9`). */
double dsd_realtime_now_s(void);

/* ---- Control ---- */

/** @brief Select the SYSTEM source. */
void dsd_decode_clock_use_system(void);

/**
 * @brief Select the REPLAY source and reset media time to zero.
 *
 * The anchor is `max(anchor_utc_s, 2000-01-01T00:00:00Z)`; a zero, negative or
 * pre-2000 anchor is raised to that floor.
 */
void dsd_decode_clock_use_replay(int64_t anchor_utc_s);

/**
 * @brief Advance REPLAY media time (nanoseconds since the anchor).
 *
 * Monotone: a value smaller than the current media time is ignored. Only the
 * decoder thread may call this.
 */
void dsd_decode_clock_set_media_ns(uint64_t media_ns);

/** @brief Select the TEST source with the given value for both mono and wall reads. */
void dsd_decode_clock_use_test(uint64_t now_ns);

/** @brief Set the TEST value. Takes effect only while the TEST source is selected. */
void dsd_decode_clock_test_set_ns(uint64_t now_ns);

/** @brief Current source, for diagnostics. */
dsd_decode_clock_source_t dsd_decode_clock_source(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_DECODE_CLOCK_H_ */
