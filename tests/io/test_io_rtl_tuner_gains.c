// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* rtl_tcp manual gain snapping (B6): the nearest of librtlsdr's gain steps for the tuner the server's header names,
 * and no step (the request goes out raw) for a tuner librtlsdr does not know or a header whose gain count differs
 * from that tuner's table. Portable: runs on every platform, radio support or not. */

#include <limits.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "rtl_tuner_gains.h"

enum { E4000 = 1, FC0012 = 2, FC0013 = 3, FC2580 = 4, R820T = 5, R828D = 6 };

/* The step @p type with @p count snaps @p request to must be @p want. */
static int
expect_step(const char* label, unsigned type, unsigned count, int request, int want) {
    int got = -12345;
    const int found = dsd_rtl_tuner_gain_nearest(type, count, request, &got);
    if (found != 1 || got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d -> found %d, step %d (want %d)\n", label, request, found, got, want);
        return 1;
    }
    return 0;
}

/* No step for @p type with @p count: 0 returned, the output left alone. */
static int
expect_raw(const char* label, unsigned type, unsigned count, int request) {
    int got = -12345;
    const int found = dsd_rtl_tuner_gain_nearest(type, count, request, &got);
    if (found != 0 || got != -12345) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d -> found %d, step %d (want raw)\n", label, request, found, got);
        return 1;
    }
    return 0;
}

static int
test_r82xx(void) {
    int rc = 0;
    for (unsigned type = R820T; type <= R828D; type++) {
        rc |= expect_step("R82xx 30 dB", type, 29U, 300, 297);
        rc |= expect_step("R82xx 20 dB", type, 29U, 200, 197);
        rc |= expect_step("R82xx top step", type, 29U, 496, 496);
        rc |= expect_step("R82xx above the top", type, 29U, 9999, 496);
        rc |= expect_step("R82xx below the bottom", type, 29U, -50, 0);
        rc |= expect_step("R82xx exact step", type, 29U, 280, 280);
        rc |= expect_step("R82xx INT_MAX", type, 29U, INT_MAX, 496);
        rc |= expect_step("R82xx INT_MIN", type, 29U, INT_MIN, 0);
        /* 20.2 dB is 0.5 dB from both 19.7 and 20.7: the lower step stays, as on USB. */
        rc |= expect_step("R82xx tie keeps the lower", type, 29U, 202, 197);
    }
    return rc;
}

static int
test_other_tuners(void) {
    int rc = 0;
    rc |= expect_step("E4000 30 dB", E4000, 14U, 300, 290);
    rc |= expect_step("E4000 7.1 dB", E4000, 14U, 71, 65);
    rc |= expect_step("E4000 10 dB", E4000, 14U, 100, 90);
    rc |= expect_step("E4000 below the bottom", E4000, 14U, -500, -10);
    rc |= expect_step("FC0012 10 dB", FC0012, 5U, 100, 71);
    rc |= expect_step("FC0013 10 dB", FC0013, 23U, 100, 71);
    rc |= expect_step("FC0013 top step", FC0013, 23U, 300, 197);
    rc |= expect_step("FC2580 any gain", FC2580, 1U, 300, 0);
    return rc;
}

static int
test_unknown_or_mismatched(void) {
    int rc = 0;
    rc |= expect_raw("unknown tuner (0)", 0U, 29U, 300);
    rc |= expect_raw("unknown tuner (7)", 7U, 29U, 300);
    rc |= expect_raw("unknown tuner (huge)", 0xFFFFFFFFU, 29U, 300);
    rc |= expect_raw("R820T with 28 gains", R820T, 28U, 300);
    rc |= expect_raw("R828D with 30 gains", R828D, 30U, 300);
    rc |= expect_raw("E4000 with 29 gains", E4000, 29U, 300);
    rc |= expect_raw("FC2580 with no gains", FC2580, 0U, 300);
    if (dsd_rtl_tuner_gain_nearest(R820T, 29U, 300, NULL) != 0) {
        DSD_FPRINTF(stderr, "FAIL: no output pointer must find no step\n");
        rc |= 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_r82xx();
    rc |= test_other_tuners();
    rc |= test_unknown_or_mismatched();
    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_RTL_TUNER_GAINS: OK\n");
    }
    return rc ? 1 : 0;
}
