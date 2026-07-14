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
