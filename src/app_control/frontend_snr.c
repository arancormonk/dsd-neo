// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Which SNR estimator a modulation reads, shared by every frontend.
 *
 * Kept apart from frontend.c, which reaches into the radio backend: this is pure
 * selection over metrics the caller already holds, so it links anywhere -- including
 * into frontends and tests that have no radio at all.
 */

#include <dsd-neo/app_control/frontend.h>

/* An estimator with no measurement reports a large negative sentinel (-100 dB from
 * the metrics defaults). Anything at or below this is "nothing to show" rather than a
 * reading: a real decode never sits down here. Matches FRONTEND_SNR_INVALID_DB in
 * frontend.c, which decides when the fallbacks are worth fetching at all. */
#define FRONTEND_SNR_INVALID_DB (-50.0)

static int
snr_is_valid(double snr_db) {
    return snr_db > FRONTEND_SNR_INVALID_DB;
}

/**
 * @brief @p primary if it reported, else @p fallback, else invalid.
 *
 * An invalid result still carries @p primary rather than a placeholder: it is what
 * the estimator for this modulation actually said, so a caller that logs the value
 * alongside the invalid flag sees the sentinel it came from.
 */
static dsd_frontend_snr_readout
snr_pick(double primary, double fallback) {
    dsd_frontend_snr_readout out;
    out.valid = 1;
    if (snr_is_valid(primary)) {
        out.snr_db = primary;
        return out;
    }
    if (snr_is_valid(fallback)) {
        out.snr_db = fallback;
        return out;
    }
    out.snr_db = primary;
    out.valid = 0;
    return out;
}

dsd_frontend_snr_readout
dsd_app_frontend_snr_for_mod(const dsd_frontend_metrics* metrics, int rf_mod) {
    dsd_frontend_snr_readout out;
    out.snr_db = FRONTEND_SNR_INVALID_DB;
    out.valid = 0;
    if (!metrics) {
        return out;
    }

    if (rf_mod == 2) {
        return snr_pick(metrics->snr_gfsk_db, metrics->snr_gfsk_eye_db);
    }
    if (rf_mod != 1) {
        return snr_pick(metrics->snr_c4fm_db, metrics->snr_c4fm_eye_db);
    }

    /* QPSK reaches furthest. Past the CQPSK estimator and the constellation it will
     * take the better of the two FSK estimators, because a stream the UI is still
     * calling QPSK can be carrying C4FM -- a P25 phase 1 control channel, say. */
    out = snr_pick(metrics->snr_cqpsk_db, metrics->snr_qpsk_const_db);
    if (out.valid) {
        return out;
    }
    const double fsk = (metrics->snr_c4fm_db > metrics->snr_gfsk_db) ? metrics->snr_c4fm_db : metrics->snr_gfsk_db;
    if (snr_is_valid(fsk)) {
        out.snr_db = fsk;
        out.valid = 1;
    }
    return out;
}
