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
#include <cstring>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/unicode.h>

/**
 * @brief Write a formatted log message to the logging sink.
 *
 * Currently forwards to `stderr`. The `level` parameter is reserved for future
 * runtime gating and may be used to filter messages at runtime.
 *
 * @param level  Log severity level (currently not used for filtering).
 * @param format printf-style format string.
 * @param ...    Variadic arguments corresponding to `format`.
 */
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
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (dsd_unicode_supported()) {
        fputs(buf, stderr);
    } else {
        char safe[4096];
        dsd_ascii_fallback(buf, safe, sizeof(safe));
        fputs(safe, stderr);
    }
}
