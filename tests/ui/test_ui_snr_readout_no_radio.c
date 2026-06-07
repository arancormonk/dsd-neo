// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_snr_readout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
expect_no_radio_readout(int rf_mod, const char* expected_label) {
    ui_snr_readout got = ui_snr_readout_for_mod(rf_mod);
    const char* actual_label = got.mod_label ? got.mod_label : "";

    assert(got.valid == 0);
    assert(got.snr_db == -50.0);
    assert(strcmp(actual_label, expected_label) == 0);
}

int
main(void) {
    expect_no_radio_readout(0, "C4FM");
    expect_no_radio_readout(1, "QPSK");
    expect_no_radio_readout(2, "GFSK");

    printf("UI_SNR_READOUT_NO_RADIO: OK\n");
    return 0;
}
