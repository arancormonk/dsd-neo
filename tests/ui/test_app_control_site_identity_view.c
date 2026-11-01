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
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
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

    /* A Hytera XPT site's free channel, after the access code: it identifies the site's state on the compact
       line as the terminal's "Free LCN" always did. */
    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 2U);
    dsd_rxid_note_dmr_brand(state, "Hytera XPT");
    dsd_rxid_note_dmr_xpt_free_lcn(state, 4U);
    expect_text("xpt compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "DMR" DOT "CC 2" DOT "Free LCN 4");
    expect_text("xpt full", state, DSD_APP_SITE_IDENTITY_FULL, 0, "CC 2" DOT "Free LCN 4" DOT "Hytera XPT");
    /* A Capacity Plus site on the same carrier is another system: the XPT site's free channel goes. */
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
    dsd_rxid_note_dmr_rest_lsn(state, 5U);
    expect_text("another trunking system replaces the xpt site", state, DSD_APP_SITE_IDENTITY_COMPACT, 0,
                "CC 2" DOT "Site 2" DOT "Rest LSN 5");
    free(state);
}

/* The DMR site text is one token per part, so a narrow line keeps the network and site that fit: those two are
   compact; the Tier III model, the Capacity Plus and Connect Plus tags and the Tier III system code are full form. */
static void
test_dmr_site_parts(void) {
    dsd_state* state = new_state();
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_t3_syscode(state, 0x1170U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 16U, 1U,
                           "TIII Small" DOT "Net 12" DOT "Site 3.1" DOT "SYS 1170");
    dsd_app_site_identity view;
    dsd_app_site_identity_view(state, &view);
    static const char* const texts[] = {"CC 1", "TIII Small", "Net 12", "Site 3.1", "SYS 1170"};
    static const uint8_t compact[] = {1U, 0U, 1U, 1U, 0U};
    expect_u("tier iii tokens", view.token_count, 5U);
    for (size_t i = 0U; i < 5U && i < view.token_count; i++) {
        if (strcmp(view.tokens[i].text, texts[i]) != 0 || view.tokens[i].compact != compact[i]) {
            DSD_FPRINTF(stderr, "FAIL tier iii token %zu: got \"%s\" (%u)\n", i, view.tokens[i].text,
                        (unsigned)view.tokens[i].compact);
            g_fail = 1;
        }
    }
    expect_text("tier iii compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "CC 1" DOT "Net 12" DOT "Site 3.1");
    expect_text("tier iii full", state, DSD_APP_SITE_IDENTITY_FULL, 0,
                "CC 1" DOT "TIII Small" DOT "Net 12" DOT "Site 3.1" DOT "SYS 1170");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+" DOT "Site 2");
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_text("cap+ compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "CC 1" DOT "Site 2" DOT "Rest LSN 3");
    expect_text("cap+ full", state, DSD_APP_SITE_IDENTITY_FULL, 0,
                "CC 1" DOT "Cap+" DOT "Site 2" DOT "Rest LSN 3" DOT "Capacity Plus");

    dsd_rxid_reset(state);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 5U, 0U,
                           "Con+" DOT "Net 3" DOT "Site 5");
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    expect_text("con+ compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "Net 3" DOT "Site 5");
    expect_text("con+ full", state, DSD_APP_SITE_IDENTITY_FULL, 0, "Con+" DOT "Net 3" DOT "Site 5" DOT "Connect Plus");
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

    /* The decoder runs extended addressing before it reads the site (the record keeps the mode it was read under). */
    dsd_rxid_reset(state);
    state->ea_mode = 1;
    state->esk_mask = 0xA0;
    dsd_rxid_note_edacs_site(state, 42U);
    expect_text("edacs", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "EDACS" DOT "SITE 042 [2A]" DOT "Extended Addressing" DOT "ESK");
    expect_text("edacs compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "SITE 042 [2A]");
    free(state);
}

/* RAS on the carrier is a compact token after the site's own, before the full-form details; a record of another family,
   or one the RAS reset dropped it from, shows none. */
static void
test_dmr_ras(void) {
    dsd_state* state = new_state();
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 0U);
    dsd_rxid_note_dmr_ras(state);
    expect_text("ras compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 1, "DMR" DOT "CC 0" DOT "RAS");
    expect_text("ras full", state, DSD_APP_SITE_IDENTITY_FULL, 0, "CC 0" DOT "RAS");
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 0U, 0U, 2U,
                           "TIII Large \xC2\xB7 Net 1 \xC2\xB7 Site 1");
    dsd_rxid_note_dmr_brand(state, "Motorola Tier III");
    expect_text("ras with a site compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0,
                "CC 0" DOT "Net 1" DOT "Site 1" DOT "RAS");
    expect_text("ras with a site full", state, DSD_APP_SITE_IDENTITY_FULL, 0,
                "CC 0" DOT "TIII Large" DOT "Net 1" DOT "Site 1" DOT "RAS" DOT "Motorola Tier III");
    dsd_rxid_forget_dmr_ras(state);
    expect_text("ras forgotten", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "CC 0" DOT "Net 1" DOT "Site 1");
    free(state);
}

static void
expect_s(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", label, got, want);
        g_fail = 1;
    }
}

/* The record form of @p state's record, joined full with the protocol. */
static void
expect_record_text(const char* label, const dsd_received_identity* id, const char* want) {
    dsd_app_site_identity view;
    dsd_app_site_identity_view_record(id, &view);
    char got[256];
    const size_t n = dsd_app_site_identity_format(&view, DSD_APP_SITE_IDENTITY_FULL, 1, got, sizeof(got));
    expect_s(label, got, want);
    expect_u(label, n, strlen(want));
}

/* The live view is the record form plus what only the running decoder knows: ESK and whether the carrier is live. */
static void
expect_record_matches_live(const char* label, const dsd_state* state, size_t live_extra) {
    dsd_app_site_identity live;
    dsd_app_site_identity record;
    dsd_app_site_identity_view(state, &live);
    dsd_app_site_identity_view_record(&state->rx_id, &record);
    expect_s(label, record.protocol, live.protocol);
    expect_u(label, record.family, live.family);
    expect_u(label, record.epoch, live.epoch);
    expect_u(label, record.cc_freq_hz, live.cc_freq_hz);
    expect_u(label, record.live, 0U);
    expect_u(label, record.token_count + live_extra, live.token_count);
    for (size_t i = 0U; i < record.token_count && i < live.token_count; i++) {
        expect_s(label, record.tokens[i].text, live.tokens[i].text);
        expect_u(label, record.tokens[i].compact, live.tokens[i].compact);
    }
}

static void
test_record_form_matches_live_without_esk(void) {
    dsd_state* state = new_state();
    state->synctype = DSD_SYNC_P25P1_POS;
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_p25_lra(state, 0x348U, 0x1AU);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    dsd_app_site_identity live;
    dsd_app_site_identity_view(state, &live);
    expect_u("p25 live view is live", live.live, 1U);
    expect_record_matches_live("p25 record", state, 0U);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_t3_syscode(state, 0x1170U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U,
                           "TIII Small" DOT "Net 13" DOT "Site 28" DOT "SYS 1170");
    dsd_rxid_note_dmr_brand(state, "Motorola Tier III");
    expect_record_matches_live("dmr record", state, 0U);

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 1U, 12U, 201U);
    expect_record_matches_live("nxdn record", state, 0U);

    dsd_rxid_reset(state);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_record_matches_live("tetra record", state, 0U);

    /* EDACS: the live view adds the ESK word last, the record form leaves it out. */
    dsd_rxid_reset(state);
    state->ea_mode = 1;
    state->esk_mask = 0xA0;
    dsd_rxid_note_edacs_site(state, 42U);
    dsd_rxid_note_edacs_cc_lcn(state, 3U);
    expect_record_matches_live("edacs record", state, 1U);
    expect_record_text("edacs record text", &state->rx_id,
                       "EDACS" DOT "SITE 042 [2A]" DOT "CC LCN 3" DOT "Extended Addressing");
    expect_text("edacs live text", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "EDACS" DOT "SITE 042 [2A]" DOT "CC LCN 3" DOT "Extended Addressing" DOT "ESK");
    state->esk_mask = 0U;
    expect_text("edacs live no esk", state, DSD_APP_SITE_IDENTITY_FULL, 0,
                "SITE 042 [2A]" DOT "CC LCN 3" DOT "Extended Addressing" DOT "No ESK");

    dsd_rxid_reset(state);
    expect_record_text("empty record", &state->rx_id, "");
    dsd_app_site_identity_view_record(NULL, NULL);
    dsd_app_site_identity record;
    DSD_MEMSET(&record, 0xFF, sizeof(record));
    dsd_app_site_identity_view_record(NULL, &record);
    expect_u("null record zeroes", record.token_count, 0U);
    expect_s("null record no protocol", record.protocol, "");
    free(state);
}

/* The record names the addressing mode its EDACS parts were read under: the record form reads that, never the
   decoder's current setting. */
static void
test_record_form_edacs_mode_from_record(void) {
    dsd_state* state = new_state();
    state->ea_mode = 1;
    dsd_rxid_note_edacs_site(state, 7U);
    dsd_received_identity saved = state->rx_id;
    state->ea_mode = 0;
    expect_record_text("extended from the record", &saved, "EDACS" DOT "SITE 007 [07]" DOT "Extended Addressing");

    dsd_rxid_reset(state);
    state->ea_mode = 0;
    dsd_rxid_note_edacs_site(state, 7U);
    saved = state->rx_id;
    state->ea_mode = 1;
    expect_record_text("standard from the record", &saved, "EDACS" DOT "SITE 007 [07]" DOT "Standard/Networked");
    free(state);
}

/* The EDACS system reads hex, as the decoder log ("System ID [%04X]") and the event line print it. */
static void
test_edacs_sys_hex(void) {
    dsd_state* state = new_state();
    state->ea_mode = 1;
    state->esk_mask = 0U;
    dsd_rxid_note_edacs_system(state, 0x0A3FU);
    dsd_rxid_note_edacs_area(state, 2U);
    expect_text("edacs live sys hex", state, DSD_APP_SITE_IDENTITY_FULL, 1,
                "EDACS" DOT "SYS 0A3F" DOT "Area 2" DOT "Extended Addressing" DOT "No ESK");
    expect_text("edacs live sys hex compact", state, DSD_APP_SITE_IDENTITY_COMPACT, 0, "SYS 0A3F");
    expect_record_text("edacs record sys hex", &state->rx_id,
                       "EDACS" DOT "SYS 0A3F" DOT "Area 2" DOT "Extended Addressing");
    free(state);
}

static void
expect_channel_token(const char* label, uint8_t kind, uint32_t channel, const char* want) {
    char got[32];
    DSD_MEMSET(got, 'x', sizeof(got));
    const size_t n = dsd_app_call_channel_token(kind, channel, got, sizeof(got));
    expect_s(label, got, want);
    expect_u(label, n, strlen(want));
}

static void
test_channel_tokens(void) {
    expect_channel_token("lsn", DSD_CALL_CHANNEL_LSN, 5U, "LSN 5");
    expect_channel_token("lpcn decimal", DSD_CALL_CHANNEL_LPCN, 12U, "LPCN 12");
    expect_channel_token("lcn", DSD_CALL_CHANNEL_LCN, 3U, "LCN 3");
    expect_channel_token("lpcn wide", DSD_CALL_CHANNEL_LPCN, 4095U, "LPCN 4095");
    expect_channel_token("none kind", DSD_CALL_CHANNEL_NONE, 7U, "");
    expect_channel_token("no channel", DSD_CALL_CHANNEL_LSN, 0U, "");
    expect_channel_token("unknown kind", 9U, 7U, "");
    char small[4];
    expect_u("token cut short", dsd_app_call_channel_token(DSD_CALL_CHANNEL_LPCN, 12U, small, sizeof(small)), 3U);
    expect_s("token cut short text", small, "LPC");
    expect_u("token null buffer", dsd_app_call_channel_token(DSD_CALL_CHANNEL_LSN, 5U, NULL, 8U), 0U);
    expect_u("token no capacity", dsd_app_call_channel_token(DSD_CALL_CHANNEL_LSN, 5U, small, 0U), 0U);
}

/* The history line of a row heard under @p state's record: the record's site parts (dsd_rxid_site_snapshot(), as a
   history row keeps them) and the call's own channel. */
static void
expect_history(const char* label, const dsd_state* state, uint8_t kind, uint32_t channel, const char* want) {
    dsd_received_identity site;
    dsd_rxid_site_snapshot(&state->rx_id, &site);
    char got[256];
    DSD_MEMSET(got, 'x', sizeof(got));
    const size_t n = dsd_app_site_history_text(&site, kind, channel, got, sizeof(got));
    expect_s(label, got, want);
    expect_u(label, n, strlen(want));
}

static void
test_history_text_examples(void) {
    dsd_state* state = new_state();
    expect_history("empty", state, DSD_CALL_CHANNEL_NONE, 0U, "");
    expect_history("channel only", state, DSD_CALL_CHANNEL_LPCN, 12U, "DMR" DOT "LPCN 12");

    dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_p25_network(state, 0xBEE00U, 0x348U);
    dsd_rxid_note_p25_site(state, 0x348U, 3U, 21U);
    dsd_rxid_note_p25_lra(state, 0x348U, 0x1AU);
    dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_P25, 851012500U);
    expect_history("p25", state, DSD_CALL_CHANNEL_NONE, 0x1234U,
                   "P25" DOT "SYS 348" DOT "RFSS 3" DOT "SITE 21" DOT "WACN BEE00" DOT "LRA 1A");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 1234U, 5U, 0U,
                           "Con+" DOT "Net 1234" DOT "Site 5");
    dsd_rxid_note_dmr_brand(state, "Connect Plus");
    expect_history("con+ with lcn", state, DSD_CALL_CHANNEL_LCN, 3U,
                   "DMR" DOT "Con+" DOT "Net 1234" DOT "Site 5" DOT "Connect Plus" DOT "LCN 3");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_t3_syscode(state, 0x1170U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 12U, 27U, 1U,
                           "TIII Small" DOT "Net 13" DOT "Site 28" DOT "SYS 1170");
    dsd_rxid_note_dmr_ras(state);
    expect_history("tier iii with lpcn", state, DSD_CALL_CHANNEL_LPCN, 12U,
                   "DMR" DOT "TIII Small" DOT "Net 13" DOT "Site 28" DOT "SYS 1170" DOT "LPCN 12");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 9U, 0U, "Cap+" DOT "Site 9");
    dsd_rxid_note_dmr_rest_lsn(state, 3U);
    dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    expect_history("cap+", state, DSD_CALL_CHANNEL_LSN, 5U,
                   "DMR" DOT "Cap+" DOT "Site 9" DOT "Capacity Plus" DOT "LSN 5");

    dsd_rxid_reset(state);
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_NXDN, DSD_ACCESS_CODE_RAN, 5U);
    dsd_rxid_note_nxdn_location(state, 1U, 12U, 201U);
    expect_history("nxdn type-c with category", state, DSD_CALL_CHANNEL_NONE, 0U,
                   "NXDN" DOT "SYS 12" DOT "SITE 201" DOT "Regional");

    dsd_rxid_reset(state);
    dsd_rxid_note_nxdn_idas_area(state, 1U);
    dsd_rxid_note_nxdn_idas_site(state, 3U, 17U);
    expect_history("idas with area", state, DSD_CALL_CHANNEL_NONE, 0U, "IDAS" DOT "Area 1" DOT "SITE 17" DOT "Narrow");

    dsd_rxid_reset(state);
    dsd_rxid_note_tetra_cell(state, 262U, 1001U, 25U);
    dsd_rxid_note_tetra_la(state, 4242U);
    expect_history("tetra with la", state, DSD_CALL_CHANNEL_NONE, 0U,
                   "TETRA" DOT "MCC 262" DOT "MNC 1001" DOT "LA 4242");

    /* Cut short, the line stays terminated; no buffer writes nothing. */
    dsd_received_identity site;
    dsd_rxid_site_snapshot(&state->rx_id, &site);
    char small[10];
    expect_u("history cut short", dsd_app_site_history_text(&site, DSD_CALL_CHANNEL_NONE, 0U, small, sizeof(small)),
             sizeof(small) - 1U);
    expect_u("history cut short terminated", small[sizeof(small) - 1U] == '\0', 1U);
    expect_u("history null buffer", dsd_app_site_history_text(&site, DSD_CALL_CHANNEL_NONE, 0U, NULL, 8U), 0U);
    char none[8];
    expect_u("history null identity, no channel",
             dsd_app_site_history_text(NULL, DSD_CALL_CHANNEL_NONE, 0U, none, sizeof(none)), 0U);
    expect_s("history null identity text", none, "");
    free(state);
}

int
main(void) {
    test_dmr();
    test_dmr_site_parts();
    test_dmr_ras();
    test_p25();
    test_others();
    test_record_form_matches_live_without_esk();
    test_record_form_edacs_mode_from_record();
    test_edacs_sys_hex();
    test_channel_tokens();
    test_history_text_examples();
    dsd_app_site_identity_view(NULL, NULL);
    expect_text("null state", NULL, DSD_APP_SITE_IDENTITY_FULL, 1, "");
    if (g_fail) {
        return 1;
    }
    printf("APP_CONTROL_SITE_IDENTITY_VIEW: OK\n");
    return 0;
}
