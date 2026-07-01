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
#include <mutex>
#include "dsd-neo/core/safe_api.h"

namespace {

std::mutex g_log_sink_mutex;
dsd_neo_log_sink g_log_sink = {};
bool g_log_sink_active = false;

dsd_neo_log_sink
log_sink_snapshot(bool* active) {
    std::lock_guard<std::mutex> lock(g_log_sink_mutex);
    if (active != nullptr) {
        *active = g_log_sink_active;
    }
    return g_log_sink;
}

} // namespace

extern "C" void
dsd_neo_log_sink_set(const dsd_neo_log_sink* sink) {
    std::lock_guard<std::mutex> lock(g_log_sink_mutex);
    if (sink == nullptr || sink->write == nullptr) {
        g_log_sink = {};
        g_log_sink_active = false;
        return;
    }
    g_log_sink = *sink;
    g_log_sink_active = true;
}

extern "C" void
dsd_neo_log_sink_reset(void) {
    dsd_neo_log_sink_set(nullptr);
}

extern "C" void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    if (format == nullptr) {
        return;
    }

    va_list args;
    va_start(args, format);
    /* Format into a temporary buffer first so we can apply ASCII fallback if needed. */
    char buf[4096];
    DSD_VSNPRINTF(buf, sizeof(buf), format, args);
    va_end(args);

    const char* out = buf;
    char safe[4096];
    if (!dsd_unicode_supported()) {
        dsd_ascii_fallback(buf, safe, sizeof(safe));
        out = safe;
    }

    bool sink_active = false;
    dsd_neo_log_sink sink = log_sink_snapshot(&sink_active);
    if (sink_active && sink.write != nullptr) {
        sink.write(level, out, sink.context);
    }
    if (!sink_active || sink.mirror_stderr) {
        fputs(out, stderr);
    }
}
