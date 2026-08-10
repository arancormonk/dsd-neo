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

/**
 * @brief Fold an ordered transform into @p inout_db as smoothed dB, fftshifted.
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
 * @param inout_db Destination of @p n floats, read as the previous frame unless
 *                 @p seed is set.
 */
inline void
spectrum_fold_db(const float* z, int n, float new_weight, bool seed, float* inout_db) {
    if (z == nullptr || inout_db == nullptr) {
        return;
    }
    /* Never zero: a bin with no energy would otherwise be log10(0). */
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
        inout_db[k] = seed ? db : (new_weight * db + old_weight * inout_db[k]);
    }
}

} // namespace dsd_io

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_SPECTRUM_KERNELS_H_ */
