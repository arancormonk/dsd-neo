// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Internal cross-translation-unit RTL stream state declarations.
 *
 * These symbols are intentionally shared between rtl_sdr_fm.cpp and
 * rtl_metrics.cpp. Keep this header private to src/io/radio/.
 */

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_STREAM_SHARED_HPP_
#define DSD_NEO_SRC_IO_RADIO_RTL_STREAM_SHARED_HPP_

#include <atomic>
#include <dsd-neo/dsp/demod_state.h>

extern demod_state demod;

extern std::atomic<double> g_snr_c4fm_db;
extern std::atomic<double> g_snr_qpsk_db;
extern std::atomic<double> g_snr_gfsk_db;

extern std::atomic<int> g_tuner_autogain_on;

extern std::atomic<int> g_auto_ppm_enabled;
extern std::atomic<int> g_auto_ppm_user_en;
extern std::atomic<int> g_auto_ppm_locked;
extern std::atomic<int> g_auto_ppm_training;
extern std::atomic<int> g_auto_ppm_lock_ppm;
extern std::atomic<double> g_auto_ppm_lock_snr_db;
extern std::atomic<double> g_auto_ppm_lock_df_hz;
extern std::atomic<double> g_auto_ppm_snr_db;
extern std::atomic<double> g_auto_ppm_df_hz;
extern std::atomic<double> g_auto_ppm_est_ppm;
extern std::atomic<int> g_auto_ppm_last_dir;
extern std::atomic<int> g_auto_ppm_cooldown;

extern std::atomic<double> g_spec_peak_db;
extern std::atomic<double> g_spec_snr_db;
extern std::atomic<double> g_resid_cfo_phase_hz;

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_STREAM_SHARED_HPP_ */
