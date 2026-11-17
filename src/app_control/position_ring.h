// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The published position ring (issue #800), private to app-control: its storage lives in ui_snapshot.c beside
 * the published event history and is guarded by the snapshot lock; position_view.c fills and reads it.
 */

#ifndef DSD_NEO_SRC_APP_CONTROL_POSITION_RING_H_
#define DSD_NEO_SRC_APP_CONTROL_POSITION_RING_H_

#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The records of one stream as last published: seq s sits at entries[(s - 1) % DSD_POSITION_FEED_CAPACITY], and the
 * ring holds every seq from max(1, newest_seq - DSD_POSITION_FEED_CAPACITY + 1) to newest_seq. Zero-initialized: no
 * stream, nothing held.
 */
typedef struct {
    dsd_position_fix entries[DSD_POSITION_FEED_CAPACITY];
    uint64_t stream_id;             /**< the stream the records belong to; 0 before the first run's first publish */
    uint64_t newest_seq;            /**< the newest seq held; 0 while empty */
    dsd_app_position_sample sample; /**< the clock sample of the last publish */
} dsd_app_position_ring;

/**
 * @brief The publish hop: copy the decoder's feed records above @p ring's newest into @p ring and take the clock
 * sample. A new live stream replaces the ring's records; a state with no live stream (no run) keeps them. No
 * allocation. Decoder thread, under the snapshot lock.
 */
void dsd_app_position_ring_publish(dsd_app_position_ring* ring, const dsd_state* state);

/**
 * @brief The read: dsd_app_position_copy_since()'s contract on @p ring. Under the snapshot lock; @p next is non-NULL
 * and @p out is non-NULL when @p cap is above 0 (the caller checked).
 */
int dsd_app_position_ring_read(const dsd_app_position_ring* ring, uint64_t stream_id, uint64_t after_seq,
                               dsd_position_fix* out, size_t cap, dsd_app_position_cursor* next);

/** ui_snapshot.c: dsd_app_position_ring_read() on the published ring, under the snapshot lock. */
int dsd_app_snapshot_position_copy_since(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                                         dsd_app_position_cursor* next);

#ifdef DSD_NEO_TEST_HOOKS
/** Records each hop copied: the publish hop's (decoder feed -> published ring) and the reads' (published ring ->
 * readers). */
typedef struct {
    uint64_t published;
    uint64_t read;
} dsd_app_position_copy_counts;

/** position_view.c, under the snapshot lock: write the counts so far into @p out (NULL: none), then zero them when
 * @p reset is nonzero. */
void dsd_app_position_ring_test_counts(dsd_app_position_copy_counts* out, int reset);
/** ui_snapshot.c: zero the counts, taking the snapshot lock. */
void dsd_app_position_test_reset_copy_counts(void);
/** ui_snapshot.c: the counts so far, taking the snapshot lock. */
void dsd_app_position_test_get_copy_counts(dsd_app_position_copy_counts* counts);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_APP_CONTROL_POSITION_RING_H_ */
