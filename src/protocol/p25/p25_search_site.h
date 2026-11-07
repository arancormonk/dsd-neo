// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Site notes from P25 broadcasts: the signal search's, and the received identity record's (dsd_state::rx_id). Call
 * each only after the message passed its CRC (TSBK CRC-16, MBT header CRC-16 and CRC-32, Phase 2 MAC CRC-12/16) and
 * was handled, with that message's own values: never decoder state (p2_wacn, p2_sysid, RFSS/site, which hard-set
 * values, caches and other messages also write), a voice LCW (its Reed-Solomon word accepts random input) or a
 * fill-in. The record binds a site and the location registration area to the SYSID the message carries, as the search
 * does. The carrier's proven Phase 2 NAC, which a burst's Reed-Solomon check or a network status's CRC proves, is noted
 * to the search here too (p25_search_note_proven_nac()). */
#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H
#define DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
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

/* The NAC a Phase 2 network status that passed its CRC carries: this carrier's, whatever seed is configured. dsd-neo
 * takes no NAC $000 or $FFF, as for a Phase 1 NID: a dead or stuck input decodes as either. */
static inline void
p25_search_note_network_nac(dsd_state* state, uint16_t nac) {
    nac &= 0xFFFU;
    if (nac != 0U && nac != 0xFFFU) {
        dsd_rxid_note_access(state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, nac);
    }
}

/* A checked Phase 2 proof on this carrier named @p nac: a burst descrambled with the seed passed its Reed-Solomon check
 * (p25p2_note_seed_proven()), or a network status passed its CRC (p25p2_vpdu_apply_nsb_identity()). The latest proof is
 * the carrier's proven NAC, which a Phase 2 call reads (dsd_state::p2_proven_nac). A proof may come after the
 * transmission's last evidence (a final 2V burst's ESS, the burst carrying MAC_END_PTT, a final network status naming
 * another NAC under the same WACN and SYSID), so each new value tells the search, whatever else the message noted.
 * 1 when the proven NAC changed. */
static inline int
p25_search_note_proven_nac(dsd_state* state, unsigned long long nac) {
    if (!dsd_state_note_p2_proven_nac(state, nac)) {
        return 0;
    }
    dsd_signal_search_note_access(state);
    return 1;
}

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H */
