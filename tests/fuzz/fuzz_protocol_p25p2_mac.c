// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>

#include "fuzz_support.h"

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

    return 0;
}
