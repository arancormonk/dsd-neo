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
int dsd_rtl_stream_test_manual_retune_completion_result(int retune_rc, int reconfigured, int refused,
                                                        uint32_t target_hz, uint32_t applied_freq_hz);
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

/* A return to the NFM monitor at 16 kHz, checked at 48 kHz and refused where it landed at 16 kHz (issue #578), by a
 * stream on the analog family: what rtl_stream_receive_request_refusal() says it kept, and what the stream publishes
 * then, which a leave the front end refused at once reads instead: the analog kind and width setting the family runs
 * (rtl_stream_get_analog_setting()) and whether the monitor output runs (rtl_stream_get_analog_profile()). */
typedef struct rtl_stream_test_monitor_return_refusal {
    int kept_analog;
    int kept_monitor;
    int kept_kind;
    int kept_width_hz;
    int published_family;
    int published_kind;
    int published_width_hz;
    int published_monitor;
} rtl_stream_test_monitor_return_refusal;

/* What became of receive requests on a running stream (rtl_stream_receive_request_outcome()), each consumed the way
 * the demod thread consumes them between blocks. */
typedef struct rtl_stream_test_rx_request_result {
    uint32_t first_seq;                /* the number of the CQPSK-on request */
    int queued_outcome;                /* its outcome right after it was queued */
    int outcome_across_generation;     /* after the output generation moved (a retune) with nothing taken */
    int published_cqpsk_while_pending; /* rtl_stream_get_cqpsk_status() while it is pending: the stream before it */
    int outcome_after_consume;         /* once the demod thread took it */
    int published_cqpsk_after_consume; /* rtl_stream_get_cqpsk_status() then */
    int replaced_outcome;              /* a request a later one replaced before a consume, once that one is taken */
    int analog_request_rc;             /* an NFM width the published demod rate filters, queued */
    int analog_outcome;                /* that request, taken at a demod rate that cannot filter it */
    int after_refused_outcome;         /* a request queued after the refused one, once taken */
    int refused_outcome_kept;          /* the refused request's outcome after that later one settled */
    int open_outcome;                  /* a queued request once a stream open drops the queue */
    int no_stream_outcome;             /* a queued request once a request is made with no pipeline running */
    int second_seq_follows;            /* 1 when each queued request took the next number */
    int requested_cqpsk_while_pending; /* rtl_stream_requested_cqpsk() with the CQPSK-on request pending */
    int requested_cqpsk_analog_queued; /* ... with the analog request queued over CQPSK on */
    int requested_cqpsk_after_refusal; /* ... once that request was refused: the CQPSK state the stream kept */
    /* The receive family (dsd_rx_family) the queued requests leave the stream on (issue #583), at the same three
       points: the CQPSK-on request on the digital stream, the analog request over AM, and once it was refused. */
    int requested_family_while_pending;
    int requested_family_analog_queued;
    int requested_family_after_refusal;
    int requested_family_entry_queued; /* a switch onto the monitor queued on the digital stream */
    int requested_family_entry_kept;   /* ... once refused where it landed: the digital family the stream kept */
    int requested_family_digital_held; /* a switch to the digital family the demod thread holds for its profile */
    int refusal_reported;              /* rtl_stream_receive_request_refusal() for the refused request */
    int kept_analog_family;            /* the family it says the stream kept (on the analog family, at 12.5 kHz) */
    int kept_width_hz;                 /* the analog width it says the stream kept */
    int kept_kind;                     /* the analog kind it says the stream kept (AM: the kind it ran when an NFM
                                          request was refused) */
    int kept_monitor;                  /* whether it says the stream kept the monitor output (CQPSK on there: 0) */
    int settled_refusal_reported;      /* rtl_stream_receive_request_refusal() for a settled request */
    int entry_kept_analog_family;      /* the family kept when a switch onto the monitor was refused */
    int entry_kept_monitor;            /* ... and the monitor output it did not keep */
    uint32_t refused_seq;              /* the number of that refused switch */
    int refused_outcome_after_open;    /* its outcome once a stream open has run */
    /* A return to the NFM monitor refused where it landed (rtl_stream_test_monitor_return_refusal), by a stream on the
       NFM monitor at 12.5 kHz, by one on the analog family under a typed digital row's channel profile with a 16 kHz
       NFM setting, and by one running AM at 10 kHz under that profile (a switch to NFM armed under the row). */
    rtl_stream_test_monitor_return_refusal monitor_return;
    rtl_stream_test_monitor_return_refusal typed_row_return;
    rtl_stream_test_monitor_return_refusal am_row_return;
    /* An NFM width replaced in the queue by a switch to the digital family, which the demod thread holds for its symbol
       profile, and that switch replaced in turn by the same width again, which the demod thread takes at a demod rate
       that cannot filter it (issue #578): what each reads. */
    uint32_t analog_replaced_seq;           /* the number of the first width */
    int analog_replaced_pending_outcome;    /* the first width while the switch that replaced it is held */
    int analog_replaced_outcome;            /* the first width once the last one is taken */
    int digital_replaced_outcome;           /* the switch to the digital family then */
    int analog_replacing_outcome;           /* the last width, taken and refused */
    int analog_replaced_outcome_after_open; /* the first width once a stream open has run */
} rtl_stream_test_rx_request_result;

int rtl_stream_test_rx_request_outcomes(rtl_stream_test_rx_request_result* out);
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
    int demod_is_am; /* dsd_demod_am_active(): the AM envelope detector */
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
    int sps_is_integer; /* the complex demod rate is a whole multiple of the symbol rate */
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
    int squelch_hits;         /* consecutive squelched blocks toward a multi-frequency hop: 0 after an open */
    int am_carrier_u;         /* the AM detector's carrier estimate, in millionths: 0 (cold) after an open */
    int am_squelched_samples; /* the AM detector's closed-squelch run: 0 after an open */
    /* 1 when the filter delay lines hold nothing (no resampler counts as clear), as after a fresh open. */
    int channel_hist_clear;
    int hb_hist_clear;
    int resamp_hist_clear;
    /* 1 when the I/Q DC and I/Q balance estimates are zero, as an open starts them. */
    int iq_correction_clear;
    /* 1 when the post-demod decimator's delay line, head and phase are at their start (none counts as clear). */
    int post_decim_clear;
} rtl_stream_test_demod_fields;

/* A symbol profile a -fA session applies on its own, without a family request, which moves the front end off the
 * analog monitor while the stream stays on the analog family. */
enum {
    RTL_STREAM_TEST_UNDER_ANALOG_NONE = 0,
    RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE = 1, /* the DSP menu's CQPSK toggle: symbols instead of monitor audio */
    RTL_STREAM_TEST_UNDER_ANALOG_TYPED_ROW = 2,    /* a typed DMR scan row: monitor output on the row's channel */
    /* The DSP menu's CQPSK toggle still queued, not yet consumed, when the digital mode is picked (both commands in
       one drain of the decoder's command queue): the family request must not land on it. */
    RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE_QUEUED = 3,
};

/* What svc_publish_symbol_profile() queues for a digital mode after the family request. */
typedef struct rtl_stream_test_digital_request {
    int cqpsk_enable;
    int symbol_rate_hz;
    int levels;
    int channel_profile;
    int ted_sps;
    /* 1: the demod thread reaches a block boundary between the family request and the symbol profile request. */
    int boundary_between_requests;
    /* RTL_STREAM_TEST_UNDER_ANALOG_*: a profile the -fA session had applied before the digital mode was picked
       (rtl_stream_test_analog_start_family_switch() only). */
    int profile_under_analog;
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
    /* Output kind once the digital session, after the switch, has had a CQPSK symbol profile applied and then a C4FM
       one: a fresh digital open comes back to the FSK discriminator. */
    int output_kind_after_cqpsk_round_trip;
    /* The two legs of that round trip, CQPSK then C4FM: the output kind each ran, and the output rate after it. */
    int round_trip_output_kind[2];
    int round_trip_output_rate[2];
    /* The DSP menu's CQPSK toggle (a CQPSK flip with no symbol profile, as apply_dsp_op_cqpsk_toggle() queues it) made
       twice, right after the switch and on a fresh open of the digital mode: the channel profile, output kind and
       symbol levels after each. Turning CQPSK off returns to the FSK channel profile an open picks from the decode
       modes it runs. */
    int switched_toggle_channel_profile[2];
    int switched_toggle_output_kind[2];
    int switched_toggle_levels[2];
    int fresh_toggle_channel_profile[2];
    int fresh_toggle_output_kind[2];
    int fresh_toggle_levels[2];
    /* With a profile_under_analog: the -fA session once that profile landed, before the digital mode was picked. */
    int under_analog_output_kind;
    int under_analog_channel_profile;
    int under_analog_family;        /* demod_state::analog_family */
    int under_analog_published;     /* rtl_stream_get_analog_profile() */
    int under_analog_family_active; /* rtl_stream_analog_family_active(), what the decoder times the switch by */
    /* After the switch: rtl_stream_analog_family_active() */
    int family_active_after_digital;
    /* rtl_stream_test_analog_start_family_switch() only: the same running stream switched back onto the analog
       family it started on (the start's kind and width, after stale monitor state was seeded again), as the operator
       picking the analog preset again does. reentered_* are its request, its demod state and what it published. */
    int reentered_analog_request_rc;
    rtl_stream_test_demod_fields reentered_analog;
    int reentered_published_rc;
    int reentered_published_kind;
    int reentered_published_width_hz;
} rtl_stream_test_family_switch_result;

/* Open @p digital_opts at @p rate_hz, switch live to @p analog_opts and back (each request consumed the way the
 * demod thread consumes it between blocks), and report each state next to a fresh open of the same options. Before
 * each switch the session being left gets running carrier/timing loops and stale monitor audio and filter state.
 * @p forced_rate_out_hz > 0 has the device settle every open on that demod rate instead (a fixed rate grid, such as
 * Airspy's 78125 Hz), which puts the digital resampler under its forced-rate policy. The stream keeps the options
 * snapshot it opened with throughout, as the orchestrator's private copy does in a real session: only the family
 * requests, and the digital decode modes the decoder notes before its digital one (rtl_stream_set_digital_decode_modes()
 * with @p digital_opts, as svc_publish_symbol_profile() notes them), tell it about the switches. The DSP menu's CQPSK
 * toggle is made twice on the fresh digital open (a stream that opened with @p digital_opts) and after the switch. */
int rtl_stream_test_analog_family_switch(const dsd_opts* digital_opts, const dsd_opts* analog_opts, int rate_hz,
                                         int forced_rate_out_hz, const rtl_stream_test_digital_request* digital_request,
                                         rtl_stream_test_family_switch_result* out);

/* The session a -fA start leaves before a digital mode is picked: open @p analog_opts at @p rate_hz, give it running
 * carrier/timing loops and stale monitor audio and filter state, and switch live to @p digital_opts through
 * @p digital_request. Fills fresh_digital (a fresh open of @p digital_opts), fresh_analog (the -fA open the session
 * started from), switched_digital and the digital-switch fields; the analog request fields stay zero, and
 * generation_after_analog is the generation the -fA session ran at (after the profile_under_analog, if any, which is
 * applied and consumed at a block boundary before the stale state is seeded, and reported in the under_analog_*
 * fields; RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE_QUEUED is queued the same way but left unconsumed, so the
 * under_analog_* fields still show the monitor). Then the digital session, with stale state seeded again, is switched
 * back onto @p analog_opts' analog profile (the reentered_* fields). */
int rtl_stream_test_analog_start_family_switch(const dsd_opts* digital_opts, const dsd_opts* analog_opts, int rate_hz,
                                               int forced_rate_out_hz,
                                               const rtl_stream_test_digital_request* digital_request,
                                               rtl_stream_test_family_switch_result* out);

/* rtl_stream_test_analog_start_family_switch() on a -fA session at @p rate_hz (no forced rate), with a retune landing
 * between the decoder's digital requests (timed for the rate the stream published when they were made) and the demod
 * thread's consume: the device settles the retune on @p landed_rate_out_hz and the rate chain is finalized as the
 * controller finalizes one. fresh_digital is an open of @p digital_opts the device forces to @p landed_rate_out_hz. */
int rtl_stream_test_analog_start_family_switch_across_retune(const dsd_opts* digital_opts, const dsd_opts* analog_opts,
                                                             int rate_hz, int landed_rate_out_hz,
                                                             const rtl_stream_test_digital_request* digital_request,
                                                             rtl_stream_test_family_switch_result* out);

/* Channel profile after the DSP menu's CQPSK toggle turned CQPSK on and off again, for a P25 C4FM open (at 48 kHz)
 * whose decoder has noted D-STAR as its digital modes (rtl_stream_set_digital_decode_modes()). */
typedef struct rtl_stream_test_noted_modes_result {
    int unswitched_profile; /* no live family switch yet */
    int switched_profile;   /* after a live switch to analog and back to digital (D-STAR's symbol profile) */
    int reopened_profile;   /* the same switches after a new open, which drops the note, with no note since */
} rtl_stream_test_noted_modes_result;

int rtl_stream_test_noted_digital_modes_scope(rtl_stream_test_noted_modes_result* out);

/* A live switch to analog whose ring clear meets a decoder read in flight. */
typedef struct rtl_stream_test_read_race_result {
    int paused;                  /* 1 when the read stopped between its copy and its tail store */
    int switch_done_during_read; /* 1 when the switch finished while the read was still stopped there */
    int read_got;                /* what the live read returned (0: discarded, the generation moved under it) */
    int analog_family_after;     /* demod_state::analog_family once both finished */
    size_t used_before;
    size_t used_after; /* samples the ring reports once both finished */
    size_t tail_after;
    size_t head_after;
    uint32_t generation_before;
    uint32_t generation_after;
} rtl_stream_test_read_race_result;

/* Open DMR at 48 kHz with a seeded output ring and queue a switch to analog. A decoder-side live read then stops
 * between copying samples and publishing its tail, and another thread consumes the switch as the demod thread would
 * (its ring clear included). The read stays stopped until the switch finished or @p hold_ms passed, then completes. */
int rtl_stream_test_family_switch_during_live_read(int hold_ms, rtl_stream_test_read_race_result* out);

/* A live switch to analog whose ring clear meets a decoder read that reaches the ring before it. */
typedef struct rtl_stream_test_clear_race_result {
    int paused;                  /* 1 when the switch stopped in its ring clear, before taking ready_m */
    int switch_done_during_read; /* 1 when the switch finished before the read did */
    int read_got;                /* what the live read returned */
    int analog_family_after;     /* demod_state::analog_family once the switch finished */
    size_t used_before;
    size_t used_after; /* samples the ring reports once the switch finished */
    size_t tail_after;
    size_t head_after;
    uint32_t generation_before; /* before the switch */
    uint32_t read_generation;   /* the generation the read ran under */
    uint32_t generation_after;  /* once the switch finished */
} rtl_stream_test_clear_race_result;

/* Open DMR at 48 kHz with a seeded output ring and queue a switch to analog. Another thread consumes the switch as the
 * demod thread would and stops in its ring clear after the clear's first generation bump, before it takes ready_m;
 * a decoder-side live read then runs from start to end, and the switch is released to finish. */
int rtl_stream_test_live_read_during_family_switch_clear(rtl_stream_test_clear_race_result* out);

typedef struct rtl_stream_test_digital_row_result {
    int open_rc;
    /* After the row's symbol profile landed on the -fA session. */
    int row_output_kind;
    int row_analog_family;    /* demod_state::analog_family */
    int row_published_family; /* rtl_stream_get_analog_profile(), what the decoder sees */
    int row_output_rate;
    int row_resamp_l;
    int row_resamp_m;
    /* A digital family request with no symbol profile behind it, at one block boundary. */
    int lone_request_rc;
    int lone_request_held; /* 1 when it stayed queued, waiting for a symbol profile */
    /* The row's symbol profile a scoped command republishes during the row, with no family request: on an analog
       session the decoder asks for the digital family only when its configured mode is digital. */
    int republish_rc;
    uint32_t generation_before;
    uint32_t generation_after;
    size_t used_before;
    size_t used_after;
    int output_kind_after;
    int output_rate_after;
    int resamp_l_after;
    int resamp_m_after;
    /* The -fA baseline requested back when the row is left. */
    int restore_rc;
    int restored_output_kind;
    int restored_published_family;
    int restored_output_rate;
    uint32_t generation_after_restore;
} rtl_stream_test_digital_row_result;

/* An analog session at @p rate_hz on a typed DMR scan row that queued only its symbol profile: the front end keeps
 * the monitor output, with the row's channel profile in place of the analog channel, while the analog family flag is
 * still set. Queue a lone digital family request (held for a symbol profile, then dropped), then republish the row's
 * symbol profile as a scoped command on the analog session does, then request the -fA baseline back as the row's
 * leave does; each consumed at a demod-thread block boundary. The session is a -fA open, or with @p start_digital a
 * DMR open switched live to analog first; either way the stream keeps the options snapshot it opened with, as a real
 * session's does. */
int rtl_stream_test_digital_row_on_analog_session(int rate_hz, int start_digital,
                                                  rtl_stream_test_digital_row_result* out);

typedef struct rtl_stream_test_am_symbol_profile_result {
    int open_rc;
    int am_on_open;         /* dsd_demod_am_active() on the -fM open */
    int am_after_cqpsk_off; /* ... after a CQPSK-off toggle on that monitor, whose CQPSK is off already */
    /* Under a typed digital row's CQPSK-off symbol profile, consumed at a block boundary. */
    int row_monitor;      /* dsd_demod_analog_monitor_active() */
    int row_am;           /* dsd_demod_am_active(): 0, full_demod() reads the row's signal with the discriminator */
    int row_kind;         /* demod_state::analog_demod */
    int am_after_restore; /* dsd_demod_am_active() once a failed tune's restore put the monitor's profile back */
    int am_after_width_request; /* ... once an AM width request on that monitor was consumed */
    int width_after_request;    /* demod_state::channel_lpf_width_hz, likewise */
    /* Retunes landed as the controller lands them: a typed digital row's CQPSK-off symbol profile, then the analog
       profile a row running the analog family queues for its target (dsd_engine_scan_tune_to_freq()). */
    int retune_row_am;         /* dsd_demod_am_active() once the row's retune landed */
    int retune_analog_am;      /* ... once the analog profile's retune landed */
    int retune_analog_monitor; /* dsd_demod_analog_monitor_active(), likewise */
    int retune_analog_width;   /* demod_state::channel_lpf_width_hz, likewise */
} rtl_stream_test_am_symbol_profile_result;

/* An AM monitor (-fM) opened at @p rate_hz, running live, taken through every CQPSK-off symbol profile a session can
 * put on it without a family or kind switch: a CQPSK toggle that finds CQPSK off already, a typed digital scan row's
 * profile, the restore of the monitor's own profile a failed tune queues, an AM width request after that, and the
 * same row profile and then an analog row's analog profile landed by retunes. */
int rtl_stream_test_am_monitor_symbol_profiles(int rate_hz, rtl_stream_test_am_symbol_profile_result* out);

typedef struct rtl_stream_test_typed_row_leave_result {
    int open_rc;
    int row_monitor;   /* dsd_demod_analog_monitor_active() with the outgoing typed row's profile on air */
    int taken;         /* the incoming typed row's retune was taken (by the controller, or by the external landing) */
    int leave_rc;      /* the leave's rtl_stream_request_analog_profile() */
    int leave_monitor; /* dsd_demod_analog_monitor_active() once the demod thread took the leave's request */
    int leave_outcome; /* rtl_stream_receive_request_outcome() of the leave's request then */
    /* Once the incoming row's retune landed, and published as the demod thread publishes. */
    int landed_family;          /* demod_state::analog_family */
    int landed_output_kind;     /* demod_state::output_kind */
    int landed_cqpsk_enable;    /* demod_state::cqpsk_enable */
    int landed_channel_profile; /* demod_state::channel_lpf_profile */
    int landed_monitor;         /* dsd_demod_analog_monitor_active() */
    int landed_published;       /* rtl_stream_get_analog_profile(), what the decoder sees */
    int landed_published_kind;
    int boundary_monitor; /* dsd_demod_analog_monitor_active() at the demod thread's next block boundary */
} rtl_stream_test_typed_row_leave_result;

/* A -Y scan on a -fA (@p kind FM) or -fM (AM) session left while a typed digital row's retune is in flight (issue
 * #582). The outgoing typed row's symbol profile is on air (the analog family, off the monitor). The incoming row's
 * retune queues its symbol profile (@p row_symbol_profile, RTL_STREAM_TEST_SYMBOL_DMR_FSK or _P25_CQPSK) with no
 * receive family, as a row does whose configured mode is analog, and the controller takes it. The leave then asks for
 * the monitor (rtl_stream_request_analog_profile()), which the demod thread takes at its next block boundary before the
 * retune lands, as it does when the controller starts the retune late. The retune lands last: on the controller, or
 * with @p external_backend as an external backend's retune does. With @p leave_before_queue the leave's request is made
 * before the retune's profile is queued instead, which leaves that profile newer than the request. */
int rtl_stream_test_typed_row_retune_across_leave(int kind, int row_symbol_profile, int leave_before_queue,
                                                  int external_backend, rtl_stream_test_typed_row_leave_result* out);

/* What the front end runs at one step of the family-less retune cases below. */
typedef struct rtl_stream_test_front_end_state {
    int analog_family;   /* demod_state::analog_family */
    int output_kind;     /* demod_state::output_kind */
    int cqpsk_enable;    /* demod_state::cqpsk_enable */
    int channel_profile; /* demod_state::channel_lpf_profile */
    int symbol_rate_hz;  /* demod_state::symbol_rate_hz */
    int monitor;         /* dsd_demod_analog_monitor_active() */
    int published;       /* rtl_stream_get_analog_profile() */
} rtl_stream_test_front_end_state;

typedef struct rtl_stream_test_digital_leave_result {
    int rc;            /* 0 when every request was accepted and the retune taken */
    int leave_outcome; /* rtl_stream_receive_request_outcome() of the leave's symbol profile request, once taken */
    rtl_stream_test_front_end_state configured;  /* the DMR session's own profile, before the scan row */
    rtl_stream_test_front_end_state after_leave; /* once the demod thread took the leave's requests */
    rtl_stream_test_front_end_state landed;      /* once the row's retune landed, late */
    rtl_stream_test_front_end_state after;       /* four demod block boundaries later */
} rtl_stream_test_digital_leave_result;

/* A -Y scan on a DMR session left while a typed row's retune is in flight (issue #582): the row's retune carries no
 * family (the configured mode is digital: a digital-only session's retunes never attach one), its symbol profile
 * @p row_symbol_profile (RTL_STREAM_TEST_SYMBOL_P25_CQPSK or _P25_C4FM_EXPLICIT). The controller has taken it and
 * starts it late. The leave (channel_scan_restore_frontend()) supersedes the family-less retunes outstanding
 * (rtl_stream_supersede_familyless_retunes()), then asks for the digital family (with @p landing, as the marked
 * landing) and the session's DMR symbol profile, which the demod thread takes before the retune lands. With
 * @p leave_before_queue the leave is made before the row's profile is queued instead. */
int rtl_stream_test_digital_leave_across_familyless_retune(int row_symbol_profile, int landing, int leave_before_queue,
                                                           rtl_stream_test_digital_leave_result* out);

typedef struct rtl_stream_test_older_analog_request_result {
    int rc;              /* 0 when every request was accepted and the retune taken */
    int request_outcome; /* rtl_stream_receive_request_outcome() of the older analog request, at the next boundary */
    rtl_stream_test_front_end_state landed; /* once the row's retune landed */
    rtl_stream_test_front_end_state after;  /* at the demod thread's next block boundary */
} rtl_stream_test_older_analog_request_result;

/* A width command drained on a -fA (@p kind FM) or -fM (AM) session just before the -Y scanner advanced to a typed
 * digital row (issue #582): a live analog request the demod thread has not taken, then the row's retune, whose symbol
 * profile @p row_symbol_profile carries no family (with @p attach_digital the digital family is attached instead, as a
 * digital configured session attaches it), which the controller lands before the demod thread's next block boundary. */
int rtl_stream_test_older_analog_request_across_familyless_retune(int kind, int row_symbol_profile, int attach_digital,
                                                                  rtl_stream_test_older_analog_request_result* out);

typedef struct rtl_stream_test_monitor_return_result {
    int open_rc;
    int cqpsk_on;                 /* demod_state::cqpsk_enable once the DSP menu's CQPSK-on toggle was taken */
    int cqpsk_output_kind;        /* demod_state::output_kind, likewise */
    int request_rc;               /* rtl_stream_request_analog_profile() for the return to the monitor, accepted */
    int accepted_cqpsk;           /* demod_state::cqpsk_enable once the analog request alone was taken */
    int accepted_monitor;         /* dsd_demod_analog_monitor_active(), likewise */
    int accepted_kind_active;     /* dsd_demod_am_active() == (kind == AM): the detector of the kind asked for runs */
    int accepted_width_hz;        /* demod_state::channel_lpf_width_hz, likewise */
    int accepted_requested_cqpsk; /* rtl_stream_requested_cqpsk() once it settled */
    int refused_request_rc;       /* the same return asked for again after CQPSK went back on: queued */
    int refused_outcome;          /* rtl_stream_receive_request_outcome() once taken at the lower landed rate */
    int refused_kept_analog;      /* rtl_stream_receive_request_refusal(): the analog family kept */
    int refused_kept_kind;        /* ... and its kind */
    int refused_cqpsk;            /* demod_state::cqpsk_enable after the refusal */
    int refused_output_kind;      /* demod_state::output_kind, likewise */
    int refused_channel_profile;  /* demod_state::channel_lpf_profile, likewise */
    int refused_requested_cqpsk;  /* rtl_stream_requested_cqpsk(), likewise */
} rtl_stream_test_monitor_return_result;

/* An analog monitor of @p kind opened at 48 kHz, running live, with the DSP menu's CQPSK toggle turned on and taken;
 * then the return to the monitor as svc_toggle_rtl_cqpsk() asks for it, the analog request with @p width_hz alone,
 * taken at the same rate; then CQPSK on again and the same request, taken after a retune moved the demod rate to
 * @p landed_rate_hz, which cannot filter the width. */
int rtl_stream_test_monitor_return_from_cqpsk(int kind, int width_hz, int landed_rate_hz,
                                              rtl_stream_test_monitor_return_result* out);

typedef struct rtl_stream_test_width_change_result {
    int request_rc;
    int deferred_until_consume; /* 1 when the queued request left the channel width alone until consumed */
    int width_after;
    int lpf_enable_after;
    int output_kind_after;
    int analog_family_after;
    int plan_invalidated;   /* the consume dropped the channel-filter plan (taps and width) */
    int channel_state_kept; /* 1 when the consume left the (seeded) channel history and pending count as they were */
    int hb_state_kept;      /* likewise every half-band stage's history and pending count */
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

/* A live width edit on a running NFM monitor, with its blocks run through full_demod(): the half-band cascade, the
 * channel filter, then a pass-through demodulator, so each block's output is the channel filter's (the I/Q DC blocker,
 * I/Q balance and the channel squelch are off). */
typedef struct rtl_stream_test_width_continuity_case {
    int rate_hz;         /* the demod rate the monitor opens at */
    int passes;          /* half-band passes ahead of the channel filter (1..10): the input runs at rate_hz << passes */
    int width_before_hz; /* the NFM width the monitor opens with */
    int width_after_hz;  /* the width the live request asks for */
    /* NULL, or the channel plan the blocks before the edit run in place of the opening width's design. At one rate
       every analog width designs the same tap count (the Blackman transition is fixed), so a seeded plan of another
       length is what makes the edit change the tap count. */
    const float* seeded_taps;
    int seeded_taps_len;
    const float* iq;        /* interleaved input, the sum of block_sizes complex samples */
    const int* block_sizes; /* complex samples per block, each at most MAXIMUM_BUF_LENGTH / 2 */
    int n_blocks;
    int edit_block; /* the request is queued and consumed at the block boundary before this block */
    float* out;     /* the channel filter's output over all the blocks, interleaved */
    int out_cap;    /* floats */
} rtl_stream_test_width_continuity_case;

typedef struct rtl_stream_test_width_continuity_result {
    int request_rc;              /* rtl_stream_request_analog_profile() for the new width, while the stream runs */
    int monitor_after;           /* dsd_demod_analog_monitor_active() once the edit was consumed */
    int width_after;             /* demod_state::channel_lpf_width_hz, likewise */
    int plan_dropped;            /* the consume dropped the running plan, so the next block designs the new width's */
    int channel_state_kept;      /* the consume left the channel history and pending count as they were */
    int hb_state_kept;           /* likewise every half-band stage's history and pending count */
    int channel_pending_at_edit; /* demod_state::channel_lpf_pending when the request was queued */
    int hb_pending_at_edit;      /* the sum of demod_state::hb_pending over the cascade's stages, likewise */
    int taps_before;             /* the plan's tap count on the last block before the edit */
    int taps_after;              /* the plan's tap count on the last block */
    int outputs_before_edit;     /* complex samples the channel filter made before the edit */
    int outputs;                 /* complex samples it made over all the blocks (in out) */
    int channel_pending_end;     /* demod_state::channel_lpf_pending after the last block */
} rtl_stream_test_width_continuity_result;

/* Open the NFM monitor described by @p c, run its blocks with the width edit consumed before c->edit_block, and record
 * the channel filter's output in c->out. Returns 0, or a negative value when the case is invalid, the open failed or
 * the output did not fit c->out_cap. */
int rtl_stream_test_analog_width_continuity(const rtl_stream_test_width_continuity_case* c,
                                            rtl_stream_test_width_continuity_result* out);

/* With no stream running and @p stale_rate_out_hz left in the published rate mirror by an earlier session, ask for an
 * analog profile (@p kind, @p width_hz) through the live request and through a retune profile. Returns the live
 * request's result and stores the retune profile's in @p out_retune_rc; any profile queued is discarded. */
int rtl_stream_test_analog_request_without_stream(int stale_rate_out_hz, int kind, int width_hz, int* out_retune_rc);

typedef struct rtl_stream_test_live_request_result {
    int check_rc;       /* rtl_stream_check_analog_profile() for the same profile, asked before the request */
    int request_rc;     /* rtl_stream_request_analog_profile() while the stream runs */
    int request_queued; /* 1 when that request left something queued for the demod thread */
    int family_before;  /* demod_state::analog_family before the request ... */
    int family_after;   /* ... and after one demod-thread block boundary */
    int width_before;   /* demod_state::channel_lpf_width_hz, likewise */
    int width_after;
    int plan_kept; /* 1 when the boundary left the running channel plan alone */
    int published_width_before;
    int published_width_after;
    int retune_rc;     /* rtl_stream_prepare_retune_analog_profile_for_target() while the stream runs */
    int retune_queued; /* 1 when a retune profile for the target was left pending */
    int monitor_after; /* dsd_demod_analog_monitor_active() after the boundary */
    int published_lpf_on_after;
    int request_rate_hz; /* rtl_stream_get_request_rate_hz() while the stream runs */
} rtl_stream_test_live_request_result;

/* Run a stream at @p rate_hz (the analog monitor at its default width, or a DMR session when @p analog_stream is 0)
 * with @p post_downsample published as an I/Q replay sidecar would set it, give it a designed channel plan, and ask
 * it while it runs for the analog profile (@p kind, @p width_hz): first through the check a decode-mode change makes
 * before it commits, then as a live request consumed at one demod-thread block boundary (made whatever the check
 * said), then as a retune profile for a target. */
int rtl_stream_test_analog_request_with_stream(int rate_hz, int analog_stream, int post_downsample, int kind,
                                               int width_hz, rtl_stream_test_live_request_result* out);

/* A live NFM request for @p width_hz checked against the rate a running stream published at @p rate_hz (a -fA session
 * at the unset default width, or a DMR session when @p analog_stream is 0), then consumed after a retune settled the
 * stream on @p landed_rate_hz: the request is queued, the rate chain is finalized on the landed rate as a retune does,
 * and one demod-thread block boundary consumes it. The *_before fields are read after the retune, just before that
 * boundary; the retune fields are unused. */
int rtl_stream_test_analog_request_across_rate_change(int rate_hz, int landed_rate_hz, int width_hz, int analog_stream,
                                                      rtl_stream_test_live_request_result* out);

/* What a decode-mode change to Analog does on a DMR session running at @p rate_hz, with a retune landing in between:
 * the NFM width @p width_hz is checked against the published rate before the decoder commits (check_rc), then a
 * retune settles the stream on @p landed_rate_hz and publishes it, and only then is the analog profile requested
 * (request_rc) and consumed at one demod-thread block boundary. The *_before fields are read after the retune, just
 * before the request; the retune fields are unused. */
int rtl_stream_test_analog_switch_request_after_rate_change(int rate_hz, int landed_rate_hz, int width_hz,
                                                            rtl_stream_test_live_request_result* out);

typedef struct rtl_stream_test_audio_reset_result {
    float deemph_avg;
    float dc_avg;
    float audio_lpf_state;
    float squelch_env;
    float am_carrier; /* the AM detector's carrier estimate once the retune finalized */
    int demod_is_am;  /* dsd_demod_am_active() once the retune finalized */
    int deemph_after; /* demod_state::deemph, likewise */
    int analog_kind;  /* demod_state::analog_demod, likewise */
    int channel_hist_cleared;
    int hb_hist_cleared;
    int resamp_hist_cleared;
    int post_decim_cleared; /* a replay's post-demod decimator back at its start (none allocated counts) */
    float deemph_a_before;
    float deemph_a_after;
    float audio_lpf_alpha_before;
    float audio_lpf_alpha_after;
    int channel_lpf_width_before;
    int channel_lpf_width_after;
    int channel_lpf_enable_after;
    int published_width_after;
    int published_lpf_on_after;
    int retune_refused;        /* 1 when the retune was refused for the monitor's analog width */
    int rate_out_after;        /* demod_state::rate_out once the retune finalized */
    uint32_t center_before;    /* the centre frequency the stream ran on before the retune */
    uint32_t retune_target_hz; /* the centre frequency the retune asked for */
    uint32_t center_after;     /* the centre frequency the retune finalized on */
} rtl_stream_test_audio_reset_result;

/* Seed an analog monitor at @p rate_before_hz (NFM width @p nfm_width_hz, 0 = default) with stale filter state, then
 * land a retune to the next channel that the device settled on @p rate_after_hz, the way the controller lands one: the
 * landing check that may refuse it (and put the capture back), then the rate-chain finalize (75 us de-emphasis, 3 kHz
 * audio LPF). No device is open, so a refusal restores the capture settings the stream keeps and programs nothing. */
int rtl_stream_test_audio_monitor_retune(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                         rtl_stream_test_audio_reset_result* out);

/* rtl_stream_test_audio_monitor_retune() for an analog monitor of @p kind (dsd_analog_demod) at width @p width_hz
 * (0 = the kind's default), its AM carrier estimate seeded stale along with the rest. */
int rtl_stream_test_audio_monitor_retune_kind(int kind, int rate_before_hz, int rate_after_hz, int width_hz,
                                              rtl_stream_test_audio_reset_result* out);

typedef struct rtl_stream_test_kind_switch_result {
    rtl_stream_test_demod_fields fresh;    /* a fresh open of the options switched to */
    rtl_stream_test_demod_fields switched; /* the running monitor once the live switch was consumed */
    int request_rc;
    int deferred_until_consume; /* 1 when the queued request left the running kind alone until consumed */
    int published_rc;           /* rtl_stream_get_analog_profile() after the switch */
    int published_kind;
    int published_width_hz;
    uint32_t generation_before;
    uint32_t generation_after;
    size_t used_before;        /* output ring samples the old kind left queued */
    size_t used_while_pending; /* ... with the request queued, not yet consumed */
    size_t used_after;         /* ... once the switch was consumed */
} rtl_stream_test_kind_switch_result;

/* Open the analog monitor on @p from_opts at @p rate_hz, give it a running session's stale monitor audio, filter,
 * resampler, carrier-estimate and I/Q DC state and a queued output ring, queue a live request for @p to_opts's kind and
 * width (an FM <-> AM switch, or the same kind again), and consume it at one demod-thread block boundary; fresh is an
 * open of @p to_opts at the same rate. */
int rtl_stream_test_analog_kind_switch(const dsd_opts* from_opts, const dsd_opts* to_opts, int rate_hz,
                                       rtl_stream_test_kind_switch_result* out);

typedef struct rtl_stream_test_output_scale_result {
    int demod_is_am;      /* dsd_demod_am_active() on the open */
    int output_kind;      /* demod_state::output_kind on the open */
    int resampled;        /* 1 when the block went through the output resampler */
    float live_scale;     /* the output scale a live stream runs (1/pi) */
    int unscaled_samples; /* samples the ring took with no output scale (0, as I/Q replay runs) */
    int scaled_samples;   /* ... and with the live scale */
    float gain;           /* least-squares gain from the unscaled output to the scaled one */
} rtl_stream_test_output_scale_result;

/* Open @p opts at @p rate_hz and write one block of demodulated samples through the output block the demod thread
 * writes (demod_write_output_block()), once with no output scale, as I/Q replay runs, and once with the scale a live
 * stream runs: gain says what the live scale did to the audio the ring took (1 when the output is exempt). */
int rtl_stream_test_monitor_output_scale(const dsd_opts* opts, int rate_hz, rtl_stream_test_output_scale_result* out);

typedef struct rtl_stream_test_retune_profile_landing_result {
    int retune_refused;          /* 1 when the landing check refused the retune */
    int profile_refused;         /* 1 when the retune landed and refused its own profile, which the retune reports */
    int rate_out_after;          /* demod_state::rate_out once the retune finalized */
    int analog_family_after;     /* demod_state::analog_family, likewise */
    int monitor_after;           /* dsd_demod_analog_monitor_active(), likewise */
    int channel_lpf_width_after; /* demod_state::channel_lpf_width_hz, likewise */
} rtl_stream_test_retune_profile_landing_result;

/* As rtl_stream_test_audio_monitor_retune(), with the retune carrying a retune profile for its target that switches to
 * @p profile_family (dsd_rx_family), with NFM width @p profile_width_hz for the analog family. */
int rtl_stream_test_audio_monitor_retune_with_profile(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                                      int profile_family, int profile_width_hz,
                                                      rtl_stream_test_retune_profile_landing_result* out);

typedef struct rtl_stream_test_retune_completion_result {
    int retune_refused;    /* 1 when the landing check refused the retune */
    uint32_t center_after; /* the centre frequency the retune finalized on */
    int completion_result; /* rtl_stream_tune_result the manual retune completes with */
} rtl_stream_test_retune_completion_result;

/* As rtl_stream_test_audio_monitor_retune(), for a manual retune to @p target_hz from a stream running on
 * @p running_center_hz (0: no centre applied yet), which the device reconfigured for: reports the result the controller
 * completes that retune with (controller_manual_retune_completion_result()). */
int rtl_stream_test_audio_monitor_retune_completion(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                                    uint32_t running_center_hz, uint32_t target_hz,
                                                    rtl_stream_test_retune_completion_result* out);

typedef struct rtl_stream_test_rate_not_restored_result {
    int retune_refused;     /* 1 when the landing check refused the retune */
    int exit_requested;     /* dsd_exitflag_load() once the retune finalized */
    int input_failure_kind; /* dsd_input_failure kind reported by then */
} rtl_stream_test_rate_not_restored_result;

/* As rtl_stream_test_audio_monitor_retune() for an explicit width, with a device that does not return to the rate the
 * refused retune put back: the rate the device reports once the capture is restored is still @p rate_after_hz when
 * the rate chain finalizes. The exit request and input failure that reports are cleared before this returns. */
int rtl_stream_test_audio_monitor_rate_not_restored(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                                    rtl_stream_test_rate_not_restored_result* out);

typedef struct rtl_stream_test_restore_failure_result {
    int retune_refused;              /* 1 when the landing check refused the retune */
    int program_calls;               /* times the refusal asked the device for the capture it ran */
    uint32_t program_freq_hz;        /* the capture frequency it asked for */
    uint32_t program_rate_hz;        /* the capture rate it asked for */
    uint32_t capture_freq_before_hz; /* the capture the stream ran before the retune */
    uint32_t capture_rate_before_hz;
    int rate_out_after;     /* demod rate once the retune finalized */
    int exit_requested;     /* dsd_exitflag_load() once the retune finalized */
    int input_failure_kind; /* dsd_input_failure kind reported by then */
    int input_failure_code; /* ... and its native code */
} rtl_stream_test_restore_failure_result;

/* As rtl_stream_test_audio_monitor_retune() for an explicit width the landed rate cannot realize, with the device the
 * refusal programs back standing in for a real one: it answers the capture frequency with @p frequency_rc and the
 * capture rate with @p rate_rc (0 = taken; the rate the device then reports is the one it was put back to). The exit
 * request and input failure a failure reports are cleared before this returns. */
int rtl_stream_test_audio_monitor_restore_failure(int rate_before_hz, int rate_after_hz, int nfm_width_hz,
                                                  int frequency_rc, int rate_rc,
                                                  rtl_stream_test_restore_failure_result* out);

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

/* rtl_stream_test_retune_analog_profile() on a DMR stream opened at @p rate_hz. The profile is queued with no stream
 * running (checked only against the kind and range rules then) and the retune keeps that demod rate. */
int rtl_stream_test_retune_analog_profile_at_rate(uint32_t target_hz, int rate_hz, int family, int kind, int width_hz,
                                                  int with_cqpsk_symbol_profile,
                                                  rtl_stream_test_retune_analog_result* out);

/* A live request queued for the demod thread around a scan row's retune, and still unconsumed when the retune lands
 * (rtl_stream_test_retune_step::queued_live_request): queued before the row's retune profile is, or made on the
 * decoder's thread while the retune lands, once the controller has found the profile's family not superseded
 * (rtl_stream_retune_family_superseded()) and before it retires the requests older than that family. */
enum {
    RTL_STREAM_TEST_QUEUED_NONE = 0,
    RTL_STREAM_TEST_QUEUED_NFM_WIDTH = 1,    /* an NFM width command: the analog family at queued_live_width_hz */
    RTL_STREAM_TEST_QUEUED_SYMBOL_AFTER = 2, /* the same, then a C4FM symbol profile queued once the profile is */
    RTL_STREAM_TEST_QUEUED_NFM_WIDTH_AT_LANDING = 3, /* an NFM width command at queued_live_width_hz, while it lands */
    RTL_STREAM_TEST_QUEUED_DIGITAL_AT_LANDING = 4,   /* a scan leave to a digital session while it lands: the digital
                                                        family, then its C4FM symbol profile */
};

/* The symbol profile a retune step queues for its target before it attaches the family
 * (rtl_stream_test_retune_step::with_symbol_profile), as a digital row or target does. The explicit ones attach the
 * digital family with cqpsk_explicit set, as the engine does for a trunk-scan target whose CQPSK choice is its own
 * (issue #583): a P25 target with a modulation value, and every DMR or NXDN target. */
enum {
    RTL_STREAM_TEST_SYMBOL_NONE = 0,
    RTL_STREAM_TEST_SYMBOL_P25_CQPSK = 1, /* P25 on CQPSK: 4800 sym/s, the P25 CQPSK channel filter */
    RTL_STREAM_TEST_SYMBOL_DMR_FSK = 2,   /* DMR on the FSK discriminator: CQPSK off, 4800 sym/s, the 12.5 kHz filter */
    RTL_STREAM_TEST_SYMBOL_P25_CQPSK_EXPLICIT = 3, /* P25 on CQPSK, the target's own choice (modulation=cqpsk) */
    RTL_STREAM_TEST_SYMBOL_P25_C4FM_EXPLICIT = 4,  /* P25 on C4FM: CQPSK off, 4800 sym/s, the P25 C4FM channel filter,
                                                      the target's own choice (modulation=c4fm) */
    RTL_STREAM_TEST_SYMBOL_DMR_FSK_EXPLICIT = 5, /* DMR on the FSK discriminator, as a DMR trunk-scan target runs it */
    /* P25 whose profile leaves the CQPSK state to the stream (cqpsk_enable -1), with the P25 CQPSK channel filter: what
       a P25 target with no modulation queues under -mq while DSD_NEO_CQPSK is set. The decoder times it for CQPSK. */
    RTL_STREAM_TEST_SYMBOL_P25_CQPSK_UNSET = 6,
};

/* One scan row's retune (issue #526): the receive family its profile attaches, and the live family request a scanner
 * or a command makes around it. */
typedef struct rtl_stream_test_retune_step {
    int family; /* dsd_rx_family the retune profile attaches; -1 attaches none, as a digital-only session's retune */
    int kind;   /* analog demodulator (with the analog family) */
    int width_hz;
    int with_symbol_profile;    /* RTL_STREAM_TEST_SYMBOL_*: queued for the target before the family is attached */
    int live_family_before;     /* -1, or a live family request made before the profile is queued */
    int live_family_after_take; /* -1, or a live family request made once the controller has taken the profile */
    int queued_live_request;    /* RTL_STREAM_TEST_QUEUED_*: made with a pipeline running, so it waits in the queue */
    int queued_live_width_hz;
} rtl_stream_test_retune_step;

typedef struct rtl_stream_test_retune_landing {
    int queued_rc;           /* rtl_stream_prepare_retune_analog_profile_for_target() */
    int taken;               /* the controller took a profile for the step's target */
    int applied_family;      /* demod_state::analog_family once the retune finalized */
    int applied_width_hz;    /* demod_state::channel_lpf_width_hz then */
    int applied_output_kind; /* demod_state::output_kind then */
    int applied_cqpsk_enable;
    int applied_channel_profile; /* demod_state::channel_lpf_profile then */
    int applied_output_rate;     /* the output rate then */
    int applied_ted_sps;         /* demod_state::ted_sps then */
    /* With a symbol profile: rtl_stream_output_rate_for_family() for the digital family, asked before the retune landed
       with the CQPSK state the decoder times the target for and whether it is the target's own, as the decoder asks. */
    int predicted_output_rate;
    /* With a queued_live_request: the same once the demod thread reached its next block boundary after the retune,
       and what became of the queued request (rtl_stream_receive_request_outcome()). */
    int boundary_family;
    int boundary_width_hz;
    int boundary_output_kind;
    int boundary_symbol_rate_hz;
    int queued_request_outcome;
} rtl_stream_test_retune_landing;

/* Land @p count scan-row retunes one after another on a DMR stream opened at 48 kHz, each on its own channel, the way
 * the controller does (queue the profile, take it, finalize the retune with it), and report what the demodulator ended
 * on after each. With no stream running a live family request switches nothing itself; it only stands, as it does on
 * the demod thread, for a newer request than the profile queued before it. A queued_live_request is made with a
 * pipeline running instead, so it waits for the demod thread, which reaches a block boundary once the retune landed. */
int rtl_stream_test_retune_profile_sequence(const rtl_stream_test_retune_step* steps, size_t count,
                                            rtl_stream_test_retune_landing* out);

/* rtl_stream_test_retune_profile_sequence() with the device settled on @p forced_rate_out_hz (0: none) from the open,
 * where the FSK discriminator is resampled to 48 kHz and CQPSK is not (issue #583). */
int rtl_stream_test_retune_profile_sequence_at_rate(const rtl_stream_test_retune_step* steps, size_t count,
                                                    int forced_rate_out_hz, rtl_stream_test_retune_landing* out);

/* rtl_stream_test_retune_profile_sequence_at_rate(), with each step whose bit is set in @p external_steps (bit i for
 * step i, at most 32 steps) landed as an external backend's retune is (issue #583: a rigctl peer that tuned an RTL
 * input): its queued profile applied from the decoder's thread with the pipeline running, through
 * rtl_stream_apply_pending_retune_profile_for_target(), with no controller retune and no finalize. Such a step makes no
 * live_family_after_take request, and its taken says the landing took the profile queued for its target. */
int rtl_stream_test_retune_profile_sequence_external(const rtl_stream_test_retune_step* steps, size_t count,
                                                     int forced_rate_out_hz, uint32_t external_steps,
                                                     rtl_stream_test_retune_landing* out);

/* External backends' retunes landing NFM profiles on a running monitor (issue #572), each over stale filter state. */
typedef struct rtl_stream_test_external_landing_filter_result {
    int first_taken;                 /* 1 when the first landing (16 kHz on frequency A) took its profile */
    int new_freq_width_taken;        /* frequency B, 12.5 kHz */
    int new_freq_width_cleared;      /* 1 when that landing started the half-band and channel filters over */
    int new_freq_same_width_taken;   /* frequency C, 12.5 kHz again */
    int new_freq_same_width_cleared; /* likewise */
    int same_freq_width_taken;       /* frequency C again, 16 kHz */
    int same_freq_width_kept;   /* 1 when that landing left every filter history and pending count as it found them */
    int same_freq_width_after;  /* demod_state::channel_lpf_width_hz after it */
    int same_freq_plan_dropped; /* 1 when it dropped the channel plan, so the next block designs 16 kHz */
} rtl_stream_test_external_landing_filter_result;

/* Open the NFM monitor at 48 kHz, 16 kHz wide, and land NFM retune profiles as an external backend's retunes do
 * (rtl_stream_apply_pending_retune_profile_for_target(), with no controller retune and no finalize): on frequency A,
 * then each over seeded stale filter state, on B with a new width, on C with the width B left, and on C again with
 * only a new width. */
int rtl_stream_test_external_landing_filter_state(rtl_stream_test_external_landing_filter_result* out);

/* An external backend's retune landing against a controller reconfiguration on another thread (issue #583). */
typedef struct rtl_stream_test_external_landing_race_result {
    /* The controller holds its reconfigure gate (controller_enter_reconfigure_gate()) when the landing starts. */
    int landing_taken;             /* 1 when the landing took the profile queued for its target */
    int landing_waited;            /* 1 when the landing was seen waiting for the gate while the controller held it */
    int landed_during_reconfigure; /* 1 when the landing's receive part ran before the reconfiguration ended */
    int held_output_kind;          /* demod_state::output_kind once both finished */
    int held_output_rate;          /* the output rate then */
    /* The landing holds the gate, stopped inside it, when a controller reconfiguration starts on another thread. */
    int arriving_taken;
    int reconfigure_waited;         /* 1 when the reconfiguration was seen waiting for the gate the landing held */
    int reconfigure_during_landing; /* 1 when the reconfiguration entered the gate before the landing left it */
    int gate_held_through_landing;  /* 1 when the gate was still closed once the reconfiguration had tried to enter */
    int arriving_output_kind;
    int arriving_output_rate;
    int gate_open_after; /* 1 when the gate was open again once every thread finished */
} rtl_stream_test_external_landing_race_result;

/* Open DMR at 48 kHz on a device forced to 78,125 Hz and land two digital-family retunes as an external backend does
 * (rtl_stream_apply_pending_retune_profile_for_target(), on a thread of its own standing for the decoder's): an
 * explicit P25 CQPSK target's while the test thread holds the controller's reconfigure gate, then an explicit DMR
 * target's, stopped inside the gate while a controller reconfiguration starts on a third thread. Each wait is bounded,
 * so a landing or reconfiguration that never waits reports so instead of hanging the suite. */
int rtl_stream_test_external_landing_against_reconfigure(rtl_stream_test_external_landing_race_result* out);

/* What rtl_stream_family_landing_after_pending() answers at each point (issue #583). */
typedef struct rtl_stream_test_family_landing_after_pending_result {
    int digital_only;        /* a digital stream with nothing queued or in flight */
    int width_queued;        /* an NFM width request queued on that stream, not yet taken by the demod thread */
    int width_queued_public; /* the public query then, which reads no controller with none running */
    int width_retiring;      /* a digital-family retune queued after it, taken by the controller */
    int width_retired;       /* ... once that retune has landed and retired the request */
    int width_outcome;       /* the request's outcome then (rtl_stream_receive_request_outcome()) */
    /* A digital-family retune on the digital stream, with no analog work anywhere. */
    int digital_queued;            /* queued on the controller */
    int digital_taken;             /* ... taken by it, in flight */
    int digital_landed_on_digital; /* ... landed */
    int digital_superseded_queued; /* one queued, then a live digital family request made after the attach */
    int digital_superseded_taken;  /* one taken, then a live digital family request made after the take */
    int digital_stale_queued;      /* a digital-family profile left queued for a target a coalesced retune moved from */
    int analog_queued;             /* an analog retune queued on the controller */
    int analog_taken;              /* ... taken by it, in flight */
    int analog_landed;             /* ... landed: the stream publishes the analog family */
    int digital_landed;            /* a digital-family retune landed after it */
    int analog_live;               /* the stream on the analog family again, nothing outstanding */
    int analog_live_public;        /* the public query then, with no stream running: the published family it keeps */
    int digital_in_flight;         /* a digital-family retune taken while the stream runs the analog family */
    int digital_left_analog;       /* ... once it has landed */
    int superseded_taken;          /* an analog retune taken, then a live digital family request made after the take */
    int superseded_landed;         /* ... once it has landed, neither its family nor its profile applied */
    /* Two retunes outstanding at once on a digital stream: one taken (in flight), one queued behind it. A plain retune
     * carries no family. */
    int analog_in_flight_digital_queued;  /* an analog retune in flight, a digital-family one queued */
    int digital_in_flight_analog_queued;  /* a digital-family retune in flight, an analog one queued */
    int digital_in_flight_digital_queued; /* a digital-family retune in flight, another queued */
    int digital_in_flight_plain_queued;   /* a digital-family retune in flight, a plain one queued */
    int plain_in_flight_digital_queued;   /* a plain retune in flight, a digital-family one queued */
    int analog_in_flight_plain_queued;    /* an analog retune in flight, a plain one queued */
    int stale_queued;  /* an analog profile left queued for a target a coalesced retune moved away from */
    int landed_family; /* demod_state::analog_family once the last retune landed */
} rtl_stream_test_family_landing_after_pending_result;

/* Drive rtl_stream_family_landing_after_pending() on a DMR stream opened at 48 kHz, with a stand-in controller whose
 * retunes are queued, taken and landed the way the controller thread does (without a device), and the real request
 * queue, whose requests wait for the demod thread as with a pipeline running. */
int rtl_stream_test_family_landing_after_pending(rtl_stream_test_family_landing_after_pending_result* out);

/* When a live republish's digital family request lands against a retune that carries the digital family, already
 * attached and taken by the controller when the republish is made (rtl_stream_test_live_digital_landing(), issue
 * #583). */
enum {
    RTL_STREAM_TEST_LANDING_ALONE = 0,            /* no retune outstanding */
    RTL_STREAM_TEST_LANDING_BEFORE_RETUNE = 1,    /* taken by the demod thread before the retune lands */
    RTL_STREAM_TEST_LANDING_AFTER_SUPERSEDED = 2, /* taken once the retune, superseded by it, has landed */
    RTL_STREAM_TEST_LANDING_AFTER_RETUNE = 3,     /* requested once the retune has landed, not superseded */
    RTL_STREAM_TEST_LANDING_REPLACED = 4, /* a plain digital family request and its profile queued after it, before the
                                             demod thread took either */
};

/* One live republish of a P25 target on a stream already on the digital family. */
typedef struct rtl_stream_test_live_landing_case {
    int forced_rate_out_hz; /* the demod rate the device settled on (0: none forced) */
    int from_cqpsk;     /* the stream runs CQPSK (a P25 target's own), else the FSK discriminator (a DMR target's) */
    int request_cqpsk;  /* the CQPSK state the republish's symbol profile asks for (the decoder's rf_mod == 1) */
    int marked;         /* rtl_stream_request_digital_family_landing(), else rtl_stream_request_analog_profile() */
    int cqpsk_explicit; /* what the marked request says of the profile's CQPSK state */
    int order;          /* RTL_STREAM_TEST_LANDING_* */
} rtl_stream_test_live_landing_case;

typedef struct rtl_stream_test_live_landing_result {
    /* rtl_stream_output_rate_for_family() for the digital family, asked before the republish with the CQPSK state its
       profile asks for and cqpsk_explicit, as the decoder asks it; the profile's TED is timed for it. */
    int predicted_output_rate;
    int outstanding_queued;  /* rtl_stream_family_landing_after_pending() with the republish queued */
    int outstanding_after;   /* ... once the republish and the retune have both landed */
    int retune_output_kind;  /* demod_state::output_kind right after the retune landed (the AFTER orders) */
    int retune_output_rate;  /* the output rate then */
    int output_kind;         /* demod_state::output_kind once everything landed */
    int cqpsk_enable;        /* demod_state::cqpsk_enable then */
    int channel_profile;     /* demod_state::channel_lpf_profile then */
    int output_rate;         /* the output rate then */
    int ted_sps;             /* demod_state::ted_sps then */
    int analog_family;       /* demod_state::analog_family then */
    int family_switch_noted; /* the stream recorded a switch to the digital family (RtlSdrInternals::rx_family_switch) */
    int request_outcome;     /* rtl_stream_receive_request_outcome() of the republish's family request */
} rtl_stream_test_live_landing_result;

/* On a DMR stream opened at 48 kHz with the device settled on the case's forced rate, put the front end on CQPSK or the
 * FSK discriminator with a retune that carries no family, then queue a live republish as the decoder makes it (the
 * digital family request, then a P25 symbol profile at 4800 sym/s with the filter of the CQPSK state it asks for and a
 * TED timed for the predicted rate) and let the demod thread take it at a block boundary, in the case's order against a
 * P25 retune that carries the digital family and leaves the CQPSK state to the stream. Retunes are queued, taken and
 * landed on a stand-in controller the way the controller thread does it, without a device. */
int rtl_stream_test_live_digital_landing(const rtl_stream_test_live_landing_case* c,
                                         rtl_stream_test_live_landing_result* out);

typedef struct rtl_stream_test_replay_state {
    int replay_input_eof;
    int replay_input_drained;
    int replay_demod_drained;
    int replay_output_drained;
    int replay_forced_stop;
    int replay_reader_exited;
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
    uint64_t replay_out_written;      /* output batches the demod published, the virtual block 0 included */
    uint64_t replay_out_acked;        /* ... that the decoder acknowledged */
    uint64_t replay_output_truncated; /* output samples a replay block could not publish (none should be) */
} rtl_stream_test_replay_state;

int dsd_rtl_stream_test_get_replay_state(rtl_stream_test_replay_state* out_state);
int rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev);

/* Points of the I/Q replay pipeline a test can stop at (issue #572). Each is reported to the hook that
 * rtl_stream_test_set_replay_stage_hook() installs, on the thread named, with the count the stage names (0 where it
 * names none). The reporting thread holds no stream lock, so a hook may wait there for another thread to reach its own
 * stage. */
enum {
    RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE = 1,       /* demod: took an input block (count: floats) */
    RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE = 2, /* demod: about to publish the block's output */
    RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_OUTPUT_WRITE = 3,  /* demod: published it (count: samples written) */
    RTL_STREAM_TEST_REPLAY_DEMOD_DRAIN_DECISION = 4,      /* demod: about to decide whether the input is drained */
    RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECISION = 5,     /* replay reader: input ring empty at EOF, about to decide */
    RTL_STREAM_TEST_REPLAY_DECODER_OUTPUT_FOUND = 6,      /* decoder: saw output queued, before it copies any */
    RTL_STREAM_TEST_REPLAY_DECODER_OUTPUT_EMPTY = 7,      /* decoder: found nothing to copy, before its end check */
    /* replay reader: read the next chunk, about to wait for the demod to take the one before it (count: the next
       chunk's sequence) */
    RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT = 8,
    /* replay reader: bumped the submit generation for a chunk it has not committed yet (count: that generation) */
    RTL_STREAM_TEST_REPLAY_READER_BEFORE_COMMIT = 9,
    /* replay reader: made its drain decision (count: 1 if it reported the demod drained) */
    RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECIDED = 10,
    /* demod: copied a block that wraps the ring end out of the ring and released its input, before processing it
       (count: floats) */
    RTL_STREAM_TEST_REPLAY_DEMOD_WRAPPED_RELEASED = 11,
    /* demod: released the input of a replay block it discards, before acknowledging the block (count: floats) */
    RTL_STREAM_TEST_REPLAY_DEMOD_DISCARD_RELEASED = 12,
    /* demod: a block's output is ready, and neither in the output ring nor counted as published yet (count:
       samples) */
    RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_PUBLISH = 13,
    /* demod: about to wait for the decoder's demand before it starts a block (count: blocks published so far, the
       virtual block 0 included) */
    RTL_STREAM_TEST_REPLAY_DEMOD_WAIT_FOR_DEMAND = 14,
    /* the thread opening a replay: the demod thread runs, and the replay reader is about to start */
    RTL_STREAM_TEST_REPLAY_READER_START = 15,
    /* demod: took the input purge flag, before it discards the input ring */
    RTL_STREAM_TEST_REPLAY_DEMOD_PURGE_TAKEN = 16,
    /* replay reader: about to wait for the pipeline to go idle before an event (count: the event's DSD_IQ_EVENT_*
       kind; 0 before a loop rewind) */
    RTL_STREAM_TEST_REPLAY_READER_EVENT_BOUNDARY = 17,
    /* replay reader: requested the input purge of a RESET or loop boundary, about to wait for it to be applied */
    RTL_STREAM_TEST_REPLAY_READER_PURGE_WAIT = 18,
};

typedef void (*rtl_stream_test_replay_stage_fn)(int stage, size_t count, void* ctx);

/* Install only while the stream is stopped. NULL removes the hook. */
void rtl_stream_test_set_replay_stage_hook(rtl_stream_test_replay_stage_fn hook, void* ctx);
/* Report @p stage to the installed hook. The replay reader in rtl_device.cpp reports its stage through this too. */
void rtl_stream_test_replay_stage(int stage, size_t count);

/* One block the demod took from an I/Q replay's input ring (issue #572), reported on the demod thread as it takes it.
 * The chunk fields are what the replay reader attached to the capture chunk the block holds. */
typedef struct rtl_stream_test_replay_block {
    uint64_t sequence;       /* the chunk's place in the replay, from 1 */
    uint64_t submit_gen;     /* the submit generation the chunk was committed under */
    uint64_t media_start_ns; /* capture time of the chunk's first sample, time a MUTE omitted included */
    uint64_t media_end_ns;   /* capture time just past its last sample */
    int have_input_level;    /* the chunk carries an input-level snapshot */
    size_t float_count;      /* interleaved I/Q floats in the block */
    const float* p1;         /* the block's floats, in one or two ring segments, valid during the call */
    size_t n1;
    const float* p2;
    size_t n2;
} rtl_stream_test_replay_block;

typedef void (*rtl_stream_test_replay_block_fn)(const rtl_stream_test_replay_block* block, void* ctx);

/* Install only while the stream is stopped. NULL removes the hook. */
void rtl_stream_test_set_replay_block_hook(rtl_stream_test_replay_block_fn hook, void* ctx);
/* The demod discards the I/Q replay block that holds chunk @p sequence instead of processing it, as a controller gate
 * closing on it would; 0 disarms. Set while no replay runs. */
void rtl_stream_test_replay_discard_chunk(uint64_t sequence);
/* Raise the running I/Q replay's forced-stop flag and wake every replay wait, as the replay device's stop does, without
 * tearing the stream down. */
void rtl_stream_test_replay_force_stop(void);

/* The next I/Q replay reader fails the read that follows @p after_chunks reads that returned data, with @p code (a
 * DSD_IQ_ERR_* value) as dsd_iq_replay_read() would. One failure per arming; a negative @p after_chunks disarms it. */
void rtl_device_test_replay_inject_read_error(int after_chunks, int code);
/* Each capture read the next I/Q replay reader makes returns at most @p max_bytes, as a source that returns short reads
 * would; 0 lifts the limit. Set while no replay runs. */
void rtl_device_test_replay_limit_read(size_t max_bytes);
/* Nonzero: rtl_device_start_async() refuses to start an I/Q replay reader, as a failed thread create would. */
void rtl_device_test_replay_fail_start(int fail);
/* How many times the last replay reader's EOF sequence looked at the input ring while it waited for the ring to
 * empty. */
uint64_t rtl_device_test_replay_eof_wait_iterations(void);

/* The last retune reset plan applied to the stream's demodulator (a retune's finalize, a replay RESET, a stream open,
 * a CQPSK reacquire), with the FLL decision the seed cache left it on. */
typedef struct rtl_stream_test_reset_plan {
    char reason[24]; /* the plan's reason: "frequency", "distant-frequency", "fresh-stream", ... */
    uint32_t previous_center_hz;
    uint32_t next_center_hz;
    int reset_retained_fll;  /* the band-edge FLL starts fresh */
    int restored_cached_fll; /* ... or from the seed cached for the new centre */
} rtl_stream_test_reset_plan;

/* 0 with the last plan in @p out, -1 when none was applied since the process started. */
int rtl_stream_test_get_last_reset_plan(rtl_stream_test_reset_plan* out);

#ifdef __cplusplus
}
#endif

#endif
