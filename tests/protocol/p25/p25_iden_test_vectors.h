// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
#ifndef DSD_NEO_TEST_P25_IDEN_VECTORS_H
#define DSD_NEO_TEST_P25_IDEN_VECTORS_H

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>

/* Independent wire encoders: the LCW inputs are octets unpacked into MSB-first bits. */
static inline void
iden_test_lcw(dsd_opts* opts, dsd_state* state, int format, uint32_t base, int spacing) {
    uint8_t octets[9] = {0x58, 0x43, 0x25, 0xE0, 0x64, 0x09, 0x2A, 0xD2, 0x22};
    octets[0] = (uint8_t)format;
    if (format == 0x59) {
        octets[1] = 0x45;
        octets[2] = 0x81;
        octets[3] = 0x40; // +80 channel spacings
    }
    octets[3] = (uint8_t)((octets[3] & 0xFC) | ((spacing >> 8) & 3));
    octets[4] = (uint8_t)spacing;
    for (int i = 0; i < 4; i++) {
        octets[5 + i] = (uint8_t)(base >> (24 - 8 * i));
    }
    uint8_t bits[96] = {0};
    for (int i = 0; i < 72; i++) {
        bits[i] = (uint8_t)((octets[i / 8] >> (7 - i % 8)) & 1);
    }
    p25_lcw(opts, state, bits, 0);
}

static inline void
iden_test_mac(dsd_opts* opts, dsd_state* state, int opcode, int iden, int type, uint32_t base, int spacing,
              unsigned long long wacn, unsigned long long sysid, int crc_ok) {
    unsigned long long mac[24] = {0};
    mac[0] = 7;
    mac[1] = (unsigned)opcode;
    const int p = opcode == 0xF3 ? 3 : 2;
    const unsigned offset = type > 2 ? 2400U : 80U;
    mac[p] = (unsigned)((iden << 4) | type);
    mac[p + 1] = 0x80U | (offset >> 6);
    mac[p + 2] = ((offset & 63U) << 2) | ((unsigned)spacing >> 8);
    if (opcode == 0x7D) {
        mac[p] = (unsigned)((iden << 4) | 3);
        mac[p + 1] = 0x25; // bandwidth 100, positive offset 120
        mac[p + 2] = 0xE0U | ((unsigned)spacing >> 8);
    }
    mac[p + 3] = (unsigned)spacing & 255U;
    for (int i = 0; i < 4; i++) {
        mac[p + 4 + i] = (base >> (24 - 8 * i)) & 255U;
    }
    if (opcode == 0xF3) {
        mac[11] = (wacn >> 12) & 255U;
        mac[12] = (wacn >> 4) & 255U;
        mac[13] = ((wacn & 15U) << 4) | ((sysid >> 8) & 15U);
        mac[14] = sysid & 255U;
    }
    if (crc_ok) {
        process_MAC_VPDU(opts, state, 0, P25_MAC_PDU_SIGNAL, mac);
    } else {
        process_MAC_VPDU_crc(opts, state, 0, P25_MAC_PDU_SIGNAL, mac, 0);
    }
}
#endif
