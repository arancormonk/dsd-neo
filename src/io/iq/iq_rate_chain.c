// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The live rate chain a capture records and a replay reads back (issue #806). See <dsd-neo/io/iq_types.h>.
 *
 * The capture writer checks the chain before it writes it and the replay reader checks it as it reads it, both here,
 * so a sidecar the writer accepts is one the reader accepts.
 */

#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rate_plan.h>

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

/* How far iq_step_approx_ppm may be from the planner's value: the writer prints it to 6 decimals. */
#define IQ_RATE_CHAIN_PPM_TOLERANCE 1e-4

static void iq_rate_chain_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
iq_rate_chain_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0U || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1U] = '\0';
}

/* Whether @p rate_hz is within the read-back tolerance of @p ref_hz (inclusive), as the rate planner counts it. */
static int
iq_rate_within_tolerance(uint32_t rate_hz, uint64_t ref_hz) {
    if (ref_hz == 0U) {
        return 0;
    }
    const uint64_t diff = rate_hz > ref_hz ? rate_hz - ref_hz : ref_hz - rate_hz;
    return diff * 1000000U <= (uint64_t)DSD_RATE_PLAN_READBACK_TOLERANCE_PPM * ref_hz;
}

/* The RTL-shaped request of a recorded chain that @p sample_rate_hz is within tolerance of: the DSP bandwidth times the
   base decimation, or that times the post decimation for a chain whose bandwidth names the rate after it; 0 for none. */
static uint64_t
iq_rate_chain_matched_request(uint32_t sample_rate_hz, int dsp_bw_khz, uint32_t base_decimation,
                              uint32_t post_downsample) {
    if (dsp_bw_khz <= 0 || base_decimation == 0U) {
        return 0U;
    }
    const uint64_t request_hz = (uint64_t)dsp_bw_khz * 1000U * base_decimation;
    if (iq_rate_within_tolerance(sample_rate_hz, request_hz)) {
        return request_hz;
    }
    if (post_downsample > 1U && iq_rate_within_tolerance(sample_rate_hz, request_hz * post_downsample)) {
        return request_hz * post_downsample;
    }
    return 0U;
}

int
dsd_iq_rate_chain_derived_forced(uint32_t sample_rate_hz, int dsp_bw_khz, uint32_t base_decimation,
                                 uint32_t post_downsample) {
    if (dsp_bw_khz <= 0 || base_decimation == 0U) {
        return 0;
    }
    return iq_rate_chain_matched_request(sample_rate_hz, dsp_bw_khz, base_decimation, post_downsample) == 0U ? 1 : 0;
}

uint32_t
dsd_iq_rate_chain_derived_nominal(uint32_t sample_rate_hz, int dsp_bw_khz, uint32_t base_decimation,
                                  uint32_t post_downsample) {
    const uint64_t request_hz =
        iq_rate_chain_matched_request(sample_rate_hz, dsp_bw_khz, base_decimation, post_downsample);
    return (request_hz != 0U && request_hz <= UINT32_MAX) ? (uint32_t)request_hz : sample_rate_hz;
}

/* The passes behind @p base_decimation, or -1 when it is no power of two the planner takes. */
static int
iq_rate_chain_passes(uint32_t base_decimation) {
    for (int p = 0; p <= DSD_RATE_PLAN_MAX_PASSES; p++) {
        if ((1U << p) == base_decimation) {
            return p;
        }
    }
    return -1;
}

/* A recorded chain that does not step: its DSP rate is the half-band output, which must be whole. */
static int
iq_rate_chain_check_unstepped(const dsd_iq_rate_chain* chain, uint32_t nominal_hz, uint32_t base, char* err_buf,
                              size_t err_buf_size) {
    if (fabs(chain->step_approx_ppm) > IQ_RATE_CHAIN_PPM_TOLERANCE) {
        iq_rate_chain_error(err_buf, err_buf_size, "iq_step_approx_ppm (%.6f) is set for a chain that does not step",
                            chain->step_approx_ppm);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (nominal_hz % base != 0U || nominal_hz / base != chain->dsp_rate_hz) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "dsp_rate_hz (%u) is not nominal_rate_hz/base_decimation (%u/%u), and iq_step_l/iq_step_m "
                            "say the chain does not step",
                            (unsigned)chain->dsp_rate_hz, (unsigned)nominal_hz, (unsigned)base);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    return DSD_IQ_OK;
}

/* A recorded chain that steps: its ratio must be the planner's for the nominal rate, the passes and the DSP rate. */
static int
iq_rate_chain_check_stepped(const dsd_iq_rate_chain* chain, uint32_t nominal_hz, uint32_t base, int passes,
                            char* err_buf, size_t err_buf_size) {
    if (chain->has_forced && !chain->device_forced) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "iq_step_l/iq_step_m (%u/%u) say the chain steps, but capture_rate_device_forced is false: "
                            "only a rate the device forced steps",
                            (unsigned)chain->step_l, (unsigned)chain->step_m);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    const uint64_t request_hz = (uint64_t)chain->dsp_rate_hz << passes;
    dsd_rate_plan plan;
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = DSD_RATE_PLAN_BACKEND_REPLAY_KEEP;
    /* The RTL-shaped request for the recorded DSP rate: a chain that steps did not get it, so it plans as forced. */
    in.request_hz = request_hz <= (uint64_t)INT32_MAX ? (uint32_t)request_hz : 0U;
    in.programmed_hz = nominal_hz;
    in.target_hz = chain->dsp_rate_hz;
    in.max_passes = passes;
    if (dsd_rate_plan_compute(&in, &plan) != DSD_RATE_PLAN_OK) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "no step takes nominal_rate_hz/base_decimation (%u/%u) to dsp_rate_hz (%u)%s%s",
                            (unsigned)nominal_hz, (unsigned)base, (unsigned)chain->dsp_rate_hz,
                            plan.message[0] ? ": " : "", plan.message);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (plan.passes != passes) {
        iq_rate_chain_error(
            err_buf, err_buf_size,
            "base_decimation (%u) takes nominal_rate_hz (%u) below dsp_rate_hz (%u), which the recorded "
            "step cannot reach",
            (unsigned)base, (unsigned)nominal_hz, (unsigned)chain->dsp_rate_hz);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (!plan.step.enabled) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "iq_step_l/iq_step_m (%u/%u) say the chain steps, but nominal_rate_hz/base_decimation "
                            "(%u/%u) reaches dsp_rate_hz (%u) with no step",
                            (unsigned)chain->step_l, (unsigned)chain->step_m, (unsigned)nominal_hz, (unsigned)base,
                            (unsigned)chain->dsp_rate_hz);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if ((uint32_t)plan.step.L != chain->step_l || (uint32_t)plan.step.M != chain->step_m) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "iq_step_l/iq_step_m (%u/%u) is not the step from nominal_rate_hz/base_decimation (%u/%u) "
                            "to dsp_rate_hz (%u), which is %d/%d",
                            (unsigned)chain->step_l, (unsigned)chain->step_m, (unsigned)nominal_hz, (unsigned)base,
                            (unsigned)chain->dsp_rate_hz, plan.step.L, plan.step.M);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (!(fabs(chain->step_approx_ppm - plan.step.approx_ppm) <= IQ_RATE_CHAIN_PPM_TOLERANCE)) {
        iq_rate_chain_error(err_buf, err_buf_size, "iq_step_approx_ppm (%.6f) is not the %d/%d step's %.6f",
                            chain->step_approx_ppm, plan.step.L, plan.step.M, plan.step.approx_ppm);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    return DSD_IQ_OK;
}

static int
iq_rate_chain_check_step(const dsd_iq_rate_chain* chain, uint32_t nominal_hz, uint32_t base, char* err_buf,
                         size_t err_buf_size) {
    const int passes = iq_rate_chain_passes(base);
    if (passes < 0) {
        iq_rate_chain_error(err_buf, err_buf_size, "base_decimation (%u) is not a power of two", (unsigned)base);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (chain->dsp_rate_hz == 0U) {
        iq_rate_chain_error(err_buf, err_buf_size, "dsp_rate_hz must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if ((chain->step_l == 0U) != (chain->step_m == 0U)) {
        iq_rate_chain_error(err_buf, err_buf_size,
                            "iq_step_l and iq_step_m (%u/%u) must both be 0 (no step) or both be set",
                            (unsigned)chain->step_l, (unsigned)chain->step_m);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (chain->step_l == 0U) {
        return iq_rate_chain_check_unstepped(chain, nominal_hz, base, err_buf, err_buf_size);
    }
    return iq_rate_chain_check_stepped(chain, nominal_hz, base, passes, err_buf, err_buf_size);
}

int
dsd_iq_rate_chain_check(const dsd_iq_rate_chain* chain, uint32_t sample_rate_hz, uint32_t base_decimation,
                        char* err_buf, size_t err_buf_size) {
    if (!chain) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    uint32_t nominal_hz = sample_rate_hz;
    if (chain->has_nominal) {
        if (chain->nominal_rate_hz == 0U || !iq_rate_within_tolerance(sample_rate_hz, chain->nominal_rate_hz)) {
            iq_rate_chain_error(
                err_buf, err_buf_size, "nominal_rate_hz (%u) is not within %d ppm of sample_rate_hz (%u)",
                (unsigned)chain->nominal_rate_hz, DSD_RATE_PLAN_READBACK_TOLERANCE_PPM, (unsigned)sample_rate_hz);
            return DSD_IQ_ERR_RATE_CHAIN;
        }
        nominal_hz = chain->nominal_rate_hz;
    }
    if (!chain->has_step) {
        return DSD_IQ_OK;
    }
    return iq_rate_chain_check_step(chain, nominal_hz, base_decimation, err_buf, err_buf_size);
}
