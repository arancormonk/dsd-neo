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
 *
 * Switch the source only while no reader runs. A REPLAY read is three separate
 * loads (source, anchor, media), so a switch while another thread reads can hand
 * that reader one value mixed from the old and new sources. The engine selects
 * REPLAY at the start of a replay run, before its common setup and before the
 * stream, the P25 watchdog and the terminal frontend start, and returns to SYSTEM
 * only once the stream has stopped (dsd_engine_decode_clock_enter_replay(),
 * dsd_engine_decode_clock_leave_replay()). A host frontend that already polls
 * then (the Qt shell starts before the engine) can read one mixed value for one
 * display refresh; nothing decides on it.
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

/** @brief Decode-domain monotonic seconds (`dsd_decode_now_mono_ns() / 1e9`). */
double dsd_decode_now_mono_s(void);

/** @brief Decode-domain monotonic milliseconds (replaces `dsd_time_monotonic_ms`). */
uint64_t dsd_decode_now_mono_ms(void);

/** @brief Decode-domain monotonic nanoseconds (replaces `dsd_time_monotonic_ns`). */
uint64_t dsd_decode_now_mono_ns(void);

/** @brief Decode-domain wall-clock seconds with sub-second precision (`dsd_time_realtime_ns() / 1e9` under SYSTEM). */
double dsd_decode_now_realtime_s(void);

/* ---- Real-time domain (never affected by the decode source) ---- */

/** @brief Real monotonic milliseconds. */
uint64_t dsd_realtime_mono_ms(void);

/** @brief Real monotonic nanoseconds. */
uint64_t dsd_realtime_mono_ns(void);

/** @brief Real monotonic seconds (`dsd_realtime_mono_ns() / 1e9`). */
double dsd_realtime_mono_s(void);

/** @brief Real `time(NULL)`. */
time_t dsd_realtime_time(void);

/** @brief Real wall-clock seconds with sub-second precision (`dsd_time_realtime_ns() / 1e9`). */
double dsd_realtime_now_s(void);

/* ---- Control ---- */

/** @brief Select the SYSTEM source. Call it only while no reader runs (see the file comment). */
void dsd_decode_clock_use_system(void);

/** @brief Earliest REPLAY anchor, in seconds since the Unix epoch: 2000-01-01T00:00:00Z. */
#define DSD_DECODE_CLOCK_ANCHOR_FLOOR_S 946684800LL

/** @brief Latest REPLAY anchor, in seconds since the Unix epoch (dsd_decode_clock_use_replay()). */
#define DSD_DECODE_CLOCK_ANCHOR_MAX_S   18000000000LL

/**
 * @brief Select the REPLAY source and reset media time to zero.
 *
 * The anchor is `max(anchor_utc_s, 2000-01-01T00:00:00Z)`; a zero, negative or
 * pre-2000 anchor is raised to that floor, which keeps a 1970-anchored capture's
 * stamps clear of the "0 = unset" sentinels. An anchor past
 * DSD_DECODE_CLOCK_ANCHOR_MAX_S (about the year 2540) is held there, so the
 * anchor in nanoseconds fits in 64 bits with some 14 years of media time to
 * spare.
 *
 * Call it only while no reader runs (see the file comment).
 */
void dsd_decode_clock_use_replay(int64_t anchor_utc_s);

/**
 * @brief Advance REPLAY media time (nanoseconds since the anchor).
 *
 * Monotone: a value smaller than the current media time is ignored. Only the
 * decoder thread may call this.
 */
void dsd_decode_clock_set_media_ns(uint64_t media_ns);

/**
 * @brief Media time of sample @p index of an I/Q replay batch.
 *
 * A batch of @p count samples spans [@p start_ns, @p start_ns + @p duration_ns)
 * of capture time, its samples evenly spaced: sample @p index sits at
 * `start_ns + index * duration_ns / count`, rounded down. The arithmetic is exact
 * in integers for any span and count (the 128-bit product is never formed). An
 * @p index past @p count is held at @p count, the batch's end; a @p count of 0
 * gives @p start_ns. The sum saturates at UINT64_MAX.
 */
uint64_t dsd_decode_clock_batch_media_ns(uint64_t start_ns, uint64_t duration_ns, uint32_t count, uint32_t index);

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
