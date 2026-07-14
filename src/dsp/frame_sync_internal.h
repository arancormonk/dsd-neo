// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void frame_sync_maybe_tick_p25_trunk_sm(dsd_opts* opts, dsd_state* state, time_t now);
void frame_sync_maybe_auto_switch_modulation(const dsd_opts* opts, dsd_state* state, int t_max, int* lastt);
int frame_sync_active_profile_modulation(const dsd_opts* opts, const dsd_state* state);
int frame_sync_should_skip_snr_or_power_gate(const dsd_opts* opts, const dsd_state* state);
int frame_sync_hamming_distance_pattern(const char* symbols, const char* pattern, int len);
int frame_sync_best_ham_for_patterns(const char* symbols, const char* const patterns[], int pattern_count,
                                     int pattern_len, int best_start);
int frame_sync_best_nxdn_scaled_ham(const char* symbols10, int best_start);
int frame_sync_sps_hunt_next_index(const dsd_opts* opts, const dsd_state* state);
void frame_sync_apply_sps_hunt_profile(const dsd_opts* opts, dsd_state* state, int next_idx, int preserve_modulation);
void frame_sync_ensure_enabled_sps_profile(const dsd_opts* opts, dsd_state* state);
void frame_sync_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state);
double frame_sync_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp);
void frame_sync_p25_slot_activity(const dsd_opts* opts, const dsd_state* state, time_t now, double nowm,
                                  double mac_hold, double ring_hold, double dt, int* left_active, int* right_active);
#ifdef USE_RADIO
double frame_sync_active_profile_snr_db(const dsd_opts* opts, const dsd_state* state);
#endif

#ifdef __cplusplus
}
#endif

#endif
