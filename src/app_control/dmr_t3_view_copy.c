// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DMR Tier III channel record's two snapshot hops (dmr_t3_view_internal.h). The body (origins, ledger, overlay
 * rows) is keyed by the state instance, the parked trunk-scan target, the channel revision, the ledger's revision and
 * the channel map's sequence: a restarted session, or another target whose revisions happen to match, is another key,
 * so no copy keeps a record it does not hold.
 */

#include <dsd-neo/app_control/dmr_t3_view.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dmr_t3_view_internal.h"

_Static_assert(sizeof(((dmr_t3_missing_ledger*)0)->missing) == DSD_UI_DMR_T3_SET_BYTES
                   && sizeof(((dmr_t3_missing_ledger*)0)->details)
                          == DSD_UI_DMR_T3_MISSING_DETAILS * sizeof(((dmr_t3_missing_ledger*)0)->details[0])
                   && (int)DMR_T3_OVERLAY_MAX_ROWS == (int)DSD_UI_DMR_T3_OVERLAY_MAX_ROWS,
               "the view's sizes must match the DMR module's");
_Static_assert((int)DMR_CHAN_ORIGIN_NONE == (int)DSD_UI_DMR_T3_ORIGIN_NONE
                   && (int)DMR_CHAN_ORIGIN_FILE == (int)DSD_UI_DMR_T3_ORIGIN_FILE
                   && (int)DMR_CHAN_ORIGIN_USER == (int)DSD_UI_DMR_T3_ORIGIN_USER
                   && (int)DMR_CHAN_ORIGIN_SITE == (int)DSD_UI_DMR_T3_ORIGIN_SITE
                   && (int)DMR_CHAN_ORIGIN_UNVERIFIED == (int)DSD_UI_DMR_T3_ORIGIN_UNVERIFIED
                   && (int)DMR_CHAN_ORIGIN_NEIGHBOUR == (int)DSD_UI_DMR_T3_ORIGIN_NEIGHBOUR
                   && (int)DMR_CHAN_ORIGIN_ESTIMATE == (int)DSD_UI_DMR_T3_ORIGIN_ESTIMATE,
               "the view's origins must be dmr_chan_origin_e's");
_Static_assert((int)DMR_T3_OVERLAY_SET == (int)DSD_UI_DMR_T3_OP_SET
                   && (int)DMR_T3_OVERLAY_HIDE == (int)DSD_UI_DMR_T3_OP_HIDE
                   && (int)DMR_T3_OVERLAY_RESTORE == (int)DSD_UI_DMR_T3_OP_RESTORE
                   && (long)DMR_T3_OVERLAY_MIN_HZ == (long)DSD_UI_DMR_T3_MIN_HZ
                   && (long)DMR_T3_OVERLAY_MAX_HZ == (long)DSD_UI_DMR_T3_MAX_HZ,
               "the view's operations and frequency bounds must be the DMR module's");

/* Both counters only move, so a value is never seen twice in a process. Written under the snapshot lock. */
static uint32_t g_target_epoch;
static uint64_t g_content_seq;

static void
dmr_t3_view_copy_missing(dsd_ui_dmr_t3_missing* dst, const dsd_state* src) {
    dmr_t3_missing_ledger ledger;
    dmr_t3_missing_copy(src, &ledger);
    DSD_MEMCPY(dst->missing, ledger.missing, sizeof(dst->missing));
    DSD_MEMCPY(dst->checked, ledger.checked, sizeof(dst->checked));
    DSD_MEMCPY(dst->reported, ledger.reported, sizeof(dst->reported));
    dst->revision = ledger.revision;
    dst->detail_count = ledger.detail_count <= DSD_UI_DMR_T3_MISSING_DETAILS ? ledger.detail_count : 0U;
    DSD_MEMSET(dst->details, 0, sizeof(dst->details));
    for (uint8_t i = 0; i < dst->detail_count; i++) {
        const dmr_t3_missing_detail* d = &ledger.details[i];
        dsd_ui_dmr_t3_missing_detail* out = &dst->details[i];
        out->lpcn = d->lpcn;
        out->slot = d->slot;
        out->group = d->group;
        out->data = d->data;
        out->unverified = d->unverified;
        out->caller = d->caller;
        out->recipient = d->recipient;
        out->last_heard_ms = d->last_heard_ms;
    }
}

static void
dmr_t3_view_copy_overlay(dsd_ui_dmr_t3_view* dst, const dsd_state* src) {
    dst->overlay_count = 0U;
    DSD_MEMSET(dst->overlay, 0, sizeof(dst->overlay));
    if (dmr_t3_overlay_count(src) == 0U) {
        return;
    }
    for (uint16_t l = 1U; l < (uint16_t)(DSD_UI_DMR_T3_ROWS - 1) && dst->overlay_count < DSD_UI_DMR_T3_OVERLAY_MAX_ROWS;
         l++) {
        long hz = 0;
        long base_hz = 0;
        const int op = dmr_t3_overlay_get(src, l, &hz, &base_hz);
        if (op == 0) {
            continue;
        }
        dsd_ui_dmr_t3_overlay_row* row = &dst->overlay[dst->overlay_count++];
        row->lpcn = l;
        row->hidden = op == (int)DMR_T3_OVERLAY_HIDE ? 1U : 0U;
        row->hz = row->hidden ? 0 : hz;
        row->base_hz = base_hz;
    }
}

/* The receiver is on a system that is not Tier III: the carrier's received identity names another protocol's system,
   or another DMR trunking system. A carrier that named nothing yet (a reset record) is none. */
static uint8_t
dmr_t3_view_other_system_here(const dsd_state* src) {
    const uint8_t family = src->rx_id.family;
    if (family == (uint8_t)DSD_RXID_FAMILY_NONE) {
        return 0U;
    }
    if (family != (uint8_t)DSD_RXID_FAMILY_DMR) {
        return 1U;
    }
    const uint8_t trunk = dsd_rxid_dmr_trunk_named(src);
    return trunk != (uint8_t)DSD_RXID_DMR_TRUNK_NONE && trunk != (uint8_t)DSD_RXID_DMR_TRUNK_TIER3 ? 1U : 0U;
}

/* The parked trunk-scan target: a new id is a new target_id. */
static void
dmr_t3_view_note_target(dsd_app_dmr_t3_view_mark* mark, const dsd_state* src) {
    if (mark->target_id != 0U && strncmp(mark->target, src->trunk_scan_active_id, sizeof(mark->target)) == 0) {
        return;
    }
    DSD_SNPRINTF(mark->target, sizeof(mark->target), "%s", src->trunk_scan_active_id);
    mark->target_id = ++g_target_epoch;
}

static int
dmr_t3_view_key_moved(const dsd_app_dmr_t3_view_mark* mark, const dsd_state* src, uint8_t present, uint32_t target_id,
                      uint32_t revision, uint32_t missing_revision) {
    return !mark->valid || mark->present != present || mark->instance != src->state_instance
           || mark->map_seq != src->trunk_chan_map_seq || mark->body_target_id != target_id
           || mark->revision != revision || mark->missing_revision != missing_revision;
}

int
dsd_app_dmr_t3_view_publish(dsd_ui_dmr_t3_view* dst, dsd_app_dmr_t3_view_mark* mark, const dsd_state* src) {
    if (dst == NULL || mark == NULL || src == NULL) {
        return 0;
    }
    dmr_t3_view_note_target(mark, src);
    /* The live state, on the decoder thread: a row another writer filled since the last reconcile leaves the ledger
       before it is shown. */
    dmr_t3_missing_sync(src);
    const uint8_t present = dmr_t3_chan_recorded(src) ? 1U : 0U;
    const uint32_t revision = dmr_t3_chan_revision(src);
    const uint32_t missing_revision = dmr_t3_missing_revision(src);
    const long cc_hz = dmr_t3_cc_confirmed_hz(src);
    const uint8_t other = dmr_t3_view_other_system_here(src);
    dst->present = present;
    dst->other_system_here = other;
    dst->tier3 = !other && dmr_t3_tier3_seen(src) ? 1U : 0U;
    dst->tier3_unchecked = !other && dmr_t3_tier3_unchecked_seen(src) ? 1U : 0U;
    dst->cc_hz = cc_hz;
    dst->cc_confirmed_here = cc_hz != 0 ? 1U : 0U;
    dst->state_id = src->state_instance;
    dst->target_id = mark->target_id;
    dst->revision = revision;
    dst->generation = dmr_t3_chan_generation(src);
    if (!dmr_t3_view_key_moved(mark, src, present, mark->target_id, revision, missing_revision)) {
        return 0;
    }
    if (present) {
        dmr_t3_view_copy_missing(&dst->missing, src);
        dmr_t3_chan_origins_copy(src, dst->origin, sizeof(dst->origin));
        dmr_t3_view_copy_overlay(dst, src);
        dmr_t3_chan_effective_copy(src, dst->channel_hz, sizeof(dst->channel_hz) / sizeof(dst->channel_hz[0]));
    } else {
        DSD_MEMSET(&dst->missing, 0, sizeof(dst->missing));
        DSD_MEMSET(dst->origin, 0, sizeof(dst->origin));
        DSD_MEMSET(dst->channel_hz, 0, sizeof(dst->channel_hz));
        dst->overlay_count = 0U;
        DSD_MEMSET(dst->overlay, 0, sizeof(dst->overlay));
    }
    dst->content_seq = ++g_content_seq;
    mark->valid = 1U;
    mark->present = present;
    mark->instance = src->state_instance;
    mark->map_seq = src->trunk_chan_map_seq;
    mark->body_target_id = mark->target_id;
    mark->revision = revision;
    mark->missing_revision = missing_revision;
    return 1;
}

int
dsd_app_dmr_t3_view_consume(dsd_ui_dmr_t3_view* dst, const dsd_ui_dmr_t3_view* src) {
    if (dst == NULL || src == NULL) {
        return 0;
    }
    if (dst->content_seq != src->content_seq) {
        DSD_MEMCPY(dst, src, sizeof(*dst));
        return 1;
    }
    dst->present = src->present;
    dst->other_system_here = src->other_system_here;
    dst->tier3 = src->tier3;
    dst->tier3_unchecked = src->tier3_unchecked;
    dst->cc_confirmed_here = src->cc_confirmed_here;
    dst->cc_hz = src->cc_hz;
    dst->state_id = src->state_id;
    dst->target_id = src->target_id;
    dst->revision = src->revision;
    dst->generation = src->generation;
    return 0;
}

void
dsd_app_dmr_t3_view_attach(dsd_state* snapshot, dsd_ui_dmr_t3_view* view) {
    if (snapshot == NULL) {
        return;
    }
    snapshot->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = view;
    snapshot->state_ext_cleanup[DSD_STATE_EXT_APP_DMR_T3_VIEW] = NULL;
}
