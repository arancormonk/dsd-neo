// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Readout text for live numbers: no negative zero, and fixed widths for status rows.
 *
 * printf prints "-0.0" for a negative value that rounds to zero (C11 7.21.6.1, footnote 276). These helpers format
 * with the platform printf, so every non-zero reading keeps its native rounding, and drop the sign only when every
 * digit of the result is zero. The rule works on the text, so it holds under -ffast-math, where `x + 0.0` and isnan()
 * are folded away.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NUMBER_TEXT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NUMBER_TEXT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Print a sign on every value: "+1.5", "+0.0", "-1.5" (printf's `+` flag). */
#define DSD_NUMBER_TEXT_PLUS     1u
/** Never wider than `width`: a value that does not fit prints as ">999" or "<-99" (see dsd_number_text()). */
#define DSD_NUMBER_TEXT_SATURATE 2u

/**
 * @brief Drop the minus sign from printf "%f" text whose digits are all zero.
 *
 * The text is a zero reading when, after an optional leading sign, it holds at least one '0', no digit 1-9 and no
 * ASCII letter. Any radix passes ('.', ',' or a multibyte one such as U+066B), since "%f" writes only a sign, digits
 * and the locale's radix; "nan" and "inf" hold letters and are left alone. A zero reading loses its '-', or gets '+'
 * in its place under DSD_NUMBER_TEXT_PLUS.
 *
 * @param text NUL-terminated text, rewritten in place (it can only get shorter or keep its length).
 * @param flags DSD_NUMBER_TEXT_PLUS or 0.
 * @return 1 when the sign was changed, 0 when not, -1 when text is NULL.
 */
int dsd_number_text_drop_negative_zero(char* text, unsigned flags);

/**
 * @brief `value` as "%[+]*.*f", right-aligned in `width`, without a negative zero.
 *
 * Under DSD_NUMBER_TEXT_SATURATE a value whose text is longer than `width` prints as '>' (above zero) or '<' (below)
 * followed by the largest number of that sign and precision that fits the remaining width ("<-99", ">+9999.9"); when
 * no number fits, the marker fills the width. Text that is not a number ("nan", "inf") is cut to `width`. Widths
 * count bytes, as printf's do.
 *
 * @param out Destination, always NUL-terminated when out_size > 0.
 * @param out_size Capacity of out in bytes.
 * @param value The reading.
 * @param width Field width, 0..63.
 * @param precision Digits after the radix, 0..9.
 * @param flags DSD_NUMBER_TEXT_PLUS and/or DSD_NUMBER_TEXT_SATURATE.
 * @return Length of the full text (as snprintf: it may exceed out_size - 1), or -1 for out == NULL, out_size == 0, a
 *         width or precision out of range, SATURATE with width 0, or a formatting error.
 */
int dsd_number_text(char* out, size_t out_size, double value, int width, int precision, unsigned flags);

/**
 * @brief A packet or event count in at most six characters, right-aligned in `width`.
 *
 * 0..999999 print as digits; larger counts as tenths with a unit letter, truncated toward zero: "1.0M", "999.9M",
 * "4.2G", then T, P and E (UINT64_MAX is "18.4E").
 *
 * @return Length of the full text (as snprintf), or -1 for out == NULL, out_size == 0 or a width outside 0..63.
 */
int dsd_count_text(char* out, size_t out_size, unsigned long long count, int width);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_NUMBER_TEXT_H_ */
