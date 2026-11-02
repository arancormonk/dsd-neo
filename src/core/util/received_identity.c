// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The system identity received on the carrier being decoded (see received_identity.h).
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "received_identity_t3.h"

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

/* The record a note changes, or NULL when the frame being dispatched was read across a carrier boundary or cut by an
   input interruption (dsd_state_frame_read_lost()): its decoder drops it at its own carrier check, which may come after
   a note, and a frame of no carrier names none. Outside a dispatch nothing is gated. */
static dsd_received_identity*
rxid_record(dsd_state* state) {
    if (state == NULL
        || (state->frame_dispatch_active && dsd_state_frame_read_lost(state, state->frame_dispatch_carrier_seq))) {
        return NULL;
    }
    return &state->rx_id;
}

/* The record @p family's note goes into: a record of another family is dropped first. */
static dsd_received_identity*
rxid_for(dsd_state* state, dsd_rxid_family family) {
    if (family == DSD_RXID_FAMILY_NONE || family == DSD_RXID_FAMILY_OTHER) {
        return NULL;
    }
    dsd_received_identity* const id = rxid_record(state);
    if (id == NULL) {
        return NULL;
    }
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

/* Drop @p parts because a note named another system, site, cell or subtype than the one they belong to: what the record
   holds from here is another identity, so its epoch moves (a reader that took the old one, a call's history row, keeps
   it). Nothing held, nothing moves; the Capacity Max marker alone is no identity, so dropping it moves nothing. */
static void
rxid_invalidate(dsd_received_identity* id, uint32_t parts) {
    const uint32_t held = id->valid & parts;
    if (held != 0U) {
        rxid_drop(id, parts);
        if ((held & ~(uint32_t)DSD_RXID_DMR_T3_CAPMAX) != 0U) {
            id->epoch++;
        }
    }
}

static int
rxid_has(const dsd_received_identity* id, uint32_t part) {
    return (id->valid & part) != 0U;
}

/* The whole field is written, zero past the NUL, so equal notes leave equal bytes whatever it held before (a history
   row's frozen copy compares whole records). */
static void
rxid_copy_text(char* dst, size_t capacity, const char* text) {
    DSD_MEMSET(dst, 0, capacity);
    DSD_SNPRINTF(dst, capacity, "%s", text);
}

void
dsd_rxid_reset(dsd_state* state) {
    if (state != NULL) {
        rxid_clear(&state->rx_id);
        state->rx_id_trunk_tune_pending = 0U; /* a retune of the carrier left, whose code is gone with the rest */
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

int
dsd_rxid_request_trunk_tune(dsd_state* state) {
    if (state == NULL) {
        return 0;
    }
    state->rx_id_trunk_tune_pending = 1U;
    /* A retune a frame's decoder makes (a grant, a rest channel a CACH names) runs on the decoder thread inside
       processFrame()'s hold on the tick guard, where no watchdog tick runs: the drop is applied at once. A handler
       that goes on to publish, or to decode the bursts the carrier it reached sends, within the same dispatch never
       shows the carrier left's code, and the first code noted after it is the reached carrier's own, not a change. */
    return state->frame_dispatch_active ? dsd_rxid_apply_trunk_tune(state) : 0;
}

int
dsd_rxid_apply_trunk_tune(dsd_state* state) {
    if (state == NULL || state->rx_id_trunk_tune_pending == 0U) {
        return 0;
    }
    state->rx_id_trunk_tune_pending = 0U;
    dsd_rxid_trunk_tune(state);
    return 1;
}

/* The P25 parts that belong to the system the record's SYSID names. */
#define RXID_P25_SYSTEM                                                                                                \
    (DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_LRA | DSD_RXID_CC_FREQ)

/* The parts a system of @p family sends besides the carrier's access code: what another access code on the same
   carrier (another system sharing the channel) did not send. An M17 CAN is per transmission and names no system. */
static uint32_t
rxid_system_parts(uint8_t family) {
    switch (family) {
        case DSD_RXID_FAMILY_DMR:
            return DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE
                   | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_T3_CAPMAX | DSD_RXID_DMR_XPT_FREE_LCN
                   | DSD_RXID_DMR_VENDOR | DSD_RXID_CC_FREQ;
        case DSD_RXID_FAMILY_P25: return RXID_P25_SYSTEM;
        case DSD_RXID_FAMILY_NXDN:
            return DSD_RXID_NXDN_LOCATION | DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE
                   | DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_CC_FREQ;
        case DSD_RXID_FAMILY_DPMR: return DSD_RXID_CC_FREQ;
        case DSD_RXID_FAMILY_TETRA: return DSD_RXID_TETRA_LA | DSD_RXID_CC_FREQ;
        case DSD_RXID_FAMILY_EDACS:
            return DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM | DSD_RXID_EDACS_CC_LCN
                   | DSD_RXID_CC_FREQ;
        default: return 0U;
    }
}

static dsd_received_identity* rxid_nxdn(dsd_state* state, int type_d);

void
dsd_rxid_note_access(dsd_state* state, dsd_rxid_family family, uint8_t kind, uint16_t value) {
    if (kind == (uint8_t)DSD_ACCESS_CODE_NONE) {
        return;
    }
    /* An NXDN RAN is Type-C's (rxid_nxdn()). */
    dsd_received_identity* const id = family == DSD_RXID_FAMILY_NXDN ? rxid_nxdn(state, 0) : rxid_for(state, family);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_ACCESS) && (id->access_kind != kind || id->access != value)) {
        /* Another system on this carrier: another identity, though the old one had sent nothing but its code. */
        rxid_drop(id, rxid_system_parts(id->family));
        id->epoch++;
    }
    id->access_kind = kind;
    id->access = value;
    rxid_set(id, DSD_RXID_ACCESS);
}

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
        rxid_invalidate(id, RXID_P25_SYSTEM);
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
    if (rxid_has(id, DSD_RXID_P25_SITE) && (id->p25_rfss != rfss || id->p25_site != site)) {
        rxid_invalidate(id, DSD_RXID_P25_SITE | DSD_RXID_P25_LRA | DSD_RXID_CC_FREQ); /* another site */
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

/* The parts one DMR trunking system sends, apart from the carrier's colour code, and the Tier III Capacity Max marker. */
#define RXID_DMR_TRUNK_PARTS                                                                                           \
    (DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_BRAND        \
     | DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_T3_CAPMAX | DSD_RXID_DMR_XPT_FREE_LCN)

/* What one DMR site names: its numbers and text, the rest channel and free channel it names, its control channel. */
#define RXID_DMR_SITE_PARTS                                                                                            \
    (DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_XPT_FREE_LCN \
     | DSD_RXID_CC_FREQ)

/* The DMR record a note of @p trunk (dsd_rxid_dmr_trunk) goes into: a note of another trunking system than the one
   the record's parts belong to drops them and the control channel first. A note of no known system (an unknown brand)
   changes nothing. */
static dsd_received_identity*
rxid_dmr(dsd_state* state, uint8_t trunk) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL || trunk == (uint8_t)DSD_RXID_DMR_TRUNK_NONE) {
        return id;
    }
    if ((id->valid & RXID_DMR_TRUNK_PARTS) != 0U && id->dmr_trunk != trunk) {
        rxid_invalidate(id, RXID_DMR_TRUNK_PARTS | DSD_RXID_CC_FREQ); /* another trunking system on this carrier */
    }
    id->dmr_trunk = trunk;
    return id;
}

/* The trunking system a brand names. */
static uint8_t
rxid_dmr_trunk_named(const char* brand) {
    static const struct {
        const char* brand;
        uint8_t trunk;
    } k_names[] = {
        {"Capacity Plus", (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS},
        {"Connect Plus", (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS},
        {"Hytera XPT", (uint8_t)DSD_RXID_DMR_TRUNK_XPT},
    };

    for (size_t i = 0U; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (strcmp(brand, k_names[i].brand) == 0) {
            return k_names[i].trunk;
        }
    }
    return (uint8_t)DSD_RXID_DMR_TRUNK_NONE;
}

void
dsd_rxid_note_dmr_rest_lsn(dsd_state* state, uint16_t lsn) {
    dsd_received_identity* const id = rxid_dmr(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS);
    if (id == NULL) {
        return;
    }
    id->dmr_rest_lsn = lsn;
    rxid_set(id, DSD_RXID_DMR_REST_LSN);
}

/* Whether a site note's raw parts name another site than the record's valid ones (the trunking system already is the
   record's, rxid_dmr()). */
static int
rxid_dmr_other_site(const dsd_received_identity* id, int has_net, uint16_t net, uint16_t site, uint8_t model) {
    if (!rxid_has(id, DSD_RXID_DMR_SITE | DSD_RXID_DMR_NET)) {
        return 0;
    }
    if (!rxid_has(id, DSD_RXID_DMR_SITE) || id->dmr_site != site || id->dmr_t3_model != model) {
        return 1;
    }
    if (rxid_has(id, DSD_RXID_DMR_NET) != (has_net != 0)) {
        return 1;
    }
    return has_net && id->dmr_net != net;
}

void
dsd_rxid_note_dmr_site(dsd_state* state, uint8_t trunk, int has_net, uint16_t net, uint16_t site, uint8_t t3_model,
                       const char* text) {
    if (trunk == (uint8_t)DSD_RXID_DMR_TRUNK_NONE || trunk > (uint8_t)DSD_RXID_DMR_TRUNK_XPT) {
        return;
    }
    dsd_received_identity* const id = rxid_dmr(state, trunk);
    if (id == NULL) {
        return;
    }
    const uint8_t model = trunk == (uint8_t)DSD_RXID_DMR_TRUNK_TIER3 ? (uint8_t)(t3_model & 0x3U) : 0U;
    if (rxid_dmr_other_site(id, has_net, net, site, model)) {
        rxid_invalidate(id, RXID_DMR_SITE_PARTS); /* another site: the text is never compared */
    }
    id->dmr_net = has_net ? net : 0U;
    id->dmr_site = site;
    id->dmr_t3_model = model;
    /* A valid net with none here was another site, dropped above. */
    rxid_set(id, DSD_RXID_DMR_SITE | (has_net ? DSD_RXID_DMR_NET : 0U));
    if (text == NULL || text[0] == '\0') {
        rxid_drop(id, DSD_RXID_DMR_SITE_TEXT); /* no text for these numbers: none built for others stays beside them */
        return;
    }
    rxid_copy_text(id->dmr_site_text, sizeof(id->dmr_site_text), text);
    rxid_set(id, DSD_RXID_DMR_SITE_TEXT);
}

void
dsd_rxid_note_dmr_brand(dsd_state* state, const char* brand) {
    if (brand == NULL || brand[0] == '\0') {
        return;
    }
    dsd_received_identity* const id = rxid_dmr(state, rxid_dmr_trunk_named(brand));
    if (id == NULL) {
        return;
    }
    rxid_copy_text(id->dmr_brand, sizeof(id->dmr_brand), brand);
    rxid_set(id, DSD_RXID_DMR_BRAND);
}

void
dsd_rxid_note_dmr_vendor(dsd_state* state, uint8_t mfid) {
    if (mfid == 0U) {
        return;
    }
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id == NULL) {
        return;
    }
    id->dmr_vendor = mfid;
    rxid_set(id, DSD_RXID_DMR_VENDOR);
}

void
dsd_rxid_note_dmr_ras(dsd_state* state) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_DMR);
    if (id != NULL) {
        rxid_set(id, DSD_RXID_DMR_RAS);
    }
}

void
dsd_rxid_forget_dmr_ras(dsd_state* state) {
    if (state != NULL) {
        rxid_drop(&state->rx_id, DSD_RXID_DMR_RAS);
    }
}

void
dsd_rxid_note_dmr_t3_syscode(dsd_state* state, uint16_t syscode) {
    dsd_received_identity* const id = rxid_dmr(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_DMR_T3_SYSCODE) && id->dmr_t3_syscode != syscode) {
        /* another system: the old one's site goes with it */
        rxid_invalidate(id, DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE
                                | DSD_RXID_CC_FREQ);
    }
    if (rxid_has(id, DSD_RXID_DMR_T3_CAPMAX)) {
        /* Another site of the recognised network keeps the marker; a code of another network (model and NET) drops
           it, whether or not a code came before. */
        const rxid_t3_code code = rxid_t3_decode(syscode);
        if (code.model != id->dmr_t3_capmax_model || code.net != id->dmr_t3_capmax_net) {
            rxid_invalidate(id, DSD_RXID_DMR_T3_CAPMAX);
        }
    }
    id->dmr_t3_syscode = syscode;
    rxid_set(id, DSD_RXID_DMR_T3_SYSCODE);
}

void
dsd_rxid_note_dmr_t3_capmax(dsd_state* state, uint8_t model, uint16_t net) {
    if (model > 3U || net > rxid_t3_net_max(model)) {
        return; /* names no Tier III network */
    }
    dsd_received_identity* const id = rxid_dmr(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3);
    if (id == NULL) {
        return;
    }
    id->dmr_t3_capmax_model = model;
    id->dmr_t3_capmax_net = net;
    /* Valid only: the marker is no identity received, so it never makes a stale record read as live. */
    id->valid |= DSD_RXID_DMR_T3_CAPMAX;
}

int
dsd_rxid_dmr_t3_is_capmax(const dsd_state* state, uint8_t model, uint16_t net) {
    if (state == NULL) {
        return 0;
    }
    const dsd_received_identity* const id = &state->rx_id;
    return id->family == (uint8_t)DSD_RXID_FAMILY_DMR && id->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_TIER3
           && rxid_has(id, DSD_RXID_DMR_T3_CAPMAX) && id->dmr_t3_capmax_model == model && id->dmr_t3_capmax_net == net;
}

void
dsd_rxid_note_dmr_xpt_free_lcn(dsd_state* state, uint8_t lcn) {
    dsd_received_identity* const id = rxid_dmr(state, (uint8_t)DSD_RXID_DMR_TRUNK_XPT);
    if (id == NULL) {
        return;
    }
    id->dmr_xpt_free_lcn = (uint8_t)(lcn & 0xFU);
    rxid_set(id, DSD_RXID_DMR_XPT_FREE_LCN);
}

/* The parts an IDAS (Type-D) site sends. */
#define RXID_NXDN_TYPE_D (DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM)

/* The NXDN record a Type-C (@p type_d 0) or Type-D note goes into: checked content of one kind of system drops what the
   other kind sent on the carrier, and its control channel. The RAN is Type-C's: a Type-D RTCH2 carries SCCH, FACCH1,
   UDCH2 and FACCH3 (NXDN TS 1-E 4.2.3), none of which carries one, while the CAC, FACCH2/UDCH and SACCH that do are
   Type-C's (and conventional NXDN's). So a Type-D note drops a Type-C location and a RAN, and a RAN or a Type-C location
   drops the IDAS parts; a location keeps the RAN its own channel's check noted just before. */
static dsd_received_identity*
rxid_nxdn(dsd_state* state, int type_d) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_NXDN);
    if (id == NULL) {
        return NULL;
    }
    if (type_d && rxid_has(id, DSD_RXID_NXDN_LOCATION | DSD_RXID_ACCESS)) {
        rxid_invalidate(id, DSD_RXID_NXDN_LOCATION | DSD_RXID_ACCESS | DSD_RXID_CC_FREQ);
    } else if (!type_d && rxid_has(id, RXID_NXDN_TYPE_D)) {
        rxid_invalidate(id, RXID_NXDN_TYPE_D | DSD_RXID_CC_FREQ);
    }
    return id;
}

void
dsd_rxid_note_nxdn_location(dsd_state* state, uint8_t category, uint32_t sys, uint16_t site) {
    if (!dsd_nxdn_location_valid(category, sys, site)) {
        return; /* a reserved category or code names no site, and drops nothing */
    }
    dsd_received_identity* const id = rxid_nxdn(state, 0);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_NXDN_LOCATION)
        && (id->nxdn_category != category || id->nxdn_sys != sys || id->nxdn_site != site)) {
        rxid_invalidate(id, DSD_RXID_NXDN_LOCATION | DSD_RXID_CC_FREQ); /* another site */
    }
    id->nxdn_category = category;
    id->nxdn_sys = sys;
    id->nxdn_site = site;
    rxid_set(id, DSD_RXID_NXDN_LOCATION);
}

void
dsd_rxid_note_nxdn_idas_area(dsd_state* state, uint8_t area) {
    dsd_received_identity* const id = rxid_nxdn(state, 1);
    if (id == NULL) {
        return;
    }
    area &= 1U;
    if (rxid_has(id, DSD_RXID_NXDN_IDAS_AREA) && id->nxdn_idas_area != area) {
        /* The Area bit tells adjacent co-channel sites apart (NXDN TS 1-E 6.6.3): another one is another site. */
        rxid_invalidate(id, DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM
                                | DSD_RXID_CC_FREQ);
    }
    id->nxdn_idas_area = area;
    rxid_set(id, DSD_RXID_NXDN_IDAS_AREA);
}

void
dsd_rxid_note_nxdn_idas_site(dsd_state* state, uint8_t site_type, uint8_t site_code) {
    if (site_type < 1U || site_type > 3U || site_code < 1U || site_code > 250U) {
        return; /* reserved (NXDN TS 1-E 6.6.4, 6.6.5) */
    }
    dsd_received_identity* const id = rxid_nxdn(state, 1);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_NXDN_IDAS_SITE)
        && (id->nxdn_idas_site_type != site_type || id->nxdn_idas_site_code != site_code)) {
        rxid_invalidate(id, DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_CC_FREQ); /* another site */
    }
    if (rxid_has(id, DSD_RXID_NXDN_TYPED_SYSTEM) && id->nxdn_typed_site != site_code) {
        /* A Type-D SRV_INFO named another site: the newer site ID replaces it, with its system. */
        rxid_invalidate(id, DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_CC_FREQ);
    }
    id->nxdn_idas_site_type = site_type;
    id->nxdn_idas_site_code = site_code;
    rxid_set(id, DSD_RXID_NXDN_IDAS_SITE);
}

void
dsd_rxid_note_nxdn_typed_system(dsd_state* state, uint32_t sys, uint8_t site) {
    dsd_received_identity* const id = rxid_nxdn(state, 1);
    if (id == NULL) {
        return;
    }
    sys &= 0x7FFFFU;
    if (rxid_has(id, DSD_RXID_NXDN_TYPED_SYSTEM) && id->nxdn_typed_sys != sys) {
        /* Another system: the old one's SCCH area and site ID and its control channel. */
        rxid_invalidate(id, DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE
                                | DSD_RXID_CC_FREQ);
    } else if (rxid_has(id, DSD_RXID_NXDN_TYPED_SYSTEM) && id->nxdn_typed_site != site) {
        /* Another site: its Area bit was the old site's (NXDN TS 1-E 6.6.3). */
        rxid_invalidate(id, DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_CC_FREQ);
    }
    if (rxid_has(id, DSD_RXID_NXDN_IDAS_SITE) && id->nxdn_idas_site_code != site) {
        /* The SCCH site ID named another site: the newer system ID replaces it, and the Area that site's SCCH sent. */
        rxid_invalidate(id, DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_CC_FREQ);
    }
    id->nxdn_typed_sys = sys;
    id->nxdn_typed_site = site;
    rxid_set(id, DSD_RXID_NXDN_TYPED_SYSTEM);
}

/* The EDACS parts, which every EDACS message carries. */
#define RXID_EDACS_PARTS (DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM | DSD_RXID_EDACS_CC_LCN)

/* The EDACS record a note goes into: the parts are read under the addressing mode the decoder runs
   (dsd_state::ea_mode), so a record of the other mode's reading is dropped first. */
static dsd_received_identity*
rxid_edacs(dsd_state* state) {
    dsd_received_identity* const id = rxid_for(state, DSD_RXID_FAMILY_EDACS);
    if (id == NULL) {
        return NULL;
    }
    if (rxid_has(id, RXID_EDACS_PARTS) && id->edacs_ea_mode != (int8_t)state->ea_mode) {
        rxid_invalidate(id, RXID_EDACS_PARTS | DSD_RXID_CC_FREQ);
    }
    id->edacs_ea_mode = (int8_t)state->ea_mode;
    return id;
}

void
dsd_rxid_note_edacs_site(dsd_state* state, uint16_t site) {
    dsd_received_identity* const id = rxid_edacs(state);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_EDACS_SITE) && id->edacs_site != site) {
        rxid_invalidate(id, DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_CC_LCN | DSD_RXID_CC_FREQ); /* another site */
    }
    id->edacs_site = site;
    rxid_set(id, DSD_RXID_EDACS_SITE);
}

void
dsd_rxid_note_edacs_area(dsd_state* state, uint16_t area) {
    dsd_received_identity* const id = rxid_edacs(state);
    if (id == NULL) {
        return;
    }
    id->edacs_area = area;
    rxid_set(id, DSD_RXID_EDACS_AREA);
}

void
dsd_rxid_note_edacs_system(dsd_state* state, uint16_t system) {
    dsd_received_identity* const id = rxid_edacs(state);
    if (id == NULL) {
        return;
    }
    if (rxid_has(id, DSD_RXID_EDACS_SYSTEM) && id->edacs_system != system) {
        rxid_invalidate(id, DSD_RXID_EDACS_SYSTEM | DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_CC_LCN
                                | DSD_RXID_CC_FREQ);
    }
    id->edacs_system = system;
    rxid_set(id, DSD_RXID_EDACS_SYSTEM);
}

void
dsd_rxid_note_edacs_cc_lcn(dsd_state* state, uint16_t lcn) {
    dsd_received_identity* const id = rxid_edacs(state);
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
    /* The cell's colour binding, not the access code a trunk tune drops from display. */
    const int same_cell =
        rxid_has(id, DSD_RXID_TETRA_NET) && id->tetra_mcc == mcc && id->tetra_mnc == mnc && id->tetra_colour == colour;
    if (!same_cell) {
        /* As a BSCH identity change clears SYSINFO. */
        rxid_invalidate(id, DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA | DSD_RXID_CC_FREQ);
    }
    id->tetra_mcc = mcc;
    id->tetra_mnc = mnc;
    id->tetra_colour = colour;
    id->access_kind = (uint8_t)DSD_ACCESS_CODE_COLOR_CODE;
    id->access = colour;
    rxid_set(id, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS);
}

/* The TETRA record a note changes without starting one, or NULL. */
static dsd_received_identity*
rxid_tetra(dsd_state* state) {
    dsd_received_identity* const id = rxid_record(state);
    return (id != NULL && id->family == (uint8_t)DSD_RXID_FAMILY_TETRA) ? id : NULL;
}

void
dsd_rxid_note_tetra_la(dsd_state* state, uint16_t la) {
    dsd_received_identity* const id = rxid_tetra(state);
    if (id == NULL || !rxid_has(id, DSD_RXID_TETRA_NET)) {
        return; /* a location area of no network this record holds */
    }
    id->tetra_la = (uint16_t)(la & 0x3FFFU);
    rxid_set(id, DSD_RXID_TETRA_LA);
}

void
dsd_rxid_note_tetra_other_cell(dsd_state* state) {
    dsd_received_identity* const id = rxid_tetra(state);
    if (id != NULL) {
        rxid_invalidate(id, DSD_RXID_TETRA_LA | DSD_RXID_CC_FREQ);
    }
}

void
dsd_rxid_note_tetra_direct_mode(dsd_state* state) {
    dsd_received_identity* const id = rxid_tetra(state);
    if (id != NULL) {
        rxid_clear(id);
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

void
dsd_rxid_forget_system(dsd_state* state, dsd_rxid_family family, int with_access) {
    if (state != NULL && family != DSD_RXID_FAMILY_NONE && state->rx_id.family == (uint8_t)family) {
        rxid_invalidate(&state->rx_id, rxid_system_parts(state->rx_id.family) | (with_access ? DSD_RXID_ACCESS : 0U));
    }
}

/* Whether @p opts decode @p family at all. */
static int
rxid_family_decoded(const dsd_opts* opts, uint8_t family) {
    switch (family) {
        case DSD_RXID_FAMILY_DMR: return opts->frame_dmr == 1;
        case DSD_RXID_FAMILY_P25: return opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1;
        case DSD_RXID_FAMILY_NXDN: return opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1;
        case DSD_RXID_FAMILY_DPMR: return opts->frame_dpmr == 1;
        case DSD_RXID_FAMILY_M17: return opts->frame_m17 == 1;
        case DSD_RXID_FAMILY_TETRA: return opts->frame_tetra == 1;
        case DSD_RXID_FAMILY_EDACS: return opts->frame_provoice == 1;
        default: return 1;
    }
}

void
dsd_rxid_note_decoder_settings(const dsd_opts* opts, dsd_state* state) {
    if (opts == NULL || state == NULL) {
        return;
    }
    dsd_received_identity* const id = &state->rx_id;
    if (id->family == (uint8_t)DSD_RXID_FAMILY_NONE) {
        return;
    }
    /* A protocol the decoder no longer runs names nothing; EDACS parts read under the other addressing mode neither. */
    const int other_reading =
        id->family == (uint8_t)DSD_RXID_FAMILY_EDACS && id->edacs_ea_mode != (int8_t)state->ea_mode;
    if (!rxid_family_decoded(opts, id->family) || other_reading) {
        rxid_clear(id);
    }
}
