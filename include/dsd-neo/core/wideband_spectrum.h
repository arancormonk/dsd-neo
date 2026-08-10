// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shape of a published wideband spectrum frame.
 *
 * The producer lives in io and the consumers are frontends, which are not
 * allowed to include each other's headers — so without a shared home these
 * numbers get copied into the FFT, the poll timer and the waterfall history
 * separately, and a change to one silently mismatches the others. A buffer
 * sized off a stale copy is the failure that matters: it truncates a frame the
 * producer still labels with the whole span.
 */
#ifndef DSD_NEO_CORE_WIDEBAND_SPECTRUM_H
#define DSD_NEO_CORE_WIDEBAND_SPECTRUM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bins in every published wideband spectrum frame.
 *
 * Fixed rather than negotiable: at the ~1.536 MHz capture span this is ~1.5 kHz
 * per bin, finer than any channel plan the decoder handles, and a consumer that
 * cannot be handed a smaller frame needs no resampling path to get the axis
 * right. A receiving buffer must hold at least this many floats.
 */
#define DSD_WIDEBAND_SPECTRUM_BINS      1024

/**
 * @brief Interval between published frames, in milliseconds.
 *
 * ~15 FPS: enough for a readable waterfall, cheap enough to ride the demod
 * thread. A consumer polling faster only re-copies a frame the producer has not
 * replaced yet.
 */
#define DSD_WIDEBAND_SPECTRUM_PERIOD_MS 66

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_CORE_WIDEBAND_SPECTRUM_H */
