// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_LOG_H
#define DSD_NEO_LOG_H

#include <dsd-neo/platform/platform.h>

/**
 * @file
 * @brief Runtime logging interface used across DSD-neo components.
 *
 * Declares log severity levels, the core write routine, and convenience macros.
 */

/**
 * @brief Log severity levels for runtime logging.
 */
typedef enum DSD_ATTR_PACKED {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN = 1,
    LOG_LEVEL_INFO = 2,
    LOG_LEVEL_DEBUG = 3
} dsd_neo_log_level_t;

/* Compile-time log level control (default to INFO) */
#ifndef DSD_NEO_LOG_LEVEL
#define DSD_NEO_LOG_LEVEL LOG_LEVEL_INFO
#endif

/**
 * @brief Destination for messages written through dsd_neo_log_write().
 *
 * `STDERR` is the default everywhere and keeps log output on the process's
 * standard error stream. `PLATFORM` selects the host platform's native logging
 * facility when one exists (Android: logcat, with severities mapped from
 * ::dsd_neo_log_level_t); on platforms without one it behaves like `STDERR`.
 *
 * Embedders that run the decoder inside an application process (where stderr is
 * discarded) should select `PLATFORM` during initialization.
 */
typedef enum DSD_ATTR_PACKED { DSD_NEO_LOG_SINK_STDERR = 0, DSD_NEO_LOG_SINK_PLATFORM = 1 } dsd_neo_log_sink_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write a formatted log message to the active log sink.
 *
 * @param level  Log severity for the message.
 * @param format printf-style format string.
 * @param ...    Variadic arguments corresponding to `format`.
 */
void dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) DSD_ATTR_FORMAT(printf, 2, 3);

/**
 * @brief Select the destination for subsequent log writes.
 *
 * Thread-safe and callable at any time. When never called, the sink resolves
 * once from the `DSD_NEO_LOG_SINK` environment variable (`platform`/`logcat`
 * select ::DSD_NEO_LOG_SINK_PLATFORM; anything else, including unset, selects
 * ::DSD_NEO_LOG_SINK_STDERR).
 *
 * @param sink Destination to use.
 */
void dsd_neo_log_set_sink(dsd_neo_log_sink_t sink);

/**
 * @brief Report the active log sink, resolving the environment default if needed.
 *
 * @return Currently selected destination.
 */
dsd_neo_log_sink_t dsd_neo_log_get_sink(void);

#ifdef __cplusplus
}
#endif

/* Logging macros route through dsd_neo_log_write to allow environment handling */

/* Error messages - always shown */
#define LOG_ERROR(...)                                                                                                 \
    do {                                                                                                               \
        dsd_neo_log_write(LOG_LEVEL_ERROR, __VA_ARGS__);                                                               \
    } while (0)

/* Warning messages - always shown */
#define LOG_WARN(...)                                                                                                  \
    do {                                                                                                               \
        dsd_neo_log_write(LOG_LEVEL_WARN, __VA_ARGS__);                                                                \
    } while (0)

/* Info messages - always shown */
#define LOG_INFO(...)                                                                                                  \
    do {                                                                                                               \
        dsd_neo_log_write(LOG_LEVEL_INFO, __VA_ARGS__);                                                                \
    } while (0)

/* Debug messages - compile-time gated */
#if DSD_NEO_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...)                                                                                                 \
    do {                                                                                                               \
        dsd_neo_log_write(LOG_LEVEL_DEBUG, __VA_ARGS__);                                                               \
    } while (0)
#else
#define LOG_DEBUG(...)                                                                                                 \
    do {                                                                                                               \
        /* Debug logging disabled */                                                                                   \
    } while (0)
#endif

#endif /* DSD_NEO_LOG_H */
