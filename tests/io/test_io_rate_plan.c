// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The live rate plan (issue #806): dsd_rate_plan_compute() over every backend.
 *
 * - The grid: the Airspy rates (R2 2.5 and 10 MS/s, Mini 3 and 6 MS/s) and a SoapySDR 2 MS/s device at every DSP
 *   bandwidth: passes, the exact half-band output fraction, L/M, the step landing on the target with a whole number of
 *   samples per symbol, and the step's taps, delay and tail (2.5 MS/s to 4 kHz: about 5.3 ms). The backends that cannot
 *   step (RTL-SDR, rtl_tcp, the replays that do not keep a forced chain, WAV) plan the same passes and never step.
 * - Tolerance before passes: 1535999 Hz against 1536000 Hz plans as 1536000 Hz, unforced; the 100 ppm edges, inclusive.
 * - Refusals: an RTL-SDR or rtl_tcp read-back out of tolerance (1536200 Hz, 960123 Hz for 30 kHz) with the read-back
 *   and the forced flag still reported, a SoapySDR 1536200 Hz at 48 kHz (7680/7681, best fit about 114 ppm off), a rate
 *   below the target, and bad arguments. One read-back whose best fitting ratio is within 5 ppm is accepted.
 * - device_forced stays set across the read-back of an Airspy rate already programmed.
 */

#include <dsd-neo/io/rate_plan.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* One grid point: the device rate, the DSP bandwidth rate, and what the plan must be (research A's rate table). */
typedef struct {
    const char* name;
    uint32_t rate_hz;
    uint32_t target_hz;
    int passes;
    int64_t hb_num; /* hb_out = hb_num / 2^hb_shift, lowest terms */
    unsigned hb_shift;
    int L;
    int M;
    int T; /* taps per phase */
} grid_point;

static const grid_point kGrid[] = {
    {"R2 2.5M 4k", 2500000U, 4000, 9, 78125LL, 4u, 512, 625, 52},
    {"R2 2.5M 6k", 2500000U, 6000, 8, 78125LL, 3u, 384, 625, 68},
    {"R2 2.5M 8k", 2500000U, 8000, 8, 78125LL, 3u, 512, 625, 52},
    {"R2 2.5M 12k", 2500000U, 12000, 7, 78125LL, 2u, 384, 625, 68},
    {"R2 2.5M 16k", 2500000U, 16000, 7, 78125LL, 2u, 512, 625, 52},
    {"R2 2.5M 24k", 2500000U, 24000, 6, 78125LL, 1u, 384, 625, 68},
    {"R2 2.5M 48k", 2500000U, 48000, 5, 78125LL, 0u, 384, 625, 68},
    {"R2 2.5M 72k", 2500000U, 72000, 5, 78125LL, 0u, 576, 625, 44},
    {"R2 10M 4k", 10000000U, 4000, 10, 78125LL, 3u, 256, 625, 100},
    {"R2 10M 6k", 10000000U, 6000, 10, 78125LL, 3u, 384, 625, 68},
    {"R2 10M 8k", 10000000U, 8000, 10, 78125LL, 3u, 512, 625, 52},
    {"R2 10M 12k", 10000000U, 12000, 9, 78125LL, 2u, 384, 625, 68},
    {"R2 10M 16k", 10000000U, 16000, 9, 78125LL, 2u, 512, 625, 52},
    {"R2 10M 24k", 10000000U, 24000, 8, 78125LL, 1u, 384, 625, 68},
    {"R2 10M 48k", 10000000U, 48000, 7, 78125LL, 0u, 384, 625, 68},
    {"R2 10M 72k", 10000000U, 72000, 7, 78125LL, 0u, 576, 625, 44},
    {"Mini 3M 4k", 3000000U, 4000, 9, 46875LL, 3u, 256, 375, 60},
    {"Mini 3M 6k", 3000000U, 6000, 8, 46875LL, 2u, 64, 125, 80},
    {"Mini 3M 8k", 3000000U, 8000, 8, 46875LL, 2u, 256, 375, 60},
    {"Mini 3M 12k", 3000000U, 12000, 7, 46875LL, 1u, 64, 125, 80},
    {"Mini 3M 16k", 3000000U, 16000, 7, 46875LL, 1u, 256, 375, 60},
    {"Mini 3M 24k", 3000000U, 24000, 6, 46875LL, 0u, 64, 125, 80},
    {"Mini 3M 48k", 3000000U, 48000, 5, 93750LL, 0u, 64, 125, 80},
    {"Mini 3M 72k", 3000000U, 72000, 5, 93750LL, 0u, 96, 125, 52},
    {"Mini 6M 4k", 6000000U, 4000, 10, 46875LL, 3u, 256, 375, 60},
    {"Mini 6M 6k", 6000000U, 6000, 9, 46875LL, 2u, 64, 125, 80},
    {"Mini 6M 8k", 6000000U, 8000, 9, 46875LL, 2u, 256, 375, 60},
    {"Mini 6M 12k", 6000000U, 12000, 8, 46875LL, 1u, 64, 125, 80},
    {"Mini 6M 16k", 6000000U, 16000, 8, 46875LL, 1u, 256, 375, 60},
    {"Mini 6M 24k", 6000000U, 24000, 7, 46875LL, 0u, 64, 125, 80},
    {"Mini 6M 48k", 6000000U, 48000, 6, 93750LL, 0u, 64, 125, 80},
    {"Mini 6M 72k", 6000000U, 72000, 6, 93750LL, 0u, 96, 125, 52},
    {"Soapy 2M 4k", 2000000U, 4000, 8, 15625LL, 1u, 64, 125, 80},
    {"Soapy 2M 6k", 2000000U, 6000, 8, 15625LL, 1u, 96, 125, 52},
    {"Soapy 2M 8k", 2000000U, 8000, 7, 15625LL, 0u, 64, 125, 80},
    {"Soapy 2M 12k", 2000000U, 12000, 7, 15625LL, 0u, 96, 125, 52},
    {"Soapy 2M 16k", 2000000U, 16000, 6, 31250LL, 0u, 64, 125, 80},
    {"Soapy 2M 24k", 2000000U, 24000, 6, 31250LL, 0u, 96, 125, 52},
    {"Soapy 2M 48k", 2000000U, 48000, 5, 62500LL, 0u, 96, 125, 52},
    {"Soapy 2M 72k", 2000000U, 72000, 4, 125000LL, 0u, 72, 125, 72},
};

#define N_GRID     (sizeof(kGrid) / sizeof(kGrid[0]))

/* The live chain's half-band cap (kMaxDownsamplePasses). */
#define MAX_PASSES 10

/* The RTL-shaped rate optimal_settings() asks for at each DSP bandwidth, and its passes. */
static uint32_t
rtl_request_hz(uint32_t target_hz) {
    switch (target_hz) {
        case 4000U:
        case 8000U:
        case 16000U: return 1024000U;
        case 72000U: return 1152000U;
        default: return 1536000U;
    }
}

static int
rtl_request_passes(uint32_t target_hz) {
    int p = 0;
    while ((target_hz << (p + 1)) <= rtl_request_hz(target_hz)) {
        p++;
    }
    return p;
}

static dsd_rate_plan_input
input_for(dsd_rate_plan_backend backend, uint32_t request_hz, uint32_t programmed_hz, uint32_t readback_hz,
          uint32_t target_hz) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = backend;
    in.request_hz = request_hz;
    in.programmed_hz = programmed_hz;
    in.readback_hz = readback_hz;
    in.target_hz = target_hz;
    in.max_passes = MAX_PASSES;
    return in;
}

static int
plan(dsd_rate_plan_backend backend, uint32_t request_hz, uint32_t programmed_hz, uint32_t readback_hz,
     uint32_t target_hz, dsd_rate_plan* out) {
    const dsd_rate_plan_input in = input_for(backend, request_hz, programmed_hz, readback_hz, target_hz);
    return dsd_rate_plan_compute(&in, out);
}

static int
step_off(const dsd_rate_plan* p) {
    return !p->step.enabled && p->step.L == 0 && p->step.M == 0 && p->step.protect_hz == 0 && p->step.stop_hz == 0
           && fabs(p->step.approx_ppm) <= 0.0;
}

static int
message_names(const dsd_rate_plan* p, const char* text) {
    return strstr(p->message, text) != NULL;
}

/* A refusal a user reads names rates and the DSP bandwidth, not the filters inside the chain. */
static int
plain_message(const dsd_rate_plan* p) {
    static const char* const kJargon[] = {"half-band", "step ratio", "upsample", "resample", "fractional", "L/M"};
    for (size_t i = 0; i < sizeof(kJargon) / sizeof(kJargon[0]); i++) {
        if (strstr(p->message, kJargon[i]) != NULL) {
            return 0;
        }
    }
    return p->message[0] != '\0';
}

/* A stepping plan at grid point @p k: everything the step and the half-band chain run with. */
static void
check_stepping(const char* backend, const grid_point* k, const dsd_rate_plan* p, int rc) {
    CHECK(rc == DSD_RATE_PLAN_OK && p->status == DSD_RATE_PLAN_OK && p->message[0] == '\0', "%s %s: status %d (%s)",
          backend, k->name, rc, p->message);
    CHECK(p->nominal_hz == k->rate_hz && p->passes == k->passes, "%s %s: nominal %u, %d passes", backend, k->name,
          (unsigned)p->nominal_hz, p->passes);
    CHECK(p->hb_out_num == k->hb_num && p->hb_out_shift == k->hb_shift, "%s %s: hb_out %lld/2^%u", backend, k->name,
          (long long)p->hb_out_num, p->hb_out_shift);
    CHECK(p->device_forced && p->step_eligible, "%s %s: forced %d, eligible %d", backend, k->name, p->device_forced,
          p->step_eligible);
    CHECK(p->step.enabled && p->step.L == k->L && p->step.M == k->M && fabs(p->step.approx_ppm) <= 0.0,
          "%s %s: step %d %d/%d %.6f ppm", backend, k->name, p->step.enabled, p->step.L, p->step.M, p->step.approx_ppm);
    CHECK(p->step.protect_hz == (int)(k->target_hz * 9U / 20U)
              && p->step.stop_hz == (int)k->target_hz - p->step.protect_hz,
          "%s %s: edges %d/%d", backend, k->name, p->step.protect_hz, p->step.stop_hz);
    CHECK(p->step_out_hz == k->target_hz, "%s %s: step_out %u", backend, k->name, (unsigned)p->step_out_hz);
    /* The step's output really is the target: hb_out x L / M, exactly. */
    CHECK(p->hb_out_num * p->step.L == ((int64_t)k->target_hz << p->hb_out_shift) * p->step.M,
          "%s %s: hb_out x L / M is not the target", backend, k->name);
}

/* The CQPSK rates (P25 phase 1 4800, phase 2 6000 symbols/s) and TETRA's 18000 land on a whole number of samples per
   symbol at the bandwidths they run at. */
static void
check_whole_sps(const grid_point* k, const dsd_rate_plan* p) {
    if (k->target_hz >= 24000U) {
        CHECK(p->step_out_hz % 4800U == 0U && p->step_out_hz % 6000U == 0U, "%s: %u Hz is not whole SPS for P25",
              k->name, (unsigned)p->step_out_hz);
    }
    if (k->target_hz == 72000U) {
        CHECK(p->step_out_hz % 18000U == 0U, "%s: %u Hz is not whole SPS for TETRA", k->name, (unsigned)p->step_out_hz);
    }
}

/* The step's taps, delay and tail at grid point @p k, from the formulas in the I/Q resampler's header. */
static void
check_bounds(const grid_point* k, const dsd_rate_plan* p) {
    dsd_rate_plan_bounds b;
    CHECK(dsd_rate_plan_step_bounds(p, &b) == DSD_RATE_PLAN_OK, "%s: bounds", k->name);
    const double hb_out = ldexp((double)k->hb_num, -(int)k->hb_shift);
    const double delay = ((double)k->L * (double)k->T - 1.0) / (2.0 * (double)k->L);
    CHECK(b.taps_per_phase == k->T, "%s: %d taps per phase, want %d", k->name, b.taps_per_phase, k->T);
    CHECK(fabs(b.group_delay_inputs - delay) <= 1e-12 && fabs(b.group_delay_s - delay / hb_out) <= 1e-15,
          "%s: delay %.9f inputs, %.9f s", k->name, b.group_delay_inputs, b.group_delay_s);
    CHECK(b.tail_inputs == (uint32_t)(k->T - 1) && fabs(b.tail_s - (double)(k->T - 1) / hb_out) <= 1e-15,
          "%s: tail %u inputs, %.9f s", k->name, (unsigned)b.tail_inputs, b.tail_s);
    /* No step delays more than the slowest grid point, 2.5 MS/s to 4 kHz. */
    CHECK(b.group_delay_s <= 5.33e-3 && b.tail_s <= 10.45e-3, "%s: delay %.6f s, tail %.6f s", k->name, b.group_delay_s,
          b.tail_s);
}

/* The forced backends step at every grid point; the provisional plan (no read-back yet) is the final one. */
static void
test_grid_steps(void) {
    static const dsd_rate_plan_backend kStepping[] = {DSD_RATE_PLAN_BACKEND_AIRSPY, DSD_RATE_PLAN_BACKEND_SOAPY,
                                                      DSD_RATE_PLAN_BACKEND_REPLAY_KEEP};
    static const char* const kNames[] = {"AIRSPY", "SOAPY", "REPLAY_KEEP"};
    for (size_t c = 0; c < N_GRID; c++) {
        const grid_point* k = &kGrid[c];
        const uint32_t request = rtl_request_hz(k->target_hz);
        for (size_t b = 0; b < sizeof(kStepping) / sizeof(kStepping[0]); b++) {
            dsd_rate_plan p;
            const int rc = plan(kStepping[b], request, k->rate_hz, k->rate_hz, k->target_hz, &p);
            check_stepping(kNames[b], k, &p, rc);
            check_whole_sps(k, &p);
            check_bounds(k, &p);
            dsd_rate_plan provisional;
            const int rc_provisional = plan(kStepping[b], request, k->rate_hz, 0U, k->target_hz, &provisional);
            check_stepping(kNames[b], k, &provisional, rc_provisional);
            CHECK(provisional.readback_hz == 0U, "%s %s: provisional read-back %u", kNames[b], k->name,
                  (unsigned)provisional.readback_hz);
        }
    }
}

/* The backends that cannot step plan the same passes at the same rates and never step: whole half-band outputs run as
   they are, fractional ones are refused. */
static void
test_grid_no_step(void) {
    static const dsd_rate_plan_backend kFixed[] = {
        DSD_RATE_PLAN_BACKEND_RTL_USB,      DSD_RATE_PLAN_BACKEND_RTL_TCP, DSD_RATE_PLAN_BACKEND_REPLAY_RESAMPLED,
        DSD_RATE_PLAN_BACKEND_REPLAY_EXACT, DSD_RATE_PLAN_BACKEND_WAV,
    };
    for (size_t c = 0; c < N_GRID; c++) {
        const grid_point* k = &kGrid[c];
        for (size_t b = 0; b < sizeof(kFixed) / sizeof(kFixed[0]); b++) {
            dsd_rate_plan p;
            const int rc = plan(kFixed[b], rtl_request_hz(k->target_hz), k->rate_hz, k->rate_hz, k->target_hz, &p);
            CHECK(!p.step_eligible && step_off(&p), "%s backend %d: steps", k->name, (int)kFixed[b]);
            CHECK(p.nominal_hz == k->rate_hz && p.passes == k->passes && p.hb_out_num == k->hb_num
                      && p.hb_out_shift == k->hb_shift && p.device_forced,
                  "%s backend %d: chain %u/%d %lld/2^%u forced %d", k->name, (int)kFixed[b], (unsigned)p.nominal_hz,
                  p.passes, (long long)p.hb_out_num, p.hb_out_shift, p.device_forced);
            if (k->hb_shift == 0U) {
                CHECK(rc == DSD_RATE_PLAN_OK && p.step_out_hz == (uint32_t)k->hb_num, "%s backend %d: %d, step_out %u",
                      k->name, (int)kFixed[b], rc, (unsigned)p.step_out_hz);
            } else {
                CHECK(rc == DSD_RATE_PLAN_ERR_FRACTIONAL && p.status == rc && p.step_out_hz == 0U
                          && message_names(&p, "whole hertz") && plain_message(&p),
                      "%s backend %d: %d (%s)", k->name, (int)kFixed[b], rc, p.message);
            }
        }
        /* A replay that keeps an unforced chain at the same rate does not step either. */
        dsd_rate_plan p;
        (void)plan(DSD_RATE_PLAN_BACKEND_REPLAY_KEEP, k->rate_hz, k->rate_hz, k->rate_hz, k->target_hz, &p);
        CHECK(!p.device_forced && !p.step_eligible && step_off(&p), "%s: unforced KEEP steps", k->name);
    }
}

/* Every bandwidth on an RTL-SDR and rtl_tcp in tolerance: the RTL-shaped request lands on the target, no step. */
static void
test_rtl_shaped(void) {
    static const uint32_t kTargets[] = {4000U, 6000U, 8000U, 12000U, 16000U, 24000U, 48000U, 72000U};
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++) {
        const uint32_t t = kTargets[i];
        const uint32_t req = rtl_request_hz(t);
        for (int tcp = 0; tcp <= 1; tcp++) {
            dsd_rate_plan p;
            const int rc =
                plan(tcp ? DSD_RATE_PLAN_BACKEND_RTL_TCP : DSD_RATE_PLAN_BACKEND_RTL_USB, req, req, req, t, &p);
            CHECK(rc == DSD_RATE_PLAN_OK && p.passes == rtl_request_passes(t) && p.hb_out_num == (int64_t)t
                      && p.hb_out_shift == 0U && p.step_out_hz == t && !p.device_forced && !p.step_eligible
                      && step_off(&p),
                  "RTL %u: rc %d, %d passes, %lld/2^%u, step_out %u, forced %d", (unsigned)t, rc, p.passes,
                  (long long)p.hb_out_num, p.hb_out_shift, (unsigned)p.step_out_hz, p.device_forced);
        }
    }
}

/* The read-back is held to the programmed rate before the passes are chosen. */
static void
test_tolerance_first(void) {
    dsd_rate_plan p;
    int rc = plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 1536000U, 1536000U, 1535999U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && p.nominal_hz == 1536000U && p.passes == 5 && p.hb_out_num == 48000
              && p.hb_out_shift == 0U && p.step_out_hz == 48000U && !p.device_forced && step_off(&p)
              && p.readback_hz == 1535999U,
          "1535999: rc %d nominal %u, %d passes, forced %d", rc, (unsigned)p.nominal_hz, p.passes, p.device_forced);
    /* On a SoapySDR device too: nothing to step. */
    rc = plan(DSD_RATE_PLAN_BACKEND_SOAPY, 1536000U, 1536000U, 1535999U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && p.nominal_hz == 1536000U && p.step_eligible && step_off(&p) && !p.device_forced,
          "Soapy 1535999: rc %d", rc);

    /* 100 ppm of 1536000 Hz is 153.6 Hz: 153 Hz either way is in, 154 Hz out. */
    static const uint32_t kIn[] = {1536153U, 1535847U};
    static const uint32_t kOut[] = {1536154U, 1535846U};
    for (size_t i = 0; i < 2; i++) {
        rc = plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 1536000U, 1536000U, kIn[i], 48000U, &p);
        CHECK(rc == DSD_RATE_PLAN_OK && p.nominal_hz == 1536000U && !p.device_forced && p.step_out_hz == 48000U,
              "%u: in tolerance, rc %d nominal %u", (unsigned)kIn[i], rc, (unsigned)p.nominal_hz);
        rc = plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 1536000U, 1536000U, kOut[i], 48000U, &p);
        CHECK(rc == DSD_RATE_PLAN_ERR_READBACK && p.nominal_hz == kOut[i] && p.device_forced,
              "%u: out of tolerance, rc %d nominal %u", (unsigned)kOut[i], rc, (unsigned)p.nominal_hz);
    }
    /* Exactly 100 ppm counts as in: 1 MS/s (15625 Hz x 2^6) read back 100 Hz either way, then 101 Hz. */
    static const uint32_t kEdge[] = {1000100U, 999900U};
    static const uint32_t kPast[] = {1000101U, 999899U};
    for (size_t i = 0; i < 2; i++) {
        rc = plan(DSD_RATE_PLAN_BACKEND_RTL_TCP, 1000000U, 1000000U, kEdge[i], 15625U, &p);
        CHECK(rc == DSD_RATE_PLAN_OK && p.nominal_hz == 1000000U && p.passes == 6 && p.step_out_hz == 15625U
                  && !p.device_forced,
              "%u: 100 ppm, rc %d nominal %u", (unsigned)kEdge[i], rc, (unsigned)p.nominal_hz);
        rc = plan(DSD_RATE_PLAN_BACKEND_RTL_TCP, 1000000U, 1000000U, kPast[i], 15625U, &p);
        CHECK(rc == DSD_RATE_PLAN_ERR_READBACK && p.nominal_hz == kPast[i] && p.device_forced,
              "%u: 101 ppm, rc %d nominal %u", (unsigned)kPast[i], rc, (unsigned)p.nominal_hz);
    }
}

/* An RTL-SDR or rtl_tcp read-back out of tolerance is refused, with the read-back and the forced flag reported. */
static void
test_rtl_readback_refused(void) {
    for (int tcp = 0; tcp <= 1; tcp++) {
        const dsd_rate_plan_backend be = tcp ? DSD_RATE_PLAN_BACKEND_RTL_TCP : DSD_RATE_PLAN_BACKEND_RTL_USB;
        dsd_rate_plan p;
        int rc = plan(be, 1536000U, 1536000U, 1536200U, 48000U, &p);
        CHECK(rc == DSD_RATE_PLAN_ERR_READBACK && p.status == rc && p.readback_hz == 1536200U
                  && p.nominal_hz == 1536200U && p.device_forced && !p.step_eligible && step_off(&p)
                  && p.step_out_hz == 0U,
              "RTL 1536200: rc %d nominal %u forced %d", rc, (unsigned)p.nominal_hz, p.device_forced);
        CHECK(message_names(&p, "1536200 Hz") && message_names(&p, "1536000 Hz") && message_names(&p, "100 ppm")
                  && plain_message(&p),
              "RTL 1536200 message: %s", p.message);
        /* 30 kHz asks for 960 kS/s; 960123 Hz is 128 ppm off. */
        rc = plan(be, 960000U, 960000U, 960123U, 30000U, &p);
        CHECK(rc == DSD_RATE_PLAN_ERR_READBACK && p.readback_hz == 960123U && p.nominal_hz == 960123U && p.device_forced
                  && step_off(&p),
              "RTL 960123: rc %d nominal %u forced %d", rc, (unsigned)p.nominal_hz, p.device_forced);
        CHECK(message_names(&p, "960123 Hz") && message_names(&p, "960000 Hz"), "RTL 960123 message: %s", p.message);
    }
}

/* A step ratio past 4096 is approximated within 5 ppm or refused. */
static void
test_ratio_limits(void) {
    /* 1536200 Hz at 48 kHz: 1536000/1536200 = 7680/7681, and the best ratio with terms up to 4096, 4095/4096, is about
       114 ppm under it. */
    dsd_rate_plan p;
    int rc = plan(DSD_RATE_PLAN_BACKEND_SOAPY, 1536000U, 1536200U, 1536200U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_ERR_RATIO && p.status == rc && p.nominal_hz == 1536200U && p.passes == 5
              && p.device_forced && p.step_eligible && step_off(&p) && p.step_out_hz == 0U,
          "Soapy 1536200: rc %d nominal %u, %d passes", rc, (unsigned)p.nominal_hz, p.passes);
    CHECK(message_names(&p, "1536200 Hz") && message_names(&p, "48000 Hz") && message_names(&p, "114 ppm")
              && message_names(&p, "DSP bandwidth") && plain_message(&p),
          "Soapy 1536200 message: %s", p.message);
    /* Read back out of tolerance to the same rate: the same refusal. */
    rc = plan(DSD_RATE_PLAN_BACKEND_SOAPY, 1536000U, 1536000U, 1536200U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_ERR_RATIO && p.nominal_hz == 1536200U && p.device_forced, "Soapy read back 1536200: %d",
          rc);

    /* 2000001 Hz at 48 kHz: 1536000/2000001 = 512000/666667, and 96/125 is 0.5 ppm over it, so the step runs 96/125
       and reports the target. */
    rc = plan(DSD_RATE_PLAN_BACKEND_SOAPY, 1536000U, 2000001U, 2000001U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && p.nominal_hz == 2000001U && p.passes == 5 && p.hb_out_num == 2000001
              && p.hb_out_shift == 5U && p.step.enabled && p.step.L == 96 && p.step.M == 125
              && fabs(p.step.approx_ppm - 0.5) <= 1e-6 && p.step_out_hz == 48000U,
          "Soapy 2000001: rc %d step %d/%d %.9f ppm (%s)", rc, p.step.L, p.step.M, p.step.approx_ppm, p.message);
}

/* A rate below the target is never upsampled live. */
static void
test_below_target(void) {
    dsd_rate_plan p;
    int rc = plan(DSD_RATE_PLAN_BACKEND_SOAPY, 1536000U, 40000U, 40000U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_ERR_BELOW_TARGET && p.nominal_hz == 40000U && step_off(&p) && p.step_out_hz == 0U
              && message_names(&p, "40000 Hz") && message_names(&p, "48000 Hz") && plain_message(&p),
          "40000 for 48000: rc %d (%s)", rc, p.message);
    /* A read-back out of tolerance that falls below it. */
    rc = plan(DSD_RATE_PLAN_BACKEND_AIRSPY, 48000U, 48000U, 47000U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_ERR_BELOW_TARGET && p.nominal_hz == 47000U && p.device_forced, "47000 for 48000: rc %d",
          rc);
    /* p = 0 is no guarantee: the rate itself is the target here, and one Hz less is refused. */
    rc = plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 48000U, 48000U, 48000U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && p.passes == 0 && p.step_out_hz == 48000U, "48000 for 48000: rc %d", rc);
    rc = plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 47999U, 47999U, 47999U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_ERR_BELOW_TARGET, "47999 for 48000: rc %d", rc);
}

/* An Airspy stays forced once its rate is programmed: the provisional plan, then its read-back, in and out of
   tolerance. */
static void
test_forced_across_readback(void) {
    dsd_rate_plan p;
    static const uint32_t kReadback[] = {0U, 2500000U, 2500100U};
    for (size_t i = 0; i < sizeof(kReadback) / sizeof(kReadback[0]); i++) {
        const int rc = plan(DSD_RATE_PLAN_BACKEND_AIRSPY, 1536000U, 2500000U, kReadback[i], 48000U, &p);
        CHECK(rc == DSD_RATE_PLAN_OK && p.device_forced && p.nominal_hz == 2500000U && p.step.enabled && p.step.L == 384
                  && p.step.M == 625 && p.step_out_hz == 48000U,
              "Airspy read back %u: rc %d forced %d nominal %u", (unsigned)kReadback[i], rc, p.device_forced,
              (unsigned)p.nominal_hz);
    }
    /* A rate already programmed and asked for again (request = programmed) is forced only by an out-of-tolerance
       read-back. */
    int rc = plan(DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000U, 2500000U, 2500000U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && !p.device_forced && p.step.enabled, "Airspy re-asked: rc %d forced %d", rc,
          p.device_forced);
    rc = plan(DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000U, 2500000U, 2500300U, 48000U, &p);
    CHECK(rc == DSD_RATE_PLAN_OK && p.device_forced && p.nominal_hz == 2500300U, "Airspy 2500300: rc %d forced %d", rc,
          p.device_forced);
}

/* The edges, the bounds of a plan that does not step, and the arguments refused. */
static void
test_edges_and_args(void) {
    int protect = -1;
    int stop = -1;
    dsd_rate_plan_step_edges(48000U, &protect, &stop);
    CHECK(protect == 21600 && stop == 26400, "48k edges %d/%d", protect, stop);
    dsd_rate_plan_step_edges(72000U, &protect, &stop);
    CHECK(protect == 32400 && stop == 39600, "72k edges %d/%d", protect, stop);
    dsd_rate_plan_step_edges(15625U, &protect, &stop);
    CHECK(protect == 7031 && stop == 8594, "15625 edges %d/%d", protect, stop);

    dsd_rate_plan p;
    (void)plan(DSD_RATE_PLAN_BACKEND_RTL_USB, 1536000U, 1536000U, 1536000U, 48000U, &p);
    dsd_rate_plan_bounds b;
    DSD_MEMSET(&b, 0xff, sizeof(b));
    CHECK(dsd_rate_plan_step_bounds(&p, &b) == DSD_RATE_PLAN_OK && b.taps_per_phase == 0 && b.tail_inputs == 0U
              && fabs(b.group_delay_inputs) <= 0.0 && fabs(b.group_delay_s) <= 0.0 && fabs(b.tail_s) <= 0.0,
          "no-step bounds");
    CHECK(dsd_rate_plan_step_bounds(NULL, &b) == DSD_RATE_PLAN_ERR_ARGS
              && dsd_rate_plan_step_bounds(&p, NULL) == DSD_RATE_PLAN_ERR_ARGS,
          "bounds NULL");

    dsd_rate_plan_input in = input_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 1536000U, 2500000U, 0U, 48000U);
    CHECK(dsd_rate_plan_compute(NULL, &p) == DSD_RATE_PLAN_ERR_ARGS
              && dsd_rate_plan_compute(&in, NULL) == DSD_RATE_PLAN_ERR_ARGS,
          "compute NULL");
    dsd_rate_plan_input bad[8];
    for (size_t i = 0; i < 8; i++) {
        bad[i] = in;
    }
    bad[0].target_hz = 0U;
    bad[1].programmed_hz = 0U;
    bad[2].request_hz = 0U;
    bad[3].max_passes = -1;
    bad[4].max_passes = DSD_RATE_PLAN_MAX_PASSES + 1;
    bad[5].backend = (dsd_rate_plan_backend)99;
    bad[6].programmed_hz = 0x80000000U;
    bad[7].readback_hz = 0x80000000U;
    for (size_t i = 0; i < 8; i++) {
        const int rc = dsd_rate_plan_compute(&bad[i], &p);
        CHECK(rc == DSD_RATE_PLAN_ERR_ARGS && p.status == rc && step_off(&p) && p.step_out_hz == 0U,
              "bad input %zu: rc %d", i, rc);
    }
    /* max_passes caps the chain: R2 10 MS/s at 4 kHz stops at 10 passes, one fewer stops at 9. */
    in = input_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 1024000U, 10000000U, 10000000U, 4000U);
    in.max_passes = 9;
    CHECK(dsd_rate_plan_compute(&in, &p) == DSD_RATE_PLAN_OK && p.passes == 9 && p.step.L == 128 && p.step.M == 625
              && p.step_out_hz == 4000U,
          "9 passes: %d, %d/%d", p.passes, p.step.L, p.step.M);
}

int
main(void) {
    test_grid_steps();
    test_grid_no_step();
    test_rtl_shaped();
    test_tolerance_first();
    test_rtl_readback_refused();
    test_ratio_limits();
    test_below_target();
    test_forced_across_readback();
    test_edges_and_args();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "IO_RATE_PLAN: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("IO_RATE_PLAN: OK\n");
    return 0;
}
