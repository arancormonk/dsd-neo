// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Site identities a call row keeps: the snapshot that keeps only site parts, the part-by-part compare and union, the
 * one-record consistency check, the NXDN location ranges, and the row-level check with the call's channel kind. The
 * module is built into this test on its own (no decoder state), as the Qt tests build it.
 */

#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pinned: history rows, the control API and the Qt store keep these values. */
_Static_assert(DSD_CALL_CHANNEL_NONE == 0, "channel kind NONE is pinned");
_Static_assert(DSD_CALL_CHANNEL_LSN == 1, "channel kind LSN is pinned");
_Static_assert(DSD_CALL_CHANNEL_LPCN == 2, "channel kind LPCN is pinned");
_Static_assert(DSD_CALL_CHANNEL_LCN == 3, "channel kind LCN is pinned");

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

static void
expect_same_bytes(const char* label, const dsd_received_identity* got, const dsd_received_identity* want) {
    if (memcmp(got, want, sizeof(*got)) != 0) {
        DSD_FPRINTF(stderr, "FAIL %s: records differ\n", label);
        g_fail = 1;
    }
}

static void
clear_id(dsd_received_identity* id) {
    DSD_MEMSET(id, 0, sizeof(*id));
}

static void
set_text(char* dst, size_t capacity, const char* text) {
    DSD_MEMSET(dst, 0, capacity);
    DSD_SNPRINTF(dst, capacity, "%s", text);
}

static void
p25_id(dsd_received_identity* id, uint32_t wacn, uint16_t sysid, uint8_t rfss, uint8_t site) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_P25;
    id->p25_sysid = sysid;
    id->valid = DSD_RXID_P25_SYSID;
    if (wacn != 0U) {
        id->p25_wacn = wacn;
        id->valid |= DSD_RXID_P25_WACN;
    }
    if (rfss != 0U || site != 0U) {
        id->p25_rfss = rfss;
        id->p25_site = site;
        id->valid |= DSD_RXID_P25_SITE;
    }
}

static void
dmr_id(dsd_received_identity* id, uint8_t trunk, int has_net, uint16_t net, uint16_t site, uint8_t model,
       const char* text) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_DMR;
    id->dmr_trunk = trunk;
    id->dmr_site = site;
    id->dmr_t3_model = model;
    id->valid = DSD_RXID_DMR_SITE;
    if (has_net) {
        id->dmr_net = net;
        id->valid |= DSD_RXID_DMR_NET;
    }
    if (text != NULL) {
        set_text(id->dmr_site_text, sizeof(id->dmr_site_text), text);
        id->valid |= DSD_RXID_DMR_SITE_TEXT;
    }
}

static void
nxdn_location_id(dsd_received_identity* id, uint8_t category, uint32_t sys, uint16_t site) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_NXDN;
    id->nxdn_category = category;
    id->nxdn_sys = sys;
    id->nxdn_site = site;
    id->valid = DSD_RXID_NXDN_LOCATION;
}

static void
nxdn_idas_site_id(dsd_received_identity* id, uint8_t area, uint8_t site_type, uint8_t site_code) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_NXDN;
    id->nxdn_idas_area = area;
    id->nxdn_idas_site_type = site_type;
    id->nxdn_idas_site_code = site_code;
    id->valid = DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE;
}

static void
nxdn_typed_id(dsd_received_identity* id, uint32_t sys, uint8_t site) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_NXDN;
    id->nxdn_typed_sys = sys;
    id->nxdn_typed_site = site;
    id->valid = DSD_RXID_NXDN_TYPED_SYSTEM;
}

static void
edacs_id(dsd_received_identity* id, int8_t ea_mode, uint16_t site, uint16_t area, uint16_t system) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_EDACS;
    id->edacs_ea_mode = ea_mode;
    id->edacs_site = site;
    id->edacs_area = area;
    id->edacs_system = system;
    id->valid = DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM;
}

static void
tetra_id(dsd_received_identity* id, uint16_t mcc, uint16_t mnc, uint8_t colour, uint16_t la) {
    clear_id(id);
    id->family = (uint8_t)DSD_RXID_FAMILY_TETRA;
    id->tetra_mcc = mcc;
    id->tetra_mnc = mnc;
    id->tetra_colour = colour;
    id->tetra_la = la;
    id->valid = DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA;
}

static int
compatible_both_orders(const char* label, const dsd_received_identity* a, const dsd_received_identity* b) {
    const int ab = dsd_rxid_site_compatible(a, b);
    const int ba = dsd_rxid_site_compatible(b, a);
    if (ab != ba) {
        DSD_FPRINTF(stderr, "FAIL %s: compare is not symmetric (%d vs %d)\n", label, ab, ba);
        g_fail = 1;
    }
    return ab;
}

/* The parts that change with the moment or the carrier (access code, rest and free channels, RAS, vendor, control
   channel and its LCN) never reach a row; the epoch and fresh bits neither. */
static void
test_snapshot_drops_moment_parts(void) {
    dsd_received_identity in;
    dsd_received_identity out;
    dmr_id(&in, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+ \xC2\xB7 Site 2");
    in.access_kind = 1U;
    in.access = 7U;
    in.dmr_rest_lsn = 3U;
    in.dmr_xpt_free_lcn = 4U;
    in.dmr_vendor = 0x10U;
    in.cc_freq_hz = 851012500U;
    set_text(in.dmr_brand, sizeof(in.dmr_brand), "Capacity Plus");
    in.valid |= DSD_RXID_ACCESS | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_XPT_FREE_LCN | DSD_RXID_DMR_VENDOR
                | DSD_RXID_DMR_RAS | DSD_RXID_CC_FREQ | DSD_RXID_DMR_BRAND;
    in.fresh = in.valid;
    in.epoch = 41U;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("dmr snapshot valid", out.valid, DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND);
    expect_u("dmr snapshot fresh", out.fresh, 0U);
    expect_u("dmr snapshot epoch", out.epoch, 0U);
    expect_u("dmr snapshot access", out.access, 0U);
    expect_u("dmr snapshot access kind", out.access_kind, 0U);
    expect_u("dmr snapshot rest lsn", out.dmr_rest_lsn, 0U);
    expect_u("dmr snapshot free lcn", out.dmr_xpt_free_lcn, 0U);
    expect_u("dmr snapshot vendor", out.dmr_vendor, 0U);
    expect_u("dmr snapshot cc freq", out.cc_freq_hz, 0U);
    expect_u("dmr snapshot trunk", out.dmr_trunk, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS);
    expect_u("dmr snapshot site", out.dmr_site, 2U);
    expect_s("dmr snapshot text", out.dmr_site_text, "Cap+ \xC2\xB7 Site 2");
    expect_s("dmr snapshot brand", out.dmr_brand, "Capacity Plus");
    expect_u("dmr snapshot family", out.family, DSD_RXID_FAMILY_DMR);

    /* The trunk subtype goes with the last part it qualifies: an XPT record with only its free channel keeps nothing. */
    clear_id(&in);
    in.family = (uint8_t)DSD_RXID_FAMILY_DMR;
    in.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_XPT;
    in.dmr_xpt_free_lcn = 5U;
    in.valid = DSD_RXID_DMR_XPT_FREE_LCN | DSD_RXID_ACCESS;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("xpt free-only valid", out.valid, 0U);
    expect_u("xpt free-only trunk", out.dmr_trunk, 0U);
    expect_u("xpt free-only family", out.family, DSD_RXID_FAMILY_NONE);
    expect_u("xpt free-only empty", (unsigned long)dsd_rxid_site_empty(&out), 1U);

    /* EDACS: the control channel LCN goes, the mode stays with the site; the mode alone goes when no part is left. */
    edacs_id(&in, 1, 5U, 2U, 0x123U);
    in.edacs_cc_lcn = 9U;
    in.valid |= DSD_RXID_EDACS_CC_LCN;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("edacs snapshot valid", out.valid, DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM);
    expect_u("edacs snapshot lcn", out.edacs_cc_lcn, 0U);
    expect_u("edacs snapshot mode", (unsigned long)(uint8_t)out.edacs_ea_mode, 1U);
    in.valid = DSD_RXID_EDACS_CC_LCN;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("edacs lcn-only mode", (unsigned long)(uint8_t)out.edacs_ea_mode, 0U);
    expect_u("edacs lcn-only family", out.family, DSD_RXID_FAMILY_NONE);

    /* TETRA: the colour stays with the network; the IDAS area stays with an IDAS site. */
    tetra_id(&in, 901U, 9999U, 5U, 77U);
    in.access_kind = 1U;
    in.access = 5U;
    in.valid |= DSD_RXID_ACCESS;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("tetra snapshot valid", out.valid, DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA);
    expect_u("tetra snapshot colour", out.tetra_colour, 5U);
    expect_u("tetra snapshot access", out.access, 0U);
    nxdn_idas_site_id(&in, 1U, 2U, 40U);
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("idas snapshot valid", out.valid, DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE);
    expect_u("idas snapshot area", out.nxdn_idas_area, 1U);

    /* A part of another family than the record's is not this record's. */
    p25_id(&in, 0xBEE00U, 0x123U, 1U, 2U);
    in.valid |= DSD_RXID_EDACS_SITE;
    in.edacs_site = 4U;
    dsd_rxid_site_snapshot(&in, &out);
    expect_u("cross-family part dropped", out.valid, DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE);
    expect_u("cross-family value dropped", out.edacs_site, 0U);

    /* NULL in leaves an empty record; in place works. */
    DSD_MEMSET(&out, 0xA5, sizeof(out));
    dsd_rxid_site_snapshot(NULL, &out);
    dsd_received_identity zero;
    clear_id(&zero);
    expect_same_bytes("null snapshot is empty", &out, &zero);
    p25_id(&in, 0xBEE00U, 0x123U, 1U, 2U);
    in.cc_freq_hz = 1U;
    in.valid |= DSD_RXID_CC_FREQ;
    dsd_received_identity copy;
    dsd_rxid_site_snapshot(&in, &copy);
    dsd_rxid_site_snapshot(&in, &in);
    expect_same_bytes("in-place snapshot", &in, &copy);
}

/* A snapshot is bytes a row compares: garbage beside the valid parts, in invalid fields or past a text's NUL, never
   reaches it. */
static void
test_snapshot_zero_bytes(void) {
    dsd_received_identity clean;
    dsd_received_identity dirty;
    dsd_received_identity a;
    dsd_received_identity b;
    dmr_id(&clean, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small \xC2\xB7 Net 12");
    clean.dmr_t3_syscode = 0x0A3FU;
    clean.valid |= DSD_RXID_DMR_T3_SYSCODE;

    DSD_MEMSET(&dirty, 0x5A, sizeof(dirty));
    dirty.family = clean.family;
    dirty.valid = clean.valid | DSD_RXID_ACCESS | DSD_RXID_CC_FREQ;
    dirty.dmr_trunk = clean.dmr_trunk;
    dirty.dmr_net = clean.dmr_net;
    dirty.dmr_site = clean.dmr_site;
    dirty.dmr_t3_model = clean.dmr_t3_model;
    dirty.dmr_t3_syscode = clean.dmr_t3_syscode;
    /* The same text, garbage after its NUL. */
    DSD_MEMCPY(dirty.dmr_site_text, clean.dmr_site_text, strlen(clean.dmr_site_text) + 1U);
    dsd_rxid_site_snapshot(&clean, &a);
    dsd_rxid_site_snapshot(&dirty, &b);
    expect_same_bytes("garbage-filled snapshot equals clean", &b, &a);

    /* A text without a NUL in its field is cut to the field, NUL-terminated. */
    DSD_MEMSET(dirty.dmr_site_text, 'x', sizeof(dirty.dmr_site_text));
    dsd_rxid_site_snapshot(&dirty, &b);
    expect_u("unterminated text cut", (unsigned long)strlen(b.dmr_site_text), sizeof(b.dmr_site_text) - 1U);
}

static void
test_p25_wacn_sys_conflict(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    p25_id(&a, 0xBEE00U, 0x123U, 1U, 2U);
    p25_id(&b, 0xBEE01U, 0x123U, 1U, 2U);
    expect_u("p25 wacn conflict", (unsigned long)compatible_both_orders("p25 wacn", &a, &b), 0U);
    p25_id(&b, 0xBEE00U, 0x124U, 1U, 2U);
    expect_u("p25 sysid conflict", (unsigned long)compatible_both_orders("p25 sysid", &a, &b), 0U);
    p25_id(&b, 0xBEE00U, 0x123U, 1U, 3U);
    expect_u("p25 site conflict", (unsigned long)compatible_both_orders("p25 site", &a, &b), 0U);
    p25_id(&b, 0xBEE00U, 0x123U, 2U, 2U);
    expect_u("p25 rfss conflict", (unsigned long)compatible_both_orders("p25 rfss", &a, &b), 0U);
    p25_id(&b, 0xBEE00U, 0x123U, 1U, 2U);
    a.p25_lra = 1U;
    b.p25_lra = 2U;
    a.valid |= DSD_RXID_P25_LRA;
    b.valid |= DSD_RXID_P25_LRA;
    expect_u("p25 lra conflict", (unsigned long)compatible_both_orders("p25 lra", &a, &b), 0U);
    b.p25_lra = 1U;
    expect_u("p25 same site", (unsigned long)compatible_both_orders("p25 same", &a, &b), 1U);

    /* Families both known and different never match; an empty side always does. */
    edacs_id(&b, 0, 1U, 1U, 1U);
    expect_u("family conflict", (unsigned long)compatible_both_orders("family", &a, &b), 0U);
    clear_id(&b);
    expect_u("empty side", (unsigned long)compatible_both_orders("empty", &a, &b), 1U);
    expect_u("null side", (unsigned long)dsd_rxid_site_compatible(&a, NULL), 1U);
    expect_u("null side reversed", (unsigned long)dsd_rxid_site_compatible(NULL, &a), 1U);
    /* Moment parts are never compared. */
    p25_id(&b, 0xBEE00U, 0x123U, 1U, 2U);
    a.access = 0x293U;
    b.access = 0x294U;
    a.valid |= DSD_RXID_ACCESS;
    b.valid |= DSD_RXID_ACCESS;
    expect_u("access not compared", (unsigned long)compatible_both_orders("access", &a, &b), 1U);
}

static void
test_p25_site_only_gains_wacn(void) {
    dsd_received_identity site_only;
    dsd_received_identity full;
    dsd_received_identity u;
    p25_id(&site_only, 0U, 0x123U, 1U, 2U);
    p25_id(&full, 0xBEE00U, 0x123U, 0U, 0U);
    expect_u("site-only vs wacn", (unsigned long)compatible_both_orders("p25 gains", &site_only, &full), 1U);
    dsd_rxid_site_union(&site_only, &full, &u);
    expect_u("union valid", u.valid, DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE);
    expect_u("union wacn", u.p25_wacn, 0xBEE00U);
    expect_u("union sysid", u.p25_sysid, 0x123U);
    expect_u("union rfss", u.p25_rfss, 1U);
    expect_u("union site", u.p25_site, 2U);
    expect_u("union family", u.family, DSD_RXID_FAMILY_P25);
}

/* DMR sites are told apart by raw parts (trunk, net, site, model, system code); the text follows -D and is not. */
static void
test_dmr_raw_compare_ignores_text(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    dsd_received_identity u;
    dmr_id(&a, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 301");
    dmr_id(&b, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 38.5");
    set_text(b.dmr_brand, sizeof(b.dmr_brand), "Other");
    set_text(a.dmr_brand, sizeof(a.dmr_brand), "Brand");
    a.valid |= DSD_RXID_DMR_BRAND;
    b.valid |= DSD_RXID_DMR_BRAND;
    expect_u("text differs, raw same", (unsigned long)compatible_both_orders("dmr text", &a, &b), 1U);
    dsd_rxid_site_union(&a, &b, &u);
    /* Equal raw parts, two texts: the strcmp-smaller one, whichever side it came from. */
    expect_s("union text smaller", u.dmr_site_text, "TIII Small \xC2\xB7 Net 12 \xC2\xB7 Site 301");
    expect_s("union brand smaller", u.dmr_brand, "Brand");

    b.dmr_site = 301U;
    expect_u("site differs", (unsigned long)compatible_both_orders("dmr site", &a, &b), 0U);
    b.dmr_site = 300U;
    b.dmr_net = 12U;
    expect_u("net differs", (unsigned long)compatible_both_orders("dmr net", &a, &b), 0U);
    b.dmr_net = 11U;
    b.dmr_t3_model = 2U;
    expect_u("model differs", (unsigned long)compatible_both_orders("dmr model", &a, &b), 0U);
    b.dmr_t3_model = 1U;
    a.dmr_t3_syscode = 0x0A3FU;
    b.dmr_t3_syscode = 0x0A40U;
    a.valid |= DSD_RXID_DMR_T3_SYSCODE;
    b.valid |= DSD_RXID_DMR_T3_SYSCODE;
    expect_u("syscode differs", (unsigned long)compatible_both_orders("dmr syscode", &a, &b), 0U);
    b.dmr_t3_syscode = 0x0A3FU;
    expect_u("same site", (unsigned long)compatible_both_orders("dmr same", &a, &b), 1U);
}

static void
test_dmr_capplus_vs_conplus_conflict(void) {
    dsd_received_identity cap;
    dsd_received_identity con;
    dsd_received_identity text_only;
    dmr_id(&cap, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, NULL);
    dmr_id(&con, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 2U, 0U, NULL);
    expect_u("cap+ vs con+", (unsigned long)compatible_both_orders("cap con", &cap, &con), 0U);
    /* A brand-only side names its trunk too. */
    clear_id(&con);
    con.family = (uint8_t)DSD_RXID_FAMILY_DMR;
    con.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS;
    set_text(con.dmr_brand, sizeof(con.dmr_brand), "Connect Plus");
    con.valid = DSD_RXID_DMR_BRAND;
    expect_u("cap+ vs con+ brand", (unsigned long)compatible_both_orders("cap con brand", &cap, &con), 0U);
    /* A part of no known trunk (an unknown text prefix) names no trunk to conflict with. */
    clear_id(&text_only);
    text_only.family = (uint8_t)DSD_RXID_FAMILY_DMR;
    set_text(text_only.dmr_site_text, sizeof(text_only.dmr_site_text), "Unknown site");
    text_only.valid = DSD_RXID_DMR_SITE_TEXT;
    expect_u("unknown trunk vs cap+", (unsigned long)compatible_both_orders("unknown cap", &text_only, &cap), 1U);
    dsd_received_identity u;
    dsd_rxid_site_union(&text_only, &cap, &u);
    expect_u("union takes known trunk", u.dmr_trunk, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS);
}

/* The record's own rule (received_identity.c): an IDAS SCCH site code against a Type-D system ID's site code. */
static void
test_nxdn_idas_site_vs_typed_site_both_orders(void) {
    dsd_received_identity idas;
    dsd_received_identity typed;
    nxdn_idas_site_id(&idas, 0U, 2U, 40U);
    nxdn_typed_id(&typed, 0x12345U, 41U);
    expect_u("idas site vs typed site", (unsigned long)compatible_both_orders("idas typed", &idas, &typed), 0U);
    typed.nxdn_typed_site = 40U;
    expect_u("idas site matches typed", (unsigned long)compatible_both_orders("idas typed ok", &idas, &typed), 1U);
    dsd_received_identity u;
    dsd_rxid_site_union(&idas, &typed, &u);
    expect_u("idas union valid", u.valid,
             DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM);
    expect_u("idas union consistent", (unsigned long)dsd_rxid_site_consistent(&u), 1U);
}

/* Type-C (a location ID) and IDAS Type-D are two kinds of system. */
static void
test_nxdn_typec_vs_idas_conflict(void) {
    dsd_received_identity typec;
    dsd_received_identity idas;
    nxdn_location_id(&typec, 0U, 100U, 20U);
    nxdn_idas_site_id(&idas, 0U, 1U, 20U);
    expect_u("type-c vs idas site", (unsigned long)compatible_both_orders("typec idas", &typec, &idas), 0U);
    idas.valid = DSD_RXID_NXDN_IDAS_AREA;
    expect_u("type-c vs idas area", (unsigned long)compatible_both_orders("typec area", &typec, &idas), 0U);
    nxdn_typed_id(&idas, 100U, 20U);
    expect_u("type-c vs typed sys", (unsigned long)compatible_both_orders("typec typed", &typec, &idas), 0U);
    dsd_received_identity other;
    nxdn_location_id(&other, 0U, 100U, 21U);
    expect_u("location differs", (unsigned long)compatible_both_orders("location", &typec, &other), 0U);
    nxdn_location_id(&other, 1U, 100U, 20U);
    expect_u("category differs", (unsigned long)compatible_both_orders("category", &typec, &other), 0U);
    nxdn_location_id(&other, 0U, 100U, 20U);
    expect_u("same location", (unsigned long)compatible_both_orders("location same", &typec, &other), 1U);
}

/* The IDAS area tells adjacent co-channel sites apart (NXDN TS 1-E 6.6.3). */
static void
test_nxdn_idas_area_conflict(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    nxdn_idas_site_id(&a, 0U, 2U, 40U);
    nxdn_idas_site_id(&b, 1U, 2U, 40U);
    expect_u("idas area differs", (unsigned long)compatible_both_orders("idas area", &a, &b), 0U);
    nxdn_idas_site_id(&b, 0U, 3U, 40U);
    expect_u("idas site type differs", (unsigned long)compatible_both_orders("idas type", &a, &b), 0U);
    nxdn_typed_id(&a, 0x12345U, 40U);
    nxdn_typed_id(&b, 0x12346U, 40U);
    expect_u("typed sys differs", (unsigned long)compatible_both_orders("typed sys", &a, &b), 0U);
}

static void
test_edacs_mode_conflict(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    edacs_id(&a, 0, 5U, 2U, 0x123U);
    edacs_id(&b, 1, 5U, 2U, 0x123U);
    expect_u("edacs mode differs", (unsigned long)compatible_both_orders("edacs mode", &a, &b), 0U);
    edacs_id(&b, 0, 6U, 2U, 0x123U);
    expect_u("edacs site differs", (unsigned long)compatible_both_orders("edacs site", &a, &b), 0U);
    edacs_id(&b, 0, 5U, 3U, 0x123U);
    expect_u("edacs area differs", (unsigned long)compatible_both_orders("edacs area", &a, &b), 0U);
    edacs_id(&b, 0, 5U, 2U, 0x124U);
    expect_u("edacs system differs", (unsigned long)compatible_both_orders("edacs system", &a, &b), 0U);
    edacs_id(&b, 0, 5U, 2U, 0x123U);
    expect_u("edacs same", (unsigned long)compatible_both_orders("edacs same", &a, &b), 1U);
}

static void
test_tetra_la_conflict_same_net(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    tetra_id(&a, 901U, 9999U, 5U, 77U);
    tetra_id(&b, 901U, 9999U, 5U, 78U);
    expect_u("tetra la differs", (unsigned long)compatible_both_orders("tetra la", &a, &b), 0U);
    tetra_id(&b, 901U, 9999U, 6U, 77U);
    expect_u("tetra colour differs", (unsigned long)compatible_both_orders("tetra colour", &a, &b), 0U);
    tetra_id(&b, 902U, 9999U, 5U, 77U);
    expect_u("tetra mcc differs", (unsigned long)compatible_both_orders("tetra mcc", &a, &b), 0U);
    tetra_id(&b, 901U, 9998U, 5U, 77U);
    expect_u("tetra mnc differs", (unsigned long)compatible_both_orders("tetra mnc", &a, &b), 0U);
    tetra_id(&b, 901U, 9999U, 5U, 77U);
    b.valid = DSD_RXID_TETRA_NET;
    expect_u("tetra net only", (unsigned long)compatible_both_orders("tetra net only", &a, &b), 1U);
}

static void
expect_union_order_free(const char* label, const dsd_received_identity* a, const dsd_received_identity* b) {
    dsd_received_identity ab;
    dsd_received_identity ba;
    dsd_rxid_site_union(a, b, &ab);
    dsd_rxid_site_union(b, a, &ba);
    expect_same_bytes(label, &ab, &ba);
}

static void
test_union_order_free(void) {
    dsd_received_identity a;
    dsd_received_identity b;
    dmr_id(&a, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small \xC2\xB7 Site 301");
    dmr_id(&b, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII Small \xC2\xB7 Site 38.5");
    b.dmr_t3_syscode = 0x0A3FU;
    b.valid |= DSD_RXID_DMR_T3_SYSCODE;
    a.access = 3U;
    a.valid |= DSD_RXID_ACCESS;
    b.epoch = 9U;
    expect_union_order_free("dmr union order-free", &a, &b);
    dsd_received_identity u;
    dsd_rxid_site_union(&a, &b, &u);
    expect_u("dmr union valid", u.valid,
             DSD_RXID_DMR_SITE | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_T3_SYSCODE);
    expect_u("dmr union epoch", u.epoch, 0U);
    expect_u("dmr union access", u.access, 0U);

    p25_id(&a, 0U, 0x123U, 1U, 2U);
    p25_id(&b, 0xBEE00U, 0x123U, 0U, 0U);
    b.p25_lra = 4U;
    b.valid |= DSD_RXID_P25_LRA;
    expect_union_order_free("p25 union order-free", &a, &b);
    edacs_id(&a, 1, 5U, 2U, 0x123U);
    a.valid = DSD_RXID_EDACS_SITE;
    edacs_id(&b, 1, 5U, 2U, 0x123U);
    b.valid = DSD_RXID_EDACS_SYSTEM | DSD_RXID_EDACS_AREA;
    expect_union_order_free("edacs union order-free", &a, &b);
    tetra_id(&a, 901U, 9999U, 5U, 77U);
    a.valid = DSD_RXID_TETRA_NET;
    tetra_id(&b, 901U, 9999U, 5U, 77U);
    expect_union_order_free("tetra union order-free", &a, &b);
    clear_id(&b);
    expect_union_order_free("empty union order-free", &a, &b);
    dsd_received_identity snap;
    dsd_rxid_site_union(&a, &b, &u);
    dsd_rxid_site_snapshot(&a, &snap);
    expect_same_bytes("union with empty is the snapshot", &u, &snap);

    /* Outside the precondition (a conflict) the result is empty, never one side's pick. */
    p25_id(&a, 0xBEE00U, 0x123U, 1U, 2U);
    p25_id(&b, 0xBEE01U, 0x123U, 1U, 2U);
    expect_union_order_free("conflict union order-free", &a, &b);
    dsd_rxid_site_union(&a, &b, &u);
    expect_u("conflict union empty", (unsigned long)dsd_rxid_site_empty(&u), 1U);
}

/* NXDN TS 1-A 6.5.2 Table 6.5-1, in the record's category encoding (0 Global, 1 Regional, 2 Local). */
static void
test_nxdn_location_ranges(void) {
    static const struct {
        uint8_t category;
        uint32_t sys_max;
        uint16_t site_max;
    } k_ranges[] = {{0U, 1022U, 4094U}, {1U, 16382U, 254U}, {2U, 131070U, 30U}};

    for (size_t i = 0U; i < sizeof(k_ranges) / sizeof(k_ranges[0]); i++) {
        const uint8_t c = k_ranges[i].category;
        const uint32_t sys_max = k_ranges[i].sys_max;
        const uint16_t site_max = k_ranges[i].site_max;
        expect_u("location 1/1", (unsigned long)dsd_nxdn_location_valid(c, 1U, 1U), 1U);
        expect_u("location max/max", (unsigned long)dsd_nxdn_location_valid(c, sys_max, site_max), 1U);
        expect_u("location sys 0", (unsigned long)dsd_nxdn_location_valid(c, 0U, 1U), 0U);
        expect_u("location site 0", (unsigned long)dsd_nxdn_location_valid(c, 1U, 0U), 0U);
        expect_u("location sys ones", (unsigned long)dsd_nxdn_location_valid(c, sys_max + 1U, 1U), 0U);
        expect_u("location site ones", (unsigned long)dsd_nxdn_location_valid(c, 1U, (uint16_t)(site_max + 1U)), 0U);
    }
    expect_u("location reserved category", (unsigned long)dsd_nxdn_location_valid(3U, 1U, 1U), 0U);
    expect_u("location category 4", (unsigned long)dsd_nxdn_location_valid(4U, 1U, 1U), 0U);
}

static void
expect_inconsistent(const char* label, const dsd_received_identity* id) {
    expect_u(label, (unsigned long)dsd_rxid_site_consistent(id), 0U);
}

static void
test_consistent_rejects_ranges_and_cross_parts(void) {
    dsd_received_identity id;
    expect_u("null consistent", (unsigned long)dsd_rxid_site_consistent(NULL), 0U);
    clear_id(&id);
    expect_u("empty consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    id.family = 9U;
    expect_inconsistent("family > 7", &id);
    id.family = (uint8_t)DSD_RXID_FAMILY_NONE;
    id.valid = DSD_RXID_P25_SYSID;
    expect_inconsistent("parts with no family", &id);

    p25_id(&id, 0xBEE00U, 0x123U, 1U, 2U);
    expect_u("p25 consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    id.valid |= DSD_RXID_ACCESS;
    expect_inconsistent("moment part", &id);
    id.valid &= ~DSD_RXID_ACCESS;
    id.valid |= DSD_RXID_EDACS_SITE;
    expect_inconsistent("other family's part", &id);
    id.valid &= ~DSD_RXID_EDACS_SITE;
    id.p25_wacn = 0x100000U;
    expect_inconsistent("wacn > 20 bits", &id);
    p25_id(&id, 0xBEE00U, 0x1000U, 1U, 2U);
    expect_inconsistent("sysid > 12 bits", &id);
    p25_id(&id, 0xBEE00U, 0U, 1U, 2U);
    expect_inconsistent("sysid 0", &id);
    p25_id(&id, 0xBEE00U, 0x123U, 1U, 2U);
    id.valid &= ~DSD_RXID_P25_SYSID;
    expect_inconsistent("p25 site without sysid", &id);

    nxdn_location_id(&id, 0U, 1022U, 4094U);
    expect_u("nxdn location consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    nxdn_location_id(&id, 0U, 1023U, 1U);
    expect_inconsistent("nxdn global sys all-ones", &id);
    nxdn_location_id(&id, 2U, 131070U, 31U);
    expect_inconsistent("nxdn local site all-ones", &id);
    nxdn_location_id(&id, 3U, 1U, 1U);
    expect_inconsistent("nxdn reserved category", &id);
    nxdn_location_id(&id, 0U, 100U, 20U);
    id.nxdn_idas_area = 1U;
    id.valid |= DSD_RXID_NXDN_IDAS_AREA;
    expect_inconsistent("type-c with idas", &id);
    nxdn_idas_site_id(&id, 0U, 0U, 40U);
    expect_inconsistent("idas site type 0", &id);
    nxdn_idas_site_id(&id, 0U, 4U, 40U);
    expect_inconsistent("idas site type 4", &id);
    nxdn_idas_site_id(&id, 0U, 1U, 0U);
    expect_inconsistent("idas site code 0", &id);
    nxdn_idas_site_id(&id, 0U, 1U, 251U);
    expect_inconsistent("idas site code 251", &id);
    nxdn_idas_site_id(&id, 0U, 1U, 250U);
    id.nxdn_typed_sys = 0x12345U;
    id.nxdn_typed_site = 249U;
    id.valid |= DSD_RXID_NXDN_TYPED_SYSTEM;
    expect_inconsistent("idas site vs typed site", &id);
    id.nxdn_typed_site = 250U;
    expect_u("idas with typed consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);

    tetra_id(&id, 1023U, 16383U, 63U, 16383U);
    expect_u("tetra max consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    tetra_id(&id, 1024U, 1U, 1U, 1U);
    expect_inconsistent("tetra mcc", &id);
    tetra_id(&id, 1U, 16384U, 1U, 1U);
    expect_inconsistent("tetra mnc", &id);
    tetra_id(&id, 1U, 1U, 1U, 16384U);
    expect_inconsistent("tetra la", &id);
    tetra_id(&id, 1U, 1U, 1U, 1U);
    id.valid = DSD_RXID_TETRA_LA;
    expect_inconsistent("tetra la without net", &id);

    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 3U, "TIII Huge");
    expect_u("dmr consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    id.dmr_t3_model = 4U;
    expect_inconsistent("dmr model > 3", &id);
    /* A model only Tier III has: the record keeps 0 for another trunking system. */
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, "Cap+");
    expect_u("cap+ model 0 consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
    id.dmr_t3_model = 1U;
    expect_inconsistent("cap+ site with a tier iii model", &id);
    dmr_id(&id, 5U, 1, 11U, 300U, 0U, NULL);
    expect_inconsistent("dmr trunk unknown", &id);
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_NONE, 1, 11U, 300U, 0U, NULL);
    expect_inconsistent("dmr numbers without trunk", &id);
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 0U, NULL);
    id.valid = DSD_RXID_DMR_NET;
    expect_inconsistent("dmr net without site", &id);
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, NULL);
    id.dmr_t3_syscode = 1U;
    id.valid |= DSD_RXID_DMR_T3_SYSCODE;
    expect_inconsistent("syscode on cap+", &id);
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII");
    DSD_MEMSET(id.dmr_site_text, 'x', sizeof(id.dmr_site_text));
    expect_inconsistent("unterminated site text", &id);
    dmr_id(&id, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, "TIII");
    DSD_MEMSET(id.dmr_brand, 'x', sizeof(id.dmr_brand));
    expect_inconsistent("unterminated brand", &id);

    edacs_id(&id, 0, 1U, 2U, 3U);
    expect_u("edacs consistent", (unsigned long)dsd_rxid_site_consistent(&id), 1U);
}

static void
test_call_channel_kind_vs_trunk(void) {
    dsd_received_identity con;
    dsd_received_identity cap;
    dsd_received_identity t3;
    dsd_received_identity empty;
    dsd_received_identity p25;
    dmr_id(&con, (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS, 1, 3U, 5U, 0U, NULL);
    dmr_id(&cap, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 2U, 0U, NULL);
    dmr_id(&t3, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, 11U, 300U, 1U, NULL);
    clear_id(&empty);
    p25_id(&p25, 0xBEE00U, 0x123U, 1U, 2U);
    const uint8_t none = (uint8_t)DSD_CALL_CHANNEL_NONE;
    const uint8_t lsn = (uint8_t)DSD_CALL_CHANNEL_LSN;
    const uint8_t lpcn = (uint8_t)DSD_CALL_CHANNEL_LPCN;
    const uint8_t lcn = (uint8_t)DSD_CALL_CHANNEL_LCN;

    /* A channel-only Tier III fragment never joins a Connect Plus row, either way round. */
    expect_u("lpcn vs con+", (unsigned long)dsd_call_site_compatible(&empty, lpcn, &con, none), 0U);
    expect_u("con+ vs lpcn", (unsigned long)dsd_call_site_compatible(&con, none, &empty, lpcn), 0U);
    expect_u("lcn vs con+", (unsigned long)dsd_call_site_compatible(&empty, lcn, &con, none), 1U);
    /* Its own side's trunk too. */
    expect_u("lpcn on con+", (unsigned long)dsd_call_site_compatible(&con, lpcn, &empty, none), 0U);
    expect_u("lcn on con+", (unsigned long)dsd_call_site_compatible(&con, lcn, &con, lcn), 1U);
    expect_u("lsn vs cap+", (unsigned long)dsd_call_site_compatible(&empty, lsn, &cap, lsn), 1U);
    expect_u("lpcn vs t3", (unsigned long)dsd_call_site_compatible(&t3, lpcn, &empty, lpcn), 1U);
    expect_u("lsn vs t3", (unsigned long)dsd_call_site_compatible(&t3, none, &empty, lsn), 0U);
    expect_u("lsn vs lcn", (unsigned long)dsd_call_site_compatible(&empty, lsn, &empty, lcn), 0U);
    expect_u("channel-only vs empty", (unsigned long)dsd_call_site_compatible(&empty, lsn, &empty, none), 1U);
    expect_u("both empty", (unsigned long)dsd_call_site_compatible(&empty, none, &empty, none), 1U);
    expect_u("null sites", (unsigned long)dsd_call_site_compatible(NULL, lsn, NULL, lsn), 1U);
    /* XPT names its channels by LSN. */
    dsd_received_identity xpt;
    clear_id(&xpt);
    xpt.family = (uint8_t)DSD_RXID_FAMILY_DMR;
    xpt.dmr_trunk = (uint8_t)DSD_RXID_DMR_TRUNK_XPT;
    set_text(xpt.dmr_brand, sizeof(xpt.dmr_brand), "Hytera XPT");
    xpt.valid = DSD_RXID_DMR_BRAND;
    expect_u("lsn vs xpt", (unsigned long)dsd_call_site_compatible(&xpt, lsn, &empty, lsn), 1U);
    expect_u("lpcn vs xpt", (unsigned long)dsd_call_site_compatible(&xpt, none, &empty, lpcn), 0U);
    /* Conflicting sites stay apart whatever the kinds. */
    expect_u("site conflict", (unsigned long)dsd_call_site_compatible(&cap, none, &con, none), 0U);
    /* A channel kind belongs to DMR. */
    expect_u("kind on p25", (unsigned long)dsd_call_site_compatible(&p25, lsn, &empty, none), 0U);
    expect_u("kind vs p25", (unsigned long)dsd_call_site_compatible(&empty, lcn, &p25, none), 0U);
    expect_u("no kind on p25", (unsigned long)dsd_call_site_compatible(&p25, none, &p25, none), 1U);
    /* A kind value outside the enum is no channel kind anyone sent. */
    expect_u("unknown kind", (unsigned long)dsd_call_site_compatible(&empty, 4U, &empty, none), 0U);

    expect_s("label lsn", dsd_call_channel_kind_label(lsn), "LSN");
    expect_s("label lpcn", dsd_call_channel_kind_label(lpcn), "LPCN");
    expect_s("label lcn", dsd_call_channel_kind_label(lcn), "LCN");
    expect_s("label none", dsd_call_channel_kind_label(none), "");
    expect_s("label unknown", dsd_call_channel_kind_label(9U), "");
}

int
main(void) {
    test_snapshot_drops_moment_parts();
    test_snapshot_zero_bytes();
    test_p25_wacn_sys_conflict();
    test_p25_site_only_gains_wacn();
    test_dmr_raw_compare_ignores_text();
    test_dmr_capplus_vs_conplus_conflict();
    test_nxdn_idas_site_vs_typed_site_both_orders();
    test_nxdn_typec_vs_idas_conflict();
    test_nxdn_idas_area_conflict();
    test_edacs_mode_conflict();
    test_tetra_la_conflict_same_net();
    test_union_order_free();
    test_nxdn_location_ranges();
    test_consistent_rejects_ranges_and_cross_parts();
    test_call_channel_kind_vs_trunk();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stdout, "core received identity site: OK\n");
    return 0;
}
