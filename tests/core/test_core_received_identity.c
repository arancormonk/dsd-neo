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
    dsd_rxid_note_dmr_site_text(state, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3.1 \xC2\xB7 SYS 0A3F");
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A3FU);
    expect_s("dmr site text", id->dmr_site_text, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 3.1 \xC2\xB7 SYS 0A3F");
    expect_s("dmr brand", id->dmr_brand, "Capacity Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A40U);
    expect_u("syscode change drops site text", id->valid & DSD_RXID_DMR_SITE_TEXT, 0U);
    dsd_rxid_note_dmr_site_text(state, "");
    expect_u("empty text ignored", id->valid & DSD_RXID_DMR_SITE_TEXT, 0U);
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
    dsd_rxid_note_dmr_site_text(state, "Cap+ \xC2\xB7 Site 7");
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_dmr_rest_lsn(state, 4U);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    const uint32_t dmr_system = DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_REST_LSN | DSD_RXID_CC_FREQ;
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
    dsd_rxid_note_dmr_site_text(state, "Cap+ \xC2\xB7 Site 7");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    dsd_rxid_trunk_tune(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 5U);
    expect_u("dmr follow keeps the system", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_CC_FREQ);

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
    dsd_rxid_note_dmr_site_text(state, "Net 1 Â· Site 2");
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

/* Capacity Plus, Connect Plus, Hytera XPT and Tier III are different DMR trunking systems: checked content naming
   another one than the record's drops the old one's parts and its control channel; the colour code is the carrier's
   and stays. */
static void
test_dmr_trunking_types_exclusive(void) {
    dsd_state* state = new_state();
    const dsd_received_identity* id = &state->rx_id;

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site_text(state, "Cap+ \xC2\xB7 Site 2");
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, 451000000U);
    expect_u("cap+ parts", id->valid,
             DSD_RXID_ACCESS | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_BRAND | DSD_RXID_CC_FREQ);
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    expect_u("xpt brand drops cap+", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_BRAND);
    dsd_rxid_note_dmr_xpt_free_lcn(state, 4U);
    expect_u("xpt free lcn joins xpt", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_XPT_FREE_LCN);
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    expect_u("cap+ rest drops xpt", id->valid, DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN);

    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site_text(state, "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    dsd_rxid_note_dmr_t3_syscode(state, 0x0A3FU);
    expect_u("tier iii drops con+", id->valid, DSD_RXID_DMR_T3_SYSCODE);
    dsd_rxid_note_dmr_site_text(state, "TIII Small \xC2\xB7 Net 1 \xC2\xB7 Site 2 \xC2\xB7 SYS 0A3F");
    expect_u("tier iii site joins", id->valid, DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_SITE_TEXT);
    dsd_rxid_note_dmr_site_text(state, "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    expect_u("con+ site drops tier iii", id->valid, DSD_RXID_DMR_SITE_TEXT);

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
    test_dmr_trunking_types_exclusive();
    test_decoder_settings();
    dsd_rxid_reset(NULL);
    dsd_rxid_note_sync(NULL, DSD_SYNC_DMR_BS_DATA_POS);
    if (g_fail) {
        return 1;
    }
    printf("CORE_RECEIVED_IDENTITY: OK\n");
    return 0;
}
