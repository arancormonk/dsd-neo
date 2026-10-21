// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/receive_policy.h>

static const char*
receive_status_name(dsd_receive_verdict verdict) {
    switch (verdict) {
        case DSD_RECEIVE_CHECKING: return "Checking received identifier";
        case DSD_RECEIVE_REJECTED: return "Rejected by receive filter";
        case DSD_RECEIVE_ALLOWED: return "Allowed";
        default: return "Filters off";
    }
}

void
dsd_app_receive_status_text(const dsd_opts* opts, const dsd_state* state, char* out, size_t capacity) {
    if (!out || !capacity) {
        return;
    }
    out[0] = '\0';
    if (!opts || !state) {
        return;
    }
    int protocol = state->synctype == DSD_SYNC_NONE ? state->lastsynctype : state->synctype;
    if (opts->analog_only) {
        protocol = DSD_SYNC_ANALOG;
    }
    if (protocol == DSD_SYNC_NONE && dsd_receive_policy_filters_enabled(&opts->receive)) {
        DSD_SNPRINTF(out, capacity, "%s", "Checking received identifier");
        return;
    }
    dsd_receive_identity identity;
    dsd_receive_identity_current(state, protocol, 0, &identity);
    const char* first = receive_status_name(dsd_receive_status(opts, state, protocol, 0));
    if (DSD_SYNC_IS_DMR(protocol) && state->dmr_mono_rx_slot != DSD_DMR_MONO_RX_NONE) {
        /* The single-slot decoder has one lane, carrying the over-the-air slot it proved (issue #637). */
        if (identity.slot) {
            DSD_SNPRINTF(out, capacity, "Slot %u: %s", (unsigned int)identity.slot, first);
        } else {
            DSD_SNPRINTF(out, capacity, "%s", first);
        }
    } else if (identity.slot) {
        DSD_SNPRINTF(out, capacity, "Slot 1: %s; slot 2: %s", first,
                     receive_status_name(dsd_receive_status(opts, state, protocol, 1)));
    } else {
        DSD_SNPRINTF(out, capacity, "%s", first);
    }
}
