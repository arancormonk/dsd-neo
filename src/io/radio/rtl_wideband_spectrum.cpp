// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Wideband (capture-rate) spectrum snapshots for the graphical spectrum view.
 *
 * Separate from rtl_metrics.cpp on purpose. That module publishes a *narrow*,
 * post-decimation spectrum at the demod output rate which feeds the supervisory
 * tuner auto-gain gate (`demod_autogain_spectral_gate_ok()`); retuning its size,
 * rate, or smoothing to also serve a UI would regress gain control. This module
 * taps the block before decimation instead, costs nothing until a consumer
 * enables it, and carries the tuned center and span alongside the bins so the
 * consumer can label a frequency axis.
 *
 * Only the generic plumbing both need — a cached pffft setup and a cached Hann
 * window — is shared, through rtl_fft_cache.h, as caches each module owns an
 * instance of. Sharing the *state* would be wrong: the two run on this same
 * thread at different sizes, so one cache between them would destroy and
 * rebuild the setup on every alternating call.
 *
 * Threading: `rtl_wideband_spectrum_maybe_update()` runs on the demod thread
 * and is the only writer. Readers may call `rtl_stream_wideband_spectrum_get()`
 * from any thread; a seqlock keeps the bins consistent with the center/span
 * they were captured at, because a torn axis-vs-bins frame is visibly wrong on
 * a waterfall.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/core/wideband_spectrum.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <stddef.h>
#include <stdint.h>

#include "rtl_fft_cache.h"
#include "rtl_wideband_spectrum.h"

namespace {

/* One published size, so a consumer's buffer, its frequency axis and the FFT
 * can never disagree. See <dsd-neo/core/wideband_spectrum.h>. */
constexpr int kWbSpecN = DSD_WIDEBAND_SPECTRUM_BINS;
constexpr uint64_t kWbSpecMinPeriodNs = static_cast<uint64_t>(DSD_WIDEBAND_SPECTRUM_PERIOD_MS) * 1000ULL * 1000ULL;
/* Seqlock reads are best-effort: give the writer a few chances, then give up.
 * Reporting no frame is safe (the consumer holds its last picture); reporting a
 * torn one is not. */
constexpr int kWbSpecReadAttempts = 4;

std::atomic<int> g_wb_enabled{0};

/* Bumped by clear()/disable. A published frame is only valid while its stamped
 * generation still matches, which invalidates stale frames without needing a
 * second writer on the seqlock below. */
std::atomic<uint32_t> g_wb_clear_gen{0};

/* Published frame. Written only by the demod thread, guarded by g_wb_seq.
 * The payload is relaxed-atomic so the seqlock is race-free by the memory
 * model as well as in practice; on every supported target these lower to plain
 * loads and stores. */
std::atomic<uint32_t> g_wb_seq{0}; /* odd => writer in progress */
std::atomic<float> g_wb_pub_db[kWbSpecN];
std::atomic<int> g_wb_pub_n{0};
std::atomic<uint32_t> g_wb_pub_center_hz{0};
std::atomic<uint32_t> g_wb_pub_span_hz{0};
std::atomic<uint32_t> g_wb_pub_gen{0};
/* Serial number of the published frame. A consumer polls on its own clock, so
 * without this it cannot tell a new frame from a re-read of the one it already
 * drew — and a waterfall would scroll duplicate rows. Kept in the payload
 * rather than inferred from g_wb_seq so frame identity does not depend on how
 * the lock happens to count. */
std::atomic<uint32_t> g_wb_pub_serial{0};

#ifdef DSD_NEO_TEST_HOOKS
/* Makes every read attempt observe a writer landing inside it. A torn read is
 * otherwise a race no single-threaded test can stage, and "what the reader does
 * when it never gets a clean copy" is the whole question. */
std::atomic<int> g_wb_test_tear{0};
#endif

/* Demod-thread-private state (never touched by readers). */
uint64_t g_wb_last_publish_ns = 0;
float g_wb_ema[kWbSpecN];
int g_wb_ema_valid = 0;
uint32_t g_wb_seen_clear_gen = 0;
uint32_t g_wb_next_serial = 1;

dsd_io::FftSetupCache g_wb_fft_setup;
dsd_io::HannWindowCache<kWbSpecN> g_wb_hann;

void
wb_bump_clear_gen(void) {
    g_wb_clear_gen.fetch_add(1, std::memory_order_acq_rel);
}

/**
 * @brief Window @p n complex pairs starting at @p start into @p z, DC removed.
 *
 * The mean is subtracted before windowing because the residual DC of an RTL
 * front end is large enough to bury the middle of the display in a spike that
 * is not a signal.
 */
void
wb_window_block(const float* iq_interleaved, int start, int n, const float* hann, float* z) {
    double sum_i = 0.0;
    double sum_q = 0.0;
    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        sum_i += static_cast<double>(iq_interleaved[idx]);
        sum_q += static_cast<double>(iq_interleaved[idx + 1]);
    }
    const float mean_i = static_cast<float>(sum_i / static_cast<double>(n));
    const float mean_q = static_cast<float>(sum_q / static_cast<double>(n));

    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        const float w = hann[k];
        z[static_cast<size_t>(k) << 1] = w * (iq_interleaved[idx] - mean_i);
        z[(static_cast<size_t>(k) << 1) + 1] = w * (iq_interleaved[idx + 1] - mean_q);
    }
}

/**
 * @brief Convert @p z to dB in display order and smooth it into the EMA.
 *
 * fftshift as it goes: bin 0 is center - span/2 and bin n/2 is DC, which is the
 * order the published frame promises. The EMA is what keeps a waterfall from
 * flickering between frames; it is seeded rather than blended on the first
 * frame after a clear, so a retune shows the new band immediately.
 */
void
wb_fold_into_ema(const float* z, int n) {
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
}

void
wb_publish(const float* db, uint32_t center_hz, uint32_t span_hz, uint32_t gen, uint32_t serial) {
    const uint32_t s = g_wb_seq.load(std::memory_order_relaxed);
    g_wb_seq.store(s + 1u, std::memory_order_release); /* odd: writer active */
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < kWbSpecN; i++) {
        g_wb_pub_db[i].store(db[i], std::memory_order_relaxed);
    }
    g_wb_pub_n.store(kWbSpecN, std::memory_order_relaxed);
    g_wb_pub_center_hz.store(center_hz, std::memory_order_relaxed);
    g_wb_pub_span_hz.store(span_hz, std::memory_order_relaxed);
    g_wb_pub_gen.store(gen, std::memory_order_relaxed);
    g_wb_pub_serial.store(serial, std::memory_order_relaxed);
    g_wb_seq.store(s + 2u, std::memory_order_release); /* even: frame complete */
}

} // namespace

/**
 * @brief Fold one capture-rate I/Q block into the published wideband spectrum.
 *
 * See rtl_wideband_spectrum.h. Skips blocks shorter than the FFT size rather
 * than zero-padding them, so the published span always reflects a full analysis
 * window.
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

    const int n = kWbSpecN;
    const int pairs = len_interleaved >> 1;
    if (pairs < n) {
        return;
    }

    const uint32_t gen = g_wb_clear_gen.load(std::memory_order_acquire);
    if (gen != g_wb_seen_clear_gen) {
        g_wb_seen_clear_gen = gen;
        g_wb_ema_valid = 0;
    }

    const float* hann = g_wb_hann.get(n);
    if (!hann) {
        return;
    }

    /* Analyse the most recent n pairs of the block. */
    alignas(16) static float z[2 * kWbSpecN];
    wb_window_block(iq_interleaved, pairs - n, n, hann, z);
    if (!g_wb_fft_setup.forward(n, z)) {
        return;
    }
    wb_fold_into_ema(z, n);

    wb_publish(g_wb_ema, center_freq_hz, capture_rate_hz, gen, g_wb_next_serial);
    /* Serial 0 is "no frame", so skip it on the wrap. */
    g_wb_next_serial = (g_wb_next_serial == UINT32_MAX) ? 1u : (g_wb_next_serial + 1u);
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

/** @brief Make every read attempt tear; see rtl_wideband_spectrum.h. */
void
rtl_wideband_spectrum_test_set_tear(int on) {
    g_wb_test_tear.store(on ? 1 : 0, std::memory_order_relaxed);
}
#endif

namespace {

/** @brief Everything published alongside a frame's bins. */
struct WbFrameHeader {
    uint32_t center_hz = 0;
    uint32_t span_hz = 0;
    uint32_t gen = 0;
    uint32_t serial = 0;
    int bins = 0;
};

/**
 * @brief Copy the published frame and its header under the seqlock.
 *
 * @param out_db Destination for the bins; must hold kWbSpecN floats.
 * @return true when one publish was captured whole. False means every attempt
 *         had the writer land inside it — the caller must report no frame
 *         rather than pass on bins that may straddle two publishes.
 */
bool
wb_read_frame(float* out_db, WbFrameHeader* out) {
    for (int attempt = 0; attempt < kWbSpecReadAttempts; attempt++) {
        const uint32_t s1 = g_wb_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) {
            continue; /* writer mid-frame */
        }
        int n = g_wb_pub_n.load(std::memory_order_relaxed);
        n = (n < 0) ? 0 : ((n > kWbSpecN) ? kWbSpecN : n);
        for (int i = 0; i < n; i++) {
            out_db[i] = g_wb_pub_db[i].load(std::memory_order_relaxed);
        }
        out->bins = n;
        out->center_hz = g_wb_pub_center_hz.load(std::memory_order_relaxed);
        out->span_hz = g_wb_pub_span_hz.load(std::memory_order_relaxed);
        out->gen = g_wb_pub_gen.load(std::memory_order_relaxed);
        out->serial = g_wb_pub_serial.load(std::memory_order_relaxed);
#ifdef DSD_NEO_TEST_HOOKS
        if (g_wb_test_tear.load(std::memory_order_relaxed) != 0) {
            g_wb_seq.fetch_add(2u, std::memory_order_release);
        }
#endif
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_wb_seq.load(std::memory_order_relaxed) == s1) {
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * @brief Copy the latest wideband spectrum frame.
 *
 * See the doc comment on the declaration in <dsd-neo/io/rtl_stream_c.h>.
 */
extern "C" int
rtl_stream_wideband_spectrum_get(float* out_db, int max_bins, uint32_t* out_center_freq_hz, uint32_t* out_span_hz,
                                 uint32_t* out_frame_serial) {
    if (!out_db) {
        return 0;
    }
    /* Every frame is the full DSD_WIDEBAND_SPECTRUM_BINS wide. Writing a prefix
     * of one into a smaller buffer would hand back the low end of the span
     * labelled as the whole of it, so a short buffer is refused instead. */
    if (max_bins < kWbSpecN) {
        return 0;
    }
    if (g_wb_enabled.load(std::memory_order_relaxed) == 0) {
        return 0;
    }

    /* Staged, not read straight into the caller's buffer. Every rejection below
     * returns 0, and a consumer reading that as "no new frame, keep the picture I
     * have" would then be drawing a picture this function had already scribbled a
     * torn or stale-generation copy over — against the axis of the frame before
     * it. The buffer is only touched once the frame is known to be whole. */
    float staged[kWbSpecN];
    WbFrameHeader header;
    /* A copy the writer ran through is not a frame: its bins may straddle two
     * publishes, and the axis it came with may belong to either. */
    if (!wb_read_frame(staged, &header) || header.bins <= 0) {
        return 0;
    }
    /* Retune / disable since this frame was published: report no data rather
     * than mislabelled bins. */
    if (header.gen != g_wb_clear_gen.load(std::memory_order_acquire)) {
        return 0;
    }
    for (int i = 0; i < header.bins; i++) {
        out_db[i] = staged[i];
    }
    if (out_center_freq_hz) {
        *out_center_freq_hz = header.center_hz;
    }
    if (out_span_hz) {
        *out_span_hz = header.span_hz;
    }
    if (out_frame_serial) {
        *out_frame_serial = header.serial;
    }
    return header.bins;
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
