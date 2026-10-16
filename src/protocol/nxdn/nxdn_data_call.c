// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * The NXDN data call context: a data call's header, its packet assembly, crypto and LRRP identities. A DISC and a
 * finished data call drop it (nxdn_element.c), and so does a control-channel hunt tune (nxdn_cc_hunt.c).
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <stddef.h>
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
}
