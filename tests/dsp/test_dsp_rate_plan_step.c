// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The rate plan (io_iq, issue #806) against the I/Q resampler it plans for (dsp). io_iq cannot link the DSP module, so
   the plan repeats the resampler's limits and its Kaiser tap estimate; this pins the copies to the originals. Every
   step the plan makes over the Airspy and SoapySDR rate grid, an approximated ratio and a capped chain, designs with
   configure_ratio() from the plan's own fields, to the taps per phase, delay and tail the plan reported, bit for bit. */

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/io/rate_plan.h>

#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/safe_api.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static int
same_double(double a, double b) {
    uint64_t x = 0U;
    uint64_t y = 0U;
    DSD_MEMCPY(&x, &a, sizeof(x));
    DSD_MEMCPY(&y, &b, sizeof(y));
    return x == y;
}

static void
test_limits(void) {
    CHECK(same_double(DSD_RATE_PLAN_MAX_APPROX_PPM, DSD_IQ_RESAMPLER_MAX_APPROX_PPM),
          "approximation bound %.3f vs %.3f", DSD_RATE_PLAN_MAX_APPROX_PPM, DSD_IQ_RESAMPLER_MAX_APPROX_PPM);
    CHECK(DSD_RATE_PLAN_MAX_RATIO_TERM == DSD_IQ_RESAMPLER_MAX_RATIO_TERM, "ratio term %d vs %d",
          DSD_RATE_PLAN_MAX_RATIO_TERM, DSD_IQ_RESAMPLER_MAX_RATIO_TERM);
    CHECK(DSD_RATE_PLAN_MAX_PASSES == DSD_IQ_RESAMPLER_MAX_RATE_SHIFT, "passes %d vs shift %d",
          DSD_RATE_PLAN_MAX_PASSES, DSD_IQ_RESAMPLER_MAX_RATE_SHIFT);
}

/* Designs @p p's step and compares it with what the plan reported. Returns the taps per phase, or 0. */
static int
check_design(const char* name, const dsd_rate_plan* p) {
    dsd_rate_plan_bounds b;
    CHECK(dsd_rate_plan_step_bounds(p, &b) == DSD_RATE_PLAN_OK, "%s: bounds", name);
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    const int st = dsd_iq_resampler_configure_ratio(&r, p->step.L, p->step.M, p->hb_out_num, p->hb_out_shift,
                                                    p->step.protect_hz, p->step.stop_hz, 512);
    CHECK(st == DSD_IQ_RESAMPLER_OK, "%s: configure_ratio %d (%s)", name, st, dsd_iq_resampler_status_text(st));
    int taps = 0;
    if (st == DSD_IQ_RESAMPLER_OK) {
        taps = r.taps_per_phase;
        CHECK(b.taps_per_phase == r.taps_per_phase, "%s: plan %d taps per phase, design %d", name, b.taps_per_phase,
              r.taps_per_phase);
        CHECK(same_double(b.group_delay_inputs, dsd_iq_resampler_group_delay_inputs(&r))
                  && same_double(b.group_delay_s, dsd_iq_resampler_group_delay_seconds(&r)),
              "%s: delay %.12f / %.12g s, design %.12f / %.12g s", name, b.group_delay_inputs, b.group_delay_s,
              dsd_iq_resampler_group_delay_inputs(&r), dsd_iq_resampler_group_delay_seconds(&r));
        CHECK((size_t)b.tail_inputs == dsd_iq_resampler_tail_inputs(&r), "%s: tail %u, design %zu", name,
              (unsigned)b.tail_inputs, dsd_iq_resampler_tail_inputs(&r));
        CHECK(dsd_iq_resampler_matches(&r, p->step.L, p->step.M, p->hb_out_num, p->hb_out_shift, p->step.protect_hz,
                                       p->step.stop_hz),
              "%s: the design does not match the plan's key", name);
    }
    dsd_iq_resampler_free(&r);
    return taps;
}

static int
plan_step(uint32_t request_hz, uint32_t rate_hz, uint32_t target_hz, int max_passes, dsd_rate_plan* p) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = DSD_RATE_PLAN_BACKEND_SOAPY;
    in.request_hz = request_hz;
    in.programmed_hz = rate_hz;
    in.readback_hz = rate_hz;
    in.target_hz = target_hz;
    in.max_passes = max_passes;
    return dsd_rate_plan_compute(&in, p);
}

/* Every grid step designs to the plan's taps, and the taps are the ones test_dsp_iq_resampler.c pins for the live
   ratios (44 to 100 per phase). */
static void
test_grid(void) {
    static const uint32_t kRates[] = {2500000U, 10000000U, 3000000U, 6000000U, 2000000U};
    static const uint32_t kTargets[] = {4000U, 6000U, 8000U, 12000U, 16000U, 24000U, 48000U, 72000U};
    int min_taps = 1 << 30;
    int max_taps = 0;
    for (size_t i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++) {
        for (size_t j = 0; j < sizeof(kTargets) / sizeof(kTargets[0]); j++) {
            char name[48];
            DSD_SNPRINTF(name, sizeof(name), "%u Hz to %u Hz", (unsigned)kRates[i], (unsigned)kTargets[j]);
            dsd_rate_plan p;
            const int rc = plan_step(1536000U, kRates[i], kTargets[j], 10, &p);
            CHECK(rc == DSD_RATE_PLAN_OK && p.step.enabled, "%s: plan %d, step %d", name, rc, p.step.enabled);
            if (rc != DSD_RATE_PLAN_OK || !p.step.enabled) {
                continue;
            }
            const int taps = check_design(name, &p);
            if (taps > 0) {
                min_taps = taps < min_taps ? taps : min_taps;
                max_taps = taps > max_taps ? taps : max_taps;
            }
        }
    }
    CHECK(min_taps == 44 && max_taps == 100, "grid taps %d..%d, want 44..100", min_taps, max_taps);
}

/* An approximated ratio (2000001 Hz to 48 kHz as 96/125, 0.5 ppm over) and a chain capped short of its passes
   (10 MS/s to 4 kHz in 9) design the same way. */
static void
test_off_grid(void) {
    dsd_rate_plan p;
    CHECK(plan_step(1536000U, 2000001U, 48000U, 10, &p) == DSD_RATE_PLAN_OK && p.step.L == 96 && p.step.M == 125
              && p.step.approx_ppm > 0.0,
          "2000001: %d/%d", p.step.L, p.step.M);
    (void)check_design("2000001 Hz to 48000 Hz", &p);
    CHECK(plan_step(1024000U, 10000000U, 4000U, 9, &p) == DSD_RATE_PLAN_OK && p.passes == 9, "9 passes: %d", p.passes);
    (void)check_design("10 MS/s to 4000 Hz in 9 passes", &p);
}

int
main(void) {
    test_limits();
    test_grid();
    test_off_grid();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DSP_RATE_PLAN_STEP: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("DSP_RATE_PLAN_STEP: OK\n");
    return 0;
}
