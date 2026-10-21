// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>

void
dsd_app_receive_note_unreachable(const dsd_opts* opts, const dsd_state* state, const dsd_scan_option_values* values,
                                 char* notice, size_t capacity) {
    if (!notice || !capacity) {
        return;
    }
    dsd_scan_option_baseline configured;
    dsd_scan_option_baseline_configured(opts, state, &configured);
    /* Whole identifiers, ending in "..." when more are left out; the notice gives way to the note if both do not fit. */
    char names[48];
    if (dsd_scan_option_unreachable_names(values, &configured, names, sizeof names) > 0) {
        char note[sizeof names + 16];
        DSD_SNPRINTF(note, sizeof note, "; never shown: %s", names);
        dsd_receive_note_append(notice, capacity, note);
    }
    DSD_SECURE_ZERO(&configured, sizeof configured);
}
