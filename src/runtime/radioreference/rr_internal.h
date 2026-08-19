// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Helpers shared between the RadioReference translation units. Module-private:
 * include as "rr_internal.h" from siblings in this directory.
 */

#ifndef DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_INTERNAL_H
#define DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Grow a heap array to hold at least `needed` elements, doubling as it goes.
 *
 * New tail elements are zeroed, so a freshly committed entry starts from a known
 * state. On failure the existing allocation is left untouched.
 *
 * @param items     Address of the array pointer.
 * @param cap       Address of the current capacity in elements.
 * @param needed    Required capacity in elements.
 * @param elem_size Size of one element.
 * @return 0 on success, -1 on overflow or allocation failure.
 */
int rr_array_reserve(void** items, size_t* cap, size_t needed, size_t elem_size);

/**
 * @brief Strict base-10 parse of a whole token.
 *
 * Rejects trailing junk, an empty token and out-of-range values, which the bare
 * strtol pattern would silently accept. Leading and trailing ASCII whitespace is
 * allowed because SOAP text nodes may carry it.
 *
 * @param text Token to parse.
 * @param out  Receives the value.
 * @return 0 on success, -1 on invalid input.
 */
int rr_parse_long_strict(const char* text, long* out);

/**
 * @brief Copy a NUL-terminated string into a fixed-size field, truncating safely.
 *
 * Takes the destination size explicitly: sizeof() on a pointer parameter is both
 * wrong and a semgrep violation.
 *
 * @param dst    Destination buffer.
 * @param dst_sz Destination size in bytes.
 * @param src    Source string, or NULL to write an empty string.
 */
void rr_copy_field(char* dst, size_t dst_sz, const char* src);

/**
 * @brief Whether a RadioReference subscription expiry date has passed.
 *
 * The date arrives as MM-DD-YYYY. An UNPARSEABLE value is treated as valid, not
 * expired: RR answers "Never - Feed Provider" and "Never - Admin" for those
 * accounts, and locking them out would be worse than trusting them. Two days of
 * slack absorbs time-zone skew between client and server.
 *
 * Exposed here rather than kept static so it can be tested against a fixed clock.
 *
 * @param sub_expire        The subExpireDate string, or NULL.
 * @param now_epoch_seconds Current time as seconds since the Unix epoch.
 * @return 1 when definitely expired, 0 when valid or undeterminable.
 */
int rr_subscription_expired(const char* sub_expire, long long now_epoch_seconds);

/**
 * @brief Strip control bytes, replace commas and collapse whitespace runs.
 *
 * Commas become slashes because the group parser has no quoting at all; both
 * ends end up trimmed because a leading space would survive into the UI - the
 * importer trims the mode column but hands the name column through verbatim.
 * A filename stem built on this must UNDO the comma rewrite: see
 * dsd_rr_sanitize_file_stem() in rr_import.c.
 *
 * @param in     Source label.
 * @param out    Destination buffer.
 * @param out_sz Destination size in bytes, passed explicitly.
 * @return Length written, excluding the terminator.
 */
size_t rr_collapse_label(const char* in, char* out, size_t out_sz);

/**
 * @brief Longest prefix of `text` that fits in `limit` bytes and ends on a
 *        UTF-8 codepoint boundary.
 *
 * A byte-oriented cut would leave a lone lead byte in the file. An invalid lead
 * byte is treated as one byte, so malformed input still makes progress.
 *
 * @param text  Text to measure.
 * @param len   Text length.
 * @param limit Byte ceiling.
 * @return Prefix length in bytes.
 */
size_t rr_utf8_prefix(const char* text, size_t len, size_t limit);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_RUNTIME_RADIOREFERENCE_RR_INTERNAL_H */
