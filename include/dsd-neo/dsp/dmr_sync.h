// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR resample-on-sync support.
 *
 * Implements SDRTrunk-style resample-on-sync for DMR to improve first-frame
 * decode accuracy. When sync is detected, this module:
 * 1. Initializes symbol thresholds from the detected sync symbols
 * 2. Resamples CACH and message prefix with corrected timing
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_DMR_SYNC_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_DMR_SYNC_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct dsd_opts;
struct dsd_state;

/* ─────────────────────────────────────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────────────────────────────────────── */

#define DMR_SYNC_SYMBOLS     24 /* Sync pattern length in symbols */
#define DMR_CACH_DIBITS      12 /* CACH length (6 dibits × 2 for interleave) */
#define DMR_RESAMPLE_SYMBOLS 66 /* CACH + message prefix to resample */

/* ─────────────────────────────────────────────────────────────────────────────
 * CACH Resampling
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Resample CACH and message prefix after sync detection.
 *
 * Goes back through sample history and re-digitizes the 66 symbols before
 * the sync pattern using calibrated timing and thresholds. Overwrites the
 * stale dibits in dmr_payload_buf.
 *
 * @param opts Decoder options
 * @param state Decoder state
 * @param sync_sample_offset Sample offset where sync was detected
 */
void dmr_resample_cach(struct dsd_opts* opts, struct dsd_state* state, int sync_sample_offset);

/**
 * @brief Perform full resample-on-sync sequence for DMR.
 *
 * Called after DMR sync detection. Performs:
 * 1. Initialize thresholds from the latest sync symbols
 * 2. Re-digitize CACH with the resulting thresholds
 *
 * @param opts Decoder options
 * @param state Decoder state
 * @return 0 on success, -1 if sample history unavailable
 */
int dmr_resample_on_sync(struct dsd_opts* opts, struct dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_DMR_SYNC_H_ */
