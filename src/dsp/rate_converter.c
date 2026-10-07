// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Streaming sample-rate converter to a sink rate (issue #633). See <dsd-neo/dsp/rate_converter.h>.
 */

#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/rate_converter.h>
#include <dsd-neo/dsp/resampler.h>

#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

static int64_t
rate_converter_abs64(int64_t v) {
    return v < 0 ? -v : v;
}

/* |L/M - p/q| scaled by q: |L * q - p * M|. */
static int64_t
rate_converter_error(int64_t p, int64_t q, int64_t L, int64_t M) {
    return rate_converter_abs64(L * q - p * M);
}

/* Whether L/M is at least as close to p/q as L2/M2. */
static int
rate_converter_closer(int64_t p, int64_t q, int64_t L, int64_t M, int64_t L2, int64_t M2) {
    return rate_converter_error(p, q, L, M) * M2 <= rate_converter_error(p, q, L2, M2) * M;
}

/* The best L/M for p/q with both terms up to DSD_RATE_CONVERTER_MAX_TERM: the continued fraction's last convergent
   within the bound, or the semiconvergent after it when that is closer. Returns 0 when no term fits. */
static int
rate_converter_best_ratio(int64_t p, int64_t q, int* L, int* M) {
    const int64_t n = DSD_RATE_CONVERTER_MAX_TERM;
    int64_t h_prev2 = 0;
    int64_t h_prev = 1;
    int64_t k_prev2 = 1;
    int64_t k_prev = 0;
    int64_t num = p;
    int64_t den = q;
    while (den != 0) {
        const int64_t a = num / den;
        const int64_t h = a * h_prev + h_prev2;
        const int64_t k = a * k_prev + k_prev2;
        if (h > n || k > n) {
            /* The largest t < a that keeps both terms in bound. */
            int64_t t = a - 1;
            if (h_prev > 0 && (n - h_prev2) / h_prev < t) {
                t = (n - h_prev2) / h_prev;
            }
            if (k_prev > 0 && (n - k_prev2) / k_prev < t) {
                t = (n - k_prev2) / k_prev;
            }
            int64_t best_h = h_prev;
            int64_t best_k = k_prev;
            if (t >= 1) {
                const int64_t sh = t * h_prev + h_prev2;
                const int64_t sk = t * k_prev + k_prev2;
                if (best_k < 1 || rate_converter_closer(p, q, sh, sk, best_h, best_k)) {
                    best_h = sh;
                    best_k = sk;
                }
            }
            if (best_h < 1 || best_k < 1) {
                return 0;
            }
            *L = (int)best_h;
            *M = (int)best_k;
            return 1;
        }
        h_prev2 = h_prev;
        h_prev = h;
        k_prev2 = k_prev;
        k_prev = k;
        const int64_t r = num - a * den;
        num = den;
        den = r;
    }
    if (h_prev < 1 || k_prev < 1) {
        return 0;
    }
    *L = (int)h_prev;
    *M = (int)k_prev;
    return 1;
}

/* Whether a converter from @p from_hz to @p to_hz is supported at all: an input from DSD_RATE_CONVERTER_MIN_IN_HZ up to
   DSD_RATE_CONVERTER_MAX_DECIMATION times the output. */
static int
rate_converter_in_range(int from_hz, int to_hz) {
    return from_hz >= DSD_RATE_CONVERTER_MIN_IN_HZ && to_hz > 0
           && (int64_t)from_hz <= (int64_t)to_hz * DSD_RATE_CONVERTER_MAX_DECIMATION;
}

/* The L/M for @p from_hz to @p to_hz: exact where both reduced terms fit DSD_RATE_CONVERTER_MAX_TERM, else the closest
   ratio whose terms do, provided it is within DSD_RATE_CONVERTER_TOLERANCE_PPM. Returns 0 when none is. */
static int
rate_converter_pick(int from_hz, int to_hz, int* L, int* M) {
    const int g = gcd_int(from_hz, to_hz);
    const int64_t p = (int64_t)(to_hz / g);
    const int64_t q = (int64_t)(from_hz / g);
    int bl = (int)p;
    int bm = (int)q;
    if ((p > DSD_RATE_CONVERTER_MAX_TERM || q > DSD_RATE_CONVERTER_MAX_TERM)
        && !rate_converter_best_ratio(p, q, &bl, &bm)) {
        return 0;
    }
    /* Within tolerance: |L*q - p*M| / (p*M) <= 250 ppm. */
    if (bl < 1 || bm < 1
        || rate_converter_error(p, q, bl, bm) * 1000000 > (int64_t)DSD_RATE_CONVERTER_TOLERANCE_PPM * p * bm) {
        return 0;
    }
    *L = bl;
    *M = bm;
    return 1;
}

int
dsd_rate_converter_ratio(int from_hz, int to_hz, int* L, int* M) {
    int l = 1;
    int m = 1;
    int mode = DSD_RATE_CONVERTER_UNSUPPORTED;
    if (from_hz > 0 && to_hz > 0 && from_hz == to_hz) {
        mode = DSD_RATE_CONVERTER_IDENTITY;
    } else if (rate_converter_in_range(from_hz, to_hz) && rate_converter_pick(from_hz, to_hz, &l, &m)) {
        mode = (l == 1 && m == 1) ? DSD_RATE_CONVERTER_IDENTITY : DSD_RATE_CONVERTER_CONVERTING;
    }
    if (mode != DSD_RATE_CONVERTER_CONVERTING) {
        l = 1;
        m = 1;
    }
    if (L) {
        *L = l;
    }
    if (M) {
        *M = m;
    }
    return mode;
}

/* Taps per phase: the filter spans K input samples, so keep its span the same in output samples. 16 when upsampling,
   16 x the decimation (rounded up to a multiple of 4) when decimating, up to the resampler's limit. */
static int
rate_converter_taps_per_phase(int L, int M) {
    int64_t k = (16 * (int64_t)M + (int64_t)L - 1) / (int64_t)L;
    k = (k + 3) & ~(int64_t)3;
    if (k < DSD_RESAMPLER_DEFAULT_TAPS_PER_PHASE) {
        k = DSD_RESAMPLER_DEFAULT_TAPS_PER_PHASE;
    }
    if (k > DSD_RESAMPLER_MAX_TAPS_PER_PHASE) {
        k = DSD_RESAMPLER_MAX_TAPS_PER_PHASE;
    }
    return (int)k;
}

void
dsd_rate_converter_init(dsd_rate_converter* c) {
    if (!c) {
        return;
    }
    DSD_MEMSET(c, 0, sizeof(*c));
    dsd_resampler_reset(&c->rs);
    c->mode = DSD_RATE_CONVERTER_UNSUPPORTED;
    c->L = 1;
    c->M = 1;
}

void
dsd_rate_converter_free(dsd_rate_converter* c) {
    if (!c) {
        return;
    }
    dsd_resampler_reset(&c->rs);
    dsd_rate_converter_init(c);
}

int
dsd_rate_converter_configure(dsd_rate_converter* c, int from_hz, int to_hz) {
    if (!c) {
        return DSD_RATE_CONVERTER_UNSUPPORTED;
    }
    if (c->configured && c->in_hz == from_hz && c->out_hz == to_hz && c->mode != DSD_RATE_CONVERTER_FAILED) {
        return c->mode;
    }
    int L = 1;
    int M = 1;
    int mode = dsd_rate_converter_ratio(from_hz, to_hz, &L, &M);
    dsd_resampler_reset(&c->rs);
    if (mode == DSD_RATE_CONVERTER_CONVERTING
        && !dsd_resampler_design_taps(&c->rs, L, M, rate_converter_taps_per_phase(L, M))) {
        dsd_resampler_reset(&c->rs);
        mode = DSD_RATE_CONVERTER_FAILED;
    }
    c->configured = 1;
    c->in_hz = from_hz;
    c->out_hz = to_hz;
    c->mode = mode;
    c->L = mode == DSD_RATE_CONVERTER_CONVERTING ? L : 1;
    c->M = mode == DSD_RATE_CONVERTER_CONVERTING ? M : 1;
    c->carry_len = 0;
    c->carry_pos = 0;
    c->carry_tag = 0U;
    return mode;
}

int
dsd_rate_converter_output_hz(const dsd_rate_converter* c) {
    if (!c || !c->configured) {
        return 0;
    }
    return c->mode == DSD_RATE_CONVERTER_CONVERTING ? c->out_hz : c->in_hz;
}

void
dsd_rate_converter_clear(dsd_rate_converter* c) {
    if (!c) {
        return;
    }
    dsd_resampler_clear_history(&c->rs);
    c->carry_len = 0;
    c->carry_pos = 0;
    c->carry_tag = 0U;
}

int
dsd_rate_converter_max_out_per_input(const dsd_rate_converter* c) {
    if (!c || c->mode != DSD_RATE_CONVERTER_CONVERTING) {
        return 1;
    }
    return (c->L + c->M - 1) / c->M;
}

/* An identity's process: as many samples as fit, copied. */
static int
rate_converter_copy(const float* in, size_t in_len, float* out, size_t out_cap, size_t* consumed) {
    size_t n = in_len < out_cap ? in_len : out_cap;
    if (n > (size_t)INT32_MAX) {
        n = (size_t)INT32_MAX;
    }
    if (n > 0 && out != in) {
        DSD_MEMCPY(out, in, n * sizeof(float));
    }
    *consumed = n;
    return (int)n;
}

/* The most of @p in_len inputs whose outputs fit @p cap: floor((cap * M + phase) / L), at least one when the cap holds
   one input's outputs. */
static int
rate_converter_inputs_that_fit(const dsd_rate_converter* c, size_t in_len, size_t cap) {
    int64_t n = ((int64_t)cap * (int64_t)c->M + (int64_t)c->rs.phase) / (int64_t)c->L;
    if ((uint64_t)n > (uint64_t)in_len) {
        n = (int64_t)in_len;
    }
    return n > (int64_t)INT32_MAX ? INT32_MAX : (int)n;
}

int
dsd_rate_converter_process(dsd_rate_converter* c, const float* in, size_t in_len, float* out, size_t out_cap,
                           size_t* consumed) {
    if (!c || !consumed || (!in && in_len > 0) || !out) {
        return -1;
    }
    *consumed = 0;
    if (in_len == 0) {
        return 0;
    }
    if (c->mode == DSD_RATE_CONVERTER_IDENTITY) {
        return rate_converter_copy(in, in_len, out, out_cap, consumed);
    }
    if (c->mode != DSD_RATE_CONVERTER_CONVERTING || out_cap < (size_t)dsd_rate_converter_max_out_per_input(c)) {
        return -1;
    }
    const size_t cap = out_cap > (size_t)INT32_MAX ? (size_t)INT32_MAX : out_cap;
    const int n = rate_converter_inputs_that_fit(c, in_len, cap);
    const int got = dsd_resampler_process_block(&c->rs, in, n, out, (int)cap);
    if (got < 0) {
        return -1;
    }
    *consumed = (size_t)n;
    return got;
}

/* Up to @p room of the outputs the pull API kept, with their input's tag; returns how many. */
static size_t
rate_converter_take_carry(dsd_rate_converter* c, float* out, uint8_t* tags, size_t room) {
    size_t take = (size_t)(c->carry_len - c->carry_pos);
    if (take > room) {
        take = room;
    }
    DSD_MEMCPY(out, c->carry + c->carry_pos, take * sizeof(float));
    if (tags) {
        DSD_MEMSET(tags, c->carry_tag, take);
    }
    c->carry_pos += (int)take;
    return take;
}

int
dsd_rate_converter_fill(dsd_rate_converter* c, float* out, uint8_t* tags, size_t count, dsd_rate_converter_read_fn read,
                        void* ctx) {
    if (!c || !read || (!out && count > 0) || count > (size_t)INT32_MAX
        || (c->mode != DSD_RATE_CONVERTER_IDENTITY && c->mode != DSD_RATE_CONVERTER_CONVERTING)) {
        return -1;
    }
    size_t done = 0;
    while (done < count) {
        if (c->carry_pos < c->carry_len) {
            done += rate_converter_take_carry(c, out + done, tags ? tags + done : NULL, count - done);
            continue;
        }
        float sample = 0.0f;
        uint8_t tag = 0U;
        if (!read(ctx, &sample, &tag)) {
            return -1;
        }
        if (c->mode == DSD_RATE_CONVERTER_IDENTITY) {
            if (tags) {
                tags[done] = tag;
            }
            out[done++] = sample;
            continue;
        }
        const int got = dsd_resampler_process_block(&c->rs, &sample, 1, c->carry, DSD_RATE_CONVERTER_CARRY);
        if (got < 0) {
            return -1;
        }
        c->carry_len = got;
        c->carry_pos = 0;
        c->carry_tag = tag;
    }
    return (int)count;
}
