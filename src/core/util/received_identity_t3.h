// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tier III system code fields for the received identity record (core only; the DMR decoder keeps its own). The 14-bit
 * code is MODEL (2 bits), NET and SITE, and the model sets the NET and SITE widths (TS 102 361-4 §6.3.2.2.1.1, Table
 * 6.5): Tiny 9 and 3 bits, Small 7 and 5, Large 4 and 8, Huge 2 and 10. libc only, as the site module that uses it.
 */

#ifndef DSD_NEO_SRC_CORE_UTIL_RECEIVED_IDENTITY_T3_H_
#define DSD_NEO_SRC_CORE_UTIL_RECEIVED_IDENTITY_T3_H_

#include <stdint.h>

/* The largest Tier III system code (14 bits). */
#define RXID_T3_SYSCODE_MAX 0x3FFFU

typedef struct {
    uint8_t model; /* 0 Tiny, 1 Small, 2 Large, 3 Huge */
    uint16_t net;  /* raw */
    uint16_t site; /* raw, the whole SITE field */
} rxid_t3_code;

/* The SITE width of model @p model; the NET width is what the 12 bits below MODEL leave. */
static inline unsigned
rxid_t3_site_bits(uint8_t model) {
    static const unsigned k_site_bits[4] = {3U, 5U, 8U, 10U};
    return k_site_bits[model & 0x3U];
}

static inline uint16_t
rxid_t3_net_max(uint8_t model) {
    return (uint16_t)((1U << (12U - rxid_t3_site_bits(model))) - 1U);
}

static inline uint16_t
rxid_t3_site_max(uint8_t model) {
    return (uint16_t)((1U << rxid_t3_site_bits(model)) - 1U);
}

/* The MODEL, NET and SITE of 14-bit system code @p syscode (bits above 14 ignored). */
static inline rxid_t3_code
rxid_t3_decode(uint16_t syscode) {
    rxid_t3_code c;
    c.model = (uint8_t)((syscode >> 12) & 0x3U);
    const unsigned site_bits = rxid_t3_site_bits(c.model);
    c.net = (uint16_t)((syscode >> site_bits) & rxid_t3_net_max(c.model));
    c.site = (uint16_t)(syscode & rxid_t3_site_max(c.model));
    return c;
}

#endif /* DSD_NEO_SRC_CORE_UTIL_RECEIVED_IDENTITY_T3_H_ */
