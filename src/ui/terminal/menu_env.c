// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Environment variable helpers for menu subsystem.
 */

#include "menu_env.h"
#include <dsd-neo/core/parse.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <float.h> // IWYU pragma: keep
#include <limits.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

// IWYU pragma: no_include <__float_float.h>

static int
parse_int_with_default(const char* text, int defv) {
    if (!text || *text == '\0') {
        return defv;
    }
    int value = 0;
    if (dsd_parse_int_strict(text, 10, INT_MIN, INT_MAX, &value) != 0) {
        return 0;
    }
    return value;
}

static double
parse_double_with_default(const char* text, double defv) {
    if (!text || *text == '\0') {
        return defv;
    }
    double value = 0.0;
    if (dsd_parse_double_strict(text, -DBL_MAX, DBL_MAX, &value) != 0) {
        return 0.0;
    }
    return value;
}

int
env_get_int(const char* name, int defv) {
    const char* v = dsd_neo_env_get(name);
    return parse_int_with_default(v, defv);
}

double
env_get_double(const char* name, double defv) {
    const char* v = dsd_neo_env_get(name);
    return parse_double_with_default(v, defv);
}

void
env_set_int(const char* name, int v) {
    char buf[64];
    DSD_SNPRINTF(buf, sizeof buf, "%d", v);
    dsd_setenv(name, buf, 1);
}

void
env_set_double(const char* name, double v) {
    char buf[64];
    // limit precision just for display sanity
    DSD_SNPRINTF(buf, sizeof buf, "%.6g", v);
    dsd_setenv(name, buf, 1);
}

void
env_reparse_runtime_cfg(dsd_opts* opts) {
    dsd_neo_config_init();
    dsd_apply_runtime_config_to_opts(dsd_neo_get_config(), opts, NULL);
}
