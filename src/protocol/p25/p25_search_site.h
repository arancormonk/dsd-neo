// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Signal search site notes from P25 broadcasts. Call each only after the message passed its CRC (TSBK CRC-16, MBT
 * header CRC-16 and CRC-32, Phase 2 MAC CRC-12/16) and was handled, with that message's own values: never decoder
 * state (p2_wacn, p2_sysid, RFSS/site, which hard-set values, caches and other messages also write), a voice LCW (its
 * Reed-Solomon word accepts random input) or a fill-in. */
#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H
#define DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>

static inline void
p25_search_note_network(const dsd_state* state, uint32_t wacn, uint16_t sysid) {
    /* SYSID 0 names no system (an RFSS status would not bind to it either). */
    if ((sysid & 0xFFFU) == 0U) {
        return;
    }
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_P25_NETWORK;
    note.wacn = wacn & 0xFFFFFU;
    note.sysid = (uint16_t)(sysid & 0xFFFU);
    dsd_signal_search_note_site(state, &note);
}

static inline void
p25_search_note_rfss(const dsd_state* state, uint16_t sysid, uint8_t rfss, uint8_t site) {
    /* SYSID 0 names no system, as for a network status. */
    if ((sysid & 0xFFFU) == 0U) {
        return;
    }
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_P25_SITE;
    note.sysid = (uint16_t)(sysid & 0xFFFU);
    note.rfss = rfss;
    note.site = site;
    dsd_signal_search_note_site(state, &note);
}

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_SEARCH_SITE_H */
