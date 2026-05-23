// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime logging implementation for environment-independent logging.
 *
 * Implements the low-level write routine used by logging macros to emit
 * messages. Currently forwards to `stderr`. Future enhancements may include
 * runtime level control, timestamps, and file sinks.
 */

#include <cstdarg>
#include <cstdio>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/unicode.h>
#include "dsd-neo/core/safe_api.h"

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level; /* Currently unused, but available for future runtime gating */

    if (format == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);
    /* Format into a temporary buffer first so we can apply ASCII fallback if needed. */
    char buf[4096];
    DSD_VSNPRINTF(buf, sizeof(buf), format, args);
    va_end(args);

    if (dsd_unicode_supported()) {
        fputs(buf, stderr);
    } else {
        char safe[4096];
        dsd_ascii_fallback(buf, safe, sizeof(safe));
        fputs(safe, stderr);
    }
}
