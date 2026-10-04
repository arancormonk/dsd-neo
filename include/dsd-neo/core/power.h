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

/**
 * @brief How the RTL squelch decides (issue #518 follow-up).
 *
 * LEVEL compares the channel power against a fixed threshold, as it always has. AUTO learns the channel's noise floor
 * from windows it classes as noise and opens the gate a margin above it (src/dsp/squelch_floor.c), so the same setting
 * works whatever the dongle, antenna or gain. NOISE opens an FM channel when the discriminator's output above the voice
 * band quiets by a threshold (src/dsp/nfm_noise_squelch.c), as a radio's noise squelch does; it needs no floor, and an
 * AM channel, or an FM one too narrow for the band it measures, runs AUTO instead. AUTO and NOISE are the dynamic modes:
 * both gate the monitor per sample.
 */
typedef enum {
    DSD_SQUELCH_MODE_LEVEL = 0,
    DSD_SQUELCH_MODE_AUTO = 1,
    DSD_SQUELCH_MODE_NOISE = 2,
} dsd_squelch_mode;

/** @brief Whether @p mode is one of the dynamic modes (AUTO, NOISE), which gate the monitor per sample. */
static inline int
dsd_squelch_mode_is_dynamic(int mode) {
    return mode == DSD_SQUELCH_MODE_AUTO || mode == DSD_SQUELCH_MODE_NOISE;
}

/** @brief @p mode when it is a dsd_squelch_mode, LEVEL otherwise. */
static inline int
dsd_squelch_mode_or_level(int mode) {
    return dsd_squelch_mode_is_dynamic(mode) ? mode : DSD_SQUELCH_MODE_LEVEL;
}

/**
 * @brief The per-sample gate flag a dynamic squelch attaches to each monitor sample: set when the gate was closed for
 * it. 0 is open, so a buffer of zeros (and every sample under LEVEL) reads open.
 */
enum { DSD_SQUELCH_FLAG_CLOSED = 0x01 };

/**
 * @brief The margin an AUTO squelch opens above the noise floor, and the quieting a NOISE squelch opens at, in whole dB.
 */
enum {
    DSD_SQUELCH_MARGIN_MIN_DB = 3,
    DSD_SQUELCH_MARGIN_MAX_DB = 30,
    DSD_SQUELCH_MARGIN_DEFAULT_DB = 10,
};

/**
 * @brief A squelch setting: the mode, the threshold a LEVEL squelch compares against (mean-power units, 0 = off), and
 * the margin a dynamic squelch opens at (whole dB: above the floor under AUTO, of quieting under NOISE). Each field
 * means something in its own modes only.
 */
typedef struct {
    int mode; /**< dsd_squelch_mode */
    double level;
    int margin_db;
} dsd_squelch_setting;

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
 * @param level Stored threshold in mean-power units.
 * @return Non-zero when the squelch is off.
 */
static inline int
dsd_squelch_is_off(double level) {
    return !(level > 0.0);
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
