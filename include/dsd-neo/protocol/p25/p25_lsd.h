// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_PROTOCOL_P25_P25_LSD_H
#define DSD_NEO_PROTOCOL_P25_P25_LSD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * P25p1 Low Speed Data FEC (16,8) — scaffold API.
 *
 * bits16: array of 16 bits stored as bytes 0/1 (MSB first). The function may
 * correct single-bit errors and return 1 for valid/corrected frames, or 0 for
 * uncorrectable.
 *
 * This is a placeholder to be replaced with a spec-accurate cyclic decoder.
 */
int p25_lsd_fec_16x8(uint8_t* bits16);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_P25_P25_LSD_H */
