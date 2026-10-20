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
    if ((DSD_SYNC_IS_DMR(protocol) || DSD_SYNC_IS_P25P2(protocol) || DSD_SYNC_IS_X2TDMA(protocol)) && slot >= 0
        && slot <= 1) {
        out->slot = (uint8_t)(slot + 1);
    }
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
    return a && b && a->known && b->known && (a->kind != b->kind || a->value != b->value);
}

dsd_receive_verdict
dsd_receive_status(const dsd_opts* opts, const dsd_state* state, int protocol, int slot) {
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, protocol, slot, &identity);
    return dsd_receive_policy_match(opts ? &opts->receive : NULL, &identity);
}

int
dsd_receive_allows(const dsd_opts* opts, const dsd_state* state, int protocol, int slot) {
    dsd_receive_verdict verdict = dsd_receive_status(opts, state, protocol, slot);
    return verdict == DSD_RECEIVE_OFF || verdict == DSD_RECEIVE_ALLOWED;
}

int
dsd_receive_current_allows(const dsd_opts* opts, const dsd_state* state, int slot) {
    int protocol = state ? (state->synctype == DSD_SYNC_NONE ? state->lastsynctype : state->synctype) : DSD_SYNC_NONE;
    if (state && state->receive_context_active) {
        protocol = state->receive_context_protocol;
    }
    return dsd_receive_allows(opts, state, protocol, slot);
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
