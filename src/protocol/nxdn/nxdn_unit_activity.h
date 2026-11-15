// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief NXDN Type-C mobility management and remote-control messages (NXDN TS 1-A v1.3 clause 6.4) decoded for the
 * unit activity log (<dsd-neo/core/unit_activity.h>): REG_RESP 0x20, REG_C_RESP 0x22, GRP_REG_RESP 0x24,
 * REM_CON_REQ 0x34 and REM_CON_RESP 0x35.
 *
 * NXDN_Elements_Content_decode() (which only a CRC-checked CAC, FACCH or SACCH reaches) hands each of them to
 * nxdn_ua_dispatch(). It decodes only on the Type-C control channel path (a CAC, or a composite control channel once
 * Type-C trunking decoded): a Type-D (IDAS) message under the same type code has another layout (TS 1-E 6.4), and is
 * not parsed. A message shorter than its mandatory part is refused, and an optional visitor field is read only when
 * its option bit is set and it arrived. It prints the decoded fields and pushes one row.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_UNIT_ACTIVITY_H_
#define DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_UNIT_ACTIVITY_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode @p message_type (the 6-bit type of octet 0) when it is one of the five unit activity messages, and
 * push its row:
 * - REG_RESP (Fig 6.4-45): a registration response for the Destination Unit, its home Location ID as the subject's
 *   home, the Group ID and, for a visitor, the Visitor Unit ID (assigned) and Visitor Group ID (value2);
 * - REG_C_RESP (Fig 6.4-47): a deregistration response ("Logout response");
 * - GRP_REG_RESP (Fig 6.4-50): a talkgroup join response, with the Visitor Group ID when sent;
 * - REM_CON_REQ (Fig 6.4-20): a stun / revive / kill / remote monitor command; the destination (a group or a unit, by
 *   G/U) is the subject and the source the actor, the Control Parameter keyed with it;
 * - REM_CON_RESP (Fig 6.4-21): a remote-control response to stun / revive / kill; the source (the controlled unit) is
 *   the subject, the destination (the requester) the recipient, Cause (SS) the reason.
 *
 * @return 1 when @p message_type is one of them (handled, whether a row was pushed or not), 0 otherwise.
 */
int nxdn_ua_dispatch(const dsd_opts* opts, dsd_state* state, uint8_t message_type, const uint8_t* elements,
                     size_t elements_bits);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_NXDN_NXDN_UNIT_ACTIVITY_H_ */
