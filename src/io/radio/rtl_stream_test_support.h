// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_STREAM_TEST_SUPPORT_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_STREAM_TEST_SUPPORT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int dsd_rtl_stream_test_request_retune(long int frequency, int timeout_ms);
int rtl_stream_test_prepare_reconfigure_input(size_t queued_samples, size_t* out_used_after,
                                              uint32_t* out_generation_before, uint32_t* out_generation_after);
int rtl_stream_test_retune_output_pending(size_t queued_samples, int cached_symbols, size_t* out_ring_pending,
                                          int* out_cache_pending, int* out_drained);
int rtl_stream_test_tune_result_output_drain(int tune_result, size_t queued_samples, int cached_symbols,
                                             size_t* out_used_after, int* out_cache_pending_after,
                                             uint32_t* out_generation_before, uint32_t* out_generation_after);
int rtl_stream_test_tune_timeout_read_gate(size_t queued_samples, int* out_read_while_pending,
                                           size_t* out_used_while_pending, int* out_read_after_failed_completion,
                                           int* out_read_after_recovery, uint32_t* out_generation_before,
                                           uint32_t* out_generation_after_gate);
int dsd_rtl_stream_test_tune_completion_result(int wait_result, int completion_result);
int dsd_rtl_stream_test_manual_retune_completion_result(int retune_rc, int reconfigured, uint32_t target_hz,
                                                        uint32_t applied_freq_hz);
int dsd_rtl_stream_test_tune_failure_reconciles_applied(uint32_t requested_freq_hz, uint32_t applied_freq_hz,
                                                        long int* out_opts_freq, uint32_t* out_capture_freq_hz);
int dsd_rtl_stream_test_capture_settings_failure_restore(uint32_t* out_full_freq_hz, uint32_t* out_full_rate_hz,
                                                         int* out_full_rate_out_hz, uint32_t* out_partial_freq_hz,
                                                         uint32_t* out_partial_rate_hz, int* out_partial_rate_out_hz);
int dsd_rtl_stream_test_ppm_store_if_applied(int ppm_rc, int requested_ppm, int* out_ppm_error);
int dsd_rtl_stream_test_retune_completion_result_binding(int* out_first_result, int* out_second_result);
int rtl_stream_test_clear_output(size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                 int* out_cache_pending_after, uint32_t* out_generation_before,
                                 uint32_t* out_generation_after);
int rtl_stream_test_clear_output_fsk_reset(size_t queued_samples, int* out_have_prev_after_clear,
                                           int* out_consumed_reset, int* out_have_prev_after_consume);

typedef struct rtl_stream_test_cqpsk_toggle_result {
    size_t used_after;
    int cache_pending_after;
    uint32_t generation_before;
    uint32_t generation_after;
    int output_kind_after;
    int fsk_reset_pending_after_toggle;
    int reset_consumed;
    int have_prev_after_consume;
} rtl_stream_test_cqpsk_toggle_result;

int rtl_stream_test_cqpsk_toggle_output_clear(int start_cqpsk, int target_cqpsk, int active_rtl_digital,
                                              size_t queued_samples, int cached_symbols,
                                              rtl_stream_test_cqpsk_toggle_result* out_result);
int rtl_stream_test_fsk_cfo_snapshot(double dc_rad_per_sample, int rate_out_hz, double* out_cfo_hz,
                                     int* out_after_generation_bump_available, int* out_after_reset_available);
int rtl_stream_test_fsk_snr_sps(int rate_out_hz, int symbol_rate_hz, int stale_ted_sps);
int rtl_stream_test_direct_output_rate_after_open_update(int output_kind, int rate_out_hz, int resamp_target_hz,
                                                         unsigned int* out_rate_hz, int* out_resamp_enabled);
int rtl_stream_test_source_policy_matrix(int* out_kind, int* out_rtltcp, int* out_soapy, int* out_replay,
                                         int* out_family, size_t count, char* out_names, size_t names_size,
                                         char* out_soapy_args, size_t args_size);
int rtl_stream_test_mode_policy_matrix(int* out_values, size_t count);
int rtl_stream_test_fsk_profile_policy_matrix(int* out_profiles, size_t count);
uint64_t rtl_stream_test_replay_acknowledge_discarded_span(uint64_t submitted_gen, uint64_t consumed_gen);
int rtl_stream_test_fsk_reacquire(int output_kind, size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                  int* out_cache_pending_after, uint32_t* out_generation_before,
                                  uint32_t* out_generation_after, int* out_request_rc, int* out_consumed);

typedef struct rtl_stream_test_cqpsk_reacquire_result {
    size_t used_after;
    int cache_pending_after;
    uint32_t generation_before;
    uint32_t generation_after;
    int request_rc;
    int second_request_rc;
    int consumed;
    int second_consumed;
    float fll_freq_before;
    float fll_freq_after;
    float fll_phase_after;
    float costas_freq_after;
    float costas_phase_after;
    float costas_error_after;
    float ted_mu_after;
    float ted_delay_after;
    float diff_prev_r_after;
    float diff_prev_j_after;
    float cqpsk_agc_before;
    float cqpsk_agc_after;
    int resamp_phase_after;
    int histories_cleared;
    int output_kind_after;
    int symbol_rate_after;
    int channel_profile_after;
    int ted_sps_after;
} rtl_stream_test_cqpsk_reacquire_result;

int rtl_stream_test_cqpsk_reacquire(int active_cqpsk, int symbol_rate_hz, int ted_sps, size_t queued_samples,
                                    int cached_symbols, rtl_stream_test_cqpsk_reacquire_result* out_result);

typedef struct rtl_stream_test_fll_retune_result {
    float fll_freq_before;
    float fll_freq_after;
    float retained_fll_scale;
    int reset_retained_fll;
    int restored_cached_fll;
    int distant_frequency_reason;
} rtl_stream_test_fll_retune_result;

int rtl_stream_test_fll_retune_policy(uint32_t previous_center_freq_hz, uint32_t next_center_freq_hz,
                                      int previous_rate_out_hz, int next_rate_out_hz,
                                      rtl_stream_test_fll_retune_result* out_result);

typedef struct rtl_stream_test_fll_retune_cache_result {
    int first_hop_reset;
    float first_hop_fll_after;
    int cc_restore_used_cache;
    float cc_restore_fll_after;
    float expected_cc_fll;
    int vc_restore_used_cache;
    float vc_restore_fll_after;
    float expected_vc_fll;
} rtl_stream_test_fll_retune_cache_result;

int rtl_stream_test_fll_retune_cache_round_trip(rtl_stream_test_fll_retune_cache_result* out_result);
int rtl_stream_test_retune_profile_request_binding(int* out_first_profile, int* out_second_profile,
                                                   uint32_t* out_first_freq_hz, uint32_t* out_second_freq_hz,
                                                   uint32_t* out_first_request_id, uint32_t* out_second_request_id);
int rtl_stream_test_retune_profile_coalesced_no_profile(int* out_profile, uint32_t* out_profile_freq_hz,
                                                        uint32_t* out_manual_freq_hz, uint32_t* out_request_id,
                                                        uint32_t* out_coalesced_request_id);
int rtl_stream_test_tagged_retune_ownership(uint64_t owner_token, uint64_t contender_token, int terminal_result,
                                            uint32_t* out_owner_freq_hz, uint32_t* out_profile_freq_hz,
                                            uint64_t* out_owner_token, int* out_completion_result);
int dsd_rtl_stream_test_retune_without_controller_rejected(void);
int rtl_stream_test_retune_profile_gain_binding(int* out_gain_is_set, int* out_gain_tenth_db, int* out_gain_is_auto,
                                                int* out_autogain_is_set, int* out_autogain_on);

typedef struct rtl_stream_test_replay_state {
    int replay_input_eof;
    int replay_input_drained;
    int replay_demod_drained;
    int replay_output_drained;
    int replay_forced_stop;
    int should_exit;
    uint64_t replay_last_submit_gen;
    uint64_t replay_last_submit_gen_at_eof;
    uint64_t replay_last_consume_gen;
    size_t input_ring_used;
    size_t output_ring_used;
    uint32_t replay_event_retune_count;
    uint32_t replay_event_mute_count;
    uint32_t replay_event_reset_count;
    uint32_t replay_event_last_frequency_hz;
    uint64_t replay_event_last_mute_bytes;
    int replay_event_last_reset_reason;
    uint32_t replay_loop_restart_count;
    uint32_t replay_loop_restart_last_frequency_hz;
} rtl_stream_test_replay_state;

int dsd_rtl_stream_test_get_replay_state(rtl_stream_test_replay_state* out_state);
int rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev);

#ifdef __cplusplus
}
#endif

#endif
