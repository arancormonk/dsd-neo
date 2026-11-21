// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Classifier for verified group ids worth remembering as talkgroups.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol variant a group id was heard on. Values are persisted; never renumber. */
typedef enum {
    DSD_HEARD_TG_NONE = 0,
    DSD_HEARD_TG_P25 = 1,
    DSD_HEARD_TG_DMR = 2,
    DSD_HEARD_TG_CAPPLUS = 3,
    DSD_HEARD_TG_CONPLUS = 4,
    DSD_HEARD_TG_XPT = 5,
    DSD_HEARD_TG_NXDN_C = 6,
    DSD_HEARD_TG_NXDN_CONV = 7,
    DSD_HEARD_TG_NXDN_D = 8,
    DSD_HEARD_TG_TETRA = 9
} dsd_heard_tg_variant;

/** One verified group id plus the qualifiers the classifier needs. */
typedef struct {
    dsd_heard_tg_variant variant;
    uint32_t id;               /**< NXDN Type-D: full (prefix << 11) | id11. */
    uint8_t nxdn_call_type;    /**< NXDN call type bits (000 trunked group, 001 group). */
    uint8_t nxdn_inter_system; /**< NXDN Type-C: CC Option bit 6 (inter-system) set. */
    uint8_t p25_supergroup;    /**< P25: the id is a supergroup. */
} dsd_heard_tg_input;

/**
 * @brief Say whether a verified group id is an ordinary talkgroup.
 * @param in Variant, id and qualifiers; NULL yields 0.
 * @return 1 to remember the id, 0 otherwise.
 */
int dsd_heard_tg_ordinary(const dsd_heard_tg_input* in);

#ifdef __cplusplus
}
#endif
