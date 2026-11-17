// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The verified position feed (<dsd-neo/core/position.h>) as frontends read it (issue #800): the records the
 * decoder last published, read through a cursor, from any thread.
 *
 * Every snapshot publish copies the decoder's new feed records, those above the last one it published, into one
 * published ring (DSD_POSITION_FEED_CAPACITY records, fixed storage) under the snapshot lock. A frontend reads that
 * ring with dsd_app_position_copy_since(): no allocation, no I/O, one copy under the same lock, and none of the
 * single-consumer snapshot accessors' state touched, so the Qt model, a background tap and any other reader can each
 * follow it with a cursor of their own.
 *
 * Streams: every decoder run is a new stream (dsd_position_fix::stream_id, never 0, never repeated in the process)
 * whose seq starts at 1. The published ring holds one stream at a time: the run's first publish replaces the previous
 * run's records. After a run ends, its records stay until the next run's first publish, and the run's final publish
 * (the engine publishes at the end of every run whose lifecycle started) carries its last records.
 *
 * Gaps: the ring keeps the newest DSD_POSITION_FEED_CAPACITY records. A reader that falls further behind (or the
 * decoder, between two publishes) loses the oldest; the cursor says so (lost), so a frontend can break a trail there
 * instead of drawing a line across the gap.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_POSITION_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_POSITION_VIEW_H_

#include <dsd-neo/core/position.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What the decoder said about its clock at the publish that made the ring a reader sees, taken by the decoder
 * thread at that publish and stored with the ring under the same lock, so a reader gets it with the records in one
 * read.
 *
 * A frontend ages a fix heard on the REPLAY clock (dsd_position_fix::rx_clock) only while the sample names that fix's
 * stream, is REPLAY and running: the age is decode_now_s - rx_time_s, which stops while a replay is paused, as the
 * decode clock does, and never mixes in the system clock or another capture's clock.
 */
typedef struct {
    /** The decoder's live stream at that publish; 0 when the decoder had none (no run, or its feed failed to start). */
    uint64_t stream_id;
    /** dsd_decode_time() at that publish: whole seconds on the decode clock, the unit of
     * dsd_position_fix::rx_time_s. */
    int64_t decode_now_s;
    /** dsd_decode_clock_source() at that publish (a dsd_decode_clock_source_t value). */
    uint8_t clock_source;
    /** 1 while the run is going: set by every publish of a run from its first one on; 0 from the moment the run is
     * stopping (the engine sets the exit flag before the run's final publish, so that publish clears it), and at a
     * publish with no live stream. */
    uint8_t running;
    uint8_t pad[6];
} dsd_app_position_sample;

/**
 * @brief Where a reader stands after dsd_app_position_copy_since(): pass stream_id and last_seq to the next call.
 */
typedef struct {
    uint64_t stream_id;  /**< the published stream; 0 before the first run's first publish */
    uint64_t last_seq;   /**< the last seq returned (the next after_seq); the effective after_seq when none was */
    uint64_t oldest_seq; /**< the oldest seq the published ring holds; 0 while it is empty */
    uint64_t newest_seq; /**< the newest seq the published ring holds; 0 while it is empty */
    uint8_t more;        /**< records remain past the ones returned (the call was given too small a cap) */
    /** records after the reader's position were overwritten before this read: a gap before the first one returned
     * (after a reset: the new stream's first records are gone) */
    uint8_t lost;
    /** the reader named another stream than the published one (a new run): the call read the published stream from
     * its start (after_seq 0). A first read (stream_id 0) is not a reset. */
    uint8_t reset;
    uint8_t pad[5];
    dsd_app_position_sample sample; /**< the clock sample stored with the ring the call read */
} dsd_app_position_cursor;

/**
 * @brief Copy up to @p cap published records with seq above @p after_seq, oldest first, into @p out.
 *
 * A reader starts with stream_id 0 and after_seq 0 and then passes the cursor's stream_id and last_seq. A stream_id
 * that is neither 0 nor the published stream is a reset: the call reads the published stream from its start. A reader
 * whose after_seq is older than oldest_seq - 1 gets the oldest records held and lost set. Any thread, any number of
 * readers, each with its own @p out and @p next; one copy under the snapshot lock, no allocation.
 *
 * @param stream_id The stream the reader follows (the last cursor's stream_id), or 0 for none yet.
 * @param after_seq The last seq the reader has (the last cursor's last_seq), or 0.
 * @param out Receives the records; may be NULL when @p cap is 0 (a read of the cursor alone).
 * @param cap Records @p out holds.
 * @param next Receives the cursor on every return >= 0 (required).
 * @return the number of records copied: 0 when none is new, or when nothing was published yet (then next->stream_id is
 *         0); -1 for a NULL @p next, or a NULL @p out with @p cap above 0.
 */
int dsd_app_position_copy_since(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                                dsd_app_position_cursor* next);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_POSITION_VIEW_H_ */
