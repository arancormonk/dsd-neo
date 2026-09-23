// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/power.h>
#include <string.h>
#include "ncurses_dsp_status_format.h"

static void
test_squelch_status_open(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-28.4), dB_to_pwr(-120.0), 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-28.4 dB sql:-120.0 dB") == 0);
}

static void
test_squelch_status_closed(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-80.0), dB_to_pwr(-40.0), 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Closed ch:-80.0 dB sql:-40.0 dB") == 0);
}

/*
 * A disabled squelch has to say so. Rendered as a number it read as a threshold
 * gating at -120 dB, which is why a reporter concluded squelch was applied and
 * very low when it was switched off entirely.
 */
static void
test_squelch_status_open_when_disabled(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(0.0, 0.0, 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-120.0 dB sql:off") == 0);
}

/* The channel measurement is unaffected: only the threshold reads as off. */
static void
test_squelch_status_reports_power_while_disabled(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-60.0), 0.0, 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-60.0 dB sql:off") == 0);
}

static void
test_squelch_status_open_at_threshold(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-40.0), dB_to_pwr(-40.0), 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-40.0 dB sql:-40.0 dB") == 0);
}

/* Issue #521: a scan row's --squelch-db reaches rtl_squelch_level through
 * dsd_squelch_level_from_sql(), and the DSP panel gates and prints it like any other
 * threshold -- including a row that switches the squelch off. */
static void
test_squelch_status_row_thresholds(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-70.0), dsd_squelch_level_from_sql(-60.0), 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Closed ch:-70.0 dB sql:-60.0 dB") == 0);
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-70.0), dsd_squelch_level_from_sql(-100.0), 0, out, sizeof(out))
           == 0);
    assert(strcmp(out, "Open ch:-70.0 dB sql:-100.0 dB") == 0);
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-70.0), dsd_squelch_level_from_sql(0.0), 0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-70.0 dB sql:off") == 0);
    /* While the row sets the threshold, the panel says so, as the input line does. */
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-70.0), dsd_squelch_level_from_sql(-60.0), 1, out, sizeof(out)) == 0);
    assert(strcmp(out, "Closed ch:-70.0 dB sql:-60.0 dB (row)") == 0);
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-70.0), dsd_squelch_level_from_sql(0.0), 1, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-70.0 dB sql:off (row)") == 0);
}

static void
test_squelch_status_rejects_missing_output(void) {
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-20.0), dB_to_pwr(-30.0), 0, NULL, 0U) != 0);
}

int
main(void) {
    test_squelch_status_open();
    test_squelch_status_closed();
    test_squelch_status_open_when_disabled();
    test_squelch_status_reports_power_while_disabled();
    test_squelch_status_open_at_threshold();
    test_squelch_status_row_thresholds();
    test_squelch_status_rejects_missing_output();
    return 0;
}
