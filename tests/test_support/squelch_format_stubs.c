// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* core's squelch level text for tests that compile the app-control squelch view without the core library: off, or the
 * level in dB, as dsd_squelch_format() and pwr_to_dB() write them. */

#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <math.h>
#include <stddef.h>

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    if (mean_power <= 0.0) {
        return -120.0;
    }
    const double db = 10.0 * log10(mean_power);
    return db < -120.0 ? -120.0 : (db > 0.0 ? 0.0 : db);
}

int
dsd_squelch_format(double mean_power, const char* unit, char* out,
                   size_t out_size) { // NOLINT(misc-use-internal-linkage)
    if (!out || out_size == 0U) {
        return -1;
    }
    if (dsd_squelch_is_off(mean_power)) {
        DSD_SNPRINTF(out, out_size, "%s", "off");
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%.1f%s", pwr_to_dB(mean_power), unit ? unit : "");
    return 0;
}
