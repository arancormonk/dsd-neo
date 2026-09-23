// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_STREAM_TEST_SUPPORT_H_
#define DSD_NEO_SRC_IO_RADIO_RTL_STREAM_TEST_SUPPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/platform/threading.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install only while the stream is stopped. NULL restores platform creation. */
void rtl_stream_test_set_thread_create(int (*create)(dsd_thread_t*, dsd_thread_fn, void*));
int rtl_stream_test_has_resources(void);

int dsd_rtl_stream_test_request_retune(long int frequency, int timeout_ms);
/* Republish the cross-thread demod profile/TED mirrors after a test mutates
 * demod fields directly; public getters read the mirrors, not the fields. */
void rtl_stream_test_publish_demod_snapshot(void);
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
int rtl_stream_test_retune_mute_plan(uint32_t sample_rate_hz, int cfg_mute_ms, int cfg_mute_ms_is_set, int post_retune,
                                     int buffered_backend, uint32_t min_bytes);
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

typedef struct {
    const float* samples;
    size_t pairs;
    size_t capacity;
    size_t start;
    int mute;
    int hold;
    uint64_t hardware_drops;
} rtl_device_test_airspy_request;

int rtl_device_test_airspy_ingest(const rtl_device_test_airspy_request* request, float* output, size_t* output_count,
                                  uint64_t* dropped);
int rtl_stream_test_passes_for_actual_rate(uint32_t actual_rate_hz, int rate_in_hz);
int rtl_stream_test_digital_resample_chain(int output_kind, int rate_out_hz, int resamp_target_hz, int symbol_rate_hz,
                                           int digital_resample_mode, int capture_rate_device_forced,
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

typedef struct rtl_stream_test_finalize_profile_result {
    int symbol_rate_hz;
    int symbol_levels;
    int ted_sps;
    int ted_sps_override;
    int sps_is_integer;
    int channel_lpf_profile;
} rtl_stream_test_finalize_profile_result;

/* Seed the published symbol profile, then run the retune finalize path with the given decoder
 * options (NULL models a replay RESET) and report the profile the front end ended up on. */
int rtl_stream_test_finalize_rate_chain_profile(const dsd_opts* opts, int rate_out_hz, int seed_symbol_rate_hz,
                                                int seed_symbol_levels, int seed_channel_profile,
                                                rtl_stream_test_finalize_profile_result* out_result);

/* Demod fields that define a receive family, as a fresh stream open would leave them. */
typedef struct rtl_stream_test_demod_fields {
    int output_kind;
    int cqpsk_enable;
    int demod_is_fm;
    int demod_is_qpsk;
    int deemph;
    int deemph_a_q15;
    int audio_lpf_enable;
    int channel_lpf_enable;
    int channel_lpf_profile;
    int channel_lpf_width_hz;
    int analog_family;
    int analog_demod;
    int symbol_rate_hz;
    int symbol_levels;
    int ted_enabled;
    int ted_sps;
    int resamp_enabled;
    int resamp_l;
    int resamp_m;
    int output_rate;
    int fsk_sample_rate_hz;
    int fsk_symbol_rate_hz;
    int fsk_levels;
    int fsk_channel_profile;
    /* Carrier and timing loops, in micro-radians: a fresh open starts them from zero, and the Gardner TED waits to
       initialise from the SPS on its first block. */
    int costas_freq_urad;
    int costas_phase_urad;
    int fll_freq_urad;
    int fll_phase_urad;
    int ted_awaiting_init;
    /* Monitor audio state, in millionths: a fresh open starts de-emphasis, DC and the audio LPF at zero and the
       squelch envelope open (1000000). */
    int deemph_avg_u;
    int dc_avg_u;
    int audio_lpf_state_u;
    int squelch_env_u;
    int squelch_gate_open;
    /* 1 when the filter delay lines hold nothing (no resampler counts as clear), as after a fresh open. */
    int channel_hist_clear;
    int hb_hist_clear;
    int resamp_hist_clear;
} rtl_stream_test_demod_fields;

/* What svc_publish_symbol_profile() queues for a digital mode after the family request. */
typedef struct rtl_stream_test_digital_request {
    int cqpsk_enable;
    int symbol_rate_hz;
    int levels;
    int channel_profile;
    int ted_sps;
    /* 1: the demod thread reaches a block boundary between the family request and the symbol profile request. */
    int boundary_between_requests;
} rtl_stream_test_digital_request;

typedef struct rtl_stream_test_family_switch_result {
    rtl_stream_test_demod_fields fresh_digital;
    rtl_stream_test_demod_fields fresh_analog;
    rtl_stream_test_demod_fields switched_analog;
    rtl_stream_test_demod_fields switched_digital;
    int analog_request_rc;
    int digital_request_rc;
    int analog_deferred_until_consume; /* 1 when the live request left demod state alone until consumed */
    int digital_held_until_profile;    /* 1 when a boundary before the symbol profile left the analog family in place */
    uint32_t generation_before;
    uint32_t generation_after_analog;
    uint32_t generation_after_digital;
    size_t used_before;
    size_t used_after_analog;
    size_t used_after_digital;
    int published_analog_rc;
    int published_kind;
    int published_width_hz;
    int published_lpf_on;
    int published_after_digital_rc;
    unsigned int predicted_analog_output_rate;  /* rtl_stream_output_rate_for_family() before the analog switch */
    unsigned int predicted_digital_output_rate; /* ... and before the digital switch */
} rtl_stream_test_family_switch_result;

/* Open @p digital_opts at @p rate_hz, switch live to @p analog_opts and back (each request consumed the way the
 * demod thread consumes it between blocks), and report each state next to a fresh open of the same options. Before
 * each switch the session being left gets running carrier/timing loops and stale monitor audio and filter state.
 * @p forced_rate_out_hz > 0 has the device settle every open on that demod rate instead (a fixed rate grid, such as
 * Airspy's 78125 Hz), which puts the digital resampler under its forced-rate policy. */
int rtl_stream_test_analog_family_switch(const dsd_opts* digital_opts, const dsd_opts* analog_opts, int rate_hz,
                                         int forced_rate_out_hz, const rtl_stream_test_digital_request* digital_request,
                                         rtl_stream_test_family_switch_result* out);

typedef struct rtl_stream_test_width_change_result {
    int request_rc;
    int deferred_until_consume; /* 1 when the queued request left the channel width alone until consumed */
    int width_after;
    int lpf_enable_after;
    int output_kind_after;
    int analog_family_after;
    int plan_invalidated; /* the consume dropped the channel-filter plan (taps and width) */
    int channel_hist_cleared;
    int hb_hist_cleared;
    int published_width_hz;
    int published_lpf_on;
    uint32_t generation_before;
    uint32_t generation_after;
    size_t used_before;
    size_t used_after;
    /* A second request for the width already running. */
    int same_width_rc;
    int same_width_kept_histories; /* 1 when that request left the (re-seeded) filter histories alone */
    int same_width_kept_plan;
} rtl_stream_test_width_change_result;

/* Run an analog stream at @p rate_hz with NFM width @p width_before_hz (0 = default), give it a designed channel plan
 * and stale filter histories, queue a width-only request for @p width_after_hz while it runs, consume it at a
 * demod-thread block boundary, then queue the same width again. */
int rtl_stream_test_analog_width_change(int rate_hz, int width_before_hz, int width_after_hz,
                                        rtl_stream_test_width_change_result* out);

/* With no stream running and @p stale_rate_out_hz left in the published rate mirror by an earlier session, ask for an
 * analog profile (@p kind, @p width_hz) through the live request and through a retune profile. Returns the live
 * request's result and stores the retune profile's in @p out_retune_rc; any profile queued is discarded. */
int rtl_stream_test_analog_request_without_stream(int stale_rate_out_hz, int kind, int width_hz, int* out_retune_rc);

typedef struct rtl_stream_test_audio_reset_result {
    float deemph_avg;
    float dc_avg;
    float audio_lpf_state;
    float squelch_env;
    int channel_hist_cleared;
    int hb_hist_cleared;
    int resamp_hist_cleared;
    float deemph_a_before;
    float deemph_a_after;
    float audio_lpf_alpha_before;
    float audio_lpf_alpha_after;
    int channel_lpf_width_before;
    int channel_lpf_width_after;
    int channel_lpf_enable_after;
    int published_width_after;
    int published_lpf_on_after;
} rtl_stream_test_audio_reset_result;

/* Seed an analog monitor at @p rate_before_hz (NFM width @p nfm_width_hz, 0 = default) with stale filter state and
 * run the retune finalize path after the device settled on @p rate_after_hz (75 us de-emphasis, 3 kHz audio LPF). */
int rtl_stream_test_audio_monitor_retune(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                         rtl_stream_test_audio_reset_result* out);

typedef struct rtl_stream_test_retune_analog_result {
    int queued_rc;
    int taken;
    int profile_family;
    int profile_kind;
    int profile_width_hz;
    uint32_t profile_target_hz;
    int applied_family;
    int applied_kind;
    int applied_width_hz;
    int applied_output_kind;
    int applied_lpf_enable;
    int applied_cqpsk_enable;
    int applied_demod_is_fm;
    int other_target_left_alone;
} rtl_stream_test_retune_analog_result;

/* Queue an analog profile for @p target_hz on a digital stream, take it the way the controller does, finalize the
 * retune with it, and report what the profile carried and what the demodulator ended on. A second profile bound to a
 * different frequency must not apply. With @p with_cqpsk_symbol_profile a P25 CQPSK symbol profile is queued for the
 * same target first, the combined shape the retune-profile API documents. */
int rtl_stream_test_retune_analog_profile(uint32_t target_hz, int family, int kind, int width_hz,
                                          int with_cqpsk_symbol_profile, rtl_stream_test_retune_analog_result* out);

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
