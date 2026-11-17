// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Motorola MOTOTRBO Automatic Registration Service (ARS) message decode.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_ARS_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_ARS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dmr_ars_print_message(dsd_state* state, const uint8_t* msg, uint16_t len);

/**
 * @brief Whether `msg` (size prefix first, `len` octets) is exactly one ARS message as spec section 3.4
 *        defines it: the size covers the rest of `len`, Priority and Control are set, the PDU type
 *        is defined, the Ext/Ack bits and second header are the ones that type allows, and the
 *        payload ends where the size says, optionally followed by the CSBK capability 0x10 0x80.
 *
 * Used where the payload is the only evidence that a datagram is ARS (a manufacturer-specific
 * compressed UDP port index with no configured port); the port-4005 paths keep the lenient decoder.
 */
int dmr_ars_record_is_exact(const uint8_t* msg, uint16_t len);

/**
 * @brief Log an ARS registration or deregistration sent by radio `source` on `slot` in the unit activity log
 *        (dmr_unit_activity.h): only on DMR, only from a data PDU whose CRC passed on that slot (no `-F` relaxed one,
 *        the slot's event_crc_invalid clear, as the LRRP gate reads it), and only for a message
 *        dmr_ars_record_is_exact() accepts. The print (dmr_ars_print_message()) is unchanged.
 */
void dmr_ars_note_activity(const dsd_opts* opts, dsd_state* state, uint8_t slot, const uint8_t* msg, uint16_t len,
                           uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_ARS_H_ */
