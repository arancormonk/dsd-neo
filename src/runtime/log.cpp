// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime logging implementation for environment-independent logging.
 *
 * Implements the low-level write routine used by logging macros, plus sink
 * selection so an embedding application process can divert log output to the
 * platform's native logging facility instead of stderr.
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/unicode.h>
#include "dsd-neo/core/safe_api.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <cstring>
#endif

namespace {

/** Sentinel stored until the sink has been resolved from the environment. */
constexpr int kLogSinkUnresolved = -1;

std::atomic<int> g_log_sink{kLogSinkUnresolved};

/**
 * @brief Derive the default sink from the `DSD_NEO_LOG_SINK` environment variable.
 *
 * @return Sink value as an int.
 */
int
log_sink_from_env(void) {
    const char* v = getenv("DSD_NEO_LOG_SINK");
    if (v != nullptr && (v[0] == 'p' || v[0] == 'P' || v[0] == 'l' || v[0] == 'L')) {
        /* "platform" or "logcat" */
        return (int)DSD_NEO_LOG_SINK_PLATFORM;
    }
    return (int)DSD_NEO_LOG_SINK_STDERR;
}

/**
 * @brief Fetch the active sink, resolving the environment default exactly once.
 *
 * @return Sink value as an int.
 */
int
log_sink_current(void) {
    int sink = g_log_sink.load(std::memory_order_relaxed);
    if (sink == kLogSinkUnresolved) {
        sink = log_sink_from_env();
        int expected = kLogSinkUnresolved;
        if (!g_log_sink.compare_exchange_strong(expected, sink, std::memory_order_relaxed)) {
            /* Another thread resolved or explicitly set the sink first. */
            sink = expected;
        }
    }
    return sink;
}

#ifdef __ANDROID__
/**
 * @brief Emit one already-formatted message to logcat at the mapped priority.
 *
 * @param level Severity of the message.
 * @param text  NUL-terminated message body.
 */
void
log_write_android(dsd_neo_log_level_t level, const char* text) {
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case LOG_LEVEL_ERROR: prio = ANDROID_LOG_ERROR; break;
        case LOG_LEVEL_WARN: prio = ANDROID_LOG_WARN; break;
        case LOG_LEVEL_INFO: prio = ANDROID_LOG_INFO; break;
        case LOG_LEVEL_DEBUG: prio = ANDROID_LOG_DEBUG; break;
        default: break;
    }
    (void)__android_log_write(prio, "dsd-neo", text);
}
#endif

} // namespace

extern "C" void
dsd_neo_log_set_sink(dsd_neo_log_sink_t sink) {
    g_log_sink.store((int)sink, std::memory_order_relaxed);
}

extern "C" dsd_neo_log_sink_t
dsd_neo_log_get_sink(void) {
    return (dsd_neo_log_sink_t)log_sink_current();
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

    /* Writable only where the platform sink trims the message in place. */
#ifdef __ANDROID__
    char* out = buf;
#else
    const char* out = buf;
#endif
    char safe[4096];
    if (!dsd_unicode_supported()) {
        dsd_ascii_fallback(buf, safe, sizeof(safe));
        out = safe;
    }

    const int sink = log_sink_current();
#ifdef __ANDROID__
    if (sink == (int)DSD_NEO_LOG_SINK_PLATFORM) {
        /* Each logcat record is already one line; a trailing newline would show
           up as a spurious blank entry. */
        size_t len = strlen(out);
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
            len--;
            out[len] = '\0';
        }
        log_write_android(level, out);
        return;
    }
#else
    /* No native facility off-Android: PLATFORM falls through to stderr. */
    (void)sink;
#endif

    (void)level;
    fputs(out, stderr);
}
