// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel provenance (<dsd-neo/protocol/dmr/dmr_t3_channels.h>): the origin record of each channel map
 * row, the map import and clear notes, the network change, the missing-channel ledger and its log lines, and the
 * per-target save and restore. Also owns the DMR state extension (dmr_t3_channels_internal.h) that the RC notice dedupe
 * shares. The over-the-air writer is dmr_t3_chan_learn.c; nothing here posts an event, so the engine and app-control
 * links stay free of the emitter.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dmr_call_channel.h"
#include "dmr_t3_channels_internal.h"
#include "dmr_tiii_site.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

enum { DMR_T3_ROWS = 0x1000 };

dmr_ext_t*
dmr_ext_get(const dsd_state* state) {
    return state != NULL ? DSD_STATE_EXT_GET_AS(dmr_ext_t, state, DSD_STATE_EXT_PROTO_DMR_RC) : NULL;
}

dmr_ext_t*
dmr_ext_get_or_create(dsd_state* state) {
    if (state == NULL) {
        return NULL;
    }
    dmr_ext_t* ext = dmr_ext_get(state);
    if (ext != NULL) {
        return ext;
    }
    ext = (dmr_ext_t*)calloc(1, sizeof(*ext));
    if (ext == NULL) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_DMR_RC, ext, free) != 0) {
        free(ext);
        return NULL;
    }
    return ext;
}

int
dmr_t3_chan_origin_is_ota(unsigned origin) {
    return origin >= (unsigned)DMR_CHAN_ORIGIN_SITE && origin <= (unsigned)DMR_CHAN_ORIGIN_ESTIMATE;
}

/* The origin a recorded byte names; a byte no origin has (none is ever written) reads as FILE, which nothing over the air
   replaces. */
static dmr_chan_origin_e
dmr_t3_chan_origin_of_byte(uint8_t origin) {
    switch (origin) {
        case DMR_CHAN_ORIGIN_USER: return DMR_CHAN_ORIGIN_USER;
        case DMR_CHAN_ORIGIN_SITE: return DMR_CHAN_ORIGIN_SITE;
        case DMR_CHAN_ORIGIN_UNVERIFIED: return DMR_CHAN_ORIGIN_UNVERIFIED;
        case DMR_CHAN_ORIGIN_NEIGHBOUR: return DMR_CHAN_ORIGIN_NEIGHBOUR;
        case DMR_CHAN_ORIGIN_ESTIMATE: return DMR_CHAN_ORIGIN_ESTIMATE;
        default: return DMR_CHAN_ORIGIN_FILE;
    }
}

dmr_chan_origin_e
dmr_t3_chan_origin(const dsd_state* state, uint16_t lpcn) {
    if (state == NULL || lpcn >= DMR_T3_ROWS || state->trunk_chan_map[lpcn] == 0) {
        return DMR_CHAN_ORIGIN_NONE;
    }
    const dmr_ext_t* ext = dmr_ext_get(state);
    const uint8_t origin = ext != NULL ? ext->origin[lpcn] : (uint8_t)DMR_CHAN_ORIGIN_NONE;
    /* A row another writer set after the air is that writer's, as a file's is. */
    if (origin == (uint8_t)DMR_CHAN_ORIGIN_NONE
        || (dmr_t3_chan_origin_is_ota(origin) && !dsd_state_dmr_lcn_ota_get(state, lpcn))) {
        return DMR_CHAN_ORIGIN_FILE;
    }
    return dmr_t3_chan_origin_of_byte(origin);
}

int
dmr_t3_chan_drop_rows(dsd_state* state, dmr_ext_t* ext, int (*drop)(unsigned origin)) {
    int dropped = 0;
    for (unsigned l = 1U; l <= DMR_T3_MAX_LCN; l++) {
        if (!drop(ext->origin[l])) {
            continue;
        }
        if (!dsd_state_dmr_lcn_ota_get(state, l)) {
            /* Another writer set the row after the air: it stays theirs, and the air's record of it goes. */
            ext->origin[l] = (uint8_t)DMR_CHAN_ORIGIN_NONE;
            continue;
        }
        dsd_state_dmr_lcn_ota_set(state, l, 0); /* the air's own row: no takeover */
        dsd_state_set_trunk_chan_freq(state, l, 0);
        state->dmr_lcn_trust[l] = 0U;
        ext->origin[l] = (uint8_t)DMR_CHAN_ORIGIN_NONE;
        dropped = 1;
    }
    return dropped;
}

void
dmr_t3_chan_note_map_loaded(dsd_state* state) {
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return; /* every nonzero row reads as FILE without the record */
    }
    for (unsigned l = 0U; l < DMR_T3_ROWS; l++) {
        const int file_row = dmr_t3_lpcn_is_channel(l) && state->trunk_chan_map[l] != 0;
        ext->origin[l] = (uint8_t)(file_row ? DMR_CHAN_ORIGIN_FILE : DMR_CHAN_ORIGIN_NONE);
    }
    DSD_MEMSET(state->dmr_lcn_ota, 0, sizeof(state->dmr_lcn_ota));
    ext->revision++;
    dmr_t3_missing_reconcile(state);
}

void
dmr_t3_chan_note_map_cleared(dsd_state* state) {
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMSET(ext->origin, 0, sizeof(ext->origin));
    DSD_MEMSET(state->dmr_lcn_ota, 0, sizeof(state->dmr_lcn_ota));
    ext->revision++;
    dmr_t3_missing_reconcile(state);
}

/* ---- Missing-channel ledger ---- */

#define DMR_T3_MISSING_DETAIL_CAP(l) (sizeof((l)->details) / sizeof((l)->details[0]))

static int
dmr_t3_missing_bit(const uint8_t* set, uint16_t lpcn) {
    return (set[lpcn >> 3] >> (lpcn & 7U)) & 1U;
}

static void
dmr_t3_missing_bit_set(uint8_t* set, uint16_t lpcn) {
    set[lpcn >> 3] = (uint8_t)(set[lpcn >> 3] | (1U << (lpcn & 7U)));
}

static void
dmr_t3_missing_bit_clear(uint8_t* set, uint16_t lpcn) {
    set[lpcn >> 3] = (uint8_t)(set[lpcn >> 3] & ~(1U << (lpcn & 7U)));
}

static size_t
dmr_t3_missing_popcount(const dmr_t3_missing_ledger* l) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(l->missing); i++) {
        for (unsigned b = l->missing[i]; b != 0U; b &= b - 1U) {
            n++;
        }
    }
    return n;
}

/* The detail slot for @p lpcn, moved to the front: its own entry, else a new one, the oldest giving way when all are
   taken. */
static dmr_t3_missing_detail*
dmr_t3_missing_front(dmr_t3_missing_ledger* l, uint16_t lpcn) {
    size_t i = 0;
    while (i < l->detail_count && l->details[i].lpcn != lpcn) {
        i++;
    }
    if (i == l->detail_count) {
        if (l->detail_count < DMR_T3_MISSING_DETAIL_CAP(l)) {
            l->detail_count++;
        }
        i = (size_t)l->detail_count - 1U;
    }
    if (i > 0U) {
        DSD_MEMMOVE(&l->details[1], &l->details[0], i * sizeof(l->details[0]));
    }
    return &l->details[0];
}

/* Drop the details whose channel left the missing set, keeping the order of the rest. */
static void
dmr_t3_missing_prune_details(dmr_t3_missing_ledger* l) {
    size_t kept = 0;
    for (size_t i = 0; i < l->detail_count; i++) {
        if (dmr_t3_missing_bit(l->missing, l->details[i].lpcn)) {
            l->details[kept++] = l->details[i];
        }
    }
    l->detail_count = (uint8_t)kept;
}

/* A network change: nothing it recorded, reported included, says anything about the new network. */
static void
dmr_t3_missing_reset(dmr_t3_missing_ledger* l) {
    const uint32_t revision = l->revision;
    DSD_MEMSET(l, 0, sizeof(*l));
    l->revision = revision + 1U;
}

void
dmr_t3_missing_note_grant(dsd_opts* opts, dsd_state* state, uint16_t lpcn, const dmr_t3_grant_class* cls, uint8_t slot,
                          int unverified) {
    (void)opts;
    if (state == NULL || cls == NULL || !dmr_t3_lpcn_is_channel(lpcn) || state->trunk_chan_map[lpcn] != 0) {
        return;
    }
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    dmr_t3_missing_ledger* l = &ext->missing;
    dmr_t3_missing_bit_set(l->missing, lpcn);
    if (!unverified) {
        dmr_t3_missing_bit_set(l->checked, lpcn);
    }
    dmr_t3_missing_detail* d = dmr_t3_missing_front(l, lpcn);
    d->lpcn = lpcn;
    d->slot = (uint8_t)(slot & 1U);
    d->group = (uint8_t)(cls->group != 0U);
    d->data = (uint8_t)(cls->data != 0U);
    d->unverified = (uint8_t)!dmr_t3_missing_bit(l->checked, lpcn);
    d->caller = cls->caller;
    d->recipient = cls->recipient;
    d->last_heard_ms = dsd_decode_now_mono_ms();
    l->revision++;
    if (!dmr_t3_missing_bit(l->reported, lpcn)) {
        dmr_t3_missing_bit_set(l->reported, lpcn);
        LOG_INFO("NOTICE: DMR Tier III: channel %u has no frequency; dsd-neo cannot retune to its calls%s\n",
                 (unsigned)lpcn, unverified ? " (unchecked)" : "");
    }
}

void
dmr_t3_missing_forget(dmr_ext_t* ext, uint16_t lpcn) {
    dmr_t3_missing_ledger* l = &ext->missing;
    if (!dmr_t3_lpcn_is_channel(lpcn) || !dmr_t3_missing_bit(l->missing, lpcn)) {
        return;
    }
    dmr_t3_missing_bit_clear(l->missing, lpcn);
    dmr_t3_missing_bit_clear(l->checked, lpcn);
    dmr_t3_missing_prune_details(l);
    l->revision++;
}

void
dmr_t3_missing_reconcile(const dsd_state* state) {
    dmr_ext_t* ext = dmr_ext_get(state);
    if (ext == NULL) {
        return;
    }
    dmr_t3_missing_ledger* l = &ext->missing;
    int changed = 0;
    for (uint16_t c = 1U; c <= DMR_T3_MAX_LCN; c++) {
        if (dmr_t3_missing_bit(l->missing, c) && state->trunk_chan_map[c] != 0) {
            dmr_t3_missing_bit_clear(l->missing, c);
            dmr_t3_missing_bit_clear(l->checked, c);
            changed = 1;
        }
    }
    if (changed) {
        dmr_t3_missing_prune_details(l);
        l->revision++;
    }
}

size_t
dmr_t3_missing_count(const dsd_state* state) {
    const dmr_ext_t* ext = dmr_ext_get(state);
    return ext != NULL ? dmr_t3_missing_popcount(&ext->missing) : 0U;
}

void
dmr_t3_missing_copy(const dsd_state* state, dmr_t3_missing_ledger* out) {
    if (out == NULL) {
        return;
    }
    const dmr_ext_t* ext = dmr_ext_get(state);
    if (ext == NULL) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return;
    }
    DSD_MEMCPY(out, &ext->missing, sizeof(*out));
}

/* Append channel @p lpcn after @p sep to the summary in @p msg, its length @p used; past the buffer it is cut. */
static void
dmr_t3_missing_append(char* msg, size_t cap, size_t* used, const char* sep, uint16_t lpcn, int unchecked) {
    if (*used >= cap) {
        return;
    }
    const int w =
        DSD_SNPRINTF(msg + *used, cap - *used, "%s%u%s", sep, (unsigned)lpcn, unchecked ? " (unchecked)" : "");
    if (w > 0) {
        *used += (size_t)w;
    }
}

/* The first channels of @p l in ascending order, at most as many as it has details, then how many more. */
static void
dmr_t3_missing_append_channels(char* msg, size_t cap, size_t* used, const dmr_t3_missing_ledger* l, size_t total) {
    const size_t shown_max = DMR_T3_MISSING_DETAIL_CAP(l);
    size_t shown = 0;
    for (uint16_t c = 1U; c <= DMR_T3_MAX_LCN && shown < shown_max; c++) {
        if (dmr_t3_missing_bit(l->missing, c)) {
            dmr_t3_missing_append(msg, cap, used, shown == 0U ? " " : ", ", c, !dmr_t3_missing_bit(l->checked, c));
            shown++;
        }
    }
    if (total > shown && *used < cap) {
        const int w = DSD_SNPRINTF(msg + *used, cap - *used, " (+%zu more)", total - shown);
        if (w > 0) {
            *used += (size_t)w;
        }
    }
}

void
dmr_t3_missing_log_summary_for(const dmr_t3_missing_ledger* ledger, const char* target) {
    if (ledger == NULL) {
        return;
    }
    const size_t total = dmr_t3_missing_popcount(ledger);
    if (total == 0U) {
        return;
    }
    const int named = target != NULL && target[0] != '\0';
    char msg[512];
    const int n = DSD_SNPRINTF(msg, sizeof msg,
                               "DMR Tier III%s%s%s: %zu channel%s granted with no frequency, so dsd-neo could not "
                               "follow %s calls:",
                               named ? " (" : "", named ? target : "", named ? ")" : "", total,
                               total == 1U ? " was" : "s were", total == 1U ? "its" : "their");
    if (n < 0) {
        return;
    }
    size_t used = (size_t)n;
    dmr_t3_missing_append_channels(msg, sizeof msg, &used, ledger, total);
    LOG_INFO("NOTICE: %s\n", msg);
}

void
dmr_t3_missing_log_summary(const dsd_state* state) {
    const dmr_ext_t* ext = dmr_ext_get(state);
    if (ext != NULL) {
        dmr_t3_missing_log_summary_for(&ext->missing, NULL);
    }
}

/* What the air taught goes with the network it came from; the next checked system code starts a new baseline. The
   control channels it named stay in the hunt's candidate list until the next carrier loss, as any other's. */
static void
dmr_t3_chan_drop_network(dsd_state* state, dmr_ext_t* ext) {
    (void)dmr_t3_chan_drop_rows(state, ext, dmr_t3_chan_origin_is_ota);
    dmr_t3_missing_reset(&ext->missing);
    ext->generation++;
    ext->revision++;
    ext->syscode_valid = 0U;
}

void
dmr_t3_chan_network_changed(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    /* With no record nothing was learned over the air, so nothing goes: a carrier boundary of any protocol runs
       this. */
    dmr_ext_t* ext = dmr_ext_get(state);
    if (ext != NULL) {
        dmr_t3_chan_drop_network(state, ext);
    }
}

/* Whether two system codes name one network: the same MODEL and NET fields, whatever the SITE (TS 102 361-4 §6.1.2, the
   channel plan is the network's). Both decode alike, so a vendor's numbering makes no difference here. */
static int
dmr_t3_chan_same_network(uint16_t a, uint16_t b) {
    const dmr_tiii_code ca = dmr_tiii_decode_syscode(a);
    const dmr_tiii_code cb = dmr_tiii_decode_syscode(b);
    return ca.model == cb.model && ca.net == cb.net;
}

void
dmr_t3_chan_note_checked_syscode(dsd_state* state, uint16_t syscode) {
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    if (ext->syscode_valid != 0U && !dmr_t3_chan_same_network(ext->syscode, syscode)) {
        dmr_t3_chan_drop_network(state, ext);
    }
    ext->syscode = syscode;
    ext->syscode_valid = 1U;
}

uint32_t
dmr_t3_chan_generation(const dsd_state* state) {
    const dmr_ext_t* ext = dmr_ext_get(state);
    return ext != NULL ? ext->generation : 0U;
}

/* The record's own count, plus the takeovers the shared setter counted (dsd_state::dmr_lcn_ota_seq): another writer
   taking a row at the frequency it had changes its origin without a DMR write. */
uint32_t
dmr_t3_chan_revision(const dsd_state* state) {
    if (state == NULL) {
        return 0U;
    }
    const dmr_ext_t* ext = dmr_ext_get(state);
    return (ext != NULL ? ext->revision : 0U) + state->dmr_lcn_ota_seq;
}

void
dmr_t3_chan_save(const dsd_state* state, dmr_t3_chan_target_snapshot* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (state == NULL) {
        return;
    }
    DSD_MEMCPY(out->trust, state->dmr_lcn_trust, sizeof(out->trust));
    DSD_MEMCPY(out->ota, state->dmr_lcn_ota, sizeof(out->ota));
    out->revision = dmr_t3_chan_revision(state); // the one a reader sees, the takeovers included
    const dmr_ext_t* ext = dmr_ext_get(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMCPY(out->origin, ext->origin, sizeof(out->origin));
    out->generation = ext->generation;
    out->syscode = ext->syscode;
    out->syscode_valid = ext->syscode_valid;
    DSD_MEMCPY(&out->missing, &ext->missing, sizeof(out->missing));
}

void
dmr_t3_chan_restore(dsd_state* state, const dmr_t3_chan_target_snapshot* in) {
    if (state == NULL || in == NULL) {
        return;
    }
    DSD_MEMCPY(state->dmr_lcn_trust, in->trust, sizeof(state->dmr_lcn_trust));
    DSD_MEMCPY(state->dmr_lcn_ota, in->ota, sizeof(state->dmr_lcn_ota));
    dmr_ext_t* ext = dmr_ext_get(state);
    /* A snapshot with no revision recorded nothing (every change bumps it): with no record here either, none is due. */
    if (ext == NULL && in->revision == 0U && in->generation == 0U && in->syscode_valid == 0U
        && in->missing.revision == 0U) {
        return;
    }
    ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMCPY(ext->origin, in->origin, sizeof(ext->origin));
    ext->generation = in->generation;
    /* Past both revisions a reader saw, each the record's count plus this state's takeover count (which the snapshot
       does not carry): the record keeps the part the takeovers do not. */
    const uint32_t current = dmr_t3_chan_revision(state);
    ext->revision = (current > in->revision ? current : in->revision) + 1U - state->dmr_lcn_ota_seq;
    ext->syscode = in->syscode;
    ext->syscode_valid = in->syscode_valid;
    const uint32_t missing_revision =
        (ext->missing.revision > in->missing.revision ? ext->missing.revision : in->missing.revision) + 1U;
    DSD_MEMCPY(&ext->missing, &in->missing, sizeof(ext->missing));
    ext->missing.revision = missing_revision;
}
