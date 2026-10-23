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
#include <stdint.h>

static inline uint16_t
dmr_tiii_model_default_split_n(uint8_t model) {
    switch (model & 0x03U) {
        case 0: return 3U;
        case 1: return 5U;
        case 2: return 8U;
        default: return 10U;
    }
}

static inline uint16_t
dmr_tiii_effective_split_n(uint16_t default_n, uint8_t override_is_set, uint8_t override_n, uint16_t site_bits) {
    uint16_t n = override_is_set ? (uint16_t)override_n : default_n;
    if (n > site_bits) {
        n = site_bits;
    }
    return n;
}

static inline uint16_t
dmr_tiii_subsite_mask(uint16_t n) {
    return (n == 0U) ? 0U : (uint16_t)((1U << n) - 1U);
}

static inline uint16_t
dmr_tiii_display_net(uint16_t net, uint16_t n) {
    return (n == 0U) ? net : (uint16_t)(net + 1U);
}

static inline uint16_t
dmr_tiii_display_site(uint16_t site, uint16_t n) {
    return (n == 0U) ? site : (uint16_t)((site >> n) + 1U);
}

static inline uint16_t
dmr_tiii_display_subsite(uint16_t site, uint16_t sub_mask, uint16_t n) {
    return (n == 0U) ? 0U : (uint16_t)((site & sub_mask) + 1U);
}

/* Site note for a Tier III system identity, to the received identity record (dsd_state::rx_id) and, while one runs, the
 * signal search: call it only from a CSBK that passed its CRC (C_ALOHA, not one -F accepted by RAS) or a short LC that
 * passed Hamming(17,12,3) and its CRC-8 (C/P_SYS_PARMS), with that message's own fields, after it was handled. The text
 * reads as the site parameters do, e.g. "TIII Small · Net 12 · Site 3.1 · SYS 0A3F". */
static inline void
dmr_tiii_note_search_site(dsd_state* state, const char* model, uint16_t net, uint16_t site, uint16_t n,
                          uint16_t sub_mask, uint16_t syscode) {
    static const char dot[] = " \xC2\xB7 ";
    dsd_signal_search_site_note note;
    DSD_MEMSET(&note, 0, sizeof(note));
    note.kind = DSD_SEARCH_SITE_NOTE_DMR_TIII;
    if (n != 0U) {
        DSD_SNPRINTF(note.text, sizeof(note.text), "TIII %s%sNet %u%sSite %u.%u%sSYS %04X", model, dot,
                     (unsigned)dmr_tiii_display_net(net, n), dot, (unsigned)dmr_tiii_display_site(site, n),
                     (unsigned)dmr_tiii_display_subsite(site, sub_mask, n), dot, (unsigned)syscode);
    } else {
        DSD_SNPRINTF(note.text, sizeof(note.text), "TIII %s%sNet %u%sSite %u%sSYS %04X", model, dot, (unsigned)net, dot,
                     (unsigned)site, dot, (unsigned)syscode);
    }
    /* The system code first: another one drops the site text the old system left. */
    dsd_rxid_note_dmr_t3_syscode(state, syscode);
    dsd_rxid_note_dmr_site_text(state, note.text);
    if (dsd_signal_search_is_running(state)) {
        dsd_signal_search_note_site(state, &note);
    }
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_TIII_SITE_H_H */
