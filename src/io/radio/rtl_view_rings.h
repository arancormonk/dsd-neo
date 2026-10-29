// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_VIEW_RINGS_H
#define DSD_NEO_IO_RADIO_RTL_VIEW_RINGS_H

#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inputs of a view-ring SNR fallback estimate besides the ring contents.
 *
 * `ted_sps` places the eye symbol centers and, with `rate_out`,
 * `channel_profile` and `tetra_chain`, sets the estimator bias
 * (dsd_snr_bias_*_db()).
 */
typedef struct {
    int ted_sps;
    int rate_out;
    int channel_profile;
    int tetra_chain;
} rtl_view_snr_inputs;

/** @brief Empty the constellation ring (retune), so stale points stop counting. */
void rtl_view_constellation_clear(void);

/**
 * @brief Append decimated I/Q pairs of one demod block to the constellation ring.
 *
 * Demod thread. Keeps every `sps_hint`-th pair (4 when the hint is outside 1..64).
 */
void rtl_view_constellation_append(const float* iq, int len, int sps_hint);

/** @brief Copy up to `max_points` pairs, oldest first; returns the pairs copied. */
int rtl_view_constellation_copy(float* out_xy, int max_points);

/** @brief Empty the eye ring (retune). */
void rtl_view_eye_clear(void);

/** @brief Append the I channel of one demod block to the eye ring (demod thread). */
void rtl_view_eye_append_i_chan(const float* iq_interleaved, int len_interleaved);

/** @brief Copy up to `max_samples` eye samples, oldest first; returns the samples copied. */
int rtl_view_eye_copy(float* out, int max_samples);

/**
 * @brief SNR fallbacks over the view rings (see rtl_stream_estimate_snr_*()).
 *
 * Each returns exactly what a fresh estimate over the current ring contents and
 * `in` returns, but recomputes only when a ring write batch has completed since
 * the calling thread's last estimate or `in` changed: callers poll these once
 * per symbol while the rings change once per demod block. The cache is per
 * thread, so the decoder thread and a UI snapshot thread never share one.
 *
 * Writers bracket every append and clear as one batch. An estimate that ran
 * while a batch was in flight is returned (it read the rings with the same
 * relaxed, racy-by-design semantics as before) but not cached.
 */
double rtl_view_snr_c4fm_eye(const rtl_view_snr_inputs* in);
double rtl_view_snr_gfsk_eye(const rtl_view_snr_inputs* in);
double rtl_view_snr_qpsk_const(const rtl_view_snr_inputs* in);

#ifdef DSD_NEO_TEST_HOOKS
/** @brief The same estimates, always recomputed and never touching the cache. */
double rtl_view_snr_c4fm_eye_uncached(const rtl_view_snr_inputs* in);
double rtl_view_snr_gfsk_eye_uncached(const rtl_view_snr_inputs* in);
double rtl_view_snr_qpsk_const_uncached(const rtl_view_snr_inputs* in);

/** @brief How many full estimates (cached or not) this process has run. */
uint64_t rtl_view_snr_test_compute_count(void);

/**
 * @brief Make every estimate observe a write batch landing inside its read.
 *
 * A batch racing the read is a two-thread event no single-threaded test can
 * stage; with this on, an estimate must still be returned but never cached.
 */
void rtl_view_snr_test_set_tear(int on);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_VIEW_RINGS_H */
