// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Reader for the access code a protocol's carrier is decoded with.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>

#include <stdint.h>

#include "dsd-neo/core/state_fwd.h"

dsd_access_code_kind
dsd_access_code_kind_for_protocol(int synctype) {
    if (DSD_SYNC_IS_DMR(synctype) || DSD_SYNC_IS_DPMR(synctype)) {
        return DSD_ACCESS_CODE_COLOR_CODE;
    }
    if (DSD_SYNC_IS_P25(synctype)) {
        return DSD_ACCESS_CODE_NAC;
    }
    if (DSD_SYNC_IS_NXDN(synctype)) {
        return DSD_ACCESS_CODE_RAN;
    }
    if (DSD_SYNC_IS_M17(synctype)) {
        return DSD_ACCESS_CODE_CAN;
    }
    return DSD_ACCESS_CODE_NONE;
}

/* A P25 NAC as the NID and the Phase 2 network status carry it: twelve bits, with 0x000 and 0xFFF reserved. */
static int
access_code_valid_nac(unsigned long long nac) {
    return nac >= 0x001ULL && nac <= 0xFFEULL;
}

/* The code the decoder holds for this protocol now, or 0 when it holds none it can vouch for. */
static int
access_code_read(const dsd_state* state, int protocol, uint16_t service_options, uint8_t has_service_metadata,
                 uint16_t* value) {
    if (DSD_SYNC_IS_DMR(protocol)) {
        /* 16 is "not locked"; anything above 15 is no colour code at all. */
        if (state->dmr_color_code > 15U) {
            return 0;
        }
        *value = (uint16_t)state->dmr_color_code;
        return 1;
    }
    if (DSD_SYNC_IS_P25P1(protocol)) {
        if (state->nac < 0 || !access_code_valid_nac((unsigned long long)state->nac)) {
            return 0;
        }
        *value = (uint16_t)state->nac;
        return 1;
    }
    if (DSD_SYNC_IS_P25P2(protocol)) {
        if (!access_code_valid_nac(state->p2_cc)) {
            return 0;
        }
        *value = (uint16_t)state->p2_cc;
        return 1;
    }
    if (DSD_SYNC_IS_NXDN(protocol)) {
        /* (unsigned)-1 is "no RAN decoded yet"; a RAN is six bits. An IDAS (Type-D) carrier's area bit or site type,
         * or DCR's fixed 7, stands in for one and is no access code. */
        if (state->nxdn_last_ran >= 64U || state->nxdn_last_ran_stand_in != 0U) {
            return 0;
        }
        *value = (uint16_t)state->nxdn_last_ran;
        return 1;
    }
    if (DSD_SYNC_IS_DPMR(protocol)) {
        /* -1 is "none on this carrier"; the colour-code map yields 0..63. */
        if (state->dpmr_color_code < 0 || state->dpmr_color_code > 63) {
            return 0;
        }
        *value = (uint16_t)state->dpmr_color_code;
        return 1;
    }
    if (DSD_SYNC_IS_M17(protocol)) {
        if (has_service_metadata == 0U) {
            return 0;
        }
        *value = (uint16_t)(service_options & 0xFU);
        return 1;
    }
    return 0;
}

int
dsd_access_code_current(const dsd_state* state, int protocol, uint16_t service_options, uint8_t has_service_metadata,
                        uint8_t* kind_out, uint16_t* value_out) {
    uint16_t value = 0U;
    const int valid =
        state != NULL && access_code_read(state, protocol, service_options, has_service_metadata, &value) != 0;
    if (kind_out != NULL) {
        *kind_out = valid ? (uint8_t)dsd_access_code_kind_for_protocol(protocol) : (uint8_t)DSD_ACCESS_CODE_NONE;
    }
    if (value_out != NULL) {
        *value_out = valid ? value : 0U;
    }
    return valid ? 1 : 0;
}
