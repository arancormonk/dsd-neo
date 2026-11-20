// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The NXDN data call context: a data call's header, its packet assembly, crypto and LRRP identities. A DISC and a
 * finished data call drop it (nxdn_element.c), and so does a control-channel hunt tune (nxdn_cc_hunt.c).
 *
 * The header's binding (issue #800 B6): a payload is attributed to the checked header that opened its assembly, never
 * to an earlier one. The binding holds the reception and carrier the header was heard on and when; another reception
 * (a retune, a RAN or system change), another carrier, or NXDN_DATA_HEADER_HOLD_MS on the decode clock ends it, and
 * so do a rejected header (nxdn_element.c) and a message lost on any channel that can carry a header (nxdn_deperm.c):
 * a failed FACCH2/UDCH, FACCH3/UDCH2 or CAC CRC, or a FACCH1 none of whose copies passed. A SACCH cannot hold a
 * header (26 or 72 bits against 79 and 88), so its failure ends nothing.
 */

#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_internal.h"

void
nxdn_reset_data_call_state(dsd_state* state) {
    if (state == NULL) {
        return;
    }

    DSD_MEMSET(state->dmr_pdu_sf[0], 0, sizeof(state->dmr_pdu_sf[0]));
    state->data_header_blocks[0] = 1;
    state->data_header_padding[0] = 0;
    state->data_header_format[0] = 0;
    state->data_header_valid[0] = 0;

    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_mi = 0;
    DSD_MEMSET(state->aes_ivR, 0, sizeof(state->aes_ivR));

    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_target[0] = 0;
    nxdn_data_call_drop_header(state);
}

void
nxdn_data_call_bind_header(const dsd_opts* opts, dsd_state* state) {
    if (state == NULL) {
        return;
    }
    dsd_position_origin_capture(opts, state, &state->nxdn_data_origin);
    state->nxdn_data_header_ms = dsd_decode_now_mono_ms();
    state->nxdn_data_carrier_seq = state->carrier_seq;
}

int
nxdn_data_call_header_bound(const dsd_state* state) {
    if (state == NULL || state->data_header_valid[0] == 0U || state->nxdn_data_origin.valid == 0U) {
        return 0;
    }
    if (state->carrier_seq != state->nxdn_data_carrier_seq
        || !dsd_position_origin_matches(state, &state->nxdn_data_origin)) {
        return 0;
    }
    /* A clock that went back since the header ends the binding as its expiry does. */
    const uint64_t now_ms = dsd_decode_now_mono_ms();
    return now_ms >= state->nxdn_data_header_ms && now_ms - state->nxdn_data_header_ms <= NXDN_DATA_HEADER_HOLD_MS;
}

void
nxdn_data_call_drop_header(dsd_state* state) {
    if (state == NULL) {
        return;
    }
    state->data_header_valid[0] = 0U;
    DSD_MEMSET(&state->nxdn_data_origin, 0, sizeof(state->nxdn_data_origin));
    state->nxdn_data_header_ms = 0U;
    state->nxdn_data_carrier_seq = 0U;
}
