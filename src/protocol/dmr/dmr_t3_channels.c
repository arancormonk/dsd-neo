// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR Tier III channel provenance (<dsd-neo/protocol/dmr/dmr_t3_channels.h>): the origin record of each channel map
 * row, the map import and clear notes, the network change, and the per-target save and restore. Also owns the DMR
 * state extension (dmr_t3_channels_internal.h) that the RC notice dedupe shares. The over-the-air writer is
 * dmr_t3_chan_learn.c; nothing here posts an event, so the engine and app-control links stay free of the emitter.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
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

dmr_chan_origin_e
dmr_t3_chan_origin(const dsd_state* state, uint16_t lpcn) {
    if (state == NULL || lpcn >= DMR_T3_ROWS || state->trunk_chan_map[lpcn] == 0) {
        return DMR_CHAN_ORIGIN_NONE;
    }
    const dmr_ext_t* ext = dmr_ext_get(state);
    const uint8_t origin = ext != NULL ? ext->origin[lpcn] : (uint8_t)DMR_CHAN_ORIGIN_NONE;
    return origin == (uint8_t)DMR_CHAN_ORIGIN_NONE ? DMR_CHAN_ORIGIN_FILE : (dmr_chan_origin_e)origin;
}

int
dmr_t3_chan_drop_rows(dsd_state* state, dmr_ext_t* ext, int (*drop)(unsigned origin)) {
    int dropped = 0;
    for (uint16_t l = 1U; l <= DMR_T3_MAX_LCN; l++) {
        if (!drop(ext->origin[l])) {
            continue;
        }
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
    for (uint16_t l = 0U; l < DMR_T3_ROWS; l++) {
        const int file_row = dmr_t3_lpcn_is_channel(l) && state->trunk_chan_map[l] != 0;
        ext->origin[l] = (uint8_t)(file_row ? DMR_CHAN_ORIGIN_FILE : DMR_CHAN_ORIGIN_NONE);
    }
    ext->revision++;
}

void
dmr_t3_chan_note_map_cleared(dsd_state* state) {
    dmr_ext_t* ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMSET(ext->origin, 0, sizeof(ext->origin));
    ext->revision++;
}

/* What the air taught goes with the network it came from; the next checked system code starts a new baseline. */
static void
dmr_t3_chan_drop_network(dsd_state* state, dmr_ext_t* ext) {
    (void)dmr_t3_chan_drop_rows(state, ext, dmr_t3_chan_origin_is_ota);
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

uint32_t
dmr_t3_chan_revision(const dsd_state* state) {
    const dmr_ext_t* ext = dmr_ext_get(state);
    return ext != NULL ? ext->revision : 0U;
}

void
dmr_t3_chan_save(const dsd_state* state, dmr_t3_chan_target_snapshot* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    const dmr_ext_t* ext = dmr_ext_get(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMCPY(out->origin, ext->origin, sizeof(out->origin));
    out->generation = ext->generation;
    out->revision = ext->revision;
    out->syscode = ext->syscode;
    out->syscode_valid = ext->syscode_valid;
}

void
dmr_t3_chan_restore(dsd_state* state, const dmr_t3_chan_target_snapshot* in) {
    if (state == NULL || in == NULL) {
        return;
    }
    dmr_ext_t* ext = dmr_ext_get(state);
    /* A snapshot with no revision recorded nothing (every change bumps it): with no record here either, none is due. */
    if (ext == NULL && in->revision == 0U && in->generation == 0U && in->syscode_valid == 0U) {
        return;
    }
    ext = dmr_ext_get_or_create(state);
    if (ext == NULL) {
        return;
    }
    DSD_MEMCPY(ext->origin, in->origin, sizeof(ext->origin));
    ext->generation = in->generation;
    ext->revision = (ext->revision > in->revision ? ext->revision : in->revision) + 1U;
    ext->syscode = in->syscode;
    ext->syscode_valid = in->syscode_valid;
}
