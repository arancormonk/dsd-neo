// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frame sync helper APIs.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h> // IWYU pragma: keep

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset modulation auto-detect state used by frame sync.
 */
void dsd_frame_sync_reset_mod_state(void);

/**
 * @brief Return non-zero when alternate-protocol sync should be suppressed during active P25 trunking.
 */
int dsd_frame_sync_suppress_p25_alt_sync(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero when TCP no-signal diagnostics should stay off the console.
 */
int dsd_frame_sync_suppress_tcp_no_signal_console(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return the number of no-sync buffer passes to dwell before SPS hunt advances.
 */
int dsd_frame_sync_sps_hunt_dwell_passes(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Scan for a valid frame sync pattern and return its type.
 */
int getFrameSync(dsd_opts* opts, dsd_state* state);

/**
 * @brief Emit diagnostic information about detected frame sync.
 *
 * @param frametype Human-friendly frame type string.
 * @param offset Bit offset into the buffer where sync was found.
 * @param modulation Modulation label (e.g., C4FM, QPSK).
 */
void printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
                    const char* modulation);

#ifdef DSD_NEO_TEST_HOOKS
int dsd_frame_sync_test_sps_hunt_profile_count(void);
int dsd_frame_sync_test_sps_hunt_profile_rate(int profile_index);
int dsd_frame_sync_test_sps_hunt_profile_levels(int profile_index);
int dsd_frame_sync_test_sps_hunt_next_index(const dsd_opts* opts, const dsd_state* state);
void dsd_frame_sync_test_apply_sps_hunt_profile(const dsd_opts* opts, dsd_state* state, int next_idx);
void dsd_frame_sync_test_ensure_enabled_sps_profile(const dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_test_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state);
int dsd_frame_sync_test_history_window(const char* symbols, int symbol_count, int window_length, char* out,
                                       int out_size);
int dsd_frame_sync_test_try_protocol_matches(dsd_opts* opts, dsd_state* state, const char* symbols, int symbol_count);
int dsd_frame_sync_test_active_profile_modulation(const dsd_opts* opts, const dsd_state* state);
int dsd_frame_sync_test_should_skip_snr_or_power_gate(const dsd_opts* opts, const dsd_state* state);
double dsd_frame_sync_test_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp);
void dsd_frame_sync_test_p25_slot_activity(const dsd_opts* opts, const dsd_state* state, time_t now, double nowm,
                                           double mac_hold, double ring_hold, double dt, int* left_active,
                                           int* right_active);
int dsd_frame_sync_test_hamming_distance_pattern(const char* symbols, const char* pattern, int len);
int dsd_frame_sync_test_best_ham_for_patterns(const char* symbols, const char* const patterns[], int pattern_count,
                                              int pattern_len, int best_start);
int dsd_frame_sync_test_best_nxdn_scaled_ham(const char* symbols10, int best_start);
void dsd_frame_sync_test_set_recent_hamming(int ham_c4fm, int ham_qpsk, int ham_gfsk);
void dsd_frame_sync_test_get_mod_votes(int* out_c4fm, int* out_qpsk, int* out_gfsk);
void dsd_frame_sync_test_auto_switch_modulation(const dsd_opts* opts, dsd_state* state, int t_max, int* lastt);
void dsd_frame_sync_test_reset_p25_trunk_tick_state(void);
void dsd_frame_sync_test_maybe_tick_p25_trunk_sm(dsd_opts* opts, dsd_state* state, time_t now);
#ifdef USE_RADIO
int dsd_frame_sync_test_rtl_profile_for_sps_index(const dsd_opts* opts, const dsd_state* state, int profile_index);
double dsd_frame_sync_test_active_profile_snr_db(const dsd_opts* opts, const dsd_state* state);
#endif
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H */
