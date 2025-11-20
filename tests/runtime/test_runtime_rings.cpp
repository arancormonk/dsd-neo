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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/ring.h>

/* Global exit flag referenced by runtime ring implementations */
volatile uint8_t exitflag = 0;

/* RTL-SDR stream exit shim (when USE_RTLSDR is enabled in runtime) */
extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

static int
test_input_ring_wrap_and_read(void) {
    const size_t cap = 8;
    struct input_ring_state r;
    memset(&r, 0, sizeof(r));

    r.buffer = (int16_t*)calloc(cap, sizeof(int16_t));
    if (!r.buffer) {
        fprintf(stderr, "input_ring: allocation failed\n");
        return 1;
    }
    r.capacity = cap;
    r.head.store(0);
    r.tail.store(0);
    r.producer_drops.store(0);
    r.read_timeouts.store(0);
    pthread_cond_init(&r.ready, NULL);
    pthread_mutex_init(&r.ready_m, NULL);

    /* First write: no wrap, fills positions [0..5] */
    int16_t src1[6] = {10, 20, 30, 40, 50, 60};
    input_ring_write(&r, src1, 6);

    /* Read three samples to advance tail -> positions [0..2] */
    int16_t out[8] = {0};
    int read = input_ring_read_block(&r, out, 3);
    if (read != 3) {
        fprintf(stderr, "input_ring: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 10 || out[1] != 20 || out[2] != 30) {
        fprintf(stderr, "input_ring: first read mismatch\n");
        return 1;
    }

    /* Second write: triggers wrap-around from head near end of buffer */
    int16_t src2[3] = {70, 80, 90};
    input_ring_write(&r, src2, 3);

    /* Queue should now contain {40,50,60,70,80,90} */
    size_t used = input_ring_used(&r);
    if (used != 6) {
        fprintf(stderr, "input_ring: expected used=6, got %zu\n", used);
        return 1;
    }

    memset(out, 0, sizeof(out));
    read = input_ring_read_block(&r, out, 6);
    if (read != 6) {
        fprintf(stderr, "input_ring: expected read 6, got %d\n", read);
        return 1;
    }
    const int16_t expect[6] = {40, 50, 60, 70, 80, 90};
    if (memcmp(out, expect, sizeof(expect)) != 0) {
        fprintf(stderr, "input_ring: wrap/read sequence mismatch\n");
        return 1;
    }

    pthread_mutex_destroy(&r.ready_m);
    pthread_cond_destroy(&r.ready);
    free(r.buffer);
    return 0;
}

static int
test_input_ring_drop_on_full(void) {
    const size_t cap = 4;
    struct input_ring_state r;
    memset(&r, 0, sizeof(r));

    r.buffer = (int16_t*)calloc(cap, sizeof(int16_t));
    if (!r.buffer) {
        fprintf(stderr, "input_ring drop: allocation failed\n");
        return 1;
    }
    r.capacity = cap;
    r.head.store(0);
    r.tail.store(0);
    r.producer_drops.store(0);
    r.read_timeouts.store(0);
    pthread_cond_init(&r.ready, NULL);
    pthread_mutex_init(&r.ready_m, NULL);

    /* Fill ring to capacity-1 (maximum usable occupancy) */
    int16_t initial[3] = {1, 2, 3};
    input_ring_write(&r, initial, 3);
    if (input_ring_used(&r) != 3) {
        fprintf(stderr, "input_ring drop: expected used=3 after initial write, got %zu\n", input_ring_used(&r));
        return 1;
    }

    /* Write more than available space -> should be dropped, not overwrite */
    r.producer_drops.store(0);
    int16_t extra[2] = {9, 10};
    input_ring_write(&r, extra, 2);

    if (input_ring_used(&r) != 3) {
        fprintf(stderr, "input_ring drop: expected used=3 after drop write, got %zu\n", input_ring_used(&r));
        return 1;
    }
    if (r.producer_drops.load() != 2) {
        fprintf(stderr, "input_ring drop: expected producer_drops=2, got %llu\n",
                (unsigned long long)r.producer_drops.load());
        return 1;
    }

    /* Ensure the original data is still present and in order */
    int16_t out[4] = {0};
    int read = input_ring_read_block(&r, out, 3);
    if (read != 3) {
        fprintf(stderr, "input_ring drop: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 1 || out[1] != 2 || out[2] != 3) {
        fprintf(stderr, "input_ring drop: queue contents corrupted after drop\n");
        return 1;
    }

    pthread_mutex_destroy(&r.ready_m);
    pthread_cond_destroy(&r.ready);
    free(r.buffer);
    return 0;
}

static int
test_output_ring_wrap_and_read(void) {
    const size_t cap = 8;
    struct output_state o;
    memset(&o, 0, sizeof(o));

    o.buffer = (int16_t*)calloc(cap, sizeof(int16_t));
    if (!o.buffer) {
        fprintf(stderr, "output_ring: allocation failed\n");
        return 1;
    }
    o.capacity = cap;
    o.head.store(0);
    o.tail.store(0);
    o.write_timeouts.store(0);
    o.read_timeouts.store(0);
    pthread_cond_init(&o.ready, NULL);
    pthread_mutex_init(&o.ready_m, NULL);
    pthread_cond_init(&o.space, NULL);

    /* First write: no wrap, fills positions [0..5] */
    int16_t src1[6] = {1, 2, 3, 4, 5, 6};
    ring_write_no_signal(&o, src1, 6);

    /* Read three samples to advance tail -> positions [0..2] */
    int16_t out[8] = {0};
    int read = ring_read_batch(&o, out, 3);
    if (read != 3) {
        fprintf(stderr, "output_ring: expected read 3, got %d\n", read);
        return 1;
    }
    if (out[0] != 1 || out[1] != 2 || out[2] != 3) {
        fprintf(stderr, "output_ring: first read mismatch\n");
        return 1;
    }

    /* Second write: triggers wrap-around from head near end of buffer */
    int16_t src2[3] = {7, 8, 9};
    ring_write_no_signal(&o, src2, 3);

    /* Queue should now contain {4,5,6,7,8,9} */
    size_t used = ring_used(&o);
    if (used != 6) {
        fprintf(stderr, "output_ring: expected used=6, got %zu\n", used);
        return 1;
    }

    memset(out, 0, sizeof(out));
    read = ring_read_batch(&o, out, 6);
    if (read != 6) {
        fprintf(stderr, "output_ring: expected read 6, got %d\n", read);
        return 1;
    }
    const int16_t expect[6] = {4, 5, 6, 7, 8, 9};
    if (memcmp(out, expect, sizeof(expect)) != 0) {
        fprintf(stderr, "output_ring: wrap/read sequence mismatch\n");
        return 1;
    }

    pthread_cond_destroy(&o.space);
    pthread_mutex_destroy(&o.ready_m);
    pthread_cond_destroy(&o.ready);
    free(o.buffer);
    return 0;
}

struct OutputWriterArgs {
    struct output_state* ring;
    const int16_t* data;
    size_t count;
    pthread_mutex_t* mu;
    pthread_cond_t* cv;
    int* ready_flag;
};

struct OutputReaderArgs {
    struct output_state* ring;
    size_t total_expected;
    int16_t* out;
    size_t* out_count;
    int* error_flag;
};

static void*
output_writer_thread(void* arg) {
    OutputWriterArgs* ctx = (OutputWriterArgs*)arg;
    /* Signal that writer is about to start the blocking write */
    pthread_mutex_lock(ctx->mu);
    *(ctx->ready_flag) = 1;
    pthread_cond_signal(ctx->cv);
    pthread_mutex_unlock(ctx->mu);

    ring_write(ctx->ring, ctx->data, ctx->count);
    return NULL;
}

static void*
output_reader_thread(void* arg) {
    OutputReaderArgs* ctx = (OutputReaderArgs*)arg;
    size_t have = 0;
    int16_t tmp[16];
    while (have < ctx->total_expected) {
        int n = ring_read_batch(ctx->ring, tmp, 8);
        if (n < 0) {
            *(ctx->error_flag) = 1;
            return NULL;
        }
        memcpy(ctx->out + have, tmp, (size_t)n * sizeof(int16_t));
        have += (size_t)n;
    }
    *(ctx->out_count) = have;
    return NULL;
}

static int
test_output_ring_blocking_producer_consumer(void) {
    const size_t cap = 4;
    struct output_state o;
    memset(&o, 0, sizeof(o));

    o.buffer = (int16_t*)calloc(cap, sizeof(int16_t));
    if (!o.buffer) {
        fprintf(stderr, "output_ring pc: allocation failed\n");
        return 1;
    }
    o.capacity = cap;
    o.head.store(0);
    o.tail.store(0);
    o.write_timeouts.store(0);
    o.read_timeouts.store(0);
    pthread_cond_init(&o.ready, NULL);
    pthread_mutex_init(&o.ready_m, NULL);
    pthread_cond_init(&o.space, NULL);

    /* Prefill ring to capacity-1 so the next producer write observes full state */
    int16_t pre[3] = {100, 101, 102};
    ring_write_no_signal(&o, pre, 3);
    if (ring_used(&o) != 3) {
        fprintf(stderr, "output_ring pc: expected used=3 after prefill, got %zu\n", ring_used(&o));
        return 1;
    }

    int16_t bulk[10];
    for (int i = 0; i < 10; i++) {
        bulk[i] = (int16_t)(200 + i);
    }

    pthread_mutex_t barrier_mu;
    pthread_cond_t barrier_cv;
    int writer_ready = 0;
    pthread_mutex_init(&barrier_mu, NULL);
    pthread_cond_init(&barrier_cv, NULL);

    int16_t all[16] = {0};
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

    pthread_t wthread;
    pthread_t rthread;

    if (pthread_create(&wthread, NULL, output_writer_thread, &wargs) != 0) {
        fprintf(stderr, "output_ring pc: failed to create writer thread\n");
        return 1;
    }

    /* Wait until writer has entered its write path (and likely observed a full ring) */
    pthread_mutex_lock(&barrier_mu);
    while (!writer_ready) {
        pthread_cond_wait(&barrier_cv, &barrier_mu);
    }
    pthread_mutex_unlock(&barrier_mu);

    if (pthread_create(&rthread, NULL, output_reader_thread, &rargs) != 0) {
        fprintf(stderr, "output_ring pc: failed to create reader thread\n");
        return 1;
    }

    pthread_join(wthread, NULL);
    pthread_join(rthread, NULL);

    if (read_error != 0) {
        fprintf(stderr, "output_ring pc: reader saw error\n");
        return 1;
    }
    if (all_count != 13) {
        fprintf(stderr, "output_ring pc: expected 13 samples, got %zu\n", all_count);
        return 1;
    }

    /* First three samples must be the prefilled values (FIFO), remainder the bulk sequence */
    if (all[0] != 100 || all[1] != 101 || all[2] != 102) {
        fprintf(stderr, "output_ring pc: prefilled samples out of order\n");
        return 1;
    }
    for (int i = 0; i < 10; i++) {
        if (all[3 + i] != (int16_t)(200 + i)) {
            fprintf(stderr, "output_ring pc: bulk sample mismatch at index %d (got %d, expected %d)\n", i,
                    (int)all[3 + i], 200 + i);
            return 1;
        }
    }

    if (ring_used(&o) != 0) {
        fprintf(stderr, "output_ring pc: expected ring empty after producer/consumer, got used=%zu\n", ring_used(&o));
        return 1;
    }

    pthread_cond_destroy(&barrier_cv);
    pthread_mutex_destroy(&barrier_mu);
    pthread_cond_destroy(&o.space);
    pthread_mutex_destroy(&o.ready_m);
    pthread_cond_destroy(&o.ready);
    free(o.buffer);
    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_input_ring_wrap_and_read();
    rc |= test_output_ring_wrap_and_read();
    rc |= test_input_ring_drop_on_full();
    rc |= test_output_ring_blocking_producer_consumer();
    if (rc == 0) {
        fprintf(stderr, "runtime ring tests: OK\n");
    }
    return rc;
}
