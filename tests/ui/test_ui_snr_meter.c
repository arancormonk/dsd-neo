// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/ui/ncurses_snr.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

int ui_unicode_supported(void);      // NOLINT(misc-use-internal-linkage)
int ui_block_glyphs_supported(void); // NOLINT(misc-use-internal-linkage)

#if defined(DSD_NEO_FAST_MATH) || defined(__FAST_MATH__) || defined(_M_FP_FAST)
#define DSD_NEO_TEST_FAST_MATH 1
#elif defined(__FINITE_MATH_ONLY__) && (__FINITE_MATH_ONLY__ != 0)
#define DSD_NEO_TEST_FAST_MATH 1
#else
#define DSD_NEO_TEST_FAST_MATH 0
#endif

int
ui_unicode_supported(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

int
ui_block_glyphs_supported(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

static void
assert_bars(double snr_db, int expected) {
    int actual = dsd_ncurses_snr_meter_bar_count_for_test(snr_db);
    if (actual != expected) {
        DSD_FPRINTF(stderr, "bar count for %.1f dB: expected %d, got %d\n", snr_db, expected, actual);
    }
    assert(actual == expected);
}

static void
assert_ascii(double snr_db, const char* expected) {
    char actual[16];
    DSD_MEMSET(actual, 'X', sizeof(actual));
    dsd_ncurses_snr_meter_ascii_for_test(snr_db, actual, sizeof(actual));
    if (strcmp(actual, expected) != 0) {
        DSD_FPRINTF(stderr, "ASCII meter for %.1f dB: expected '%s', got '%s'\n", snr_db, expected, actual);
    }
    assert(strcmp(actual, expected) == 0);
    assert(actual[9] == '\0');
}

int
main(void) {
    assert(dsd_ncurses_snr_use_unicode_for_test(0, 0) == 0);
    assert(dsd_ncurses_snr_use_unicode_for_test(0, 1) == 0);
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 0) == 0);
#if defined(DSD_USE_PDCURSES) && !defined(DSD_HAS_PDCURSES_WIDE_API)
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 1) == 0);
#else
    assert(dsd_ncurses_snr_use_unicode_for_test(1, 1) == 1);
#endif

#if !DSD_NEO_TEST_FAST_MATH
    assert(dsd_ncurses_snr_meter_bar_count_for_test(NAN) == 0);
#endif
    assert_bars(-50.0, 0);
    assert_bars(-20.0, 1);
    assert_bars(-15.0, 1);
    assert_bars(-6.0, 2);
    assert_bars(3.0, 3);
    assert_bars(12.0, 4);
    assert_bars(21.0, 5);
    assert_bars(30.0, 5);
    assert_bars(60.0, 5);

#if !DSD_NEO_TEST_FAST_MATH
    assert_ascii(NAN, "         ");
#endif
    assert_ascii(-50.0, "         ");
    assert_ascii(-15.0, "|        ");
    assert_ascii(-6.0, "| |      ");
    assert_ascii(3.0, "| | |    ");
    assert_ascii(12.0, "| | | |  ");
    assert_ascii(21.0, "| | | | |");

    {
        char tiny[3];
        DSD_MEMSET(tiny, 'X', sizeof(tiny));
        dsd_ncurses_snr_meter_ascii_for_test(21.0, tiny, sizeof(tiny));
        assert(strcmp(tiny, "| ") == 0);
        assert(tiny[2] == '\0');
    }

    printf("UI_SNR_METER: OK\n");
    return 0;
}
