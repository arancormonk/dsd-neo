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

#include <stddef.h>

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

/**
 * @brief Format a run of raw dibits as a "Debug Demod -Sync" hex line.
 *
 * Packs 4 dibits per byte (MSB first) with no burst alignment implied; used
 * by the --dmr-debug-unsynced dump while hunting for sync.
 *
 * @param out Output buffer (NUL-terminated on return)
 * @param out_size Output buffer size in bytes
 * @param dibits Raw dibit values (low 2 bits used)
 * @param count Number of dibits to format (multiple of 4; remainder dropped)
 * @return Number of characters written, or 0 on invalid arguments
 */
size_t dmr_debug_format_unsynced(char* out, size_t out_size, const int* dibits, size_t count);

/**
 * @brief Append `count` dibits as [XX] hex bytes (4 dibits per byte, MSB first).
 *
 * Shared tail for the DMR debug-dump formatters (unsynced and RC burst).
 * Truncates safely: the buffer is always NUL-terminated on return.
 *
 * @param out Output buffer
 * @param out_size Output buffer size in bytes
 * @param pos Write position to append at (returned unchanged if out of range)
 * @param dibits Raw dibit values (low 2 bits used)
 * @param count Number of dibits to append (multiple of 4; remainder dropped)
 * @return New write position (may exceed what fit if truncated)
 */
size_t dmr_debug_append_dibit_bytes(char* out, size_t out_size, size_t pos, const int* dibits, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_DMR_SYNC_H_ */
