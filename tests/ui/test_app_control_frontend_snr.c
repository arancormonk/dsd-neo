// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The SNR estimator selection every frontend shares.
 *
 * The Qt frontend used to pick between the CQPSK and C4FM estimators on
 * cqpsk_enable and publish the result unconditionally, so a GFSK stream read the
 * C4FM estimator and an input with no demodulator behind it (UDP, a file, rtl_tcp)
 * rendered the estimator's -100 dB no-reading sentinel as though it were a
 * measurement. Selection now lives here, and reports whether anything read at all.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <dsd-neo/app_control/frontend.h>

#include "dsd-neo/core/safe_api.h"

/* The value every estimator reports until it has a measurement. */
#define SENTINEL (-100.0)

static dsd_frontend_metrics
nothing_reported(void) {
    dsd_frontend_metrics m;
    DSD_MEMSET(&m, 0, sizeof(m));
    m.snr_c4fm_db = SENTINEL;
    m.snr_c4fm_eye_db = SENTINEL;
    m.snr_cqpsk_db = SENTINEL;
    m.snr_gfsk_db = SENTINEL;
    m.snr_gfsk_eye_db = SENTINEL;
    m.snr_qpsk_const_db = SENTINEL;
    return m;
}

static void
expect_valid(const char* what, dsd_frontend_snr_readout got, double want_db) {
    if (!got.valid || fabs(got.snr_db - want_db) > 1e-6) {
        (void)DSD_FPRINTF(stderr, "%s: valid=%d snr=%.6f want %.6f\n", what, got.valid, got.snr_db, want_db);
    }
    assert(got.valid == 1);
    assert(fabs(got.snr_db - want_db) <= 1e-6);
}

static void
expect_invalid(const char* what, dsd_frontend_snr_readout got) {
    if (got.valid) {
        (void)DSD_FPRINTF(stderr, "%s: reported %.6f as a reading\n", what, got.snr_db);
    }
    assert(got.valid == 0);
}

/* Each modulation reads its own estimator, and only its own. */
static void
test_selection_is_by_modulation(void) {
    dsd_frontend_metrics m = nothing_reported();
    m.snr_c4fm_db = 11.0;
    m.snr_cqpsk_db = 22.0;
    m.snr_gfsk_db = 33.0;

    expect_valid("c4fm", dsd_app_frontend_snr_for_mod(&m, 0), 11.0);
    expect_valid("qpsk", dsd_app_frontend_snr_for_mod(&m, 1), 22.0);
    expect_valid("gfsk", dsd_app_frontend_snr_for_mod(&m, 2), 33.0);

    /* An rf_mod no frontend defines is read as C4FM rather than as nothing. */
    expect_valid("unknown mod", dsd_app_frontend_snr_for_mod(&m, 99), 11.0);
}

/*
 * A GFSK stream must not borrow the C4FM estimator.
 *
 * This is the Qt regression: with a C4FM reading present and the GFSK estimator
 * silent, selecting on cqpsk_enable reported the C4FM number for an NXDN or -mg DMR
 * stream, which is a reading of a different demodulator.
 */
static void
test_gfsk_does_not_borrow_c4fm(void) {
    dsd_frontend_metrics m = nothing_reported();
    m.snr_c4fm_db = 18.0;

    expect_invalid("gfsk with only a c4fm reading", dsd_app_frontend_snr_for_mod(&m, 2));
}

/* The eye/constellation fallbacks stand in, per modulation. */
static void
test_fallbacks(void) {
    dsd_frontend_metrics m = nothing_reported();
    m.snr_c4fm_eye_db = 5.5;
    expect_valid("c4fm eye", dsd_app_frontend_snr_for_mod(&m, 0), 5.5);

    m = nothing_reported();
    m.snr_gfsk_eye_db = 7.25;
    expect_valid("gfsk eye", dsd_app_frontend_snr_for_mod(&m, 2), 7.25);

    m = nothing_reported();
    m.snr_qpsk_const_db = 9.5;
    expect_valid("qpsk constellation", dsd_app_frontend_snr_for_mod(&m, 1), 9.5);

    /* QPSK reaches past its own fallback to the better FSK estimator, because a
     * stream the UI calls QPSK can be carrying C4FM. */
    m = nothing_reported();
    m.snr_c4fm_db = 4.25;
    m.snr_gfsk_db = 6.75;
    expect_valid("qpsk cross-modulation", dsd_app_frontend_snr_for_mod(&m, 1), 6.75);
}

/* The sentinel is a reading of nothing, and so is the threshold itself. */
static void
test_sentinel_is_not_a_reading(void) {
    dsd_frontend_metrics m = nothing_reported();
    for (int rf_mod = 0; rf_mod < 3; rf_mod++) {
        expect_invalid("silent estimators", dsd_app_frontend_snr_for_mod(&m, rf_mod));
    }

    /* -50 dB is the boundary, and is itself invalid: no real decode sits there. */
    m = nothing_reported();
    m.snr_c4fm_db = -50.0;
    expect_invalid("threshold", dsd_app_frontend_snr_for_mod(&m, 0));
    m.snr_c4fm_eye_db = -49.5;
    expect_valid("just above threshold", dsd_app_frontend_snr_for_mod(&m, 0), -49.5);
}

/* An invalid result still carries the estimator's own value, for logging. */
static void
test_invalid_carries_the_primary_value(void) {
    dsd_frontend_metrics m = nothing_reported();
    m.snr_gfsk_db = -73.0;

    dsd_frontend_snr_readout got = dsd_app_frontend_snr_for_mod(&m, 2);
    expect_invalid("gfsk silent", got);
    assert(fabs(got.snr_db - (-73.0)) <= 1e-6);
}

/* No metrics at all is a clean invalid, not a crash. */
static void
test_null_metrics(void) {
    expect_invalid("null metrics", dsd_app_frontend_snr_for_mod(NULL, 0));
}

int
main(void) {
    test_selection_is_by_modulation();
    test_gfsk_does_not_borrow_c4fm();
    test_fallbacks();
    test_sentinel_is_not_a_reading();
    test_invalid_carries_the_primary_value();
    test_null_metrics();
    printf("APP_CONTROL_FRONTEND_SNR: OK\n");
    return 0;
}
