// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DSP demodulation pipeline public API.
 *
 * Declares decimation, discrimination, deemphasis, DC block, audio filtering,
 * and the full pipeline entrypoint implemented in `src/dsp/demod_pipeline.cpp`.
 */

#ifndef DSP_DEMOD_PIPELINE_H
#define DSP_DEMOD_PIPELINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of demod_state structure */
struct demod_state;

/**
 * Boxcar low-pass and decimate by step (no wraparound).
 * Length must be a multiple of step.
 *
 * @param signal2 In/out buffer of samples.
 * @param len     Length of input buffer.
 * @param step    Decimation factor.
 * @return New length after decimation.
 */
int low_pass_simple(float* signal2, int len, int step);

/**
 * Simple square window FIR on real samples with decimation to rate_out2.
 *
 * @param s Demodulator state (uses result buffer and decimation state).
 */
void low_pass_real(struct demod_state* s);

/**
 * Perform FM discrimination on interleaved low-passed I/Q to produce audio PCM.
 *
 * @param fm Demodulator state (uses lowpassed as input, writes to result).
 * @note Renamed to avoid collision with codec2's fm_demod symbol on Windows.
 */
void dsd_fm_demod(struct demod_state* fm);

/* AM envelope detector design (issue #524). The carrier estimate is a one-pole average of |z| with this time constant
   in milliseconds, recomputed for the rate the detector runs at. */
#define DSD_AM_CARRIER_TAU_MS 50

/**
 * AM envelope detector on interleaved low-passed I/Q (issue #524).
 *
 * Output is 0.25 x clamp(|z| / C - 1, -2, 2), where C is the carrier estimate held in `am_carrier`: a one-pole average
 * of |z| with a DSD_AM_CARRIER_TAU_MS time constant. Dividing by the carrier makes the level the modulation depth,
 * whatever the RF level or input scaling (100% modulation peaks at 0.25, as live FM does at about 6 kHz deviation), so
 * the RTL output scale is not applied to it. The envelope has no phase, so a carrier offset inside the channel changes
 * nothing. A block the channel squelch zeroed (`channel_squelched`) is silence and leaves C where it was. C at 0 (a
 * reset) is warm-started from the block's mean magnitude; with no carrier at all the output is silence.
 *
 * @param fm Demodulator state (uses lowpassed as input, writes to result, updates am_carrier).
 */
void dsd_am_demod(struct demod_state* fm);

/**
 * Whether the AM envelope detector demodulates the monitor audio (the analog family's AM kind installs it).
 *
 * @param d Demodulator state; NULL reads as not AM.
 * @return 1 when `mode_demod` is dsd_am_demod(), else 0.
 */
int dsd_demod_am_active(const struct demod_state* d);

/**
 * Whether the complex I/Q DC blocker runs on the next block.
 *
 * It runs when enabled (`iq_dc_block_enable`), except under the AM detector: AM keeps its carrier at 0 Hz after
 * tuning (centred I/Q, offset tuning), where the blocker would remove it. The setting itself is kept.
 *
 * @param d Demodulator state; NULL reads as not running.
 * @return 1 when the blocker runs, else 0.
 */
int dsd_demod_iq_dc_block_active(const struct demod_state* d);

/**
 * Pass-through demodulator: copies low-passed samples to output unchanged.
 *
 * @param fm Demodulator state (copies lowpassed to result).
 */
void raw_demod(struct demod_state* fm);

/**
 * Differential QPSK demodulator for CQPSK/LSM.
 *
 * Converts each carrier-corrected differential phasor to a real symbol using
 * `atan2f(Q, I) * (4/pi)`, matching OP25's `multiply_const_ff(4.0/pi)` stage.
 * The output range maps nominal CQPSK decision points to approximately
 * `{-3, -1, +1, +3}` for four-level slicers.
 *
 * @param fm Demodulator state (reads interleaved I/Q in lowpassed, writes phase deltas to result).
 */
void qpsk_differential_demod(struct demod_state* fm);

/**
 * Apply post-demod deemphasis IIR filter with Q15 coefficient.
 *
 * @param fm Demodulator state (reads/writes result, updates deemph_avg).
 */
void deemph_filter(struct demod_state* fm);

/**
 * Apply a simple DC blocking (leaky integrator high-pass) filter to audio.
 *
 * @param fm Demodulator state (reads/writes result, updates dc_avg).
 */
void dc_block_filter(struct demod_state* fm);

/**
 * Apply a simple one-pole low-pass filter to audio.
 *
 * @param fm Demodulator state (reads/writes result, updates audio_lpf_state).
 */
void audio_lpf_filter(struct demod_state* fm);

/**
 * Calculate mean power (squared RMS) with DC bias removed.
 *
 * @param samples Input samples buffer.
 * @param len     Number of samples.
 * @param step    Step size for sampling.
 * @return Mean power (squared RMS) with DC bias removed.
 */
float mean_power(const float* samples, int len, int step);

/**
 * Full demodulation pipeline for one block.
 * Applies decimation via half-band cascade and the selected demodulation chain.
 *
 * @param d Demodulator state (consumes lowpassed, produces result).
 */
void full_demod(struct demod_state* d);

/**
 * Channel edge (Hz) that the channel low-pass protects for a profile.
 *
 * The half-width of the nominal channel — 12.5 kHz modes return 6250. This is
 * the channel, not the filter: DSD_CH_LPF_PROFILE_P25_CQPSK runs a roomier
 * 7250 Hz cutoff and still protects a 12.5 kHz channel.
 *
 * @param profile DSD_CH_LPF_PROFILE_* value.
 * @return Channel half-width in Hz; the wide/analog edge for unknown profiles.
 */
double dsd_channel_lpf_protected_edge_hz(int profile);

/**
 * Design the analog channel filter for a full RF channel width.
 *
 * Cutoff is width/2 + DSD_ANALOG_CHANNEL_GUARD_HZ with the fixed
 * DSD_ANALOG_CHANNEL_TRANSITION_HZ Blackman transition, so the width is the
 * protected passband. 16000 Hz reproduces the WIDE profile's taps wherever the
 * WIDE design succeeds (rates from about 19.1 to 51.4 kHz: no 0.9 x Nyquist
 * clamp, within its 144-tap cap). Above that WIDE runs its 63-tap fallback
 * prototype, and this design gives 16000 Hz its full length instead (219 taps at
 * 78,125 Hz), up to the analog tap capacity at about 102.7 kHz. There is no
 * Nyquist clamp and no fallback prototype here: a width the rate cannot realize
 * (see dsd_analog_width_realizable()) is an error.
 *
 * @param rate_hz  Channel-filter sample rate (demod rate_out) in Hz.
 * @param width_hz Full channel width in Hz.
 * @param taps_out Output taps; must hold @p max_taps floats.
 * @param max_taps Capacity of @p taps_out (at most DSD_ANALOG_CHANNEL_MAX_TAPS is used).
 * @return Number of taps written, or -1 when the width cannot be designed.
 */
int dsd_channel_lpf_design_analog(int rate_hz, int width_hz, float* taps_out, int max_taps);

/**
 * Channel width the legacy WIDE profile plan protects at a rate.
 *
 * The plan the unset NFM default runs where the rate cannot realize the 16 kHz
 * width-driven design: the 144-tap WIDE design with its cutoff held to 0.9 x
 * Nyquist, or, at rates that design cannot fit (above about 51.4 kHz), the
 * 63-tap WIDE fallback prototype, whose response scales with the rate it runs
 * at. The width is the protected passband (twice the edge the transition
 * starts at), measured the same way as an analog width.
 *
 * @param rate_hz Channel-filter sample rate (demod rate_out) in Hz.
 * @return Protected width in Hz, or 0 for a rate of 0 or less.
 */
int dsd_channel_lpf_legacy_wide_width_hz(int rate_hz);

#ifdef __cplusplus
}
#endif

#endif /* DSP_DEMOD_PIPELINE_H */
