// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(DSD_SERVICE_LABEL_SIZE == DSD_CHANNEL_LABEL_SIZE, "service and channel labels have equal capacity");

static void
receive_analog_identity(const dsd_state* state, dsd_receive_identity* out) {
    if (state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_NONE) {
        out->known = 1; /* Confirmed untoned reception is a service identity too. */
    } else if (state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
        if (state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
            out->kind = DSD_RECEIVE_CTCSS;
            out->known = 1;
            out->value = (uint16_t)state->analog_rx.ctcss_tenths_hz;
        } else if (state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
            int code = 0, inverted = 0;
            if (dsd_dcs_canonical(state->analog_rx.dcs_code, state->analog_rx.dcs_inverted, &code, &inverted) == 0) {
                out->kind = DSD_RECEIVE_DCS;
                out->known = 1;
                out->value = (uint16_t)(code * 2 + inverted);
            }
        }
    }
}

/* The over-the-air slot (1/2) of a two-slot protocol's lane @p slot (0/1); none for FDMA and analog. */
static void
receive_identity_slot(const dsd_state* state, int protocol, int slot, dsd_receive_identity* out) {
    if (DSD_SYNC_IS_DMR(protocol) && state->dmr_mono_rx_slot != DSD_DMR_MONO_RX_NONE) {
        /* The single-slot decoder carries one over-the-air slot on its one lane: the slot its CACH proved. */
        if (state->dmr_mono_rx_slot >= DSD_DMR_MONO_RX_TS1) {
            out->slot = (uint8_t)(state->dmr_mono_rx_slot - DSD_DMR_MONO_RX_TS1 + 1);
        } else {
            out->slot_pending = 1;
        }
    } else if ((DSD_SYNC_IS_DMR(protocol) || DSD_SYNC_IS_P25P2(protocol) || DSD_SYNC_IS_X2TDMA(protocol)) && slot >= 0
               && slot <= 1) {
        out->slot = (uint8_t)(slot + 1);
    }
}

void
dsd_receive_identity_current(const dsd_state* state, int protocol, int slot, dsd_receive_identity* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->kind = DSD_RECEIVE_NONE;
    if (!state) {
        return;
    }
    receive_identity_slot(state, protocol, slot, out);
    if (DSD_SYNC_IS_DMR(protocol)) {
        out->kind = DSD_RECEIVE_DCC;
    } else if (DSD_SYNC_IS_P25(protocol)) {
        out->kind = DSD_RECEIVE_NAC;
    } else if (DSD_SYNC_IS_NXDN(protocol)) {
        out->kind = DSD_RECEIVE_RAN;
    }
    if (out->kind != DSD_RECEIVE_NONE) {
        out->known = (uint8_t)dsd_access_code_current(state, protocol, 0, 0, NULL, &out->value);
    } else if (protocol == DSD_SYNC_ANALOG && state->analog_rx.carrier_open) {
        receive_analog_identity(state, out);
    }
}

int
dsd_receive_identity_changed(const dsd_receive_identity* a, const dsd_receive_identity* b) {
    if (!a || !b) {
        return 0;
    }
    if (a->slot != 0U && b->slot != 0U && a->slot != b->slot) {
        return 1;
    }
    return a->known && b->known && (a->kind != b->kind || a->value != b->value);
}

void
dsd_receive_identity_merge(dsd_receive_identity* held, const dsd_receive_identity* received) {
    if (!held || !received) {
        return;
    }
    if (received->known) {
        held->kind = received->kind;
        held->value = received->value;
        held->known = 1;
    }
    if (received->slot != 0U) {
        held->slot = received->slot;
        held->slot_pending = 0;
    } else if (held->slot == 0U) {
        held->slot_pending = received->slot_pending;
    }
}

dsd_receive_verdict
dsd_receive_status(const dsd_opts* opts, const dsd_state* state, int protocol, int slot) {
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, protocol, slot, &identity);
    return dsd_receive_policy_match(opts ? &opts->receive : NULL, &identity);
}

int
dsd_receive_identity_admitted(const dsd_opts* opts, const dsd_receive_identity* identity) {
    if (!dsd_receive_filters_enabled(opts)) {
        return 1;
    }
    const dsd_receive_verdict verdict = dsd_receive_policy_match(&opts->receive, identity);
    return verdict == DSD_RECEIVE_OFF || verdict == DSD_RECEIVE_ALLOWED;
}

int
dsd_receive_carrier_allows(const dsd_opts* opts, const dsd_state* state) {
    if (!dsd_receive_filters_enabled(opts) || !state) {
        return 1;
    }
    int protocol = state->synctype == DSD_SYNC_NONE ? state->lastsynctype : state->synctype;
    if (state->receive_context_active) {
        protocol = state->receive_context_protocol;
    }
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, protocol, -1, &identity);
    if (!(DSD_SYNC_IS_DMR(protocol) || DSD_SYNC_IS_P25P2(protocol) || DSD_SYNC_IS_X2TDMA(protocol))) {
        const dsd_receive_verdict verdict = dsd_receive_policy_match(&opts->receive, &identity);
        return verdict == DSD_RECEIVE_OFF || verdict == DSD_RECEIVE_ALLOWED;
    }
    /* Each over-the-air slot on its own, whichever the single-slot decoder's lane has proven for its voice: a data
       burst on the other slot is that slot's traffic. */
    identity.slot_pending = 0;
    for (uint8_t slot = 1; slot <= 2; slot++) {
        identity.slot = slot;
        const dsd_receive_verdict verdict = dsd_receive_policy_match(&opts->receive, &identity);
        if (verdict != DSD_RECEIVE_OFF && verdict != DSD_RECEIVE_ALLOWED) {
            return 0;
        }
    }
    return 1;
}

int
dsd_receive_filters_enabled(const dsd_opts* opts) {
    return opts && dsd_receive_policy_filters_enabled(&opts->receive);
}

int
dsd_receive_allows(const dsd_opts* opts, const dsd_state* state, int protocol, int slot) {
    /* The default policy (every filter off) admits everything: answer before reading the identity, since decode and
       output paths ask on every frame. */
    if (!dsd_receive_filters_enabled(opts)) {
        return 1;
    }
    dsd_receive_verdict verdict = dsd_receive_status(opts, state, protocol, slot);
    return verdict == DSD_RECEIVE_OFF || verdict == DSD_RECEIVE_ALLOWED;
}

dsd_receive_context
dsd_receive_context_begin(dsd_state* state, int protocol) {
    dsd_receive_context saved = {0, 0};
    if (!state) {
        return saved;
    }
    saved.protocol = state->receive_context_protocol;
    saved.active = state->receive_context_active;
    state->receive_context_protocol = protocol;
    state->receive_context_active = 1;
    return saved;
}

void
dsd_receive_context_end(dsd_state* state, dsd_receive_context saved) {
    if (state) {
        state->receive_context_protocol = saved.protocol;
        state->receive_context_active = saved.active;
    }
}

int
dsd_receive_current_allows(const dsd_opts* opts, const dsd_state* state, int slot) {
    int protocol = state ? (state->synctype == DSD_SYNC_NONE ? state->lastsynctype : state->synctype) : DSD_SYNC_NONE;
    if (state && state->receive_context_active) {
        protocol = state->receive_context_protocol;
    }
    return dsd_receive_allows(opts, state, protocol, slot);
}

void
dsd_receive_carriers_note(dsd_receive_carriers* carriers, const dsd_state* state, int protocol, long freq_hz) {
    if (!carriers || freq_hz <= 0) {
        return;
    }
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, protocol, -1, &identity);
    if (!identity.known || identity.kind == DSD_RECEIVE_SLOT || identity.kind >= DSD_RECEIVE_FILTER_COUNT) {
        return;
    }
    dsd_receive_carrier* slot = NULL;
    for (unsigned int i = 0; i < DSD_RECEIVE_CARRIER_MEMORY; i++) {
        if (carriers->entries[i].freq_hz == freq_hz) {
            slot = &carriers->entries[i];
            break;
        }
    }
    if (!slot) {
        slot = &carriers->entries[carriers->next % DSD_RECEIVE_CARRIER_MEMORY];
        carriers->next = (carriers->next + 1U) % DSD_RECEIVE_CARRIER_MEMORY;
    }
    slot->freq_hz = freq_hz;
    slot->kind = identity.kind;
    slot->value = identity.value;
}

int
dsd_receive_carriers_rejects(const dsd_receive_carriers* carriers, const dsd_opts* opts, long freq_hz) {
    if (!carriers || !opts || freq_hz <= 0) {
        return 0;
    }
    for (unsigned int i = 0; i < DSD_RECEIVE_CARRIER_MEMORY; i++) {
        const dsd_receive_carrier* entry = &carriers->entries[i];
        if (entry->freq_hz == freq_hz) {
            /* The code alone: the grant's slot is judged by dsd_receive_grant_allows(). */
            const dsd_receive_identity identity = {.value = entry->value, .kind = entry->kind, .known = 1, .slot = 0};
            return dsd_receive_policy_match(&opts->receive, &identity) == DSD_RECEIVE_REJECTED;
        }
    }
    return 0;
}

int
dsd_receive_grant_allows(const dsd_opts* opts, int protocol, int slot) {
    if (!opts || !(DSD_SYNC_IS_DMR(protocol) || DSD_SYNC_IS_P25P2(protocol) || DSD_SYNC_IS_X2TDMA(protocol))) {
        return 1;
    }
    if (slot < 0 || slot > 1) {
        return dsd_receive_grant_allows(opts, protocol, 0) || dsd_receive_grant_allows(opts, protocol, 1);
    }
    const dsd_receive_identity identity = {.kind = DSD_RECEIVE_NONE, .slot = (uint8_t)(slot + 1)};
    return dsd_receive_policy_match(&opts->receive, &identity) != DSD_RECEIVE_REJECTED;
}
