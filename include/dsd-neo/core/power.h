// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Mean power and dB conversion helpers, and the RTL squelch contract.
 *
 * These helpers are used by the RTL squelch/VOX paths and UI displays.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_POWER_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_POWER_H_H

#include <math.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

double raw_pwr(const short* samples, int len, int step);
double pwr_to_dB(double mean_power);
double dB_to_pwr(double dB);

/**
 * @brief Whether a stored squelch threshold gates anything.
 *
 * A threshold of zero or below never closes the gate: every consumer tests
 * `level > 0` before comparing channel power against it (see
 * `demod_pipeline.cpp` and `dsd_frame_sync.c`). Displaying such a level as
 * decibels reads as a very low threshold rather than as "not gating", which is
 * exactly the confusion this predicate exists to prevent.
 *
 * @param mean_power Stored threshold in mean-power units.
 * @return Non-zero when the squelch is off.
 */
static inline int
dsd_squelch_is_off(double mean_power) {
    return !(mean_power > 0.0);
}

/**
 * @brief Map a user-facing `sql` setting onto a stored threshold.
 *
 * One definition for every entry point that accepts the setting: the `sql` field
 * of an `rtl:`/`rtltcp:`/`soapy:` input string, the `[input] rtl_sql` config key,
 * and the runtime squelch commands. Negative values are decibels, zero switches
 * the squelch off, and a positive value is a linear mean power taken as given
 * (the legacy CLI contract).
 *
 * The decibel branch is the same computation as dB_to_pwr(), inlined here so the
 * runtime library — which does not link the core object that defines it — shares
 * one implementation instead of keeping private copies.
 *
 * @param sql Setting as the user wrote it.
 * @return Threshold in mean-power units; 0.0 when squelch is off.
 */
static inline double
dsd_squelch_level_from_sql(double sql) {
    if (!(sql < 0.0)) {
        /* Zero is off, and a positive value is already a mean power. NaN lands
         * here too, and off is the safe reading of a value that is not a number. */
        return (sql > 0.0) ? sql : 0.0;
    }
    if (sql < -200.0) {
        sql = -200.0; /* avoid denormals, matching dB_to_pwr() */
    }
    /* exp(dB * ln(10)/10) instead of pow(10, dB/10) to avoid generic pow overhead. */
    const double kLn10_over_10 = 2.302585092994046 / 10.0;
    double pwr = exp(sql * kLn10_over_10);
    if (pwr < 0.0) {
        pwr = 0.0;
    }
    if (pwr > 1.0) {
        pwr = 1.0;
    }
    return pwr;
}

/**
 * @brief Render a squelch threshold for display.
 *
 * Writes `off` when the threshold gates nothing, otherwise the level in decibels
 * to one place followed by @p unit. The unit is the caller's so each surface
 * keeps its own spacing for a real threshold ("dB" for `SQ=-47.0dB`, " dB" for
 * `SQL: -47.0 dB`).
 *
 * @param mean_power Stored threshold in mean-power units.
 * @param unit       Text appended after the number; may be empty, not NULL.
 * @param out        Destination buffer.
 * @param out_size   Capacity of @p out in bytes.
 * @return 0 on success, -1 when @p out is NULL or @p out_size is zero.
 */
int dsd_squelch_format(double mean_power, const char* unit, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_POWER_H_H */
