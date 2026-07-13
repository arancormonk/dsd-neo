// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR resample-on-sync implementation.
 *
 * Implements SDRTrunk-style resample-on-sync for DMR to improve first-frame
 * decode accuracy. This module provides:
 * - Threshold initialization through the generic sync calibrator
 * - CACH re-digitization with corrected thresholds
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* ─────────────────────────────────────────────────────────────────────────────
 * Generic Warm-Start Calibration
 * ───────────────────────────────────────────────────────────────────────────── */

/* ─────────────────────────────────────────────────────────────────────────────
 * CACH Resampling (Re-digitization)
 *
 * After sync detection, re-digitize the CACH and message prefix symbols
 * using the corrected thresholds from the sync pattern.
 * ───────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Digitize a symbol using current thresholds.
 * @param state Decoder state with thresholds
 * @param symbol Raw symbol value
 * @return Dibit value (0-3)
 */
static int
dmr_digitize_symbol(const dsd_state* state, float symbol) {
    /* 4-level slicer using calibrated thresholds */
    if (symbol > state->center) {
        if (symbol > state->umid) {
            return 1; /* +3 */
        } else {
            return 0; /* +1 */
        }
    } else {
        if (symbol < state->lmid) {
            return 3; /* -3 */
        } else {
            return 2; /* -1 */
        }
    }
}

void
dmr_resample_cach(dsd_opts* opts, dsd_state* state, int sync_sample_offset) {
    (void)opts; /* Currently unused */

    if (state == NULL || state->symbol_history == NULL) {
        return;
    }

    if (state->dmr_payload_buf == NULL || state->dmr_payload_p == NULL) {
        return;
    }

    /* Check we have enough history */
    if (state->symbol_history_count < DMR_RESAMPLE_SYMBOLS + DMR_SYNC_SYMBOLS) {
        return;
    }

    /* Re-digitize 66 symbols before the sync pattern (CACH + message prefix).
     * The sync pattern is at symbols [-24..-1] from current position.
     * CACH + prefix is at symbols [-90..-25] from current position.
     *
     * Note: sync_sample_offset is relative to current head, typically 0.
     */
    int start_offset = sync_sample_offset - DMR_SYNC_SYMBOLS - DMR_RESAMPLE_SYMBOLS + 1;

    /* The DMR payload buffer is a rolling append-only buffer; dmr_payload_p points
     * one past the most recently written dibit. At sync detection time, the
     * current (last) sync symbol is at (dmr_payload_p - 1). Therefore, the 66
     * symbols preceding the sync pattern map to:
     *   [dmr_payload_p - (DMR_SYNC_SYMBOLS + DMR_RESAMPLE_SYMBOLS) .. dmr_payload_p - DMR_SYNC_SYMBOLS - 1]
     */
    int* out_dibits = state->dmr_payload_p - (DMR_SYNC_SYMBOLS + DMR_RESAMPLE_SYMBOLS);

    for (int i = 0; i < DMR_RESAMPLE_SYMBOLS; i++) {
        /* Get symbol from history */
        float symbol = dsd_symbol_history_get_back(state, -(start_offset + i));

        /* Re-digitize with corrected thresholds */
        int dibit = dmr_digitize_symbol(state, symbol);

        out_dibits[i] = dibit;
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Main Resample-on-Sync Entry Point
 * ───────────────────────────────────────────────────────────────────────────── */

int
dmr_resample_on_sync(dsd_opts* opts, dsd_state* state) {
    if (state == NULL) {
        return -1;
    }

    /* Check sample history is available */
    if (state->symbol_history == NULL || state->symbol_history_count < DMR_SYNC_SYMBOLS) {
        return -1;
    }

    /* 1. Initialize thresholds from the most recent DMR sync symbols. */
    (void)dsd_sync_warm_start_thresholds_outer_only(opts, state, DMR_SYNC_SYMBOLS);

    /* 2. Re-digitize CACH even if warm-start is disabled or rejects a degenerate sync. */
    dmr_resample_cach(opts, state, 0);

    return 0;
}
