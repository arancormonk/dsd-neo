// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Site notes from P25 broadcasts: the signal search's, and the received identity record's (dsd_state::rx_id). Call
 * each only after the message passed its CRC (TSBK CRC-16, MBT header CRC-16 and CRC-32, Phase 2 MAC CRC-12/16) and
 * was handled, with that message's own values: never decoder state (p2_wacn, p2_sysid, RFSS/site, which hard-set
 * values, caches and other messages also write), a voice LCW (its Reed-Solomon word accepts random input) or a
 * fill-in. The record binds a site and the location registration area to the SYSID the message carries, as the search
 * does. */
#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H
#define DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>

static inline void
p25_search_note_network(dsd_state* state, uint32_t wacn, uint16_t sysid, uint8_t lra) {
    /* SYSID 0 names no system (an RFSS status would not bind to it either). */
    if ((sysid & 0xFFFU) == 0U) {
        return;
    }
    dsd_rxid_note_p25_network(state, wacn, sysid);
    dsd_rxid_note_p25_lra(state, sysid, lra);
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_P25_NETWORK;
    note.wacn = wacn & 0xFFFFFU;
    note.sysid = (uint16_t)(sysid & 0xFFFU);
    dsd_signal_search_note_site(state, &note);
}

static inline void
p25_search_note_rfss(dsd_state* state, uint16_t sysid, uint8_t rfss, uint8_t site, uint8_t lra) {
    /* SYSID 0 names no system, as for a network status. */
    if ((sysid & 0xFFFU) == 0U) {
        return;
    }
    dsd_rxid_note_p25_site(state, sysid, rfss, site);
    dsd_rxid_note_p25_lra(state, sysid, lra);
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_P25_SITE;
    note.sysid = (uint16_t)(sysid & 0xFFFU);
    note.rfss = rfss;
    note.site = site;
    dsd_signal_search_note_site(state, &note);
}

/* The NAC a Phase 2 network status that passed its CRC carries: this carrier's, whatever seed is configured. NAC $000
 * and $FFF name no carrier (TIA-102.BAAC), as for a Phase 1 NID. */
static inline void
p25_search_note_network_nac(dsd_state* state, uint16_t nac) {
    nac &= 0xFFFU;
    if (nac != 0U && nac != 0xFFFU) {
        dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, nac);
    }
}

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H */
