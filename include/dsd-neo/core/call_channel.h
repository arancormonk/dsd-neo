// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The kind of channel number a DMR trunked call was followed on (its LSN, LPCN or LCN).
 *
 * A call's channel number means something only with the numbering it was named in: Capacity Plus and Hytera XPT name
 * logical slot numbers (LSN), Tier III logical physical channel numbers (LPCN), Connect Plus logical channel numbers
 * (LCN). Other protocols keep their channel number with kind NONE.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_CHANNEL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_CHANNEL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The numbering a call's channel was named in. The values are stable: history rows, the control API and the Qt
 * history store keep them. */
typedef enum {
    DSD_CALL_CHANNEL_NONE = 0, /**< No DMR channel numbering (another protocol, or no channel). */
    DSD_CALL_CHANNEL_LSN = 1,  /**< Logical slot number: Capacity Plus, Hytera XPT. */
    DSD_CALL_CHANNEL_LPCN = 2, /**< Logical physical channel number: Tier III. */
    DSD_CALL_CHANNEL_LCN = 3,  /**< Logical channel number: Connect Plus. */
} dsd_call_channel_kind;

/** The label for @p kind (dsd_call_channel_kind): "LSN", "LPCN", "LCN", or "" for NONE or an unknown value. */
static inline const char*
dsd_call_channel_kind_label(uint8_t kind) {
    switch (kind) {
        case DSD_CALL_CHANNEL_LSN: return "LSN";
        case DSD_CALL_CHANNEL_LPCN: return "LPCN";
        case DSD_CALL_CHANNEL_LCN: return "LCN";
        default: return "";
    }
}

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CALL_CHANNEL_H_ */
