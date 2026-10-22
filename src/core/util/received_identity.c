// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The system identity received on the carrier being decoded (see received_identity.h).
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

dsd_rxid_family
dsd_rxid_family_for_sync(int synctype) {
    if (synctype < 0) {
        return DSD_RXID_FAMILY_NONE;
    }
    if (DSD_SYNC_IS_DMR(synctype)) {
        return DSD_RXID_FAMILY_DMR;
    }
    if (DSD_SYNC_IS_P25(synctype)) {
        return DSD_RXID_FAMILY_P25;
    }
    if (DSD_SYNC_IS_NXDN(synctype)) {
        return DSD_RXID_FAMILY_NXDN;
    }
    if (DSD_SYNC_IS_DPMR(synctype)) {
        return DSD_RXID_FAMILY_DPMR;
    }
    if (DSD_SYNC_IS_M17(synctype)) {
        return DSD_RXID_FAMILY_M17;
    }
    if (DSD_SYNC_IS_TETRA(synctype)) {
        return DSD_RXID_FAMILY_TETRA;
    }
    if (DSD_SYNC_IS_EDACS(synctype)) {
        return DSD_RXID_FAMILY_EDACS;
    }
    return DSD_RXID_FAMILY_OTHER;
}

static void
rxid_clear(dsd_received_identity* id) {
    const uint32_t epoch = id->epoch + 1U;
    DSD_MEMSET(id, 0, sizeof(*id));
    id->epoch = epoch;
}

/* The record @p family's note goes into: a record of another family is dropped first. */
static dsd_received_identity*
rxid_for(dsd_state* state, dsd_rxid_family family) {
    if (state == NULL || family == DSD_RXID_FAMILY_NONE || family == DSD_RXID_FAMILY_OTHER) {
        return NULL;
    }
    dsd_received_identity* const id = &state->rx_id;
    if (id->family != (uint8_t)family) {
        if (id->family != (uint8_t)DSD_RXID_FAMILY_NONE) {
            rxid_clear(id);
        }
        id->family = (uint8_t)family;
    }
    return id;
}

static void
rxid_set(dsd_received_identity* id, uint32_t parts) {
    id->valid |= parts;
    id->fresh |= parts;
}

static void
rxid_drop(dsd_received_identity* id, uint32_t parts) {
    id->valid &= ~parts;
    id->fresh &= ~parts;
}

static int
rxid_has(const dsd_received_identity* id, uint32_t part) {
    return (id->valid & part) != 0U;
}

static void
rxid_copy_text(char* dst, size_t capacity, const char* text) {
    DSD_SNPRINTF(dst, capacity, "%s", text);
}

void
dsd_rxid_reset(dsd_state* state) {
    if (state != NULL) {
        rxid_clear(&state->rx_id);
    }
}

void
dsd_rxid_note_sync(dsd_state* state, int synctype) {
    const dsd_rxid_family family = dsd_rxid_family_for_sync(synctype);
    if (state == NULL || family == DSD_RXID_FAMILY_NONE) {
        return;
    }
    dsd_received_identity* const id = &state->rx_id;
    if (id->family != (uint8_t)DSD_RXID_FAMILY_NONE && id->family != (uint8_t)family) {
        rxid_clear(id);
    }
}

void
dsd_rxid_mark_stale(dsd_state* state) {
    if (state != NULL) {
        state->rx_id.fresh = 0U;
    }
}

void
dsd_rxid_trunk_tune(dsd_state* state) {
    if (state != NULL) {
        rxid_drop(&state->rx_id, DSD_RXID_ACCESS);
    }
}

void
dsd_rxid_note_access(dsd_state* state, dsd_rxid_family family, uint8_t kind, uint16_t value) {
    if (kind == (uint8_t)DSD_ACCESS_CODE_NONE) {
        return;
    }
    dsd_received_identity* const id = rxid_for(state, family);
    if (id == NULL) {
        return;
    }
    id->access_kind = kind;
    id->access = value;
    rxid_set(id, DSD_RXID_ACCESS);
}

/* The P25 parts that belong to the system the record's SYSID names. */
#define RXID_P25_SYSTEM                                                                                                \
    (DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_LRA | DSD_RXID_CC_FREQ)

void
dsd_rxid_note_p25_network(dsd_state* state, uint32_t wacn, uint16_t sysid) {
    sysid &= 0xFFFU;
    wacn &= 0xFFFFFU;
    if (sysid == 0U) {
        return; /* names no system */
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_P25);
    if (id == NULL) {
        return;
    }
    const int same_system = rxid_has(id, DSD_RXID_P25_SYSID) && id->p25_sysid == sysid
                            && (!rxid_has(id, DSD_RXID_P25_WACN) || id->p25_wacn == wacn);
    if (!same_system) {
        rxid_drop(id, RXID_P25_SYSTEM);
    }
    id->p25_wacn = wacn;
    id->p25_sysid = sysid;
    rxid_set(id, DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID);
}

/* The record for a P25 site or LRA note under @p sysid, or NULL when the note names another system than the record's:
   a site of another system is not this carrier's. */
static dsd_received_identity*
rxid_p25_bound(dsd_state* state, uint16_t sysid) {
    if (sysid == 0U) {
        return NULL;
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_P25);
    if (id == NULL) {
        return NULL;
    }
    if (rxid_has(id, DSD_RXID_P25_SYSID)) {
        if (id->p25_sysid != sysid) {
            return NULL;
        }
    } else {
        id->p25_sysid = sysid;
    }
    rxid_set(id, DSD_RXID_P25_SYSID);
    return id;
}

void
dsd_rxid_note_p25_site(dsd_state* state, uint16_t sysid, uint8_t rfss, uint8_t site) {
    dsd_received_identity* const id = rxid_p25_bound(state, (uint16_t)(sysid & 0xFFFU));
    if (id == NULL) {
        return;
    }
    id->p25_rfss = rfss;
    id->p25_site = site;
    rxid_set(id, DSD_RXID_P25_SITE);
}

void
dsd_rxid_note_p25_lra(dsd_state* state, uint16_t sysid, uint8_t lra) {
    dsd_received_identity* const id = rxid_p25_bound(state, (uint16_t)(sysid & 0xFFFU));
    if (id == NULL) {
        return;
    }
    id->p25_lra = lra;
    rxid_set(id, DSD_RXID_P25_LRA);
}

void
dsd_rxid_note_dmr_rest_lsn(dsd_state* state, uint16_t lsn) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    id->dmr_rest_lsn = lsn;
    rxid_set(id, DSD_RXID_DMR_REST_LSN);
}

void
dsd_rxid_note_dmr_site_text(dsd_state* state, const char* text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    char next[sizeof(id->dmr_site_text)];
    rxid_copy_text(next, sizeof(next), text);
    if (rxid_has(id, DSD_RXID_DMR_SITE_TEXT) && strcmp(id->dmr_site_text, next) != 0) {
        rxid_drop(id, DSD_RXID_CC_FREQ); /* another site */
    }
    DSD_MEMCPY(id->dmr_site_text, next, sizeof(next));
    rxid_set(id, DSD_RXID_DMR_SITE_TEXT);
}

void
dsd_rxid_note_dmr_brand(dsd_state* state, const char* brand) {
    if (brand == NULL || brand[0] == '\0') {
        return;
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    rxid_copy_text(id->dmr_brand, sizeof(id->dmr_brand), brand);
    rxid_set(id, DSD_RXID_DMR_BRAND);
}

void
dsd_rxid_note_dmr_t3_syscode(dsd_state* state, uint16_t syscode) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_DMR_T3_SYSCODE) && id->dmr_t3_syscode != syscode) {
        rxid_drop(id, DSD_RXID_DMR_SITE_TEXT | DSD_RXID_CC_FREQ); /* another system */
    }
    id->dmr_t3_syscode = syscode;
    rxid_set(id, DSD_RXID_DMR_T3_SYSCODE);
}

void
dsd_rxid_note_dmr_xpt_free_lcn(dsd_state* state, uint8_t lcn) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    id->dmr_xpt_free_lcn = (uint8_t)(lcn & 0xFU);
    rxid_set(id, DSD_RXID_DMR_XPT_FREE_LCN);
}

void
dsd_rxid_note_nxdn_location(dsd_state* state, uint8_t category, uint32_t sys, uint16_t site) {
    if (category > 2U) {
        return; /* 3 is reserved */
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_NXDN);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_NXDN_LOCATION)
        && (id->nxdn_category != category || id->nxdn_sys != sys || id->nxdn_site != site)) {
        rxid_drop(id, DSD_RXID_CC_FREQ); /* another site */
    }
    id->nxdn_category = category;
    id->nxdn_sys = sys;
    id->nxdn_site = site;
    rxid_set(id, DSD_RXID_NXDN_LOCATION);
}

void
dsd_rxid_note_nxdn_idas_area(dsd_state* state, uint8_t area) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_NXDN);
    if (id == NULL) {
        return;
    }
    id->nxdn_idas_area = (uint8_t)(area & 1U);
    rxid_set(id, DSD_RXID_NXDN_IDAS_AREA);
}

void
dsd_rxid_note_nxdn_idas_site(dsd_state* state, uint8_t site_type, uint8_t site_code) {
    if (site_type < 1U || site_type > 3U || site_code < 1U || site_code > 250U) {
        return; /* reserved (NXDN TS 1-E 6.6.4, 6.6.5) */
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_NXDN);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_NXDN_IDAS_SITE)
        && (id->nxdn_idas_site_type != site_type || id->nxdn_idas_site_code != site_code)) {
        rxid_drop(id, DSD_RXID_CC_FREQ); /* another site */
    }
    id->nxdn_idas_site_type = site_type;
    id->nxdn_idas_site_code = site_code;
    rxid_set(id, DSD_RXID_NXDN_IDAS_SITE);
}

void
dsd_rxid_note_nxdn_typed_system(dsd_state* state, uint32_t sys, uint8_t site) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_NXDN);
    if (id == NULL) {
        return;
    }
    id->nxdn_typed_sys = sys & 0x7FFFFU;
    id->nxdn_typed_site = site;
    rxid_set(id, DSD_RXID_NXDN_TYPED_SYSTEM);
}

void
dsd_rxid_note_edacs_site(dsd_state* state, uint16_t site) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_EDACS);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_EDACS_SITE) && id->edacs_site != site) {
        rxid_drop(id, DSD_RXID_EDACS_CC_LCN | DSD_RXID_CC_FREQ); /* another site */
    }
    id->edacs_site = site;
    rxid_set(id, DSD_RXID_EDACS_SITE);
}

void
dsd_rxid_note_edacs_area(dsd_state* state, uint16_t area) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_EDACS);
    if (id == NULL) {
        return;
    }
    id->edacs_area = area;
    rxid_set(id, DSD_RXID_EDACS_AREA);
}

void
dsd_rxid_note_edacs_system(dsd_state* state, uint16_t system) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_EDACS);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_EDACS_SYSTEM) && id->edacs_system != system) {
        rxid_drop(id, DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_CC_LCN | DSD_RXID_CC_FREQ);
    }
    id->edacs_system = system;
    rxid_set(id, DSD_RXID_EDACS_SYSTEM);
}

void
dsd_rxid_note_edacs_cc_lcn(dsd_state* state, uint16_t lcn) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_EDACS);
    if (id == NULL) {
        return;
    }
    id->edacs_cc_lcn = lcn;
    rxid_set(id, DSD_RXID_EDACS_CC_LCN);
}

void
dsd_rxid_note_tetra_cell(dsd_state* state, uint16_t mcc, uint16_t mnc, uint8_t colour) {
    mcc &= 0x3FFU;
    mnc &= 0x3FFFU;
    colour &= 0x3FU;
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_TETRA);
    if (id == NULL) {
        return;
    }
    const int same_cell = rxid_has(id, DSD_RXID_TETRA_NET) && id->tetra_mcc == mcc && id->tetra_mnc == mnc
                          && (!rxid_has(id, DSD_RXID_ACCESS) || id->access == colour);
    if (!same_cell) {
        rxid_drop(id, DSD_RXID_TETRA_LA | DSD_RXID_CC_FREQ); /* as a BSCH identity change clears SYSINFO */
    }
    id->tetra_mcc = mcc;
    id->tetra_mnc = mnc;
    id->access_kind = (uint8_t)DSD_ACCESS_CODE_COLOR_CODE;
    id->access = colour;
    rxid_set(id, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS);
}

void
dsd_rxid_note_tetra_la(dsd_state* state, uint16_t la) {
    if (state == NULL || state->rx_id.family != (uint8_t)DSD_RXID_FAMILY_TETRA
        || !rxid_has(&state->rx_id, DSD_RXID_TETRA_NET)) {
        return; /* a location area of no network this record holds */
    }
    state->rx_id.tetra_la = (uint16_t)(la & 0x3FFFU);
    rxid_set(&state->rx_id, DSD_RXID_TETRA_LA);
}

void
dsd_rxid_note_tetra_direct_mode(dsd_state* state) {
    if (state != NULL && state->rx_id.family == (uint8_t)DSD_RXID_FAMILY_TETRA) {
        rxid_clear(&state->rx_id);
    }
}

void
dsd_rxid_note_cc_freq(dsd_state* state, dsd_rxid_family family, uint32_t freq_hz) {
    if (freq_hz == 0U) {
        return;
    }
    dsd_received_identity* const id = rxid_for(state, family);
    if (id == NULL) {
        return;
    }
    id->cc_freq_hz = freq_hz;
    rxid_set(id, DSD_RXID_CC_FREQ);
}
