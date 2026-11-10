// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The digital (`-g`) and analog (`-n`) output gain settings: their ranges and the rules every surface applies
 * (the CLI, the app-control handlers, the terminal menu and the JSON API).
 *
 * A digital setting is 0, meaning auto, or a fixed multiplier inside its range. Anything else is refused, never
 * clamped or turned into auto: NaN, infinities, negative values, values above the maximum, and positive values below
 * the minimum (a value such as 1e-50 would otherwise round to the float 0 in dsd_opts and select auto).
 *
 * An analog setting keeps `-n`'s documented rule: anything under 1 (0, a fraction, a negative value) is auto, and
 * 1..100 is a fixed gain. NaN, infinities and values above 100 are refused.
 *
 * The checks run in double, before the value narrows to the float setting. They rely on IEEE comparisons, so a caller
 * must be compiled without fast-math (see the -fno-fast-math source lists in the CMakeLists of runtime, app_control,
 * api and ui/terminal).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_GAIN_SETTING_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_GAIN_SETTING_H

#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Digital gain (`-g`, dsd_opts::audio_gain): a multiplier on the vocoder's reference level (1), about -60..+34 dB. */
#define DSD_DIGITAL_GAIN_MIN            0.001
#define DSD_DIGITAL_GAIN_MAX            50.0
/** Where a digital gain step from auto counts from: 0, so the first step up lands on 1, the reference level. */
#define DSD_DIGITAL_GAIN_STEP_FROM_AUTO 0.0

/** Analog gain (`-n`, dsd_opts::audio_gainA): 50 is the reference level; anything under the minimum is auto. */
#define DSD_ANALOG_GAIN_MIN             1.0
#define DSD_ANALOG_GAIN_MAX             100.0
/** Where an analog gain step from auto starts: the reference level. */
#define DSD_ANALOG_GAIN_STEP_FROM_AUTO  50.0

/**
 * @brief Whether @p value is a valid digital gain setting: 0 (auto) or DSD_DIGITAL_GAIN_MIN..DSD_DIGITAL_GAIN_MAX.
 * Written so NaN fails: every comparison with it is false.
 */
static inline int
dsd_digital_gain_setting_valid(double value) {
    return (value == 0.0 || (value >= DSD_DIGITAL_GAIN_MIN && value <= DSD_DIGITAL_GAIN_MAX)) ? 1 : 0;
}

/**
 * @brief The analog gain setting @p value selects, in @p out: 0 (auto) for anything under DSD_ANALOG_GAIN_MIN, the
 * value itself up to DSD_ANALOG_GAIN_MAX. Returns 1, or 0 with @p out untouched for NaN, infinities and values above
 * the maximum.
 */
static inline int
dsd_analog_gain_setting(double value, double* out) {
    /* The positive form, so NaN (every comparison false) falls through to the refusal. */
    if (value >= -DBL_MAX && value <= DSD_ANALOG_GAIN_MAX) {
        *out = (value < DSD_ANALOG_GAIN_MIN) ? 0.0 : value;
        return 1;
    }
    return 0;
}

/**
 * @brief A gain setting moved by @p steps whole units: from auto (0) the step starts at @p from_auto; a result below
 * @p lo goes back to auto; one above @p hi stays at @p hi. No step (0) leaves the setting as it is. In double, so a
 * fractional setting moves by the step instead of being truncated, and any int32 step stays exact.
 */
static inline double
dsd_gain_setting_step(double setting, int steps, double lo, double hi, double from_auto) {
    if (steps == 0) {
        return setting;
    }
    const double from = (setting > 0.0) ? setting : from_auto;
    const double moved = from + (double)steps;
    if (!(moved >= lo)) {
        return 0.0;
    }
    return (moved > hi) ? hi : moved;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_GAIN_SETTING_H */
