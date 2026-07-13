// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_TRELLIS34_H_
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_TRELLIS34_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t dsd_trellis_interleave_98[98];
extern const uint8_t dsd_trellis34_constellation[16];
extern const uint8_t dsd_trellis34_inverse_constellation[16];
extern const uint8_t dsd_trellis34_fsm[64];

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_TRELLIS34_H_ */
