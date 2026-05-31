// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H

#include <stdint.h>

static inline uint16_t
dmr_tiii_model_default_split_n(uint8_t model) {
    switch (model & 0x03U) {
        case 0: return 3U;
        case 1: return 5U;
        case 2: return 8U;
        default: return 10U;
    }
}

static inline uint16_t
dmr_tiii_effective_split_n(uint16_t default_n, uint8_t override_is_set, uint8_t override_n, uint16_t site_bits) {
    uint16_t n = override_is_set ? (uint16_t)override_n : default_n;
    if (n > site_bits) {
        n = site_bits;
    }
    return n;
}

static inline uint16_t
dmr_tiii_subsite_mask(uint16_t n) {
    return (n == 0U) ? 0U : (uint16_t)((1U << n) - 1U);
}

static inline uint16_t
dmr_tiii_display_net(uint16_t net, uint16_t n) {
    return (n == 0U) ? net : (uint16_t)(net + 1U);
}

static inline uint16_t
dmr_tiii_display_site(uint16_t site, uint16_t n) {
    return (n == 0U) ? site : (uint16_t)((site >> n) + 1U);
}

static inline uint16_t
dmr_tiii_display_subsite(uint16_t site, uint16_t sub_mask, uint16_t n) {
    return (n == 0U) ? 0U : (uint16_t)((site & sub_mask) + 1U);
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H */
