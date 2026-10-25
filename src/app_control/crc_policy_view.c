// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/crc_policy_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>

/* The scan row's own -F or --strict-crc, or NULL when the row sets none. */
static const dsd_scan_option_values*
crc_policy_row(const dsd_state* state) {
    const dsd_scan_option_values* row = state ? dsd_scan_mode_row_options(state) : NULL;
    return (row && (row->present & DSD_SCAN_OPT_CRC) != 0U) ? row : NULL;
}

void
dsd_app_crc_policy_view(const dsd_opts* opts, const dsd_state* state, dsd_app_crc_policy* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts) {
        return;
    }
    /* The live scope's baseline holds the configured pair; without a scope, or while a command has it suspended,
       dsd_opts does. */
    out->configured = dsd_scan_mode_configured_relaxed_crc(opts, state) ? 1U : 0U;
    /* dsd_opts holds the pair in force, except while suspended: then the row's own decides once it resumes. */
    const dsd_scan_option_values* row = crc_policy_row(state);
    out->row_override = row ? 1U : 0U;
    if (row) {
        out->effective = row->strict_crc ? 0U : 1U;
    } else {
        out->effective = dsd_opts_relaxed_crc(opts) ? 1U : 0U;
    }
}

int
dsd_app_crc_policy_edit_notice(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    dsd_app_crc_policy policy;
    dsd_app_crc_policy_view(opts, state, &policy);
    const char* configured = policy.configured ? "On" : "Off";
    if (!policy.row_override) {
        DSD_SNPRINTF(out, out_size, "Applied: relaxed CRC -> %s", configured);
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "Default relaxed CRC -> %s; this channel overrides it (%s)", configured,
                 policy.effective ? "On" : "Off");
    return 1;
}
