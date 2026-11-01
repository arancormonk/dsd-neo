// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/site_identity_view.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

/* The record form (site_identity_record.c) plus what only the running decoder knows: the EDACS ESK setting, a live
   setting the record never holds, and whether frame sync is on the record's protocol. */
void
dsd_app_site_identity_view(const dsd_state* state, dsd_app_site_identity* out) {
    if (out == NULL) {
        return;
    }
    if (state == NULL) {
        DSD_MEMSET(out, 0, sizeof(*out));
        return;
    }
    const dsd_received_identity* id = &state->rx_id;
    dsd_app_site_identity_view_record(id, out);
    if (id->family == DSD_RXID_FAMILY_EDACS && out->token_count > 0U
        && out->token_count < DSD_APP_SITE_IDENTITY_MAX_TOKENS) {
        dsd_app_site_identity_token* const token = &out->tokens[out->token_count++];
        DSD_SNPRINTF(token->text, sizeof(token->text), "%s", state->esk_mask == 0xA0 ? "ESK" : "No ESK");
        token->compact = 0U;
    }
    if (out->token_count == 0U && out->cc_freq_hz == 0U) {
        return;
    }
    out->live = (id->fresh != 0U && dsd_rxid_family_for_sync(state->synctype) == (dsd_rxid_family)id->family) ? 1U : 0U;
}
