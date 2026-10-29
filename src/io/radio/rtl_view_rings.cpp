// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Constellation and eye view rings, and the SNR fallbacks estimated over them.
 *
 * Threading: the demod thread appends one batch per block; a retune clears
 * from whichever thread runs it. Readers (the decoder thread, frontend
 * snapshots) copy from any thread. The samples are relaxed atomics, so a copy
 * racing a batch may mix old and new samples, which display and estimation
 * accept.
 *
 * Every append and clear runs between view_ring_write_begin() and
 * view_ring_write_end(): a writer count, raised for the batch, and a batch
 * counter, bumped when it completes. An estimate that saw no writer active
 * and the same batch count on both sides of its copy read a ring no batch
 * touched, so it is a pure function of that batch count and its inputs, and
 * the calling thread caches it under that key. Callers poll the estimates per
 * symbol; the rings change per block.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/dsp/snr_bias.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

#include <algorithm>

#include "rtl_view_rings.h"

namespace {

constexpr int kConstMaxPairs = 8192;
constexpr int kEyeMax = 16384;
/* Samples (eye) and pairs (constellation) an estimate reads from its ring. */
constexpr int kSnrMaxSamples = 4096;

struct ViewRingWrites {
    std::atomic<uint32_t> writers{0}; /* batches in flight */
    std::atomic<uint64_t> batches{0}; /* batches completed */
};

struct SnrCacheEntry {
    int valid;
    uint64_t batches;
    rtl_view_snr_inputs in;
    double value;
};

/* Per-thread copy buffers: the estimates run on more than one thread. */
struct SnrScratch {
    std::vector<float> samples;
    std::vector<float> quant;
};

} // namespace

/* ---------------- Constellation capture (simple lock-free ring) ---------------- */

/* Relaxed atomics: single demod-thread writer, main-thread reader. Tearing
 * across samples is acceptable for display/estimation; atomics keep the
 * unsynchronized access well-defined. Relaxed ops compile to plain moves. */
static std::atomic<float> g_const_xy[kConstMaxPairs * 2];
static std::atomic<int> g_const_head{0}; /* pairs written [0..kConstMaxPairs-1], wraps */
static ViewRingWrites g_const_writes;

/* ---------------- Eye diagram capture (I-channel of complex baseband) ---------------- */
/* Relaxed atomics for the same reason as the constellation ring above. */
static std::atomic<float> g_eye_buf[kEyeMax];
static std::atomic<int> g_eye_head{0}; /* samples written [0..kEyeMax-1], wraps */
static ViewRingWrites g_eye_writes;

#ifdef DSD_NEO_TEST_HOOKS
static std::atomic<uint64_t> g_snr_test_computes{0};
static std::atomic<int> g_snr_test_tear{0};
#endif

/* The release fence orders the raised count before every sample store of the
 * batch: a reader whose copy saw one of them sees the count after its acquire
 * fence. */
static void
view_ring_write_begin(ViewRingWrites* w) {
    w->writers.fetch_add(1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

/* Bump the batch count before dropping the writer count, so a reader that sees
 * the dropped count (acquire) also sees the new batch count. */
static void
view_ring_write_end(ViewRingWrites* w) {
    w->batches.fetch_add(1u, std::memory_order_release);
    w->writers.fetch_sub(1u, std::memory_order_release);
}

extern "C" void
rtl_view_constellation_clear(void) {
    view_ring_write_begin(&g_const_writes);
    for (int k = 0; k < kConstMaxPairs * 2; k++) {
        g_const_xy[k].store(0.0f, std::memory_order_relaxed);
    }
    g_const_head.store(0, std::memory_order_relaxed);
    view_ring_write_end(&g_const_writes);
}

extern "C" void
rtl_view_constellation_append(const float* iq, int len, int sps_hint) {
    if (!iq || len < 2) {
        return;
    }
    view_ring_write_begin(&g_const_writes);
    int N = len >> 1;                                              /* complex samples */
    int stride = (sps_hint >= 1 && sps_hint <= 64) ? sps_hint : 4; /* rough decimation */
    for (int n = 0; n < N; n += stride) {
        float i = iq[(size_t)(n << 1) + 0];
        float q = iq[(size_t)(n << 1) + 1];
        int h = g_const_head.load(std::memory_order_relaxed);
        g_const_xy[(size_t)(h << 1) + 0].store(i, std::memory_order_relaxed);
        g_const_xy[(size_t)(h << 1) + 1].store(q, std::memory_order_relaxed);
        h++;
        if (h >= kConstMaxPairs) {
            h = 0;
        }
        g_const_head.store(h, std::memory_order_relaxed);
    }
    view_ring_write_end(&g_const_writes);
}

extern "C" int
rtl_view_constellation_copy(float* out_xy, int max_points) {
    if (!out_xy || max_points <= 0) {
        return 0;
    }
    int head = g_const_head.load(std::memory_order_relaxed); /* snapshot */
    int n = (max_points < kConstMaxPairs) ? max_points : kConstMaxPairs;
    int start = head;
    for (int k = 0; k < n; k++) {
        int idx = (start + k) % kConstMaxPairs;
        out_xy[(size_t)(k << 1) + 0] = g_const_xy[(size_t)(idx << 1) + 0].load(std::memory_order_relaxed);
        out_xy[(size_t)(k << 1) + 1] = g_const_xy[(size_t)(idx << 1) + 1].load(std::memory_order_relaxed);
    }
    return n;
}

extern "C" void
rtl_view_eye_clear(void) {
    view_ring_write_begin(&g_eye_writes);
    for (int k = 0; k < kEyeMax; k++) {
        g_eye_buf[k].store(0.0f, std::memory_order_relaxed);
    }
    g_eye_head.store(0, std::memory_order_relaxed);
    view_ring_write_end(&g_eye_writes);
}

extern "C" void
rtl_view_eye_append_i_chan(const float* iq_interleaved, int len_interleaved) {
    if (!iq_interleaved || len_interleaved < 2) {
        return;
    }
    view_ring_write_begin(&g_eye_writes);
    int N = len_interleaved >> 1; /* complex samples */
    for (int n = 0; n < N; n++) {
        float i = iq_interleaved[(size_t)(n << 1) + 0];
        int h = g_eye_head.load(std::memory_order_relaxed);
        g_eye_buf[h].store(i, std::memory_order_relaxed);
        h++;
        if (h >= kEyeMax) {
            h = 0;
        }
        g_eye_head.store(h, std::memory_order_relaxed);
    }
    view_ring_write_end(&g_eye_writes);
}

extern "C" int
rtl_view_eye_copy(float* out, int max_samples) {
    if (!out || max_samples <= 0) {
        return 0;
    }
    int head = g_eye_head.load(std::memory_order_relaxed);
    int n = (max_samples < kEyeMax) ? max_samples : kEyeMax;
    int start = head;
    for (int k = 0; k < n; k++) {
        int idx = (start + k) % kEyeMax;
        out[k] = g_eye_buf[idx].load(std::memory_order_relaxed);
    }
    return n;
}

static SnrScratch&
snr_scratch(void) {
    static thread_local SnrScratch scratch;
    if (scratch.samples.empty()) {
        scratch.samples.resize((size_t)kSnrMaxSamples * 2);
        scratch.quant.resize((size_t)kSnrMaxSamples);
    }
    return scratch;
}

/* ---------------- Eye-based SNR estimation (C4FM fallback) ---------------- */
static int
snr_eye_phase_in_window(int phase, int c1, int c2, int win) {
    return (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
}

static int
snr_eye_downsample_for_quantiles(const float* src, int src_count, float* dst, int dst_capacity) {
    if (!src || !dst || src_count <= 0 || dst_capacity <= 0) {
        return 0;
    }
    int step = (src_count > dst_capacity) ? (src_count / dst_capacity) : 1;
    int copied = 0;
    for (int i = 0; i < src_count && copied < dst_capacity; i += step) {
        dst[copied++] = src[i];
    }
    return copied;
}

static int
snr_eye_quartiles(float* vals, int count, float* q1, float* q2, float* q3) {
    if (!vals || count < 8 || !q1 || !q2 || !q3) {
        return 0;
    }
    int idx1 = (int)((size_t)count / 4);
    int idx2 = (int)((size_t)count / 2);
    int idx3 = (int)((size_t)(3 * (size_t)count) / 4);
    std::nth_element(vals, vals + idx2, vals + count);
    *q2 = vals[idx2];
    std::nth_element(vals, vals + idx1, vals + idx2);
    *q1 = vals[idx1];
    std::nth_element(vals + idx2 + 1, vals + idx3, vals + count);
    *q3 = vals[idx3];
    return 1;
}

static int
snr_eye_median(float* vals, int count, float* q2) {
    if (!vals || !q2 || count < 8) {
        return 0;
    }
    int idx2 = (int)((size_t)count / 2);
    std::nth_element(vals, vals + idx2, vals + count);
    *q2 = vals[idx2];
    return 1;
}

static int
snr_eye_c4fm_cluster_index(float v, float q1, float q2, float q3) {
    return (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
}

static long long
snr_eye_c4fm_collect_clusters(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q1, float q2,
                              float q3, long long cnt[4], double sum[4]) {
    for (int b = 0; b < 4; b++) {
        cnt[b] = 0;
        sum[b] = 0.0;
    }
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        int b = snr_eye_c4fm_cluster_index(v, q1, q2, q3);
        cnt[b]++;
        sum[b] += (double)v;
    }
    return cnt[0] + cnt[1] + cnt[2] + cnt[3];
}

static double
snr_eye_c4fm_noise_variance(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q1, float q2,
                            float q3, const double mu[4], long long total) {
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        int b = snr_eye_c4fm_cluster_index(v, q1, q2, q3);
        double e = (double)v - mu[b];
        nsum += e * e;
    }
    return nsum / (double)total;
}

static double
snr_eye_signal_variance_from_means(const double* mu, const long long* cnt, long long total) {
    double mu_all = 0.0;
    for (int b = 0; b < 4; b++) {
        mu_all += mu[b] * (double)cnt[b] / (double)total;
    }
    double ssum = 0.0;
    for (int b = 0; b < 4; b++) {
        double d = mu[b] - mu_all;
        ssum += (double)cnt[b] * d * d;
    }
    return ssum / (double)total;
}

static int
snr_qpsk_axis_means(const float* xy, int n, double* aI_out, double* aQ_out) {
    double sum_abs_i = 0.0;
    double sum_abs_q = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        sum_abs_i += fabs(I);
        sum_abs_q += fabs(Q);
    }
    double aI = sum_abs_i / (double)n;
    double aQ = sum_abs_q / (double)n;
    if (!(aI > 1e-9 && aQ > 1e-9)) {
        return 0;
    }
    *aI_out = aI;
    *aQ_out = aQ;
    return 1;
}

static double
snr_qpsk_error_axis(const float* xy, int n, double aI, double aQ) {
    double e2 = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aI : -aI;
        double tq = (Q >= 0.0) ? aQ : -aQ;
        double ei = I - ti;
        double eq = Q - tq;
        e2 += ei * ei + eq * eq;
    }
    return e2;
}

static double
snr_qpsk_error_diag(const float* xy, int n, double aD) {
    double e2 = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aD : -aD;
        double tq = (Q >= 0.0) ? aD : -aD;
        double ei = I - ti;
        double eq = Q - tq;
        e2 += ei * ei + eq * eq;
    }
    return e2;
}

namespace {

struct snr_eye_gfsk_window {
    int two_sps;
    int c1;
    int c2;
    int win;
    float q2;
};

} // namespace

namespace {

struct snr_eye_gfsk_sums {
    double sumL;
    double sumH;
    int cntL;
    int cntH;
};

} // namespace

static int
snr_eye_gfsk_collect_binary(const float* eb, int nfb, const snr_eye_gfsk_window* window, snr_eye_gfsk_sums* sums) {
    sums->sumL = 0.0;
    sums->sumH = 0.0;
    sums->cntL = 0;
    sums->cntH = 0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % window->two_sps;
        if (!snr_eye_phase_in_window(phase, window->c1, window->c2, window->win)) {
            continue;
        }
        float v = eb[i];
        if (v <= window->q2) {
            sums->sumL += v;
            sums->cntL++;
        } else {
            sums->sumH += v;
            sums->cntH++;
        }
    }
    return (sums->cntL > 0 && sums->cntH > 0) ? 1 : 0;
}

static double
snr_eye_gfsk_noise_variance(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q2, double muL,
                            double muH, int total) {
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        double mu = (v <= q2) ? muL : muH;
        double e = (double)v - mu;
        nsum += e * e;
    }
    return nsum / (double)total;
}

static double
snr_c4fm_eye_compute(const rtl_view_snr_inputs* in) {
    SnrScratch& scratch = snr_scratch();
    float* eb = scratch.samples.data();
    int sps_fb = in->ted_sps;
    int nfb = rtl_view_eye_copy(eb, kSnrMaxSamples);
    if (nfb <= 100 || sps_fb <= 0) {
        return -100.0;
    }
    int two_sps = 2 * sps_fb;
    int c1 = sps_fb / 2;
    int c2 = (3 * sps_fb) / 2;
    int win = sps_fb / 10;
    if (win < 1) {
        win = 1;
    }
    float* qv = scratch.quant.data();
    int mct = snr_eye_downsample_for_quantiles(eb, nfb, qv, kSnrMaxSamples);
    float q1 = 0.0f;
    float q2 = 0.0f;
    float q3 = 0.0f;
    if (!snr_eye_quartiles(qv, mct, &q1, &q2, &q3)) {
        return -100.0;
    }

    long long cnt[4] = {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    long long total = snr_eye_c4fm_collect_clusters(eb, nfb, two_sps, c1, c2, win, q1, q2, q3, cnt, sum);
    if (!(total > 50 && cnt[0] && cnt[1] && cnt[2] && cnt[3])) {
        return -100.0;
    }
    double mu[4];
    for (int b = 0; b < 4; b++) {
        mu[b] = sum[b] / (double)cnt[b];
    }
    double noise_var = snr_eye_c4fm_noise_variance(eb, nfb, two_sps, c1, c2, win, q1, q2, q3, mu, total);
    if (noise_var <= 1e-9) {
        return -100.0;
    }
    double sig_var = snr_eye_signal_variance_from_means(mu, cnt, total);
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    double bias = dsd_snr_bias_c4fm_db(in->rate_out, in->ted_sps, in->channel_profile, in->tetra_chain);
    return 10.0 * log10(sig_var / noise_var) - bias;
}

/* ---------------- Constellation-based SNR estimation (QPSK fallback) ---------------- */
static double
snr_qpsk_const_compute(const rtl_view_snr_inputs* in) {
    float* xy = snr_scratch().samples.data();
    int n = rtl_view_constellation_copy(xy, kSnrMaxSamples);
    if (n <= 64) {
        return -100.0;
    }
    double aI = 0.0;
    double aQ = 0.0;
    if (!snr_qpsk_axis_means(xy, n, &aI, &aQ)) {
        return -100.0;
    }
    double e2_axis = snr_qpsk_error_axis(xy, n, aI, aQ);
    double t2_axis = (double)n * (aI * aI + aQ * aQ);
    double aD = std::max(1e-9, 0.5 * (aI + aQ));
    double e2_diag = snr_qpsk_error_diag(xy, n, aD);
    double t2_diag = (double)n * (2.0 * aD * aD);
    double best_snr = -100.0;
    if (t2_axis > 1e-9 && e2_axis > 0.0) {
        double ratio = (e2_axis <= 1e-12) ? 1e12 : (t2_axis / e2_axis);
        best_snr = 10.0 * log10(ratio);
    }
    if (t2_diag > 1e-9 && e2_diag > 0.0) {
        double ratio_d = (e2_diag <= 1e-12) ? 1e12 : (t2_diag / e2_diag);
        double snr_d = 10.0 * log10(ratio_d);
        if (snr_d > best_snr) {
            best_snr = snr_d;
        }
    }
    double bias = dsd_snr_bias_evm_db(in->rate_out, in->ted_sps, in->channel_profile, in->tetra_chain);
    return best_snr - bias;
}

/* ---------------- Eye-based SNR estimation (GFSK fallback, 2-level) ---------------- */
static double
snr_gfsk_eye_compute(const rtl_view_snr_inputs* in) {
    SnrScratch& scratch = snr_scratch();
    float* eb = scratch.samples.data();
    int sps_fb = in->ted_sps;
    int nfb = rtl_view_eye_copy(eb, kSnrMaxSamples);
    if (nfb <= 100 || sps_fb <= 0) {
        return -100.0;
    }
    int two_sps = 2 * sps_fb;
    int c1 = sps_fb / 2;
    int c2 = (3 * sps_fb) / 2;
    int win = sps_fb / 10;
    if (win < 1) {
        win = 1;
    }
    float* qv = scratch.quant.data();
    int mct = snr_eye_downsample_for_quantiles(eb, nfb, qv, kSnrMaxSamples);
    float q2 = 0.0f;
    if (!snr_eye_median(qv, mct, &q2)) {
        return -100.0;
    }
    snr_eye_gfsk_window window = {two_sps, c1, c2, win, q2};
    snr_eye_gfsk_sums sums;
    if (!snr_eye_gfsk_collect_binary(eb, nfb, &window, &sums)) {
        return -100.0;
    }
    double muL = sums.sumL / (double)sums.cntL, muH = sums.sumH / (double)sums.cntH;
    int total = sums.cntL + sums.cntH;
    double noise_var = snr_eye_gfsk_noise_variance(eb, nfb, two_sps, c1, c2, win, q2, muL, muH, total);
    if (noise_var <= 1e-9) {
        return -100.0;
    }
    double mu_all = (muL * (double)sums.cntL + muH * (double)sums.cntH) / (double)total;
    double ssum =
        (double)sums.cntL * (muL - mu_all) * (muL - mu_all) + (double)sums.cntH * (muH - mu_all) * (muH - mu_all);
    double sig_var = ssum / (double)total;
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    double bias = dsd_snr_bias_evm_db(in->rate_out, in->ted_sps, in->channel_profile, in->tetra_chain);
    return 10.0 * log10(sig_var / noise_var) - bias;
}

/* ---------------- Per-thread estimate cache ---------------- */

typedef double (*snr_compute_fn)(const rtl_view_snr_inputs* in);

static int
snr_inputs_equal(const rtl_view_snr_inputs* a, const rtl_view_snr_inputs* b) {
    return (a->ted_sps == b->ted_sps && a->rate_out == b->rate_out && a->channel_profile == b->channel_profile
            && a->tetra_chain == b->tetra_chain)
               ? 1
               : 0;
}

static double
snr_compute_counted(snr_compute_fn compute, const rtl_view_snr_inputs* in) {
#ifdef DSD_NEO_TEST_HOOKS
    g_snr_test_computes.fetch_add(1u, std::memory_order_relaxed);
#endif
    return compute(in);
}

/*
 * A hit needs the batch count and inputs the entry was stored under: no batch
 * completed since, so the ring holds what the stored estimate read. A batch in
 * flight on a hit is a write racing the read, which may as well land after it.
 * A miss is stored only when no writer was active and the batch count held
 * across the copy; otherwise its result (which may mix batches) is returned and
 * the entry dropped, so a later call never goes back to an older ring than
 * this thread already read.
 */
static double
snr_cached(SnrCacheEntry* entry, ViewRingWrites* writes, const rtl_view_snr_inputs* in, snr_compute_fn compute) {
    if (!in) {
        return -100.0;
    }
    const uint32_t writers_before = writes->writers.load(std::memory_order_acquire);
    const uint64_t batches = writes->batches.load(std::memory_order_acquire);
    if (entry->valid && entry->batches == batches && snr_inputs_equal(&entry->in, in)) {
        return entry->value;
    }
    const double value = snr_compute_counted(compute, in);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t writers_after = writes->writers.load(std::memory_order_acquire);
    const uint64_t batches_after = writes->batches.load(std::memory_order_relaxed);
    int stable = (writers_before == 0u && writers_after == 0u && batches_after == batches) ? 1 : 0;
#ifdef DSD_NEO_TEST_HOOKS
    if (g_snr_test_tear.load(std::memory_order_relaxed) != 0) {
        stable = 0;
    }
#endif
    if (stable) {
        entry->batches = batches;
        entry->in = *in;
        entry->value = value;
        entry->valid = 1;
    } else {
        entry->valid = 0;
    }
    return value;
}

extern "C" double
rtl_view_snr_c4fm_eye(const rtl_view_snr_inputs* in) {
    static thread_local SnrCacheEntry entry = {};
    return snr_cached(&entry, &g_eye_writes, in, snr_c4fm_eye_compute);
}

extern "C" double
rtl_view_snr_gfsk_eye(const rtl_view_snr_inputs* in) {
    static thread_local SnrCacheEntry entry = {};
    return snr_cached(&entry, &g_eye_writes, in, snr_gfsk_eye_compute);
}

extern "C" double
rtl_view_snr_qpsk_const(const rtl_view_snr_inputs* in) {
    static thread_local SnrCacheEntry entry = {};
    return snr_cached(&entry, &g_const_writes, in, snr_qpsk_const_compute);
}

#ifdef DSD_NEO_TEST_HOOKS
extern "C" double
rtl_view_snr_c4fm_eye_uncached(const rtl_view_snr_inputs* in) {
    return in ? snr_compute_counted(snr_c4fm_eye_compute, in) : -100.0;
}

extern "C" double
rtl_view_snr_gfsk_eye_uncached(const rtl_view_snr_inputs* in) {
    return in ? snr_compute_counted(snr_gfsk_eye_compute, in) : -100.0;
}

extern "C" double
rtl_view_snr_qpsk_const_uncached(const rtl_view_snr_inputs* in) {
    return in ? snr_compute_counted(snr_qpsk_const_compute, in) : -100.0;
}

extern "C" uint64_t
rtl_view_snr_test_compute_count(void) {
    return g_snr_test_computes.load(std::memory_order_relaxed);
}

extern "C" void
rtl_view_snr_test_set_tear(int on) {
    g_snr_test_tear.store(on ? 1 : 0, std::memory_order_relaxed);
}
#endif
