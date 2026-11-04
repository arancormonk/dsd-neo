// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * A frequency typed by hand that is a row of the scan in force (issue #644): the match over the -Y list or the
 * trunk-scan targets (D4, D5), and the -Y row confirmed on air ([F1]). The trunk scan walks its own targets
 * (dsd_engine_trunk_scan_entry_rows()); the -Y list is walked here.
 */

#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/manual_tune.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/tuner_owner.h>
#include <stddef.h>
#include <stdint.h>

#include "scan_entry_internal.h"
#include "scan_landing.h"

int
dsd_engine_channel_scan_row_confirmed(const dsd_state* state, int row) {
    /* A pause (a resume pending included) left the row; a typed row's request or retry, or an untyped step's or walk's
       tune still pending, is movement outstanding, whatever landing is still recorded. */
    return state && !state->lcn_scan_paused && dsd_engine_scan_landing_is_row(state, row)
           && !dsd_engine_channel_scan_waiting(state) && dsd_engine_manual_tune_pending_row(state, NULL) < 0;
}

/* Whether a -Y list of the user's own is the scan in force: running, paused, or with a resume pending. A map the
   stopped scan left, one under -T and channels learned over the air are not. */
static int
scan_entry_channel_in_force(const dsd_opts* opts, const dsd_state* state) {
    return opts->trunk_scan_enabled != 1 && (dsd_channel_scan_owns_tuner(opts, state) || state->lcn_scan_paused)
           && dsd_state_trunk_lcn_user_list_present(opts, state);
}

/* Whether -Y row @p row holds exactly @p hz: a long row frequency is widened to compare, never the frequency typed
   narrowed; a placeholder (0 Hz) row holds none. */
static int
scan_entry_row_on(const dsd_state* state, int row, uint32_t hz) {
    const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
    return freq > 0 && (unsigned long long)freq == (unsigned long long)hz;
}

static dsd_scan_entry_reason
scan_entry_row_reason(const dsd_opts* opts, const dsd_state* state, int row, int rate_hz) {
    if (dsd_state_trunk_lcn_avoid_get(state, (size_t)row)) {
        return DSD_SCAN_ENTRY_AVOIDED;
    }
    return dsd_engine_channel_scan_row_width_fits(opts, state, row, rate_hz) ? DSD_SCAN_ENTRY_OK : DSD_SCAN_ENTRY_WIDTH;
}

/* The -Y rows on @p hz, in file order. On an untyped list a run of adjacent rows on one frequency is one channel, as
   `L` and `J` walk it: its first row speaks for the run. Every row of a typed list counts (each may run another
   mode, options or keys). */
static void
scan_entry_channel_rows(const dsd_opts* opts, const dsd_state* state, uint32_t hz, dsd_scan_entry_tally* tally) {
    const int untyped = !dsd_channel_modes_present(state);
    const int rate_hz = dsd_engine_scan_dsp_rate_hz(opts, state);
    for (int row = 0; row < state->lcn_freq_count; row++) {
        if (!scan_entry_row_on(state, row, hz) || (untyped && row > 0 && scan_entry_row_on(state, row - 1, hz))) {
            continue;
        }
        const char* name = dsd_state_trunk_lcn_name_get(state, (size_t)row);
        char label[sizeof tally->out->label];
        if (name && name[0] != '\0') {
            DSD_SNPRINTF(label, sizeof label, "%s", name);
        } else {
            DSD_SNPRINTF(label, sizeof label, "row %d", row + 1);
        }
        dsd_scan_entry_tally_row(tally, DSD_SCAN_ENTRY_CHANNEL, row, scan_entry_row_reason(opts, state, row, rate_hz),
                                 dsd_engine_channel_scan_row_confirmed(state, row), 0, label);
    }
}

int
dsd_engine_scan_entry_match(const dsd_opts* opts, const dsd_state* state, uint32_t hz, dsd_scan_entry_match_t* out) {
    dsd_scan_entry_match_t scratch;
    dsd_scan_entry_tally tally;
    dsd_scan_entry_tally_begin(&tally, out ? out : &scratch);
    if (!opts || !state || hz == 0U) {
        return 0;
    }
    if (opts->trunk_scan_enabled == 1) {
        (void)dsd_engine_trunk_scan_entry_rows(opts, state, hz, &tally);
    } else if (scan_entry_channel_in_force(opts, state)) {
        scan_entry_channel_rows(opts, state, hz, &tally);
    }
    return dsd_scan_entry_tally_end(&tally);
}
