// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/platform/threading.h>
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
    return 0;
}
