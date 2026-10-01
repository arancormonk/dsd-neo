// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The channel FIR's output does not depend on how its input is cut into blocks (issue #572).
 *
 * simd_fir_complex_apply() holds back the outputs whose look-ahead has not arrived yet (`pending`) and makes them on a
 * later call, so a stream gives the same outputs whatever its blocks. Each backend and the dispatcher are checked, for
 * many filters and block splits, against the documented output counts, a double-precision centred convolution with
 * zero left padding, and the backend's own output for the whole stream in one call. Tap changes without a clear,
 * invalid calls, short output capacities and exact-size buffers are covered too.
 *
 * The pipeline side: dsd_demod_reset_filter_state() returns the filter state to a fresh one; the channel state is kept
 * across a profile change at the same rate and cleared by a rate change and by a block the filter does not run on;
 * and full_demod() makes no per-block decision on a block its front end left empty.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/simd_fir.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "dsp/simd_fir_internal.h"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
#include "dsp/simd_x86_cpu.h"
#endif
#endif

namespace {

using FirFn = int (*)(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q, int hist_len,
                      int* pending, const float* taps, int taps_len);

struct Backend {
    const char* name;
    FirFn fn;
    float split_tol;  /* split vs whole: 0 for scalar (bit-exact), 1e-5 for the SIMD kernels and the dispatcher */
    int vector_width; /* complex outputs per vector step */
};

} // namespace

/* ---------------- The functions under test ---------------- */

/* Every binding to the code under test sits in this block: the backends, the demodulator's pending count and its
 * filter reset. */

#if defined(__x86_64__) || defined(_M_X64)
extern "C" int simd_fir_complex_apply_sse2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
extern "C" int simd_fir_complex_apply_avx2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
#endif
#endif
#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" int simd_fir_complex_apply_neon(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
#endif

static std::vector<Backend>
fir_backends(void) {
    std::vector<Backend> backends;
    backends.push_back(Backend{"scalar", simd_fir_complex_apply_scalar, 0.0f, 1});
#if defined(__x86_64__) || defined(_M_X64)
    backends.push_back(Backend{"sse2", simd_fir_complex_apply_sse2, 1e-5f, 2});
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        backends.push_back(Backend{"avx2", simd_fir_complex_apply_avx2, 1e-5f, 8});
    } else {
        std::printf("Skipping the AVX2 backend: CPU/OS AVX2+FMA support unavailable\n");
    }
#endif
#endif
#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    backends.push_back(Backend{"neon", simd_fir_complex_apply_neon, 1e-5f, 2});
#endif
    backends.push_back(Backend{"dispatch", simd_fir_complex_apply, 1e-5f, 8});
    return backends;
}

static int*
channel_pending_of(demod_state* d) {
    return &d->channel_lpf_pending;
}

static void
reset_filter_state(demod_state* d) {
    dsd_demod_reset_filter_state(d);
}

/* ---------------- Helpers ---------------- */

static const int kH = DSD_CHANNEL_LPF_HIST_LEN; /* the history every call here runs with: the pipeline's */
static const int kStreamLen = 9000;             /* complex samples */
static const double kRefTol = 2e-5;             /* against the double-precision reference */

static uint32_t g_rand_state = 0x5EEDF17Bu;

static uint32_t
rand_u32(void) {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

static float
randf(void) {
    const float unit = (float)(rand_u32() >> 8) * (1.0f / 16777215.0f);
    return (unit * 2.0f) - 1.0f;
}

static std::vector<float>
make_stream(int complex_samples, uint32_t seed) {
    g_rand_state = seed;
    std::vector<float> x((size_t)complex_samples * 2U);
    for (float& v : x) {
        v = randf();
    }
    return x;
}

/* Out of tolerance, NaN included. */
static int
off_by_more(double got, double want, double tol) {
    return !(std::fabs(got - want) <= tol);
}

/* Centred convolution in double with zero left padding: complex output j is sum_k taps[k] * x[j + k - c]. Only the
 * outputs whose look-ahead the stream holds (j + c < n), interleaved. */
static std::vector<double>
reference_of(const std::vector<float>& x, const std::vector<float>& taps) {
    const int n = (int)(x.size() / 2U);
    const int len = (int)taps.size();
    const int c = (len - 1) / 2;
    const int outputs = n > c ? n - c : 0;
    std::vector<double> ref((size_t)outputs * 2U, 0.0);
    for (int j = 0; j < outputs; j++) {
        double acc_i = 0.0;
        double acc_q = 0.0;
        for (int k = 0; k < len; k++) {
            const int idx = j + k - c;
            if (idx < 0) {
                continue;
            }
            acc_i += (double)taps[(size_t)k] * (double)x[(size_t)idx * 2U];
            acc_q += (double)taps[(size_t)k] * (double)x[(size_t)idx * 2U + 1U];
        }
        ref[(size_t)j * 2U] = acc_i;
        ref[(size_t)j * 2U + 1U] = acc_q;
    }
    return ref;
}

/* The documented count: outputs = max(0, pending + N - c). */
static int
formula_outputs(int pending, int n_in, int taps_len) {
    const int c = (taps_len - 1) / 2;
    const int lt = pending + n_in - c;
    return lt > 0 ? lt : 0;
}

namespace {

struct FirState {
    std::vector<float> hist_i;
    std::vector<float> hist_q;
    int pending = 0;

    FirState() : hist_i((size_t)kH, 0.0f), hist_q((size_t)kH, 0.0f) {}
};

/* Everything a run of a stream through one backend gave. */
struct Run {
    std::vector<float> out;     /* every output, in order, interleaved */
    std::vector<int> lookahead; /* per output: the stream samples after its centre the call that made it had */
    std::vector<int> emitter;   /* per output: the index of the tap set it was made with */
    int produced = 0;
    int consumed = 0;
    int calls = 0;
    int count_mismatches = 0;
    char first_count_mismatch[200] = {0};
    int invalid_calls = 0;
    FirState state;
};

/* One block size after another: the list cycled, or the list once and then the rest of the stream in one call. */
struct Split {
    const char* name;
    std::vector<int> sizes;
    int cycle;
};

} // namespace

/* Feed @p n_in samples of @p x from the run's position through @p fn with @p taps (tap set @p emitter), checking the
 * count and pending against the formula. */
static void
run_call(FirFn fn, const std::vector<float>& x, int n_in, const std::vector<float>& taps, int emitter, Run* run) {
    const int len = (int)taps.size();
    const int before = run->state.pending;
    const int want = formula_outputs(before, n_in, len);
    const size_t room = run->out.size() / 2U - (size_t)run->produced;
    const int got =
        fn(x.data() + (size_t)run->consumed * 2U, n_in, run->out.data() + (size_t)run->produced * 2U, (int)room,
           run->state.hist_i.data(), run->state.hist_q.data(), kH, &run->state.pending, taps.data(), len);
    run->calls++;
    if (got < 0) {
        run->invalid_calls++;
    }
    if (got != want || run->state.pending != before + n_in - want) {
        if (run->count_mismatches++ == 0) {
            DSD_SNPRINTF(run->first_count_mismatch, sizeof(run->first_count_mismatch),
                         "call %d (N=%d, pending %d, %d taps): %d outputs, pending %d; want %d outputs, pending %d",
                         run->calls, n_in, before, len, got, run->state.pending, want, before + n_in - want);
        }
    }
    run->consumed += n_in;
    for (int k = 0; k < got; k++) {
        run->lookahead.push_back(run->consumed - 1 - (run->produced + k));
        run->emitter.push_back(emitter);
    }
    if (got > 0) {
        run->produced += got;
    }
}

static void
run_split(FirFn fn, const std::vector<float>& x, const std::vector<float>& taps, const Split& split, Run* run) {
    const int n = (int)(x.size() / 2U);
    run->out.assign((size_t)(n + kH) * 2U, 0.0f);
    size_t i = 0U;
    while (run->consumed < n) {
        int size = 0;
        if (split.cycle) {
            size = split.sizes[i % split.sizes.size()];
        } else {
            size = i < split.sizes.size() ? split.sizes[i] : n;
        }
        i++;
        if (size > n - run->consumed) {
            size = n - run->consumed;
        }
        run_call(fn, x, size, taps, 0, run);
        if (run->invalid_calls > 0) {
            return;
        }
    }
}

/* Each output against the reference of the taps it was made with. */
static int
check_against_reference(const char* what, const Run& run, const std::vector<const std::vector<double>*>& refs) {
    double worst = 0.0;
    int worst_at = -1;
    int bad = 0;
    for (int j = 0; j < run.produced; j++) {
        const std::vector<double>& ref = *refs[(size_t)run.emitter[(size_t)j]];
        if ((size_t)j * 2U + 1U >= ref.size()) {
            continue; /* past the outputs the stream can make (a backend that pads its look-ahead) */
        }
        for (int part = 0; part < 2; part++) {
            const double got = (double)run.out[(size_t)j * 2U + (size_t)part];
            const double want = ref[(size_t)j * 2U + (size_t)part];
            const double err = std::isnan(got) ? INFINITY : std::fabs(got - want);
            if (off_by_more(got, want, kRefTol)) {
                bad++;
            }
            if (err > worst) {
                worst = err;
                worst_at = j;
            }
        }
    }
    if (bad == 0) {
        return 0;
    }
    DSD_FPRINTF(stderr,
                "  FAIL: %s: %d values off the reference by more than %.0e; worst %.3e at output %d, made with %d "
                "samples of look-ahead\n",
                what, bad, kRefTol, worst, worst_at, run.lookahead[(size_t)worst_at]);
    return 1;
}

static int
check_counts(const char* what, const Run& run) {
    int rc = 0;
    if (run.count_mismatches != 0) {
        DSD_FPRINTF(stderr, "  FAIL: %s: %d of %d calls off the count formula; first: %s\n", what, run.count_mismatches,
                    run.calls, run.first_count_mismatch);
        rc = 1;
    }
    if (run.invalid_calls != 0) {
        DSD_FPRINTF(stderr, "  FAIL: %s: %d calls refused\n", what, run.invalid_calls);
        rc = 1;
    }
    return rc;
}

static int
check_split_matches_whole(const char* what, const Run& run, const Run& whole, float tol) {
    if (run.produced != whole.produced) {
        DSD_FPRINTF(stderr, "  FAIL: %s: %d outputs, the whole stream in one call made %d\n", what, run.produced,
                    whole.produced);
        return 1;
    }
    for (int k = 0; k < run.produced * 2; k++) {
        if (off_by_more((double)run.out[(size_t)k], (double)whole.out[(size_t)k], (double)tol)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: output %d is %.9g, the whole stream in one call made %.9g (tol %.0e)\n",
                        what, k / 2, (double)run.out[(size_t)k], (double)whole.out[(size_t)k], (double)tol);
            return 1;
        }
    }
    return 0;
}

/* After the stream: pending is what the formula leaves, and the history holds the stream's newest kH samples,
 * right-aligned and zero-padded on the left. */
static int
check_final_state(const char* what, const Run& run, const std::vector<float>& x, int want_pending) {
    int rc = 0;
    if (run.state.pending != want_pending) {
        DSD_FPRINTF(stderr, "  FAIL: %s: final pending %d, want %d\n", what, run.state.pending, want_pending);
        rc = 1;
    }
    const int n = (int)(x.size() / 2U);
    for (int k = 0; k < kH; k++) {
        const int idx = n - kH + k;
        const float want_i = idx >= 0 ? x[(size_t)idx * 2U] : 0.0f;
        const float want_q = idx >= 0 ? x[(size_t)idx * 2U + 1U] : 0.0f;
        if (off_by_more((double)run.state.hist_i[(size_t)k], (double)want_i, 0.0)
            || off_by_more((double)run.state.hist_q[(size_t)k], (double)want_q, 0.0)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: history slot %d holds (%.9g, %.9g), want (%.9g, %.9g)\n", what, k,
                        (double)run.state.hist_i[(size_t)k], (double)run.state.hist_q[(size_t)k], (double)want_i,
                        (double)want_q);
            return 1;
        }
    }
    return rc;
}

/* ---------------- Filters ---------------- */

namespace {
struct Filter {
    const char* name;
    std::vector<float> taps;
};
} // namespace

/* The 63-tap prototype the pipeline falls back to where a digital profile's design does not fit (WIDE at 78125 Hz),
 * read from the demodulator's own plan. */
static std::vector<float>
fallback_taps(void) {
    std::vector<float> taps;
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return taps;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = 78125;
    s->rate_out = 78125;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    full_demod(s);
    taps.assign(s->channel_lpf_plan_taps, s->channel_lpf_plan_taps + s->channel_lpf_plan_taps_len);
    dsd_neo_aligned_free(s);
    return taps;
}

static std::vector<float>
analog_design(int rate_hz) {
    std::vector<float> taps((size_t)DSD_CHANNEL_LPF_MAX_TAPS);
    const int len = dsd_channel_lpf_design_analog(rate_hz, 16000, taps.data(), DSD_CHANNEL_LPF_MAX_TAPS);
    taps.resize(len > 0 ? (size_t)len : 0U);
    return taps;
}

/* A synthetic filter at the history's full span: random symmetric taps, scaled to an absolute sum of 1. */
static std::vector<float>
synthetic_taps(int len) {
    g_rand_state = 0xA5A5F00Du;
    std::vector<float> taps((size_t)len);
    double abs_sum = 0.0;
    for (int k = 0; k <= len / 2; k++) {
        const float v = randf();
        taps[(size_t)k] = v;
        taps[(size_t)(len - 1 - k)] = v;
    }
    for (float v : taps) {
        abs_sum += std::fabs((double)v);
    }
    for (float& v : taps) {
        v = (float)((double)v / abs_sum);
    }
    return taps;
}

static int
make_filters(std::vector<Filter>* filters) {
    struct Want {
        const char* name;
        std::vector<float> taps;
        size_t len;
    };

    const Want wants[] = {
        {"blackman 48k (135 taps)", analog_design(48000), 135U},
        {"blackman 24k (67 taps)", analog_design(24000), 67U},
        {"blackman 78.125k (219 taps)", analog_design(78125), 219U},
        {"fallback (63 taps)", fallback_taps(), 63U},
        {"synthetic (287 taps)", synthetic_taps(287), 287U},
        {"three taps", std::vector<float>{0.25f, 0.5f, 0.25f}, 3U},
    };
    int rc = 0;
    for (const Want& want : wants) {
        if (want.taps.size() != want.len) {
            DSD_FPRINTF(stderr, "FAIL: filter %s has %zu taps, want %zu\n", want.name, want.taps.size(), want.len);
            rc = 1;
            continue;
        }
        filters->push_back(Filter{want.name, want.taps});
    }
    return rc;
}

/* ---------------- Splits ---------------- */

static std::vector<Split>
make_splits(const Filter& filter, int vector_width) {
    const int len = (int)filter.taps.size();
    static const int kFixed[] = {0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 257, 1023, 4096};
    std::vector<Split> splits;
    splits.push_back(Split{"fixed sizes cycled", std::vector<int>(std::begin(kFixed), std::end(kFixed)), 1});
    for (int size : kFixed) {
        if (size > 0) {
            splits.push_back(Split{"one fixed size", std::vector<int>{size}, 1});
        }
    }
    for (uint32_t seed : {0x1234567u, 0x89ABCDEu, 0x0F1E2D3u}) {
        g_rand_state = seed ^ (uint32_t)len;
        Split split{"seeded random in [0, 3 x taps]", std::vector<int>(), 1};
        int total = 0;
        while (total < kStreamLen) {
            const int size = (int)(rand_u32() % (uint32_t)(3 * len + 1));
            split.sizes.push_back(size);
            total += size;
        }
        splits.push_back(split);
    }
    Split vector_split{"k x W + r", std::vector<int>(), 1};
    for (int k = 1; k <= 3; k++) {
        for (int r = 0; r < vector_width; r++) {
            vector_split.sizes.push_back(k * vector_width + r);
        }
    }
    splits.push_back(vector_split);
    splits.push_back(Split{"1-sample calls after a reset", std::vector<int>((size_t)(3 * len), 1), 0});
    return splits;
}

/* ---------------- FIR: segmentation ---------------- */

static int
test_segmentation(const Backend& backend, const Filter& filter) {
    const std::vector<float> x = make_stream(kStreamLen, 0xC0FFEE11u ^ (uint32_t)filter.taps.size());
    const std::vector<double> ref = reference_of(x, filter.taps);
    const std::vector<const std::vector<double>*> refs = {&ref};
    const int c = ((int)filter.taps.size() - 1) / 2;
    const int want_pending = kStreamLen < c ? kStreamLen : c;
    char what[256];
    int rc = 0;

    Run whole;
    run_split(backend.fn, x, filter.taps, Split{"whole", std::vector<int>{kStreamLen}, 0}, &whole);
    DSD_SNPRINTF(what, sizeof(what), "%s, %s, whole stream", backend.name, filter.name);
    rc |= check_counts(what, whole);
    rc |= check_against_reference(what, whole, refs);
    rc |= check_final_state(what, whole, x, want_pending);

    for (const Split& split : make_splits(filter, backend.vector_width)) {
        Run run;
        run_split(backend.fn, x, filter.taps, split, &run);
        DSD_SNPRINTF(what, sizeof(what), "%s, %s, %s (first size %d)", backend.name, filter.name, split.name,
                     split.sizes.empty() ? 0 : split.sizes[0]);
        int split_rc = check_counts(what, run);
        split_rc |= check_against_reference(what, run, refs);
        split_rc |= check_split_matches_whole(what, run, whole, backend.split_tol);
        split_rc |= check_final_state(what, run, x, want_pending);
        rc |= split_rc;
        if (split_rc != 0) {
            break; /* one failing split per filter says enough */
        }
    }
    return rc;
}

/* ---------------- FIR: tap changes without a clear ---------------- */

/* 135 -> 63 -> 219 taps mid-stream, each change on a call of no samples first: an output is the reference of the taps
 * it was made with, every sample is made once or still pending, and a split gives what one call per segment gives. */
static int
test_tap_switches(const Backend& backend, const std::vector<Filter>& filters) {
    const Filter* sets[3] = {&filters[0], &filters[3], &filters[2]}; /* 135, 63, 219 */
    const std::vector<float> x = make_stream(kStreamLen, 0x7A95u);
    const std::vector<double> refs_storage[3] = {reference_of(x, sets[0]->taps), reference_of(x, sets[1]->taps),
                                                 reference_of(x, sets[2]->taps)};
    const std::vector<const std::vector<double>*> refs = {&refs_storage[0], &refs_storage[1], &refs_storage[2]};
    const int bounds[4] = {0, 3001, 6007, kStreamLen};
    char what[200];
    int rc = 0;

    Run per_segment;
    Run split;
    per_segment.out.assign((size_t)(kStreamLen + kH) * 2U, 0.0f);
    split.out.assign((size_t)(kStreamLen + kH) * 2U, 0.0f);
    g_rand_state = 0x5117u;
    for (int s = 0; s < 3; s++) {
        const std::vector<float>& taps = sets[s]->taps;
        run_call(backend.fn, x, 0, taps, s, &per_segment);
        run_call(backend.fn, x, bounds[s + 1] - bounds[s], taps, s, &per_segment);
        run_call(backend.fn, x, 0, taps, s, &split);
        while (split.consumed < bounds[s + 1]) {
            int size = (int)(rand_u32() % 700U);
            if (size > bounds[s + 1] - split.consumed) {
                size = bounds[s + 1] - split.consumed;
            }
            run_call(backend.fn, x, size, taps, s, &split);
        }
    }
    for (const Run* run : {&per_segment, &split}) {
        DSD_SNPRINTF(what, sizeof(what), "%s, tap switches 135 -> 63 -> 219, %s", backend.name,
                     run == &split ? "random split" : "one call per segment");
        rc |= check_counts(what, *run);
        rc |= check_against_reference(what, *run, refs);
        if (run->produced + run->state.pending != kStreamLen) {
            DSD_FPRINTF(stderr, "  FAIL: %s: %d outputs and %d pending for %d samples\n", what, run->produced,
                        run->state.pending, kStreamLen);
            rc = 1;
        }
    }
    DSD_SNPRINTF(what, sizeof(what), "%s, tap switches 135 -> 63 -> 219, random split", backend.name);
    rc |= check_split_matches_whole(what, split, per_segment, backend.split_tol);
    if (split.emitter != per_segment.emitter) {
        DSD_FPRINTF(stderr, "  FAIL: %s: outputs made with other taps than one call per segment made them\n", what);
        rc = 1;
    }
    return rc;
}

/* ---------------- FIR: reset, invalid calls, capacity ---------------- */

/* A state zeroed after a stream (history and pending) runs the next stream as a fresh state does. */
static int
test_reset_equals_fresh(const Backend& backend, const Filter& filter) {
    const std::vector<float> first = make_stream(1234, 0xFEEDu);
    const std::vector<float> next = make_stream(3000, 0xBEEFu);
    const Split split{"mixed", {17, 300, 1, 0, 64, 999}, 1};
    Run used;
    run_split(backend.fn, first, filter.taps, split, &used);
    Run after_reset;
    after_reset.state = used.state;
    std::fill(after_reset.state.hist_i.begin(), after_reset.state.hist_i.end(), 0.0f);
    std::fill(after_reset.state.hist_q.begin(), after_reset.state.hist_q.end(), 0.0f);
    after_reset.state.pending = 0;
    run_split(backend.fn, next, filter.taps, split, &after_reset);
    Run fresh;
    run_split(backend.fn, next, filter.taps, split, &fresh);
    char what[200];
    DSD_SNPRINTF(what, sizeof(what), "%s, %s, reset vs fresh", backend.name, filter.name);
    return check_split_matches_whole(what, after_reset, fresh, 0.0f);
}

namespace {
/* A call's whole state, to see that a refused call changed none of it. */
struct Snapshot {
    std::vector<float> hist_i;
    std::vector<float> hist_q;
    std::vector<float> out;
    int pending;
};
} // namespace

static Snapshot
snapshot_of(const FirState& state, const std::vector<float>& out) {
    return Snapshot{state.hist_i, state.hist_q, out, state.pending};
}

/* Exactly the same values (a zero tolerance). */
static int
same_floats(const float* a, const float* b, size_t n) {
    for (size_t k = 0; k < n; k++) {
        if (off_by_more((double)a[k], (double)b[k], 0.0)) {
            return 0;
        }
    }
    return 1;
}

static int
same_snapshot(const Snapshot& a, const Snapshot& b) {
    return a.pending == b.pending && a.hist_i.size() == b.hist_i.size() && a.hist_q.size() == b.hist_q.size()
           && a.out.size() == b.out.size() && same_floats(a.hist_i.data(), b.hist_i.data(), a.hist_i.size())
           && same_floats(a.hist_q.data(), b.hist_q.data(), a.hist_q.size())
           && same_floats(a.out.data(), b.out.data(), a.out.size());
}

/* A warmed state: pending at c, the history full. */
static FirState
warmed_state(FirFn fn, const std::vector<float>& taps) {
    const std::vector<float> warm = make_stream(1000, 0x3141u);
    Run run;
    run_split(fn, warm, taps, Split{"warm", {1000}, 0}, &run);
    return run.state;
}

static int
test_invalid_calls(const Backend& backend, const Filter& filter) {
    const int len = (int)filter.taps.size();
    const int c = (len - 1) / 2;
    const std::vector<float> in = make_stream(64, 0x2718u);
    const std::vector<float> even_taps = {0.25f, 0.5f, 0.5f, 0.25f};
    int rc = 0;

    struct Case {
        const char* what;
        int pending;
        int n_in;
        int hist_len;
        const float* taps;
        int taps_len;
        int null_pending;
    };

    const Case cases[] = {
        {"negative pending", -1, 64, kH, filter.taps.data(), len, 0},
        {"pending + c past the history", kH - c + 1, 64, kH, filter.taps.data(), len, 0},
        {"even taps", c, 64, kH, even_taps.data(), 4, 0},
        {"one tap", c, 64, kH, filter.taps.data(), 1, 0},
        {"history shorter than taps - 1", 0, 64, len - 2, filter.taps.data(), len, 0},
        {"negative N", c, -1, kH, filter.taps.data(), len, 0},
        {"no pending", c, 64, kH, filter.taps.data(), len, 1},
    };
    for (const Case& t : cases) {
        FirState state = warmed_state(backend.fn, filter.taps);
        state.pending = t.pending;
        std::vector<float> out((size_t)(64 + kH) * 2U, 12345.0f);
        const Snapshot before = snapshot_of(state, out);
        const int got = backend.fn(in.data(), t.n_in, out.data(), 64 + kH, state.hist_i.data(), state.hist_q.data(),
                                   t.hist_len, t.null_pending ? nullptr : &state.pending, t.taps, t.taps_len);
        if (got != -1 || !same_snapshot(before, snapshot_of(state, out))) {
            DSD_FPRINTF(stderr, "  FAIL: %s, %s: %s returned %d (want -1) or changed the state\n", backend.name,
                        filter.name, t.what, got);
            rc = 1;
        }
    }
    return rc;
}

static int
test_out_capacity(const Backend& backend, const Filter& filter) {
    const int len = (int)filter.taps.size();
    const int c = (len - 1) / 2;
    const std::vector<float> in = make_stream(10, 0x1618u);
    int rc = 0;
    /* A warmed state makes N outputs for N samples: a capacity of 0 or N - 1 is refused, N is taken. */
    for (int cap : {0, 9, 10}) {
        FirState state = warmed_state(backend.fn, filter.taps);
        std::vector<float> out(20U, 777.0f);
        const Snapshot before = snapshot_of(state, out);
        const int got = backend.fn(in.data(), 10, out.data(), cap, state.hist_i.data(), state.hist_q.data(), kH,
                                   &state.pending, filter.taps.data(), len);
        const int want = cap < 10 ? -1 : 10;
        if (got != want || (want < 0 && !same_snapshot(before, snapshot_of(state, out)))) {
            DSD_FPRINTF(stderr, "  FAIL: %s, %s: out_cap %d returned %d, want %d with the state unchanged\n",
                        backend.name, filter.name, cap, got, want);
            rc = 1;
        }
    }
    /* A call that makes no outputs needs no capacity, and still takes its samples. */
    if (c > 10) {
        FirState state;
        const int got = backend.fn(in.data(), 10, nullptr, 0, state.hist_i.data(), state.hist_q.data(), kH,
                                   &state.pending, filter.taps.data(), len);
        if (got != 0 || state.pending != 10) {
            DSD_FPRINTF(stderr, "  FAIL: %s, %s: a 10-sample warm-up call with out_cap 0 returned %d, pending %d\n",
                        backend.name, filter.name, got, state.pending);
            rc = 1;
        }
    }
    return rc;
}

/* ---------------- FIR: a maximum block on a tap shrink, exact-size buffers ---------------- */

/* A maximum block right after a shrink from @p from to @p to taps makes N + c_from - c_to outputs: they fit the
 * pipeline's work buffer, MAXIMUM_BUF_LENGTH / 2 + DSD_CHANNEL_LPF_HIST_LEN complex, allocated to exactly that. */
static int
test_max_block_shrink(const Backend& backend, const Filter& from, const Filter& to) {
    const int n_block = MAXIMUM_BUF_LENGTH / 2;
    const int warm_len = 1000;
    const std::vector<float> x = make_stream(warm_len + n_block, 0x4242u);
    const std::vector<float> warm(x.begin(), x.begin() + (ptrdiff_t)warm_len * 2);
    const std::vector<double> ref_to = reference_of(x, to.taps);
    const int c_from = ((int)from.taps.size() - 1) / 2;
    const int c_to = ((int)to.taps.size() - 1) / 2;
    const int cap = MAXIMUM_BUF_LENGTH / 2 + DSD_CHANNEL_LPF_HIST_LEN;
    std::unique_ptr<float[]> out(new float[(size_t)cap * 2U]);
    std::unique_ptr<float[]> in(new float[(size_t)n_block * 2U]);
    DSD_MEMCPY(in.get(), x.data() + (size_t)warm_len * 2U, (size_t)n_block * 2U * sizeof(float));

    Run run;
    run_split(backend.fn, warm, from.taps, Split{"warm", {warm_len}, 0}, &run);
    const int first = run.produced; /* outputs before the shrink */
    const int got = backend.fn(in.get(), n_block, out.get(), cap, run.state.hist_i.data(), run.state.hist_q.data(), kH,
                               &run.state.pending, to.taps.data(), (int)to.taps.size());
    const int want = n_block + c_from - c_to;
    char what[200];
    DSD_SNPRINTF(what, sizeof(what), "%s, a %d-sample block on a %zu -> %zu tap shrink", backend.name, n_block,
                 from.taps.size(), to.taps.size());
    if (got != want || run.state.pending != c_to) {
        DSD_FPRINTF(stderr, "  FAIL: %s: %d outputs, pending %d; want %d, pending %d\n", what, got, run.state.pending,
                    want, c_to);
        return 1;
    }
    for (int k = 0; k < got * 2; k++) {
        const size_t j = (size_t)first * 2U + (size_t)k;
        if (off_by_more((double)out[(size_t)k], ref_to[j], kRefTol)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: output %d is %.9g, the reference %.9g\n", what, first + k / 2,
                        (double)out[(size_t)k], ref_to[j]);
            return 1;
        }
    }
    return 0;
}

/* Calls at nonzero pending into buffers of exactly the size the call needs, fenced by NaN: nothing outside them is
 * written, and a read outside them would turn an output NaN. Then the same calls on exact heap allocations, for the
 * sanitizers. */
static int
test_exact_buffers(const Backend& backend, const Filter& filter) {
    const int len = (int)filter.taps.size();
    const int c = (len - 1) / 2;
    const int kFence = 16;
    const float nan = std::nanf("");
    char what[200];
    int rc = 0;
    for (int fenced = 1; fenced >= 0; fenced--) {
        for (int pending0 : {c / 2 + 1, c}) {
            for (int n_in : {1, 2, 3, 7, 8, 9, 17, 33, 100}) {
                /* The state: pending0 samples of warm-up after a reset, or a full warm-up. */
                const std::vector<float> x = make_stream(pending0 + n_in, 0x9999u + (uint32_t)n_in);
                const std::vector<double> ref = reference_of(x, filter.taps);
                Run warm;
                run_split(backend.fn, std::vector<float>(x.begin(), x.begin() + (ptrdiff_t)pending0 * 2), filter.taps,
                          Split{"warm", {pending0}, 0}, &warm);
                const int want = formula_outputs(warm.state.pending, n_in, len);
                const int pad = fenced ? kFence : 0;
                std::vector<float> in_buf((size_t)(n_in * 2 + 2 * pad), nan);
                std::vector<float> out_buf((size_t)(want * 2 + 2 * pad), nan);
                std::vector<float> hi_buf((size_t)(kH + 2 * pad), nan);
                std::vector<float> hq_buf((size_t)(kH + 2 * pad), nan);
                std::unique_ptr<float[]> in_exact(new float[(size_t)n_in * 2U]);
                std::unique_ptr<float[]> out_exact(new float[want > 0 ? (size_t)want * 2U : 1U]);
                std::unique_ptr<float[]> hi_exact(new float[(size_t)kH]);
                std::unique_ptr<float[]> hq_exact(new float[(size_t)kH]);
                float* in = fenced ? in_buf.data() + pad : in_exact.get();
                float* out = fenced ? out_buf.data() + pad : out_exact.get();
                float* hi = fenced ? hi_buf.data() + pad : hi_exact.get();
                float* hq = fenced ? hq_buf.data() + pad : hq_exact.get();
                DSD_MEMCPY(in, x.data() + (size_t)pending0 * 2U, (size_t)n_in * 2U * sizeof(float));
                DSD_MEMCPY(hi, warm.state.hist_i.data(), (size_t)kH * sizeof(float));
                DSD_MEMCPY(hq, warm.state.hist_q.data(), (size_t)kH * sizeof(float));
                int pending = warm.state.pending;
                const int got = backend.fn(in, n_in, out, want, hi, hq, kH, &pending, filter.taps.data(), len);
                DSD_SNPRINTF(what, sizeof(what), "%s, %s, %s buffers, N=%d at pending %d", backend.name, filter.name,
                             fenced ? "NaN-fenced" : "exact heap", n_in, warm.state.pending);
                if (got != want) {
                    DSD_FPRINTF(stderr, "  FAIL: %s: %d outputs, want %d\n", what, got, want);
                    rc = 1;
                    continue;
                }
                for (int k = 0; k < want * 2; k++) {
                    const size_t j = (size_t)warm.produced * 2U + (size_t)k;
                    if (off_by_more((double)out[k], ref[j], kRefTol)) {
                        DSD_FPRINTF(stderr, "  FAIL: %s: output %d is %.9g, the reference %.9g\n", what, k / 2,
                                    (double)out[k], ref[j]);
                        rc = 1;
                        break;
                    }
                }
                for (int k = 0; fenced && k < pad; k++) {
                    const std::vector<float>* bufs[] = {&in_buf, &out_buf, &hi_buf, &hq_buf};
                    for (const std::vector<float>* buf : bufs) {
                        if (!std::isnan((*buf)[(size_t)k]) || !std::isnan((*buf)[buf->size() - 1U - (size_t)k])) {
                            DSD_FPRINTF(stderr, "  FAIL: %s: wrote outside a buffer\n", what);
                            rc = 1;
                            k = pad;
                            break;
                        }
                    }
                }
            }
        }
    }
    return rc;
}

/* ---------------- Pipeline ---------------- */

namespace {
struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};
} // namespace

/* A pass-through demodulator with the channel filter on at @p rate_out, after @p passes half-band passes. */
static void
configure_raw(demod_state* s, int rate_out, int profile, int passes) {
    s->rate_in = rate_out << passes;
    s->rate_out = rate_out;
    s->mode_demod = &raw_demod;
    s->downsample_passes = passes;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile;
}

static void
run_block(demod_state* s, const std::vector<float>& iq) {
    if (!iq.empty()) {
        DSD_MEMCPY(s->input_cb_buf, iq.data(), iq.size() * sizeof(float));
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = (int)iq.size();
    full_demod(s);
}

static int
all_zero(const float* v, size_t n) {
    for (size_t k = 0; k < n; k++) {
        if (std::fabs(v[k]) > 0.0f) {
            return 0;
        }
    }
    return 1;
}

static int
test_reset_filter_state(void) {
    std::printf("Testing dsd_demod_reset_filter_state()...\n");
    DemodBox used;
    DemodBox fresh;
    if (!used.s || !fresh.s) {
        DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
        return 1;
    }
    int rc = 0;
    /* Seeded state: every history slot and a pending count. */
    for (int st = 0; st < 10; st++) {
        for (int k = 0; k < HB_TAPS_MAX - 1; k++) {
            used.s->hb_hist_i[st][k] = 1.0f;
            used.s->hb_hist_q[st][k] = -1.0f;
        }
    }
    for (int k = 0; k < DSD_CHANNEL_LPF_HIST_LEN; k++) {
        used.s->channel_lpf_hist_i[k] = 2.0f;
        used.s->channel_lpf_hist_q[k] = -2.0f;
    }
    *channel_pending_of(used.s) = 7;
    reset_filter_state(used.s);
    if (!all_zero(&used.s->hb_hist_i[0][0], sizeof(used.s->hb_hist_i) / sizeof(float))
        || !all_zero(&used.s->hb_hist_q[0][0], sizeof(used.s->hb_hist_q) / sizeof(float))
        || !all_zero(used.s->channel_lpf_hist_i, (size_t)DSD_CHANNEL_LPF_HIST_LEN)
        || !all_zero(used.s->channel_lpf_hist_q, (size_t)DSD_CHANNEL_LPF_HIST_LEN)
        || *channel_pending_of(used.s) != 0) {
        DSD_FPRINTF(stderr, "  FAIL: the reset left history or a pending count behind\n");
        rc = 1;
    }

    /* After a stream and a reset, the next block comes out as from a fresh demodulator. */
    configure_raw(used.s, 48000, DSD_CH_LPF_PROFILE_12K5, 1);
    configure_raw(fresh.s, 48000, DSD_CH_LPF_PROFILE_12K5, 1);
    run_block(used.s, make_stream(1500, 0xAAAAu));
    reset_filter_state(used.s);
    const std::vector<float> next = make_stream(1200, 0xBBBBu);
    run_block(used.s, next);
    run_block(fresh.s, next);
    if (used.s->result_len != fresh.s->result_len
        || !same_floats(used.s->result, fresh.s->result, (size_t)fresh.s->result_len)) {
        DSD_FPRINTF(stderr, "  FAIL: a block after a reset gave %d outputs, a fresh demodulator %d (or other values)\n",
                    used.s->result_len, fresh.s->result_len);
        rc = 1;
    }
    if (rc == 0) {
        std::printf("  PASS\n");
    }
    return rc;
}

/* The channel state is kept across a profile change at the same rate (a tap-count change reconciles through pending),
 * and cleared by a rate change and by a block the filter does not run on. */
static int
test_channel_state_rules(void) {
    std::printf("Testing when the pipeline keeps and clears the channel filter state...\n");
    DemodBox box;
    demod_state* s = box.s;
    if (!s) {
        DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
        return 1;
    }
    int rc = 0;
    const std::vector<float> warm = make_stream(1000, 0x1111u);
    const std::vector<float> small = make_stream(10, 0x2222u);

    /* Warm at 48 kHz: 135 taps, pending 67. A profile change at the same rate keeps the state: 10 samples make 10. */
    configure_raw(s, 48000, DSD_CH_LPF_PROFILE_12K5, 0);
    run_block(s, warm);
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_6K25;
    run_block(s, small);
    if (s->channel_lpf_plan_taps_len != 135 || s->result_len != 20 || *channel_pending_of(s) != 67) {
        DSD_FPRINTF(stderr,
                    "  FAIL: a profile change at 48 kHz: %d taps, %d floats out, pending %d; want 135, 20, 67\n",
                    s->channel_lpf_plan_taps_len, s->result_len, *channel_pending_of(s));
        rc = 1;
    }

    /* A rate change starts the channel over: 67 taps at 24 kHz, 10 samples make nothing yet. */
    s->rate_in = 24000;
    s->rate_out = 24000;
    run_block(s, small);
    if (s->channel_lpf_plan_taps_len != 67 || s->result_len != 0 || *channel_pending_of(s) != 10) {
        DSD_FPRINTF(stderr, "  FAIL: a rate change: %d taps, %d floats out, pending %d; want 67, 0, 10\n",
                    s->channel_lpf_plan_taps_len, s->result_len, *channel_pending_of(s));
        rc = 1;
    }

    /* A block with the filter off passes unfiltered and clears what the filter held. */
    run_block(s, warm);
    s->channel_lpf_enable = 0;
    run_block(s, small);
    if (s->result_len != 20 || *channel_pending_of(s) != 0
        || !all_zero(s->channel_lpf_hist_i, (size_t)DSD_CHANNEL_LPF_HIST_LEN)
        || !all_zero(s->channel_lpf_hist_q, (size_t)DSD_CHANNEL_LPF_HIST_LEN)) {
        DSD_FPRINTF(stderr, "  FAIL: a block with the filter off: %d floats out, pending %d (want 20, 0, history 0)\n",
                    s->result_len, *channel_pending_of(s));
        rc = 1;
    }

    /* So does a block with no plan: an analog width the rate cannot realize. */
    s->channel_lpf_enable = 1;
    run_block(s, warm);
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->analog_family = 1;
    s->analog_demod = DSD_ANALOG_DEMOD_FM;
    s->channel_lpf_width_hz = 40000;
    run_block(s, small);
    if (s->channel_lpf_plan_taps_len != 0 || s->result_len != 20 || *channel_pending_of(s) != 0
        || !all_zero(s->channel_lpf_hist_i, (size_t)DSD_CHANNEL_LPF_HIST_LEN)) {
        DSD_FPRINTF(stderr, "  FAIL: a block with no plan: %d taps, %d floats out, pending %d (want 0, 20, 0)\n",
                    s->channel_lpf_plan_taps_len, s->result_len, *channel_pending_of(s));
        rc = 1;
    }
    if (rc == 0) {
        std::printf("  PASS\n");
    }
    return rc;
}

/* A block the front end left empty (a channel-filter warm-up, or nothing in) makes no output and changes no per-block
 * decision: the squelch gate, the squelch envelope, the channel power and the CQPSK zero symbols. */
static int
test_empty_front_end_block(void) {
    std::printf("Testing full_demod() on a block its front end left empty...\n");
    int rc = 0;

    struct Case {
        const char* what;
        int cqpsk;
        int samples; /* complex, into the channel filter (no half-band) */
    };

    const Case cases[] = {
        {"FM monitor, no samples", 0, 0},
        {"FM monitor, a channel-filter warm-up block", 0, 10},
        {"CQPSK, no samples", 1, 0},
        {"CQPSK, a channel-filter warm-up block", 1, 10},
    };
    for (const Case& t : cases) {
        DemodBox box;
        demod_state* s = box.s;
        if (!s) {
            DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
            return 1;
        }
        s->rate_in = 48000;
        s->rate_out = 48000;
        s->channel_lpf_enable = 1;
        s->channel_lpf_profile = t.cqpsk ? DSD_CH_LPF_PROFILE_P25_CQPSK : DSD_CH_LPF_PROFILE_WIDE;
        s->mode_demod = &dsd_fm_demod;
        s->output_kind = t.cqpsk ? DSD_DEMOD_OUTPUT_SYMBOL_CQPSK : DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
        s->cqpsk_enable = t.cqpsk;
        s->ted_sps = 10;
        s->channel_squelch_level.store(1.0f);
        /* The previous block was squelched: the gate closed, the envelope down, the power low. */
        s->channel_squelched = 1;
        s->squelch_gate_open = 0;
        s->squelch_env = 0.0f;
        s->channel_pwr = 0.25f;
        s->result_len = 77;
        run_block(s, make_stream(t.samples, 0x3333u));
        if (s->result_len != 0 || s->channel_squelched != 1 || s->squelch_gate_open != 0
            || std::fabs(s->squelch_env) > 0.0f || off_by_more((double)s->channel_pwr, 0.25, 0.0)) {
            DSD_FPRINTF(stderr,
                        "  FAIL: %s: result_len %d, squelched %d, gate %d, envelope %.4f, power %.4f; want 0, 1, 0, "
                        "0, 0.25\n",
                        t.what, s->result_len, s->channel_squelched, s->squelch_gate_open, (double)s->squelch_env,
                        (double)s->channel_pwr);
            rc = 1;
        }
    }
    if (rc == 0) {
        std::printf("  PASS\n");
    }
    return rc;
}

int
main(void) {
    int failures = 0;
    std::vector<Filter> filters;
    if (make_filters(&filters) != 0) {
        return 1;
    }
    const std::vector<Backend> backends = fir_backends();
    for (const Backend& backend : backends) {
        std::printf("Testing the channel FIR across block splits (%s)...\n", backend.name);
        int rc = 0;
        for (const Filter& filter : filters) {
            rc |= test_segmentation(backend, filter);
            rc |= test_reset_equals_fresh(backend, filter);
            rc |= test_invalid_calls(backend, filter);
            rc |= test_out_capacity(backend, filter);
            rc |= test_exact_buffers(backend, filter);
        }
        rc |= test_tap_switches(backend, filters);
        rc |= test_max_block_shrink(backend, filters[2], filters[3]); /* 219 -> 63 */
        rc |= test_max_block_shrink(backend, filters[4], filters[5]); /* 287 -> 3 */
        std::printf(rc == 0 ? "  PASS\n" : "  FAILED\n");
        failures += rc;
    }
    failures += test_reset_filter_state();
    failures += test_channel_state_rules();
    failures += test_empty_front_end_block();
    if (failures > 0) {
        std::printf("\n%d test group(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll tests PASSED\n");
    return 0;
}
