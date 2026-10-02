// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_parse_double_strict() is the validator behind user-supplied numbers (Airspy squelch, Soapy settings): it must
 * refuse "nan" and infinities, not only out-of-range finite values. Built with IEEE semantics, like those callers.
 */

#include <assert.h>
#include <dsd-neo/core/parse.h>
#include <float.h>
#include <math.h>

static int
parses(const char* text, double lo, double hi, double* out) {
    return dsd_parse_double_strict(text, lo, hi, out) == 0;
}

int
main(void) {
    double v = 0.0;

    assert(parses("1.5", -10.0, 10.0, &v) && fabs(v - 1.5) < 1e-12);
    assert(parses("-10", -10.0, 10.0, &v) && fabs(v + 10.0) < 1e-12);
    assert(parses("10", -10.0, 10.0, &v) && fabs(v - 10.0) < 1e-12);
    assert(!parses("10.5", -10.0, 10.0, &v));
    assert(!parses("1.5dB", -10.0, 10.0, &v));
    assert(!parses("", -10.0, 10.0, &v));

    /* NaN compares false with everything, so it used to slip past "value < min || value > max". */
    assert(!parses("nan", -10.0, 10.0, &v));
    assert(!parses("NAN", -DBL_MAX, DBL_MAX, &v));
    assert(!parses("-nan", -DBL_MAX, DBL_MAX, &v));
    assert(!parses("inf", -DBL_MAX, DBL_MAX, &v));
    assert(!parses("-infinity", -DBL_MAX, DBL_MAX, &v));
    assert(!parses("1e999", -DBL_MAX, DBL_MAX, &v));
    return 0;
}
