// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The NFM call's pre-roll ring (issue #668). See analog_preroll.h. */

#include "analog_preroll.h"

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef DSD_NEO_TEST_HOOKS
static int g_analog_preroll_test_fail_alloc = 0;

void
analog_preroll_test_fail_alloc(int fail) {
    g_analog_preroll_test_fail_alloc = fail;
}
#endif

static short*
analog_preroll_alloc(size_t cap) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_analog_preroll_test_fail_alloc) {
        return NULL;
    }
#endif
    return (short*)calloc(cap, sizeof(short));
}

void
analog_preroll_resize(analog_preroll* p, int ms) {
    free(p->buf);
    p->buf = NULL;
    p->cap = 0;
    p->ms = ms > 0 ? ms : 0;
    analog_preroll_clear(p);
    if (p->ms == 0) {
        return;
    }
    const size_t cap = (size_t)p->ms * 48U;
    p->buf = analog_preroll_alloc(cap);
    if (!p->buf) {
        LOG_WARN("NFM call: no memory for a %d ms pre-roll; recordings start at the reception.\n", p->ms);
        return;
    }
    p->cap = cap;
}

int
analog_preroll_active(const analog_preroll* p) {
    return p->buf != NULL;
}

void
analog_preroll_clear(analog_preroll* p) {
    p->head = 0;
    p->len = 0;
    p->has_generation = 0;
}

int
analog_preroll_lane_moved(dsd_state* state, const analog_preroll* p) {
    uint32_t now = 0;
    if (!dsd_analog_sink_generation(state, DSD_ANALOG_SINK_CALL_WAV, &now)) {
        return 1;
    }
    return p->has_generation && now != p->generation;
}

/* What the CALL_WAV lane hands the ring: the ring, and the samples the lane made for it. */
typedef struct {
    analog_preroll* ring;
    uint64_t* made;
} analog_preroll_writer;

static void
analog_preroll_push(const void* context, const short* samples, size_t count) {
    const analog_preroll_writer* w = (const analog_preroll_writer*)context;
    analog_preroll* p = w->ring;
    *w->made += count;
    if (count >= p->cap) {
        DSD_MEMCPY(p->buf, samples + (count - p->cap), p->cap * sizeof(short));
        p->head = 0;
        p->len = p->cap;
        return;
    }
    const size_t first = count < p->cap - p->head ? count : p->cap - p->head;
    DSD_MEMCPY(p->buf + p->head, samples, first * sizeof(short));
    DSD_MEMCPY(p->buf, samples + first, (count - first) * sizeof(short));
    p->head = (p->head + count) % p->cap;
    p->len = p->len + count < p->cap ? p->len + count : p->cap;
}

uint64_t
analog_preroll_append(const dsd_opts* opts, dsd_state* state, analog_preroll* p, const float* samples, size_t count,
                      int sample_rate, int* moved) {
    *moved = 0;
    uint32_t before = 0;
    if (!dsd_analog_sink_generation(state, DSD_ANALOG_SINK_CALL_WAV, &before)) {
        analog_preroll_clear(p);
        return 0;
    }
    uint64_t made = 0;
    const analog_preroll_writer writer = {p, &made};
    (void)dsd_analog_sink_write(opts, state, DSD_ANALOG_SINK_CALL_WAV, samples, count, 1.0, sample_rate, 48000,
                                analog_preroll_push, &writer);
    uint32_t after = before;
    (void)dsd_analog_sink_generation(state, DSD_ANALOG_SINK_CALL_WAV, &after);
    if (after != before) {
        /* Only this block runs on into the lane's next write; the ring's older samples are one conversion behind. */
        *moved = 1;
        p->len = made < (uint64_t)p->cap ? (size_t)made : p->cap;
    }
    p->generation = after;
    p->has_generation = 1;
    return made;
}

void
analog_preroll_take(const analog_preroll* p, size_t count, dsd_analog_sink_write_fn write, const void* ctx) {
    if (count > p->len) {
        count = p->len;
    }
    if (count == 0) {
        return;
    }
    const size_t start = (p->head + p->cap - count) % p->cap;
    const size_t first = count < p->cap - start ? count : p->cap - start;
    write(ctx, p->buf + start, first);
    if (count > first) {
        write(ctx, p->buf, count - first);
    }
}

void
analog_preroll_consume(dsd_state* state, analog_preroll* p) {
    analog_preroll_clear(p);
    uint32_t now = 0;
    if (dsd_analog_sink_generation(state, DSD_ANALOG_SINK_CALL_WAV, &now)) {
        p->generation = now;
        p->has_generation = 1;
    }
}
