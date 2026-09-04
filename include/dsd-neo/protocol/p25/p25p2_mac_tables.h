// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 Phase 2 MAC opcode/field lookup tables.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC opcode length table and vendor overrides.
 *
 * Exposes p25p2_mac_len_for for callers that need to determine the
 * message-carrying octet length for a given (mfid, opcode) pair.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_TABLES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_TABLES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns the MAC message length for a given opcode with vendor overrides.
 *
 * Length semantics match the MAC structure length used by the Phase 2 parser:
 * whole structure octets, including the opcode byte.
 */
int p25p2_mac_len_for(uint8_t mfid, uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25P2_MAC_TABLES_H_ */
