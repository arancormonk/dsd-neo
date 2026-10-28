// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Digit-pattern IDs: one group-list or radio-list row that covers many talkgroup or radio IDs.
 *
 * A pattern is an ID token that contains `x`/`X` or `*` and otherwise only decimal digits. It matches the ID written in
 * decimal without leading zeros: `x` is exactly one digit, `*` is any run of digits including none. `1xxx` covers
 * 1000-1999, `12*` every ID starting 12, `*05` every ID ending 05, `*` every ID.
 *
 * Parsing trims ASCII whitespace around the token, then normalises (lower-case `x`, `**` collapsed to `*`) and
 * validates in that order. It rejects any other character (an inner space included), an empty result, a token with no
 * wildcard (that is an exact ID), more than 15 canonical characters, a leading literal `0` (IDs carry no leading
 * zeros, so such a pattern could only ever match 0 itself, which is written `0`), and a pattern that no ID in
 * 0..4294967295 matches. The canonical text is what callers store, key, export and compare.
 *
 * Matching does not allocate or recurse: the ID is formatted once (dsd_id_decimal()) and an iterative glob runs over
 * its digits, so a lookup over many rows formats the ID once and calls dsd_id_pattern_match_decimal() per row.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_ID_PATTERN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_ID_PATTERN_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_ID_PATTERN_TEXT_MAX = 16, /**< Canonical text capacity: at most 15 characters plus the terminator. */
    DSD_ID_DECIMAL_MAX = 11       /**< A uint32_t ID in decimal: at most 10 digits plus the terminator. */
};

/** A parsed digit pattern. A zeroed value is the empty pattern, which matches nothing. */
typedef struct {
    char text[DSD_ID_PATTERN_TEXT_MAX]; /**< Canonical text: lower-case `x`, single stars. */
    uint8_t literals;                   /**< Number of literal digits. */
    uint8_t has_star;                   /**< 1 when the pattern contains `*`. */
} dsd_id_pattern;

/**
 * Whether a token is meant as a pattern: it contains `x`, `X` or `*`. True even when the token would then fail to
 * parse, so a caller can report a bad pattern instead of a bad number. NULL is not a candidate.
 */
int dsd_id_pattern_is_candidate(const char* token);

/**
 * Parse a pattern token (see the file comment for the rules). Returns 0 and fills `out` on success; returns 1 on an
 * invalid token or NULL argument, and then leaves `out` (when not NULL) as the empty pattern.
 */
int dsd_id_pattern_parse(const char* token, dsd_id_pattern* out);

/**
 * Write `id` in decimal without leading zeros (0 is "0") and NUL-terminate it. Returns the digit count, or 0 when
 * `out` is NULL or `out_size` cannot hold the digits and the terminator (then `out`, when it has room, holds "").
 * DSD_ID_DECIMAL_MAX bytes always suffice.
 */
size_t dsd_id_decimal(uint32_t id, char* out, size_t out_size);

/**
 * Whether `p` matches the `len` digits at `digits`, which must be an ID as dsd_id_decimal() writes it. Returns 0 for
 * a NULL argument or no digits.
 */
int dsd_id_pattern_match_decimal(const dsd_id_pattern* p, const char* digits, size_t len);

/** Whether `p` matches `id`. Returns 0 for a NULL pattern. */
int dsd_id_pattern_match(const dsd_id_pattern* p, uint32_t id);

/**
 * Returns 1 only when `a` is strictly more specific than `b`: more literal digits, or as many and `a` has no `*`
 * while `b` has one. Equally specific patterns return 0 both ways; the caller breaks that tie (the last row wins).
 * Returns 0 for a NULL argument.
 */
int dsd_id_pattern_more_specific(const dsd_id_pattern* a, const dsd_id_pattern* b);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_ID_PATTERN_H_ */
