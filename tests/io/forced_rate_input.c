// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A committed fixture at a forced device rate; forced_rate_input.h describes it. */

#include "forced_rate_input.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/platform/file_compat.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* noise_floor's level: 16 LSB a component, in the floats a cu8 replay converts to. */
#define FORCED_RATE_NOISE_SIGMA (16.0 / 127.5)
#define FORCED_RATE_NOISE_SEED  806U
/* Headroom for the interpolation's overshoot: p25p2_cc's samples come within a few LSB of full scale. */
#define FORCED_RATE_GAIN        0.9
/* The longest half-band filter the live cascade runs: HB_TAPS_MAX in <dsd-neo/dsp/demod_state.h>, a C++ header. */
#define FORCED_RATE_HB_TAPS_MAX 31

static void
set_err(char* err, size_t err_size, const char* what, const char* detail) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    }
}

/* The fixture's cu8 bytes from @p start to @p start + @p count complex samples, as floats, into @p dst. */
static int
read_excerpt(const dsd_iq_replay_config* cfg, size_t start, size_t count, float* dst) {
    FILE* fp = dsd_fopen_existing_regular_file(cfg->data_path, "rb");
    if (!fp) {
        return -1;
    }
    uint8_t* raw = (uint8_t*)malloc(count * 2U);
    int rc = -1;
    if (raw && fseek(fp, (long)(start * 2U), SEEK_SET) == 0 && fread(raw, 1U, count * 2U, fp) == count * 2U) {
        for (size_t i = 0; i < count * 2U; i++) {
            dst[i] = ((float)raw[i] - 127.5f) / 127.5f;
        }
        rc = 0;
    }
    free(raw);
    (void)fclose(fp);
    return rc;
}

/* xorshift64*, then Box-Muller: seeded complex Gaussian noise of FORCED_RATE_NOISE_SIGMA a component. */
static uint64_t
next_u64(uint64_t* s) {
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static double
next_unit(uint64_t* s) {
    /* (0, 1]: 53 random bits, never 0, so the logarithm below is finite. */
    return ((double)(next_u64(s) >> 11) + 1.0) / 9007199254740992.0;
}

static void
fill_noise(float* dst, size_t pairs, uint64_t* s) {
    for (size_t i = 0; i < pairs; i++) {
        const double r = FORCED_RATE_NOISE_SIGMA * sqrt(-2.0 * log(next_unit(s)));
        const double a = 2.0 * M_PI * next_unit(s);
        dst[2U * i] = (float)(r * cos(a));
        dst[2U * i + 1U] = (float)(r * sin(a));
    }
}

/* The chain an Airspy at @p device_rate_hz plans for @p dsp_rate_hz, as the live stream plans it, and its tail bound in
   device samples: the half-band cascade's history at every pass (FORCED_RATE_HB_TAPS_MAX - 1 inputs of each) and the step's tail at
   the half-band output rate. */
static int
plan_live_chain(const dsd_test_forced_rate_spec* spec, dsd_test_forced_rate_input* out, size_t* tail_device) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = DSD_RATE_PLAN_BACKEND_AIRSPY;
    in.target_hz = spec->dsp_rate_hz;
    in.max_passes = 10;
    int p = 0;
    while (p < 10 && ((uint64_t)spec->dsp_rate_hz << (p + 1)) <= spec->device_rate_hz) {
        p++;
    }
    in.request_hz = spec->dsp_rate_hz << p;
    in.programmed_hz = spec->device_rate_hz;
    in.readback_hz = spec->device_rate_hz;
    dsd_rate_plan plan;
    dsd_rate_plan_bounds bounds;
    if (dsd_rate_plan_compute(&in, &plan) != DSD_RATE_PLAN_OK || dsd_rate_plan_step_bounds(&plan, &bounds) != 0) {
        return -1;
    }
    out->passes = plan.passes;
    out->step_l = plan.step.enabled ? plan.step.L : 0;
    out->step_m = plan.step.enabled ? plan.step.M : 0;
    *tail_device = (size_t)(FORCED_RATE_HB_TAPS_MAX - 1) * (((size_t)1U << plan.passes) - 1U)
                   + ((size_t)bounds.tail_inputs << plan.passes);
    return 0;
}

/* Upsample @p n source pairs @p src to the device rate into a new buffer (*@p dst, *@p dst_pairs). */
static int
upsample(dsd_iq_resampler* r, const float* src, size_t n, float** dst, size_t* dst_pairs) {
    const size_t pass = 4096U;
    size_t cap = dsd_iq_resampler_max_out(r, n) + dsd_iq_resampler_max_out(r, pass);
    float* out = (float*)malloc(cap * 2U * sizeof(float));
    if (!out) {
        return -1;
    }
    size_t made = 0U;
    for (size_t at = 0U; at < n; at += pass) {
        const size_t take = (n - at < pass) ? n - at : pass;
        const int got = dsd_iq_resampler_process(r, src + 2U * at, take, out + 2U * made, cap - made);
        if (got < 0) {
            free(out);
            return -1;
        }
        made += (size_t)got;
    }
    *dst = out;
    *dst_pairs = made;
    return 0;
}

/* Quantize @p out's floats to cu8 as a capture holds them, at FORCED_RATE_GAIN, then put the floats back to what a
   replay converts the bytes to. */
static int
quantize(dsd_test_forced_rate_input* out) {
    out->cu8 = (uint8_t*)malloc(out->pairs * 2U);
    if (!out->cu8) {
        return -1;
    }
    for (size_t i = 0; i < out->pairs * 2U; i++) {
        double v = floor((double)out->iq[i] * FORCED_RATE_GAIN * 127.5 + 127.5 + 0.5);
        if (v < 0.0 || v > 255.0) {
            out->clipped++;
            v = v < 0.0 ? 0.0 : 255.0;
        }
        out->cu8[i] = (uint8_t)v;
        out->iq[i] = ((float)out->cu8[i] - 127.5f) / 127.5f;
    }
    return 0;
}

/* The fixture's sidecar into @p cfg and the excerpt's first sample and length, in source samples. */
static int
open_excerpt(const char* fixture_dir, const dsd_test_forced_rate_spec* spec, dsd_iq_replay_config* cfg, size_t* start,
             size_t* count, char* err, size_t err_size) {
    char meta_path[4096];
    DSD_SNPRINTF(meta_path, sizeof(meta_path), "%s/%s.iq.json", fixture_dir, spec->fixture);
    char why[256] = {0};
    if (dsd_iq_replay_read_metadata(meta_path, cfg, why, sizeof(why)) != DSD_IQ_OK) {
        set_err(err, err_size, meta_path, why);
        return -1;
    }
    const size_t total = (size_t)(cfg->data_bytes / 2U);
    *start = (size_t)llround(spec->start_s * (double)cfg->sample_rate_hz);
    *count = 0U;
    if (*start < total) {
        *count = spec->length_s > 0.0 ? (size_t)llround(spec->length_s * (double)cfg->sample_rate_hz) : total - *start;
    }
    if (cfg->format != DSD_IQ_FORMAT_CU8 || *count == 0U || *start + *count > total) {
        set_err(err, err_size, "the excerpt does not fit the fixture (or it is not cu8)", spec->fixture);
        return -1;
    }
    return 0;
}

/* The noise each side, in source samples: the asked padding, raised to the tail bound (the upsampler's, in device
   samples, and the live chain's). */
static size_t
pad_source_pairs(const dsd_test_forced_rate_spec* spec, const dsd_iq_resampler* up, size_t chain_tail,
                 dsd_test_forced_rate_input* out) {
    const size_t up_tail = dsd_iq_resampler_max_out(up, dsd_iq_resampler_tail_inputs(up));
    out->tail_bound_pairs = up_tail + chain_tail;
    size_t pad_device = (size_t)llround(spec->pad_s * (double)spec->device_rate_hz);
    if (pad_device < out->tail_bound_pairs) {
        pad_device = out->tail_bound_pairs;
    }
    const uint64_t src_hz = out->source_rate_hz;
    const size_t pad_src = (size_t)(((uint64_t)pad_device * src_hz + spec->device_rate_hz - 1U) / spec->device_rate_hz);
    out->pad_pairs = (size_t)(((uint64_t)pad_src * spec->device_rate_hz) / src_hz);
    return pad_src;
}

/* Noise, the excerpt, noise, at the source rate, upsampled into @p out. */
static int
make_samples(const dsd_iq_replay_config* cfg, size_t start, size_t count, size_t pad_src, dsd_iq_resampler* up,
             dsd_test_forced_rate_input* out) {
    const size_t src_pairs = count + 2U * pad_src;
    float* src = (float*)malloc(src_pairs * 2U * sizeof(float));
    if (!src) {
        return -1;
    }
    uint64_t seed = FORCED_RATE_NOISE_SEED;
    fill_noise(src, pad_src, &seed);
    int rc = read_excerpt(cfg, start, count, src + 2U * pad_src);
    fill_noise(src + 2U * (pad_src + count), pad_src, &seed);
    if (rc == 0) {
        rc = upsample(up, src, src_pairs, &out->iq, &out->pairs);
    }
    free(src);
    return rc;
}

int
dsd_test_forced_rate_input_make(const char* fixture_dir, const dsd_test_forced_rate_spec* spec,
                                dsd_test_forced_rate_input* out, char* err, size_t err_size) {
    if (!fixture_dir || !spec || !spec->fixture || !out || spec->device_rate_hz == 0U || spec->dsp_rate_hz == 0U) {
        set_err(err, err_size, "bad arguments", NULL);
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    size_t start = 0U;
    size_t count = 0U;
    if (open_excerpt(fixture_dir, spec, &cfg, &start, &count, err, err_size) != 0) {
        dsd_iq_replay_config_clear(&cfg);
        return -1;
    }
    out->device_rate_hz = spec->device_rate_hz;
    out->source_rate_hz = cfg.sample_rate_hz;
    out->center_frequency_hz = cfg.center_frequency_hz;

    size_t chain_tail = 0U;
    dsd_iq_resampler up;
    dsd_iq_resampler_init(&up);
    const int src_hz = (int)cfg.sample_rate_hz;
    int rc = plan_live_chain(spec, out, &chain_tail) == 0
                     && dsd_iq_resampler_configure(&up, src_hz, (int)spec->device_rate_hz, (int)(0.45 * src_hz), 4096U)
                            == DSD_IQ_RESAMPLER_OK
                 ? 0
                 : -1;
    if (rc == 0) {
        rc = make_samples(&cfg, start, count, pad_source_pairs(spec, &up, chain_tail, out), &up, out);
    }
    if (rc == 0) {
        rc = quantize(out);
    }
    dsd_iq_resampler_free(&up);
    dsd_iq_replay_config_clear(&cfg);
    if (rc != 0) {
        dsd_test_forced_rate_input_free(out);
        set_err(err, err_size, "no chain for the device rate, or the fixture could not be read or upsampled",
                spec->fixture);
        return -1;
    }
    return 0;
}

void
dsd_test_forced_rate_input_free(dsd_test_forced_rate_input* in) {
    if (!in) {
        return;
    }
    free(in->cu8);
    free(in->iq);
    in->cu8 = NULL;
    in->iq = NULL;
    in->pairs = 0U;
}
