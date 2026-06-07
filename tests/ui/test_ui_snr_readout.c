// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#include <assert.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static rtl_stream_fsk_metrics g_fsk_metrics;
static int g_fsk_result = 0;
static int g_fsk_calls = 0;
static int g_snr_c4fm_calls = 0;
static int g_snr_c4fm_eye_calls = 0;
static int g_snr_cqpsk_calls = 0;
static int g_snr_gfsk_calls = 0;
static int g_snr_gfsk_eye_calls = 0;
static int g_snr_qpsk_const_calls = 0;
static double g_snr_c4fm = -100.0;
static double g_snr_c4fm_eye = -100.0;
static double g_snr_cqpsk = -100.0;
static double g_snr_gfsk = -100.0;
static double g_snr_gfsk_eye = -100.0;
static double g_snr_qpsk_const = -100.0;

int
rtl_stream_get_fsk_metrics(rtl_stream_fsk_metrics* out) {
    g_fsk_calls++;
    if (out) {
        *out = g_fsk_metrics;
    }
    return g_fsk_result;
}

double
rtl_stream_get_snr_c4fm(void) {
    g_snr_c4fm_calls++;
    return g_snr_c4fm;
}

double
rtl_stream_estimate_snr_c4fm_eye(void) {
    g_snr_c4fm_eye_calls++;
    return g_snr_c4fm_eye;
}

double
rtl_stream_get_snr_cqpsk(void) {
    g_snr_cqpsk_calls++;
    return g_snr_cqpsk;
}

double
rtl_stream_get_snr_gfsk(void) {
    g_snr_gfsk_calls++;
    return g_snr_gfsk;
}

double
rtl_stream_estimate_snr_gfsk_eye(void) {
    g_snr_gfsk_eye_calls++;
    return g_snr_gfsk_eye;
}

double
rtl_stream_estimate_snr_qpsk_const(void) {
    g_snr_qpsk_const_calls++;
    return g_snr_qpsk_const;
}

static void
reset_fakes(void) {
    ui_snr_readout_reset_for_test();
    DSD_MEMSET(&g_fsk_metrics, 0, sizeof(g_fsk_metrics));
    g_fsk_result = 0;
    g_fsk_calls = 0;
    g_snr_c4fm_calls = 0;
    g_snr_c4fm_eye_calls = 0;
    g_snr_cqpsk_calls = 0;
    g_snr_gfsk_calls = 0;
    g_snr_gfsk_eye_calls = 0;
    g_snr_qpsk_const_calls = 0;
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
test_c4fm_prefers_valid_fsk_soft_snr(void) {
    reset_fakes();
    g_fsk_metrics.valid = 1;
    g_fsk_metrics.evm_snr_db = 18.25f;
    g_snr_c4fm = 4.0;

    expect_readout("c4fm-soft", 0, 18.25, "C4FM");
    assert(g_fsk_calls == 1);
    assert(g_snr_c4fm_calls == 0);
}

static void
test_gfsk_prefers_valid_fsk_soft_snr(void) {
    reset_fakes();
    g_fsk_metrics.valid = 1;
    g_fsk_metrics.evm_snr_db = 21.5f;
    g_snr_gfsk = 6.0;

    expect_readout("gfsk-soft", 2, 21.5, "GFSK");
    assert(g_fsk_calls == 1);
    assert(g_snr_gfsk_calls == 0);
}

static void
test_gfsk_falls_back_when_fsk_metrics_invalid(void) {
    reset_fakes();
    g_fsk_metrics.valid = 0;
    g_snr_gfsk = -100.0;
    g_snr_gfsk_eye = 7.25;

    expect_readout("gfsk-fallback", 2, 7.25, "GFSK");
    assert(g_fsk_calls == 1);
    assert(g_snr_gfsk_calls == 1);
    assert(g_snr_gfsk_eye_calls == 1);
}

static void
test_fsk_soft_snr_at_invalid_threshold_falls_back(void) {
    reset_fakes();
    g_fsk_metrics.valid = 1;
    g_fsk_metrics.evm_snr_db = -50.0f;
    g_snr_c4fm = 5.5;

    expect_readout("c4fm-threshold-fallback", 0, 5.5, "C4FM");
    assert(g_fsk_calls == 1);
    assert(g_snr_c4fm_calls == 1);
}

static void
test_qpsk_ignores_fsk_soft_snr(void) {
    reset_fakes();
    g_fsk_metrics.valid = 1;
    g_fsk_metrics.evm_snr_db = 44.0f;
    g_snr_cqpsk = 12.75;

    expect_readout("qpsk-no-fsk", 1, 12.75, "QPSK");
    assert(g_fsk_calls == 0);
    assert(g_snr_cqpsk_calls == 1);
}

int
main(void) {
    test_c4fm_prefers_valid_fsk_soft_snr();
    test_gfsk_prefers_valid_fsk_soft_snr();
    test_gfsk_falls_back_when_fsk_metrics_invalid();
    test_fsk_soft_snr_at_invalid_threshold_falls_back();
    test_qpsk_ignores_fsk_soft_snr();
    printf("UI_SNR_READOUT: OK\n");
    return 0;
}
