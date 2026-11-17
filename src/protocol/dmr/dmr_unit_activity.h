// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR rows of the unit activity log (<dsd-neo/core/unit_activity.h>): Tier III C_ACKD answers and C_AHOY
 * commands (TS 102 361-4), and Motorola ARS registration reports.
 *
 * The callers decide provenance: a CSBK row comes only from a standard (FID 0x00) CSBK that passed its own CRC (never
 * one `-F` accepted as RAS, an MBC whose header failed, or a P_CLEAR the decoder synthesized), and an ARS row only from
 * a data PDU whose CRC passed and which is exactly one ARS message. Gateway addresses (Table A.8) are the actor's
 * INFRASTRUCTURE identity, never the subject, and a command row says only that the command was sent.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_UNIT_ACTIVITY_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_UNIT_ACTIVITY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The C_ACKD fields a row reads (TS 102 361-4 Table 7.23). */
typedef struct {
    uint8_t response_info; /**< Bits 16-22 (Table 7.41). */
    uint8_t reason;        /**< Bits 23-30 (Tables 7.42-7.45). */
    uint32_t target;       /**< Bits 32-55: the MS that made the request (Table 7.41: a talkgroup with G/I 1). */
    uint32_t source;       /**< Bits 56-79: the request's destination or a gateway (REGI for registration). */
} dmr_ua_ackd_fields;

/**
 * @brief Log a checked C_ACKD: a registration acknowledged (0x62 from REGI, no reason: it answers a registration and a
 * deregistration alike), refused (0x2A) or denied (0x2B) by REGI, a talkgroup subscription answer (0x65 from REGI,
 * the acceptance mask as the value), and any other refusal, queued or wait answer with its Tier III reason. Other
 * acknowledgements add no row.
 */
void dmr_ua_c_ackd(const dsd_opts* opts, dsd_state* state, const dmr_ua_ackd_fields* fields);

/**
 * @brief Log a checked C_AHOY of @p kind (a dmr_ahoy_kind, dmr_reason.h) as the command it is: stun, revive, kill,
 * radio check, authentication challenge (the challenge never becomes an identity) or talkgroup subscription list
 * request. The other kinds add no row. @p group is the PDU's G/I bit, @p target and @p source its address fields.
 */
void dmr_ua_c_ahoy(const dsd_opts* opts, dsd_state* state, int kind, uint8_t group, uint32_t target, uint32_t source);

/**
 * @brief Log an ARS message (MOTOTRBO ARS spec 3.4) of @p pdu_type sent by radio @p source on @p slot: a device or user
 * registration as a registration report, a device or user deregistration as a deregistration report. Other types add
 * no row.
 */
void dmr_ua_ars(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t pdu_type, uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_UNIT_ACTIVITY_H_ */
