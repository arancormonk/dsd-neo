// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The site a call was heard on, compared and merged part by part (see received_identity_site.h).
 *
 * libc only: no decoder state, so the Qt history store builds this file as it is.
 */

#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>

#include <stdint.h>
#include <string.h>

/* The site parts of each family: everything a carrier's system and site send, without the moment's parts (access
   code, rest and free channels, RAS, vendor, control channel and its EDACS LCN). */
#define SITE_P25 (DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE | DSD_RXID_P25_LRA)
/* Every DMR site part belongs to a trunking system (dmr_trunk). */
#define SITE_DMR                                                                                                       \
    (DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE)
#define SITE_NXDN_TYPE_D (DSD_RXID_NXDN_IDAS_AREA | DSD_RXID_NXDN_IDAS_SITE | DSD_RXID_NXDN_TYPED_SYSTEM)
#define SITE_NXDN        (DSD_RXID_NXDN_LOCATION | SITE_NXDN_TYPE_D)
#define SITE_EDACS       (DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_AREA | DSD_RXID_EDACS_SYSTEM)
#define SITE_TETRA       (DSD_RXID_TETRA_NET | DSD_RXID_TETRA_LA)

static uint32_t
site_parts(uint8_t family) {
    switch (family) {
        case DSD_RXID_FAMILY_DMR: return SITE_DMR;
        case DSD_RXID_FAMILY_P25: return SITE_P25;
        case DSD_RXID_FAMILY_NXDN: return SITE_NXDN;
        case DSD_RXID_FAMILY_TETRA: return SITE_TETRA;
        case DSD_RXID_FAMILY_EDACS: return SITE_EDACS;
        default: return 0U;
    }
}

static int
site_has(const dsd_received_identity* id, uint32_t parts) {
    return (id->valid & parts) != 0U;
}

/* Whether @p part is valid on both sides with other values. */
static int
site_differs(const dsd_received_identity* a, const dsd_received_identity* b, uint32_t part, uint32_t va, uint32_t vb) {
    return site_has(a, part) && site_has(b, part) && va != vb;
}

/* Copies a text field up to its NUL (the whole field less one when it has none) into zeroed @p dst. */
static void
site_copy_text(char* dst, const char* src, size_t capacity) {
    const char* const nul = (const char*)memchr(src, '\0', capacity);
    const size_t len = nul != NULL ? (size_t)(nul - src) : capacity - 1U;
    DSD_MEMCPY(dst, src, len);
}

static void
site_copy_p25(dsd_received_identity* dst, const dsd_received_identity* src, uint32_t parts) {
    if ((parts & DSD_RXID_P25_WACN) != 0U) {
        dst->p25_wacn = src->p25_wacn;
    }
    if ((parts & DSD_RXID_P25_SYSID) != 0U) {
        dst->p25_sysid = src->p25_sysid;
    }
    if ((parts & DSD_RXID_P25_SITE) != 0U) {
        dst->p25_rfss = src->p25_rfss;
        dst->p25_site = src->p25_site;
    }
    if ((parts & DSD_RXID_P25_LRA) != 0U) {
        dst->p25_lra = src->p25_lra;
    }
}

static void
site_copy_dmr(dsd_received_identity* dst, const dsd_received_identity* src, uint32_t parts) {
    if ((parts & DSD_RXID_DMR_SITE_TEXT) != 0U) {
        site_copy_text(dst->dmr_site_text, src->dmr_site_text, sizeof(dst->dmr_site_text));
    }
    if ((parts & DSD_RXID_DMR_BRAND) != 0U) {
        site_copy_text(dst->dmr_brand, src->dmr_brand, sizeof(dst->dmr_brand));
    }
    if ((parts & DSD_RXID_DMR_T3_SYSCODE) != 0U) {
        dst->dmr_t3_syscode = src->dmr_t3_syscode;
    }
    if ((parts & DSD_RXID_DMR_NET) != 0U) {
        dst->dmr_net = src->dmr_net;
    }
    if ((parts & DSD_RXID_DMR_SITE) != 0U) {
        dst->dmr_site = src->dmr_site;
        dst->dmr_t3_model = src->dmr_t3_model;
    }
}

static void
site_copy_nxdn(dsd_received_identity* dst, const dsd_received_identity* src, uint32_t parts) {
    if ((parts & DSD_RXID_NXDN_LOCATION) != 0U) {
        dst->nxdn_category = src->nxdn_category;
        dst->nxdn_sys = src->nxdn_sys;
        dst->nxdn_site = src->nxdn_site;
    }
    if ((parts & DSD_RXID_NXDN_IDAS_AREA) != 0U) {
        dst->nxdn_idas_area = src->nxdn_idas_area;
    }
    if ((parts & DSD_RXID_NXDN_IDAS_SITE) != 0U) {
        dst->nxdn_idas_site_type = src->nxdn_idas_site_type;
        dst->nxdn_idas_site_code = src->nxdn_idas_site_code;
    }
    if ((parts & DSD_RXID_NXDN_TYPED_SYSTEM) != 0U) {
        dst->nxdn_typed_sys = src->nxdn_typed_sys;
        dst->nxdn_typed_site = src->nxdn_typed_site;
    }
}

static void
site_copy_edacs_tetra(dsd_received_identity* dst, const dsd_received_identity* src, uint32_t parts) {
    if ((parts & DSD_RXID_EDACS_SITE) != 0U) {
        dst->edacs_site = src->edacs_site;
    }
    if ((parts & DSD_RXID_EDACS_AREA) != 0U) {
        dst->edacs_area = src->edacs_area;
    }
    if ((parts & DSD_RXID_EDACS_SYSTEM) != 0U) {
        dst->edacs_system = src->edacs_system;
    }
    if ((parts & DSD_RXID_TETRA_NET) != 0U) {
        dst->tetra_mcc = src->tetra_mcc;
        dst->tetra_mnc = src->tetra_mnc;
        dst->tetra_colour = src->tetra_colour;
    }
    if ((parts & DSD_RXID_TETRA_LA) != 0U) {
        dst->tetra_la = src->tetra_la;
    }
}

/* Copies the values of @p parts (and the qualifiers bound to one part: the Tier III model, the TETRA colour). */
static void
site_copy_parts(dsd_received_identity* dst, const dsd_received_identity* src, uint32_t parts) {
    site_copy_p25(dst, src, parts);
    site_copy_dmr(dst, src, parts);
    site_copy_nxdn(dst, src, parts);
    site_copy_edacs_tetra(dst, src, parts);
}

void
dsd_rxid_site_snapshot(const dsd_received_identity* in, dsd_received_identity* out) {
    if (out == NULL) {
        return;
    }
    dsd_received_identity s;
    DSD_MEMSET(&s, 0, sizeof(s));
    if (in != NULL) {
        const uint32_t parts = in->valid & site_parts(in->family);
        site_copy_parts(&s, in, parts);
        s.valid = parts;
        if ((parts & SITE_DMR) != 0U) {
            s.dmr_trunk = in->dmr_trunk;
        }
        if ((parts & SITE_EDACS) != 0U) {
            s.edacs_ea_mode = in->edacs_ea_mode;
        }
        s.family = parts != 0U ? in->family : (uint8_t)DSD_RXID_FAMILY_NONE;
    }
    DSD_MEMCPY(out, &s, sizeof(s));
}

int
dsd_rxid_site_empty(const dsd_received_identity* a) {
    return a == NULL || (a->valid & site_parts(a->family)) == 0U;
}

static int
site_p25_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    return !(site_differs(a, b, DSD_RXID_P25_WACN, a->p25_wacn, b->p25_wacn)
             || site_differs(a, b, DSD_RXID_P25_SYSID, a->p25_sysid, b->p25_sysid)
             || site_differs(a, b, DSD_RXID_P25_SITE, ((uint32_t)a->p25_rfss << 8) | a->p25_site,
                             ((uint32_t)b->p25_rfss << 8) | b->p25_site)
             || site_differs(a, b, DSD_RXID_P25_LRA, a->p25_lra, b->p25_lra));
}

/* Raw parts only: the site text follows -D and the brand is presentation. A snapshot's trunk is known (nonzero) only
   beside a part it qualifies. */
static int
site_dmr_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    if (a->dmr_trunk != (uint8_t)DSD_RXID_DMR_TRUNK_NONE && b->dmr_trunk != (uint8_t)DSD_RXID_DMR_TRUNK_NONE
        && a->dmr_trunk != b->dmr_trunk) {
        return 0;
    }
    return !(site_differs(a, b, DSD_RXID_DMR_T3_SYSCODE, a->dmr_t3_syscode, b->dmr_t3_syscode)
             || site_differs(a, b, DSD_RXID_DMR_NET, a->dmr_net, b->dmr_net)
             || site_differs(a, b, DSD_RXID_DMR_SITE, ((uint32_t)a->dmr_site << 8) | a->dmr_t3_model,
                             ((uint32_t)b->dmr_site << 8) | b->dmr_t3_model));
}

/* The record's rule: an IDAS SCCH site ID of @p x names another site than a Type-D system ID of @p y. */
static int
site_idas_typed_conflict(const dsd_received_identity* x, const dsd_received_identity* y) {
    return site_has(x, DSD_RXID_NXDN_IDAS_SITE) && site_has(y, DSD_RXID_NXDN_TYPED_SYSTEM)
           && x->nxdn_idas_site_code != y->nxdn_typed_site;
}

/* A Type-C location of @p x beside any IDAS (Type-D) part of @p y: two kinds of system. */
static int
site_typec_idas_conflict(const dsd_received_identity* x, const dsd_received_identity* y) {
    return site_has(x, DSD_RXID_NXDN_LOCATION) && site_has(y, SITE_NXDN_TYPE_D);
}

static int
site_nxdn_cross_conflict(const dsd_received_identity* a, const dsd_received_identity* b) {
    return site_idas_typed_conflict(a, b) || site_idas_typed_conflict(b, a) || site_typec_idas_conflict(a, b)
           || site_typec_idas_conflict(b, a);
}

static int
site_nxdn_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    const uint32_t a_idas = ((uint32_t)a->nxdn_idas_site_type << 8) | a->nxdn_idas_site_code;
    const uint32_t b_idas = ((uint32_t)b->nxdn_idas_site_type << 8) | b->nxdn_idas_site_code;
    return !(site_differs(a, b, DSD_RXID_NXDN_LOCATION, a->nxdn_category, b->nxdn_category)
             || site_differs(a, b, DSD_RXID_NXDN_LOCATION, a->nxdn_sys, b->nxdn_sys)
             || site_differs(a, b, DSD_RXID_NXDN_LOCATION, a->nxdn_site, b->nxdn_site)
             || site_differs(a, b, DSD_RXID_NXDN_IDAS_AREA, a->nxdn_idas_area, b->nxdn_idas_area)
             || site_differs(a, b, DSD_RXID_NXDN_IDAS_SITE, a_idas, b_idas)
             || site_differs(a, b, DSD_RXID_NXDN_TYPED_SYSTEM, a->nxdn_typed_sys, b->nxdn_typed_sys)
             || site_differs(a, b, DSD_RXID_NXDN_TYPED_SYSTEM, a->nxdn_typed_site, b->nxdn_typed_site)
             || site_nxdn_cross_conflict(a, b));
}

/* Both snapshots hold EDACS parts here, so both modes qualify one. */
static int
site_edacs_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    return a->edacs_ea_mode == b->edacs_ea_mode
           && !(site_differs(a, b, DSD_RXID_EDACS_SITE, a->edacs_site, b->edacs_site)
                || site_differs(a, b, DSD_RXID_EDACS_AREA, a->edacs_area, b->edacs_area)
                || site_differs(a, b, DSD_RXID_EDACS_SYSTEM, a->edacs_system, b->edacs_system));
}

static int
site_tetra_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    return !(site_differs(a, b, DSD_RXID_TETRA_NET, a->tetra_mcc, b->tetra_mcc)
             || site_differs(a, b, DSD_RXID_TETRA_NET, a->tetra_mnc, b->tetra_mnc)
             || site_differs(a, b, DSD_RXID_TETRA_NET, a->tetra_colour, b->tetra_colour)
             || site_differs(a, b, DSD_RXID_TETRA_LA, a->tetra_la, b->tetra_la));
}

/* dsd_rxid_site_compatible() of two snapshots. */
static int
site_snapshots_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    if (a->valid == 0U || b->valid == 0U) {
        return 1;
    }
    if (a->family != b->family) {
        return 0;
    }
    switch (a->family) {
        case DSD_RXID_FAMILY_P25: return site_p25_compatible(a, b);
        case DSD_RXID_FAMILY_DMR: return site_dmr_compatible(a, b);
        case DSD_RXID_FAMILY_NXDN: return site_nxdn_compatible(a, b);
        case DSD_RXID_FAMILY_EDACS: return site_edacs_compatible(a, b);
        case DSD_RXID_FAMILY_TETRA: return site_tetra_compatible(a, b);
        default: return 1;
    }
}

int
dsd_rxid_site_compatible(const dsd_received_identity* a, const dsd_received_identity* b) {
    dsd_received_identity sa;
    dsd_received_identity sb;
    dsd_rxid_site_snapshot(a, &sa);
    dsd_rxid_site_snapshot(b, &sb);
    return site_snapshots_compatible(&sa, &sb);
}

/* Where both snapshots carry a text part, the strcmp-smaller text (snapshots are NUL-terminated, zero past the NUL). */
static void
site_union_text(char* dst, const char* a, const char* b, size_t capacity) {
    if (strcmp(b, a) < 0) {
        DSD_MEMCPY(dst, b, capacity);
    }
}

void
dsd_rxid_site_union(const dsd_received_identity* a, const dsd_received_identity* b, dsd_received_identity* out) {
    if (out == NULL) {
        return;
    }
    dsd_received_identity sa;
    dsd_received_identity sb;
    dsd_received_identity u;
    dsd_rxid_site_snapshot(a, &sa);
    dsd_rxid_site_snapshot(b, &sb);
    DSD_MEMSET(&u, 0, sizeof(u));
    if (site_snapshots_compatible(&sa, &sb)) {
        /* A part both hold has equal values on both sides (compatible), so taking it from sa is order-free. */
        site_copy_parts(&u, &sa, sa.valid);
        site_copy_parts(&u, &sb, sb.valid & ~sa.valid);
        u.valid = sa.valid | sb.valid;
        if (site_has(&sa, DSD_RXID_DMR_SITE_TEXT) && site_has(&sb, DSD_RXID_DMR_SITE_TEXT)) {
            site_union_text(u.dmr_site_text, sa.dmr_site_text, sb.dmr_site_text, sizeof(u.dmr_site_text));
        }
        if (site_has(&sa, DSD_RXID_DMR_BRAND) && site_has(&sb, DSD_RXID_DMR_BRAND)) {
            site_union_text(u.dmr_brand, sa.dmr_brand, sb.dmr_brand, sizeof(u.dmr_brand));
        }
        /* Known on both sides they are equal; a snapshot holds 0 where nothing qualifies them. */
        u.dmr_trunk = sa.dmr_trunk != 0U ? sa.dmr_trunk : sb.dmr_trunk;
        u.edacs_ea_mode = site_has(&sa, SITE_EDACS) ? sa.edacs_ea_mode : sb.edacs_ea_mode;
        u.family = sa.valid != 0U ? sa.family : sb.family;
    }
    DSD_MEMCPY(out, &u, sizeof(u));
}

int
dsd_nxdn_location_valid(uint8_t category, uint32_t sys, uint16_t site) {
    /* NXDN TS 1-A 6.5.2 Table 6.5-1, indexed by the record's category: 0 Global, 1 Regional, 2 Local. */
    static const struct {
        uint32_t sys_max;
        uint16_t site_max;
    } k_ranges[3] = {{1022U, 4094U}, {16382U, 254U}, {131070U, 30U}};

    if (category > 2U) {
        return 0; /* 3 is reserved */
    }
    return sys >= 1U && sys <= k_ranges[category].sys_max && site >= 1U && site <= k_ranges[category].site_max;
}

static int
site_p25_consistent(const dsd_received_identity* a) {
    if (site_has(a, DSD_RXID_P25_WACN) && a->p25_wacn > 0xFFFFFU) {
        return 0;
    }
    if (site_has(a, DSD_RXID_P25_SYSID) && (a->p25_sysid == 0U || a->p25_sysid > 0xFFFU)) {
        return 0;
    }
    /* A network status carries its SYSID, and a site or LRA is bound to one. */
    return !site_has(a, DSD_RXID_P25_WACN | DSD_RXID_P25_SITE | DSD_RXID_P25_LRA) || site_has(a, DSD_RXID_P25_SYSID);
}

static int
site_dmr_consistent(const dsd_received_identity* a) {
    if (!site_has(a, SITE_DMR)) {
        return 1;
    }
    if (a->dmr_trunk > (uint8_t)DSD_RXID_DMR_TRUNK_XPT) {
        return 0;
    }
    /* Numbers are noted with their trunking system, a network with its site, a system code by Tier III only. */
    if (site_has(a, DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE) && a->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_NONE) {
        return 0;
    }
    if (site_has(a, DSD_RXID_DMR_NET) && !site_has(a, DSD_RXID_DMR_SITE)) {
        return 0;
    }
    if (site_has(a, DSD_RXID_DMR_T3_SYSCODE) && a->dmr_trunk != (uint8_t)DSD_RXID_DMR_TRUNK_TIER3) {
        return 0;
    }
    /* A model is Tier III's (0..3); a site of another trunking system is noted with model 0. */
    if (!site_has(a, DSD_RXID_DMR_SITE)) {
        return 1;
    }
    return a->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_TIER3 ? a->dmr_t3_model <= 3U : a->dmr_t3_model == 0U;
}

static int
site_nxdn_consistent(const dsd_received_identity* a) {
    if (site_has(a, DSD_RXID_NXDN_LOCATION) && !dsd_nxdn_location_valid(a->nxdn_category, a->nxdn_sys, a->nxdn_site)) {
        return 0;
    }
    if (site_has(a, DSD_RXID_NXDN_IDAS_AREA) && a->nxdn_idas_area > 1U) {
        return 0;
    }
    if (site_has(a, DSD_RXID_NXDN_IDAS_SITE)
        && (a->nxdn_idas_site_type < 1U || a->nxdn_idas_site_type > 3U || a->nxdn_idas_site_code < 1U
            || a->nxdn_idas_site_code > 250U)) {
        return 0;
    }
    if (site_has(a, DSD_RXID_NXDN_TYPED_SYSTEM) && a->nxdn_typed_sys > 0x7FFFFU) {
        return 0;
    }
    /* The cross-part rules a record keeps within itself. */
    return !site_nxdn_cross_conflict(a, a);
}

static int
site_tetra_consistent(const dsd_received_identity* a) {
    if (site_has(a, DSD_RXID_TETRA_NET)
        && (a->tetra_mcc > 0x3FFU || a->tetra_mnc > 0x3FFFU || a->tetra_colour > 0x3FU)) {
        return 0;
    }
    /* A location area is noted only under a network. */
    return !site_has(a, DSD_RXID_TETRA_LA) || (a->tetra_la <= 0x3FFFU && site_has(a, DSD_RXID_TETRA_NET));
}

int
dsd_rxid_site_consistent(const dsd_received_identity* a) {
    if (a == NULL || a->family > (uint8_t)DSD_RXID_FAMILY_EDACS || (a->valid & ~site_parts(a->family)) != 0U) {
        return 0;
    }
    if (memchr(a->dmr_site_text, '\0', sizeof(a->dmr_site_text)) == NULL
        || memchr(a->dmr_brand, '\0', sizeof(a->dmr_brand)) == NULL) {
        return 0;
    }
    switch (a->family) {
        case DSD_RXID_FAMILY_P25: return site_p25_consistent(a);
        case DSD_RXID_FAMILY_DMR: return site_dmr_consistent(a);
        case DSD_RXID_FAMILY_NXDN: return site_nxdn_consistent(a);
        case DSD_RXID_FAMILY_TETRA: return site_tetra_consistent(a);
        default: return 1;
    }
}

/* Whether a known channel @p kind is the numbering of DMR trunking system @p trunk. */
static int
site_kind_names_trunk(uint8_t kind, uint8_t trunk) {
    switch (kind) {
        case DSD_CALL_CHANNEL_LSN:
            return trunk == (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS || trunk == (uint8_t)DSD_RXID_DMR_TRUNK_XPT;
        case DSD_CALL_CHANNEL_LPCN: return trunk == (uint8_t)DSD_RXID_DMR_TRUNK_TIER3;
        case DSD_CALL_CHANNEL_LCN: return trunk == (uint8_t)DSD_RXID_DMR_TRUNK_CONNECT_PLUS;
        default: return 0;
    }
}

/* Whether channel @p kind fits snapshot @p s: no kind fits anything; a kind fits an empty site, and a DMR site whose
   trunking system is unknown or the one the kind numbers. */
static int
site_kind_fits(uint8_t kind, const dsd_received_identity* s) {
    if (kind == (uint8_t)DSD_CALL_CHANNEL_NONE || s->valid == 0U) {
        return 1;
    }
    if (s->family != (uint8_t)DSD_RXID_FAMILY_DMR) {
        return 0;
    }
    return s->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_NONE || site_kind_names_trunk(kind, s->dmr_trunk);
}

int
dsd_call_site_compatible(const dsd_received_identity* site_a, uint8_t kind_a, const dsd_received_identity* site_b,
                         uint8_t kind_b) {
    if (kind_a > (uint8_t)DSD_CALL_CHANNEL_LCN || kind_b > (uint8_t)DSD_CALL_CHANNEL_LCN) {
        return 0;
    }
    if (kind_a != (uint8_t)DSD_CALL_CHANNEL_NONE && kind_b != (uint8_t)DSD_CALL_CHANNEL_NONE && kind_a != kind_b) {
        return 0;
    }
    dsd_received_identity sa;
    dsd_received_identity sb;
    dsd_rxid_site_snapshot(site_a, &sa);
    dsd_rxid_site_snapshot(site_b, &sb);
    return site_snapshots_compatible(&sa, &sb) && site_kind_fits(kind_a, &sa) && site_kind_fits(kind_a, &sb)
           && site_kind_fits(kind_b, &sa) && site_kind_fits(kind_b, &sb);
}

/* The DMR trunking system whole record @p r names by any of its trunking parts, the moment's rest LSN and free LCN
   included; NONE when it names none. */
static uint8_t
record_dmr_trunk(const dsd_received_identity* r) {
    if (r == NULL || r->family != (uint8_t)DSD_RXID_FAMILY_DMR
        || (r->valid & (SITE_DMR | DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_XPT_FREE_LCN)) == 0U) {
        return (uint8_t)DSD_RXID_DMR_TRUNK_NONE;
    }
    return r->dmr_trunk;
}

/* Whether channel @p kind fits trunking system @p trunk: no kind, or no trunk, fits anything. */
static int
record_kind_fits_trunk(uint8_t kind, uint8_t trunk) {
    return kind == (uint8_t)DSD_CALL_CHANNEL_NONE || trunk == (uint8_t)DSD_RXID_DMR_TRUNK_NONE
           || site_kind_names_trunk(kind, trunk);
}

int
dsd_call_record_site_compatible(const dsd_received_identity* record_a, uint8_t kind_a,
                                const dsd_received_identity* record_b, uint8_t kind_b) {
    const uint8_t trunk_a = record_dmr_trunk(record_a);
    const uint8_t trunk_b = record_dmr_trunk(record_b);
    if (trunk_a != (uint8_t)DSD_RXID_DMR_TRUNK_NONE && trunk_b != (uint8_t)DSD_RXID_DMR_TRUNK_NONE
        && trunk_a != trunk_b) {
        return 0;
    }
    const uint8_t trunk = trunk_a != (uint8_t)DSD_RXID_DMR_TRUNK_NONE ? trunk_a : trunk_b;
    return dsd_call_site_compatible(record_a, kind_a, record_b, kind_b) && record_kind_fits_trunk(kind_a, trunk)
           && record_kind_fits_trunk(kind_b, trunk);
}
