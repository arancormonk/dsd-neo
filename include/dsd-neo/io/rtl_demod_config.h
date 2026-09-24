// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR demodulation configuration helpers.
 *
 * Provides a small surface for configuring the demodulation state and
 * related runtime DSP settings used by the RTL-SDR stream pipeline.
 * Exposes only pointer types so callers avoid heavy struct includes.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEMOD_CONFIG_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEMOD_CONFIG_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct demod_state;
struct output_state;

/* Runtime-config toggles shared with RTL stream/device modules. */
extern int disable_fs4_shift;

/**
 * Initialize the demodulator state for the requested mode (digital,
 * analog, or RO2) and attach its output ring target.
 *
 * @param demod            Demodulator state to initialize.
 * @param output           Output ring state used as the demod target.
 * @param opts             Decoder options (mode flags).
 * @param rtl_dsp_bw_hz    DSP baseband bandwidth in Hz.
 */
void rtl_demod_init_for_mode(struct demod_state* demod, struct output_state* output, const dsd_opts* opts,
                             int rtl_dsp_bw_hz);

/**
 * Apply environment- and options-driven DSP configuration to the
 * demodulator (resampler target and CQPSK path/timing settings).
 *
 * @param demod Demodulator state.
 * @param opts  Decoder options (CLI/runtime flags).
 */
void rtl_demod_config_from_env_and_opts(struct demod_state* demod, const dsd_opts* opts);

/**
 * Apply sensible defaults for digital vs analog modes when env/CLI
 * overrides are not present (CQPSK timing defaults, TED SPS, etc.).
 *
 * @param demod  Demodulator state.
 * @param opts   Decoder options (mode flags).
 * @param output Output state used to infer effective sample rate.
 */
void rtl_demod_select_defaults_for_mode(struct demod_state* demod, const dsd_opts* opts,
                                        const struct output_state* output);

/**
 * Report the rate the digital FSK discriminator stream should be resampled to.
 *
 * Returns 0 when the stream must pass through untouched: for CQPSK symbol output, when
 * the mode is off, or (in auto mode) when the demod rate already yields an integer SPS.
 *
 * @param demod Demodulator state.
 * @return Target rate in Hz, or 0 to bypass the resampler.
 */
int rtl_demod_digital_resample_target_hz(const struct demod_state* demod);

/**
 * Pure form of rtl_demod_digital_resample_target_hz(): the digital stream's resample target for the given policy
 * inputs, or 0 when the stream is not resampled.
 */
int rtl_demod_digital_resample_target_for(int output_kind, int digital_resample_mode, int resamp_target_hz,
                                          int symbol_rate_hz, int rate_out_hz, int capture_rate_device_forced);

/**
 * Output rate of the analog monitor stream for a demod rate: the resampler target when the monitor resamples to it,
 * otherwise @p rate_out_hz.
 */
int rtl_demod_monitor_output_rate_for(int resamp_target_hz, int rate_out_hz);

/**
 * The channel width @p kind is requested at: @p explicit_width_hz when positive, otherwise 0 (the legacy default) for
 * NFM and the kind's default for AM. Only the unset NFM default keeps the historical enable rule and WIDE fallback;
 * every requested width turns the channel filter on and is validated.
 */
int rtl_demod_analog_requested_width_hz(int kind, int explicit_width_hz);

/**
 * Check the analog channel a stream would run at @p rate_hz.
 *
 * Refuses a negative @p explicit_width_hz (only 0 selects the default), AM (the radio front end has no AM
 * demodulator yet), a requested width while DSD_NEO_CHANNEL_LPF=0 turns the channel filter off, and a requested width
 * the rate cannot realize (dsd_analog_width_check()). The unset NFM default (@p explicit_width_hz 0 with FM) never
 * fails: it keeps the historical filter behaviour. The unset AM default is checked like an explicit width (see
 * rtl_demod_analog_requested_width_hz()). @p rate_hz <= 0 means no DSP rate is known yet (no stream running): then only
 * the kind, environment and range rules apply, and the next stream open checks the width against the rate it delivers.
 *
 * @return 0 when the stream may run it, -1 with an actionable message in @p err otherwise.
 */
int rtl_demod_check_analog_channel(int kind, int explicit_width_hz, int rate_hz, char* err, size_t err_size);

/**
 * Check an analog channel against a rate chain that decimates after the demodulator.
 *
 * An I/Q replay sidecar may set post_downsample above 1: the channel filter then runs at @p rate_out_hz x
 * @p post_downsample, while the analog design and rtl_demod_check_analog_channel() work at @p rate_out_hz, so a
 * requested width would be neither applied nor refused. Such a width (explicit, or the AM default) is refused; the
 * unset NFM default keeps the legacy design, and @p post_downsample <= 1 (every live source) always passes.
 *
 * @return 0 when the stream may run it, -1 with an actionable message in @p err otherwise.
 */
int rtl_demod_check_analog_post_decimation(int kind, int explicit_width_hz, int rate_out_hz, int post_downsample,
                                           char* err, size_t err_size);

/**
 * Put @p demod on the analog channel for @p kind at its current rates.
 *
 * Marks the analog family, selects the WIDE profile, and sets the channel width: a requested width (explicit, or the
 * AM default; rtl_demod_analog_requested_width_hz()) turns the channel LPF on; the unset NFM default keeps the enable
 * decision stream configuration made (DSD_NEO_CHANNEL_LPF, else rate_in >= 20 kHz; channel_lpf_default_enable) and
 * falls back to the legacy WIDE design (width 0) where the rate cannot fit the default width. Does not validate; see
 * rtl_demod_check_analog_channel().
 *
 * @return 1 when the channel filter configuration changed, else 0.
 */
int rtl_demod_apply_analog_channel(struct demod_state* demod, int kind, int explicit_width_hz);

/**
 * Resolve the analog channel again after rate_out changed under a running stream (a retune the device settled on
 * another rate). The unset default moves between the default width and the legacy WIDE design as the new rate
 * allows. An explicit width stays as requested even when the new rate cannot realize it: the channel then has no
 * width-driven plan and runs with no channel filter, published as DSP-limited. No-op unless the analog monitor output
 * is running on the analog channel (dsd_demod_analog_monitor_active()): CQPSK toggled on under the analog family, or
 * a typed digital scan row's symbol profile, keeps its own profile filter.
 *
 * @return 0, or -1 with the validator's text in @p err when the explicit width no longer fits the rate.
 */
int rtl_demod_refresh_analog_channel_for_rate(struct demod_state* demod, char* err, size_t err_size);

/**
 * Validate and apply the analog channel @p opts ask for once rate_out is final (stream start): the checks of
 * rtl_demod_check_analog_channel() at rate_out and of rtl_demod_check_analog_post_decimation(). No-op outside the
 * analog family (including the M17 encoder).
 *
 * @return 0 on success, -1 with the refusal text in @p err (the stream must not start).
 */
int rtl_demod_finalize_analog_channel(struct demod_state* demod, const dsd_opts* opts, char* err, size_t err_size);

/**
 * Load de-emphasis (when @p demod->deemph is set) and the audio LPF from the runtime config, then compute their
 * coefficients for the current rate_out. DSD_NEO_DEEMPH=off clears deemph.
 *
 * @return 1 when the audio LPF is enabled.
 */
int rtl_demod_apply_audio_filters_from_config(struct demod_state* demod);

/** Recompute the de-emphasis and audio-LPF coefficients for @p demod->rate_out. */
void rtl_demod_refresh_audio_coefficients(struct demod_state* demod);

/** Return the monitor audio state (de-emphasis, DC, audio LPF, squelch envelope, discriminator) to fresh-open values. */
void rtl_demod_reset_audio_monitor_state(struct demod_state* demod);

/** Clear the half-band and channel-LPF histories. */
void rtl_demod_clear_filter_histories(struct demod_state* demod);

/** Reset the rational resampler's phase and history. */
void rtl_demod_reset_resampler_state(struct demod_state* demod);

/**
 * Switch the analog demodulator kind (demodulator and de-emphasis) and reset the monitor audio state.
 *
 * @return 1 when the kind changed, 0 when already on it, -1 for an invalid kind.
 */
int rtl_demod_set_analog_kind(struct demod_state* demod, int kind);

/**
 * Move a running front end to the analog family with the defaults a fresh analog open would choose (monitor output,
 * demodulator, de-emphasis and audio LPF from the config, analog channel, monitor resampler) and fresh filter state.
 * The capture rate chain is left alone. The caller clears the output ring.
 */
void rtl_demod_enter_analog_family(struct demod_state* demod, struct output_state* output, int kind,
                                   int explicit_width_hz, int rtl_dsp_bw_hz);

/**
 * Move a running front end to the digital family with fresh-open digital defaults (FSK discriminator output, no
 * de-emphasis, profile channel filter, digital resampler policy) and fresh filter state. The symbol profile and
 * CQPSK family follow in a demod-profile request; @p cqpsk_enable (> 0 for CQPSK) and @p symbol_rate_hz (> 0, else
 * the current one) name that profile, so the resampler and the output rate in @p output are decided for it the way
 * a fresh open of it would decide them. The caller clears the output ring.
 */
void rtl_demod_enter_digital_family(struct demod_state* demod, struct output_state* output, int channel_profile,
                                    int cqpsk_enable, int symbol_rate_hz, int rtl_dsp_bw_hz);

/**
 * Recompute resampler configuration when the demod output rate changes,
 * updating output.rate accordingly.
 *
 * @param demod         Demodulator state.
 * @param output        Output state to update.
 * @param rtl_dsp_bw_hz DSP baseband bandwidth in Hz (fallback when rate_out is unset).
 */
void rtl_demod_maybe_update_resampler_after_rate_change(struct demod_state* demod, struct output_state* output,
                                                        int rtl_dsp_bw_hz);

/**
 * Refresh TED SPS after rate changes unless explicitly overridden by
 * runtime configuration.
 *
 * @param demod                  Demodulator state.
 * @param opts                   Decoder options (mode flags); may be NULL.
 * @param output                 Output state (current sink rate).
 * @param preserve_active_profile Non-zero keeps the symbol rate and level count the front end is
 *                               already on (set by the SPS hunt, the trunking engine, or the
 *                               operator) and recomputes only the timing SPS for the current
 *                               output rate; zero seeds the profile from @p opts, which is the
 *                               stream-open default.
 */
void rtl_demod_maybe_refresh_ted_sps_after_rate_change(struct demod_state* demod, const dsd_opts* opts,
                                                       const struct output_state* output, int preserve_active_profile);

/**
 * Release resources owned by the demodulator state.
 *
 * @param demod Demodulator state to clean up.
 */
void rtl_demod_cleanup(struct demod_state* demod);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEMOD_CONFIG_H_ */
