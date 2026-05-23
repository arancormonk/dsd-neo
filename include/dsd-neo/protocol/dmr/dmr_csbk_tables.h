// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief DMR CSBK opcode and field lookup tables.
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR CSBK table helpers (opcode names, etc.).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_TABLES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_TABLES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return a short human-readable name for common CSBK opcodes used in
 * channel grant handling (ETSI 7.1.1.1.1).
 *
 * The returned pointer refers to a static string and must not be freed.
 * For unknown opcodes, returns "Unknown CSBK".
 */
const char* dmr_csbk_grant_opcode_name(uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_CSBK_TABLES_H_ */
