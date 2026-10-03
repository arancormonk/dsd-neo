// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_parse_bool_strict() reads a whole boolean word. The bias-tee token and the env switches rely on it telling "on"
 * from "off", which share a first letter, and on it refusing anything else without touching the output.
 */

#include <assert.h>
#include <dsd-neo/core/parse.h>
#include <stddef.h>

static int
parses_as(const char* text, int expected) {
    int v = -7;
    return dsd_parse_bool_strict(text, &v) == 0 && v == expected;
}

static int
refused(const char* text) {
    int v = -7;
    return dsd_parse_bool_strict(text, &v) == -1 && v == -7;
}

int
main(void) {
    static const char* const k_true[] = {"1", "true", "TRUE", "True", "yes", "YES", "on", "ON", "On", "oN"};
    static const char* const k_false[] = {"0", "false", "FALSE", "no", "No", "off", "OFF", "Off"};
    static const char* const k_bad[] = {"", " on", "on ", "onx", "of", "offf", "2", "-1", "y", "n", "t", "f", "enable"};

    for (size_t i = 0; i < sizeof(k_true) / sizeof(k_true[0]); i++) {
        assert(parses_as(k_true[i], 1));
    }
    for (size_t i = 0; i < sizeof(k_false) / sizeof(k_false[0]); i++) {
        assert(parses_as(k_false[i], 0));
    }
    for (size_t i = 0; i < sizeof(k_bad) / sizeof(k_bad[0]); i++) {
        assert(refused(k_bad[i]));
    }
    assert(refused(NULL));
    assert(dsd_parse_bool_strict("on", NULL) == -1);
    return 0;
}
