// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional RTL stream metrics.
 *
 * Some DSP/protocol code wants to query RTL stream metrics without directly
 * depending on IO backends. The engine installs real hook functions at
 * startup. Missing query hooks return empty metrics; missing mutating hooks
 * report that the operation is unavailable.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_METRICS_HOOKS_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_METRICS_HOOKS_H_H

#include <dsd-neo/platform/platform.h>

#include <stdint.h>

#include <dsd-neo/core/input_level.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int (*output_rate_hz)(void);
    int (*output_kind)(void);
    int (*symbol_profile)(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile);
    uint32_t (*stream_generation)(void);
    int (*cqpsk_status)(int* out_cqpsk_enable, int* out_cqpsk_timing_active);
    int (*request_cqpsk_reacquire)(void);
    int (*cqpsk_timing_bias)(void);
    double (*snr_bias_evm)(void);
    double (*snr_c4fm_db)(void);
    double (*snr_c4fm_eye_db)(void);
    double (*snr_cqpsk_db)(void);
    double (*snr_gfsk_db)(void);
    double (*snr_gfsk_eye_db)(void);
    double (*snr_qpsk_const_db)(void);
    void (*p25p1_ber_update)(int ok_delta, int err_delta);
    void (*p25p2_err_update)(int slot, int facch_ok, int facch_err, int sacch_ok, int sacch_err, int voice_err);
    int (*stream_active)(void);
    int (*input_level)(dsd_input_level_snapshot* out);
    int (*apply_demod_profile)(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps);
    /** Channel squelch threshold in mean-power units (rtl_squelch_level's); 0 switches it off. */
    void (*set_channel_squelch)(double mean_power);
    /* Receive family / analog profile request (dsd_rx_family, dsd_analog_demod, width in Hz; 0 = default). */
    int (*apply_analog_profile)(int family, int kind, int width_hz);
    /* Published analog profile; returns 1 while the analog family is active. */
    int (*analog_profile)(int* out_kind, int* out_width_hz, int* out_lpf_on);
    /* 1 while the front end runs the analog receive family, including under a symbol profile applied on its own. */
    int (*analog_family_active)(void);
    /* Output rate the front end will have once it runs the family (dsd_rx_family); 0 when unknown. */
    unsigned int (*output_rate_for_family)(int family, int cqpsk_enable, int symbol_rate_hz);
} dsd_rtl_stream_metrics_hooks;

typedef enum DSD_ATTR_PACKED dsd_rtl_stream_channel_profile {
    DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE = 0,
    DSD_RTL_STREAM_CHANNEL_PROFILE_6K25 = 1,
    DSD_RTL_STREAM_CHANNEL_PROFILE_12K5 = 2,
    DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE = 3,
    DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM = 4,
    DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK = 5,
} dsd_rtl_stream_channel_profile;

void dsd_rtl_stream_metrics_hooks_set(const dsd_rtl_stream_metrics_hooks* hooks);

unsigned int dsd_rtl_stream_metrics_hook_output_rate_hz(void);
int dsd_rtl_stream_metrics_hook_output_kind(void);
int dsd_rtl_stream_metrics_hook_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile);
uint32_t dsd_rtl_stream_metrics_hook_stream_generation(void);
int dsd_rtl_stream_metrics_hook_stream_active(void);
int dsd_rtl_stream_metrics_hook_input_level(dsd_input_level_snapshot* out);
/**
 * @brief Synchronize the RTL demodulator family, symbol profile, and timing recovery rate.
 *
 * The engine implementation applies the family transition before updating TED timing and the
 * symbol/channel profile.
 */
int dsd_rtl_stream_metrics_hook_apply_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels,
                                                    int channel_profile, int ted_sps);
/**
 * @brief Hand the demodulator's channel squelch a new threshold.
 *
 * Scan rows and targets that override the squelch (issue #521) reach the RTL demodulator
 * only through this hook; the engine maps it to rtl_stream_set_channel_squelch(). Returns 0
 * when forwarded and -1 when no radio backend is installed.
 */
int dsd_rtl_stream_metrics_hook_set_channel_squelch(double mean_power);
/**
 * @brief Ask the RTL front end for a receive family and, for analog, a demodulator kind and channel width.
 *
 * Queued for the demod thread; switching family re-applies that family's fresh-open defaults. Leaving analog expects
 * the digital symbol profile to follow through dsd_rtl_stream_metrics_hook_apply_demod_profile(), and the switch waits
 * for it so both land at the same demod block boundary.
 *
 * @param family   dsd_rx_family (runtime/analog_channel.h).
 * @param kind     dsd_analog_demod for the analog family.
 * @param width_hz Explicit analog channel width in Hz, 0 for the kind's default.
 * @return 0 when accepted, -1 when refused or when no RTL front end is installed.
 */
int dsd_rtl_stream_metrics_hook_apply_analog_profile(int family, int kind, int width_hz);
/**
 * @brief Read the published analog receive profile.
 *
 * @return 1 while the analog family is active (outputs filled), 0 otherwise (outputs zeroed).
 */
int dsd_rtl_stream_metrics_hook_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on);
/**
 * @brief Report whether the RTL front end runs the analog receive family.
 *
 * Stays 1 while a symbol profile applied without a family request (a CQPSK toggle, a typed digital scan row) has moved
 * the front end off the analog monitor output; a digital family request leaves the family from there too.
 *
 * @return 1 while the analog family runs, 0 otherwise or when no RTL front end is installed.
 */
int dsd_rtl_stream_metrics_hook_analog_family_active(void);
/**
 * @brief Predict the output rate the RTL front end will have once it runs @p family.
 *
 * A family switch lands on the demod thread after the request returns, so a caller timing the decoder for the new
 * family reads this rather than the current output rate.
 *
 * @param family         dsd_rx_family.
 * @param cqpsk_enable   Non-zero for the CQPSK symbol output (digital family only).
 * @param symbol_rate_hz Digital symbol rate, which decides the digital resampling policy.
 * @return Predicted output rate in Hz, or 0 when it is unknown or no RTL front end is installed.
 */
unsigned int dsd_rtl_stream_metrics_hook_output_rate_for_family(int family, int cqpsk_enable, int symbol_rate_hz);
int dsd_rtl_stream_metrics_hook_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active);
int dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire(void);
int dsd_rtl_stream_metrics_hook_cqpsk_timing_bias(void);
double dsd_rtl_stream_metrics_hook_snr_bias_evm(void);
double dsd_rtl_stream_metrics_hook_snr_c4fm_db(void);
double dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db(void);
double dsd_rtl_stream_metrics_hook_snr_cqpsk_db(void);
double dsd_rtl_stream_metrics_hook_snr_gfsk_db(void);
double dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db(void);
double dsd_rtl_stream_metrics_hook_snr_qpsk_const_db(void);
void dsd_rtl_stream_metrics_hook_p25p1_ber_update(int ok_delta, int err_delta);
void dsd_rtl_stream_metrics_hook_p25p2_err_update(int slot, int facch_ok, int facch_err, int sacch_ok, int sacch_err,
                                                  int voice_err);
int dsd_rtl_stream_metrics_hook_symbol_cache_pending(void);
void dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(int delta);
void dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_METRICS_HOOKS_H_H */
