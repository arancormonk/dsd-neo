// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The system identity received on the carrier being decoded (dsd_state::rx_id): independent parts, the P25 SYSID
 * binding, dependent invalidation, family replacement, freshness, and the reset and trunk-tune lifetime.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pinned: the Qt model and the history store compare these. */
_Static_assert(DSD_RXID_FAMILY_NONE == 0, "family NONE is pinned");
_Static_assert(DSD_RXID_FAMILY_EDACS == 7, "family EDACS is pinned");
/* No padding: a whole-record compare (a history row's frozen copy, memcmp) sees only values, and a struct copy leaves no
   indeterminate bytes. A field added to the record keeps it so, or moves this pin with a reason. */
_Static_assert(sizeof(dsd_received_identity) == 172U, "dsd_received_identity carries no padding");

static int g_fail;

static void
expect_u(const char* label, unsigned long got, unsigned long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %lu (0x%lX), want %lu (0x%lX)\n", label, got, got, want, want);
        g_fail = 1;
    }
}

static void
expect_s(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", label, got, want);
        g_fail = 1;
    }
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        DSD_FPRINTF(stderr, "out of memory\n");
        exit(2);
    }
    return state;
}

static void
test_parts_and_reset(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    expect_u("empty family", id->family, DSD_RXID_FAMILY_NONE);
    expect_u("empty valid", id->valid, 0U);

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    expect_u("dmr family", id->family, DSD_RXID_FAMILY_DMR);
    expect_u("dmr valid", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);
    expect_u("dmr fresh", id->fresh, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);
    expect_u("dmr cc", id->access, 1U);
    expect_u("dmr rest", id->dmr_rest_lsn, 3U);

    const uint32_t epoch = id->epoch;
    dsd_rxid_reset(state);
    expect_u("reset valid", id->valid, 0U);
    expect_u("reset family", id->family, DSD_RXID_FAMILY_NONE);
    expect_u("reset epoch", id->epoch, epoch + 1U);

    /* A partial B after the boundary shows only B's parts; an unchanged checked repeat proves its value again. */
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    expect_u("partial B valid", id->valid, DSD_RXID_DMR_REST_LSN);
    free(state);
}

static void
test_freshness(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_mark_stale(state);
    expect_u("stale keeps value", id->valid, DSD_RXID_ACCESS);
    expect_u("stale clears fresh", id->fresh, 0U);
    expect_u("stale keeps ran", id->access, 5U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    expect_u("repeat refreshes", id->fresh, DSD_RXID_ACCESS);
    free(state);
}

static void
test_family_replacement(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    /* A DMR sync keeps it; no sync changes nothing. */
    dsd_rxid_note_sync(state, DSD_SYNC_DMR_BS_DATA_POS);
    dsd_rxid_note_sync(state, DSD_SYNC_NONE);
    expect_u("same family kept", id->valid, DSD_RXID_ACCESS);
    /* DMR, then YSF, then no sync: nothing shows. */
    dsd_rxid_note_sync(state, DSD_SYNC_YSF_POS);
    expect_u("ysf drops dmr", id->valid, 0U);
    expect_u("ysf family", id->family, DSD_RXID_FAMILY_NONE);
    dsd_rxid_note_sync(state, DSD_SYNC_NONE);
    expect_u("still empty", id->valid, 0U);

    /* A note of another family replaces the record. */
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    expect_u("p25 family", id->family, DSD_RXID_FAMILY_P25);
    expect_u("p25 only", id->valid, DSD_RXID_ACCESS);
    expect_u("p25 kind", id->access_kind, DSD_ACCESS_CODE_NAC);

    /* ProVoice voice channels belong to EDACS. */
    expect_u("provoice family", dsd_rxid_family_for_sync(DSD_SYNC_PROVOICE_POS), DSD_RXID_FAMILY_EDACS);
    expect_u("none family", dsd_rxid_family_for_sync(DSD_SYNC_NONE), DSD_RXID_FAMILY_NONE);
    expect_u("dstar family", dsd_rxid_family_for_sync(DSD_SYNC_DSTAR_VOICE_POS), DSD_RXID_FAMILY_OTHER);
    free(state);
}

/* The signal search merge rule (tests/protocol/p25/test_p25_search_site_notes.c), applied per carrier. */
static void
test_p25_merge(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    /* RFSS first: the site gives the record its SYSID; the network of that SYSID completes it. */
    dsd_rxid_note_p25_site(state, 0x2B4U, 3U, 21U);
    expect_u("site first valid", id->valid, DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x2B4U);
    expect_u("network completes", id->valid, DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_WACN);
    expect_u("wacn", id->p25_wacn, 0xBEE00U);

    /* A site of another system is not this carrier's. */
    dsd_rxid_note_p25_site(state, 0x1A3U, 2U, 9U);
    expect_u("foreign site ignored rfss", id->p25_rfss, 3U);
    expect_u("foreign site ignored sysid", id->p25_sysid, 0x2B4U);
    dsd_rxid_note_p25_lra(state, 0x1A3U, 7U);
    expect_u("foreign lra ignored", id->valid & DSD_RXID_P25_LRA, 0U);
    dsd_rxid_note_p25_lra(state, 0x2B4U, 7U);
    expect_u("lra bound", id->valid & DSD_RXID_P25_LRA, DSD_RXID_P25_LRA);

    /* SYSID 0 names no system. */
    dsd_rxid_note_p25_network(state, 0x12345U, 0U);
    dsd_rxid_note_p25_site(state, 0U, 1U, 1U);
    expect_u("sysid 0 ignored", id->p25_sysid, 0x2B4U);

    /* The NAC is the carrier's: a network status of another system drops the old site, LRA and CC, not the NAC. */
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x1A3U);
    expect_u("foreign network", id->valid, DSD_RXID_ACCESS | DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID);
    expect_u("foreign network sysid", id->p25_sysid, 0x1A3U);
    /* Another WACN under the same SYSID is another system too. */
    dsd_rxid_note_p25_site(state, 0x1A3U, 1U, 4U);
    dsd_rxid_note_p25_network(state, 0xBEE01U, 0x1A3U);
    expect_u("other wacn drops site", id->valid & DSD_RXID_P25_SITE, 0U);
    free(state);
}

static void
test_dependent_invalidation(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_u("tetra valid", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS | DSD_RXID_TETRA_LA);
    expect_u("tetra kind", id->access_kind, DSD_ACCESS_CODE_COLOR_CODE);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    expect_u("same cell keeps la", id->valid & DSD_RXID_TETRA_LA, DSD_RXID_TETRA_LA);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 26U);
    expect_u("colour change drops la", id->valid & DSD_RXID_TETRA_LA, 0U);
    dsd_rxid_note_tetra_direct_mode(state);
    expect_u("direct mode drops tmo", id->valid, 0U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_u("la needs a network", id->valid, 0U);
    dsd_rxid_reset(state);

    dsd_rxid_note_edacs_site(state, 42U);
    dsd_rxid_note_edacs_cc_lcn(state, 7U);
    dsd_rxid_note_edacs_system(state, 3U);
    expect_u("edacs parts", id->valid, DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_CC_LCN | DSD_RXID_EDACS_SYSTEM);
    dsd_rxid_note_edacs_site(state, 43U);
    expect_u("edacs site change drops cc", id->valid & DSD_RXID_EDACS_CC_LCN, 0U);
    dsd_rxid_reset(state);

    dsd_rxid_note_nxdn_location(state, 3U, 1U, 1U);
    expect_u("reserved category", id->valid, 0U);
    dsd_rxid_note_nxdn_idas_site(state, 3U, 0U);
    dsd_rxid_note_nxdn_idas_site(state, 0U, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 251U);
    expect_u("reserved idas site", id->valid, 0U);
    dsd_rxid_note_nxdn_idas_site(state, 3U, 1U);
    expect_u("idas site", id->valid, DSD_RXID_NXDN_IDAS_SITE);

    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 3U, 1U,
                           "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4 \xC2\xB7 SYS 1183");
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x1183U);
    expect_s("dmr site text", id->dmr_site_text, "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4 \xC2\xB7 SYS 1183");
    expect_s("dmr brand", id->dmr_brand, "Capacity Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x1184U);
    expect_u("syscode change drops site text", id->valid & DSD_RXID_DMR_SITE_TEXT, 0U);
    free(state);
}

/* A checked access code that changes on the same carrier names another system sharing the channel (co-channel DMR
   colour codes, P25 NACs, NXDN RANs): what the old one sent is dropped with its control channel. An unchanged repeat,
   or the first code after a trunk tune dropped the carrier's, keeps the system. An M17 CAN is per transmission and
   drops nothing. */
static void
test_access_change(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 7U, 0U, "Cap+ \xC2\xB7 Site 7");
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_rest_lsn(state, 4U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    const uint32_t dmr_system =
        DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_REST_LSN | DSD_RXID_CC_FREQ;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    expect_u("dmr same cc keeps the system", id->valid, DSD_RXID_ACCESS | dmr_system);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_u("dmr cc change drops the system", id->valid, DSD_RXID_ACCESS);
    /* As it drops a Tier III or XPT site's parts. */
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A3FU);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 3U);
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    dsd_rxid_note_dmr_xpt_free_lcn(state, 2U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_u("dmr cc change drops the tier iii and xpt parts", id->valid, DSD_RXID_ACCESS);
    expect_u("dmr cc change value", id->access, 2U);

    /* A followed voice channel: the tune dropped the code, so the channel's code is the first, not a change. */
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 7U, 0U, "Cap+ \xC2\xB7 Site 7");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    dsd_rxid_trunk_tune(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 5U);
    expect_u("dmr follow keeps the system", id->valid,
             DSD_RXID_ACCESS | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_CC_FREQ);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_p25_lra(state, 0x348U, 7U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x294U);
    expect_u("p25 nac change drops the system", id->valid, DSD_RXID_ACCESS);
    /* The new system's network status binds as on a fresh carrier. */
    dsd_rxid_note_p25_site(state, 0x1A3U, 1U, 2U);
    expect_u("p25 new system's site", id->valid, DSD_RXID_ACCESS | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 2U, 17U, 3U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 6U);
    expect_u("nxdn ran change drops the system", id->valid, DSD_RXID_ACCESS);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 7U);
    expect_u("nxdn ran change drops the idas parts", id->valid, DSD_RXID_ACCESS);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DPMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DPMR, DSD_ACCESS_CODE_COLOR_CODE, 9U);
    expect_u("dpmr cc change", id->access, 9U);
    expect_u("dpmr cc change valid", id->valid, DSD_RXID_ACCESS);

    /* M17: every transmission carries its own CAN; a change names no other system. */
    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 1U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_M17, 433000000U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 2U);
    expect_u("m17 can change drops nothing", id->valid, DSD_RXID_ACCESS | DSD_RXID_CC_FREQ);
    expect_u("m17 can change value", id->access, 2U);
    free(state);
}

/* An IDAS carrier names its site twice: the SCCH site ID and a Type-D SRV_INFO (system and site code). The newer of the
   two wins when they disagree, and another system drops the control channel. */
static void
test_nxdn_idas_site_reconcile(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_nxdn_idas_site(state, 1U, 5U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 5U);
    expect_u("typed same site keeps the scch site", id->valid,
             DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_CC_FREQ);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 7U);
    expect_u("typed other site drops the scch site", id->valid, DSD_RXID_NXDN_TYPED_SYSTEM);
    expect_u("typed other site", id->nxdn_typed_site, 7U);

    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_idas_site(state, 2U, 7U);
    expect_u("scch same site keeps the typed system", id->valid,
             DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_CC_FREQ);
    dsd_rxid_note_nxdn_idas_site(state, 2U, 8U);
    expect_u("scch other site drops the typed system", id->valid, DSD_RXID_NXDN_IDAS_SITE);

    /* Another system: its control channel is not the old one's, and the old system's SCCH area and site go with it. */
    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 7U);
    dsd_rxid_note_nxdn_idas_site(state, 2U, 7U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 7U);
    expect_u("typed repeat keeps the control channel", id->valid & DSD_RXID_CC_FREQ, DSD_RXID_CC_FREQ);
    dsd_rxid_note_nxdn_typed_system(state, 0x54321U, 7U);
    expect_u("typed other system", id->valid, DSD_RXID_NXDN_TYPED_SYSTEM);
    expect_u("typed other system value", id->nxdn_typed_sys, 0x54321U);
    free(state);
}

static void
test_trunk_tune(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    dsd_rxid_trunk_tune(state);
    expect_u("tune keeps system", id->valid,
             DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_CC_FREQ);
    expect_u("tune keeps family", id->family, DSD_RXID_FAMILY_P25);
    expect_u("cc freq", id->cc_freq_hz, 851012500U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 0U);
    expect_u("cc 0 ignored", id->cc_freq_hz, 851012500U);

    /* A TETRA tune drops the colour code (the cell's access code); the network and LA stay. */
    dsd_rxid_reset(state);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    dsd_rxid_trunk_tune(state);
    expect_u("tetra tune", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    expect_u("tetra same cell after tune keeps la", id->valid & DSD_RXID_TETRA_LA, DSD_RXID_TETRA_LA);
    /* The tune dropped the colour from display, not the cell's binding: another colour is another cell. */
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_TETRA, 390012500U);
    dsd_rxid_trunk_tune(state);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 26U);
    expect_u("tetra other colour after tune drops la and cc", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS);
    expect_u("tetra other colour after tune", id->access, 26U);
    /* A SYSINFO of another cell with the same network and colour: the last cell's area and control channel go. */
    dsd_rxid_note_tetra_la(state, 4243U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_TETRA, 390012500U);
    dsd_rxid_note_tetra_other_cell(state);
    expect_u("tetra other cell", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS);
    free(state);
}

/* A Hytera XPT site's free channel is a part of its own: each checked message replaces it, it goes stale with the rest,
   a trunk tune keeps it (the site's, not the carrier's) and a boundary or another family drops it. */
static void
test_dmr_xpt_free_lcn(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    dsd_rxid_note_dmr_xpt_free_lcn(state, 3U);
    expect_u("xpt family", id->family, DSD_RXID_FAMILY_DMR);
    expect_u("xpt valid", id->valid, DSD_RXID_DMR_BRAND | DSD_RXID_DMR_XPT_FREE_LCN);
    expect_u("xpt fresh", id->fresh, DSD_RXID_DMR_BRAND | DSD_RXID_DMR_XPT_FREE_LCN);
    expect_u("xpt free lcn", id->dmr_xpt_free_lcn, 3U);
    dsd_rxid_mark_stale(state);
    dsd_rxid_note_dmr_xpt_free_lcn(state, 0x1AU);
    expect_u("xpt free lcn replaced, 4 bits", id->dmr_xpt_free_lcn, 0xAU);
    expect_u("xpt free lcn fresh again", id->fresh, DSD_RXID_DMR_XPT_FREE_LCN);
    dsd_rxid_trunk_tune(state);
    expect_u("xpt tune keeps the free lcn", id->valid & DSD_RXID_DMR_XPT_FREE_LCN, DSD_RXID_DMR_XPT_FREE_LCN);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    expect_u("another family drops the free lcn", id->valid, DSD_RXID_ACCESS);
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_xpt_free_lcn(state, 5U);
    dsd_rxid_reset(state);
    expect_u("reset drops the free lcn", id->valid, 0U);
    free(state);
}

/* A frame read across a carrier boundary (or cut by an input interruption) is no carrier's: whatever its decoder notes
   while it is dispatched, before the decoder's own carrier check, goes nowhere -- least of all into the record the
   boundary reset for the next carrier. */
static void
test_lost_frame_notes_nothing(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    state->carrier_seq = 4U;
    state->frame_dispatch_active = 1;
    state->frame_dispatch_carrier_seq = 4U;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_TETRA, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_u("dispatched frame notes", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS | DSD_RXID_TETRA_LA);

    /* The boundary inside the frame: it reset the record and moved the count. */
    dsd_rxid_reset(state);
    state->carrier_seq = 5U;
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 26U);
    dsd_rxid_note_tetra_la(state, 4243U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 3U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 1U, 1U,
                           "TIII Small \xC2\xB7 Net 1 \xC2\xB7 Site 2");
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_nxdn_location(state, 0U, 1U, 1U);
    dsd_rxid_note_edacs_site(state, 4U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    expect_u("split frame notes nothing", id->valid, 0U);
    expect_u("split frame family", id->family, DSD_RXID_FAMILY_NONE);

    /* Nor does it drop anything: the next carrier's record is not the split frame's to change. */
    state->frame_dispatch_active = 0;
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 26U);
    dsd_rxid_note_tetra_la(state, 4243U);
    state->frame_dispatch_active = 1;
    dsd_rxid_note_tetra_other_cell(state);
    dsd_rxid_note_tetra_direct_mode(state);
    expect_u("split frame drops nothing", id->valid, DSD_RXID_TETRA_NET | DSD_RXID_ACCESS | DSD_RXID_TETRA_LA);

    /* An input interruption cut the frame. */
    dsd_rxid_reset(state);
    state->frame_dispatch_carrier_seq = 5U;
    state->input_interrupted = 1;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 3U);
    expect_u("cut frame notes nothing", id->valid, 0U);

    /* Outside a dispatch (an engine call, a test) nothing is gated. */
    state->frame_dispatch_active = 0;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 3U);
    expect_u("outside a dispatch", id->valid, DSD_RXID_ACCESS);
    free(state);
}

/* NXDN Type-C (a location ID, and the RAN the CAC, FACCH2/UDCH and SACCH carry) and IDAS Type-D (the SCCH area and site
   ID, a Type-D SRV_INFO system ID) are two kinds of system: checked content of one on the carrier drops what the other
   sent, and its control channel. A Type-D RTCH2 carries SCCH, FACCH1, UDCH2 and FACCH3 only (NXDN TS 1-E 4.2.3), none
   of which carries a RAN, so a RAN is Type-C's. The message that carries a Type-C location noted its own RAN at its
   channel's check just before, so that RAN stays. */
static void
test_nxdn_subtypes_exclusive(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 1U, 12U, 3U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    expect_u("idas area drops type-c", id->valid, DSD_RXID_NXDN_IDAS_AREA);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 1U, 12U, 3U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    expect_u("typed system drops type-c", id->valid, DSD_RXID_NXDN_TYPED_SYSTEM);
    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_location(state, 1U, 12U, 3U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
    expect_u("idas site drops type-c", id->valid, DSD_RXID_NXDN_IDAS_SITE);

    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 0U, 1U, 2U);
    expect_u("type-c location drops idas", id->valid, DSD_RXID_ACCESS | DSD_RXID_NXDN_LOCATION);

    /* RAN-only records: a Type-C control channel's CAC RAN and control channel, then a checked Type-D SCCH; and IDAS
       parts, then a checked RAN. */
    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    expect_u("idas drops a ran-only type-c record", id->valid, DSD_RXID_NXDN_IDAS_AREA);
    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 3U);
    expect_u("a ran drops the idas parts", id->valid, DSD_RXID_ACCESS);
    expect_u("a ran drops the idas parts: value", id->access, 3U);

    /* The Area bit tells adjacent co-channel Type-D sites apart (NXDN TS 1-E 6.6.3): another Area is another site, so
       the site ID, the Type-D system ID and the control channel the old one sent go; the same Area keeps them. */
    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 0U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
    dsd_rxid_note_nxdn_idas_area(state, 0U);
    expect_u("same area keeps the site", id->valid,
             DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM | DSD_RXID_CC_FREQ);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    expect_u("another area drops the site", id->valid, DSD_RXID_NXDN_IDAS_AREA);
    expect_u("another area value", id->nxdn_idas_area, 1U);
    free(state);
}

/* A location ID the standard reserves (NXDN TS 1-A 6.5.2 Table 6.5-1: each category's all-zero and all-ones system and
   site codes) names no site: the note keeps nothing and drops nothing, so the location, the control channel and the
   epoch an earlier valid one gave stay, and an IDAS record is not dropped as if a Type-C site had been heard. The
   bounds of each range are kept. */
static void
test_nxdn_location_reserved_refused(void) {
    static const struct {
        uint8_t category;
        uint32_t sys_max;
        uint16_t site_max;
    } k_ranges[3] = {{0U, 1022U, 4094U}, {1U, 16382U, 254U}, {2U, 131070U, 30U}};

    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    for (int i = 0; i < 3; ++i) {
        const uint8_t c = k_ranges[i].category;
        const uint32_t sys_ones = k_ranges[i].sys_max + 1U;
        const uint16_t site_ones = (uint16_t)(k_ranges[i].site_max + 1U);
        const uint32_t bad[4][2] = {{0U, 1U}, {sys_ones, 1U}, {1U, 0U}, {1U, site_ones}};

        dsd_rxid_reset(state);
        dsd_rxid_note_nxdn_location(state, c, 1U, 1U);
        expect_u("lowest location kept", id->valid, DSD_RXID_NXDN_LOCATION);
        dsd_rxid_note_nxdn_location(state, c, k_ranges[i].sys_max, k_ranges[i].site_max);
        expect_u("highest location kept", id->valid, DSD_RXID_NXDN_LOCATION);
        expect_u("highest location system", id->nxdn_sys, k_ranges[i].sys_max);
        expect_u("highest location site", id->nxdn_site, k_ranges[i].site_max);
        dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_NXDN, 452000000U);
        const uint32_t epoch = id->epoch;
        for (int b = 0; b < 4; ++b) {
            dsd_rxid_note_nxdn_location(state, c, bad[b][0], (uint16_t)bad[b][1]);
            expect_u("reserved location keeps the earlier one", id->valid, DSD_RXID_NXDN_LOCATION | DSD_RXID_CC_FREQ);
            expect_u("reserved location keeps the earlier system", id->nxdn_sys, k_ranges[i].sys_max);
            expect_u("reserved location keeps the earlier site", id->nxdn_site, k_ranges[i].site_max);
            expect_u("reserved location keeps the epoch", id->epoch, epoch);
        }

        dsd_rxid_reset(state);
        dsd_rxid_note_nxdn_idas_area(state, 1U);
        dsd_rxid_note_nxdn_idas_site(state, 1U, 9U);
        for (int b = 0; b < 4; ++b) {
            dsd_rxid_note_nxdn_location(state, c, bad[b][0], (uint16_t)bad[b][1]);
        }
        expect_u("reserved location keeps an idas record", id->valid,
                 DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE);
    }
    free(state);
}

/* Capacity Plus, Connect Plus, Hytera XPT and Tier III are different DMR trunking systems: checked content naming
   another one than the record's drops the old one's parts and its control channel; the colour code is the carrier's
   and stays. */
static void
test_dmr_trunking_types_exclusive(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    expect_u("cap+ parts", id->valid,
             DSD_RXID_ACCESS | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_BRAND
                 | DSD_RXID_CC_FREQ);
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    expect_u("xpt brand drops cap+", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_BRAND);
    dsd_rxid_note_dmr_xpt_free_lcn(state, 4U);
    expect_u("xpt free lcn joins xpt", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_XPT_FREE_LCN);
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    expect_u("cap+ rest drops xpt", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);

    dsd_rxid_reset(state);
    const uint32_t raw = DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 5U, 0U,
                           "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x1001U);
    expect_u("tier iii drops con+", id->valid, DSD_RXID_DMR_T3_SYSCODE);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 1U, 1U,
                           "TIII Small \xC2\xB7 Net 1 \xC2\xB7 Site 2 \xC2\xB7 SYS 1001");
    expect_u("tier iii site joins", id->valid, DSD_RXID_DMR_T3_SYSCODE | raw);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 5U, 0U,
                           "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    expect_u("con+ site drops tier iii", id->valid, raw);

    /* A part of no trunking type keeps the record's. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_brand(state, "Other");
    expect_u("unknown brand", id->valid, DSD_RXID_DMR_BRAND);
    free(state);
}

/* EDACS parts are read under the addressing mode the decoder runs: a note under the other mode drops the parts read
   under the first, and so does a settings change, which also drops a record of a protocol no longer decoded. */
static void
test_decoder_settings(void) {
    dsd_state* state = new_state();
    static dsd_opts opts;
    const dsd_received_identity* id = &state->rx_id;
    state->ea_mode = 0;
    dsd_rxid_note_edacs_site(state, 12U);
    dsd_rxid_note_edacs_cc_lcn(state, 7U);
    state->ea_mode = 1;
    dsd_rxid_note_edacs_system(state, 3U);
    expect_u("edacs note under the other addressing mode", id->valid, DSD_RXID_EDACS_SYSTEM);

    DSD_MEMSET(&opts, 0, sizeof opts);
    opts.frame_provoice = 1;
    dsd_rxid_note_decoder_settings(&opts, state);
    expect_u("same addressing mode keeps the record", id->valid, DSD_RXID_EDACS_SYSTEM);
    state->ea_mode = 0;
    dsd_rxid_note_decoder_settings(&opts, state);
    expect_u("other addressing mode drops the record", id->valid, 0U);

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_decoder_settings(&opts, state);
    expect_u("dmr not decoded", id->valid, 0U);
    opts.frame_dmr = 1;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_decoder_settings(&opts, state);
    expect_u("dmr decoded", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_NXDN, 1);
    expect_u("forget another family", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 0);
    expect_u("forget the dmr site", id->valid, DSD_RXID_ACCESS);
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 1);
    expect_u("forget with the access code", id->valid, 0U);
    dsd_rxid_note_decoder_settings(NULL, state);
    free(state);
}

/* A note that invalidates parts (another system, site, cell or subtype on the carrier) starts another record identity:
   the epoch moves, so a reader holding the old one (a call's history row) keeps what it took. Notes that only add or
   repeat, the no-carrier pass and a trunk follow's dropped access code keep the epoch. */
static void
test_invalidation_moves_the_epoch(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    const uint32_t epoch = id->epoch;
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_mark_stale(state);
    dsd_rxid_trunk_tune(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    expect_u("adding, repeating, a follow keep the epoch", id->epoch, epoch);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x294U);
    expect_u("another nac moves the epoch", id->epoch, epoch + 1U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x111U);
    expect_u("a system note after it adds", id->epoch, epoch + 1U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x222U);
    expect_u("another system moves the epoch", id->epoch, epoch + 2U);

    /* An access code alone, replaced by another: another system on the carrier, though nothing else was received. */
    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    const uint32_t dmr_epoch = id->epoch;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    expect_u("the same colour code keeps the epoch", id->epoch, dmr_epoch);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_u("another colour code alone moves the epoch", id->epoch, dmr_epoch + 1U);
    dsd_rxid_trunk_tune(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 3U);
    expect_u("a followed channel's code after a tune keeps it", id->epoch, dmr_epoch + 1U);

    /* A Type-D SRV_INFO naming another site of the same system: the old site's Area goes with its site ID. */
    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 0U);
    dsd_rxid_note_nxdn_idas_site(state, 1U, 5U);
    const uint32_t idas_epoch = id->epoch;
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 7U);
    expect_u("srv_info another site drops the old area", id->valid, DSD_RXID_NXDN_TYPED_SYSTEM);
    expect_u("srv_info another site moves the epoch", id->epoch, idas_epoch + 1U);
    dsd_rxid_note_nxdn_idas_area(state, 0U);
    dsd_rxid_note_nxdn_typed_system(state, 0x12345U, 9U);
    expect_u("srv_info another typed site drops the area too", id->valid, DSD_RXID_NXDN_TYPED_SYSTEM);

    /* A Capacity Plus site change: the rest channel the old site named goes with it. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 1U, 0U, "Cap+ \xC2\xB7 Site 1");
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
    expect_u("another cap+ site drops its rest lsn", id->valid & DSD_RXID_DMR_REST_LSN, 0U);

    /* The vendor a checked LC or CSBK names: a part of its own, which no trunking type rule touches; another system on
       the carrier drops it. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_vendor(state, 0U);
    expect_u("feature id 0 names no vendor", id->valid, 0U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_vendor(state, 0x68U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    expect_u("the vendor stays through a trunking type change", id->valid & DSD_RXID_DMR_VENDOR, DSD_RXID_DMR_VENDOR);
    expect_u("vendor", id->dmr_vendor, 0x68U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_u("another colour code drops the vendor", id->valid & DSD_RXID_DMR_VENDOR, 0U);
    free(state);
}

/* The DMR RAS part: a property of the carrier's codewords, never a decoded field. Noting or forgetting it moves no epoch
   (a call's history row keeps its identity) and drops nothing else; it starts a DMR record as any DMR note does, is
   refused in a frame of no carrier, goes stale with the rest, and goes with the record at a reset or another family's
   sync. A trunk tune's drop keeps it: the engine resets it through the DMR decoder (dmr_ras_reset()) where it applies
   the drop, which dsd_rxid_apply_trunk_tune() and dsd_rxid_request_trunk_tune() report. */
static void
test_dmr_ras_part(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 0U, 2U,
                           "TIII Large \xC2\xB7 Net 1 \xC2\xB7 Site 1");
    const uint32_t epoch = id->epoch;
    dsd_rxid_note_dmr_ras(state);
    expect_u("ras parts", id->valid,
             DSD_RXID_ACCESS | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_RAS);
    expect_u("ras fresh", id->fresh & DSD_RXID_DMR_RAS, DSD_RXID_DMR_RAS);
    expect_u("ras note keeps the epoch", id->epoch, epoch);
    dsd_rxid_note_dmr_ras(state);
    expect_u("ras repeat keeps the epoch", id->epoch, epoch);
    dsd_rxid_mark_stale(state);
    expect_u("ras stale", id->fresh, 0U);
    expect_u("ras stale keeps the value", id->valid & DSD_RXID_DMR_RAS, DSD_RXID_DMR_RAS);
    dsd_rxid_forget_dmr_ras(state);
    expect_u("ras forget", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT);
    expect_u("ras forget keeps the epoch", id->epoch, epoch);

    /* A trunk tune drops the access code and leaves the part to the engine's RAS reset; the calls report the drop. */
    dsd_rxid_note_dmr_ras(state);
    expect_u("apply with nothing pending", (unsigned long)dsd_rxid_apply_trunk_tune(state), 0U);
    expect_u("request outside a dispatch waits", (unsigned long)dsd_rxid_request_trunk_tune(state), 0U);
    expect_u("request waits", id->valid & DSD_RXID_ACCESS, DSD_RXID_ACCESS);
    expect_u("apply reports the drop", (unsigned long)dsd_rxid_apply_trunk_tune(state), 1U);
    expect_u("tune keeps the ras part", id->valid,
             DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_RAS);
    expect_u("apply once", (unsigned long)dsd_rxid_apply_trunk_tune(state), 0U);
    state->frame_dispatch_active = 1;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    expect_u("request inside a dispatch applies at once", (unsigned long)dsd_rxid_request_trunk_tune(state), 1U);
    expect_u("applied at once", id->valid & DSD_RXID_ACCESS, 0U);

    /* A frame read across a carrier boundary notes nothing. */
    dsd_rxid_forget_dmr_ras(state);
    state->frame_dispatch_carrier_seq = state->carrier_seq;
    state->carrier_seq++;
    dsd_rxid_note_dmr_ras(state);
    expect_u("ras in a lost frame", id->valid & DSD_RXID_DMR_RAS, 0U);
    state->frame_dispatch_active = 0;

    /* The record's lifetime: a reset, another family. */
    dsd_rxid_note_dmr_ras(state);
    dsd_rxid_reset(state);
    expect_u("ras reset", id->valid, 0U);
    dsd_rxid_note_dmr_ras(state);
    expect_u("ras starts a dmr record", id->family, DSD_RXID_FAMILY_DMR);
    expect_u("ras alone", id->valid, DSD_RXID_DMR_RAS);
    dsd_rxid_note_sync(state, DSD_SYNC_P25P1_POS);
    expect_u("another family drops ras", id->valid, 0U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_forget_dmr_ras(state);
    expect_u("forget leaves another family alone", id->valid, DSD_RXID_ACCESS);
    free(state);
}

/* The raw DMR network and site numbers (issue #799): Tier III network 12, site 27, model 1 (Small), with the text the
   decoder built from them. The numbers are kept as received, the model with them, the text as given. */
static void
test_dmr_site_raw_parts_noted(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    static const char text[] = "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4.4 \xC2\xB7 SYS 0A3F";
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A3FU);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, text);
    expect_u("raw parts", id->valid,
             DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE);
    expect_u("raw parts fresh", id->fresh & (DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE),
             DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE);
    expect_u("raw net", id->dmr_net, 12U);
    expect_u("raw site", id->dmr_site, 27U);
    expect_u("raw model", id->dmr_t3_model, 1U);
    expect_u("raw trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_TIER3);
    expect_s("raw text", id->dmr_site_text, text);

    /* A note naming no trunking system, or an unknown one, notes nothing. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_NONE, 1, 1U, 2U, 0U, "TIII Small");
    dsd_rxid_note_dmr_site(state, 9U, 1, 1U, 2U, 0U, "TIII Small");
    expect_u("no trunking system", id->valid, 0U);
    dsd_rxid_note_dmr_site(NULL, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 1U, 2U, 0U, "TIII Small");
    free(state);
}

/* The text is presentation (it follows -D's split): the same raw numbers with another text are the same site. The
   epoch stays, the text is replaced and what the site named stays. */
static void
test_dmr_site_text_change_same_raw_is_same_site(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U,
                           "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4.4");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    const uint32_t epoch = id->epoch;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U,
                           "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 27");
    expect_u("same raw keeps the epoch", id->epoch, epoch);
    expect_s("same raw replaces the text", id->dmr_site_text, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 27");
    expect_u("same raw keeps the cc", id->valid & DSD_RXID_CC_FREQ, DSD_RXID_CC_FREQ);
    /* No text: the raw parts stay, a text built for them before is dropped rather than kept beside them. */
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "");
    expect_u("no text keeps the epoch", id->epoch, epoch);
    expect_u("no text drops the text", id->valid, DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_CC_FREQ);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, NULL);
    expect_u("null text", id->valid, DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_CC_FREQ);
    free(state);
}

/* Another raw part than a valid one (site, network, network presence, Tier III model) is another site: the epoch moves
   and what the old site named (its rest LSN, free channel and control channel) goes with it. */
static void
test_dmr_site_raw_change_invalidates(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 1U, 0U, "Cap+ \xC2\xB7 Site 1");
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    uint32_t epoch = id->epoch;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
    expect_u("another site moves the epoch", id->epoch, epoch + 1U);
    expect_u("another site drops the rest lsn and cc", id->valid,
             DSD_RXID_ACCESS | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT);
    expect_u("another site value", id->dmr_site, 2U);
    expect_s("another site text", id->dmr_site_text, "Cap+ \xC2\xB7 Site 2");

    /* Tier III: the network, the model and the network's presence each name another site. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "TIII Small");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    epoch = id->epoch;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 13U, 27U, 1U, "TIII Small");
    expect_u("another net moves the epoch", id->epoch, epoch + 1U);
    expect_u("another net drops the cc", id->valid & DSD_RXID_CC_FREQ, 0U);
    expect_u("another net value", id->dmr_net, 13U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 13U, 27U, 2U, "TIII Small");
    expect_u("another model moves the epoch", id->epoch, epoch + 2U);
    expect_u("another model drops the cc", id->valid & DSD_RXID_CC_FREQ, 0U);
    expect_u("another model value", id->dmr_t3_model, 2U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 0, 0U, 27U, 2U, "TIII Small");
    expect_u("a net gone moves the epoch", id->epoch, epoch + 3U);
    expect_u("a net gone", id->valid, DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 27U, 2U, "TIII Small");
    expect_u("a net appearing moves the epoch", id->epoch, epoch + 4U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 27U, 2U, "TIII Small");
    expect_u("a repeat keeps the epoch", id->epoch, epoch + 4U);
    free(state);
}

/* Every lifecycle step that drops a DMR site drops its raw numbers with the text: another colour code on the carrier,
   the DMR reset command, another trunking subtype, another Tier III system code. */
static void
test_dmr_raw_parts_drop_on_access_change_forget_and_subtype(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    const uint32_t raw = DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "TIII Small");
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_u("access change drops raw parts", id->valid & raw, 0U);
    expect_u("access change keeps the code", id->valid, DSD_RXID_ACCESS);

    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "TIII Small");
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 0);
    expect_u("forget drops raw parts", id->valid, DSD_RXID_ACCESS);

    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "TIII Small");
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_u("subtype switch drops raw parts", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_BRAND);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 5U, 0U, "Con+");
    expect_u("raw note of another subtype drops the brand", id->valid, DSD_RXID_ACCESS | raw | DSD_RXID_DMR_SITE_TEXT);
    expect_u("raw note of another subtype", id->dmr_trunk, DSD_RXID_DMR_TRUNK_CONNECT_PLUS);

    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A3FU);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U, "TIII Small");
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A40U);
    expect_u("syscode change drops raw parts", id->valid, DSD_RXID_DMR_T3_SYSCODE);
    free(state);
}

/* Capacity Plus names a site and no network: only the site part is set, and a model only Tier III has stays 0. */
static void
test_capplus_site_has_no_net(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 7U, 9U, 3U, "Cap+ \xC2\xB7 Site 9");
    expect_u("cap+ site only", id->valid, DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT);
    expect_u("cap+ site", id->dmr_site, 9U);
    expect_u("cap+ no net value", id->dmr_net, 0U);
    expect_u("cap+ no model", id->dmr_t3_model, 0U);
    expect_u("cap+ trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS);
    free(state);
}

/* Leaves the bytes below the caller's frame set to @p value, where the next call's locals will sit. */
static void
dirty_stack(unsigned char value) {
    volatile unsigned char junk[512];
    for (size_t i = 0U; i < sizeof(junk); i++) {
        junk[i] = value;
    }
}

static void
note_texts(dsd_state* state, unsigned char junk) {
    DSD_MEMSET(state->rx_id.dmr_site_text, junk, sizeof(state->rx_id.dmr_site_text));
    DSD_MEMSET(state->rx_id.dmr_brand, junk, sizeof(state->rx_id.dmr_brand));
    dirty_stack(junk);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dirty_stack(junk);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
}

/* Equal notes leave equal bytes (F11): a text copy fills its whole field, past the NUL, so whatever the field or the
   stack held before, a whole-record compare (a history row's frozen copy) sees no change. */
static void
test_text_notes_leave_identical_bytes(void) {
    dsd_state* a = new_state();
    dsd_state* b = new_state();
    note_texts(a, 0xAAU);
    note_texts(b, 0x55U);
    expect_u("identical notes, identical bytes", (unsigned long)(memcmp(&a->rx_id, &b->rx_id, sizeof(a->rx_id)) == 0),
             1U);
    expect_s("site text", a->rx_id.dmr_site_text, "Cap+ \xC2\xB7 Site 2");
    expect_s("brand", b->rx_id.dmr_brand, "Capacity Plus");
    free(a);
    free(b);
}

/* A checked Tier III site, as the DMR decoder notes it: the system code first, then the raw numbers and the text. */
static void
note_t3_site(dsd_state* state, uint16_t syscode, uint16_t net, uint16_t site, uint8_t model) {
    dsd_rxid_note_dmr_t3_syscode(state, syscode);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, net, site, model, "TIII");
}

static void
expect_capmax(const char* label, const dsd_state* state, uint8_t model, uint16_t net, unsigned long want) {
    expect_u(label, (unsigned long)dsd_rxid_dmr_t3_is_capmax(state, model, net), want);
}

/* The Capacity Max marker names one Tier III network by its model and raw network (never syscode >> SITE width:
   Large NET 1 and Tiny NET 33 share 0x21), and only that pair reads as recognised. */
static void
test_dmr_t3_capmax_marker_pair(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    expect_u("marker valid", id->valid, DSD_RXID_DMR_T3_CAPMAX);
    expect_u("marker never fresh", id->fresh, 0U);
    expect_u("marker family", id->family, DSD_RXID_FAMILY_DMR);
    expect_u("marker trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_TIER3);
    expect_u("marker epoch", id->epoch, 0U);
    expect_capmax("exact pair", state, 2U, 1U, 1U);
    expect_capmax("tiny net 33 shares syscode >> width", state, 0U, 33U, 0U);
    expect_capmax("other net", state, 2U, 2U, 0U);
    expect_capmax("other model", state, 1U, 1U, 0U);
    expect_capmax("null state", NULL, 2U, 1U, 0U);
    dsd_rxid_note_dmr_t3_capmax(NULL, 2U, 1U);

    /* A model or network out of range names no network and changes nothing. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_t3_capmax(state, 4U, 1U);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 16U);
    dsd_rxid_note_dmr_t3_capmax(state, 3U, 4U);
    dsd_rxid_note_dmr_t3_capmax(state, 1U, 128U);
    dsd_rxid_note_dmr_t3_capmax(state, 0U, 512U);
    expect_u("invalid ignored", id->valid, 0U);
    expect_u("invalid starts no record", id->family, DSD_RXID_FAMILY_NONE);
    dsd_rxid_note_dmr_t3_capmax(state, 0U, 511U);
    expect_capmax("tiny max net", state, 0U, 511U, 1U);
    dsd_rxid_note_dmr_t3_capmax(state, 3U, 3U);
    expect_capmax("replaced pair", state, 3U, 3U, 1U);
    expect_capmax("old pair gone", state, 0U, 511U, 0U);

    /* Only a DMR record holds it. */
    state->rx_id.family = (uint8_t)DSD_RXID_FAMILY_P25;
    expect_capmax("other family", state, 3U, 3U, 0U);
    free(state);
}

/* What drops a system drops the marker; what keeps it (a trunk follow, the no-carrier pass, the RAS reset, another
   site of the same network) keeps the marker. */
static void
test_dmr_t3_capmax_marker_lifecycle(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    expect_capmax("access change drops", state, 2U, 1U, 0U);

    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 0);
    expect_capmax("forget drops", state, 2U, 1U, 0U);

    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_capmax("another trunking system drops", state, 2U, 1U, 0U);

    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_reset(state);
    expect_capmax("reset drops", state, 2U, 1U, 0U);

    note_t3_site(state, 0x2101U, 1U, 1U, 2U);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_trunk_tune(state);
    expect_capmax("trunk tune keeps", state, 2U, 1U, 1U);
    dsd_rxid_mark_stale(state);
    expect_capmax("stale keeps", state, 2U, 1U, 1U);
    /* The RAS reset (a new carrier, a colour code change) drops the RAS part alone; the network stays recognised. */
    dsd_rxid_note_dmr_ras(state);
    dsd_rxid_forget_dmr_ras(state);
    expect_u("ras reset drops ras", id->valid & DSD_RXID_DMR_RAS, 0U);
    expect_capmax("ras reset keeps", state, 2U, 1U, 1U);
    note_t3_site(state, 0x2102U, 1U, 2U, 2U);
    expect_capmax("same network's other site keeps", state, 2U, 1U, 1U);
    dsd_rxid_note_dmr_t3_syscode(state, 0x2201U);
    expect_capmax("another network drops", state, 2U, 1U, 0U);

    /* With no system code before it as well; Tiny 0x0109 is NET 33, another network than Large NET 1. */
    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_note_dmr_t3_syscode(state, 0x0109U);
    expect_capmax("cross-model code drops", state, 2U, 1U, 0U);
    expect_u("cross-model code noted", id->valid, DSD_RXID_DMR_T3_SYSCODE);

    /* Old site A, then B's marker, then B's checked code: the code drops A's site and keeps B's marker. */
    dsd_rxid_reset(state);
    note_t3_site(state, 0x2201U, 2U, 1U, 2U);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    note_t3_site(state, 0x2101U, 1U, 1U, 2U);
    expect_capmax("marker of the network noted next kept", state, 2U, 1U, 1U);
    expect_u("network B's numbers", id->dmr_net, 1U);
    free(state);
}

/* The marker is no part of the identity: noting or dropping it alone never moves the epoch, and it never reads as
   fresh. */
static void
test_dmr_t3_capmax_marker_epoch(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;
    const uint32_t site_parts =
        DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_CC_FREQ;

    note_t3_site(state, 0x2201U, 2U, 1U, 2U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    const uint32_t epoch = id->epoch;
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    expect_u("marker install keeps epoch", id->epoch, epoch);
    expect_u("marker not fresh", id->fresh & DSD_RXID_DMR_T3_CAPMAX, 0U);
    note_t3_site(state, 0x2201U, 2U, 1U, 2U);
    expect_capmax("checked A drops marker B", state, 2U, 1U, 0U);
    expect_u("marker drop keeps epoch", id->epoch, epoch);
    expect_u("A's parts intact", id->valid, site_parts);
    expect_u("A's net", id->dmr_net, 2U);
    expect_u("A's site", id->dmr_site, 1U);
    expect_u("A's cc", id->cc_freq_hz, 451000000U);

    /* A record with only the marker: stale stays stale when it is noted again; dropping it alone moves nothing. */
    dsd_rxid_reset(state);
    const uint32_t epoch_marker = id->epoch;
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_mark_stale(state);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    expect_u("re-noted marker stays stale", id->fresh, 0U);
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 0);
    expect_u("forget of marker only", id->valid, 0U);
    expect_u("forget of marker only keeps epoch", id->epoch, epoch_marker);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_u("trunk switch of marker only keeps epoch", id->epoch, epoch_marker);
    /* A drop that takes real parts too moves it, as before. */
    note_t3_site(state, 0x2101U, 1U, 1U, 2U);
    dsd_rxid_note_dmr_t3_capmax(state, 2U, 1U);
    const uint32_t epoch_site = id->epoch;
    dsd_rxid_forget_system(state, DSD_RXID_FAMILY_DMR, 0);
    expect_u("forget with parts moves epoch", id->epoch, epoch_site + 1U);
    free(state);
}

int
main(void) {
    test_parts_and_reset();
    test_dmr_xpt_free_lcn();
    test_freshness();
    test_family_replacement();
    test_p25_merge();
    test_dependent_invalidation();
    test_trunk_tune();
    test_access_change();
    test_nxdn_idas_site_reconcile();
    test_lost_frame_notes_nothing();
    test_nxdn_subtypes_exclusive();
    test_nxdn_location_reserved_refused();
    test_dmr_trunking_types_exclusive();
    test_decoder_settings();
    test_invalidation_moves_the_epoch();
    test_dmr_ras_part();
    test_dmr_site_raw_parts_noted();
    test_dmr_site_text_change_same_raw_is_same_site();
    test_dmr_site_raw_change_invalidates();
    test_dmr_raw_parts_drop_on_access_change_forget_and_subtype();
    test_capplus_site_has_no_net();
    test_text_notes_leave_identical_bytes();
    test_dmr_t3_capmax_marker_pair();
    test_dmr_t3_capmax_marker_lifecycle();
    test_dmr_t3_capmax_marker_epoch();
    dsd_rxid_reset(NULL);
    dsd_rxid_note_sync(NULL, DSD_SYNC_DMR_BS_DATA_POS);
    if (g_fail) {
        return 1;
    }
    printf("CORE_RECEIVED_IDENTITY: OK\n");
    return 0;
}
