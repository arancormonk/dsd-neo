// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Wideband (capture-rate) spectrum snapshots for the graphical spectrum view.
 *
 * The FFT plumbing here — cached pffft setup, cached Hann window, dB
 * conversion, fftshift, EMA smoothing — deliberately mirrors rtl_metrics.cpp
 * rather than sharing it. That module publishes a *narrow*, post-decimation
 * spectrum at the demod output rate which feeds the supervisory tuner
 * auto-gain gate (`demod_autogain_spectral_gate_ok()`); retuning its size,
 * rate, or smoothing to also serve a UI would regress gain control. This
 * module taps the block before decimation instead, costs nothing until a
 * consumer enables it, and carries the tuned center and span alongside the
 * bins so the consumer can label a frequency axis.
 *
 * Threading: `rtl_wideband_spectrum_maybe_update()` runs on the demod thread
 * and is the only writer. Readers may call `rtl_stream_wideband_spectrum_get()`
 * from any thread; a seqlock keeps the bins consistent with the center/span
 * they were captured at, because a torn axis-vs-bins frame is visibly wrong on
 * a waterfall.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <pffft.h>
#include <stddef.h>
#include <stdint.h>

#include "rtl_wideband_spectrum.h"

namespace {

constexpr int kWbSpecMinN = 256;
constexpr int kWbSpecMaxN = 2048;
constexpr int kWbSpecDefaultN = 1024;
/* ~15 FPS: enough for a readable waterfall, cheap enough to ride the demod thread. */
constexpr uint64_t kWbSpecMinPeriodNs = 66ULL * 1000ULL * 1000ULL;
/* Seqlock reads are best-effort: give the writer a few chances, then take what we have. */
constexpr int kWbSpecReadAttempts = 4;

std::atomic<int> g_wb_enabled{0};
std::atomic<int> g_wb_n{kWbSpecDefaultN};

/* Bumped by clear()/set_size()/disable. A published frame is only valid while
 * its stamped generation still matches, which invalidates stale frames without
 * needing a second writer on the seqlock below. */
std::atomic<uint32_t> g_wb_clear_gen{0};

/* Published frame. Written only by the demod thread, guarded by g_wb_seq.
 * The payload is relaxed-atomic so the seqlock is race-free by the memory
 * model as well as in practice; on every supported target these lower to plain
 * loads and stores. */
std::atomic<uint32_t> g_wb_seq{0}; /* odd => writer in progress */
std::atomic<float> g_wb_pub_db[kWbSpecMaxN];
std::atomic<int> g_wb_pub_n{0};
std::atomic<uint32_t> g_wb_pub_center_hz{0};
std::atomic<uint32_t> g_wb_pub_span_hz{0};
std::atomic<uint32_t> g_wb_pub_gen{0};

/* Demod-thread-private state (never touched by readers). */
uint64_t g_wb_last_publish_ns = 0;
float g_wb_ema[kWbSpecMaxN];
int g_wb_ema_valid = 0;
uint32_t g_wb_seen_clear_gen = 0;

PFFFT_Setup*
wb_cached_setup(int n) {
    static PFFFT_Setup* setup = nullptr;
    static int setup_n = 0;
    if (!setup || setup_n != n) {
        if (setup) {
            pffft_destroy_setup(setup);
            setup = nullptr;
            setup_n = 0;
        }
        setup = pffft_new_setup(n, PFFFT_COMPLEX);
        setup_n = setup ? n : 0;
    }
    return setup;
}

const float*
wb_hann_window(int n) {
    alignas(16) static float window[kWbSpecMaxN];
    static int window_n = 0;
    if (window_n != n) {
        if (n <= 1) {
            window[0] = 1.0f;
        } else {
            const float scale = 2.0f * static_cast<float>(M_PI) / static_cast<float>(n - 1);
            for (int i = 0; i < n; i++) {
                window[i] = 0.5f * (1.0f - cosf(scale * static_cast<float>(i)));
            }
        }
        window_n = n;
    }
    return window;
}

int
wb_clamp_size(int n) {
    if (n < kWbSpecMinN) {
        n = kWbSpecMinN;
    }
    if (n > kWbSpecMaxN) {
        n = kWbSpecMaxN;
    }
    int p = kWbSpecMinN;
    while (p < n) {
        p <<= 1;
    }
    if (p > kWbSpecMaxN) {
        p = kWbSpecMaxN;
    }
    return p;
}

void
wb_bump_clear_gen(void) {
    g_wb_clear_gen.fetch_add(1, std::memory_order_acq_rel);
}

void
wb_publish(const float* db, int n, uint32_t center_hz, uint32_t span_hz, uint32_t gen) {
    const uint32_t s = g_wb_seq.load(std::memory_order_relaxed);
    g_wb_seq.store(s + 1u, std::memory_order_release); /* odd: writer active */
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < n; i++) {
        g_wb_pub_db[i].store(db[i], std::memory_order_relaxed);
    }
    g_wb_pub_n.store(n, std::memory_order_relaxed);
    g_wb_pub_center_hz.store(center_hz, std::memory_order_relaxed);
    g_wb_pub_span_hz.store(span_hz, std::memory_order_relaxed);
    g_wb_pub_gen.store(gen, std::memory_order_relaxed);
    g_wb_seq.store(s + 2u, std::memory_order_release); /* even: frame complete */
}

} // namespace

/**
 * @brief Fold one capture-rate I/Q block into the published wideband spectrum.
 *
 * See rtl_wideband_spectrum.h. Skips blocks shorter than the configured FFT
 * size rather than zero-padding them, so the published span always reflects a
 * full analysis window.
 */
void
rtl_wideband_spectrum_maybe_update(const float* iq_interleaved, int len_interleaved, uint32_t capture_rate_hz,
                                   uint32_t center_freq_hz) {
    if (g_wb_enabled.load(std::memory_order_relaxed) == 0) {
        return;
    }
    if (!iq_interleaved || len_interleaved < 2 || capture_rate_hz == 0) {
        return;
    }

    const uint64_t now_ns = dsd_time_monotonic_ns();
    if (g_wb_last_publish_ns != 0 && (now_ns - g_wb_last_publish_ns) < kWbSpecMinPeriodNs) {
        return;
    }

    const int n = wb_clamp_size(g_wb_n.load(std::memory_order_relaxed));
    const int pairs = len_interleaved >> 1;
    if (pairs < n) {
        return;
    }

    const uint32_t gen = g_wb_clear_gen.load(std::memory_order_acquire);
    if (gen != g_wb_seen_clear_gen) {
        g_wb_seen_clear_gen = gen;
        g_wb_ema_valid = 0;
    }

    PFFFT_Setup* setup = wb_cached_setup(n);
    if (!setup) {
        return;
    }

    /* Analyse the most recent n pairs of the block. */
    alignas(16) static float z[2 * kWbSpecMaxN];
    const int start = pairs - n;

    double sum_i = 0.0;
    double sum_q = 0.0;
    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        sum_i += static_cast<double>(iq_interleaved[idx]);
        sum_q += static_cast<double>(iq_interleaved[idx + 1]);
    }
    const float mean_i = static_cast<float>(sum_i / static_cast<double>(n));
    const float mean_q = static_cast<float>(sum_q / static_cast<double>(n));

    const float* hann = wb_hann_window(n);
    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        const float w = hann[k];
        z[static_cast<size_t>(k) << 1] = w * (iq_interleaved[idx] - mean_i);
        z[(static_cast<size_t>(k) << 1) + 1] = w * (iq_interleaved[idx + 1] - mean_q);
    }

    pffft_transform_ordered(setup, z, z, nullptr, PFFFT_FORWARD);

    /* fftshift into display order: bin 0 is center - span/2, bin n/2 is DC. */
    const float eps = 1e-12f;
    const bool seed = (g_wb_ema_valid == 0);
    for (int k = 0; k < n; k++) {
        int kk = k + (n >> 1);
        if (kk >= n) {
            kk -= n;
        }
        const float re = z[static_cast<size_t>(kk) << 1];
        const float im = z[(static_cast<size_t>(kk) << 1) + 1];
        const float db = 10.0f * log10f(re * re + im * im + eps);
        g_wb_ema[k] = seed ? db : (0.6f * db + 0.4f * g_wb_ema[k]);
    }
    g_wb_ema_valid = 1;

    wb_publish(g_wb_ema, n, center_freq_hz, capture_rate_hz, gen);
    g_wb_last_publish_ns = now_ns;
}

/** @brief Invalidate the published frame; see rtl_wideband_spectrum.h. */
void
rtl_wideband_spectrum_clear(void) {
    wb_bump_clear_gen();
}

#ifdef DSD_NEO_TEST_HOOKS
/** @brief Clear the publish throttle so the next update publishes immediately. */
void
rtl_wideband_spectrum_test_reset_throttle(void) {
    g_wb_last_publish_ns = 0;
}
#endif

/**
 * @brief Copy the latest wideband spectrum frame.
 *
 * See the doc comment on the declaration in <dsd-neo/io/rtl_stream_c.h>.
 */
extern "C" int
rtl_stream_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz, uint32_t* out_span_hz) {
    if (!out_db || max_bins <= 0) {
        return 0;
    }
    if (g_wb_enabled.load(std::memory_order_relaxed) == 0) {
        return 0;
    }

    const int cap = (max_bins < kWbSpecMaxN) ? max_bins : kWbSpecMaxN;
    int copied = 0;
    uint32_t center_hz = 0;
    uint32_t span_hz = 0;
    uint32_t gen = 0;
    for (int attempt = 0; attempt < kWbSpecReadAttempts; attempt++) {
        const uint32_t s1 = g_wb_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) {
            continue; /* writer mid-frame */
        }
        int n = g_wb_pub_n.load(std::memory_order_relaxed);
        if (n > cap) {
            n = cap;
        }
        if (n < 0) {
            n = 0;
        }
        for (int i = 0; i < n; i++) {
            out_db[i] = g_wb_pub_db[i].load(std::memory_order_relaxed);
        }
        center_hz = g_wb_pub_center_hz.load(std::memory_order_relaxed);
        span_hz = g_wb_pub_span_hz.load(std::memory_order_relaxed);
        gen = g_wb_pub_gen.load(std::memory_order_relaxed);
        copied = n;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_wb_seq.load(std::memory_order_relaxed) == s1) {
            break;
        }
    }

    if (copied <= 0) {
        return 0;
    }
    /* Retune / resize / disable since this frame was published: report no data
     * rather than mislabelled bins. */
    if (gen != g_wb_clear_gen.load(std::memory_order_acquire)) {
        return 0;
    }
    if (out_center_freq_hz) {
        *out_center_freq_hz = center_hz;
    }
    if (out_span_hz) {
        *out_span_hz = span_hz;
    }
    return copied;
}

/**
 * @brief Set the wideband FFT size.
 *
 * See the doc comment on the declaration in <dsd-neo/io/rtl_stream_c.h>.
 */
extern "C" int
rtl_stream_wideband_spectrum_set_size(int n) {
    const int p = wb_clamp_size(n);
    if (g_wb_n.exchange(p, std::memory_order_relaxed) != p) {
        wb_bump_clear_gen();
    }
    return p;
}

/** @brief Get the current wideband FFT size in bins. */
extern "C" int
rtl_stream_wideband_spectrum_get_size(void) {
    return wb_clamp_size(g_wb_n.load(std::memory_order_relaxed));
}

/** @brief Enable or disable wideband spectrum production. */
extern "C" void
rtl_stream_wideband_spectrum_set_enabled(int on) {
    const int v = on ? 1 : 0;
    if (g_wb_enabled.exchange(v, std::memory_order_relaxed) != v && v == 0) {
        /* Drop the frame so a later re-enable never shows pre-close bins. */
        wb_bump_clear_gen();
    }
}

/** @brief Return 1 when wideband spectrum production is enabled. */
extern "C" int
rtl_stream_wideband_spectrum_enabled(void) {
    return g_wb_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}
