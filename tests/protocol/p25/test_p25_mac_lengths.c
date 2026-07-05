// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused checks for P25 Phase 2 MAC opcode length table and vendor overrides. */

#include <dsd-neo/protocol/p25/p25p2_mac_tables.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Core standard opcodes. Values are whole MAC structure octets, including opcode.
    rc |= expect_eq("OP 0x40 (GRP_V_CH_GRANT)", p25p2_mac_len_for(0x01, 0x40), 9);
    rc |= expect_eq("OP 0x41 (GRP_V_SVC_REQ)", p25p2_mac_len_for(0x01, 0x41), 7);
    rc |= expect_eq("OP 0x45 (UU_ANS_REQ_ABBR)", p25p2_mac_len_for(0x01, 0x45), 10);
    rc |= expect_eq("OP 0x4A (TELE_ANS_RSP)", p25p2_mac_len_for(0x01, 0x4A), 7);
    rc |= expect_eq("OP 0x52 (SNDCP_REQ)", p25p2_mac_len_for(0x01, 0x52), 8);
    rc |= expect_eq("OP 0x53 (SNDCP_PAGE_RSP)", p25p2_mac_len_for(0x01, 0x53), 9);
    rc |= expect_eq("OP 0x71 (AUTH_DEMAND base fragment)", p25p2_mac_len_for(0x01, 0x71), 18);
    rc |= expect_eq("OP 0xC0 (GRP_V_CH_GRANT_EXP)", p25p2_mac_len_for(0x01, 0xC0), 11);
    rc |= expect_eq("OP 0xC3 (GRP_V_CH_GRANT_UP_EXP)", p25p2_mac_len_for(0x01, 0xC3), 8);
    rc |= expect_eq("OP 0xC5 (UU_ANS_REQ_EXT)", p25p2_mac_len_for(0x01, 0xC5), 14);
    rc |= expect_eq("OP 0xC7 (UU_GRANT_UP_EXT_LCCH base fragment)", p25p2_mac_len_for(0x01, 0xC7), 18);
    rc |= expect_eq("OP 0xCB (CALL_ALERT_EXT_LCCH base fragment)", p25p2_mac_len_for(0x01, 0xCB), 18);
    rc |= expect_eq("OP 0xDE (RUM_ENH_EXT base fragment)", p25p2_mac_len_for(0x01, 0xDE), 18);

    // Vendor overrides
    rc |= expect_eq("Moto 0x80", p25p2_mac_len_for(0x90, 0x80), 8);
    rc |= expect_eq("Moto 0x81", p25p2_mac_len_for(0x90, 0x81), 17);
    rc |= expect_eq("Moto 0x89", p25p2_mac_len_for(0x90, 0x89), 17);
    rc |= expect_eq("Moto 0x91", p25p2_mac_len_for(0x90, 0x91), 17);
    rc |= expect_eq("Moto 0x95", p25p2_mac_len_for(0x90, 0x95), 17);
    rc |= expect_eq("Moto 0xA0", p25p2_mac_len_for(0x90, 0xA0), 16);
    rc |= expect_eq("Harris 0xA0", p25p2_mac_len_for(0xA4, 0xA0), 9);
    rc |= expect_eq("Harris 0xAA", p25p2_mac_len_for(0xA4, 0xAA), 17);
    rc |= expect_eq("Harris 0xAC", p25p2_mac_len_for(0xA4, 0xAC), 12);
    rc |= expect_eq("Tait 0xB5", p25p2_mac_len_for(0xD8, 0xB5), 5);
    rc |= expect_eq("Tait unknown fixed table fallback", p25p2_mac_len_for(0xD8, 0xB4), 0);

    return rc;
}
