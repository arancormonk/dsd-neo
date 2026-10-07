// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog monitor's sinks at the sink rate (issue #633). See <dsd-neo/dsp/analog_sink.h>.
 *
 * Kept apart from the monitor's audio chain (analog_audio.c), which tests replace with their own copy: nothing here
 * calls into it.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <dsd-neo/dsp/rate_converter.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef DSD_NEO_TEST_HOOKS
#include "symbol_test_support.h"
#endif

#define ANALOG_SINK_COUNT 2

/* The reception a sink last wrote in: the RTL stream generation (RTL input), the trunk-tuning generation and the PCM
   input stream generation. */
typedef struct {
    uint32_t rtl_generation;
    uint64_t tune_generation;
    uint32_t pcm_generation;
} analog_sink_reception;

typedef struct {
    dsd_rate_converter conv;
    /* This block wrote; the previous block did not (the next write starts from silence). */
    int written;
    int gap;
    int has_reception;
    analog_sink_reception reception;
} analog_sink_lane;

typedef struct {
    analog_sink_lane lane[ANALOG_SINK_COUNT];
    /* The rate the last "no conversion" error named, so it is logged once per rate. */
    int warned_hz;
    float out_f[DSD_ANALOG_SINK_CHUNK];
    short out_s[DSD_ANALOG_SINK_CHUNK];
} analog_sink_ext;

static void
analog_sink_ext_free(void* ptr) {
    analog_sink_ext* ext = (analog_sink_ext*)ptr;
    if (!ext) {
        return;
    }
    for (int i = 0; i < ANALOG_SINK_COUNT; i++) {
        dsd_rate_converter_free(&ext->lane[i].conv);
    }
    free(ext);
}

static analog_sink_ext*
analog_sink_ext_find(const dsd_state* state) {
    return state ? DSD_STATE_EXT_GET_AS(analog_sink_ext, state, DSD_STATE_EXT_DSP_ANALOG_SINK) : NULL;
}

#ifdef DSD_NEO_TEST_HOOKS
static int g_analog_sink_test_fail_alloc = 0;

void
dsd_analog_sink_test_fail_alloc(int fail) {
    g_analog_sink_test_fail_alloc = fail;
}
#endif

static analog_sink_ext*
analog_sink_ext_get(dsd_state* state) {
    analog_sink_ext* ext = analog_sink_ext_find(state);
    if (ext) {
        return ext;
    }
#ifdef DSD_NEO_TEST_HOOKS
    if (g_analog_sink_test_fail_alloc) {
        return NULL;
    }
#endif
    ext = (analog_sink_ext*)calloc(1, sizeof(*ext));
    if (!ext) {
        return NULL;
    }
    for (int i = 0; i < ANALOG_SINK_COUNT; i++) {
        dsd_rate_converter_init(&ext->lane[i].conv);
        /* Nothing was written before: the first write starts from silence, as a fresh converter does anyway. */
        ext->lane[i].written = 1;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_DSP_ANALOG_SINK, ext, analog_sink_ext_free) != 0) {
        analog_sink_ext_free(ext);
        return NULL;
    }
    return ext;
}

static analog_sink_reception
analog_sink_reception_now(const dsd_opts* opts) {
    analog_sink_reception r;
    r.rtl_generation = opts->audio_in_type == AUDIO_IN_RTL ? dsd_rtl_stream_metrics_hook_stream_generation() : 0U;
    r.tune_generation = dsd_trunk_tuning_generation();
    r.pcm_generation = opts->pcm_input_generation;
    return r;
}

static int
analog_sink_reception_equal(const analog_sink_reception* a, const analog_sink_reception* b) {
    return a->rtl_generation == b->rtl_generation && a->tune_generation == b->tune_generation
           && a->pcm_generation == b->pcm_generation;
}

void
dsd_analog_sink_block_begin(const dsd_state* state) {
    analog_sink_ext* ext = analog_sink_ext_find(state);
    if (!ext) {
        return;
    }
    for (int i = 0; i < ANALOG_SINK_COUNT; i++) {
        if (!ext->lane[i].written) {
            ext->lane[i].gap = 1;
        }
        ext->lane[i].written = 0;
    }
}

void
dsd_analog_sink_break(const dsd_state* state) {
    analog_sink_ext* ext = analog_sink_ext_find(state);
    if (!ext) {
        return;
    }
    for (int i = 0; i < ANALOG_SINK_COUNT; i++) {
        ext->lane[i].gap = 1;
    }
}

void
dsd_analog_sink_note_reception(const dsd_state* state) {
    analog_sink_ext* ext = analog_sink_ext_find(state);
    if (ext) {
        ext->lane[DSD_ANALOG_SINK_MONITOR].gap = 1;
    }
}

static short
analog_sink_to_i16(float v) {
    if (isnan(v)) {
        return 0;
    }
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (short)lrintf(v);
}

/* Says once per rate that a block cannot be converted, and why. */
static void
analog_sink_warn(analog_sink_ext* ext, int block_hz, int sink_hz, int mode) {
    if (ext->warned_hz == block_hz) {
        return;
    }
    LOG_ERROR("Analog output: %d Hz cannot be converted to %d Hz (%s); the analog outputs stay silent at this rate.\n",
              block_hz, sink_hz, mode == DSD_RATE_CONVERTER_FAILED ? "out of memory" : "out of range");
    ext->warned_hz = block_hz;
}

/* Sets @p lane's converter for @p block_hz to @p sink_hz and starts it over from silence where its stream broke.
   Returns DSD_ANALOG_SINK_WROTE when the block is to be converted, else what the caller does with it. */
static int
analog_sink_lane_prepare(const dsd_opts* opts, analog_sink_ext* ext, analog_sink_lane* lane, int block_hz,
                         int sink_hz) {
    const int mode = dsd_rate_converter_configure(&lane->conv, block_hz, sink_hz);
    if (mode == DSD_RATE_CONVERTER_IDENTITY) {
        /* Within the converter's tolerance of the sink rate: the caller writes the block as it is. */
        return DSD_ANALOG_SINK_NATIVE;
    }
    if (mode != DSD_RATE_CONVERTER_CONVERTING) {
        analog_sink_warn(ext, block_hz, sink_hz, mode);
        return DSD_ANALOG_SINK_MUTED;
    }
    const analog_sink_reception now = analog_sink_reception_now(opts);
    if (lane->gap || !lane->has_reception || !analog_sink_reception_equal(&lane->reception, &now)) {
        dsd_rate_converter_clear(&lane->conv);
    }
    lane->gap = 0;
    lane->reception = now;
    lane->has_reception = 1;
    return DSD_ANALOG_SINK_WROTE;
}

/* Converts @p buf and hands the samples to @p write, at most DSD_ANALOG_SINK_CHUNK at a time. */
static void
analog_sink_emit(analog_sink_ext* ext, analog_sink_lane* lane, const float* buf, size_t n,
                 dsd_analog_sink_write_fn write, const void* ctx) {
    size_t pos = 0;
    while (pos < n) {
        size_t consumed = 0;
        const int got =
            dsd_rate_converter_process(&lane->conv, buf + pos, n - pos, ext->out_f, DSD_ANALOG_SINK_CHUNK, &consumed);
        if (got < 0 || consumed == 0) {
            return; /* Not reachable: a chunk always has room for one input's outputs. */
        }
        pos += consumed;
        for (int i = 0; i < got; i++) {
            ext->out_s[i] = analog_sink_to_i16(ext->out_f[i]);
        }
        if (got > 0) {
            write(ctx, ext->out_s, (size_t)got);
        }
    }
}

int
dsd_analog_sink_write(const dsd_opts* opts, dsd_state* state, dsd_analog_sink sink, const float* buf, size_t n,
                      int block_hz, int sink_hz, dsd_analog_sink_write_fn write, const void* ctx) {
    if (block_hz == sink_hz) {
        return DSD_ANALOG_SINK_NATIVE;
    }
    if (!opts || !state || !buf || !write || (int)sink < 0 || (int)sink >= ANALOG_SINK_COUNT) {
        return DSD_ANALOG_SINK_MUTED;
    }
    analog_sink_ext* ext = analog_sink_ext_get(state);
    if (!ext) {
        /* Said once per rate, as for an unconvertible one; the state keeps the rate, as there is no extension. */
        if (state->analog_sink_nomem_hz != block_hz) {
            LOG_ERROR("Analog output: no memory to convert %d Hz to %d Hz; the analog outputs stay silent.\n", block_hz,
                      sink_hz);
            state->analog_sink_nomem_hz = block_hz;
        }
        return DSD_ANALOG_SINK_MUTED;
    }
    analog_sink_lane* lane = &ext->lane[sink];
    const int prepared = analog_sink_lane_prepare(opts, ext, lane, block_hz, sink_hz);
    if (prepared != DSD_ANALOG_SINK_WROTE) {
        return prepared;
    }
    analog_sink_emit(ext, lane, buf, n, write, ctx);
    lane->written = 1;
    return DSD_ANALOG_SINK_WROTE;
}
