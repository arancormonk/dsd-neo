// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Simple runtime ring buffer tests for input/output paths.
 *
 * Exercises wrap-around behavior and basic FIFO semantics for the
 * SPSC input_ring_state and output_state helpers.
 */

#include <atomic>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/ring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

extern "C" volatile uint8_t exitflag;

/* RTL-SDR stream exit shim (when USE_RTLSDR is enabled in runtime) */
extern "C" int
dsd_rtl_stream_should_exit(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

static int
float_arrays_equal(const float* a, const float* b, size_t count) {
    for (size_t i = 0; i < count; i++) {
        float diff = a[i] - b[i];
        if (diff < -1.0e-6f || diff > 1.0e-6f) {
            return 0;
        }
    }
    return 1;
}

static int
test_input_ring_wrap_and_read(void) {
    const size_t cap = 8;
    struct input_ring_state r;
    DSD_MEMSET(&r, 0, sizeof(r));

    if (input_ring_init(&r, cap) != 0) {
        DSD_FPRINTF(stderr, "input_ring: init failed\n");
        return 1;
    }

    /* First write: no wrap, fills positions [0..5] */
    float src1[6] = {10, 20, 30, 40, 50, 60};
    input_ring_write(&r, src1, 6);

    /* Read three samples to advance tail -> positions [0..2] */
    float out[8] = {0};
    int read = input_ring_read_block(&r, out, 3);
    if (read != 3) {
        DSD_FPRINTF(stderr, "input_ring: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 10 || out[1] != 20 || out[2] != 30) {
        DSD_FPRINTF(stderr, "input_ring: first read mismatch\n");
        return 1;
    }

    /* Second write: triggers wrap-around from head near end of buffer */
    float src2[3] = {70, 80, 90};
    input_ring_write(&r, src2, 3);

    /* Queue should now contain {40,50,60,70,80,90} */
    size_t used = input_ring_used(&r);
    if (used != 6) {
        DSD_FPRINTF(stderr, "input_ring: expected used=6, got %zu\n", used);
        return 1;
    }

    DSD_MEMSET(out, 0, sizeof(out));
    read = input_ring_read_block(&r, out, 6);
    if (read != 6) {
        DSD_FPRINTF(stderr, "input_ring: expected read 6, got %d\n", read);
        return 1;
    }
    const float expect[6] = {40, 50, 60, 70, 80, 90};
    if (!float_arrays_equal(out, expect, sizeof(expect) / sizeof(expect[0]))) {
        DSD_FPRINTF(stderr, "input_ring: wrap/read sequence mismatch\n");
        return 1;
    }

    input_ring_destroy(&r);
    return 0;
}

static int
test_input_ring_drop_on_full(void) {
    const size_t cap = 4;
    struct input_ring_state r;
    DSD_MEMSET(&r, 0, sizeof(r));

    if (input_ring_init(&r, cap) != 0) {
        DSD_FPRINTF(stderr, "input_ring drop: init failed\n");
        return 1;
    }

    /* Fill ring to capacity-1 (maximum usable occupancy) */
    float initial[3] = {1, 2, 3};
    input_ring_write(&r, initial, 3);
    if (input_ring_used(&r) != 3) {
        DSD_FPRINTF(stderr, "input_ring drop: expected used=3 after initial write, got %zu\n", input_ring_used(&r));
        return 1;
    }

    /* Write more than available space -> should be dropped, not overwrite */
    r.producer_drops.store(0);
    float extra[2] = {9, 10};
    input_ring_write(&r, extra, 2);

    if (input_ring_used(&r) != 3) {
        DSD_FPRINTF(stderr, "input_ring drop: expected used=3 after drop write, got %zu\n", input_ring_used(&r));
        return 1;
    }
    if (r.producer_drops.load() != 2) {
        DSD_FPRINTF(stderr, "input_ring drop: expected producer_drops=2, got %llu\n",
                    (unsigned long long)r.producer_drops.load());
        return 1;
    }

    /* Ensure the original data is still present and in order */
    float out[4] = {0};
    int read = input_ring_read_block(&r, out, 3);
    if (read != 3) {
        DSD_FPRINTF(stderr, "input_ring drop: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 1 || out[1] != 2 || out[2] != 3) {
        DSD_FPRINTF(stderr, "input_ring drop: queue contents corrupted after drop\n");
        return 1;
    }

    input_ring_destroy(&r);
    return 0;
}

static int
test_output_ring_wrap_and_read(void) {
    const size_t cap = 8;
    struct output_state o;
    DSD_MEMSET(&o, 0, sizeof(o));

    o.buffer = (float*)calloc(cap, sizeof(float));
    if (!o.buffer) {
        DSD_FPRINTF(stderr, "output_ring: allocation failed\n");
        return 1;
    }
    o.capacity = cap;
    o.head.store(0);
    o.tail.store(0);
    o.write_timeouts.store(0);
    o.read_timeouts.store(0);
    dsd_cond_init(&o.ready);
    dsd_mutex_init(&o.ready_m);
    dsd_cond_init(&o.space);

    /* First write: no wrap, fills positions [0..5] */
    float src1[6] = {1, 2, 3, 4, 5, 6};
    ring_write_no_signal(&o, src1, 6);

    /* Read three samples to advance tail -> positions [0..2] */
    float out[8] = {0};
    int read = ring_read_batch(&o, out, 3);
    if (read != 3) {
        DSD_FPRINTF(stderr, "output_ring: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 1 || out[1] != 2 || out[2] != 3) {
        DSD_FPRINTF(stderr, "output_ring: first read mismatch\n");
        return 1;
    }

    /* Second write: triggers wrap-around from head near end of buffer */
    float src2[3] = {7, 8, 9};
    ring_write_no_signal(&o, src2, 3);

    /* Queue should now contain {4,5,6,7,8,9} */
    size_t used = ring_used(&o);
    if (used != 6) {
        DSD_FPRINTF(stderr, "output_ring: expected used=6, got %zu\n", used);
        return 1;
    }

    DSD_MEMSET(out, 0, sizeof(out));
    read = ring_read_batch(&o, out, 6);
    if (read != 6) {
        DSD_FPRINTF(stderr, "output_ring: expected read 6, got %d\n", read);
        return 1;
    }
    const float expect[6] = {4, 5, 6, 7, 8, 9};
    if (!float_arrays_equal(out, expect, sizeof(expect) / sizeof(expect[0]))) {
        DSD_FPRINTF(stderr, "output_ring: wrap/read sequence mismatch\n");
        return 1;
    }

    dsd_cond_destroy(&o.space);
    dsd_mutex_destroy(&o.ready_m);
    dsd_cond_destroy(&o.ready);
    free(o.buffer);
    return 0;
}

static int
init_output_ring(struct output_state* o, size_t cap) {
    DSD_MEMSET(o, 0, sizeof(*o));
    o->buffer = (float*)calloc(cap, sizeof(float));
    if (!o->buffer) {
        return -1;
    }
    o->capacity = cap;
    o->head.store(0);
    o->tail.store(0);
    o->write_timeouts.store(0);
    o->read_timeouts.store(0);
    dsd_cond_init(&o->ready);
    dsd_mutex_init(&o->ready_m);
    dsd_cond_init(&o->space);
    return 0;
}

static void
destroy_output_ring(struct output_state* o) {
    dsd_cond_destroy(&o->space);
    dsd_mutex_destroy(&o->ready_m);
    dsd_cond_destroy(&o->ready);
    free(o->buffer);
    o->buffer = NULL;
}

static int
test_output_ring_guards_and_single_reads(void) {
    float out = 99.0f;
    float data[4] = {10, 20, 30, 40};
    float batch[4] = {0};
    struct output_state o;
    DSD_MEMSET(&o, 0, sizeof(o));

    ring_write(NULL, data, 1);
    ring_write_no_signal(NULL, data, 1);
    if (ring_read_one(NULL, &out) != -1 || ring_read_batch(NULL, batch, 1) != -1 || ring_read_batch(&o, batch, 1) != -1
        || ring_read_batch(&o, batch, 0) != 0) {
        DSD_FPRINTF(stderr, "output_ring guards: NULL/no-buffer guards failed\n");
        return 1;
    }

    if (init_output_ring(&o, 6) != 0) {
        DSD_FPRINTF(stderr, "output_ring guards: allocation failed\n");
        return 1;
    }

    ring_write(&o, data, 1);
    if (ring_used(&o) != 1 || ring_read_one(&o, &out) != 0 || out != 10.0f || ring_used(&o) != 0) {
        DSD_FPRINTF(stderr, "output_ring guards: write/read-one simple path failed\n");
        destroy_output_ring(&o);
        return 1;
    }

    o.head.store(5);
    o.tail.store(3);
    ring_write_no_signal(&o, data, 1);
    if (o.head.load() != 0 || o.buffer[5] != 10.0f) {
        DSD_FPRINTF(stderr, "output_ring guards: no-signal exact-end write failed\n");
        destroy_output_ring(&o);
        return 1;
    }

    ring_clear(&o);
    o.head.store(4);
    o.tail.store(2);
    ring_write(&o, data + 1, 3);
    if (o.head.load() != 1 || o.buffer[4] != 20.0f || o.buffer[5] != 30.0f || o.buffer[0] != 40.0f) {
        DSD_FPRINTF(stderr, "output_ring guards: ring_write wrap split failed\n");
        destroy_output_ring(&o);
        return 1;
    }

    ring_clear(&o);
    ring_write_signal_on_empty_transition(&o, data, 2);
    if (ring_used(&o) != 2) {
        DSD_FPRINTF(stderr, "output_ring guards: signal-on-empty write failed\n");
        destroy_output_ring(&o);
        return 1;
    }

    ring_clear(&o);
    o.tail.store(5);
    o.head.store(0);
    o.buffer[5] = 77.0f;
    out = 0.0f;
    if (ring_read_one(&o, &out) != 0 || out != 77.0f || o.tail.load() != 0) {
        DSD_FPRINTF(stderr, "output_ring guards: read-one wrap failed\n");
        destroy_output_ring(&o);
        return 1;
    }

    ring_clear(&o);
    exitflag = 1;
    if (ring_read_one(&o, &out) != -1 || ring_read_batch(&o, batch, 2) != -1) {
        DSD_FPRINTF(stderr, "output_ring guards: empty read exit paths failed\n");
        exitflag = 0;
        destroy_output_ring(&o);
        return 1;
    }
    exitflag = 0;

    destroy_output_ring(&o);
    return 0;
}

struct OutputWriterArgs { // NOLINT(misc-use-internal-linkage)
    struct output_state* ring;
    const float* data;
    size_t count;
    dsd_mutex_t* mu;
    dsd_cond_t* cv;
    int* ready_flag;
};

struct OutputReaderArgs { // NOLINT(misc-use-internal-linkage)
    struct output_state* ring;
    size_t total_expected;
    float* out;
    size_t* out_count;
    int* error_flag;
};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    output_writer_thread(void* arg) {
    OutputWriterArgs* ctx = (OutputWriterArgs*)arg;
    /* Signal that writer is about to start the blocking write */
    dsd_mutex_lock(ctx->mu);
    *(ctx->ready_flag) = 1;
    dsd_cond_signal(ctx->cv);
    dsd_mutex_unlock(ctx->mu);

    ring_write(ctx->ring, ctx->data, ctx->count);
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    output_reader_thread(void* arg) {
    OutputReaderArgs* ctx = (OutputReaderArgs*)arg;
    size_t have = 0;
    float tmp[16];
    while (have < ctx->total_expected) {
        int n = ring_read_batch(ctx->ring, tmp, 8);
        if (n < 0) {
            *(ctx->error_flag) = 1;
            DSD_THREAD_RETURN;
        }
        DSD_MEMCPY(ctx->out + have, tmp, (size_t)n * sizeof(float));
        have += (size_t)n;
    }
    *(ctx->out_count) = have;
    DSD_THREAD_RETURN;
}

static int
test_output_ring_blocking_producer_consumer(void) {
    const size_t cap = 4;
    struct output_state o;
    DSD_MEMSET(&o, 0, sizeof(o));

    o.buffer = (float*)calloc(cap, sizeof(float));
    if (!o.buffer) {
        DSD_FPRINTF(stderr, "output_ring pc: allocation failed\n");
        return 1;
    }
    o.capacity = cap;
    o.head.store(0);
    o.tail.store(0);
    o.write_timeouts.store(0);
    o.read_timeouts.store(0);
    dsd_cond_init(&o.ready);
    dsd_mutex_init(&o.ready_m);
    dsd_cond_init(&o.space);

    /* Prefill ring to capacity-1 so the next producer write observes full state */
    float pre[3] = {100, 101, 102};
    ring_write_no_signal(&o, pre, 3);
    if (ring_used(&o) != 3) {
        DSD_FPRINTF(stderr, "output_ring pc: expected used=3 after prefill, got %zu\n", ring_used(&o));
        return 1;
    }

    float bulk[10];
    for (int i = 0; i < 10; i++) {
        bulk[i] = (float)(200 + i);
    }

    dsd_mutex_t barrier_mu;
    dsd_cond_t barrier_cv;
    int writer_ready = 0;
    dsd_mutex_init(&barrier_mu);
    dsd_cond_init(&barrier_cv);

    float all[16] = {0};
    size_t all_count = 0;
    int read_error = 0;

    OutputWriterArgs wargs;
    wargs.ring = &o;
    wargs.data = bulk;
    wargs.count = 10;
    wargs.mu = &barrier_mu;
    wargs.cv = &barrier_cv;
    wargs.ready_flag = &writer_ready;

    OutputReaderArgs rargs;
    rargs.ring = &o;
    rargs.total_expected = 13; /* 3 pre + 10 bulk */
    rargs.out = all;
    rargs.out_count = &all_count;
    rargs.error_flag = &read_error;

    dsd_thread_t wthread;
    dsd_thread_t rthread;

    if (dsd_thread_create(&wthread, output_writer_thread, &wargs) != 0) {
        DSD_FPRINTF(stderr, "output_ring pc: failed to create writer thread\n");
        return 1;
    }

    /* Wait until writer has entered its write path (and likely observed a full ring) */
    dsd_mutex_lock(&barrier_mu);
    while (!writer_ready) {
        dsd_cond_wait(&barrier_cv, &barrier_mu);
    }
    dsd_mutex_unlock(&barrier_mu);

    if (dsd_thread_create(&rthread, output_reader_thread, &rargs) != 0) {
        DSD_FPRINTF(stderr, "output_ring pc: failed to create reader thread\n");
        return 1;
    }

    dsd_thread_join(wthread);
    dsd_thread_join(rthread);

    if (read_error != 0) {
        DSD_FPRINTF(stderr, "output_ring pc: reader saw error\n");
        return 1;
    }
    if (all_count != 13) {
        DSD_FPRINTF(stderr, "output_ring pc: expected 13 samples, got %zu\n", all_count);
        return 1;
    }

    /* First three samples must be the prefilled values (FIFO), remainder the bulk sequence */
    float d0 = all[0] - 100.0f;
    float d1 = all[1] - 101.0f;
    float d2 = all[2] - 102.0f;
    if (d0 < -1.0e-6f || d0 > 1.0e-6f || d1 < -1.0e-6f || d1 > 1.0e-6f || d2 < -1.0e-6f || d2 > 1.0e-6f) {
        DSD_FPRINTF(stderr, "output_ring pc: prefilled samples out of order\n");
        return 1;
    }
    for (int i = 0; i < 10; i++) {
        float diff = all[3 + i] - (float)(200 + i);
        if (diff < -1.0e-6f || diff > 1.0e-6f) {
            DSD_FPRINTF(stderr, "output_ring pc: bulk sample mismatch at index %d (got %.1f, expected %d)\n", i,
                        all[3 + i], 200 + i);
            return 1;
        }
    }

    if (ring_used(&o) != 0) {
        DSD_FPRINTF(stderr, "output_ring pc: expected ring empty after producer/consumer, got used=%zu\n",
                    ring_used(&o));
        return 1;
    }

    dsd_cond_destroy(&barrier_cv);
    dsd_mutex_destroy(&barrier_mu);
    dsd_cond_destroy(&o.space);
    dsd_mutex_destroy(&o.ready_m);
    dsd_cond_destroy(&o.ready);
    free(o.buffer);
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_input_ring_wrap_and_read();
    rc |= test_output_ring_wrap_and_read();
    rc |= test_input_ring_drop_on_full();
    rc |= test_output_ring_guards_and_single_reads();
    rc |= test_output_ring_blocking_producer_consumer();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "runtime ring tests: OK\n");
    }
    return rc;
}
