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

#include <cstddef>
#include <dsd-neo/dsp/firdes.h>
#include <pffft.h>

namespace dsd_io {

/**
 * @brief A pffft complex setup and its work buffer, rebuilt only when the
 * transform size changes.
 *
 * pffft takes a transform's scratch (8 bytes per point) from the caller's
 * stack unless it is given a work buffer, and a search transform of
 * DSD_SEARCH_MAX_FFT points would need 512 KiB of it: a whole macOS secondary
 * thread stack, half a Windows one. The work buffer lives on the heap beside
 * the setup instead.
 *
 * Has no destructor. The spectrum caches are file-scope and outlive every
 * caller, and destroying one at static-destruction time could race a demod
 * thread that has not been joined yet — the same reason the function-local
 * statics this replaces were also never torn down. An owner that outlives its
 * worker frees both buffers with release().
 *
 * The transform is a method rather than a setup handed back to the caller, so
 * this header is the one place in the project that names pffft — which is what
 * the no-direct-third-party-include rule asks for, and it kept the exception
 * list from growing a line per spectrum producer.
 */
class FftSetupCache {
  public:
    FftSetupCache() = default;
    /* Owns a raw PFFFT_Setup* that get() destroys on a size change, so a copy would
     * leave two owners of one setup and the second would destroy freed memory. The
     * instances are file-scope and never copied; this is what keeps it that way. */
    FftSetupCache(const FftSetupCache&) = delete;
    FftSetupCache& operator=(const FftSetupCache&) = delete;

    /** Release only after the owning worker has stopped using this cache. */
    void
    release() {
        if (m_setup) {
            pffft_destroy_setup(m_setup);
        }
        pffft_aligned_free(m_work);
        m_setup = nullptr;
        m_work = nullptr;
        m_n = 0;
    }

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
        const PFFFT_Setup* setup = get(n);
        if (setup == nullptr || inout == nullptr) {
            return false;
        }
        pffft_transform_ordered(setup, inout, inout, m_work, PFFFT_FORWARD);
        return true;
    }

  private:
    /** @brief Setup for @p n points with its work buffer, or nullptr when pffft
     * rejects the size or either allocation fails. */
    PFFFT_Setup*
    get(int n) {
        if (m_setup != nullptr && m_n == n) {
            return m_setup;
        }
        release();
        m_setup = pffft_new_setup(n, PFFFT_COMPLEX);
        if (m_setup == nullptr) {
            return nullptr;
        }
        /* A complex transform's scratch is 2*n floats, aligned for pffft's SIMD. */
        m_work = static_cast<float*>(pffft_aligned_malloc(sizeof(float) * 2 * static_cast<size_t>(n)));
        if (m_work == nullptr) {
            /* Never transform without it: pffft would fall back to the stack. */
            release();
            return nullptr;
        }
        /* Set only once both exist, so a failure leaves 0 and the next call
         * retries rather than handing back a setup it thinks is current. */
        m_n = n;
        return m_setup;
    }

    PFFFT_Setup* m_setup = nullptr;
    float* m_work = nullptr;
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
            /* dsd_window_build() divides by (ntaps - 1), so the degenerate length
             * is answered here rather than handed to it. */
            m_window[0] = 1.0f;
        } else {
            /* The project's own window builder, not a third copy of the formula:
             * every other windowed operation in the tree goes through it, and a
             * correction made there (periodic vs symmetric, an edge case) has to
             * reach the spectrum taps too or the two views pick up a different
             * sidelobe floor than everything else that measures the same signal. */
            dsd_window_build(DSD_WIN_HANN, n, m_window);
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
