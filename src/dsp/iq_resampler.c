// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Streaming complex (I/Q) rational resampler for replayed captures (issue #701). See
 * <dsd-neo/dsp/iq_resampler.h>.
 */

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/rate_converter.h>
#include <dsd-neo/runtime/mem.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

/* The attenuation the Kaiser estimate designs for: 5 dB over the 60 dB promised, which the estimate's own error and the
   taps rounded up to a multiple of 4 never use up. It also holds the passband ripple to about +-0.005 dB. */
#define IQ_RESAMPLER_DESIGN_DB 65.0

static const double kIqResamplerPi = 3.14159265358979323846;

/* The zeroth-order modified Bessel function of the first kind, by its power series. */
static double
iq_resampler_bessel_i0(double x) {
    const double half = x / 2.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 64; k++) {
        term *= half / (double)k;
        const double sq = term * term;
        sum += sq;
        if (sq < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

static void
iq_resampler_release(dsd_iq_resampler* r) {
    dsd_neo_aligned_free(r->bank);
    dsd_neo_aligned_free(r->work);
    DSD_MEMSET(r, 0, sizeof(*r));
    r->L = 1;
    r->M = 1;
}

void
dsd_iq_resampler_init(dsd_iq_resampler* r) {
    if (!r) {
        return;
    }
    DSD_MEMSET(r, 0, sizeof(*r));
    r->L = 1;
    r->M = 1;
}

void
dsd_iq_resampler_free(dsd_iq_resampler* r) {
    if (!r) {
        return;
    }
    iq_resampler_release(r);
}

const char*
dsd_iq_resampler_status_text(int status) {
    switch (status) {
        case DSD_IQ_RESAMPLER_OK: return "ok";
        case DSD_IQ_RESAMPLER_ERR_ARGS: return "invalid rates or sizes";
        case DSD_IQ_RESAMPLER_ERR_RATIO: return "no rational ratio within 250 ppm for these rates";
        case DSD_IQ_RESAMPLER_ERR_BAND: return "the protected band leaves no transition band";
        case DSD_IQ_RESAMPLER_ERR_TAPS: return "the transition band needs more than 256 taps per phase";
        case DSD_IQ_RESAMPLER_ERR_ALLOC: return "out of memory for the filter";
        default: return "unknown resampler error";
    }
}

/* Writes the L-phase bank for a Kaiser-windowed sinc of L x T taps, cut off midway between @p pass_hz and @p stop_hz at
   the L-times rate, scaled so each phase averages unity gain. Phase p holds taps p, p + L, p + 2L, ... in reverse, so
   its last tap meets the newest input. */
static void
iq_resampler_design_bank(float* bank, int L, int T, double in_hz, double pass_hz, double stop_hz, double beta) {
    const size_t n = (size_t)L * (size_t)T;
    const double fh = in_hz * (double)L;
    const double fc = (pass_hz + stop_hz) / (2.0 * fh); /* cycles per sample */
    const double center = ((double)n - 1.0) / 2.0;
    const double i0_beta = iq_resampler_bessel_i0(beta);
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i - center;
        const double x = 2.0 * fc * t;
        double sinc = 1.0;
        if (fabs(x) > 1e-12) {
            sinc = sin(kIqResamplerPi * x) / (kIqResamplerPi * x);
        }
        const double u = t / center;
        const double inside = 1.0 - u * u;
        const double w = iq_resampler_bessel_i0(beta * sqrt(inside > 0.0 ? inside : 0.0)) / i0_beta;
        const double h = 2.0 * fc * sinc * w;
        sum += h;
        const size_t phase = i % (size_t)L;
        const size_t k = i / (size_t)L;
        bank[phase * (size_t)T + ((size_t)T - 1U - k)] = (float)h;
    }
    const double scale = sum > 0.0 ? (double)L / sum : 1.0;
    for (size_t i = 0; i < n; i++) {
        bank[i] = (float)((double)bank[i] * scale);
    }
}

int
dsd_iq_resampler_configure(dsd_iq_resampler* r, int in_hz, int out_hz, int protect_hz, size_t max_in_complex) {
    if (!r) {
        return DSD_IQ_RESAMPLER_ERR_ARGS;
    }
    iq_resampler_release(r);
    if (in_hz <= 0 || out_hz <= 0 || protect_hz <= 0 || max_in_complex == 0U
        || max_in_complex > (size_t)INT32_MAX / 4U) {
        return DSD_IQ_RESAMPLER_ERR_ARGS;
    }
    int L = 1;
    int M = 1;
    if (dsd_rate_converter_ratio(in_hz, out_hz, &L, &M) == DSD_RATE_CONVERTER_UNSUPPORTED) {
        return DSD_IQ_RESAMPLER_ERR_RATIO;
    }
    const double ppm = ((double)L * (double)in_hz / ((double)M * (double)out_hz) - 1.0) * 1e6;
    if (fabs(ppm) > DSD_IQ_RESAMPLER_MAX_PPM) {
        return DSD_IQ_RESAMPLER_ERR_RATIO;
    }

    /* The edges: see the header. */
    const double in = (double)in_hz;
    const double protect = (double)protect_hz;
    const double pass = fmin(protect, DSD_IQ_RESAMPLER_MAX_PASS_FRACTION * in);
    const double stop = fmin(in - pass, (double)out_hz - protect);
    if (!(stop > pass)) {
        return DSD_IQ_RESAMPLER_ERR_BAND;
    }

    /* Kaiser: order (A - 7.95) / (2.285 x transition in radians per sample), at the L-times rate. */
    const double fh = in * (double)L;
    const double order = (IQ_RESAMPLER_DESIGN_DB - 7.95) / (2.285 * 2.0 * kIqResamplerPi * (stop - pass) / fh);
    const double taps_per_phase = ceil((ceil(order) + 1.0) / (double)L);
    if (!(taps_per_phase <= (double)DSD_IQ_RESAMPLER_MAX_TAPS_PER_PHASE)) {
        return DSD_IQ_RESAMPLER_ERR_TAPS;
    }
    int T = (int)taps_per_phase;
    T = (T + 3) & ~3;
    if (T < 4) {
        T = 4;
    }
    if (T > DSD_IQ_RESAMPLER_MAX_TAPS_PER_PHASE || (int64_t)L * (int64_t)T > (int64_t)DSD_IQ_RESAMPLER_MAX_BANK_TAPS) {
        return DSD_IQ_RESAMPLER_ERR_TAPS;
    }
    const double beta = 0.1102 * (IQ_RESAMPLER_DESIGN_DB - 8.7);

    const size_t bank_len = (size_t)L * (size_t)T;
    const size_t work_len = 2U * ((size_t)T - 1U + max_in_complex);
    float* bank = (float*)dsd_neo_aligned_malloc(bank_len * sizeof(float));
    float* work = (float*)dsd_neo_aligned_malloc(work_len * sizeof(float));
    if (!bank || !work) {
        dsd_neo_aligned_free(bank);
        dsd_neo_aligned_free(work);
        return DSD_IQ_RESAMPLER_ERR_ALLOC;
    }
    iq_resampler_design_bank(bank, L, T, in, pass, stop, beta);
    DSD_MEMSET(work, 0, work_len * sizeof(float));

    r->configured = 1;
    r->in_hz = in_hz;
    r->out_hz = out_hz;
    r->L = L;
    r->M = M;
    r->taps_per_phase = T;
    r->passband_hz = pass;
    r->stopband_hz = stop;
    r->max_in = max_in_complex;
    r->phase = 0;
    r->bank = bank;
    r->work = work;
    return DSD_IQ_RESAMPLER_OK;
}

size_t
dsd_iq_resampler_max_out(const dsd_iq_resampler* r, size_t in_complex) {
    if (!r || !r->configured) {
        return 0U;
    }
    const uint64_t num = (uint64_t)in_complex * (uint64_t)r->L + (uint64_t)r->M - 1U;
    const uint64_t n = num / (uint64_t)r->M;
    return n > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)n;
}

void
dsd_iq_resampler_clear(dsd_iq_resampler* r) {
    if (!r || !r->configured) {
        return;
    }
    DSD_MEMSET(r->work, 0, ((size_t)r->taps_per_phase - 1U) * 2U * sizeof(float));
    r->phase = 0;
}

/* Filters @p n inputs (at most max_in) into @p out; returns the outputs written. The caller has checked the room. */
static size_t
iq_resampler_pass(dsd_iq_resampler* r, const float* in, size_t n, float* out) {
    const int L = r->L;
    const int M = r->M;
    const size_t T = (size_t)r->taps_per_phase;
    const size_t hist = T - 1U;
    float* work = r->work;
    DSD_MEMCPY(work + 2U * hist, in, n * 2U * sizeof(float));
    size_t produced = 0;
    int ph = r->phase;
    for (size_t i = 0; i < n; i++) {
        /* The window of T inputs ending at input i; tap t of a phase meets work[i + t]. */
        const float* x = work + 2U * i;
        while (ph < L) {
            const float* c = r->bank + (size_t)ph * T;
            float i0 = 0.0f;
            float i1 = 0.0f;
            float i2 = 0.0f;
            float i3 = 0.0f;
            float q0 = 0.0f;
            float q1 = 0.0f;
            float q2 = 0.0f;
            float q3 = 0.0f;
            for (size_t t = 0; t < T; t += 4U) {
                const float* xs = x + 2U * t;
                i0 += c[t] * xs[0];
                q0 += c[t] * xs[1];
                i1 += c[t + 1U] * xs[2];
                q1 += c[t + 1U] * xs[3];
                i2 += c[t + 2U] * xs[4];
                q2 += c[t + 2U] * xs[5];
                i3 += c[t + 3U] * xs[6];
                q3 += c[t + 3U] * xs[7];
            }
            out[2U * produced] = (i0 + i1) + (i2 + i3);
            out[2U * produced + 1U] = (q0 + q1) + (q2 + q3);
            produced++;
            ph += M;
        }
        ph -= L;
    }
    r->phase = ph;
    /* Keep the newest T - 1 inputs as the next pass's history. */
    DSD_MEMMOVE(work, work + 2U * n, hist * 2U * sizeof(float));
    return produced;
}

int
dsd_iq_resampler_process(dsd_iq_resampler* r, const float* in, size_t in_complex, float* out, size_t out_cap_complex) {
    if (!r || !r->configured || (!in && in_complex > 0U) || !out) {
        return -1;
    }
    const size_t bound = dsd_iq_resampler_max_out(r, in_complex);
    if (out_cap_complex < bound || bound > (size_t)INT32_MAX) {
        return -1;
    }
    size_t produced = 0;
    size_t pos = 0;
    while (pos < in_complex) {
        size_t n = in_complex - pos;
        if (n > r->max_in) {
            n = r->max_in;
        }
        produced += iq_resampler_pass(r, in + 2U * pos, n, out + 2U * produced);
        pos += n;
    }
    return (int)produced;
}

double
dsd_iq_resampler_ratio_error_ppm(const dsd_iq_resampler* r) {
    if (!r || !r->configured) {
        return 0.0;
    }
    return ((double)r->L * (double)r->in_hz / ((double)r->M * (double)r->out_hz) - 1.0) * 1e6;
}

double
dsd_iq_resampler_group_delay_seconds(const dsd_iq_resampler* r) {
    if (!r || !r->configured) {
        return 0.0;
    }
    return ((double)r->L * (double)r->taps_per_phase - 1.0) / (2.0 * (double)r->L * (double)r->in_hz);
}
