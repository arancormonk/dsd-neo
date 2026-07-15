// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_TEST_SUPPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

int dsd_frame_sync_test_sps_hunt_profile_count(void);
int dsd_frame_sync_test_sps_hunt_profile_rate(int profile_index);
int dsd_frame_sync_test_sps_hunt_profile_levels(int profile_index);
int dsd_frame_sync_test_history_window(const char* symbols, int symbol_count, int window_length, char* out,
                                       int out_size);
int dsd_frame_sync_test_try_protocol_matches(dsd_opts* opts, dsd_state* state, const char* symbols, int symbol_count);
int dsd_frame_sync_test_eval_window(dsd_opts* opts, dsd_state* state, const char* symbols, const float* levels,
                                    int symbol_count);
void dsd_frame_sync_test_set_recent_hamming(int ham_c4fm, int ham_qpsk, int ham_gfsk);
void dsd_frame_sync_test_get_mod_votes(int* out_c4fm, int* out_qpsk, int* out_gfsk);
void dsd_frame_sync_test_reset_p25_trunk_tick_state(void);
int dsd_frame_sync_test_handle_no_sync_timeout(dsd_opts* opts, dsd_state* state, int synctest_pos);
#ifdef USE_RADIO
int dsd_frame_sync_test_rtl_profile_for_sps_index(const dsd_opts* opts, const dsd_state* state, int profile_index);
#endif

#ifdef __cplusplus
}
#endif

#endif
