// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The channel numbers a DMR trunked call is named by: a Tier III LPCN, and the Hytera XPT repeater (LCN) to logical slot
 * (LSN) rule. Module-private.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_CALL_CHANNEL_H_H
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_CALL_CHANNEL_H_H

#include <stdint.h>

/* Whether @p lpcn names a Tier III channel. ETSI TS 102 361-4 §7.1.1.1.1.1.1 and the flexible channel plan: CHAN is
   1..4094; 0 is CHNULL and 4095 (0xFFF) the escape whose channel an appended MBC's CdefParms define. The same rule
   guards a grant, a C_MOVE, the CdefParms LPCN and the channel map they teach. */
static inline int
dmr_t3_lpcn_is_channel(uint32_t lpcn) {
    return lpcn >= 1U && lpcn <= 4094U;
}

/* The LSN of Hytera XPT repeater (LCN) @p lcn: its first slot, LSN 2n - 1, as the site-status bank and the LSN-keyed
   channel map number them. LCN 0 names no repeater: 0. */
static inline uint8_t
dmr_xpt_lcn_to_lsn(uint8_t lcn) {
    return lcn == 0U ? 0U : (uint8_t)((2U * (unsigned)lcn) - 1U);
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_CALL_CHANNEL_H_H */
