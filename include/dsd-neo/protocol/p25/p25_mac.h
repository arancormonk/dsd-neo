// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Minimal P25 MAC helpers exposed for tests and diagnostics.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns the MAC message length for a given opcode with vendor overrides.
// Length semantics match the internal table: number of octets following the
// opcode byte (i.e., includes MFID and payload, excludes the opcode itself).
int p25p2_mac_len_for(uint8_t mfid, uint8_t opcode);

#ifdef __cplusplus
}
#endif
