// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_TEST_SUPPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int dsd_frame_sync_test_sps_hunt_profile_count(void);
int dsd_frame_sync_test_sps_hunt_profile_rate(int profile_index);
int dsd_frame_sync_test_sps_hunt_profile_levels(int profile_index);
int dsd_frame_sync_test_sps_hunt_profile_has_candidate(const dsd_opts* opts, int profile_index);
int dsd_frame_sync_test_history_window(const char* symbols, int symbol_count, int window_length, char* out,
                                       int out_size);
int dsd_frame_sync_test_try_protocol_matches(dsd_opts* opts, dsd_state* state, const char* symbols, int symbol_count);
int dsd_frame_sync_test_eval_window(dsd_opts* opts, dsd_state* state, const char* symbols, const float* levels,
                                    int symbol_count);
int dsd_frame_sync_test_capture_tetra_soft(const dsd_state* state, float* dst, int payload_len, int sync_len);
/* frame_sync_process_dibit_and_payload(): the hunt's slice of one symbol into dibit_buf and dmr_payload_buf. Returns
   the dibit as the history character ('0'..'3'). */
int dsd_frame_sync_test_process_dibit_and_payload(dsd_opts* opts, dsd_state* state, float symbol);
/* frame_sync_dmr_refresh_cleared_soft(): the soft metrics of the dibits a DMR sync re-read below the payload buffer's
   floor, from the symbol history. */
void dsd_frame_sync_test_dmr_refresh_cleared_soft(dsd_state* state, int readback);
void dsd_frame_sync_test_set_recent_hamming(int ham_c4fm, int ham_qpsk, int ham_gfsk);
void dsd_frame_sync_test_get_mod_votes(int* out_c4fm, int* out_qpsk, int* out_gfsk);
/** @brief Whether the 2 s QPSK dwell is currently armed. */
int dsd_frame_sync_test_qpsk_dwell_armed(void);
/* The modulation lock's application at getFrameSync() entry (frame_sync_apply_cli_mod_lock()). */
void dsd_frame_sync_test_apply_cli_mod_lock(const dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_test_reset_p25_trunk_tick_state(void);
int dsd_frame_sync_test_handle_no_sync_timeout(dsd_opts* opts, dsd_state* state, int synctest_pos);
/* frame_sync_hunt_budget_exit(): getFrameSync()'s step of the hunt once its budget has run a pass. */
int dsd_frame_sync_test_hunt_budget_exit(dsd_opts* opts, dsd_state* state);
void dsd_frame_sync_test_sps_hunt_note_handler_consumption(dsd_state* state);
/* The protocol matchers the hunt tries (k_frame_sync_tries), and the exact sync words each declares (NULL-terminated),
   which the tolerant P25 tries read a window's head against: *@p out_has_enabled says whether it declares the enable
   predicate they are read under, *@p out_reading how it reads them (0 as received, 1 under the CQPSK maps, 2 under
   TETRA's derotations). NULL for a matcher with no declaration and past the last. */
size_t dsd_frame_sync_test_try_count(void);
const char* const* dsd_frame_sync_test_try_words(size_t index, int* out_has_enabled, int* out_reading);
/* Whether matcher @p index runs at all for @p opts and @p state, by the enable predicate it declares. */
int dsd_frame_sync_test_try_enabled(dsd_opts* opts, dsd_state* state, size_t index);
#ifdef USE_RADIO
int dsd_frame_sync_test_rtl_profile_for_sps_index(const dsd_opts* opts, const dsd_state* state, int profile_index);
#endif

#ifdef __cplusplus
}
#endif

#endif
