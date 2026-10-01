// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The channel FIR's and the half-band decimators' output does not depend on how their input is cut into blocks
 * (issue #572).
 *
 * simd_fir_complex_apply() holds back the outputs whose look-ahead has not arrived yet (`pending`) and makes them on a
 * later call, so a stream gives the same outputs whatever its blocks. Each backend and the dispatcher are checked, for
 * many filters and block splits, against the documented output counts, a double-precision centred convolution with
 * zero left padding, and the backend's own output for the whole stream in one call. Tap changes without a clear,
 * invalid calls, short output capacities and exact-size buffers are covered too.
 *
 * The half-band decimators (simd_hb_decim2_complex(), simd_hb_decim2_real()) carry their look-ahead and their
 * decimation phase the same way, in one pending count: the same checks run on them for the canonical, synthetic and
 * generic tap sets, with exact-size buffers at every block size up to a few filter spans.
 *
 * The pipeline side: dsd_demod_reset_filter_state() returns the filter state to a fresh one; the channel state is kept
 * across a profile change at the same rate and cleared by a rate change and by a block the filter does not run on; the
 * half-band state is kept while the pass count holds and cleared when it changes; and full_demod() makes no per-block
 * decision on a block its front end left empty.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
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

/* The half-band decimators: in_len in floats (2 per complex sample) and a return in floats for the complex one, in
 * samples for the real one. */
using HbComplexFn = int (*)(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                            const float* taps, int taps_len);
using HbRealFn = int (*)(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                         int taps_len);

struct HbBackend {
    const char* name;
    HbComplexFn complex_fn;
    HbRealFn real_fn;
    float split_tol;   /* split vs whole, as for the FIR */
    int vector_inputs; /* input samples per vector step of the backend's widest half-band kernel */
};

} // namespace

/* ---------------- The functions under test ---------------- */

/* Every binding to the code under test sits in this block: the backends, the demodulator's pending counts and its
 * filter reset. */

#if defined(__x86_64__) || defined(_M_X64)
extern "C" int simd_fir_complex_apply_sse2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_sse2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_sse2(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
extern "C" int simd_fir_complex_apply_avx2(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_avx2(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_avx2(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
#endif
#endif
#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" int simd_fir_complex_apply_neon(const float* in, int n_in, float* out, int out_cap, float* hist_i,
                                           float* hist_q, int hist_len, int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_complex_neon(const float* in, int in_len, float* out, float* hist_i, float* hist_q,
                                           int* pending, const float* taps, int taps_len);
extern "C" int simd_hb_decim2_real_neon(const float* in, int in_len, float* out, float* hist, int* pending,
                                        const float* taps, int taps_len);
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

static std::vector<HbBackend>
hb_backends(void) {
    std::vector<HbBackend> backends;
    backends.push_back(HbBackend{"scalar", simd_hb_decim2_complex_scalar, simd_hb_decim2_real_scalar, 0.0f, 2});
#if defined(__x86_64__) || defined(_M_X64)
    backends.push_back(HbBackend{"sse2", simd_hb_decim2_complex_sse2, simd_hb_decim2_real_sse2, 1e-5f, 8});
#if defined(DSD_NEO_TEST_HAVE_AVX2_IMPL)
    if (dsd_neo_cpu_has_avx2_with_os_support()) {
        backends.push_back(HbBackend{"avx2", simd_hb_decim2_complex_avx2, simd_hb_decim2_real_avx2, 1e-5f, 16});
    }
#endif
#endif
#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    backends.push_back(HbBackend{"neon", simd_hb_decim2_complex_neon, simd_hb_decim2_real_neon, 1e-5f, 8});
#endif
    backends.push_back(HbBackend{"dispatch", simd_hb_decim2_complex, simd_hb_decim2_real, 1e-5f, 16});
    return backends;
}

static int*
channel_pending_of(demod_state* d) {
    return &d->channel_lpf_pending;
}

static int*
hb_pending_of(demod_state* d, int stage) {
    return &d->hb_pending[stage];
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

/* Every call against the count formula, and none refused (the FIR's Run and the half-band's HbRun). */
template <typename R>
static int
check_counts(const char* what, const R& run) {
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

/* ---------------- Half-band: helpers ---------------- */

namespace {
struct HbFilter {
    const char* name;
    std::vector<float> taps;
    int complex; /* 1: the complex decimator (2 floats a sample), 0: the real one */
};

/* Everything a run of a stream through one half-band decimator gave. */
struct HbRun {
    std::vector<float> out;     /* every output, in order (interleaved I/Q for the complex decimator) */
    std::vector<int> lookahead; /* per output: the stream samples after its centre the call that made it had */
    std::vector<float> hist_i;  /* taps - 1 samples: the real decimator's history, or the complex one's I */
    std::vector<float> hist_q;  /* taps - 1 samples: the complex decimator's Q (the real one has none) */
    int pending = 0;
    int produced = 0; /* outputs */
    int consumed = 0; /* input samples */
    int calls = 0;
    int count_mismatches = 0;
    char first_count_mismatch[200] = {0};
    int invalid_calls = 0;
};
} // namespace

/* Floats a sample: 2 for the complex decimator, 1 for the real one. */
static int
hb_width(const HbFilter& f) {
    return f.complex ? 2 : 1;
}

/* The documented count: with lt = pending + N, (lt - c + 1) / 2 outputs once lt reaches c + 1, none before; pending
 * becomes lt - 2 x outputs. */
static int
hb_formula_outputs(int pending, int n_in, int taps_len) {
    const int c = (taps_len - 1) / 2;
    const int lt = pending + n_in;
    return lt >= c + 1 ? (lt - c + 1) / 2 : 0;
}

/* @p samples samples of @p width floats each, uniform in [-1, 1]. */
static std::vector<float>
make_samples(int samples, int width, uint32_t seed) {
    g_rand_state = seed;
    std::vector<float> x((size_t)samples * (size_t)width);
    for (float& v : x) {
        v = randf();
    }
    return x;
}

/* One call of @p n_in samples through the backend's decimator for @p f: its return as the decimator gives it (floats
 * for the complex decimator, samples for the real one). */
static int
hb_apply(const HbBackend& b, const HbFilter& f, const float* in, int n_in, float* out, float* hist_i, float* hist_q,
         int* pending) {
    const int len = (int)f.taps.size();
    if (f.complex) {
        return b.complex_fn(in, n_in * 2, out, hist_i, hist_q, pending, f.taps.data(), len);
    }
    return b.real_fn(in, n_in, out, hist_i, pending, f.taps.data(), len);
}

/* Decimating centred convolution in double with zero left padding: output j is sum_k taps[k] * x[2j + k - c], for the
 * outputs whose look-ahead the stream holds. */
static std::vector<double>
hb_reference_of(const std::vector<float>& x, const HbFilter& f) {
    const int w = hb_width(f);
    const int n = (int)(x.size() / (size_t)w);
    const int len = (int)f.taps.size();
    const int c = (len - 1) / 2;
    const int outputs = hb_formula_outputs(0, n, len);
    std::vector<double> ref((size_t)outputs * (size_t)w, 0.0);
    for (int j = 0; j < outputs; j++) {
        for (int k = 0; k < len; k++) {
            const int idx = 2 * j + k - c;
            if (idx < 0) {
                continue;
            }
            for (int part = 0; part < w; part++) {
                ref[(size_t)j * (size_t)w + (size_t)part] +=
                    (double)f.taps[(size_t)k] * (double)x[(size_t)idx * (size_t)w + (size_t)part];
            }
        }
    }
    return ref;
}

/* A fresh decimator state for a stream of up to @p stream_samples samples: zero history, nothing pending. */
static void
hb_start(const HbFilter& f, int stream_samples, HbRun* run) {
    const size_t hist_len = f.taps.size() - 1U;
    run->hist_i.assign(hist_len, 0.0f);
    run->hist_q.assign(hist_len, 0.0f);
    run->out.assign(((size_t)stream_samples / 2U + 8U) * (size_t)hb_width(f), 0.0f);
}

/* Feed @p n_in samples of @p x from the run's position, checking the count and pending against the formula. */
static void
hb_run_call(const HbBackend& b, const HbFilter& f, const std::vector<float>& x, int n_in, HbRun* run) {
    const int w = hb_width(f);
    const int before = run->pending;
    const int want = hb_formula_outputs(before, n_in, (int)f.taps.size());
    const int want_pending = before + n_in - 2 * want;
    const int got = hb_apply(b, f, x.data() + (size_t)run->consumed * (size_t)w, n_in,
                             run->out.data() + (size_t)run->produced * (size_t)w, run->hist_i.data(),
                             run->hist_q.data(), &run->pending);
    run->calls++;
    if (got < 0) {
        run->invalid_calls++;
    }
    if (got != want * w || run->pending != want_pending) {
        if (run->count_mismatches++ == 0) {
            DSD_SNPRINTF(run->first_count_mismatch, sizeof(run->first_count_mismatch),
                         "call %d (N=%d, pending %d): returned %d, pending %d; want %d, pending %d", run->calls, n_in,
                         before, got, run->pending, want * w, want_pending);
        }
    }
    run->consumed += n_in;
    const int made = got > 0 ? got / w : 0;
    for (int k = 0; k < made; k++) {
        run->lookahead.push_back(run->consumed - 1 - 2 * (run->produced + k));
    }
    run->produced += made;
}

/* The rest of @p x through the run's state, cut as @p split says. */
static void
hb_feed(const HbBackend& b, const HbFilter& f, const std::vector<float>& x, const Split& split, HbRun* run) {
    const int n = (int)(x.size() / (size_t)hb_width(f));
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
        hb_run_call(b, f, x, size, run);
        if (run->invalid_calls > 0) {
            return;
        }
    }
}

static void
hb_run_split(const HbBackend& b, const HbFilter& f, const std::vector<float>& x, const Split& split, HbRun* run) {
    hb_start(f, (int)(x.size() / (size_t)hb_width(f)), run);
    hb_feed(b, f, x, split, run);
}

static int
hb_check_against_reference(const char* what, const HbRun& run, const std::vector<double>& ref, int w) {
    double worst = 0.0;
    int worst_at = -1;
    int bad = 0;
    for (int j = 0; j < run.produced; j++) {
        if ((size_t)(j + 1) * (size_t)w > ref.size()) {
            continue; /* past the outputs the stream can make (a decimator that pads its look-ahead) */
        }
        for (int part = 0; part < w; part++) {
            const size_t at = (size_t)j * (size_t)w + (size_t)part;
            const double got = (double)run.out[at];
            const double err = std::isnan(got) ? INFINITY : std::fabs(got - ref[at]);
            if (off_by_more(got, ref[at], kRefTol)) {
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
hb_check_split_matches_whole(const char* what, const HbRun& run, const HbRun& whole, float tol, int w) {
    if (run.produced != whole.produced) {
        DSD_FPRINTF(stderr, "  FAIL: %s: %d outputs, the whole stream in one call made %d\n", what, run.produced,
                    whole.produced);
        return 1;
    }
    for (size_t k = 0; k < (size_t)run.produced * (size_t)w; k++) {
        if (off_by_more((double)run.out[k], (double)whole.out[k], (double)tol)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: output %zu is %.9g, the whole stream in one call made %.9g (tol %.0e)\n",
                        what, k / (size_t)w, (double)run.out[k], (double)whole.out[k], (double)tol);
            return 1;
        }
    }
    return 0;
}

/* After the stream: pending is what the formula leaves, and the history holds the stream's newest taps - 1 samples,
 * right-aligned and zero-padded on the left. */
static int
hb_check_final_state(const char* what, const HbRun& run, const std::vector<float>& x, int want_pending, int w) {
    if (run.pending != want_pending) {
        DSD_FPRINTF(stderr, "  FAIL: %s: final pending %d, want %d\n", what, run.pending, want_pending);
        return 1;
    }
    const int n = (int)(x.size() / (size_t)w);
    const int hist_len = (int)run.hist_i.size();
    for (int k = 0; k < hist_len; k++) {
        const int idx = n - hist_len + k;
        const float want_i = idx >= 0 ? x[(size_t)idx * (size_t)w] : 0.0f;
        const float want_q = (idx >= 0 && w == 2) ? x[(size_t)idx * 2U + 1U] : 0.0f;
        const float got_q = w == 2 ? run.hist_q[(size_t)k] : 0.0f;
        if (off_by_more((double)run.hist_i[(size_t)k], (double)want_i, 0.0)
            || off_by_more((double)got_q, (double)want_q, 0.0)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: history slot %d holds (%.9g, %.9g), want (%.9g, %.9g)\n", what, k,
                        (double)run.hist_i[(size_t)k], (double)got_q, (double)want_i, (double)want_q);
            return 1;
        }
    }
    return 0;
}

/* ---------------- Half-band: filters ---------------- */

/* A synthetic half-band filter: random even taps, the odd ones zero but the centre, which is 0.5. */
static std::vector<float>
synthetic_halfband_taps(int len, uint32_t seed) {
    g_rand_state = seed;
    const int c = (len - 1) / 2;
    std::vector<float> taps((size_t)len, 0.0f);
    for (int e = 0; e < c; e += 2) {
        const float v = 0.2f * randf();
        taps[(size_t)e] = v;
        taps[(size_t)(len - 1 - e)] = v;
    }
    taps[(size_t)c] = 0.5f;
    return taps;
}

static std::vector<HbFilter>
make_hb_filters(void) {
    const std::vector<float> hb15(hb_q15_taps, hb_q15_taps + HB_TAPS);
    const std::vector<float> hb31(hb31_q15_taps, hb31_q15_taps + 31);
    return std::vector<HbFilter>{
        {"hb_q15_taps (15, complex)", hb15, 1},
        {"hb31_q15_taps (31, complex)", hb31, 1},
        {"synthetic 15 (complex)", synthetic_halfband_taps(15, 0x15151515u), 1},
        {"synthetic 31 (complex)", synthetic_halfband_taps(31, 0x31313131u), 1},
        {"generic 23 (complex, scratch kernel)", synthetic_halfband_taps(23, 0x23232323u), 1},
        {"hb_q15_taps (15, real)", hb15, 0},
        {"hb31_q15_taps (31, real)", hb31, 0},
    };
}

/* ---------------- Half-band: segmentation ---------------- */

static int
test_hb_segmentation(const HbBackend& b, const HbFilter& f) {
    const int w = hb_width(f);
    const int len = (int)f.taps.size();
    const std::vector<float> x = make_samples(kStreamLen, w, 0xC0FFEE11u ^ (uint32_t)(len * w));
    const std::vector<double> ref = hb_reference_of(x, f);
    const int want_pending = kStreamLen - 2 * hb_formula_outputs(0, kStreamLen, len);
    char what[256];
    int rc = 0;

    HbRun whole;
    hb_run_split(b, f, x, Split{"whole", std::vector<int>{kStreamLen}, 0}, &whole);
    DSD_SNPRINTF(what, sizeof(what), "%s, %s, whole stream", b.name, f.name);
    rc |= check_counts(what, whole);
    rc |= hb_check_against_reference(what, whole, ref, w);
    rc |= hb_check_final_state(what, whole, x, want_pending, w);

    for (const Split& split : make_splits(Filter{f.name, f.taps}, b.vector_inputs)) {
        HbRun run;
        hb_run_split(b, f, x, split, &run);
        DSD_SNPRINTF(what, sizeof(what), "%s, %s, %s (first size %d)", b.name, f.name, split.name,
                     split.sizes.empty() ? 0 : split.sizes[0]);
        int split_rc = check_counts(what, run);
        split_rc |= hb_check_against_reference(what, run, ref, w);
        split_rc |= hb_check_split_matches_whole(what, run, whole, b.split_tol, w);
        split_rc |= hb_check_final_state(what, run, x, want_pending, w);
        rc |= split_rc;
        if (split_rc != 0) {
            break; /* one failing split per filter says enough */
        }
    }
    return rc;
}

/* A state zeroed after a stream (history and pending) runs the next stream as a fresh state does. */
static int
test_hb_reset_equals_fresh(const HbBackend& b, const HbFilter& f) {
    const int w = hb_width(f);
    const std::vector<float> first = make_samples(1234, w, 0xFEEDu);
    const std::vector<float> next = make_samples(3000, w, 0xBEEFu);
    const Split split{"mixed", {17, 300, 1, 0, 64, 999}, 1};
    HbRun used;
    hb_run_split(b, f, first, split, &used);
    HbRun after_reset;
    hb_start(f, 3000, &after_reset);
    after_reset.hist_i = used.hist_i;
    after_reset.hist_q = used.hist_q;
    std::fill(after_reset.hist_i.begin(), after_reset.hist_i.end(), 0.0f);
    std::fill(after_reset.hist_q.begin(), after_reset.hist_q.end(), 0.0f);
    after_reset.pending = 0;
    hb_feed(b, f, next, split, &after_reset);
    HbRun fresh;
    hb_run_split(b, f, next, split, &fresh);
    char what[200];
    DSD_SNPRINTF(what, sizeof(what), "%s, %s, reset vs fresh", b.name, f.name);
    return hb_check_split_matches_whole(what, after_reset, fresh, 0.0f, w);
}

/* A warmed state: the history full and pending at c - 1 or c. */
static HbRun
hb_warmed(const HbBackend& b, const HbFilter& f) {
    HbRun run;
    hb_run_split(b, f, make_samples(1000, hb_width(f), 0x3141u), Split{"warm", {1000}, 0}, &run);
    return run;
}

static Snapshot
hb_snapshot_of(const HbRun& run, const std::vector<float>& out) {
    return Snapshot{run.hist_i, run.hist_q, out, run.pending};
}

namespace {
/* Which argument an invalid call passes as NULL. */
enum class HbNull : uint8_t { None, Pending, HistI, HistQ, Taps, In, Out };

struct HbInvalidCase {
    const char* what;
    int pending;
    int in_len; /* as the decimator takes it: floats for the complex one */
    const float* taps;
    int taps_len;
    HbNull null_arg;
};
} // namespace

static int
hb_invalid_call(const HbBackend& b, const HbFilter& f, const std::vector<float>& in, const HbInvalidCase& t) {
    HbRun state = hb_warmed(b, f);
    state.pending = t.pending;
    std::vector<float> out((size_t)(64 + 8) * (size_t)hb_width(f), 12345.0f);
    const Snapshot before = hb_snapshot_of(state, out);
    const float* in_p = t.null_arg == HbNull::In ? nullptr : in.data();
    float* out_p = t.null_arg == HbNull::Out ? nullptr : out.data();
    float* hi = t.null_arg == HbNull::HistI ? nullptr : state.hist_i.data();
    float* hq = t.null_arg == HbNull::HistQ ? nullptr : state.hist_q.data();
    int* pending = t.null_arg == HbNull::Pending ? nullptr : &state.pending;
    const float* taps = t.null_arg == HbNull::Taps ? nullptr : t.taps;
    const int got = f.complex ? b.complex_fn(in_p, t.in_len, out_p, hi, hq, pending, taps, t.taps_len)
                              : b.real_fn(in_p, t.in_len, out_p, hi, pending, taps, t.taps_len);
    if (got != -1 || !same_snapshot(before, hb_snapshot_of(state, out))) {
        DSD_FPRINTF(stderr, "  FAIL: %s, %s: %s returned %d (want -1) or changed the state\n", b.name, f.name, t.what,
                    got);
        return 1;
    }
    return 0;
}

static int
test_hb_invalid_calls(const HbBackend& b, const HbFilter& f) {
    const int w = hb_width(f);
    const int len = (int)f.taps.size();
    const int c = (len - 1) / 2;
    const int n_floats = 64 * w;
    const std::vector<float> in = make_samples(65, w, 0x2718u); /* one sample more than any call reads */
    const std::vector<float> even_taps = {0.25f, 0.5f, 0.5f, 0.25f};
    std::vector<HbInvalidCase> cases = {
        {"negative pending", -1, n_floats, f.taps.data(), len, HbNull::None},
        {"pending past c", c + 1, n_floats, f.taps.data(), len, HbNull::None},
        {"even taps", 1, n_floats, even_taps.data(), 4, HbNull::None},
        {"one tap", 0, n_floats, f.taps.data(), 1, HbNull::None},
        {"a negative length", c, -w, f.taps.data(), len, HbNull::None},
        {"no pending", c, n_floats, f.taps.data(), len, HbNull::Pending},
        {"no history", c, n_floats, f.taps.data(), len, HbNull::HistI},
        {"no taps", c, n_floats, f.taps.data(), len, HbNull::Taps},
        {"no input for 64 samples", c, n_floats, f.taps.data(), len, HbNull::In},
        {"no output for a call that makes some", c, n_floats, f.taps.data(), len, HbNull::Out},
    };
    if (f.complex) {
        cases.push_back(HbInvalidCase{"half a complex sample", c, n_floats + 1, f.taps.data(), len, HbNull::None});
        cases.push_back(HbInvalidCase{"no Q history", c, n_floats, f.taps.data(), len, HbNull::HistQ});
    }
    int rc = 0;
    for (const HbInvalidCase& t : cases) {
        rc |= hb_invalid_call(b, f, in, t);
    }
    return rc;
}

/* A call of no samples is valid and changes nothing, with no input or output buffer; a warm-up call (no output yet)
 * needs no output buffer and still takes its samples. */
static int
test_hb_empty_and_warmup_calls(const HbBackend& b, const HbFilter& f) {
    const int w = hb_width(f);
    const int c = ((int)f.taps.size() - 1) / 2;
    int rc = 0;
    HbRun state = hb_warmed(b, f);
    const std::vector<float> no_out;
    const Snapshot before = hb_snapshot_of(state, no_out);
    int got = hb_apply(b, f, nullptr, 0, nullptr, state.hist_i.data(), state.hist_q.data(), &state.pending);
    if (got != 0 || !same_snapshot(before, hb_snapshot_of(state, no_out))) {
        DSD_FPRINTF(stderr, "  FAIL: %s, %s: a call of no samples returned %d (want 0) or changed the state\n", b.name,
                    f.name, got);
        rc = 1;
    }
    HbRun fresh;
    hb_start(f, 16, &fresh);
    const std::vector<float> in = make_samples(c, w, 0x1618u);
    got = hb_apply(b, f, in.data(), c, nullptr, fresh.hist_i.data(), fresh.hist_q.data(), &fresh.pending);
    const size_t hist_len = fresh.hist_i.size();
    const float last_i = fresh.hist_i[hist_len - 1U];
    if (got != 0 || fresh.pending != c || off_by_more((double)last_i, (double)in[(size_t)(c - 1) * (size_t)w], 0.0)) {
        DSD_FPRINTF(stderr,
                    "  FAIL: %s, %s: a %d-sample warm-up call with no output buffer returned %d, pending %d (want 0, "
                    "%d, its samples in the history)\n",
                    b.name, f.name, c, got, fresh.pending, c);
        rc = 1;
    }
    return rc;
}

/* ---------------- Half-band: exact-size buffers ---------------- */

namespace {
/* One call's buffers: NaN-fenced inside vectors, or exact heap allocations for the sanitizers. */
struct HbBuffers {
    std::vector<float> in_buf, out_buf, hi_buf, hq_buf;
    std::unique_ptr<float[]> in_exact, out_exact, hi_exact, hq_exact;
    float* in = nullptr;
    float* out = nullptr;
    float* hi = nullptr;
    float* hq = nullptr;
};
} // namespace

static void
hb_make_buffers(int fenced, int pad, size_t in_floats, size_t out_floats, size_t hist_len, HbBuffers* bufs) {
    const float nan = std::nanf("");
    if (fenced) {
        bufs->in_buf.assign(in_floats + 2U * (size_t)pad, nan);
        bufs->out_buf.assign(out_floats + 2U * (size_t)pad, nan);
        bufs->hi_buf.assign(hist_len + 2U * (size_t)pad, nan);
        bufs->hq_buf.assign(hist_len + 2U * (size_t)pad, nan);
        bufs->in = bufs->in_buf.data() + pad;
        bufs->out = bufs->out_buf.data() + pad;
        bufs->hi = bufs->hi_buf.data() + pad;
        bufs->hq = bufs->hq_buf.data() + pad;
        return;
    }
    bufs->in_exact.reset(new float[in_floats > 0U ? in_floats : 1U]);
    bufs->out_exact.reset(new float[out_floats > 0U ? out_floats : 1U]);
    bufs->hi_exact.reset(new float[hist_len]);
    bufs->hq_exact.reset(new float[hist_len]);
    bufs->in = bufs->in_exact.get();
    bufs->out = bufs->out_exact.get();
    bufs->hi = bufs->hi_exact.get();
    bufs->hq = bufs->hq_exact.get();
}

/* Whether the fences around @p bufs still hold only NaN (nothing was written outside a buffer). */
static int
hb_fences_intact(const HbBuffers& bufs, int pad) {
    const std::vector<float>* all[] = {&bufs.in_buf, &bufs.out_buf, &bufs.hi_buf, &bufs.hq_buf};
    for (const std::vector<float>* buf : all) {
        for (size_t k = 0; k < (size_t)pad; k++) {
            if (!std::isnan((*buf)[k]) || !std::isnan((*buf)[buf->size() - 1U - k])) {
                return 0;
            }
        }
    }
    return 1;
}

/* One call of @p n_in samples after @p warm_len samples of warm-up from a reset, into buffers of exactly the size the
 * call needs. */
static int
hb_exact_call(const HbBackend& b, const HbFilter& f, int fenced, int warm_len, int n_in) {
    const int w = hb_width(f);
    const int len = (int)f.taps.size();
    const size_t hist_len = (size_t)len - 1U;
    const int kFence = 16;
    const std::vector<float> x = make_samples(warm_len + n_in, w, 0x9999u + (uint32_t)(n_in * 31 + warm_len));
    const std::vector<double> ref = hb_reference_of(x, f);
    HbRun warm;
    hb_run_split(b, f, std::vector<float>(x.begin(), x.begin() + (ptrdiff_t)warm_len * w), Split{"warm", {warm_len}, 0},
                 &warm);
    const int want = hb_formula_outputs(warm.pending, n_in, len);
    HbBuffers bufs;
    hb_make_buffers(fenced, kFence, (size_t)n_in * (size_t)w, (size_t)want * (size_t)w, hist_len, &bufs);
    if (n_in > 0) {
        DSD_MEMCPY(bufs.in, x.data() + (size_t)warm_len * (size_t)w, (size_t)n_in * (size_t)w * sizeof(float));
    }
    DSD_MEMCPY(bufs.hi, warm.hist_i.data(), hist_len * sizeof(float));
    DSD_MEMCPY(bufs.hq, warm.hist_q.data(), hist_len * sizeof(float));
    int pending = warm.pending;
    const int got = hb_apply(b, f, bufs.in, n_in, bufs.out, bufs.hi, bufs.hq, &pending);
    char what[200];
    DSD_SNPRINTF(what, sizeof(what), "%s, %s, %s buffers, N=%d at pending %d", b.name, f.name,
                 fenced ? "NaN-fenced" : "exact heap", n_in, warm.pending);
    if (got != want * w || pending != warm.pending + n_in - 2 * want) {
        DSD_FPRINTF(stderr, "  FAIL: %s: returned %d, pending %d; want %d, pending %d\n", what, got, pending, want * w,
                    warm.pending + n_in - 2 * want);
        return 1;
    }
    for (size_t k = 0; k < (size_t)want * (size_t)w; k++) {
        const size_t j = (size_t)warm.produced * (size_t)w + k;
        if (off_by_more((double)bufs.out[k], ref[j], kRefTol)) {
            DSD_FPRINTF(stderr, "  FAIL: %s: output %zu is %.9g, the reference %.9g\n", what, k / (size_t)w,
                        (double)bufs.out[k], ref[j]);
            return 1;
        }
    }
    if (fenced && !hb_fences_intact(bufs, kFence)) {
        DSD_FPRINTF(stderr, "  FAIL: %s: wrote outside a buffer\n", what);
        return 1;
    }
    return 0;
}

/* Every block size from 0 to three filter spans and more, at warm-up and steady pending counts, so every vector step's
 * last load meets the end of its buffer somewhere: NaN fences first (no write outside a buffer, and a read outside one
 * would turn an output NaN), then exact heap allocations, for the sanitizers. The heap pass runs only when the fenced
 * one passed, so a decimator that writes too far is reported, not run into the heap. */
static int
test_hb_exact_buffers(const HbBackend& b, const HbFilter& f) {
    const int len = (int)f.taps.size();
    const int c = (len - 1) / 2;
    int rc = 0;
    for (int fenced = 1; fenced >= 0 && rc == 0; fenced--) {
        for (int warm_len : {1, c / 2 + 1, c, 3 * c + 1, 3 * c + 2}) {
            for (int n_in = 0; n_in <= 3 * len + 20; n_in++) {
                if (hb_exact_call(b, f, fenced, warm_len, n_in) != 0) {
                    rc = 1;
                    break; /* one failing size per pending says enough */
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
    /* Seeded state: every history slot and every pending count. */
    for (int st = 0; st < 10; st++) {
        for (int k = 0; k < HB_TAPS_MAX - 1; k++) {
            used.s->hb_hist_i[st][k] = 1.0f;
            used.s->hb_hist_q[st][k] = -1.0f;
        }
        *hb_pending_of(used.s, st) = 3;
    }
    for (int k = 0; k < DSD_CHANNEL_LPF_HIST_LEN; k++) {
        used.s->channel_lpf_hist_i[k] = 2.0f;
        used.s->channel_lpf_hist_q[k] = -2.0f;
    }
    *channel_pending_of(used.s) = 7;
    reset_filter_state(used.s);
    int hb_pending_left = 0;
    for (int st = 0; st < 10; st++) {
        hb_pending_left |= *hb_pending_of(used.s, st);
    }
    if (!all_zero(&used.s->hb_hist_i[0][0], sizeof(used.s->hb_hist_i) / sizeof(float))
        || !all_zero(&used.s->hb_hist_q[0][0], sizeof(used.s->hb_hist_q) / sizeof(float))
        || !all_zero(used.s->channel_lpf_hist_i, (size_t)DSD_CHANNEL_LPF_HIST_LEN)
        || !all_zero(used.s->channel_lpf_hist_q, (size_t)DSD_CHANNEL_LPF_HIST_LEN) || *channel_pending_of(used.s) != 0
        || hb_pending_left != 0) {
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

    /* Warm on the analog monitor's 16 kHz width at 78125 Hz: 219 taps, pending 109. A profile change at the same rate
       keeps the state, and the tap count it changes reconciles through pending. Onto the 12K5 profile, whose design
       does not fit its 144-tap cap there and runs the 63-tap fallback (a typed digital row's profile on an analog
       session at the Airspy's rate), 10 samples make 109 + 10 - 31 = 88 outputs and leave 31 pending; back onto the
       219-tap analog design, 10 samples make none and leave 41. */
    configure_raw(s, 78125, DSD_CH_LPF_PROFILE_WIDE, 0);
    s->analog_family = 1;
    s->analog_demod = DSD_ANALOG_DEMOD_FM;
    s->channel_lpf_width_hz = 16000;
    run_block(s, warm);
    const int warm_taps = s->channel_lpf_plan_taps_len;
    const int warm_pending = *channel_pending_of(s);
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    run_block(s, small);
    const int fewer_taps = s->channel_lpf_plan_taps_len;
    const int fewer_len = s->result_len;
    const int fewer_pending = *channel_pending_of(s);
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    run_block(s, small);
    if (warm_taps != 219 || warm_pending != 109 || fewer_taps != 63 || fewer_len != 176 || fewer_pending != 31
        || s->channel_lpf_plan_taps_len != 219 || s->result_len != 0 || *channel_pending_of(s) != 41) {
        DSD_FPRINTF(stderr,
                    "  FAIL: profile changes at 78125 Hz: warm %d taps, pending %d; onto fewer taps %d taps, %d floats "
                    "out, pending %d; back onto more %d taps, %d floats out, pending %d; want 219, 109; 63, 176, 31; "
                    "219, 0, 41\n",
                    warm_taps, warm_pending, fewer_taps, fewer_len, fewer_pending, s->channel_lpf_plan_taps_len,
                    s->result_len, *channel_pending_of(s));
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

/* A pass-through demodulator after @p passes half-band passes to 48 kHz, with the channel filter off. */
static void
configure_hb_raw(demod_state* s, int passes) {
    s->rate_in = 48000 << passes;
    s->rate_out = 48000;
    s->mode_demod = &raw_demod;
    s->downsample_passes = passes;
    s->channel_lpf_enable = 0;
}

/* Whether two demodulators' last blocks gave the same result, exactly. */
static int
same_result(const demod_state* a, const demod_state* b) {
    return a->result_len == b->result_len && same_floats(a->result, b->result, (size_t)b->result_len);
}

/* The half-band state belongs to a pass count: a new count starts the cascade over (restore_capture_rate_settings()
 * writes the count back with no reset), and an unchanged count carries the look-ahead and phase into the next block. */
static int
test_hb_pass_count_guard(void) {
    std::printf("Testing when the pipeline keeps and clears the half-band state...\n");
    const std::vector<float> warm = make_stream(1001, 0x4444u);
    const std::vector<float> next = make_stream(777, 0x5555u);
    int rc = 0;

    /* 2 passes, then 1: the next block comes out as from a fresh 1-pass demodulator. */
    {
        DemodBox used;
        DemodBox fresh;
        if (!used.s || !fresh.s) {
            DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
            return 1;
        }
        configure_hb_raw(used.s, 2);
        run_block(used.s, warm);
        used.s->downsample_passes = 1;
        used.s->rate_in = 96000;
        configure_hb_raw(fresh.s, 1);
        run_block(used.s, next);
        run_block(fresh.s, next);
        if (!same_result(used.s, fresh.s) || *hb_pending_of(used.s, 0) != *hb_pending_of(fresh.s, 0)
            || *hb_pending_of(used.s, 1) != 0) {
            DSD_FPRINTF(stderr,
                        "  FAIL: 2 passes then 1: %d floats, stage pending %d/%d; a fresh 1-pass demodulator %d "
                        "floats, pending %d/0 (or other values)\n",
                        used.s->result_len, *hb_pending_of(used.s, 0), *hb_pending_of(used.s, 1), fresh.s->result_len,
                        *hb_pending_of(fresh.s, 0));
            rc = 1;
        }
    }

    /* 1 pass, a block with none, then 1 again: also a fresh start. */
    {
        DemodBox used;
        DemodBox fresh;
        if (!used.s || !fresh.s) {
            DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
            return 1;
        }
        configure_hb_raw(used.s, 1);
        run_block(used.s, warm);
        used.s->downsample_passes = 0;
        run_block(used.s, make_stream(10, 0x6666u));
        used.s->downsample_passes = 1;
        configure_hb_raw(fresh.s, 1);
        run_block(used.s, next);
        run_block(fresh.s, next);
        if (!same_result(used.s, fresh.s)) {
            DSD_FPRINTF(stderr, "  FAIL: 1 pass, none, then 1: %d floats, a fresh demodulator %d (or other values)\n",
                        used.s->result_len, fresh.s->result_len);
            rc = 1;
        }
    }

    /* An unchanged count: two blocks give what their concatenation gives in one, an odd block first. */
    {
        DemodBox split;
        DemodBox whole;
        if (!split.s || !whole.s) {
            DSD_FPRINTF(stderr, "  FAIL: demod_state allocation\n");
            return 1;
        }
        configure_hb_raw(split.s, 2);
        configure_hb_raw(whole.s, 2);
        run_block(split.s, warm);
        std::vector<float> got(split.s->result, split.s->result + split.s->result_len);
        run_block(split.s, next);
        got.insert(got.end(), split.s->result, split.s->result + split.s->result_len);
        std::vector<float> both(warm);
        both.insert(both.end(), next.begin(), next.end());
        run_block(whole.s, both);
        int same = (int)got.size() == whole.s->result_len;
        for (size_t k = 0; same && k < got.size(); k++) {
            same = !off_by_more((double)got[k], (double)whole.s->result[k], 1e-5);
        }
        if (!same) {
            DSD_FPRINTF(stderr,
                        "  FAIL: an unchanged count: two blocks gave %zu floats, their concatenation %d (or "
                        "other values)\n",
                        got.size(), whole.s->result_len);
            rc = 1;
        }
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
    const std::vector<HbFilter> hb_filters = make_hb_filters();
    for (const HbBackend& backend : hb_backends()) {
        std::printf("Testing the half-band decimators across block splits (%s)...\n", backend.name);
        int rc = 0;
        for (const HbFilter& filter : hb_filters) {
            rc |= test_hb_segmentation(backend, filter);
            rc |= test_hb_reset_equals_fresh(backend, filter);
            rc |= test_hb_invalid_calls(backend, filter);
            rc |= test_hb_empty_and_warmup_calls(backend, filter);
            rc |= test_hb_exact_buffers(backend, filter);
        }
        std::printf(rc == 0 ? "  PASS\n" : "  FAILED\n");
        failures += rc;
    }
    failures += test_reset_filter_state();
    failures += test_channel_state_rules();
    failures += test_hb_pass_count_guard();
    failures += test_empty_front_end_block();
    if (failures > 0) {
        std::printf("\n%d test group(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nAll tests PASSED\n");
    return 0;
}
