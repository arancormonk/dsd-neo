// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Active speech level after ITU-T P.56 method B, for tests that check how loud speech plays.
 *
 * This is the repository's own implementation of the published method, not the ITU reference code. The signal's
 * rectified magnitude runs through two cascaded one-pole smoothers with a 30 ms time constant. A sample counts as
 * active at threshold c_j = 2^j (j = 0..14, in int16 units) when the smoothed envelope reached c_j at that sample or
 * within the 200 ms hangover before it. For each threshold the meter forms the active level A_j = 10 log10(sum x^2 /
 * active samples) and its distance to the threshold, A_j - 20 log10(c_j). The first threshold whose distance is at
 * most the 15.9 dB margin gives the level, interpolated in dB between it and the threshold below.
 *
 * Calibration: a sine reads 3.01 dB below its peak (a sine with peak 32768 reads -3.01 dBFS), and white or speech
 * signals that are active all the time read their RMS level.
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_P56_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_P56_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Active speech level of @p x in dBFS re int16 full scale (32768).
 *
 * @param x            Finite samples in int16 units (full scale 32768). They are not clipped or rounded.
 * @param n            Number of samples.
 * @param rate_hz      Sample rate. Every time constant is in seconds, so any rate gives the same reading of one
 *                     waveform.
 * @param activity_out Optional. Receives the activity factor: mean power over the whole signal divided by the active
 *                     level's power, which is the active fraction of the signal (0..1).
 *
 * @return The level in dBFS. For an empty or silent signal (the envelope never reaches 1 int16 unit, about -90 dBFS)
 *         it returns -INFINITY with an activity of 0. It returns NAN, with an activity of NAN, when @p x is NULL with
 *         samples to read, when @p rate_hz is not positive, and for a signal no threshold can measure (an isolated
 *         impulse whose envelope never comes within the margin of its power).
 */
double dsd_test_p56_level_dbfs(const float* x, size_t n, int rate_hz, double* activity_out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_P56_H_ */
