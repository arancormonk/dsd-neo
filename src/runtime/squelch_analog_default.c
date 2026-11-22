// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The analog default squelch (--squelch-analog-default, issue #899): a dynamic setting the decoder runs on an analog
   channel of a radio input while nobody set a squelch, so a frontend can start every radio session the same way and
   still hear nothing but traffic on its analog channels. Digital channels, audio input, EDACS and the M17 encoder never
   see it: they run the session's own setting, as without the option. */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/squelch.h>
#include <stddef.h>

/* Whether the scan row on air sets its own squelch (a row's --squelch or --squelch-db, a signal-search range's). */
static int
squelch_default_row_has_own(const dsd_state* state) {
    const dsd_scan_option_values* row = state ? dsd_scan_mode_row_options(state) : NULL;
    return row && (row->present & DSD_SCAN_OPT_SQUELCH);
}

int
dsd_squelch_analog_default_wanted(const dsd_opts* opts, const dsd_state* state) {
    return dsd_squelch_analog_default_pending(opts) && dsd_opts_input_is_radio(opts) && dsd_opts_is_analog_family(opts)
           && !squelch_default_row_has_own(state) && !dsd_scan_mode_updating(state);
}

/* Bring the default in: the fields' own setting becomes the base. 1 when the fields changed. */
static int
squelch_default_apply(dsd_opts* opts) {
    if (opts->squelch_default_state != DSD_SQUELCH_DEFAULT_APPLIED) {
        opts->squelch_default_base_level = opts->rtl_squelch_level;
        opts->squelch_default_base_mode = opts->rtl_squelch_mode;
        opts->squelch_default_base_margin_db = opts->rtl_squelch_margin_db;
        opts->squelch_default_state = DSD_SQUELCH_DEFAULT_APPLIED;
    }
    const dsd_squelch_setting want = dsd_squelch_analog_default_setting(opts);
    const dsd_squelch_setting now = dsd_squelch_setting_of_opts(opts);
    if (dsd_squelch_setting_equal(&want, &now)) {
        return 0;
    }
    opts->rtl_squelch_mode = want.mode;
    opts->rtl_squelch_margin_db = want.margin_db;
    return 1;
}

/* The AM monitor of a radio input runs a NOISE setting as AUTO with the same N (issue #899 D4): say so each time that
   starts, whatever made it start (the start, a decode switch, a squelch edit, a scan's leave, an input change). */
static void
squelch_note_am_auto(dsd_opts* opts, const dsd_state* state) {
    const int am_auto = opts->rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE && dsd_opts_input_is_radio(opts)
                        && dsd_squelch_noise_has_no_fm(opts, state);
    if (am_auto && !opts->squelch_am_auto_noted) {
        LOG_INFO("NOTICE: The noise squelch needs an FM channel; this AM channel runs auto+%ddB.\n",
                 opts->rtl_squelch_margin_db);
    }
    opts->squelch_am_auto_noted = am_auto;
}

int
dsd_squelch_analog_default_sync(dsd_opts* opts, const dsd_state* state) {
    /* While a scan scope is suspended for a command, the options hold its configured values, not what is on air: its
       resume decides. */
    if (!opts || dsd_scan_mode_updating(state)) {
        return 0;
    }
    int changed = 0;
    if (dsd_squelch_analog_default_pending(opts)) {
        if (squelch_default_row_has_own(state)) {
            /* The row's own squelch is in the fields (its option's store took the default out). */
            dsd_squelch_analog_default_note_written(opts);
        } else {
            changed = dsd_squelch_analog_default_wanted(opts, state) ? squelch_default_apply(opts)
                                                                     : dsd_squelch_analog_default_release(opts);
        }
    }
    /* Written over while applied (a snapshot put back, a row's or an edit's store): the receiver may still hold it. */
    if (opts->squelch_default_moved) {
        opts->squelch_default_moved = 0;
        changed = 1;
    }
    squelch_note_am_auto(opts, state);
    return changed;
}
