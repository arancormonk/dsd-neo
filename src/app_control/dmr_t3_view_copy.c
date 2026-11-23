// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The DMR Tier III channel record's two snapshot hops (dmr_t3_view_internal.h). The body is two parts, each with its
 * own key: the ledger (the state instance, the parked trunk-scan target and the ledger's revision) and the channel rows
 * (origins, frequencies, overlay rows: the instance, the target, the channel revision and the channel map's sequence).
 * A grant heard again moves the ledger alone, so only the ledger is copied. A restarted session, or another target
 * whose revisions happen to match, is another key, so no copy keeps a record it does not hold.
 */

#include <dsd-neo/app_control/dmr_t3_view.h>
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
        const int op = dmr_t3_overlay_get(src, l, &hz);
        if (op == 0) {
            continue;
        }
        dsd_ui_dmr_t3_overlay_row* row = &dst->overlay[dst->overlay_count++];
        row->lpcn = l;
        row->hidden = op == (int)DMR_T3_OVERLAY_HIDE ? 1U : 0U;
        row->hz = row->hidden ? 0 : hz;
        row->base_hz = src->trunk_chan_map[l]; /* the shared row: what RESTORE shows */
    }
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
dmr_t3_view_key_moved(const dsd_app_dmr_t3_view_key* key, const dsd_app_dmr_t3_view_key* now) {
    return !key->valid || key->present != now->present || key->instance != now->instance
           || key->target_id != now->target_id || key->revision != now->revision || key->map_seq != now->map_seq;
}

static void
dmr_t3_view_copy_ledger(dsd_ui_dmr_t3_view* dst, const dsd_state* src, uint8_t present) {
    if (present) {
        dmr_t3_view_copy_missing(&dst->missing, src);
    } else {
        DSD_MEMSET(&dst->missing, 0, sizeof(dst->missing));
    }
    dst->missing_seq = ++g_content_seq;
}

static void
dmr_t3_view_copy_rows(dsd_ui_dmr_t3_view* dst, const dsd_state* src, uint8_t present) {
    if (present) {
        dmr_t3_chan_origins_copy(src, dst->origin, sizeof(dst->origin));
        dmr_t3_view_copy_overlay(dst, src);
        dmr_t3_chan_effective_copy(src, dst->channel_hz, sizeof(dst->channel_hz) / sizeof(dst->channel_hz[0]));
    } else {
        DSD_MEMSET(dst->origin, 0, sizeof(dst->origin));
        DSD_MEMSET(dst->channel_hz, 0, sizeof(dst->channel_hz));
        dst->overlay_count = 0U;
        DSD_MEMSET(dst->overlay, 0, sizeof(dst->overlay));
    }
    dst->content_seq = ++g_content_seq;
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
    const long cc_hz = dmr_t3_cc_confirmed_hz(src);
    const uint8_t other = dsd_app_dmr_t3_other_system_here(src);
    dst->present = present;
    dst->dmr_decoding = dsd_app_dmr_t3_dmr_decoding_noted();
    dst->other_system_here = other;
    dst->overrides_applied = dmr_t3_overrides_applied(src) ? 1U : 0U;
    dst->tier3 = !other && dmr_t3_tier3_seen(src) ? 1U : 0U;
    dst->tier3_unchecked = !other && dmr_t3_tier3_unchecked_seen(src) ? 1U : 0U;
    dst->cc_hz = cc_hz;
    dst->cc_confirmed_here = cc_hz != 0 ? 1U : 0U;
    dst->state_id = src->state_instance;
    dst->target_id = mark->target_id;
    dst->revision = revision;
    dst->generation = dmr_t3_chan_generation(src);
    const dsd_app_dmr_t3_view_key missing = {
        .valid = 1U,
        .present = present,
        .instance = src->state_instance,
        .target_id = mark->target_id,
        .revision = dmr_t3_missing_revision(src),
    };
    const dsd_app_dmr_t3_view_key rows = {
        .valid = 1U,
        .present = present,
        .instance = src->state_instance,
        .target_id = mark->target_id,
        .revision = revision,
        .map_seq = src->trunk_chan_map_seq,
    };
    int copied = 0;
    if (dmr_t3_view_key_moved(&mark->missing, &missing)) {
        dmr_t3_view_copy_ledger(dst, src, present);
        mark->missing = missing;
        copied |= DSD_APP_DMR_T3_VIEW_COPIED_MISSING;
    }
    if (dmr_t3_view_key_moved(&mark->rows, &rows)) {
        dmr_t3_view_copy_rows(dst, src, present);
        mark->rows = rows;
        copied |= DSD_APP_DMR_T3_VIEW_COPIED_ROWS;
    }
    return copied;
}

/* The scalars every publish sets. */
static void
dmr_t3_view_copy_scalars(dsd_ui_dmr_t3_view* dst, const dsd_ui_dmr_t3_view* src) {
    dst->present = src->present;
    dst->dmr_decoding = src->dmr_decoding;
    dst->other_system_here = src->other_system_here;
    dst->overrides_applied = src->overrides_applied;
    dst->tier3 = src->tier3;
    dst->tier3_unchecked = src->tier3_unchecked;
    dst->cc_confirmed_here = src->cc_confirmed_here;
    dst->cc_hz = src->cc_hz;
    dst->state_id = src->state_id;
    dst->target_id = src->target_id;
    dst->revision = src->revision;
    dst->generation = src->generation;
}

int
dsd_app_dmr_t3_view_consume(dsd_ui_dmr_t3_view* dst, const dsd_ui_dmr_t3_view* src) {
    if (dst == NULL || src == NULL) {
        return 0;
    }
    dmr_t3_view_copy_scalars(dst, src);
    int copied = 0;
    if (dst->missing_seq != src->missing_seq) {
        DSD_MEMCPY(&dst->missing, &src->missing, sizeof(dst->missing));
        dst->missing_seq = src->missing_seq;
        copied |= DSD_APP_DMR_T3_VIEW_COPIED_MISSING;
    }
    if (dst->content_seq != src->content_seq) {
        DSD_MEMCPY(dst->origin, src->origin, sizeof(dst->origin));
        dst->overlay_count = src->overlay_count;
        DSD_MEMCPY(dst->overlay, src->overlay, sizeof(dst->overlay));
        DSD_MEMCPY(dst->channel_hz, src->channel_hz, sizeof(dst->channel_hz));
        dst->content_seq = src->content_seq;
        copied |= DSD_APP_DMR_T3_VIEW_COPIED_ROWS;
    }
    return copied;
}

void
dsd_app_dmr_t3_view_attach(dsd_state* snapshot, dsd_ui_dmr_t3_view* view) {
    if (snapshot == NULL) {
        return;
    }
    snapshot->state_ext[DSD_STATE_EXT_APP_DMR_T3_VIEW] = view;
    snapshot->state_ext_cleanup[DSD_STATE_EXT_APP_DMR_T3_VIEW] = NULL;
}
