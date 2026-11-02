// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Tier III site numbers (TS 102 361-4 §6.3.2.2.1.1, Table 6.5 and Figure 6.15): the system code's MODEL sets the widths
 * of NET and SITE. The DMR location area (DMRLA, §6.3.2.2.3 and Figure 6.19) is the number of most significant SITE
 * bits that form the area; the radio is programmed with it and no message sends it, so dsd-neo takes it from -D n
 * (dsd_opts::dmr_dmrla_n while dmr_dmrla_is_set).
 *
 * The standard defines only the raw fields; how they are numbered for people is the vendor's:
 * - Motorola's Radio Management numbers a Capacity Max network and site as sent, and Capacity Max has no location area.
 *   A network dsd-neo recognised as Capacity Max (dsd_rxid_dmr_t3_is_capmax()) shows NET and SITE raw, the site always
 *   whole, whatever -D says.
 * - Hytera's CPS shows the network, the area and the site in it each one more than sent, the area taken from the top
 *   SITE bits. Every other Tier III network reads this way: the raw value + 1. No -D, or -D 0, shows the site whole;
 *   -D n splits off the top n bits as the area, and an n of the SITE width or more makes the whole SITE the area, with
 *   site 0 in it ("<SITE + 1>.1").
 *
 * Recognition comes only from a type-0 C_ALOHA (the TSCC's own system code) of Motorola's feature set (FID 0x10) that
 * passed its CRC, or that -F accepted as RAS on a carrier whose RAS is confirmed (dmr_csbk.c). Limits: a RAS Capacity
 * Max network decoded without -F is never recognised, the first messages before recognition read as standard, and
 * another vendor's own numbering reads as standard too. The received identity keeps the raw values.
 */

/* The SITE width of Tier III model @p model (Tiny 3, Small 5, Large 8, Huge 10 bits). */
static inline uint16_t
dmr_tiii_site_bits(uint8_t model) {
    switch (model & 0x03U) {
        case 0: return 3U;
        case 1: return 5U;
        case 2: return 8U;
        default: return 10U;
    }
}

/* A Tier III system code's fields: MODEL in its top two bits, then NET and SITE, whose widths the model sets. */
typedef struct {
    uint8_t model;
    uint16_t net;
    uint16_t site;
    uint16_t site_bits;
    const char* model_name;
} dmr_tiii_code;

/* The fields of the 14-bit system code @p syscode. Every path reads them here, so a CSBK and a short LC naming one code
 * agree. */
static inline dmr_tiii_code
dmr_tiii_decode_syscode(uint16_t syscode) {
    static const char* const names[4] = {"Tiny", "Small", "Large", "Huge"};
    dmr_tiii_code code;
    code.model = (uint8_t)((syscode >> 12) & 0x03U);
    code.site_bits = dmr_tiii_site_bits(code.model);
    code.site = (uint16_t)(syscode & ((1U << code.site_bits) - 1U));
    code.net = (uint16_t)((syscode & 0x0FFFU) >> code.site_bits);
    code.model_name = names[code.model];
    return code;
}

/* The DMRLA the user set (-D n), 0 when none was. */
static inline uint16_t
dmr_tiii_dmrla(uint8_t is_set, uint8_t n) {
    return is_set ? (uint16_t)n : 0U;
}

static inline uint16_t
dmr_tiii_clamp_dmrla(uint16_t site_bits, uint16_t dmrla_n) {
    return dmrla_n > site_bits ? site_bits : dmrla_n;
}

/* Whether DMRLA @p dmrla_n splits a SITE into an area and a site in it: any DMRLA does, one of the SITE width or more
 * leaving the whole SITE as the area and site 0 in it. */
static inline int
dmr_tiii_is_split(uint16_t site_bits, uint16_t dmrla_n) {
    (void)site_bits;
    return dmrla_n != 0U;
}

/* The location area (SYS_AREA): the top @p dmrla_n bits of @p site. */
static inline uint16_t
dmr_tiii_area(uint16_t site, uint16_t site_bits, uint16_t dmrla_n) {
    const uint16_t low = (uint16_t)(site_bits - dmr_tiii_clamp_dmrla(site_bits, dmrla_n));
    return (uint16_t)(site >> low);
}

/* The site within its location area: the SITE bits below the top @p dmrla_n. */
static inline uint16_t
dmr_tiii_site_in_area(uint16_t site, uint16_t site_bits, uint16_t dmrla_n) {
    const uint16_t low = (uint16_t)(site_bits - dmr_tiii_clamp_dmrla(site_bits, dmrla_n));
    return (uint16_t)(site & ((1U << low) - 1U));
}

/* The network as dsd-neo shows it: raw on a recognised Capacity Max network (@p capmax), else raw + 1. */
static inline uint16_t
dmr_tiii_display_net(uint16_t net, int capmax) {
    return capmax ? net : (uint16_t)(net + 1U);
}

/* The site as dsd-neo shows it: raw and whole on a recognised Capacity Max network (@p capmax); else "<site + 1>", or
 * "<area + 1>.<site in area + 1>" when DMRLA @p dmrla_n splits it. */
static inline void
dmr_tiii_site_label(char* out, size_t out_size, uint16_t site, uint16_t site_bits, uint16_t dmrla_n, int capmax) {
    if (capmax) {
        DSD_SNPRINTF(out, out_size, "%u", (unsigned)site);
    } else if (dmr_tiii_is_split(site_bits, dmrla_n)) {
        DSD_SNPRINTF(out, out_size, "%u.%u", (unsigned)dmr_tiii_area(site, site_bits, dmrla_n) + 1U,
                     (unsigned)dmr_tiii_site_in_area(site, site_bits, dmrla_n) + 1U);
    } else {
        DSD_SNPRINTF(out, out_size, "%u", (unsigned)site + 1U);
    }
}

/* The site text of system code @p syscode as the site parameters read, e.g. "TIII Small · Net 3 · Site 28 · SYS 105B"
 * (or "... · Site 4.4 · ..." with -D 2), and on a recognised Capacity Max network (@p capmax) its raw numbers after the
 * shared token: "TIII Large · Capacity Max · Net 1 · Site 1 · SYS 2101". */
static inline void
dmr_tiii_site_text(char* out, size_t out_size, uint16_t syscode, uint16_t dmrla_n, int capmax) {
    static const char dot[] = " \xC2\xB7 ";
    const dmr_tiii_code code = dmr_tiii_decode_syscode(syscode);
    char site_label[16];
    dmr_tiii_site_label(site_label, sizeof(site_label), code.site, code.site_bits, dmrla_n, capmax);
    DSD_SNPRINTF(out, out_size, "TIII %s%s%sNet %u%sSite %s%sSYS %04X", code.model_name, dot,
                 capmax ? DSD_RXID_T3_CAPMAX_TOKEN " \xC2\xB7 " : "", (unsigned)dmr_tiii_display_net(code.net, capmax),
                 dot, site_label, dot, (unsigned)syscode);
}

/* Site note for a Tier III system identity, to the received identity record (dsd_state::rx_id) and, while one runs, the
 * signal search: call it only from a CSBK that passed its CRC (C_ALOHA, not one -F accepted by RAS) or a short LC that
 * passed Hamming(17,12,3) and its CRC-8 (C/P_SYS_PARMS), after it was handled. @p syscode is the message's 14-bit system
 * code (MODEL, NET, SITE), whose raw fields the record keeps, and @p dmrla_n the user's DMRLA. The text names the
 * numbering the network uses (dmr_tiii_site_text()). */
static inline void
dmr_tiii_note_search_site(dsd_state* state, uint16_t syscode, uint16_t dmrla_n) {
    const dmr_tiii_code code = dmr_tiii_decode_syscode(syscode);
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_DMR_TIII;
    /* The system code first: another one drops the site the old system left, and a Capacity Max mark of another
       network. */
    dsd_rxid_note_dmr_t3_syscode(state, syscode);
    dmr_tiii_site_text(note.text, sizeof(note.text), syscode, dmrla_n,
                       dsd_rxid_dmr_t3_is_capmax(state, code.model, code.net));
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, code.net, code.site, code.model, note.text);
    if (dsd_signal_search_is_running(state)) {
        dsd_signal_search_note_site(state, &note);
    }
}

/* The Capacity Plus site a checked message names (its short LC 0xF with bits 20..21 clear, or its CSBK 0x3B), raw as
 * SDRTrunk and OP25 show it: one text for both, so the two alternating are one site. Capacity Plus names no network. */
static inline void
dmr_cap_plus_note_site(dsd_state* state, uint8_t site) {
    char text[32];
    DSD_SNPRINTF(text, sizeof(text), "Cap+ \xC2\xB7 Site %u", (unsigned)site);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, site, 0U, text);
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H */
