// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel grant classes (TS 102 361-4 §7.1.1.1.1, Tables 7.9-7.15; opcodes Table B.1).
 *
 * A grant is a group or a private call, and a voice or a data call, the two kept apart:
 *
 *   opcode  name          class
 *   48      PV_GRANT      private voice
 *   49      TV_GRANT      group voice
 *   50      BTV_GRANT     group voice (broadcast)
 *   51      PD_GRANT      private data, single item
 *   52      TD_GRANT      group data, single item
 *   53      PV_GRANT_DX   private voice, duplex
 *   54      PD_GRANT_DX   private data, duplex
 *   55      PD_GRANT      private data, multi item
 *   56      TD_GRANT      group data, multi item (outbound only: inbound, on MS-sourced data, 56 is BS_Dwn_Act)
 *
 * Bits 32-55 name the target and 56-79 the source. For a duplex grant (53, 54) the target is the MS the grant is for
 * and the source its call partner, and Call Direction (bit 31) says whether that target is the called MS (0) or the
 * calling MS (1). Every other grant's target is the called party (a talkgroup for a group call) and its source the
 * caller.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_GRANT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_GRANT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A channel grant's class and parties: the calling MS and the called MS or talkgroup. */
typedef struct {
    uint8_t group;
    uint8_t data;
    uint32_t caller;
    uint32_t recipient;
} dmr_t3_grant_class;

/**
 * Whether CSBKO @p csbk_o is a channel grant opcode (48..56); @p outbound is nonzero for a BS-sourced PDU. 56 is a
 * grant only outbound: inbound, on MS-sourced data, it is BS_Dwn_Act.
 */
int dmr_t3_grant_is_grant_opcode(int csbk_o, int outbound);

/**
 * Classify CSBKO @p csbk_o with its PDU bits @p cs_pdu_bits (at least 80) as a channel grant; @p outbound is nonzero
 * for a BS-sourced PDU. Returns 1 with @p out filled for a grant, 0 (with @p out zeroed) for anything else, a NULL
 * argument included.
 */
int dmr_t3_grant_classify(int csbk_o, const uint8_t* cs_pdu_bits, int outbound, dmr_t3_grant_class* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_DMR_DMR_T3_GRANT_H_ */
