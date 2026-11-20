// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The published position ring's two halves (issue #800): the publish hop, which copies the decoder's new feed
 * records into it, and the cursor read every frontend uses. Both run under the snapshot lock (ui_snapshot.c owns the
 * ring and the lock); neither allocates.
 */

#include <dsd-neo/app_control/position_view.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stddef.h>
#include <stdint.h>
#include "position_ring.h"

#define POSITION_RING_CAPACITY ((uint64_t)DSD_POSITION_FEED_CAPACITY)

#ifdef DSD_NEO_TEST_HOOKS
static dsd_app_position_copy_counts g_counts; /* under the snapshot lock */
#define POSITION_COUNT_PUBLISHED(n) (g_counts.published += (uint64_t)(n))
#define POSITION_COUNT_READ(n)      (g_counts.read += (uint64_t)(n))

void
dsd_app_position_ring_test_counts(dsd_app_position_copy_counts* out, int reset) {
    if (out != NULL) {
        *out = g_counts;
    }
    if (reset) {
        DSD_MEMSET(&g_counts, 0, sizeof(g_counts));
    }
}
#else
#define POSITION_COUNT_PUBLISHED(n) ((void)(n))
#define POSITION_COUNT_READ(n)      ((void)(n))
#endif

/* The oldest seq a ring whose newest is @p newest holds: it keeps the last DSD_POSITION_FEED_CAPACITY. */
static uint64_t
position_ring_oldest(uint64_t newest) {
    if (newest == 0U) {
        return 0U;
    }
    return newest > POSITION_RING_CAPACITY ? newest - POSITION_RING_CAPACITY + 1U : 1U;
}

static size_t
position_ring_index(uint64_t seq) {
    return (size_t)((seq - 1U) % POSITION_RING_CAPACITY);
}

/* Copy the live records above ring->newest_seq, up to @p live_newest, into their own entries: at most two runs, split
   where the ring wraps. Records the live ring no longer holds are skipped (its oldest is where the copy starts). */
static void
position_ring_copy_new(dsd_app_position_ring* ring, const dsd_state* state, const dsd_position_feed_meta* live) {
    uint64_t after = ring->newest_seq;
    if (live->oldest_seq > 0U && after + 1U < live->oldest_seq) {
        after = live->oldest_seq - 1U;
    }
    for (int run = 0; run < 2 && after < live->newest_seq; run++) {
        const size_t index = position_ring_index(after + 1U);
        dsd_position_feed_meta meta;
        const int n = dsd_position_feed_copy_since(state, after, &ring->entries[index],
                                                   (size_t)DSD_POSITION_FEED_CAPACITY - index, &meta);
        if (n <= 0) {
            break;
        }
        POSITION_COUNT_PUBLISHED(n);
        after = meta.last_seq;
    }
    ring->newest_seq = after;
}

void
dsd_app_position_ring_publish(dsd_app_position_ring* ring, const dsd_state* state) {
    if (ring == NULL || state == NULL) {
        return;
    }
    dsd_position_feed_meta live;
    if (dsd_position_feed_copy_since(state, 0U, NULL, 0U, &live) < 0) {
        DSD_MEMSET(&live, 0, sizeof(live));
    }
    dsd_app_position_sample sample;
    DSD_MEMSET(&sample, 0, sizeof(sample));
    sample.stream_id = live.stream_id;
    sample.decode_now_s = (int64_t)dsd_decode_time();
    sample.clock_source = (uint8_t)dsd_decode_clock_source();
    sample.running = (live.stream_id != 0U && !dsd_exitflag_load()) ? 1U : 0U;
    ring->sample = sample;
    if (live.stream_id == 0U) {
        /* No run: the last run's records stay for a reader that has not caught up with them yet. */
        return;
    }
    if (live.stream_id != ring->stream_id || live.newest_seq < ring->newest_seq) {
        ring->stream_id = live.stream_id;
        ring->newest_seq = 0U;
    }
    position_ring_copy_new(ring, state, &live);
}

int
dsd_app_position_ring_read(const dsd_app_position_ring* ring, uint64_t stream_id, uint64_t after_seq,
                           dsd_position_fix* out, size_t cap, dsd_app_position_cursor* next) {
    DSD_MEMSET(next, 0, sizeof(*next));
    next->sample = ring->sample;
    next->stream_id = ring->stream_id;
    if (ring->stream_id != 0U && stream_id != 0U && stream_id != ring->stream_id) {
        next->reset = 1U;
        after_seq = 0U;
    }
    next->last_seq = after_seq;
    if (ring->stream_id == 0U || ring->newest_seq == 0U) {
        return 0;
    }
    const uint64_t newest = ring->newest_seq;
    const uint64_t oldest = position_ring_oldest(newest);
    next->oldest_seq = oldest;
    next->newest_seq = newest;
    if (after_seq >= newest) {
        return 0;
    }
    uint64_t start = after_seq + 1U;
    if (start < oldest) {
        next->lost = 1U;
        start = oldest;
    }
    const uint64_t available = newest - start + 1U;
    const uint64_t n = available < (uint64_t)cap ? available : (uint64_t)cap;
    if (n > 0U) {
        /* One or two runs, split where the ring wraps. */
        const size_t first_index = position_ring_index(start);
        size_t first = (size_t)DSD_POSITION_FEED_CAPACITY - first_index;
        if ((uint64_t)first > n) {
            first = (size_t)n;
        }
        DSD_MEMCPY(out, &ring->entries[first_index], first * sizeof(out[0]));
        if ((uint64_t)first < n) {
            DSD_MEMCPY(out + first, &ring->entries[0], ((size_t)n - first) * sizeof(out[0]));
        }
        next->last_seq = start + n - 1U;
        POSITION_COUNT_READ(n);
    }
    next->more = n < available ? 1U : 0U;
    return (int)n;
}

int
dsd_app_position_copy_since(uint64_t stream_id, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                            dsd_app_position_cursor* next) {
    if (next == NULL || (out == NULL && cap > 0U)) {
        return -1;
    }
    return dsd_app_snapshot_position_copy_since(stream_id, after_seq, out, cap, next);
}
