// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The attribution rule for position reports that borrow their sender from a call
 * (<dsd-neo/core/position_attribution.h>).
 */
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

/* The two P25 phases are separate classes; every other protocol is its family. */
int
dsd_position_attribution_class(int protocol) {
    return dsd_sync_protocol_class(protocol);
}

/* The DMR sender kinds dsd_position_dmr_air() names, above the two slot bits. */
#define DMR_AIR_KIND_BS     1U
#define DMR_AIR_KIND_MS     2U
#define DMR_AIR_KIND_DIRECT 3U

/* The over-the-air slot (1/2) of a DMR burst of @p kind on the single-slot decoder's lane, 0 for none to prove (an
 * MS-sourced burst), DSD_POSITION_DMR_AIR_UNKNOWN while a repeater's lane is still Checking. */
static uint8_t
dmr_air_mono_slot(const dsd_state* state, uint8_t kind) {
    if (state->dmr_mono_rx_slot >= DSD_DMR_MONO_RX_TS1) {
        return (uint8_t)(state->dmr_mono_rx_slot - DSD_DMR_MONO_RX_TS1 + 1U);
    }
    if (kind == DMR_AIR_KIND_DIRECT) {
        return state->dmr_dm_sync_slot;
    }
    return kind == DMR_AIR_KIND_MS ? 0U : DSD_POSITION_DMR_AIR_UNKNOWN;
}

uint8_t
dsd_position_dmr_air(const dsd_state* state) {
    if (!state) {
        return DSD_POSITION_DMR_AIR_NONE;
    }
    int sync = state->synctype;
    if (!DSD_SYNC_IS_DMR(sync)) {
        sync = state->lastsynctype;
    }
    if (!DSD_SYNC_IS_DMR(sync)) {
        return DSD_POSITION_DMR_AIR_NONE;
    }
    uint8_t kind = DMR_AIR_KIND_MS;
    if (DSD_SYNC_IS_DMR_BS(sync)) {
        kind = DMR_AIR_KIND_BS;
    } else if (state->dmr_dm_sync_slot == 1U || state->dmr_dm_sync_slot == 2U) {
        kind = DMR_AIR_KIND_DIRECT;
    }
    uint8_t slot = 0U;
    if (state->dmr_mono_rx_slot != DSD_DMR_MONO_RX_NONE) {
        slot = dmr_air_mono_slot(state, kind);
        if (slot == DSD_POSITION_DMR_AIR_UNKNOWN) {
            return DSD_POSITION_DMR_AIR_UNKNOWN;
        }
    }
    return (uint8_t)((kind << 2) | slot);
}

int
dsd_position_dmr_air_same(uint8_t a, uint8_t b) {
    return a == b && a != DSD_POSITION_DMR_AIR_UNKNOWN;
}

/* The DMR burst identity evidence or a report of @p protocol carries: dsd_position_dmr_air() for DMR, else NONE. */
static uint8_t
attribution_air(const dsd_state* state, int protocol) {
    return DSD_SYNC_IS_DMR(protocol) ? dsd_position_dmr_air(state) : (uint8_t)DSD_POSITION_DMR_AIR_NONE;
}

int
dsd_position_attribution_call_from_slot(const dsd_state* state, uint8_t slot, dsd_position_attribution_call* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    dsd_call_snapshot call;
    if (!state || slot >= DSD_CALL_STATE_SLOT_COUNT || dsd_call_state_get(state, slot, &call) <= 0
        || call.phase != DSD_CALL_PHASE_ACTIVE || call.epoch == 0U) {
        return 0;
    }
    out->epoch = call.epoch;
    out->transmission = dsd_call_snapshot_transmission(&call);
    out->source = call.ota_source_id;
    out->target = call.ota_target_id;
    out->protocol = call.protocol;
    out->open = 1U;
    return 1;
}

int
dsd_position_attribution_call_from_closing(const struct dsd_event_closing_call* closing,
                                           dsd_position_attribution_call* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!closing || !closing->valid || closing->epoch == 0U) {
        return 0;
    }
    out->epoch = closing->epoch;
    out->source = closing->source_id;
    out->target = closing->target_id;
    out->protocol = closing->protocol;
    out->open = 1U;
    return 1;
}

int
dsd_position_attribution_learn(const dsd_state* state, const dsd_position_attribution_call* call, uint32_t source,
                               uint32_t target, int group, dsd_position_attribution_bind bind,
                               dsd_position_attribution* out) {
    if (!state || !call || !out || !call->open || call->epoch == 0U || source == 0U
        || call->source != (uint64_t)source) {
        return 0;
    }
    if (bind == DSD_POSITION_ATTRIBUTION_BIND_TRANSMISSION && call->transmission == 0U) {
        return 0;
    }
    const int names_target = target != 0U && call->target == (uint64_t)target;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->call_epoch = call->epoch;
    out->transmission = bind == DSD_POSITION_ATTRIBUTION_BIND_TRANSMISSION ? call->transmission : 0U;
    out->source = source;
    out->target = names_target ? target : 0U;
    out->group = (uint8_t)(names_target && group != 0);
    out->rx_gen = dsd_state_reception_generation(state);
    out->protocol = call->protocol;
    out->air = attribution_air(state, call->protocol);
    return 1;
}

/* The evidence was learned in @p call: its transmission when bound to one, else its epoch. */
static int
attribution_same_call(const dsd_position_attribution* a, const dsd_position_attribution_call* call) {
    if (a->transmission != 0U) {
        return call->transmission == a->transmission;
    }
    return call->epoch == a->call_epoch;
}

/* The evidence was learned on the reception the receiver is on now, by the reception rule every position writer
 * applies (dsd_position_same_reception()): the evidence keeps no scope, so only the same reception generation, one
 * uninterrupted reception with no retune, epoch move or change of P25 phase in between, matches. */
static int
attribution_same_reception(const dsd_state* state, const dsd_position_attribution* a) {
    dsd_position_reception learned;
    dsd_position_reception now;
    DSD_MEMSET(&learned, 0, sizeof learned);
    DSD_MEMSET(&now, 0, sizeof now);
    learned.rx_gen = a->rx_gen;
    learned.epoch_known = 1U;
    now.rx_gen = dsd_state_reception_generation(state);
    now.epoch_known = 1U;
    return dsd_position_same_reception(&learned, &now);
}

/* Every condition of the rule but the identity: evidence held, the call open, one protocol class for the call, the
 * evidence and the report, the same call, the same reception and, for DMR, the same burst identity (sender kind and
 * over-the-air slot) as the burst carrying the report. */
static int
attribution_binds(const dsd_state* state, const dsd_position_attribution* a, const dsd_position_attribution_call* call,
                  int report_protocol) {
    if (a->call_epoch == 0U || a->source == 0U || !call->open || call->epoch == 0U) {
        return 0;
    }
    const int report_class = dsd_position_attribution_class(report_protocol);
    if (dsd_position_attribution_class(call->protocol) != report_class
        || dsd_position_attribution_class(a->protocol) != report_class) {
        return 0;
    }
    if (DSD_SYNC_IS_DMR(report_protocol)
        && !dsd_position_dmr_air_same(a->air, attribution_air(state, report_protocol))) {
        return 0;
    }
    return attribution_same_call(a, call) && attribution_same_reception(state, a);
}

int
dsd_position_attribution_grant(const dsd_state* state, const dsd_position_attribution* a,
                               const dsd_position_attribution_call* call, int report_protocol,
                               dsd_position_attribution* granted) {
    if (granted) {
        DSD_MEMSET(granted, 0, sizeof(*granted));
    }
    if (!state || !a || !call || !attribution_binds(state, a, call, report_protocol)
        || call->source != (uint64_t)a->source) {
        return 0;
    }
    if (granted) {
        granted->call_epoch = call->epoch;
        granted->transmission = a->transmission;
        granted->source = a->source;
        granted->rx_gen = a->rx_gen;
        granted->protocol = call->protocol;
        granted->air = a->air;
        if (a->target != 0U && call->target == (uint64_t)a->target) {
            granted->target = a->target;
            granted->group = a->group;
        }
    }
    return 1;
}
