// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Reusable pffft setup and Hann window caches for the radio spectrum taps.
 *
 * Both spectrum producers — the narrow post-decimation one in rtl_metrics.cpp
 * and the wideband one in rtl_wideband_spectrum.cpp — need the same two things
 * on every block: a pffft setup for their transform size and a Hann window of
 * that length, neither of which is worth rebuilding per call.
 *
 * The caches are objects rather than function-local statics precisely so the
 * two producers do not share one. They run on the same thread at different
 * sizes, so a single shared cache would destroy and rebuild the setup on every
 * alternating call — slower than not caching at all.
 */

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_FFT_CACHE_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_FFT_CACHE_H_

#include <cmath>
#include <pffft.h>

namespace dsd_io {

/**
 * @brief A pffft complex setup, rebuilt only when the transform size changes.
 *
 * Never frees the setup it holds. The instances are file-scope and outlive
 * every caller, and destroying one at static-destruction time could race a
 * demod thread that has not been joined yet — the same reason the
 * function-local statics this replaces were also never torn down.
 *
 * The transform is a method rather than a setup handed back to the caller, so
 * this header is the one place in the project that names pffft — which is what
 * the no-direct-third-party-include rule asks for, and it kept the exception
 * list from growing a line per spectrum producer.
 */
class FftSetupCache {
  public:
    /**
     * @brief Forward complex transform of @p n points, in place.
     *
     * @param n Transform size in complex points.
     * @param inout 2*n interleaved floats, 16-byte aligned; overwritten with the
     *              ordered spectrum. Untouched when the size is rejected.
     * @return false when pffft has no setup for @p n.
     */
    bool
    forward(int n, float* inout) {
        PFFFT_Setup* setup = get(n);
        if (setup == nullptr || inout == nullptr) {
            return false;
        }
        pffft_transform_ordered(setup, inout, inout, nullptr, PFFFT_FORWARD);
        return true;
    }

  private:
    /** @brief Setup for @p n points, or nullptr when pffft rejects the size. */
    PFFFT_Setup*
    get(int n) {
        if (m_setup != nullptr && m_n == n) {
            return m_setup;
        }
        if (m_setup != nullptr) {
            pffft_destroy_setup(m_setup);
            m_setup = nullptr;
            m_n = 0;
        }
        m_setup = pffft_new_setup(n, PFFFT_COMPLEX);
        /* Left at 0 on failure so the next call retries rather than handing
         * back a null setup it thinks is current. */
        m_n = (m_setup != nullptr) ? n : 0;
        return m_setup;
    }

    PFFFT_Setup* m_setup = nullptr;
    int m_n = 0;
};

/**
 * @brief A Hann window of up to @p MaxN points, recomputed only on a size change.
 *
 * @tparam MaxN Largest window the owner will ask for; longer requests are
 *              rejected rather than overrunning the buffer.
 */
template <int MaxN>
class HannWindowCache {
  public:
    /** @brief Window of @p n points, or nullptr when @p n does not fit. */
    const float*
    get(int n) {
        if (n <= 0 || n > MaxN) {
            return nullptr;
        }
        if (m_n == n) {
            return m_window;
        }
        if (n == 1) {
            m_window[0] = 1.0f;
        } else {
            const float scale = 2.0f * static_cast<float>(M_PI) / static_cast<float>(n - 1);
            for (int i = 0; i < n; i++) {
                m_window[i] = 0.5f * (1.0f - cosf(scale * static_cast<float>(i)));
            }
        }
        m_n = n;
        return m_window;
    }

  private:
    alignas(16) float m_window[MaxN] = {};
    int m_n = 0;
};

} // namespace dsd_io

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_FFT_CACHE_H_ */
