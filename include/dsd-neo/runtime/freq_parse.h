// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Parse frequency strings into Hz (supports K/M/G suffixes).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a frequency string into Hz.
 *
 * Accepts an optional K/M/G suffix (case-insensitive) and rounds to the nearest Hz. Returns 0 for
 * invalid or empty input, non-finite values (nan, inf), text of 64 bytes or more, and any value
 * that rounds above UINT32_MAX.
 */
uint32_t dsd_parse_freq_hz(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_ */
