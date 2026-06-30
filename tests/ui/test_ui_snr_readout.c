// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#include <dsd-neo/app_control/frontend.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static double g_snr_c4fm = -100.0;
static double g_snr_c4fm_eye = -100.0;
static double g_snr_cqpsk = -100.0;
static double g_snr_gfsk = -100.0;
static double g_snr_gfsk_eye = -100.0;
static double g_snr_qpsk_const = -100.0;

int
dsd_app_frontend_get_metrics(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out) {
    (void)opts;
    (void)state;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->snr_c4fm_db = g_snr_c4fm;
    out->snr_c4fm_eye_db = g_snr_c4fm_eye;
    out->snr_cqpsk_db = g_snr_cqpsk;
    out->snr_gfsk_db = g_snr_gfsk;
    out->snr_gfsk_eye_db = g_snr_gfsk_eye;
    out->snr_qpsk_const_db = g_snr_qpsk_const;
    return 0;
}

int
dsd_app_frontend_get_metrics_with_snr_fallbacks(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out,
                                                unsigned int snr_fallbacks) {
    (void)snr_fallbacks;
    return dsd_app_frontend_get_metrics(opts, state, out);
}

static void
reset_fakes(void) {
    ui_snr_readout_reset_for_test();
    g_snr_c4fm = -100.0;
    g_snr_c4fm_eye = -100.0;
    g_snr_cqpsk = -100.0;
    g_snr_gfsk = -100.0;
    g_snr_gfsk_eye = -100.0;
    g_snr_qpsk_const = -100.0;
}

static void
expect_readout(const char* name, int rf_mod, double expected_snr, const char* expected_label) {
    ui_snr_readout got = ui_snr_readout_for_mod(rf_mod);
    const char* actual_label = got.mod_label ? got.mod_label : "";
    if (!got.valid || fabs(got.snr_db - expected_snr) > 1e-6 || strcmp(actual_label, expected_label) != 0) {
        (void)DSD_FPRINTF(stderr, "%s: valid=%d snr=%.6f label=%s\n", name, got.valid, got.snr_db,
                          actual_label[0] ? actual_label : "(null)");
    }
    assert(got.valid == 1);
    assert(fabs(got.snr_db - expected_snr) <= 1e-6);
    assert(strcmp(actual_label, expected_label) == 0);
}

static void
expect_invalid_readout(const char* name, int rf_mod, double expected_snr, const char* expected_label) {
    ui_snr_readout got = ui_snr_readout_for_mod(rf_mod);
    const char* actual_label = got.mod_label ? got.mod_label : "";
    if (got.valid || fabs(got.snr_db - expected_snr) > 1e-6 || strcmp(actual_label, expected_label) != 0) {
        (void)DSD_FPRINTF(stderr, "%s: valid=%d snr=%.6f label=%s\n", name, got.valid, got.snr_db,
                          actual_label[0] ? actual_label : "(null)");
    }
    assert(got.valid == 0);
    assert(fabs(got.snr_db - expected_snr) <= 1e-6);
    assert(strcmp(actual_label, expected_label) == 0);
}

static void
test_c4fm_uses_direct_snr(void) {
    reset_fakes();
    g_snr_c4fm = 18.25;

    expect_readout("c4fm-direct", 0, 18.25, "C4FM");
}

static void
test_c4fm_keeps_stable_direct_snr(void) {
    reset_fakes();
    g_snr_c4fm = 18.25;
    g_snr_c4fm_eye = 4.0;

    for (int i = 0; i < 64; i++) {
        expect_readout("c4fm-stable-direct", 0, 18.25, "C4FM");
    }
}

static void
test_gfsk_uses_direct_snr(void) {
    reset_fakes();
    g_snr_gfsk = 21.5;

    expect_readout("gfsk-direct", 2, 21.5, "GFSK");
}

static void
test_gfsk_falls_back_when_direct_snr_invalid(void) {
    reset_fakes();
    g_snr_gfsk = -100.0;
    g_snr_gfsk_eye = 7.25;

    expect_readout("gfsk-fallback", 2, 7.25, "GFSK");
}

static void
test_c4fm_snr_at_invalid_threshold_falls_back(void) {
    reset_fakes();
    g_snr_c4fm = -50.0;
    g_snr_c4fm_eye = 5.5;

    expect_readout("c4fm-threshold-fallback", 0, 5.5, "C4FM");
}

static void
test_qpsk_uses_cqpsk_snr(void) {
    reset_fakes();
    g_snr_cqpsk = 12.75;

    expect_readout("qpsk", 1, 12.75, "QPSK");
}

static void
test_qpsk_uses_constellation_fallback(void) {
    reset_fakes();
    g_snr_cqpsk = -100.0;
    g_snr_qpsk_const = 9.5;

    expect_readout("qpsk-constellation-fallback", 1, 9.5, "QPSK");
}

static void
test_qpsk_uses_best_legacy_snr_when_constellation_invalid(void) {
    reset_fakes();
    g_snr_cqpsk = -100.0;
    g_snr_qpsk_const = -100.0;
    g_snr_c4fm = 4.25;
    g_snr_gfsk = 6.75;

    expect_readout("qpsk-legacy-best-fallback", 1, 6.75, "QPSK");
}

static void
test_qpsk_reports_invalid_when_all_estimates_stale(void) {
    reset_fakes();
    g_snr_cqpsk = -100.0;
    g_snr_qpsk_const = -50.0;
    g_snr_c4fm = -50.0;
    g_snr_gfsk = -100.0;

    expect_invalid_readout("qpsk-all-invalid", 1, -100.0, "QPSK");
}

int
main(void) {
    test_c4fm_uses_direct_snr();
    test_c4fm_keeps_stable_direct_snr();
    test_gfsk_uses_direct_snr();
    test_gfsk_falls_back_when_direct_snr_invalid();
    test_c4fm_snr_at_invalid_threshold_falls_back();
    test_qpsk_uses_cqpsk_snr();
    test_qpsk_uses_constellation_fallback();
    test_qpsk_uses_best_legacy_snr_when_constellation_invalid();
    test_qpsk_reports_invalid_when_all_estimates_stale();
    printf("UI_SNR_READOUT: OK\n");
    return 0;
}
