// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The per-block arithmetic both radio spectrum taps run.
 *
 * Companion to rtl_fft_cache.h: that header holds the state the narrow tap in
 * rtl_metrics.cpp and the wideband tap in rtl_wideband_spectrum.cpp each keep an
 * instance of, this one holds the arithmetic they share outright. Both remove
 * the block's DC before windowing, and both fold the transform into a display
 * array with the same fftshift and the same dB conversion; only the smoothing
 * weight and where the result lands differ.
 *
 * Worth one copy rather than two because the two taps are read against each
 * other — the wideband one exists precisely because the narrow one's sizing and
 * smoothing may not be retuned — so a silent divergence in the bin order or the
 * dB reference would show up as the two views disagreeing about the same signal,
 * with nothing in either file to point at.
 */

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_SPECTRUM_KERNELS_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_SPECTRUM_KERNELS_H_

#include <cmath>
#include <stddef.h>

namespace dsd_io {

/** @brief Mean I and Q of a block, the DC offset removed before windowing. */
struct IqBlockMean {
    float i = 0.0f;
    float q = 0.0f;
};

/**
 * @brief Mean of @p n complex pairs of @p iq_interleaved starting at @p start.
 *
 * Accumulated in double: an RTL front end's DC offset is a large fraction of
 * full scale, and summing thousands of samples of it in float loses the low
 * bits of exactly the quantity being measured.
 *
 * @return Zero for a non-positive @p n, so a caller that skips the window loop
 *         subtracts nothing rather than a NaN.
 */
inline IqBlockMean
iq_block_mean(const float* iq_interleaved, int start, int n) {
    IqBlockMean mean;
    if (iq_interleaved == nullptr || n <= 0) {
        return mean;
    }
    double sum_i = 0.0;
    double sum_q = 0.0;
    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        sum_i += static_cast<double>(iq_interleaved[idx]);
        sum_q += static_cast<double>(iq_interleaved[idx + 1]);
    }
    mean.i = static_cast<float>(sum_i / static_cast<double>(n));
    mean.q = static_cast<float>(sum_q / static_cast<double>(n));
    return mean;
}

/**
 * @brief Window @p n pairs from @p start into @p z with @p mean removed.
 *
 * The mean is subtracted before windowing because the residual DC of an RTL
 * front end is large enough to bury the middle of a display in a spike that is
 * not a signal — and, on the narrow tap, to be mistaken for the carrier the peak
 * search is looking for.
 *
 * @param hann Window of at least @p n points. Callers that analyse fewer pairs
 *             than their transform size pass the leading @p n of a longer
 *             window, and are expected to have zeroed the tail of @p z.
 * @param z Destination for 2*@p n interleaved floats.
 */
inline void
iq_window_dc_removed(const float* iq_interleaved, int start, int n, const float* hann, IqBlockMean mean, float* z) {
    if (iq_interleaved == nullptr || hann == nullptr || z == nullptr) {
        return;
    }
    for (int k = 0; k < n; k++) {
        const size_t idx = static_cast<size_t>(start + k) << 1;
        const float w = hann[k];
        z[static_cast<size_t>(k) << 1] = w * (iq_interleaved[idx] - mean.i);
        z[(static_cast<size_t>(k) << 1) + 1] = w * (iq_interleaved[idx + 1] - mean.q);
    }
}

/* A contiguous half of the shifted spectrum. Keeping the wrap and seed decisions outside the per-bin loop leaves
 * the same arithmetic on each bin, including on builds that preserve IEEE rounding. */
template <bool Seed>
inline void
spectrum_fold_db_span(const float* z, int n, float new_weight, const float* prev_db, float* out_db) {
    const float eps = 1e-12f;
    const float old_weight = 1.0f - new_weight;
    for (int k = 0; k < n; k++) {
        const float re = z[static_cast<size_t>(k) << 1];
        const float im = z[(static_cast<size_t>(k) << 1) + 1];
        const float db = 10.0f * log10f(re * re + im * im + eps);
        out_db[k] = Seed ? db : (new_weight * db + old_weight * prev_db[k]);
    }
}

/**
 * @brief Fold an ordered transform into @p out_db as smoothed dB, fftshifted.
 *
 * fftshift as it goes: bin 0 becomes center - rate/2 and bin n/2 becomes DC,
 * which is the order both taps' consumers expect of the arrays they read.
 *
 * @param new_weight Share of the new frame in the exponential average, in
 *                   [0, 1]. Smoothing is what stops a display flickering between
 *                   frames; the two taps weigh it differently because one feeds
 *                   a waterfall and the other a gain gate.
 * @param seed True to write the new frame outright instead of blending. The
 *             first frame has nothing to blend with, and after a retune the old
 *             band must not bleed into the new one.
 * @param prev_db The previous frame, @p n floats, read unless @p seed is set.
 * @param out_db Destination of @p n floats; may be @p prev_db (in place).
 */
inline void
spectrum_fold_db(const float* z, int n, float new_weight, bool seed, const float* prev_db, float* out_db) {
    if (z == nullptr || prev_db == nullptr || out_db == nullptr || n <= 0) {
        return;
    }
#if defined(__FAST_MATH__) || defined(_MSC_VER)
    /* Keep the vector-libm loop's shape under relaxed floating-point rules: splitting it changes contraction and
       rounding on those builds. The strict-math path below keeps the same arithmetic without a wrap per bin. */
    const float eps = 1e-12f;
    const float old_weight = 1.0f - new_weight;
    for (int k = 0; k < n; k++) {
        int kk = k + (n >> 1);
        if (kk >= n) {
            kk -= n;
        }
        const float re = z[static_cast<size_t>(kk) << 1];
        const float im = z[(static_cast<size_t>(kk) << 1) + 1];
        const float db = 10.0f * log10f(re * re + im * im + eps);
        out_db[k] = seed ? db : (new_weight * db + old_weight * prev_db[k]);
    }
#else
    const int half = n >> 1;
    const int first = n - half;
    if (seed) {
        spectrum_fold_db_span<true>(z + 2U * (size_t)half, first, new_weight, prev_db, out_db);
        spectrum_fold_db_span<true>(z, half, new_weight, prev_db + first, out_db + first);
    } else {
        spectrum_fold_db_span<false>(z + 2U * (size_t)half, first, new_weight, prev_db, out_db);
        spectrum_fold_db_span<false>(z, half, new_weight, prev_db + first, out_db + first);
    }
#endif
}

} // namespace dsd_io

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_SPECTRUM_KERNELS_H_ */
