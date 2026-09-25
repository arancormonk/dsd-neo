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
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/ring.h>
#include <stddef.h>
#include "dsd-neo/core/safe_api.h"

/* Block until the ring holds a sample: 0, or -1 on exit. */
static int
ring_wait_not_empty(struct output_state* o) {
    while (ring_is_empty(o)) {
        if (!o->buffer) {
            return -1;
        }
        dsd_mutex_lock(&o->ready_m);
        int ret = dsd_cond_timedwait(&o->ready, &o->ready_m, 10);
        dsd_mutex_unlock(&o->ready_m);
        if (ret != 0) {
            if (dsd_exitflag_load() || !o->buffer) {
                return -1;
            }
            o->read_timeouts.fetch_add(1);
        }
    }
    return 0;
}

int
ring_read_batch(struct output_state* o, float* out, size_t max_count) {
    if (max_count == 0) {
        return 0;
    }
    if (!o || !o->buffer || !out) {
        return -1;
    }
    for (;;) {
        if (ring_wait_not_empty(o) != 0) {
            return -1;
        }
        /* From the tail snapshot to the tail store under ready_m, which a clear of the ring from another thread takes
           too (rtl_stream_clear_output_ring()): a clear landing in between would otherwise be undone by the store of
           the old tail. A clear that emptied the ring since the wait sends the read back to waiting. */
        dsd_mutex_lock(&o->ready_m);
        size_t used = ring_used(o);
        if (used == 0) {
            dsd_mutex_unlock(&o->ready_m);
            continue;
        }
        size_t read_count = (used < max_count) ? used : max_count;
        size_t t = o->tail.load();
        size_t to_end = o->capacity - t;
        size_t first = (read_count < to_end) ? read_count : to_end;
        DSD_MEMCPY(out, o->buffer + t, first * sizeof(float));
        DSD_MEMCPY(out + first, o->buffer, (read_count - first) * sizeof(float));
        t = (t + read_count) % o->capacity;
        o->tail.store(t);
        dsd_cond_signal(&o->space);
        dsd_mutex_unlock(&o->ready_m);
        return (int)read_count;
    }
}
