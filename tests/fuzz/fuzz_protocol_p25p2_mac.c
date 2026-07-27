// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Drives both the pure MAC parser and the real VPDU processing path.
 *
 * The parser alone never reaches the Motorola talker-alias handlers, where an
 * unvalidated over-the-air length octet drove the unpack that overflowed the
 * staging buffer (opcodes 0x91 and 0x95 under MFID 0x90). Running the shim means
 * a malformed length reaches the same code an over-the-air frame would.
 */

#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>

#include "fuzz_support.h"
#include "p25_test_shim.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    unsigned long long mac[24];
    for (size_t i = 0; i < 24U; ++i) {
        mac[i] = i < size ? (unsigned long long)data[i] : 0ULL;
    }

    int type = (size > 24U) ? (int)(data[24] & 0x01U) : 0;
    int pos = (size > 25U) ? (int)(data[25] % 24U) : 0;

    struct p25p2_mac_result mac_result;
    DSD_MEMSET(&mac_result, 0, sizeof(mac_result));
    (void)p25p2_mac_parse(type, mac, &mac_result);

    struct p25p2_iden_update iden;
    DSD_MEMSET(&iden, 0, sizeof(iden));
    (void)p25p2_mac_decode_iden_standard(mac, pos, &iden);
    (void)p25p2_mac_decode_iden_vuhf(mac, pos, &iden);
    (void)p25p2_mac_decode_iden_tdma(mac, pos, &iden);

    /* Now the real thing: the same octets through process_MAC_VPDU(). */
    unsigned char mac_bytes[24];
    for (size_t i = 0; i < sizeof(mac_bytes); ++i) {
        mac_bytes[i] = i < size ? data[i] : 0U;
    }

    const int is_lcch = (size > 26U) ? (int)(data[26] & 0x01U) : 0;
    const int currentslot = (size > 27U) ? (int)(data[27] & 0x01U) : 0;
    const int mac_len = (size > 28U) ? (int)(data[28] % (int)(sizeof(mac_bytes) + 1U)) : (int)sizeof(mac_bytes);

    p25_test_process_mac_vpdu_ex(type, mac_bytes, mac_len, is_lcch, currentslot);

    return 0;
}
