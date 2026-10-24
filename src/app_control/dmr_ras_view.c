// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/crc_policy_view.h>
#include <dsd-neo/app_control/dmr_ras_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

void
dsd_app_dmr_ras_view(const dsd_opts* opts, const dsd_state* state, dsd_app_dmr_ras* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    dsd_app_crc_policy_view(opts, state, &out->crc);
    if (!state) {
        return;
    }
    const dsd_received_identity* const id = &state->rx_id;
    out->detected = (id->family == (uint8_t)DSD_RXID_FAMILY_DMR && (id->valid & DSD_RXID_DMR_RAS) != 0U) ? 1U : 0U;
    out->needs_relaxed = (out->detected && !out->crc.effective) ? 1U : 0U;
}

const char*
dsd_app_dmr_ras_label(const dsd_app_dmr_ras* view) {
    if (!view || !view->detected) {
        return "";
    }
    return view->needs_relaxed ? "RAS (-F decodes)" : "RAS";
}
