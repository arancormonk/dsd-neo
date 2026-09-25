// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/ring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

extern "C" volatile uint8_t exitflag;

static int
init_output_ring(struct output_state* ring, size_t capacity) {
    DSD_MEMSET(ring, 0, sizeof *ring);
    ring->buffer = (float*)calloc(capacity, sizeof(float));
    if (!ring->buffer) {
        return -1;
    }
    ring->capacity = capacity;
    dsd_cond_init(&ring->ready);
    dsd_cond_init(&ring->space);
    dsd_mutex_init(&ring->ready_m);
    return 0;
}

static void
destroy_output_ring(struct output_state* ring) {
    dsd_mutex_destroy(&ring->ready_m);
    dsd_cond_destroy(&ring->ready);
    dsd_cond_destroy(&ring->space);
    free(ring->buffer);
}

namespace {
struct BatchReader {
    struct output_state* ring;
    float out[4];
    std::atomic<int> started{0};
    std::atomic<int> got{0};
};
} // namespace

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    batch_reader(void* arg) {
    BatchReader* reader = static_cast<BatchReader*>(arg);
    reader->started.store(1);
    reader->got.store(ring_read_batch(reader->ring, reader->out, 4U));
    DSD_THREAD_RETURN;
}

/*
 * The RTL stream clears the output ring from the demod or controller thread while the decoder reads it, and holds
 * ready_m for the clear. The read holds it from its tail snapshot to its tail store, so neither a read of the stale
 * samples nor the store of a tail taken before the clear can land around it. Here the clear and one new sample go in
 * while the mutex is held and a read of the stale backlog is on its way: the read returns only the new sample, and
 * the indices agree afterwards. How far the read got before the mutex was released does not change that outcome.
 */
static int
test_clear_under_mutex_excludes_read(void) {
    struct output_state ring;
    if (init_output_ring(&ring, 8U) != 0) {
        return 1;
    }
    for (size_t i = 0; i < 4U; i++) {
        ring.buffer[i] = 1.0f + (float)i;
    }
    ring.head.store(4U);
    BatchReader reader;
    reader.ring = &ring;
    dsd_mutex_lock(&ring.ready_m);
    dsd_thread_t thread{};
    if (dsd_thread_create(&thread, batch_reader, &reader) != 0) {
        dsd_mutex_unlock(&ring.ready_m);
        destroy_output_ring(&ring);
        return 1;
    }
    for (int waited_ms = 0; waited_ms < 5000 && !reader.started.load(); waited_ms++) {
        dsd_sleep_ms(1);
    }
    dsd_sleep_ms(20); /* give the read time to reach the ring */
    ring_clear(&ring);
    ring.buffer[0] = 99.0f;
    ring.head.store(1U);
    dsd_mutex_unlock(&ring.ready_m);
    (void)dsd_thread_join(thread);
    const int got = reader.got.load();
    const int ok =
        got == 1 && std::fabs(reader.out[0] - 99.0f) <= 1.0e-6f && ring.tail.load() == 1U && ring.head.load() == 1U;
    if (!ok) {
        DSD_FPRINTF(stderr, "output ring read across a clear: got=%d first=%.1f tail=%zu head=%zu\n", got,
                    (double)reader.out[0], ring.tail.load(), ring.head.load());
    }
    destroy_output_ring(&ring);
    return ok ? 0 : 1;
}

int
main(void) {
    float out[8] = {0};
    struct output_state ring;
    if (ring_read_batch(NULL, out, 1U) != -1 || init_output_ring(&ring, 8U) != 0) {
        return 1;
    }
    if (ring_read_batch(&ring, out, 0U) != 0 || ring_read_batch(&ring, NULL, 1U) != -1) {
        destroy_output_ring(&ring);
        return 1;
    }

    ring.tail.store(6U);
    ring.head.store(4U);
    ring.buffer[6] = 10.0f;
    ring.buffer[7] = 20.0f;
    ring.buffer[0] = 30.0f;
    ring.buffer[1] = 40.0f;
    ring.buffer[2] = 50.0f;
    ring.buffer[3] = 60.0f;
    int count = ring_read_batch(&ring, out, 6U);
    const float expected[6] = {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f};
    bool values_match = true;
    for (size_t i = 0; i < 6U; i++) {
        if (std::fabs(out[i] - expected[i]) > 1.0e-6f) {
            values_match = false;
            break;
        }
    }
    if (count != 6 || ring.tail.load() != 4U || !values_match) {
        DSD_FPRINTF(stderr, "output ring wrapped batch mismatch\n");
        destroy_output_ring(&ring);
        return 1;
    }

    exitflag = 1U;
    count = ring_read_batch(&ring, out, 1U);
    exitflag = 0U;
    destroy_output_ring(&ring);
    if (count != -1) {
        DSD_FPRINTF(stderr, "output ring exit guard mismatch\n");
        return 1;
    }
    return test_clear_under_mutex_excludes_read();
}
