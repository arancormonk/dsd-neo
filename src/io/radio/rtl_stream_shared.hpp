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
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/dsp/demod_state.h>

#include "rtl_stream_mirrors.hpp"

extern demod_state demod;

extern std::atomic<double> g_snr_c4fm_db;
extern std::atomic<double> g_snr_qpsk_db;
extern std::atomic<double> g_snr_gfsk_db;

extern std::atomic<int> g_tuner_autogain_on;
extern std::atomic<uint32_t> g_tuner_autogain_set_seq;
/* A retune landing's autogain: written unconditionally, or with @p only_if_current only when no explicit setting
   (rtl_stream_set_tuner_autogain()) came after @p seq; the check and the write are one step under the setter's lock. */
void rtl_stream_land_retune_autogain(int onoff, int only_if_current, uint32_t seq);
/* A stream open's autogain, before its workers start: off on a source the supervisor cannot drive (@p capable 0: an
   I/Q replay, an Airspy); otherwise the last explicit setting, or the environment default (DSD_NEO_TUNER_AUTOGAIN)
   when there is none or the default has changed since it was made. */
void rtl_stream_open_tuner_autogain(int capable);

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

void rtl_stream_input_level_publish(const dsd_input_level_snapshot* snapshot);
void rtl_stream_input_level_reset(void);

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_STREAM_SHARED_HPP_ */
