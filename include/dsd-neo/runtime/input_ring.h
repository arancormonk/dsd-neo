// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Input ring buffer API for interleaved I/Q float samples.
 *
 * Declares the simple SPSC input ring and operations to reserve, commit,
 * and blockingly read samples with wrap-around handling.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_RING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_RING_H_

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/platform/threading.h>

/** Gap marks the input ring holds at once (input_ring_note_gap()); a producer with more pending moves the newest. */
#define INPUT_RING_GAP_SLOTS 32U

/* Simple SPSC ring for interleaved I/Q float samples (input path) */
struct input_ring_state {
    float* buffer = nullptr;
    size_t capacity = 0; /* in float elements */
    std::atomic<size_t> head{0U};
    std::atomic<size_t> tail{0U};
    dsd_cond_t ready;
    dsd_mutex_t ready_m;
    dsd_cond_t space;
    std::atomic<int> space_notify_enabled{0};
    std::atomic<uint64_t> producer_drops{0U}; /* bytes dropped when full */
    std::atomic<uint64_t> read_timeouts{0U};  /* waits for data */
    std::atomic<uint64_t> discard_generation{0U};
    /* Stream positions in floats, counted since init: what the producer committed and what the consumer released
       (read, committed or discarded). The consumer is at stream position consumed_total, the sample at tail. */
    std::atomic<uint64_t> produced_total{0U};
    std::atomic<uint64_t> consumed_total{0U};
    /* Gap marks (issue #806 X12): the stream positions where samples the producer lost would have been, so the
       samples after one do not follow the ones before it. A SPSC queue: the producer writes gap_at[gap_write % N] and
       moves gap_write, the consumer reads from gap_read. */
    std::atomic<uint64_t> gap_at[INPUT_RING_GAP_SLOTS];
    std::atomic<uint64_t> gap_write{0U};
    std::atomic<uint64_t> gap_read{0U};
};

/**
 * @brief Number of samples currently in the input ring.
 */
static inline size_t
input_ring_used(const struct input_ring_state* r) {
    size_t h = r->head.load();
    size_t t = r->tail.load();
    if (h >= t) {
        return h - t;
    }
    return r->capacity - t + h;
}

/**
 * @brief Number of free slots available for writing in the input ring.
 */
static inline size_t
input_ring_free(const struct input_ring_state* r) {
    return (r->capacity - 1) - input_ring_used(r);
}

/**
 * @brief Check if the input ring is empty.
 */
static inline int
input_ring_is_empty(const struct input_ring_state* r) {
    return r->head.load() == r->tail.load();
}

/**
 * @brief Publish that in-flight producer reservations are stale.
 *
 * Consumer-side purges can only move the consumer-owned tail. This generation
 * lets producer callbacks detect a purge that happened after they reserved
 * ring space but before they committed it.
 */
static inline void
input_ring_request_discard(struct input_ring_state* r) {
    if (!r) {
        return;
    }
    (void)r->discard_generation.fetch_add(1, std::memory_order_acq_rel);
}

/**
 * @brief Return the current producer discard generation.
 */
static inline uint64_t
input_ring_discard_generation(const struct input_ring_state* r) {
    return r ? r->discard_generation.load(std::memory_order_acquire) : 0ULL;
}

/**
 * @brief Check whether a producer reservation is still current.
 */
static inline int
input_ring_discard_generation_matches(const struct input_ring_state* r, uint64_t generation) {
    return input_ring_discard_generation(r) == generation;
}

/**
 * @brief Initialize input ring storage and synchronization primitives.
 *
 * @param r Input ring state.
 * @param capacity Number of float elements in the ring (must be > 0).
 * @return 0 on success, -1 on invalid args or allocation/init failure.
 */
int input_ring_init(struct input_ring_state* r, size_t capacity);

/**
 * @brief Destroy an initialized input ring.
 *
 * Safe to call multiple times; no-op on NULL.
 *
 * @param r Input ring state.
 */
void input_ring_destroy(struct input_ring_state* r);

/**
 * @brief Enable or disable consumer->producer space notifications.
 *
 * When enabled, input_ring_read_block() signals `space` after consuming data.
 *
 * @param r Input ring state.
 * @param enabled Non-zero to enable notifications, zero to disable.
 */
void input_ring_enable_space_notify(struct input_ring_state* r, int enabled);

/**
 * @brief Reserve writable regions in the input ring buffer.
 *
 * @param r          Input ring buffer state.
 * @param min_needed Minimum number of samples needed.
 * @param p1         [out] First writable region pointer.
 * @param n1         [out] First writable region length.
 * @param p2         [out] Second writable region pointer or NULL.
 * @param n2         [out] Second writable region length.
 * @return Total writable samples granted across regions.
 */
int input_ring_reserve(struct input_ring_state* r, size_t min_needed, float** p1, size_t* n1, float** p2, size_t* n2);

/**
 * @brief Commit previously reserved writable regions to the input ring.
 *
 * @param r         Input ring buffer state.
 * @param produced  Number of samples produced to commit.
 */
void input_ring_commit(struct input_ring_state* r, size_t produced);

/**
 * @brief Read up to max_count samples from the input ring, blocking until available.
 *
 * @param r         Input ring buffer state.
 * @param out       Destination buffer for samples.
 * @param max_count Maximum number of samples to read.
 * @return Number of samples read (>=1), 0 if max_count is 0, or -1 on exit.
 */
int input_ring_read_block(struct input_ring_state* r, float* out, size_t max_count);

/**
 * @brief Reserve readable regions in the input ring without copying.
 *
 * Blocks until at least one sample is available, then returns up to
 * @p max_count samples as one or two contiguous spans. The caller owns the
 * returned spans until it calls input_ring_read_commit().
 *
 * @param r         Input ring state.
 * @param max_count Maximum number of samples to reserve.
 * @param p1        [out] First readable region pointer.
 * @param n1        [out] First readable region length.
 * @param p2        [out] Second readable region pointer or NULL.
 * @param n2        [out] Second readable region length.
 * @return Number of reserved samples, 0 if max_count is 0, or -1 on exit/error.
 */
int input_ring_read_reserve(struct input_ring_state* r, size_t max_count, float** p1, size_t* n1, float** p2,
                            size_t* n2);

/**
 * @brief Commit samples previously obtained with input_ring_read_reserve().
 *
 * @param r        Input ring state.
 * @param consumed Number of samples consumed from the reserved spans.
 */
void input_ring_read_commit(struct input_ring_state* r, size_t consumed);

/**
 * @brief Discard all pending samples (consumer-side purge).
 *
 * Safe for the consumer thread to call; only updates tail to match the latest
 * head snapshot without touching head (producer-owned).
 */
static inline void
input_ring_discard_all_consumer(struct input_ring_state* r) {
    size_t h = r->head.load(std::memory_order_acquire);
    size_t t = r->tail.load(std::memory_order_relaxed);
    size_t discarded = (h >= t) ? h - t : r->capacity - t + h;
    r->tail.store(h, std::memory_order_release);
    r->consumed_total.store(r->consumed_total.load(std::memory_order_relaxed) + discarded, std::memory_order_relaxed);
}

/**
 * @brief Producer: mark a gap at the current end of the ring (issue #806 X12).
 *
 * Call when samples were lost before they reached the ring (a driver overflow, a full ring dropping the rest of a
 * transfer): the next sample committed does not follow the last one. The mark travels with the ring's data, so the
 * consumer finds it exactly between the two (input_ring_take_gap()) however it splits its reads. A second mark at
 * the same place adds nothing. With INPUT_RING_GAP_SLOTS marks pending the newest moves here instead, so the gap
 * before it goes unmarked; only a consumer that far behind loses one.
 */
void input_ring_note_gap(struct input_ring_state* r);

/** @brief Consumer: whether any gap mark is pending (one load each, for the per-block check). */
static inline int
input_ring_gap_pending(const struct input_ring_state* r) {
    return r->gap_read.load(std::memory_order_relaxed) != r->gap_write.load(std::memory_order_acquire);
}

/**
 * @brief Consumer: take the gap marks up to the read position.
 *
 * Marks the consumer has gone past (a purge discarded the samples around them) are dropped. A mark at the read
 * position is taken and reported in @p out_gap_at_read: the next sample read follows a gap.
 *
 * @param r                Input ring state.
 * @param out_gap_at_read  [out] 1 when a gap mark sat at the read position, else 0.
 * @return The floats the consumer may read before the next pending mark, or SIZE_MAX when none is pending.
 */
size_t input_ring_take_gap(struct input_ring_state* r, int* out_gap_at_read);

/**
 * @brief Start the stream positions and gap marks over (the ring's storage was replaced, its contents dropped).
 *
 * Only with no producer or consumer running.
 */
void input_ring_reset_positions(struct input_ring_state* r);

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_INPUT_RING_H_ */
