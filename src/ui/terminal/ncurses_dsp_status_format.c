// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ncurses_dsp_status_format.h"

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>

int
ui_dsp_format_squelch_status(double channel_power, double squelch_power, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    const int gate_closed = (!dsd_squelch_is_off(squelch_power) && channel_power < squelch_power);
    const char* gate = gate_closed ? "Closed" : "Open";
    /* The threshold and the measurement are different things: a disabled threshold
     * says so, while the channel power is always a reading and always a number. */
    char sql[24];
    (void)dsd_squelch_format(squelch_power, " dB", sql, sizeof sql);
    DSD_SNPRINTF(out, out_size, "%s ch:%.1f dB sql:%s", gate, pwr_to_dB(channel_power), sql);
    return 0;
}
