// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The live I/Q rate step in full_demod() (issue #806), driven through the pipeline itself: the half-band cascade, then
 * the step, with the channel filter off and the pass-through demodulator, so a block's result is the step's output.
 *
 * - Every live grid point the rate planner steps (Airspy R2 and Mini, SoapySDR 2 MS/s, 4..72 kHz) prepares, commits
 *   and makes the target rate.
 * - Zero-output and one-pair blocks, an empty block, and the buffer each block lands in.
 * - Maximum live blocks (MAXIMUM_BUF_LENGTH / 2 complex in) at p = 0 and p = 1 (131,072 and 65,536 step inputs, a
 *   SoapySDR device at 62.5 and 125 kS/s on a 48 kHz bandwidth) and at the Airspy passes, against the cascade's output
 *   through a resampler of the same design.
 * - Arbitrary cuts: the same output, bit for bit where no cascade runs, else within the cascade's own SIMD tolerance.
 * - dsd_demod_reset_filter_state() starts the step over with the cascade.
 * - The transaction: prepare leaves the running design alone, commit swaps and keeps the old design, rollback puts it
 *   back, finish releases it, a second commit keeps the first retained design, the active plan is reused, and a refused
 *   prepare (bad plan, capacity, a failed design) changes nothing. Run under asan for the lifetimes.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "live_iq_step_test_support.h"

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

constexpr int kMaxBlockPairs = MAXIMUM_BUF_LENGTH / 2;
/* The cascade's SIMD kernels agree with their whole-stream output within 1e-5 (docs/code_map.md, streaming front end);
   a cut moves the step's input that far, so its output moves no further than this, relative to the peak. */
constexpr double kCascadeTolOfPeak = 2e-5;

struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_live_iq_step_disable(&s->live_iq_step);
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};

/* The step plan the live wiring builds from the rate planner's output. */
dsd_live_iq_step_plan
step_plan_from(const dsd_rate_plan& rp) {
    dsd_live_iq_step_plan p;
    DSD_MEMSET(&p, 0, sizeof(p));
    p.enabled = rp.step.enabled;
    p.passes = rp.passes;
    p.hb_out_num = rp.hb_out_num;
    p.hb_out_shift = rp.hb_out_shift;
    p.L = rp.step.L;
    p.M = rp.step.M;
    p.target_hz = (int)rp.target_hz;
    p.protect_hz = rp.step.protect_hz;
    p.stop_hz = rp.step.stop_hz;
    return p;
}

/* The planner's final plan for a device that runs at @p device_hz, asked for the RTL-shaped rate. */
dsd_live_iq_step_plan
plan_for(dsd_rate_plan_backend backend, uint32_t device_hz, uint32_t target_hz) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = backend;
    in.request_hz = target_hz;
    in.programmed_hz = device_hz;
    in.readback_hz = device_hz;
    in.target_hz = target_hz;
    in.max_passes = 10;
    dsd_rate_plan rp;
    const int rc = dsd_rate_plan_compute(&in, &rp);
    CHECK(rc == DSD_RATE_PLAN_OK, "plan %u Hz -> %u Hz: %d %s", device_hz, target_hz, rc, rp.message);
    return step_plan_from(rp);
}

uint32_t g_rand = 0x5EEDC0DEu;

double
rand_unit(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return ((double)(g_rand >> 8) / 16777215.0) * 2.0 - 1.0;
}

/* A capture: a tone inside the protected band, one past the target's Nyquist (which the step must stop) and noise. */
std::vector<float>
make_capture(int pairs, double rate_hz, double target_hz, uint32_t seed) {
    const double two_pi = 6.28318530717958647692;
    std::vector<float> iq((size_t)pairs * 2U);
    g_rand = seed;
    for (int n = 0; n < pairs; n++) {
        const double a = two_pi * 0.21 * target_hz * (double)n / rate_hz;
        const double b = two_pi * 0.61 * target_hz * (double)n / rate_hz;
        iq[(size_t)n * 2U] = (float)(0.4 * std::cos(a) + 0.2 * std::cos(b) + 0.05 * rand_unit());
        iq[(size_t)n * 2U + 1U] = (float)(0.4 * std::sin(a) - 0.2 * std::sin(b) + 0.05 * rand_unit());
    }
    return iq;
}

/* The chain with the channel filter off and the pass-through demodulator: the result is the front end's output. */
void
configure_front_end(demod_state* s, int passes, int target_hz) {
    s->rate_in = target_hz;
    s->rate_out = target_hz;
    s->downsample_passes = passes;
    s->mode_demod = &raw_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_enable = 0;
    s->squelch_env = 1.0f;
}

int
install(demod_state* s, const dsd_live_iq_step_plan& plan) {
    int rc = dsd_demod_live_step_prepare(s, &plan);
    if (rc == DSD_LIVE_IQ_STEP_OK) {
        rc = dsd_live_iq_step_commit(&s->live_iq_step);
    }
    dsd_live_iq_step_finish(&s->live_iq_step);
    return rc;
}

/* One full_demod() block of @p pairs from @p in; appends the result to @p out and returns the complex outputs. */
int
run_block(demod_state* s, const float* in, int pairs, std::vector<float>* out) {
    if (pairs > 0) {
        DSD_MEMCPY(s->input_cb_buf, in, (size_t)pairs * 2U * sizeof(float));
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    if (out && s->result_len > 0) {
        out->insert(out->end(), s->result, s->result + s->result_len);
    }
    return s->result_len / 2;
}

/* The whole capture in blocks of the sizes given (cycled), the rest in maximum blocks. */
std::vector<float>
run_cuts(demod_state* s, const std::vector<float>& iq, const std::vector<int>& sizes) {
    std::vector<float> out;
    const int pairs = (int)(iq.size() / 2U);
    int pos = 0;
    size_t k = 0;
    while (pos < pairs) {
        int n = sizes.empty() ? kMaxBlockPairs : sizes[k % sizes.size()];
        k++;
        if (n > pairs - pos) {
            n = pairs - pos;
        }
        if (n > kMaxBlockPairs) {
            n = kMaxBlockPairs;
        }
        run_block(s, iq.data() + (size_t)pos * 2U, n, &out);
        pos += n;
    }
    return out;
}

double
peak_of(const std::vector<float>& v) {
    double peak = 0.0;
    for (float x : v) {
        peak = std::fmax(peak, std::fabs((double)x));
    }
    return peak;
}

uint32_t
float_bits(float x) {
    uint32_t u = 0U;
    DSD_MEMCPY(&u, &x, sizeof(u));
    return u;
}

/* Bit-for-bit equal (tol 0) or within tol of the reference's peak; counts always equal. */
int
same_output(const std::vector<float>& a, const std::vector<float>& ref, double tol_of_peak) {
    if (a.size() != ref.size()) {
        return 0;
    }
    const double tol = tol_of_peak * peak_of(ref);
    for (size_t i = 0; i < a.size(); i++) {
        const double d = std::fabs((double)a[i] - (double)ref[i]);
        if (tol_of_peak <= 0.0 ? float_bits(a[i]) != float_bits(ref[i]) : d > tol) {
            return 0;
        }
    }
    return 1;
}

/* The cascade's output for the capture, and that through a resampler of the plan's design: what the step must make.
   @p hb_pairs, when given, gets the cascade's output count. */
std::vector<float>
reference_output(const std::vector<float>& iq, const dsd_live_iq_step_plan& plan, const std::vector<int>& sizes,
                 size_t* hb_pairs = NULL) {
    DemodBox plain;
    if (!plain.s) {
        return std::vector<float>();
    }
    configure_front_end(plain.s, plan.passes, plan.target_hz);
    const std::vector<float> hb = run_cuts(plain.s, iq, sizes);
    if (hb_pairs) {
        *hb_pairs = hb.size() / 2U;
    }
    dsd_iq_resampler r;
    dsd_iq_resampler_init(&r);
    const int rc = dsd_iq_resampler_configure_ratio(&r, plan.L, plan.M, plan.hb_out_num, plan.hb_out_shift,
                                                    plan.protect_hz, plan.stop_hz, 4096);
    CHECK(rc == DSD_IQ_RESAMPLER_OK, "reference design %d", rc);
    const size_t in_pairs = hb.size() / 2U;
    std::vector<float> out((dsd_iq_resampler_max_out(&r, in_pairs) + 1U) * 2U);
    const int made = dsd_iq_resampler_process(&r, hb.data(), in_pairs, out.data(), out.size() / 2U);
    dsd_iq_resampler_free(&r);
    out.resize(made > 0 ? (size_t)made * 2U : 0U);
    return out;
}

/* Every live grid point the planner steps: prepare, commit, the target rate after the step. */
void
test_grid_points(void) {
    const uint32_t devices[] = {2500000u, 10000000u, 3000000u, 6000000u, 2000000u};
    const int targets[] = {4000, 6000, 8000, 12000, 16000, 24000, 48000, 72000};
    int stepped = 0;
    for (uint32_t dev : devices) {
        const dsd_rate_plan_backend backend =
            dev == 2000000u ? DSD_RATE_PLAN_BACKEND_SOAPY : DSD_RATE_PLAN_BACKEND_AIRSPY;
        for (int target : targets) {
            const dsd_live_iq_step_plan plan = plan_for(backend, dev, (uint32_t)target);
            DemodBox box;
            CHECK(box.s != NULL, "alloc");
            if (!box.s) {
                return;
            }
            const int rc = install(box.s, plan);
            CHECK(rc == DSD_LIVE_IQ_STEP_OK, "%u Hz -> %d Hz: install %d (design %d)", dev, target, rc,
                  box.s->live_iq_step.design_status);
            CHECK(box.s->live_iq_step.enabled == plan.enabled, "%u Hz -> %d Hz: enabled %d", dev, target,
                  box.s->live_iq_step.enabled);
            CHECK(dsd_live_iq_step_plan_out_hz(&box.s->live_iq_step.plan) == target, "%u Hz -> %d Hz: out %d", dev,
                  target, dsd_live_iq_step_plan_out_hz(&box.s->live_iq_step.plan));
            stepped += plan.enabled;
        }
    }
    /* 2.5 MS/s / 2^5 = 78125 Hz is not 72 kHz, and so on: every one of the 40 points steps. */
    CHECK(stepped == 40, "stepped %d of 40 grid points", stepped);
}

/* Blocks that make no output, one pair, and nothing at all; where each block lands. */
void
test_small_blocks(void) {
    /* p = 0: a SoapySDR device at 62.5 kS/s on 48 kHz steps 96/125 with no cascade, so each pair in reaches the
       step. */
    const dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_SOAPY, 62500u, 48000u);
    CHECK(plan.enabled && plan.passes == 0 && plan.L == 96 && plan.M == 125, "p0 plan %d %d %d/%d", plan.enabled,
          plan.passes, plan.L, plan.M);
    const std::vector<float> iq = make_capture(4000, 62500.0, 48000.0, 0x11u);
    DemodBox box;
    if (!box.s) {
        return;
    }
    configure_front_end(box.s, plan.passes, plan.target_hz);
    CHECK(install(box.s, plan) == DSD_LIVE_IQ_STEP_OK, "install");

    int zero_blocks = 0;
    int one_blocks = 0;
    std::vector<float> out;
    for (int i = 0; i < 4000; i++) {
        const int made = run_block(box.s, iq.data() + (size_t)i * 2U, 1, &out);
        CHECK(made == 0 || made == 1, "pair %d made %d", i, made);
        if (made == 0) {
            zero_blocks++;
            CHECK(box.s->front_end_empty == 1 && box.s->result_len == 0, "pair %d: empty %d result %d", i,
                  box.s->front_end_empty, box.s->result_len);
        } else {
            one_blocks++;
            CHECK(box.s->front_end_empty == 0 && box.s->result_len == 2, "pair %d: empty %d result %d", i,
                  box.s->front_end_empty, box.s->result_len);
            CHECK(box.s->lowpassed == box.s->hb_workbuf, "pair %d: the step wrote the input's buffer", i);
        }
        if (i == 1000) {
            /* An empty block makes nothing and moves nothing: the stream goes on as if it were not there. */
            const int phase = box.s->live_iq_step.active.phase;
            CHECK(run_block(box.s, NULL, 0, &out) == 0 && box.s->front_end_empty == 1, "empty block");
            CHECK(box.s->live_iq_step.active.phase == phase, "an empty block moved the phase");
        }
    }
    /* 4000 x 96 / 125 = 3072 outputs: 928 inputs make none. */
    CHECK(zero_blocks == 928 && one_blocks == 3072, "zero-output blocks %d, one-pair blocks %d", zero_blocks,
          one_blocks);
    const std::vector<float> ref = reference_output(iq, plan, std::vector<int>());
    CHECK(same_output(out, ref, 0.0), "1-pair blocks differ from the whole stream (%zu vs %zu)", out.size(),
          ref.size());
}

struct MaxCase {
    const char* name;
    dsd_rate_plan_backend backend;
    uint32_t device_hz;
    uint32_t target_hz;
    int passes;
    int step_in; /* complex inputs a maximum block brings the step */
};

/* Maximum live blocks, against the cascade's output through the same design, and the buffer they land in. */
void
test_max_blocks(void) {
    const MaxCase cases[] = {
        {"soapy 62.5k p0", DSD_RATE_PLAN_BACKEND_SOAPY, 62500u, 48000u, 0, 131072},
        {"soapy 125k p1", DSD_RATE_PLAN_BACKEND_SOAPY, 125000u, 48000u, 1, 65536},
        {"airspy 2.5M 48k", DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000u, 48000u, 5, 4096},
        {"airspy 3M 72k", DSD_RATE_PLAN_BACKEND_AIRSPY, 3000000u, 72000u, 5, 4096},
        {"airspy 2.5M 4k", DSD_RATE_PLAN_BACKEND_AIRSPY, 2500000u, 4000u, 9, 256},
        {"airspy 10M 4k", DSD_RATE_PLAN_BACKEND_AIRSPY, 10000000u, 4000u, 10, 128},
    };
    for (const MaxCase& c : cases) {
        const dsd_live_iq_step_plan plan = plan_for(c.backend, c.device_hz, c.target_hz);
        CHECK(plan.enabled && plan.passes == c.passes, "%s: plan enabled %d passes %d", c.name, plan.enabled,
              plan.passes);
        const std::vector<float> iq = make_capture(3 * kMaxBlockPairs, (double)c.device_hz, (double)c.target_hz, 0x22u);
        DemodBox box;
        if (!box.s) {
            return;
        }
        configure_front_end(box.s, plan.passes, plan.target_hz);
        CHECK(install(box.s, plan) == DSD_LIVE_IQ_STEP_OK, "%s: install", c.name);
        std::vector<float> out;
        for (int b = 0; b < 3; b++) {
            const size_t before = out.size();
            run_block(box.s, iq.data() + (size_t)b * kMaxBlockPairs * 2U, kMaxBlockPairs, &out);
            const size_t made = (out.size() - before) / 2U;
            /* The step's output never passes its input, and its input is the cascade's (N + 1) / 2 per pass. */
            CHECK(made <= (size_t)c.step_in, "%s block %d: %zu outputs from %d step inputs", c.name, b, made,
                  c.step_in);
            const float* expect = (c.passes & 1) ? box.s->timing_buf : box.s->hb_workbuf;
            CHECK(box.s->lowpassed == expect, "%s block %d: landed in the wrong work buffer", c.name, b);
        }
        size_t hb_pairs = 0;
        const std::vector<float> ref = reference_output(iq, plan, std::vector<int>(), &hb_pairs);
        CHECK(same_output(out, ref, 0.0), "%s: max blocks differ from the reference (%zu vs %zu)", c.name, out.size(),
              ref.size());
        /* From a cleared phase, n inputs make ceil(n x L / M) outputs. */
        const size_t expect_out = (hb_pairs * (size_t)plan.L + (size_t)plan.M - 1U) / (size_t)plan.M;
        CHECK(out.size() / 2U == expect_out, "%s: %zu outputs from %zu step inputs, %zu expected", c.name,
              out.size() / 2U, hb_pairs, expect_out);
    }
}

/* Arbitrary cuts give the same output as maximum blocks. */
void
test_cuts(void) {
    struct CutCase {
        const char* name;
        dsd_rate_plan_backend backend;
        uint32_t device_hz;
        uint32_t target_hz;
    };

    const CutCase cases[] = {
        {"soapy 62.5k p0", DSD_RATE_PLAN_BACKEND_SOAPY, 62500u, 48000u},
        {"312.5k 48k p2", DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 48000u},
        {"375k 24k p3", DSD_RATE_PLAN_BACKEND_AIRSPY, 375000u, 24000u},
    };
    const std::vector<std::vector<int>> cuts = {
        {1, 2, 3, 0, 5, 7, 11, 1},
        {511, 512, 513, 1, 1024, 3},
        {4999, 17, 2048, 0, 333},
    };
    for (const CutCase& c : cases) {
        const dsd_live_iq_step_plan plan = plan_for(c.backend, c.device_hz, c.target_hz);
        const std::vector<float> iq = make_capture(60000, (double)c.device_hz, (double)c.target_hz, 0x33u);
        DemodBox whole;
        if (!whole.s) {
            return;
        }
        configure_front_end(whole.s, plan.passes, plan.target_hz);
        CHECK(install(whole.s, plan) == DSD_LIVE_IQ_STEP_OK, "%s: install", c.name);
        const std::vector<float> ref = run_cuts(whole.s, iq, std::vector<int>());
        CHECK(!ref.empty(), "%s: no output", c.name);
        /* With no cascade the step's input is the capture itself, cut or not, and the step is exact across cuts. */
        const double tol = plan.passes == 0 ? 0.0 : kCascadeTolOfPeak;
        for (size_t k = 0; k < cuts.size(); k++) {
            DemodBox cut;
            if (!cut.s) {
                return;
            }
            configure_front_end(cut.s, plan.passes, plan.target_hz);
            CHECK(install(cut.s, plan) == DSD_LIVE_IQ_STEP_OK, "%s: install", c.name);
            const std::vector<float> got = run_cuts(cut.s, iq, cuts[k]);
            CHECK(same_output(got, ref, tol), "%s: cut %zu gives %zu floats, whole stream %zu", c.name, k, got.size(),
                  ref.size());
        }
    }
}

/* The energy in the active design's history: 0 once it is cleared. */
double
step_history_energy(const demod_state* s) {
    const dsd_iq_resampler* r = &s->live_iq_step.active;
    double e = 0.0;
    for (size_t i = 0; i < ((size_t)r->taps_per_phase - 1U) * 2U; i++) {
        e += (double)r->work[i] * (double)r->work[i];
    }
    return e;
}

/* dsd_demod_reset_filter_state() starts the step over with the cascade: what follows equals a fresh stream. */
void
test_reset(void) {
    const dsd_live_iq_step_plan plan = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 48000u);
    const std::vector<float> a = make_capture(20000, 312500.0, 48000.0, 0x44u);
    const std::vector<float> b = make_capture(20000, 312500.0, 48000.0, 0x55u);
    const std::vector<int> sizes = {777};
    DemodBox used;
    DemodBox fresh;
    if (!used.s || !fresh.s) {
        return;
    }
    configure_front_end(used.s, plan.passes, plan.target_hz);
    configure_front_end(fresh.s, plan.passes, plan.target_hz);
    CHECK(install(used.s, plan) == DSD_LIVE_IQ_STEP_OK && install(fresh.s, plan) == DSD_LIVE_IQ_STEP_OK, "install");
    (void)run_cuts(used.s, a, sizes);
    dsd_demod_reset_filter_state(used.s);
    CHECK(used.s->live_iq_step.active.phase == 0 && used.s->live_iq_step.enabled == 1, "reset: phase %d enabled %d",
          used.s->live_iq_step.active.phase, used.s->live_iq_step.enabled);
    const std::vector<float> got = run_cuts(used.s, b, sizes);
    const std::vector<float> ref = run_cuts(fresh.s, b, sizes);
    CHECK(same_output(got, ref, 0.0), "after a reset the step's output is not a fresh stream's");

    /* The cascade starts over by itself when its pass count changes without a reset, and the step with it. */
    CHECK(step_history_energy(used.s) > 0.0, "the step held no history to clear");
    used.s->downsample_passes = plan.passes + 1;
    run_block(used.s, NULL, 0, NULL);
    CHECK(step_history_energy(used.s) <= 0.0 && used.s->live_iq_step.active.phase == 0,
          "a pass-count change kept the step's history");
}

/* A stream through @p s for @p blocks blocks of 1000 device pairs, from @p offset in the capture. */
std::vector<float>
stream_blocks(demod_state* s, const std::vector<float>& iq, int offset, int blocks) {
    std::vector<float> out;
    for (int k = 0; k < blocks; k++) {
        run_block(s, iq.data() + (size_t)(offset + k * 1000) * 2U, 1000, &out);
    }
    return out;
}

/* Prepare leaves the running design alone; commit swaps it and keeps the old one; rollback puts it back; finish
   releases it. */
void
test_transaction_commit_rollback(void) {
    const dsd_live_iq_step_plan a = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 48000u); /* 78125 -> 48k */
    const dsd_live_iq_step_plan b = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 72000u); /* 78125 -> 72k */
    const std::vector<float> iq = make_capture(40000, 312500.0, 48000.0, 0x66u);
    DemodBox box;
    DemodBox twin;
    DemodBox fresh_b;
    DemodBox fresh_a;
    if (!box.s || !twin.s || !fresh_b.s || !fresh_a.s) {
        return;
    }
    configure_front_end(box.s, a.passes, a.target_hz);
    configure_front_end(twin.s, a.passes, a.target_hz);
    configure_front_end(fresh_b.s, b.passes, b.target_hz);
    configure_front_end(fresh_a.s, a.passes, a.target_hz);
    dsd_live_iq_step* st = &box.s->live_iq_step;

    /* Open: nothing runs until the commit. */
    CHECK(dsd_demod_live_step_prepare(box.s, &a) == DSD_LIVE_IQ_STEP_OK, "prepare a");
    CHECK(st->enabled == 0 && st->candidate_set == 1 && st->candidate.configured == 1, "prepared a: enabled %d",
          st->enabled);
    CHECK(dsd_live_iq_step_commit(st) == DSD_LIVE_IQ_STEP_OK, "commit a");
    CHECK(st->enabled == 1 && st->retained_set == 1 && st->retained.configured == 0, "committed a over nothing");
    dsd_live_iq_step_finish(st);
    CHECK(st->retained_set == 0 && st->candidate_set == 0, "finish");
    CHECK(install(twin.s, a) == DSD_LIVE_IQ_STEP_OK && install(fresh_b.s, b) == DSD_LIVE_IQ_STEP_OK
              && install(fresh_a.s, a) == DSD_LIVE_IQ_STEP_OK,
          "install twins");
    (void)stream_blocks(box.s, iq, 0, 10);
    (void)stream_blocks(twin.s, iq, 0, 10);

    /* A prepared candidate does not touch the running stream. */
    const float* a_bank = st->active.bank;
    CHECK(dsd_demod_live_step_prepare(box.s, &b) == DSD_LIVE_IQ_STEP_OK, "prepare b");
    CHECK(st->active.bank == a_bank && st->enabled == 1 && dsd_live_iq_step_plan_equal(&st->plan, &a),
          "prepare b moved the active design");
    CHECK(same_output(stream_blocks(box.s, iq, 10000, 5), stream_blocks(twin.s, iq, 10000, 5), 0.0),
          "the stream changed while b was only prepared");

    /* Commit: b runs from silence, a is retained. */
    CHECK(dsd_live_iq_step_commit(st) == DSD_LIVE_IQ_STEP_OK, "commit b");
    CHECK(st->retained_set == 1 && st->retained.bank == a_bank && dsd_live_iq_step_plan_equal(&st->retained_plan, &a),
          "a not retained");
    CHECK(dsd_live_iq_step_plan_equal(&st->plan, &b) && st->candidate_set == 0 && st->candidate.configured == 0,
          "b not active");
    dsd_demod_reset_filter_state(box.s); /* a retune resets the cascade with it */
    box.s->rate_in = box.s->rate_out = b.target_hz;
    CHECK(same_output(stream_blocks(box.s, iq, 15000, 5), stream_blocks(fresh_b.s, iq, 15000, 5), 0.0),
          "b after the commit is not a fresh b");

    /* Rollback: a runs again, cleared; b is released. */
    dsd_live_iq_step_rollback(st);
    CHECK(st->active.bank == a_bank && dsd_live_iq_step_plan_equal(&st->plan, &a) && st->enabled == 1,
          "rollback did not put a back");
    CHECK(st->retained_set == 0 && st->retained.configured == 0 && st->candidate.configured == 0,
          "rollback kept a design");
    CHECK(st->active.phase == 0, "rollback did not clear a");
    dsd_demod_reset_filter_state(box.s);
    box.s->rate_in = box.s->rate_out = a.target_hz;
    CHECK(same_output(stream_blocks(box.s, iq, 20000, 5), stream_blocks(fresh_a.s, iq, 20000, 5), 0.0),
          "a after the rollback is not a fresh a");
    dsd_live_iq_step_finish(st);
    CHECK(st->active.bank == a_bank && st->enabled == 1, "finish after the rollback moved a");
}

/* The active plan is reused, a second commit keeps the first retained design, a plan with no step turns it off, and
   disable releases everything. */
void
test_transaction_reuse_and_off(void) {
    const dsd_live_iq_step_plan a = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 48000u);
    const dsd_live_iq_step_plan b = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 72000u);
    const dsd_live_iq_step_plan c = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 375000u, 72000u);
    /* RTL-shaped 1.536 MS/s at 48 kHz: five passes, no step. */
    const dsd_live_iq_step_plan off = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 1536000u, 48000u);
    CHECK(!off.enabled && off.passes == 5 && dsd_live_iq_step_plan_out_hz(&off) == 48000, "off plan");
    const std::vector<float> iq = make_capture(40000, 312500.0, 48000.0, 0x77u);
    DemodBox box;
    DemodBox twin;
    if (!box.s || !twin.s) {
        return;
    }
    configure_front_end(box.s, a.passes, a.target_hz);
    configure_front_end(twin.s, a.passes, a.target_hz);
    CHECK(install(box.s, a) == DSD_LIVE_IQ_STEP_OK && install(twin.s, a) == DSD_LIVE_IQ_STEP_OK, "install a");
    dsd_live_iq_step* st = &box.s->live_iq_step;
    (void)stream_blocks(box.s, iq, 0, 7);
    (void)stream_blocks(twin.s, iq, 0, 7);

    /* The active plan again: nothing designed, nothing cleared, the stream runs on. */
    const float* a_bank = st->active.bank;
    CHECK(dsd_demod_live_step_prepare(box.s, &a) == DSD_LIVE_IQ_STEP_OK, "prepare a again");
    CHECK(st->candidate_reuses_active == 1 && st->candidate.configured == 0, "a again was designed");
    CHECK(dsd_live_iq_step_commit(st) == DSD_LIVE_IQ_STEP_OK && st->active.bank == a_bank && st->retained_set == 0,
          "commit of the active plan changed the design");
    dsd_live_iq_step_finish(st);
    CHECK(same_output(stream_blocks(box.s, iq, 7000, 5), stream_blocks(twin.s, iq, 7000, 5), 0.0),
          "reusing the active plan broke the stream");

    /* Two commits in one transaction: the retained design is still the one it began with. */
    CHECK(dsd_demod_live_step_prepare(box.s, &b) == DSD_LIVE_IQ_STEP_OK && dsd_live_iq_step_commit(st) == 0, "b");
    CHECK(dsd_demod_live_step_prepare(box.s, &c) == DSD_LIVE_IQ_STEP_OK && dsd_live_iq_step_commit(st) == 0, "c");
    CHECK(dsd_live_iq_step_plan_equal(&st->plan, &c) && st->retained.bank == a_bank
              && dsd_live_iq_step_plan_equal(&st->retained_plan, &a),
          "a second commit lost the retained design");
    dsd_live_iq_step_rollback(st);
    CHECK(st->active.bank == a_bank && dsd_live_iq_step_plan_equal(&st->plan, &a), "rollback after two commits");
    dsd_live_iq_step_finish(st);

    /* A plan with no step: the cascade's output passes untouched, and a rollback brings the step back. */
    CHECK(dsd_demod_live_step_prepare(box.s, &off) == DSD_LIVE_IQ_STEP_OK, "prepare off");
    CHECK(st->candidate.configured == 0 && dsd_live_iq_step_commit(st) == DSD_LIVE_IQ_STEP_OK && st->enabled == 0,
          "off did not turn the step off");
    box.s->downsample_passes = off.passes;
    const std::vector<float> rtl = make_capture(8192, 1536000.0, 48000.0, 0x88u);
    DemodBox plain;
    if (!plain.s) {
        return;
    }
    configure_front_end(plain.s, off.passes, off.target_hz);
    dsd_demod_reset_filter_state(box.s);
    CHECK(same_output(run_cuts(box.s, rtl, std::vector<int>()), run_cuts(plain.s, rtl, std::vector<int>()), 0.0),
          "a chain with the step off is not the cascade alone");
    dsd_live_iq_step_rollback(st);
    CHECK(st->enabled == 1 && st->active.bank == a_bank, "rollback from off");
    dsd_live_iq_step_finish(st);

    dsd_live_iq_step_disable(st);
    CHECK(st->enabled == 0 && st->active.configured == 0 && st->retained.configured == 0
              && st->candidate.configured == 0 && st->retained_set == 0 && st->candidate_set == 0,
          "disable kept something");
}

/* A refused prepare changes nothing: a plan that disagrees with itself, one that does not fit, a failed design. */
void
test_transaction_refusals(void) {
    const dsd_live_iq_step_plan a = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 48000u);
    const dsd_live_iq_step_plan b = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 312500u, 72000u);
    const dsd_live_iq_step_plan c = plan_for(DSD_RATE_PLAN_BACKEND_AIRSPY, 375000u, 72000u);
    DemodBox box;
    if (!box.s) {
        return;
    }
    configure_front_end(box.s, a.passes, a.target_hz);
    CHECK(install(box.s, a) == DSD_LIVE_IQ_STEP_OK, "install a");
    dsd_live_iq_step* st = &box.s->live_iq_step;
    const float* a_bank = st->active.bank;
    CHECK(dsd_demod_live_step_prepare(box.s, &b) == DSD_LIVE_IQ_STEP_OK && dsd_live_iq_step_commit(st) == 0, "b");
    const float* b_bank = st->active.bank;

    dsd_live_iq_step_plan bad = c;
    bad.M = c.M + 1; /* no longer takes 93750 Hz to 72 kHz */
    dsd_live_iq_step_plan up = c;
    up.hb_out_num = 48000;
    up.hb_out_shift = 0;
    up.passes = 0;
    up.L = 3;
    up.M = 2; /* 48 kHz up to 72 kHz: the live chain never upsamples */
    dsd_live_iq_step_plan bad_passes = c;
    bad_passes.passes = -1;
    dsd_live_iq_step_plan bad_band = c;
    bad_band.stop_hz = bad_band.protect_hz; /* no transition band */

    struct Refusal {
        const char* name;
        const dsd_live_iq_step_plan* plan;
        int fail_design;
        int expect;
        int design_status;
    };

    const Refusal refusals[] = {
        {"L/M disagrees with the rates", &bad, 0, DSD_LIVE_IQ_STEP_ERR_ARGS, DSD_IQ_RESAMPLER_OK},
        {"upsampling", &up, 0, DSD_LIVE_IQ_STEP_ERR_CAPACITY, DSD_IQ_RESAMPLER_OK},
        {"negative passes", &bad_passes, 0, DSD_LIVE_IQ_STEP_ERR_ARGS, DSD_IQ_RESAMPLER_OK},
        {"no transition band", &bad_band, 0, DSD_LIVE_IQ_STEP_ERR_DESIGN, DSD_IQ_RESAMPLER_ERR_BAND},
        {"allocation failure", &c, 1, DSD_LIVE_IQ_STEP_ERR_DESIGN, DSD_IQ_RESAMPLER_ERR_ALLOC},
        {"NULL plan", NULL, 0, DSD_LIVE_IQ_STEP_ERR_ARGS, DSD_IQ_RESAMPLER_OK},
    };
    for (const Refusal& r : refusals) {
        /* A candidate already prepared goes too: a refusal leaves none. */
        CHECK(dsd_demod_live_step_prepare(box.s, &c) == DSD_LIVE_IQ_STEP_OK, "%s: prepare c first", r.name);
        dsd_live_iq_step_test_fail_design(r.fail_design);
        const int rc = dsd_demod_live_step_prepare(box.s, r.plan);
        dsd_live_iq_step_test_fail_design(0);
        CHECK(rc == r.expect, "%s: %d, expected %d", r.name, rc, r.expect);
        if (r.plan) {
            CHECK(st->design_status == r.design_status, "%s: design status %d", r.name, st->design_status);
        }
        CHECK(st->candidate_set == 0 && st->candidate.configured == 0, "%s: a candidate survived", r.name);
        CHECK(st->active.bank == b_bank && st->enabled == 1 && dsd_live_iq_step_plan_equal(&st->plan, &b),
              "%s: the active design moved", r.name);
        CHECK(st->retained_set == 1 && st->retained.bank == a_bank
                  && dsd_live_iq_step_plan_equal(&st->retained_plan, &a),
              "%s: the retained design moved", r.name);
        CHECK(dsd_live_iq_step_commit(st) == DSD_LIVE_IQ_STEP_ERR_ARGS && st->active.bank == b_bank,
              "%s: a commit with nothing prepared changed something", r.name);
    }

    /* The generic entry: a buffer too small for a maximum block. */
    dsd_live_iq_step loose;
    dsd_live_iq_step_init(&loose);
    CHECK(dsd_live_iq_step_prepare(&loose, &c, 4096, 3000) == DSD_LIVE_IQ_STEP_ERR_CAPACITY, "small buffer");
    CHECK(dsd_live_iq_step_prepare(&loose, &c, 4096, 4096) == DSD_LIVE_IQ_STEP_OK, "room enough");
    CHECK(dsd_live_iq_step_prepare(&loose, &c, 0, 4096) == DSD_LIVE_IQ_STEP_ERR_ARGS && loose.candidate_set == 0,
          "a block of nothing");
    CHECK(dsd_live_iq_step_prepare(NULL, &c, 4096, 4096) == DSD_LIVE_IQ_STEP_ERR_ARGS, "NULL step");
    dsd_live_iq_step_disable(&loose);

    /* A design that failed once designs the next time. */
    CHECK(dsd_demod_live_step_prepare(box.s, &c) == DSD_LIVE_IQ_STEP_OK && dsd_live_iq_step_commit(st) == 0,
          "c after the refusals");
    CHECK(dsd_live_iq_step_plan_equal(&st->plan, &c) && st->retained.bank == a_bank, "c active, a retained");
    dsd_live_iq_step_finish(st);
}

} // namespace

int
main(void) {
    test_grid_points();
    test_small_blocks();
    test_max_blocks();
    test_cuts();
    test_reset();
    test_transaction_commit_rollback();
    test_transaction_reuse_and_off();
    test_transaction_refusals();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("DSP_LIVE_IQ_STEP: all passed\n");
    return 0;
}
