// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Consumer for the demodulated-output ring owned by the RTL pipeline.
 */

#include <atomic>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/ring.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

extern "C" volatile uint8_t exitflag;

int
ring_read_batch(struct output_state* o, float* out, size_t max_count) {
    if (max_count == 0) {
        return 0;
    }
    if (!o || !o->buffer || !out) {
        return -1;
    }
    while (ring_is_empty(o)) {
        if (!o->buffer) {
            return -1;
        }
        dsd_mutex_lock(&o->ready_m);
        int ret = dsd_cond_timedwait(&o->ready, &o->ready_m, 10);
        dsd_mutex_unlock(&o->ready_m);
        if (ret != 0) {
            if (exitflag || !o->buffer) {
                return -1;
            }
            o->read_timeouts.fetch_add(1);
        }
    }

    size_t used = ring_used(o);
    size_t read_count = (used < max_count) ? used : max_count;
    size_t t = o->tail.load();
    size_t to_end = o->capacity - t;
    size_t first = (read_count < to_end) ? read_count : to_end;
    DSD_MEMCPY(out, o->buffer + t, first * sizeof(float));
    DSD_MEMCPY(out + first, o->buffer, (read_count - first) * sizeof(float));
    t = (t + read_count) % o->capacity;
    o->tail.store(t);
    dsd_mutex_lock(&o->ready_m);
    dsd_cond_signal(&o->space);
    dsd_mutex_unlock(&o->ready_m);
    return (int)read_count;
}
