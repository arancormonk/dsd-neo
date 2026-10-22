// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The system identity every frontend shows for the carrier being decoded: built from the received identity record
 * only (never from seeds, presets or another carrier's decoder state), access code first, compact and full forms, and
 * whether it is live.
 */

#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

static void
expect_text(const char* label, const dsd_state* state, int form, int with_protocol, const char* want) {
    dsd_app_site_identity view;
    dsd_app_site_identity_view(state, &view);
    char got[256];
    const size_t n = dsd_app_site_identity_format(&view, form, with_protocol, got, sizeof(got));
    if (strcmp(got, want) != 0 || n != strlen(want)) {
        DSD_FPRINTF(stderr, "FAIL %s: got \"%s\" (%zu), want \"%s\"\n", label, got, n, want);
        g_fail = 1;
    }
}

static void
expect_u(const char* label, unsigned long got, unsigned long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %lu, want %lu\n", label, got, want);
        g_fail = 1;
    }
}

static dsd_state*
new_state(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (state == NULL) {
        exit(2);
    }
    state->synctype = DSD_SYNC_NONE;
    return state;
}

#define DOT " \xC2\xB7 "

static void
test_dmr(void) {
    dsd_state* state = new_state();
    expect_text("empty", state, DSD_APP_SITE_IDENTITY_FULL, 1, "");
    /* Decoder state alone shows nothing: only the record counts. */
    state->dmr_color_code = 1U;
    state->dmr_rest_channel = 3;
    expect_text("decoder state ignored", state, DSD_APP_SITE_IDENTITY_FULL, 1, "");

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_text("dmr compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "CC 1" DOT "Rest LSN 3");
    expect_text("dmr compact prefix", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "DMR" DOT "CC 1" DOT "Rest LSN 3");
    expect_text("dmr full", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "DMR" DOT "CC 1" DOT "Rest LSN 3" DOT "Capacity Plus");

    dsd_app_site_identity view;
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    dsd_app_site_identity_view(state, &view);
    expect_u("live while synced and fresh", view.live, 1U);
    state->synctype = DSD_SYNC_YSF_POS;
    dsd_app_site_identity_view(state, &view);
    expect_u("not live on another family", view.live, 0U);
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    dsd_rxid_mark_stale(state);
    dsd_app_site_identity_view(state, &view);
    expect_u("not live when stale", view.live, 0U);
    expect_u("stale keeps tokens", view.token_count, 3U);

    /* A Hytera XPT site's free channel, after the site and rest tokens: it identifies the site's state on the compact
       line as the terminal's "Free LCN" always did. */
    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    dsd_rxid_note_dmr_xpt_free_lcn(state, 4U);
    expect_text("xpt compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "DMR" DOT "CC 2" DOT "Free LCN 4");
    expect_text("xpt full", state, DSD_APP_SITE_IDENTITY_FULL, 0, "CC 2" DOT "Free LCN 4" DOT "Hytera XPT");
    dsd_rxid_note_dmr_site_text(state, "TIII Small \xC2\xB7 Net 1 \xC2\xB7 Site 2");
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    expect_text("free lcn after the site and rest tokens", state, DSD_APP_SITE_IDENTITY_COMPACT, 0,
                "CC 2" DOT "TIII Small" DOT "Net 1" DOT "Site 2" DOT "Rest LSN 5" DOT "Free LCN 4");
    free(state);
}

static void
test_p25(void) {
    dsd_state* state = new_state();
    /* A hand-set Phase 2 seed is configuration, not identity. */
    state->p2_wacn = 0xBEE00ULL;
    state->p2_sysid = 0x348ULL;
    state->p2_cc = 0x293ULL;
    state->p2_hardset = 1;
    expect_text("seed ignored", state, DSD_APP_SITE_IDENTITY_FULL, 1, "");

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_p25_lra(state, 0x348U, 0x1AU);
    expect_text("p25 compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0,
                "NAC 293" DOT "SYS 348" DOT "RFSS 3" DOT "SITE 21" DOT "WACN BEE00");
    expect_text("p25 full", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "P25" DOT "NAC 293" DOT "SYS 348" DOT "RFSS 3" DOT "SITE 21" DOT "WACN BEE00" DOT "LRA 1A");

    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    dsd_app_site_identity view;
    dsd_app_site_identity_view(state, &view);
    expect_u("cc freq", view.cc_freq_hz, 851012500U);

    /* Cut short, the text stays terminated. */
    char small[12];
    const size_t n = dsd_app_site_identity_format(&view, DSD_APP_SITE_IDENTITY_FULL, 1, small, sizeof(small));
    expect_u("truncated length", n, sizeof(small) - 1U);
    expect_u("truncated terminated", small[sizeof(small) - 1U] == '\0', 1U);
    free(state);
}

static void
test_others(void) {
    dsd_state* state = new_state();
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 0U, 12U, 3001U);
    expect_text("nxdn full", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "NXDN" DOT "RAN 5" DOT "SYS 12" DOT "SITE 3001" DOT "Global");
    expect_text("nxdn compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "RAN 5" DOT "SYS 12" DOT "SITE 3001");

    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 3U, 1U);
    expect_text("idas", state, DSD_APP_SITE_IDENTITY_FULL, 1, "IDAS" DOT "Area 1" DOT "SITE 1" DOT "Narrow");

    dsd_rxid_reset(state);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_text("tetra", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "TETRA" DOT "MCC 262" DOT "MNC 1001" DOT "CC 25" DOT "LA 4242");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_M17, DSD_ACCESS_CODE_CAN, 0U);
    expect_text("m17 can 0", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "M17" DOT "CAN 0");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DPMR, DSD_ACCESS_CODE_COLOR_CODE, 63U);
    expect_text("dpmr", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "dPMR" DOT "CC 63");

    dsd_rxid_reset(state);
    dsd_rxid_note_edacs_site(state, 42U);
    state->ea_mode = 1;
    state->esk_mask = 0xA0;
    expect_text("edacs", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "EDACS" DOT "SITE 042 [2A]" DOT "Extended Addressing" DOT "ESK");
    expect_text("edacs compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "SITE 042 [2A]");
    free(state);
}

int
main(void) {
    test_dmr();
    test_p25();
    test_others();
    dsd_app_site_identity_view(NULL, NULL);
    expect_text("null state", NULL, DSD_APP_SITE_IDENTITY_FULL, 1, "");
    if (g_fail) {
        return 1;
    }
    printf("APP_CONTROL_SITE_IDENTITY_VIEW: OK\n");
    return 0;
}
