// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/protocol/m17/m17_parse.h>

#include "fuzz_support.h"

uint64_t
ConvertBitIntoBytes(const uint8_t* bits, uint32_t n) {
    uint64_t value = 0ULL;
    for (uint32_t i = 0; i < n; ++i) {
        value = (value << 1) | (uint64_t)(bits[i] & 0x01U);
    }
    return value;
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    uint8_t bits[240];
    DSD_MEMSET(bits, 0, sizeof(bits));
    for (size_t i = 0; i < sizeof(bits); ++i) {
        size_t byte_index = i / 8U;
        if (byte_index >= size) {
            break;
        }
        bits[i] = (uint8_t)((data[byte_index] >> (7U - (i % 8U))) & 0x01U);
    }

    struct m17_lsf_result result;
    (void)m17_parse_lsf(bits, sizeof(bits), &result);
    return 0;
}
