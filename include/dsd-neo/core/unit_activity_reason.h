// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The reason texts of trunking responses (one table per protocol reason field) and the TETRA location update
 * accept types, shared by the unit activity view and the protocol decoders' own prints.
 *
 * A unit activity row stores a reason as two numbers, dsd_unit_activity::reason_set (a dsd_ua_reason_set) and
 * dsd_unit_activity::reason_code; dsd_ua_reason_text() turns them into text. A decoder that prints the same reason
 * calls the same lookup, so the log and the print never disagree.
 *
 * Sources: P25 TIA-102.AABC Annex B (deny) and Annex C (queued) reason codes, in the words dsd-neo's P25 prints have
 * always used;
 * DMR Tier III TS 102 361-4 clause 7.2.8 Tables 7.42-7.45 (C_ACK, C_NACK, C_QACK, C_WACK, plus
 * MS_Duplex_Not_Supported 0x16 from the clause 7.2.8.2 text) and Tier II TS 102 361-2 Table 7.13 (NACK_Rsp), kept
 * apart because 0x21 means different things in each; NXDN TS 1-A Table 6.5-27 Cause (MM) and Table 6.5-29 Cause (SS);
 * TETRA EN 300 392-2 Table 16.81 (location update reject cause) and Table 16.68 (location update accept type).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_REASON_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_REASON_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** dsd_unit_activity::reason_set: which table a reason code is read in. The values are stable. */
typedef enum {
    DSD_UA_REASON_NONE = 0,            /**< The row carries no reason. */
    DSD_UA_REASON_P25_DENY = 1,        /**< P25 Deny Response reason code (8 bits). */
    DSD_UA_REASON_P25_QUEUED = 2,      /**< P25 Queued Response reason code (8 bits). */
    DSD_UA_REASON_DMR_TIER3 = 3,       /**< DMR Tier III C_ACKD / C_ACKU / P_ACKD / P_ACKU Reason (8 bits). */
    DSD_UA_REASON_DMR_TIER2_NACK = 4,  /**< DMR Tier II NACK_Rsp Reason Code (8 bits). */
    DSD_UA_REASON_NXDN_MM = 5,         /**< NXDN Cause (MM): REG_RESP, REG_C_RESP, GRP_REG_RESP (7 bits). */
    DSD_UA_REASON_NXDN_SS = 6,         /**< NXDN Cause (SS): STAT_INQ_RESP, STAT_RESP, REM_CON_RESP (7 bits). */
    DSD_UA_REASON_TETRA_LU_REJECT = 7, /**< TETRA D-LOCATION UPDATE REJECT reject cause (5 bits). */
} dsd_ua_reason_set;

/**
 * @brief The text of reason @p code in table @p reason_set.
 *
 * P25 codes outside the named ones read as their range ("Reserved", "User/System Defined"), as the P25 prints always
 * showed them. Every other table names only the values its standard defines.
 *
 * @return A static plain-English text, or NULL for DSD_UA_REASON_NONE, an unknown set, or a code the table does not
 *         define (print the raw value then).
 */
const char* dsd_ua_reason_text(uint8_t reason_set, uint16_t code);

/**
 * @brief The name of TETRA location update accept type @p type (EN 300 392-2 Table 16.68), the value of a TETRA
 * location registration row ("ITSI attach", "Periodic location updating", ...).
 *
 * @return A static text, or NULL for a value above 7 (a 3-bit field: print the raw value then).
 */
const char* dsd_ua_tetra_accept_type_text(uint32_t type);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_UNIT_ACTIVITY_REASON_H_ */
