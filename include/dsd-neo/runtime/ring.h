// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Output ring buffer API for demodulated audio samples.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RING_H_

#include <atomic>
#include <stdint.h>
#include <stdlib.h>

#include <dsd-neo/platform/threading.h>

struct output_state {
    int rate = 0;
    float* buffer = nullptr;
    size_t capacity = 0;
    std::atomic<size_t> head{0U};
    std::atomic<size_t> tail{0U};
    dsd_cond_t ready;
    dsd_mutex_t ready_m;
    dsd_cond_t space;
    std::atomic<uint64_t> write_timeouts{0U}; /* producer waited for space */
    std::atomic<uint64_t> read_timeouts{0U};  /* consumer waited for data */
};

/**
 * @brief Number of queued samples in the output ring.
 *
 * @param o Output ring state.
 * @return Number of queued samples.
 */
static inline size_t
ring_used(const struct output_state* o) {
    /* Atomics policy: head/tail are atomics. We use default sequential
       consistency for simplicity. In an SPSC ring, this could be relaxed
       to acquire/release without changing behavior. */
    size_t h = o->head.load();
    size_t t = o->tail.load();
    if (h >= t) {
        return h - t;
    }
    return o->capacity - t + h;
}

/**
 * @brief Number of writable samples before the ring becomes full.
 *
 * @param o Output ring state.
 * @return Number of writable samples before the ring becomes full.
 */
static inline size_t
ring_free(const struct output_state* o) {
    return (o->capacity - 1) - ring_used(o);
}

/**
 * @brief Check if the output ring is empty.
 *
 * @param o Output ring state.
 * @return Non-zero if empty, zero otherwise.
 */
static inline int
ring_is_empty(const struct output_state* o) {
    /* See atomics note in ring_used() for ordering considerations. */
    return o->head.load() == o->tail.load();
}

/**
 * @brief Clear the output ring head/tail indices.
 *
 * @param o Output ring state to clear.
 */
static inline void
ring_clear(struct output_state* o) {
    /* Clearing indices; with relaxed ordering this would be a release store. */
    o->tail.store(0);
    o->head.store(0);
}

/**
 * @brief Read up to max_count samples into out.
 *
 * Blocks until at least one sample is available or exit.
 *
 * @param o         Output ring buffer state.
 * @param out       Destination buffer for samples.
 * @param max_count Maximum number of samples to read.
 * @return Number of samples read (>=1) or -1 on exit.
 */
int ring_read_batch(struct output_state* o, float* out, size_t max_count);

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RING_H_ */
