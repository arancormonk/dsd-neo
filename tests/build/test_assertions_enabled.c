// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Guards the test suite against being compiled with its assertions disabled.
 *
 * Most tests here carry their logic inside assert(), so a configuration that
 * defines NDEBUG - a plain `cmake ..`, which defaults to Release with testing
 * on, or the perf-bench preset - turns the suite into a set of programs that
 * exit 0 without checking anything, when it compiles at all: parameters and
 * locals that only the assertions read then trip -Werror=unused-parameter.
 * tests/CMakeLists.txt keeps assertions on for test code; this fails the build
 * the moment that stops being true.
 */

#include <assert.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#ifdef NDEBUG
#error "test code must be built with assertions enabled (see dsd-neo_test_assertions)"
#endif

static int g_evaluations = 0;

static int
observe(void) {
    g_evaluations++;
    return 1;
}

int
main(void) {
    /* NOLINTNEXTLINE(bugprone-assert-side-effect): the side effect is the
       subject of the test - it is what shows assert() still evaluates its
       expression rather than having been compiled away. */
    assert(observe());

    if (g_evaluations != 1) {
        DSD_FPRINTF(stderr, "assert() did not evaluate its expression (%d evaluations)\n", g_evaluations);
        return 1;
    }
    return 0;
}
