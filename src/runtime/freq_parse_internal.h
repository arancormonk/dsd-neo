// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Private helpers shared by the runtime's frequency parsers (freq_parse.c, input_spec.c).
 */

#ifndef DSD_NEO_RUNTIME_FREQ_PARSE_INTERNAL_H
#define DSD_NEO_RUNTIME_FREQ_PARSE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether text[0..len) is plain decimal: [0-9]*('.'[0-9]*)? with at least one digit. When it is, *out_hz is that
 * value times 10^places, rounded half up to a whole Hz in exact integer arithmetic, or 0 when that is 0 or above
 * UINT32_MAX. Returns 0, leaving *out_hz, for any other text (a sign, an exponent, hex, inf/nan, a space); the caller
 * keeps its floating-point path for those. places is 0, 3, 6 or 9.
 */
int dsd_freq_decimal_round_hz(const char* text, size_t len, unsigned places, uint32_t* out_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_FREQ_PARSE_INTERNAL_H */
