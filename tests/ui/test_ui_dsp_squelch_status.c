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
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-28.4), dB_to_pwr(-120.0), out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-28.4 dB sql:-120.0 dB") == 0);
}

static void
test_squelch_status_closed(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-80.0), dB_to_pwr(-40.0), out, sizeof(out)) == 0);
    assert(strcmp(out, "Closed ch:-80.0 dB sql:-40.0 dB") == 0);
}

static void
test_squelch_status_open_when_disabled(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(0.0, 0.0, out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-120.0 dB sql:-120.0 dB") == 0);
}

static void
test_squelch_status_open_at_threshold(void) {
    char out[64];
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-40.0), dB_to_pwr(-40.0), out, sizeof(out)) == 0);
    assert(strcmp(out, "Open ch:-40.0 dB sql:-40.0 dB") == 0);
}

static void
test_squelch_status_rejects_missing_output(void) {
    assert(ui_dsp_format_squelch_status(dB_to_pwr(-20.0), dB_to_pwr(-30.0), NULL, 0U) != 0);
}

int
main(void) {
    test_squelch_status_open();
    test_squelch_status_closed();
    test_squelch_status_open_when_disabled();
    test_squelch_status_open_at_threshold();
    test_squelch_status_rejects_missing_output();
    return 0;
}
