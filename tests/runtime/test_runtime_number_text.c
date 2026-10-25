// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Readout text without a negative zero (issue #669): the sign rule, padding, saturation and counts. */

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h> // IWYU pragma: keep (NAN and INFINITY: MSVC defines them here, not in float.h)
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/number_text.h>

static void
expect_text(double value, int width, int precision, unsigned flags, const char* want) {
    char out[96];
    const int n = dsd_number_text(out, sizeof out, value, width, precision, flags);
    if (n < 0 || strcmp(out, want) != 0 || (size_t)n != strlen(want)) {
        DSD_FPRINTF(stderr, "value %.17g width %d precision %d flags %u: got \"%s\" (%d), want \"%s\"\n", value, width,
                    precision, flags, out, n, want);
        assert(0);
    }
}

/* Non-zero readings keep the platform's printf rounding: the oracle is snprintf itself, never a hard-coded tie. */
static void
expect_like_printf(double value, int width, int precision, unsigned flags) {
    char want[96];
    if (flags & DSD_NUMBER_TEXT_PLUS) {
        DSD_SNPRINTF(want, sizeof want, "%+*.*f", width, precision, value);
    } else {
        DSD_SNPRINTF(want, sizeof want, "%*.*f", width, precision, value);
    }
    /* A reading printf rounds to zero is the one case the helper changes: its sign goes (or becomes '+'). */
    char* sign = strchr(want, '-');
    if (sign && strpbrk(want, "123456789") == NULL) {
        if (flags & DSD_NUMBER_TEXT_PLUS) {
            *sign = '+';
        } else if (sign == want) {
            DSD_MEMMOVE(want, want + 1, strlen(want));
        } else {
            *sign = ' ';
        }
    }
    expect_text(value, width, precision, flags, want);
}

static void
expect_normalized(const char* in, unsigned flags, const char* want, int want_rc) {
    char text[64];
    DSD_SNPRINTF(text, sizeof text, "%s", in);
    const int rc = dsd_number_text_drop_negative_zero(text, flags);
    if (rc != want_rc || strcmp(text, want) != 0) {
        DSD_FPRINTF(stderr, "normalize \"%s\" flags %u: got \"%s\" (%d), want \"%s\" (%d)\n", in, flags, text, rc, want,
                    want_rc);
        assert(0);
    }
}

static void
test_negative_zero(void) {
    /* Each of these prints a negative zero through printf ("-0.0", "-0"); the helper drops the sign. */
    expect_text(-0.04, 0, 1, 0, "0.0");
    expect_text(-0.049, 0, 1, 0, "0.0");
    expect_text(-0.3, 0, 0, 0, "0");
    expect_text(-0.004, 0, 2, 0, "0.00");
    expect_text(-0.0, 0, 1, 0, "0.0");
    expect_text(0.0, 0, 1, 0, "0.0");
    expect_text(-1e-300, 0, 9, 0, "0.000000000");
    expect_text(-0.04, 6, 1, 0, "   0.0");
    expect_text(-0.4, 4, 0, 0, "   0");
    /* Under the plus flag, every zero reads "+0". */
    expect_text(-0.04, 0, 1, DSD_NUMBER_TEXT_PLUS, "+0.0");
    expect_text(0.0, 0, 1, DSD_NUMBER_TEXT_PLUS, "+0.0");
    expect_text(-0.0, 8, 1, DSD_NUMBER_TEXT_PLUS, "    +0.0");
    /* Readings that do not round to zero keep their sign. */
    expect_text(-0.06, 0, 1, 0, "-0.1");
    expect_text(-0.6, 0, 0, 0, "-1");
    expect_text(-0.01, 0, 2, 0, "-0.01");
}

static void
test_matches_printf(void) {
    static const double values[] = {0.15,      0.25,  0.35, 1.45,    2.25,     2.5,    -2.5,
                                    22.5,      -22.5, 0.05, -0.05,   -0.051,   -122.5, 11459.2,
                                    -11459.25, 1e6,   -1e6, 3.14159, -3.14159, 99.95,  -99.95};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        for (int precision = 0; precision <= 3; precision++) {
            expect_like_printf(values[i], 0, precision, 0);
            expect_like_printf(values[i], 8, precision, 0);
            expect_like_printf(values[i], 8, precision, DSD_NUMBER_TEXT_PLUS);
        }
    }
}

static void
test_whole_number_before_truncation(void) {
    /* The sign is decided on the whole text, before the caller's buffer cuts it: "-0.01" stays negative. */
    char small[3];
    const int n = dsd_number_text(small, sizeof small, -0.01, 0, 2, 0);
    assert(n == 5);
    assert(strcmp(small, "-0") == 0);
    /* A zero reading cut short keeps its unsigned form. */
    const int z = dsd_number_text(small, sizeof small, -0.001, 0, 2, 0);
    assert(z == 4);
    assert(strcmp(small, "0.") == 0);
}

static void
test_not_a_number(void) {
    char out[32];
    char want[32];
    const double values[] = {NAN, -NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        /* Whatever the platform prints (nan, -nan, nan(ind), -inf, ...) is left exactly as printf wrote it. */
        DSD_SNPRINTF(want, sizeof want, "%.1f", values[i]);
        assert(dsd_number_text(out, sizeof out, values[i], 0, 1, 0) == (int)strlen(want));
        assert(strcmp(out, want) == 0);
        DSD_SNPRINTF(want, sizeof want, "%+.1f", values[i]);
        assert(dsd_number_text(out, sizeof out, values[i], 0, 1, DSD_NUMBER_TEXT_PLUS) == (int)strlen(want));
        assert(strcmp(out, want) == 0);
    }
    /* Saturated, non-numbers are cut to the width. */
    DSD_SNPRINTF(want, sizeof want, "%.1f", -INFINITY);
    assert(dsd_number_text(out, sizeof out, -INFINITY, 3, 1, DSD_NUMBER_TEXT_SATURATE) == 3);
    assert(strncmp(out, want, 3) == 0 && strlen(out) == 3);
}

static void
test_drop_negative_zero(void) {
    expect_normalized("-0.0", 0, "0.0", 1);
    expect_normalized("-0", 0, "0", 1);
    expect_normalized("-0,00", 0, "0,00", 1);
    expect_normalized("-0\xd9\xab"
                      "0",
                      0,
                      "0\xd9\xab"
                      "0",
                      1); /* U+066B ARABIC DECIMAL SEPARATOR (glibc ps_AF) */
    expect_normalized("-0.0", DSD_NUMBER_TEXT_PLUS, "+0.0", 1);
    expect_normalized("+0.0", DSD_NUMBER_TEXT_PLUS, "+0.0", 0);
    expect_normalized("0.0", 0, "0.0", 0);
    expect_normalized("-0.1", 0, "-0.1", 0);
    expect_normalized("-10", 0, "-10", 0);
    expect_normalized("-nan", 0, "-nan", 0);
    expect_normalized("-nan(0x8000000000000)", 0, "-nan(0x8000000000000)", 0);
    expect_normalized("-inf", 0, "-inf", 0);
    expect_normalized("-", 0, "-", 0);
    expect_normalized("-.", 0, "-.", 0);
    expect_normalized("", 0, "", 0);
    assert(dsd_number_text_drop_negative_zero(NULL, 0) == -1);
}

static void
test_locale_radix(const char* locale) {
    const char* old = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    DSD_SNPRINTF(saved, sizeof saved, "%s", old ? old : "C");
    if (setlocale(LC_NUMERIC, locale) == NULL) {
        DSD_FPRINTF(stderr, "locale %s not installed; skipping its radix case\n", locale);
        return;
    }
    char want[32];
    DSD_SNPRINTF(want, sizeof want, "%.1f", 0.0);
    /* Normalisation happens before padding: the padded zero has the locale's radix and no sign. */
    char out[32];
    const int n = dsd_number_text(out, sizeof out, -0.04, 8, 1, 0);
    char padded[32];
    DSD_SNPRINTF(padded, sizeof padded, "%8s", want);
    assert(n == (int)strlen(padded));
    assert(strcmp(out, padded) == 0);
    expect_like_printf(-1.25, 8, 2, 0);
    (void)setlocale(LC_NUMERIC, saved);
}

static void
test_saturate(void) {
    const unsigned sat = DSD_NUMBER_TEXT_SATURATE;
    expect_text(999.0, 4, 0, sat, " 999");
    expect_text(9999.0, 4, 0, sat, "9999");
    expect_text(10000.0, 4, 0, sat, ">999");
    expect_text(-999.0, 4, 0, sat, "-999");
    expect_text(-1000.0, 4, 0, sat, "<-99");
    expect_text(-0.4, 4, 0, sat, "   0");
    expect_text(2147483647.0, 4, 0, sat, ">999");
    expect_text(-2147483648.0, 4, 0, sat, "<-99");
    expect_text(9999.9, 8, 1, sat | DSD_NUMBER_TEXT_PLUS, " +9999.9");
    expect_text(99999.9, 8, 1, sat | DSD_NUMBER_TEXT_PLUS, "+99999.9");
    expect_text(100000.0, 8, 1, sat | DSD_NUMBER_TEXT_PLUS, ">+9999.9");
    expect_text(-100000.0, 8, 1, sat | DSD_NUMBER_TEXT_PLUS, "<-9999.9");
    expect_text(-1000.0, 6, 1, sat, "<-99.9");
    expect_text(-999.9, 6, 1, sat, "-999.9");
    expect_text(DBL_MAX, 6, 1, sat, ">999.9");
    /* No number of that precision fits: the marker fills the width. */
    expect_text(100.0, 3, 1, sat, ">>>");
    expect_text(-100.0, 2, 0, sat, "<<");
    /* Without SATURATE the width is a minimum, as printf's is. */
    expect_text(10000.0, 4, 0, 0, "10000");
}

static void
test_count(void) {
    char out[32];
    assert(dsd_count_text(out, sizeof out, 0, 6) == 6 && strcmp(out, "     0") == 0);
    assert(dsd_count_text(out, sizeof out, 9, 6) == 6 && strcmp(out, "     9") == 0);
    assert(dsd_count_text(out, sizeof out, 10, 6) == 6 && strcmp(out, "    10") == 0);
    assert(dsd_count_text(out, sizeof out, 999999, 6) == 6 && strcmp(out, "999999") == 0);
    assert(dsd_count_text(out, sizeof out, 1000000, 6) == 6 && strcmp(out, "  1.0M") == 0);
    assert(dsd_count_text(out, sizeof out, 1099999, 6) == 6 && strcmp(out, "  1.0M") == 0);
    assert(dsd_count_text(out, sizeof out, 999999999, 6) == 6 && strcmp(out, "999.9M") == 0);
    assert(dsd_count_text(out, sizeof out, 1000000000ULL, 6) == 6 && strcmp(out, "  1.0G") == 0);
    assert(dsd_count_text(out, sizeof out, 4294967295ULL, 6) == 6 && strcmp(out, "  4.2G") == 0);
    assert(dsd_count_text(out, sizeof out, 999999999999ULL, 6) == 6 && strcmp(out, "999.9G") == 0);
    assert(dsd_count_text(out, sizeof out, 1000000000000ULL, 6) == 6 && strcmp(out, "  1.0T") == 0);
    assert(dsd_count_text(out, sizeof out, 1000000000000000ULL, 6) == 6 && strcmp(out, "  1.0P") == 0);
    assert(dsd_count_text(out, sizeof out, 1000000000000000000ULL, 6) == 6 && strcmp(out, "  1.0E") == 0);
    assert(dsd_count_text(out, sizeof out, ULLONG_MAX, 6) == 6 && strcmp(out, " 18.4E") == 0);
    assert(dsd_count_text(out, sizeof out, 123, 0) == 3 && strcmp(out, "123") == 0);
    char small[4];
    assert(dsd_count_text(small, sizeof small, 999999, 6) == 6 && strcmp(small, "999") == 0);
    assert(dsd_count_text(NULL, 8, 1, 6) == -1);
    assert(dsd_count_text(out, 0, 1, 6) == -1);
    assert(dsd_count_text(out, sizeof out, 1, -1) == -1);
    assert(dsd_count_text(out, sizeof out, 1, 64) == -1);
}

static void
test_bad_arguments(void) {
    char out[16];
    assert(dsd_number_text(NULL, 16, 1.0, 0, 1, 0) == -1);
    assert(dsd_number_text(out, 0, 1.0, 0, 1, 0) == -1);
    assert(dsd_number_text(out, sizeof out, 1.0, 0, -1, 0) == -1);
    assert(dsd_number_text(out, sizeof out, 1.0, 0, 10, 0) == -1);
    assert(dsd_number_text(out, sizeof out, 1.0, -1, 1, 0) == -1);
    assert(dsd_number_text(out, sizeof out, 1.0, 64, 1, 0) == -1);
    assert(dsd_number_text(out, sizeof out, 1.0, 0, 1, DSD_NUMBER_TEXT_SATURATE) == -1);
    /* The largest double at the largest precision still fits the internal buffer. */
    char big[400];
    char want[400];
    DSD_SNPRINTF(want, sizeof want, "%.9f", -DBL_MAX);
    assert(dsd_number_text(big, sizeof big, -DBL_MAX, 0, 9, 0) == (int)strlen(want));
    assert(strcmp(big, want) == 0);
}

int
main(void) {
    test_negative_zero();
    test_matches_printf();
    test_whole_number_before_truncation();
    test_not_a_number();
    test_drop_negative_zero();
    test_saturate();
    test_count();
    test_bad_arguments();
    test_locale_radix("de_DE.UTF-8");
    test_locale_radix("ps_AF.UTF-8");
    return 0;
}
