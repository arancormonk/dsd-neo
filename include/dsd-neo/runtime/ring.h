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
#include <stddef.h>
#include <stdint.h>

#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>

/* The flag bit (struct output_state::flags) that says the voice lane ran for a sample and its voice sample is in
   struct output_state::voice. The squelch's DSD_SQUELCH_FLAG_CLOSED is bit 0. */
#define DSD_RING_FLAG_VOICE 0x80U

struct output_state {
    /* Sample rate of the ring's contents. Written by the controller and demod threads (rate-chain finalize, family
       switch) and read by the decoder and UI threads through dsd_rtl_stream_output_rate(). */
    std::atomic<int> rate{0};
    float* buffer = nullptr;
    /* One flag byte per sample, at the sample's position (the auto squelch's DSD_SQUELCH_FLAG_CLOSED; 0 is open):
       written with the samples before the head that publishes both, and read with them. NULL: every sample open.
       DSD_RING_FLAG_VOICE marks a sample the voice lane ran for; the stream's reads hand back the other bits only. */
    uint8_t* flags = nullptr;
    /* The FSK voice lane's sample at each position DSD_RING_FLAG_VOICE marks (issue #518), written with the sample and
       its flag; what the other positions hold is never read. NULL: no voice lane. */
    float* voice = nullptr;
    /* Set (for good) once the demod first hands a block with the voice lane, before the head that publishes it: until
       then no flag carries DSD_RING_FLAG_VOICE, and the reads leave the flags as they are. */
    std::atomic<int> voice_written{0};
    size_t capacity = 0;
    std::atomic<size_t> head{0U};
    std::atomic<size_t> tail{0U};
    /* Set up for monotonic waits (dsd_cond_init_monotonic()), and waited on only through output_ring_wait_ready(). */
    dsd_cond_t ready;
    dsd_mutex_t ready_m;
    dsd_cond_t space;
    std::atomic<uint64_t> write_timeouts{0U}; /* producer waited for space */
    std::atomic<uint64_t> read_timeouts{0U};  /* consumer waited for data */
};

/**
 * @brief Wait on @p o's ready condition, @p o->ready_m held, at most @p timeout_ms to a monotonic deadline, so a wall
 * clock set back never stretches the wait (and with it a hunt's service of what waits on the decoder).
 *
 * @return 0 when signaled, non-zero on timeout or error.
 */
static inline int
output_ring_wait_ready(struct output_state* o, unsigned int timeout_ms) {
    return dsd_cond_timedwait_monotonic(&o->ready, &o->ready_m,
                                        dsd_realtime_mono_ns() + (uint64_t)timeout_ms * 1000000ULL);
}

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
 * A consumer holds ready_m from its tail snapshot to its tail store (ring_read_batch()), so a caller that clears the
 * ring while a consumer may be reading holds ready_m too; otherwise the consumer's store of its old tail can land
 * after the clear and turn the reset indices into a backlog of stale samples.
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
