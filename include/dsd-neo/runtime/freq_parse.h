// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Parse frequency strings into Hz (supports K/M/G suffixes), and the exact typed-entry parser.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_

#include <stddef.h>
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

/** Longest text dsd_parse_freq_entry_hz() reads, in bytes, counted before trimming. */
#define DSD_FREQ_ENTRY_MAX_LEN     32
/** A bare number (no unit) below this value is MHz; at or above it, Hz. */
#define DSD_FREQ_ENTRY_BARE_HZ_MIN 1000000u

/** Results of dsd_parse_freq_entry_hz(). */
typedef enum {
    DSD_FREQ_ENTRY_OK = 0,
    DSD_FREQ_ENTRY_EMPTY,         /**< NULL or only spaces: the caller's cancel */
    DSD_FREQ_ENTRY_TOO_LONG,      /**< more than DSD_FREQ_ENTRY_MAX_LEN bytes */
    DSD_FREQ_ENTRY_DECIMAL_COMMA, /**< any ',' (a keypad decimal on some layouts) */
    DSD_FREQ_ENTRY_SYNTAX,        /**< no digit, sign, second '.', inner space, exponent, hex, tab, non-ASCII */
    DSD_FREQ_ENTRY_UNIT,          /**< trailing letters that are not Hz, k, kHz, M, MHz, G, GHz (any case) */
    DSD_FREQ_ENTRY_SUB_HZ,        /**< a non-zero digit below 1 Hz */
    DSD_FREQ_ENTRY_ZERO,          /**< the value is 0 Hz */
    DSD_FREQ_ENTRY_AMBIGUOUS,     /**< bare 4295..999999: too large for MHz, too small for Hz */
    DSD_FREQ_ENTRY_TOO_HIGH,      /**< above 4294.967295 MHz in any unit */
} dsd_freq_entry_result;

/**
 * @brief Parse a typed frequency exactly (integer arithmetic, no locale).
 *
 * Grammar: optional ASCII spaces, a number `[0-9]*('.'[0-9]*)?` with at least one digit, optional
 * spaces, an optional unit `Hz`, `k`, `kHz`, `M`, `MHz`, `G` or `GHz` (any case; `m` is mega), optional
 * spaces. A number without a unit is MHz when its integer part (leading zeros aside) has at most six
 * digits, and Hz otherwise. The text is read up to DSD_FREQ_ENTRY_MAX_LEN bytes.
 *
 * @param text   Text to parse (NULL is EMPTY).
 * @param out_hz Receives the frequency in Hz, written only on DSD_FREQ_ENTRY_OK (NULL is EMPTY).
 * @return A dsd_freq_entry_result.
 */
int dsd_parse_freq_entry_hz(const char* text, uint32_t* out_hz);

/**
 * @brief Static one-line message (at most 56 bytes) for a dsd_parse_freq_entry_hz() refusal.
 *
 * @return The message, or "" for DSD_FREQ_ENTRY_OK, DSD_FREQ_ENTRY_EMPTY and unknown values.
 */
const char* dsd_freq_entry_result_text(int result);

/**
 * @brief Format a frequency as MHz with six decimals ("%u.%06u"), exact and locale-free.
 *
 * Callers choose their own placeholder for 0 Hz (this prints "0.000000").
 *
 * @return The length snprintf reports (the untruncated length), or -1 when out is NULL or out_size is 0.
 */
int dsd_freq_hz_mhz_text(uint32_t hz, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_FREQ_PARSE_H_ */
