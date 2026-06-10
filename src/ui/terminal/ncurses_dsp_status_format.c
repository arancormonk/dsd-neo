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
    const int gate_closed = (squelch_power > 0.0 && channel_power < squelch_power);
    const char* gate = gate_closed ? "Closed" : "Open";
    DSD_SNPRINTF(out, out_size, "%s ch:%.1f dB sql:%.1f dB", gate, pwr_to_dB(channel_power), pwr_to_dB(squelch_power));
    return 0;
}
