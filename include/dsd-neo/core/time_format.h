// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Time/date formatting helpers.
 *
 * Provides small helpers used for console/UI timestamps and log filenames.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_TIME_FORMAT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_TIME_FORMAT_H_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DSD_LOCAL_DATETIME_TIME_COMPACT = 0, /**< HHmmss */
    DSD_LOCAL_DATETIME_TIME_COLON,       /**< HH:MM:SS */
    DSD_LOCAL_DATETIME_DATE_COMPACT,     /**< YYYYMMDD */
    DSD_LOCAL_DATETIME_DATE_SLASH,       /**< YYYY/MM/DD */
    DSD_LOCAL_DATETIME_DATE_HYPHEN,      /**< YYYY-MM-DD */
} dsd_local_datetime_format;

/**
 * Format a local calendar date or time for an explicit timestamp.
 *
 * On local-time conversion failure, writes the format's zero-value fallback.
 * Returns 1 on success and 0 on conversion failure or invalid arguments.
 */
int dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_TIME_FORMAT_H_H */
