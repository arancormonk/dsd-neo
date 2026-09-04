// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_DBURST_PROFILE_H
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_DBURST_PROFILE_H

#include <stdint.h>

enum {
    DMR_DBURST_F_BPTC = 1U << 0U,
    DMR_DBURST_F_TRELLIS = 1U << 1U,
    DMR_DBURST_F_EMB = 1U << 2U,
    DMR_DBURST_F_LC = 1U << 3U,
    DMR_DBURST_F_FULL = 1U << 4U,
    DMR_DBURST_F_UDT = 1U << 7U,
};

typedef struct {
    const char* subtype;
    uint32_t crcmask;
    uint8_t flags;
    uint8_t crclen;
    uint8_t pdu_len;
    uint8_t pdu_start;
} dmr_dburst_profile;

void dmr_dburst_profile_resolve(uint8_t databurst, uint8_t confirmed_data, uint8_t header_format,
                                dmr_dburst_profile* out);

#endif
