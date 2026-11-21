// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel grant classes (<dsd-neo/protocol/dmr/dmr_t3_grant.h>).
 */

#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/safe_api.h"

static uint32_t
dmr_t3_grant_bits24(const uint8_t* bits) {
    uint32_t value = 0U;
    for (size_t i = 0; i < 24U; i++) {
        value = (value << 1) | (uint32_t)(bits[i] & 1U);
    }
    return value;
}

/* The class sets as bit masks over CSBKO - 48: group {49, 50, 52, 56}, data {51, 52, 54, 55, 56}, duplex {53, 54}. */
enum {
    DMR_T3_GRANT_FIRST = 48,
    DMR_T3_GRANT_LAST = 56,
    DMR_T3_GRANT_GROUP_MASK = (1U << 1) | (1U << 2) | (1U << 4) | (1U << 8),
    DMR_T3_GRANT_DATA_MASK = (1U << 3) | (1U << 4) | (1U << 6) | (1U << 7) | (1U << 8),
    DMR_T3_GRANT_DUPLEX_MASK = (1U << 5) | (1U << 6),
};

static int
dmr_t3_grant_is_grant(int csbk_o, int outbound) {
    if (csbk_o < DMR_T3_GRANT_FIRST || csbk_o > DMR_T3_GRANT_LAST) {
        return 0;
    }
    return csbk_o != DMR_T3_GRANT_LAST || outbound;
}

int
dmr_t3_grant_classify(int csbk_o, const uint8_t* cs_pdu_bits, int outbound, dmr_t3_grant_class* out) {
    if (out == NULL) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (cs_pdu_bits == NULL || !dmr_t3_grant_is_grant(csbk_o, outbound)) {
        return 0;
    }
    const unsigned bit = 1U << (unsigned)(csbk_o - DMR_T3_GRANT_FIRST);
    out->group = (uint8_t)((DMR_T3_GRANT_GROUP_MASK & bit) != 0U);
    out->data = (uint8_t)((DMR_T3_GRANT_DATA_MASK & bit) != 0U);
    const uint32_t target = dmr_t3_grant_bits24(&cs_pdu_bits[32]);
    const uint32_t source = dmr_t3_grant_bits24(&cs_pdu_bits[56]);
    const int target_is_caller = (DMR_T3_GRANT_DUPLEX_MASK & bit) != 0U && cs_pdu_bits[31] != 0U;
    out->caller = target_is_caller ? target : source;
    out->recipient = target_is_caller ? source : target;
    return 1;
}
