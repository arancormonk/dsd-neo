// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The system identity received on the carrier being decoded (dsd_state::rx_id): independent parts, the P25 SYSID
 * binding, dependent invalidation, family replacement, freshness, and the reset and trunk-tune lifetime.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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

int
main(void) {
    test_parts_and_reset();
    test_dmr_xpt_free_lcn();
    test_freshness();
    test_family_replacement();
    test_p25_merge();
    test_dependent_invalidation();
    test_trunk_tune();
    dsd_rxid_reset(NULL);
    dsd_rxid_note_sync(NULL, DSD_SYNC_DMR_BS_DATA_POS);
    if (g_fail) {
        return 1;
    }
    printf("CORE_RECEIVED_IDENTITY: OK\n");
    return 0;
}
