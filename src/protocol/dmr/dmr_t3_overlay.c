// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The user's Tier III channels over the channel map (<dsd-neo/protocol/dmr/dmr_t3_channels.h>, discussion #900 F3):
 * the edits a frontend sends (DSD_APP_CMD_DMR_CHANNEL_SET), the --dmr-chan-overrides file, and the re-apply after the
 * map underneath changed. The overlay lives in the DMR state extension beside the origins (dmr_t3_channels_internal.h).
 */

#include <dsd-neo/core/dmr_chan_overrides_csv.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include "dmr_call_channel.h"
#include "dmr_t3_channels_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
dmr_t3_overlay_args_valid(uint16_t lpcn, dmr_t3_overlay_op op, long hz) {
    if (!dmr_t3_lpcn_is_channel(lpcn)) {
        return 0;
    }
    if (op == DMR_T3_OVERLAY_SET) {
        return hz >= (long)DMR_T3_OVERLAY_MIN_HZ && hz <= (long)DMR_T3_OVERLAY_MAX_HZ;
    }
    return op == DMR_T3_OVERLAY_HIDE || op == DMR_T3_OVERLAY_RESTORE;
}

/* Whether the row is still the one the editor showed: its frequency, its origin, and the network it belonged to. */
static int
dmr_t3_overlay_expected(const dsd_state* state, uint16_t lpcn, long expected_hz, uint8_t expected_origin,
                        uint32_t generation) {
    return generation == dmr_t3_chan_generation(state) && expected_hz == state->trunk_chan_map[lpcn]
           && expected_origin == (uint8_t)dmr_t3_chan_origin(state, lpcn);
}

/* SET or HIDE @p l; returns whether anything changed. A new entry keeps the row it replaces. */
static int
dmr_t3_overlay_put(dsd_state* state, dmr_ext_t* ext, uint16_t l, dmr_t3_overlay_op op, long hz) {
    dmr_t3_overlay* o = &ext->overlay;
    const long want_hz = op == DMR_T3_OVERLAY_SET ? hz : 0;
    if (o->op[l] == (uint8_t)op && o->hz[l] == want_hz) {
        return 0;
    }
    if (o->op[l] == 0U) {
        dmr_t3_overlay_take_base(state, ext, l);
        o->count++;
    }
    o->op[l] = (uint8_t)op;
    o->hz[l] = want_hz;
    dmr_t3_overlay_write_live(state, ext, l);
    return 1;
}

/* Drop @p l's entry and put back the row it replaced; returns whether there was one. */
static int
dmr_t3_overlay_drop(dsd_state* state, dmr_ext_t* ext, uint16_t l) {
    dmr_t3_overlay* o = &ext->overlay;
    if (o->op[l] == 0U) {
        return 0;
    }
    dsd_state_set_trunk_chan_freq(state, l, o->base_hz[l]);
    ext->origin[l] = o->base_origin[l];
    /* A row the air taught is the air's again (dmr_t3_overlay_take_base() kept the origin it read as). */
    dsd_state_dmr_lcn_ota_set(state, l, o->base_hz[l] != 0 && dmr_t3_chan_origin_is_ota(o->base_origin[l]));
    state->dmr_lcn_trust[l] = o->base_trust[l];
    o->op[l] = 0U;
    o->hz[l] = 0;
    o->base_hz[l] = 0;
    o->base_origin[l] = 0U;
    o->base_trust[l] = 0U;
    o->count--;
    return 1;
}

dmr_t3_set_result
dmr_t3_overlay_apply(dsd_opts* opts, dsd_state* state, uint16_t lpcn, dmr_t3_overlay_op op, long hz, long expected_hz,
                     uint8_t expected_origin, uint32_t generation, int check) {
    if (state == NULL || !dmr_t3_overlay_args_valid(lpcn, op, hz)) {
        return DMR_T3_SET_INVALID;
    }
    if (opts != NULL && opts->trunk_scan_enabled == 1) {
        return DMR_T3_SET_TRUNK_SCAN;
    }
    if (check && !dmr_t3_overlay_expected(state, lpcn, expected_hz, expected_origin, generation)) {
        return DMR_T3_SET_STALE;
    }
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return DMR_T3_SET_INVALID; /* out of memory: nothing could hold the entry */
    }
    const int changed = op == DMR_T3_OVERLAY_RESTORE ? dmr_t3_overlay_drop(state, ext, lpcn)
                                                     : dmr_t3_overlay_put(state, ext, lpcn, op, hz);
    if (changed) {
        ext->revision++;
        dmr_t3_missing_reconcile(state);
        dmr_t3_chan_anchors_changed(opts, state, ext);
    }
    return DMR_T3_SET_OK;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    int applied;
} dmr_t3_overlay_load_ctx;

static int
dmr_t3_overlay_load_row(uint16_t lpcn, long hz, void* ctx) {
    dmr_t3_overlay_load_ctx* c = (dmr_t3_overlay_load_ctx*)ctx;
    const dmr_t3_overlay_op op = hz == 0 ? DMR_T3_OVERLAY_HIDE : DMR_T3_OVERLAY_SET;
    if (dmr_t3_overlay_apply(c->opts, c->state, lpcn, op, hz, 0, 0U, 0U, 0) != DMR_T3_SET_OK) {
        return -1;
    }
    c->applied++;
    return 0;
}

int
dmr_t3_overlay_load_csv(dsd_opts* opts, dsd_state* state, const char* path) {
    if (state == NULL || path == NULL || path[0] == '\0') {
        return -1;
    }
    if (opts != NULL && opts->trunk_scan_enabled == 1) {
        LOG_ERROR("--dmr-chan-overrides cannot be combined with --trunk-scan\n");
        return -1;
    }
    dmr_t3_overlay_load_ctx ctx = {opts, state, 0};
    if (dsd_csv_dmr_chan_overrides_read(path, dmr_t3_overlay_load_row, &ctx) < 0) {
        return -1;
    }
    LOG_INFO("NOTICE: Applied %d DMR Tier III channel entr%s from %s\n", ctx.applied, ctx.applied == 1 ? "y" : "ies",
             path);
    return ctx.applied;
}
