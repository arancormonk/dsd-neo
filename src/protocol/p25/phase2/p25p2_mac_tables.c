// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC opcode length table and vendor overrides.
 */

#include <stdint.h>

#include <dsd-neo/protocol/p25/p25p2_mac_tables.h>

/*
 * Length semantics: whole MAC structure octets, including the opcode byte.
 *
 * The standard table follows sdrtrunk's MacOpcode lengths for fixed-length
 * structures. Vendor partition opcodes are intentionally handled in
 * vendor_len_for() so Motorola/Harris/Tait opcodes do not collide by opcode
 * number alone.
 */
static const uint8_t standard_mac_msg_len[256] = {
    [0x01] = 7,  [0x02] = 8,  [0x03] = 7,  [0x05] = 16, [0x21] = 14, [0x22] = 15, [0x25] = 15, [0x30] = 5,  [0x31] = 7,

    [0x40] = 9,  [0x41] = 7,  [0x42] = 9,  [0x43] = 9,  [0x44] = 9,  [0x45] = 10, [0x46] = 9,  [0x48] = 10, [0x49] = 10,
    [0x4A] = 7,  [0x4C] = 10, [0x52] = 8,  [0x53] = 9,  [0x54] = 9,  [0x55] = 7,  [0x58] = 10, [0x5A] = 7,  [0x5C] = 10,
    [0x5D] = 8,  [0x5E] = 14, [0x5F] = 7,  [0x60] = 9,  [0x61] = 9,  [0x64] = 9,  [0x67] = 9,  [0x68] = 10, [0x6A] = 7,
    [0x6B] = 10, [0x6C] = 10, [0x6D] = 7,  [0x6F] = 9,  [0x70] = 9,  [0x71] = 18, [0x72] = 9,  [0x73] = 9,  [0x74] = 9,
    [0x75] = 9,  [0x76] = 10, [0x77] = 13, [0x78] = 9,  [0x79] = 9,  [0x7A] = 9,  [0x7B] = 11, [0x7C] = 9,  [0x7D] = 9,

    [0x88] = 5,  [0x90] = 7,

    [0xC0] = 11, [0xC3] = 8,  [0xC4] = 15, [0xC5] = 14, [0xC6] = 15, [0xC7] = 18, [0xC8] = 12, [0xC9] = 12, [0xCB] = 18,
    [0xCC] = 14, [0xCD] = 18, [0xCE] = 18, [0xCF] = 18, [0xD6] = 9,  [0xD8] = 14, [0xD9] = 18, [0xDA] = 11, [0xDB] = 18,
    [0xDC] = 14, [0xDE] = 18, [0xDF] = 11, [0xE0] = 18, [0xE4] = 17, [0xE5] = 14, [0xE8] = 16, [0xE9] = 8,  [0xEA] = 11,
    [0xEC] = 13, [0xF1] = 18, [0xF2] = 16, [0xF3] = 14, [0xFA] = 11, [0xFB] = 13, [0xFC] = 11, [0xFE] = 15,
};

static const uint8_t motorola_mac_msg_len[256] = {
    [0x80] = 8,  [0x81] = 17, [0x83] = 7,  [0x84] = 11, [0x85] = 9,  [0x89] = 17, [0x91] = 17, [0x95] = 17,
    [0xA0] = 16, [0xA3] = 11, [0xA4] = 13, [0xA5] = 11, [0xA6] = 11, [0xA7] = 11, [0xA8] = 10,
};

static const uint8_t harris_mac_msg_len[256] = {
    [0xA0] = 9,
    [0xAA] = 17,
    [0xAC] = 12,
};

static const uint8_t tait_mac_msg_len[256] = {
    [0xB5] = 5,
};

static inline int
base_len_for(uint8_t opcode) {
    return standard_mac_msg_len[opcode];
}

static int
vendor_len_for(uint8_t mfid, uint8_t opcode) {
    if (mfid == 0x90u) {
        return motorola_mac_msg_len[opcode];
    }

    if (mfid == 0xA4u) {
        return harris_mac_msg_len[opcode];
    }

    if (mfid == 0xD8u) {
        return tait_mac_msg_len[opcode];
    }

    return 0;
}

int
p25p2_mac_len_for(uint8_t mfid, uint8_t opcode) {
    int vendor = vendor_len_for(mfid, opcode);
    if (vendor != 0) {
        return vendor;
    }

    int base = base_len_for(opcode);
    if (base != 0) {
        return base;
    }

    return 0;
}
