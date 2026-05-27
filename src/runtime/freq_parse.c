// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/freq_parse.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static double
freq_suffix_factor(char* buf) {
    size_t len = strlen(buf);
    if (len == 0) {
        return 0.0;
    }

    char last = buf[len - 1];
    if (last == 'g' || last == 'G') {
        buf[len - 1] = '\0';
        return 1e9;
    }
    if (last == 'm' || last == 'M') {
        buf[len - 1] = '\0';
        return 1e6;
    }
    if (last == 'k' || last == 'K') {
        buf[len - 1] = '\0';
        return 1e3;
    }

    return 1.0;
}

static int
parse_positive_double(const char* text, double* value) {
    char* end = NULL;
    double parsed = strtod(text, &end);
    if (end == text || (end && *end != '\0') || parsed <= 0.0) {
        return 0;
    }

    *value = parsed;
    return 1;
}

uint32_t
dsd_parse_freq_hz(const char* s) {
    if (!s || !*s) {
        return 0;
    }
    char buf[64];
    DSD_SNPRINTF(buf, sizeof buf, "%s", s);
    buf[sizeof buf - 1] = '\0';
    double factor = freq_suffix_factor(buf);
    if (factor <= 0.0) {
        return 0;
    }

    double val = 0.0;
    if (!parse_positive_double(buf, &val)) {
        return 0;
    }

    double hz = val * factor;
    if (hz <= 0.0) {
        return 0;
    }
    if (hz > (double)UINT32_MAX) {
        hz = (double)UINT32_MAX;
    }
    return (uint32_t)(hz + 0.5);
}
