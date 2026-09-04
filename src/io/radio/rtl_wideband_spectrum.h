// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_WIDEBAND_SPECTRUM_H
#define DSD_NEO_IO_RADIO_RTL_WIDEBAND_SPECTRUM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fold one capture-rate I/Q block into the published wideband spectrum.
 *
 * Called from the demod thread on every block. Returns immediately when no
 * consumer has enabled the feature, and otherwise publishes at most one frame
 * per ~66 ms (~15 FPS).
 *
 * @param iq_interleaved  Interleaved float I/Q at capture rate.
 * @param len_interleaved Number of floats (2x the complex pair count).
 * @param capture_rate_hz Sample rate of the block; becomes the published span.
 * @param center_freq_hz  Tuned center frequency the block was captured at.
 */
void rtl_wideband_spectrum_maybe_update(const float* iq_interleaved, int len_interleaved, uint32_t capture_rate_hz,
                                        uint32_t center_freq_hz);

/**
 * @brief Invalidate the published frame (retune, size change, or disable).
 *
 * The getter reports "no data" until the demod thread publishes again, so a
 * consumer never labels post-retune bins with the previous center frequency.
 */
void rtl_wideband_spectrum_clear(void);

#ifdef DSD_NEO_TEST_HOOKS
/** @brief Clear the publish throttle so the next update publishes immediately. */
void rtl_wideband_spectrum_test_reset_throttle(void);

/**
 * @brief Make every seqlock read attempt observe a writer landing inside it.
 *
 * A torn read is a race between two threads, which no single-threaded test can
 * stage — and what the reader does when it never gets a clean copy is exactly
 * what must not regress: reporting no frame is safe, handing back a copy the
 * writer ran through is not.
 */
void rtl_wideband_spectrum_test_set_tear(int on);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_WIDEBAND_SPECTRUM_H */
