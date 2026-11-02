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
 * (dsd_opts::dmr_dmrla_n while dmr_dmrla_is_set). No -D, or -D 0, shows the site whole; a DMRLA past the SITE width is
 * clamped to it, so it too leaves nothing to split off.
 *
 * dsd-neo shows every Tier III number one-based (issue #799 D1): the network, the site, and with a split the area and
 * the site in it, each as the raw value + 1, on every Tier III path (Capacity Max included). The received identity
 * keeps the raw values.
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

/* The DMRLA the user set (-D n), 0 when none was. */
static inline uint16_t
dmr_tiii_dmrla(uint8_t is_set, uint8_t n) {
    return is_set ? (uint16_t)n : 0U;
}

static inline uint16_t
dmr_tiii_clamp_dmrla(uint16_t site_bits, uint16_t dmrla_n) {
    return dmrla_n > site_bits ? site_bits : dmrla_n;
}

/* Whether DMRLA @p dmrla_n splits a SITE of @p site_bits bits into an area and a site in it. */
static inline int
dmr_tiii_is_split(uint16_t site_bits, uint16_t dmrla_n) {
    return dmrla_n != 0U && dmrla_n < site_bits;
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

static inline uint16_t
dmr_tiii_display_net(uint16_t net) {
    return (uint16_t)(net + 1U);
}

/* The site as dsd-neo shows it: "<site + 1>", or "<area + 1>.<site in area + 1>" when DMRLA @p dmrla_n splits it. */
static inline void
dmr_tiii_site_label(char* out, size_t out_size, uint16_t site, uint16_t site_bits, uint16_t dmrla_n) {
    if (dmr_tiii_is_split(site_bits, dmrla_n)) {
        DSD_SNPRINTF(out, out_size, "%u.%u", (unsigned)dmr_tiii_area(site, site_bits, dmrla_n) + 1U,
                     (unsigned)dmr_tiii_site_in_area(site, site_bits, dmrla_n) + 1U);
    } else {
        DSD_SNPRINTF(out, out_size, "%u", (unsigned)site + 1U);
    }
}

/* Site note for a Tier III system identity, to the received identity record (dsd_state::rx_id) and, while one runs, the
 * signal search: call it only from a CSBK that passed its CRC (C_ALOHA, not one -F accepted by RAS) or a short LC that
 * passed Hamming(17,12,3) and its CRC-8 (C/P_SYS_PARMS), with that message's own fields, after it was handled.
 * @p syscode is the 14-bit system code (MODEL, NET, SITE) whose model the record keeps with the raw @p net and @p site;
 * @p model names it, and @p dmrla_n is the user's DMRLA. The text reads as the site parameters do, one-based, e.g.
 * "TIII Small · Net 3 · Site 28 · SYS 105B", or "... · Site 4.4 · ..." with -D 2. */
static inline void
dmr_tiii_note_search_site(dsd_state* state, const char* model, uint16_t net, uint16_t site, uint16_t dmrla_n,
                          uint16_t syscode) {
    static const char dot[] = " \xC2\xB7 ";
    const uint8_t model_id = (uint8_t)((syscode >> 12) & 0x03U);
    char site_label[16];
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_DMR_TIII;
    dmr_tiii_site_label(site_label, sizeof(site_label), site, dmr_tiii_site_bits(model_id), dmrla_n);
    DSD_SNPRINTF(note.text, sizeof(note.text), "TIII %s%sNet %u%sSite %s%sSYS %04X", model, dot,
                 (unsigned)dmr_tiii_display_net(net), dot, site_label, dot, (unsigned)syscode);
    /* The system code first: another one drops the site the old system left. */
    dsd_rxid_note_dmr_t3_syscode(state, syscode);
    dsd_rxid_note_dmr_site(state, (uint8_t)DSD_RXID_DMR_TRUNK_TIER3, 1, net, site, model_id, note.text);
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
