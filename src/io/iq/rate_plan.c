// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The live rate plan (issue #806). See <dsd-neo/io/rate_plan.h>.
 */

#include <dsd-neo/io/rate_plan.h>

#include <math.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"

/* The I/Q resampler's design attenuation and Kaiser estimate (iq_resampler.c, IQ_RESAMPLER_DESIGN_DB and
   iq_resampler_pick_taps()). io_iq cannot link the DSP module, so the estimate is repeated here, operation for
   operation so the doubles round alike; DSP_RATE_PLAN_STEP pins it to the real designs. */
#define RATE_PLAN_DESIGN_DB 65.0

static const double kRatePlanPi = 3.14159265358979323846;

static int64_t
rate_plan_abs64(int64_t v) {
    return v < 0 ? -v : v;
}

static int64_t
rate_plan_gcd64(int64_t a, int64_t b) {
    while (b != 0) {
        const int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* |L x q - p x M|: how far L/M is from p/q, scaled by q x M. */
static int64_t
rate_plan_ratio_error(int64_t p, int64_t q, int64_t L, int64_t M) {
    return rate_plan_abs64(L * q - p * M);
}

/* Whether L/M is at least as close to p/q as L2/M2. */
static int
rate_plan_closer(int64_t p, int64_t q, int64_t L, int64_t M, int64_t L2, int64_t M2) {
    return rate_plan_ratio_error(p, q, L, M) * M2 <= rate_plan_ratio_error(p, q, L2, M2) * M;
}

/* The closest L/M to p/q (lowest terms, 0 < p <= q) with both terms up to DSD_RATE_PLAN_MAX_RATIO_TERM: the continued
   fraction's last convergent within the bound, or the semiconvergent after it when that is closer. The same search as
   rate_converter.c's, which io_iq cannot link. */
static void
rate_plan_best_ratio(int64_t p, int64_t q, int* L, int* M) {
    const int64_t n = DSD_RATE_PLAN_MAX_RATIO_TERM;
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
                if (best_k < 1 || rate_plan_closer(p, q, sh, sk, best_h, best_k)) {
                    best_h = sh;
                    best_k = sk;
                }
            }
            *L = (int)best_h;
            *M = (int)best_k;
            return;
        }
        h_prev2 = h_prev;
        h_prev = h;
        k_prev2 = k_prev;
        k_prev = k;
        const int64_t r = num - a * den;
        num = den;
        den = r;
    }
    *L = (int)h_prev;
    *M = (int)k_prev;
}

static int
rate_plan_backend_known(dsd_rate_plan_backend b) {
    return b >= DSD_RATE_PLAN_BACKEND_RTL_USB && b <= DSD_RATE_PLAN_BACKEND_WAV;
}

static int
rate_plan_backend_is_rtl(dsd_rate_plan_backend b) {
    return b == DSD_RATE_PLAN_BACKEND_RTL_USB || b == DSD_RATE_PLAN_BACKEND_RTL_TCP;
}

static int
rate_plan_input_ok(const dsd_rate_plan_input* in) {
    const uint32_t max = (uint32_t)INT32_MAX;
    return rate_plan_backend_known(in->backend) && in->request_hz > 0U && in->request_hz <= max
           && in->programmed_hz > 0U && in->programmed_hz <= max && in->readback_hz <= max && in->target_hz > 0U
           && in->target_hz <= max && in->max_passes >= 0 && in->max_passes <= DSD_RATE_PLAN_MAX_PASSES;
}

/* The programmed rate when the read-back is within the tolerance of it (or not read back yet), else the read-back. */
static uint32_t
rate_plan_nominal(uint32_t programmed_hz, uint32_t readback_hz) {
    if (readback_hz == 0U) {
        return programmed_hz;
    }
    const int64_t diff = rate_plan_abs64((int64_t)readback_hz - (int64_t)programmed_hz);
    const int64_t tolerance = (int64_t)DSD_RATE_PLAN_READBACK_TOLERANCE_PPM * (int64_t)programmed_hz;
    return diff * 1000000 <= tolerance ? programmed_hz : readback_hz;
}

/* The most passes up to @p max_passes that keep nominal / 2^p at or above the target. nominal >= target. */
static int
rate_plan_passes(uint32_t nominal_hz, uint32_t target_hz, int max_passes) {
    int p = 0;
    while (p < max_passes && ((int64_t)target_hz << (p + 1)) <= (int64_t)nominal_hz) {
        p++;
    }
    return p;
}

/* Stores @p num / 2^@p shift in lowest terms. */
static void
rate_plan_set_hb_out(dsd_rate_plan* out, int64_t num, unsigned shift) {
    while (shift > 0U && (num & 1) == 0) {
        num /= 2;
        shift--;
    }
    out->hb_out_num = num;
    out->hb_out_shift = shift;
}

/* Picks the step's L/M for nominal to target x 2^p: exact where both reduced terms fit, else the closest fit within
   DSD_RATE_PLAN_MAX_APPROX_PPM. Refuses (with the message set) when none is. */
static int
rate_plan_pick_step(dsd_rate_plan* out) {
    const int64_t want = (int64_t)out->target_hz << out->passes;
    const int64_t nominal = (int64_t)out->nominal_hz;
    const int64_t g = rate_plan_gcd64(want, nominal);
    const int64_t p = want / g;
    const int64_t q = nominal / g;
    int L = (int)p;
    int M = (int)q;
    double ppm = 0.0;
    if (p > DSD_RATE_PLAN_MAX_RATIO_TERM || q > DSD_RATE_PLAN_MAX_RATIO_TERM) {
        rate_plan_best_ratio(p, q, &L, &M);
        /* (L x nominal) / (M x want) - 1: the step's output rate against the target. */
        const int64_t err = (int64_t)L * nominal - (int64_t)M * want;
        ppm = (double)err * 1e6 / ((double)M * (double)want);
        if (!(fabs(ppm) <= DSD_RATE_PLAN_MAX_APPROX_PPM)) {
            DSD_SNPRINTF(
                out->message, sizeof(out->message),
                "dsd-neo cannot convert the device's %u Hz to the %u Hz DSP bandwidth closely enough (%.0f ppm "
                "off, %.0f allowed); choose another DSP bandwidth",
                (unsigned)out->nominal_hz, (unsigned)out->target_hz, fabs(ppm), DSD_RATE_PLAN_MAX_APPROX_PPM);
            return DSD_RATE_PLAN_ERR_RATIO;
        }
    }
    out->step.enabled = 1;
    out->step.L = L;
    out->step.M = M;
    out->step.approx_ppm = ppm;
    dsd_rate_plan_step_edges(out->target_hz, &out->step.protect_hz, &out->step.stop_hz);
    out->step_out_hz = out->target_hz;
    return DSD_RATE_PLAN_OK;
}

/* The rest of the plan once nominal, the forced flag and eligibility are set. */
static int
rate_plan_chain(const dsd_rate_plan_input* in, dsd_rate_plan* out) {
    if (rate_plan_backend_is_rtl(in->backend) && out->nominal_hz != out->programmed_hz) {
        DSD_SNPRINTF(out->message, sizeof(out->message),
                     "the RTL-SDR read back %u Hz for a requested %u Hz; dsd-neo needs its rate within %d ppm",
                     (unsigned)out->readback_hz, (unsigned)out->programmed_hz, DSD_RATE_PLAN_READBACK_TOLERANCE_PPM);
        return DSD_RATE_PLAN_ERR_READBACK;
    }
    if (out->nominal_hz < out->target_hz) {
        DSD_SNPRINTF(out->message, sizeof(out->message),
                     "the device runs at %u Hz, slower than the %u Hz DSP bandwidth; choose a smaller DSP bandwidth",
                     (unsigned)out->nominal_hz, (unsigned)out->target_hz);
        return DSD_RATE_PLAN_ERR_BELOW_TARGET;
    }
    out->passes = rate_plan_passes(out->nominal_hz, out->target_hz, in->max_passes);
    rate_plan_set_hb_out(out, (int64_t)out->nominal_hz, (unsigned)out->passes);
    const int on_target = ((int64_t)out->target_hz << out->passes) == (int64_t)out->nominal_hz;
    if (out->step_eligible && !on_target) {
        return rate_plan_pick_step(out);
    }
    if (out->hb_out_shift != 0U) {
        DSD_SNPRINTF(out->message, sizeof(out->message),
                     "the device's %u Hz does not divide down to the %u Hz DSP bandwidth in whole hertz (%.4f Hz), and "
                     "this input cannot convert it",
                     (unsigned)out->nominal_hz, (unsigned)out->target_hz,
                     ldexp((double)out->hb_out_num, -(int)out->hb_out_shift));
        return DSD_RATE_PLAN_ERR_FRACTIONAL;
    }
    out->step_out_hz = (uint32_t)out->hb_out_num;
    return DSD_RATE_PLAN_OK;
}

int
dsd_rate_plan_compute(const dsd_rate_plan_input* in, dsd_rate_plan* out) {
    if (!out) {
        return DSD_RATE_PLAN_ERR_ARGS;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!in || !rate_plan_input_ok(in)) {
        out->status = DSD_RATE_PLAN_ERR_ARGS;
        return out->status;
    }
    out->backend = in->backend;
    out->request_hz = in->request_hz;
    out->programmed_hz = in->programmed_hz;
    out->readback_hz = in->readback_hz;
    out->target_hz = in->target_hz;
    /* Tolerance first: the passes are chosen from the nominal rate, never from a read-back a few Hz off it. */
    out->nominal_hz = rate_plan_nominal(in->programmed_hz, in->readback_hz);
    out->device_forced = in->programmed_hz != in->request_hz || out->nominal_hz != in->programmed_hz;
    out->step_eligible = in->backend == DSD_RATE_PLAN_BACKEND_AIRSPY || in->backend == DSD_RATE_PLAN_BACKEND_SOAPY
                         || (in->backend == DSD_RATE_PLAN_BACKEND_REPLAY_KEEP && out->device_forced);
    out->status = rate_plan_chain(in, out);
    if (out->status != DSD_RATE_PLAN_OK) {
        DSD_MEMSET(&out->step, 0, sizeof(out->step));
        out->step_out_hz = 0U;
    }
    return out->status;
}

void
dsd_rate_plan_step_edges(uint32_t target_hz, int* protect_hz, int* stop_hz) {
    /* 0.45 x target as 9 / 20, in 64 bits. */
    const int64_t protect = (int64_t)target_hz * 9 / 20;
    if (protect_hz) {
        *protect_hz = (int)protect;
    }
    if (stop_hz) {
        *stop_hz = (int)((int64_t)target_hz - protect);
    }
}

/* T for the step: iq_resampler_pick_taps() at the step's edges and the exact half-band output rate. */
static int
rate_plan_taps_per_phase(double in, int L, double pass, double stop) {
    /* Kaiser: order (A - 7.95) / (2.285 x transition in radians per sample), at the L-times rate. */
    const double fh = in * (double)L;
    const double order = (RATE_PLAN_DESIGN_DB - 7.95) / (2.285 * 2.0 * kRatePlanPi * (stop - pass) / fh);
    const double taps_per_phase = ceil((ceil(order) + 1.0) / (double)L);
    int T = (int)taps_per_phase;
    T = (T + 3) & ~3;
    return T < 4 ? 4 : T;
}

int
dsd_rate_plan_step_bounds(const dsd_rate_plan* plan, dsd_rate_plan_bounds* out) {
    if (!plan || !out) {
        return DSD_RATE_PLAN_ERR_ARGS;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (plan->status != DSD_RATE_PLAN_OK || !plan->step.enabled || plan->step.L < 1) {
        return DSD_RATE_PLAN_OK;
    }
    const double hb_out = ldexp((double)plan->hb_out_num, -(int)plan->hb_out_shift);
    const int L = plan->step.L;
    const int T = rate_plan_taps_per_phase(hb_out, L, (double)plan->step.protect_hz, (double)plan->step.stop_hz);
    out->taps_per_phase = T;
    /* As dsd_iq_resampler_group_delay_inputs() and _seconds() compute them, so the values match bit for bit. */
    out->group_delay_inputs = ((double)L * (double)T - 1.0) / (2.0 * (double)L);
    out->group_delay_s = ((double)L * (double)T - 1.0) / (2.0 * (double)L * hb_out);
    out->tail_inputs = (uint32_t)(T - 1);
    out->tail_s = (double)out->tail_inputs / hb_out;
    return DSD_RATE_PLAN_OK;
}
