// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#error "DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS must be enabled for this test."
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include "dsd-neo/core/safe_api.h"

extern "C" uint64_t rtl_device_test_coalesce_capture_mute_duration(uint64_t* pending_bytes, uint64_t duration_bytes,
                                                                   size_t alignment);
extern "C" int rtl_device_test_complete_fragmented_capture_discard(int byte_count, unsigned int partial_byte_count);
extern "C" void rtl_device_test_end_capture_reconfigure_with_odd_carry(int* out_hold, int* out_mute,
                                                                       int* out_mute_byte_phase);
extern "C" int rtl_device_test_begin_capture_reconfigure_without_writer(int* out_hold);
extern "C" int rtl_device_test_usb_reconfigure_discards_samples(size_t input_bytes, size_t* out_ring_used);
extern "C" int rtl_device_test_soapy_config_settings_visibility(size_t config_size, const char* settings,
                                                                int* out_seen);
extern "C" void rtl_device_test_replay_dispatch_reset_event_state(int* phase, int* have_carry, uint8_t* carry_byte);
extern "C" int rtl_device_test_replay_event_boundary_drained(size_t ring_used, uint64_t submitted_gen,
                                                             uint64_t consumed_gen);
extern "C" int rtl_device_test_usb_apply_retry(int verify_enabled, int attempts, int apply_success_after,
                                               int verify_success_after, int* out_apply_calls, int* out_verify_calls,
                                               int* out_used_attempts);
extern "C" int rtl_device_test_usb_sample_rate_readback(uint32_t requested_rate, uint32_t actual_rate,
                                                        uint32_t* out_actual_rate);
extern "C" int rtl_device_test_usb_manual_gain_controls(int agc_rc, int gain_mode_rc, int gain_rc, int* out_agc_calls,
                                                        int* out_gain_mode_calls, int* out_gain_calls,
                                                        int* out_recorded_agc_rc);
extern "C" int rtl_device_test_usb_auto_gain_controls(int agc_rc, int gain_mode_rc, int* out_agc_calls,
                                                      int* out_gain_mode_calls, int* out_recorded_agc_rc);
extern "C" int rtl_device_test_u8_odd_carry_bridge(size_t* out_used, int* out_phase, int* out_carry_valid,
                                                   uint8_t* out_carry_byte, int* out_first_status,
                                                   int* out_second_status);
extern "C" int rtl_device_test_u8_full_ring_drop(size_t* out_used, uint64_t* out_drops, uint64_t* out_full_events,
                                                 int* out_phase, int* out_status);
extern "C" int rtl_device_test_u8_generation_stale_drop(uint64_t* out_drops, int* out_phase, int* out_dev_carry_valid,
                                                        int* out_local_carry_valid, int* out_status);
extern "C" int rtl_device_test_replay_input_level_snapshot(int format, int backend, const char* capture_stage,
                                                           size_t raw_bytes, size_t scratch_cap_f32, int* out_rc,
                                                           int* out_source, uint64_t* out_count);

// Mirrors the internal RTL replay-conversion test hook request layout.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct rtl_device_test_replay_convert_block_request {
    int format;
    const char* capture_stage;
    int fs4_shift_enabled;
    int combine_rotate_enabled;
    const uint8_t* raw_block;
    size_t raw_bytes;
    size_t out_cap_f32;
    int start_phase;
    int start_have_carry;
    uint8_t start_carry_byte;
};

extern "C" int rtl_device_test_replay_convert_block(const rtl_device_test_replay_convert_block_request* request,
                                                    float* out_f32, size_t out_f32_count, int* out_phase,
                                                    int* out_have_carry, uint8_t* out_carry_byte);
extern "C" int rtl_device_test_public_capture_policy(int* out_formats, size_t out_formats_len, uint32_t* out_counts,
                                                     size_t out_counts_len);
extern "C" int rtl_device_test_misc_string_helpers(char* tuner_labels, size_t tuner_labels_size, char* reason_small,
                                                   size_t reason_small_size, char* reason_null, size_t reason_null_size,
                                                   char* trimmed, size_t trimmed_size, size_t* rounded_pages,
                                                   size_t rounded_pages_len);
extern "C" int rtl_device_test_tcp_policy_helpers(size_t* bufsz_out, size_t bufsz_count, int* waitall_out,
                                                  size_t waitall_count, uint64_t* delta_out, size_t delta_count,
                                                  int* agc_out);
extern "C" int dsd_rtl_stream_test_tune_completion_result(int wait_result, int completion_result);
extern "C" int dsd_rtl_stream_test_manual_retune_completion_result(int retune_rc, int reconfigured, uint32_t target_hz,
                                                                   uint32_t applied_freq_hz);
extern "C" int dsd_rtl_stream_test_tune_failure_reconciles_applied(uint32_t requested_freq_hz, uint32_t applied_freq_hz,
                                                                   long int* out_opts_freq,
                                                                   uint32_t* out_capture_freq_hz);
extern "C" int
dsd_rtl_stream_test_capture_settings_failure_restore(uint32_t* out_full_freq_hz, uint32_t* out_full_rate_hz,
                                                     int* out_full_rate_out_hz, uint32_t* out_partial_freq_hz,
                                                     uint32_t* out_partial_rate_hz, int* out_partial_rate_out_hz);
extern "C" int dsd_rtl_stream_test_ppm_store_if_applied(int ppm_rc, int requested_ppm, int* out_ppm_error);
extern "C" int dsd_rtl_stream_test_retune_completion_result_binding(int* out_first_result, int* out_second_result);

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size_eq(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
call_replay_convert_block(int format, const char* capture_stage, int fs4_shift_enabled, int combine_rotate_enabled,
                          const uint8_t* raw_block, size_t raw_bytes, size_t out_cap_f32, int start_phase,
                          int start_have_carry, uint8_t start_carry_byte, float* out_f32, size_t out_f32_count,
                          int* out_phase, int* out_have_carry, uint8_t* out_carry_byte) {
    rtl_device_test_replay_convert_block_request request{
        format,    capture_stage, fs4_shift_enabled, combine_rotate_enabled, raw_block,
        raw_bytes, out_cap_f32,   start_phase,       start_have_carry,       start_carry_byte};
    return rtl_device_test_replay_convert_block(&request, out_f32, out_f32_count, out_phase, out_have_carry,
                                                out_carry_byte);
}

static int
expect_generation_eq(const char* label, uint32_t before, uint32_t after) {
    if (before != after) {
        DSD_FPRINTF(stderr, "FAIL: %s before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_generation_changed(const char* label, uint32_t before, uint32_t after) {
    if (before == after) {
        DSD_FPRINTF(stderr, "FAIL: %s before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_double_near(const char* label, double got, double want, double tolerance) {
    if (std::fabs(got - want) > tolerance) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%.9f want=%.9f tolerance=%.9f\n", label, got, want, tolerance);
        return 1;
    }
    return 0;
}

int
main(void) {
    size_t used_after = 0U;
    size_t ring_pending = 0U;
    int cache_pending = 0;
    int drained = 0;
    uint32_t generation_before = 0U;
    uint32_t generation_after = 0U;

    /*
     * Retune preparation is split between output drain state, cached symbols,
     * and replay event boundaries. The helper calls below cover each state
     * transition without starting a live RTL worker thread.
     */
    int rc = rtl_stream_test_prepare_reconfigure_input(8U, &used_after, &generation_before, &generation_after);

    int failed = 0;
    failed |= expect_int_eq("prepare reconfigure input rc", rc, 0);
    failed |= expect_size_eq("retune gate clears queued output", used_after, 0U);
    failed |= expect_generation_changed("retune gate bumps output generation", generation_before, generation_after);

    rc = rtl_stream_test_retune_output_pending(0U, 3, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune pending helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees empty ring", ring_pending, 0U);
    failed |= expect_int_eq("retune drain sees cached symbols", cache_pending, 3);
    failed |= expect_int_eq("cached symbols do not block retune drain", drained, 1);

    rc = rtl_stream_test_retune_output_pending(5U, 0, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune ring pending helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees queued ring", ring_pending, 5U);
    failed |= expect_int_eq("retune drain sees empty cache", cache_pending, 0);
    failed |= expect_int_eq("queued ring keeps retune drain open", drained, 0);

    rc = rtl_stream_test_retune_output_pending(0U, 0, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune drained helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees drained ring", ring_pending, 0U);
    failed |= expect_int_eq("retune drain sees drained cache", cache_pending, 0);
    failed |= expect_int_eq("retune drain reports drained", drained, 1);

    cache_pending = -1;
    rc = rtl_stream_test_tune_result_output_drain(RTL_STREAM_TUNE_TIMEOUT, 5U, 3, &used_after, &cache_pending,
                                                  &generation_before, &generation_after);
    failed |= expect_int_eq("timeout tune drain helper rc", rc, 0);
    failed |= expect_size_eq("timeout tune drains queued output", used_after, 0U);
    failed |= expect_int_eq("timeout tune clears cached symbols", cache_pending, 0);
    failed |= expect_generation_changed("timeout tune bumps output generation", generation_before, generation_after);

    cache_pending = -1;
    rc = rtl_stream_test_tune_result_output_drain(RTL_STREAM_TUNE_DEFERRED, 5U, 3, &used_after, &cache_pending,
                                                  &generation_before, &generation_after);
    failed |= expect_int_eq("deferred tune drain helper rc", rc, 0);
    failed |= expect_size_eq("deferred tune leaves queued output", used_after, 5U);
    failed |= expect_int_eq("deferred tune leaves cached symbols", cache_pending, 3);
    failed |= expect_generation_eq("deferred tune keeps output generation", generation_before, generation_after);
    failed |= expect_int_eq("ok completion result keeps tune ok",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_OK, RTL_STREAM_TUNE_OK),
                            RTL_STREAM_TUNE_OK);
    failed |= expect_int_eq("failed completion result maps tune failed",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_OK, RTL_STREAM_TUNE_FAILED),
                            RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("timeout completion result keeps timeout",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_FAILED),
                            RTL_STREAM_TUNE_TIMEOUT);
    failed |= expect_int_eq("committed retune failure reports tune ok",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 1, 855000000U, 855000000U),
                            RTL_STREAM_TUNE_OK);
    failed |= expect_int_eq("uncommitted retune failure reports tune failed",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 1, 855000000U, 851000000U),
                            RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("failed retune without reconfigure reports tune failed",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 0, 855000000U, 855000000U),
                            RTL_STREAM_TUNE_FAILED);

    /*
     * Failed tune and capture-setting paths reconcile staged frequency/rate state
     * back to the applied RTL configuration, while accepted PPM changes survive a
     * later retune failure.
     */
    long int reconciled_opts_freq = 0;
    uint32_t reconciled_capture_freq = 0U;
    rc = dsd_rtl_stream_test_tune_failure_reconciles_applied(855000000U, 851000000U, &reconciled_opts_freq,
                                                             &reconciled_capture_freq);
    failed |= expect_int_eq("failed tune reconcile helper rc", rc, 0);
    failed |= expect_int_eq("failed tune rolls caller freq back to applied", (int)reconciled_opts_freq, 851000000);
    failed |= expect_int_eq("failed tune restores applied capture center", (int)reconciled_capture_freq, 851240000);

    int first_completion_result = -1;
    int second_completion_result = -1;
    rc = dsd_rtl_stream_test_retune_completion_result_binding(&first_completion_result, &second_completion_result);
    failed |= expect_int_eq("retune completion result binding helper rc", rc, 0);
    failed |= expect_int_eq("first completion keeps failed result", first_completion_result, RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("second completion keeps ok result", second_completion_result, RTL_STREAM_TUNE_OK);

    uint32_t full_restore_freq_hz = 0U;
    uint32_t full_restore_rate_hz = 0U;
    int full_restore_rate_out_hz = 0;
    uint32_t partial_restore_freq_hz = 0U;
    uint32_t partial_restore_rate_hz = 0U;
    int partial_restore_rate_out_hz = 0;
    rc = dsd_rtl_stream_test_capture_settings_failure_restore(&full_restore_freq_hz, &full_restore_rate_hz,
                                                              &full_restore_rate_out_hz, &partial_restore_freq_hz,
                                                              &partial_restore_rate_hz, &partial_restore_rate_out_hz);
    failed |= expect_int_eq("capture settings restore helper rc", rc, 0);
    failed |= expect_int_eq("frequency failure restores applied frequency", (int)full_restore_freq_hz, 851000000);
    failed |= expect_int_eq("frequency failure restores staged rate", (int)full_restore_rate_hz, 960000);
    failed |= expect_int_eq("frequency failure restores demod rate", full_restore_rate_out_hz, 48000);
    failed |= expect_int_eq("partial retune keeps applied frequency", (int)partial_restore_freq_hz, 855000000);
    failed |= expect_int_eq("partial retune restores prior rate", (int)partial_restore_rate_hz, 960000);
    failed |= expect_int_eq("partial retune restores demod rate", partial_restore_rate_out_hz, 48000);

    int ppm_after_failure = 0;
    failed |= expect_int_eq("accepted ppm store helper rc",
                            dsd_rtl_stream_test_ppm_store_if_applied(0, -7, &ppm_after_failure), 0);
    failed |= expect_int_eq("accepted ppm survives retune failure", ppm_after_failure, -7);
    failed |= expect_int_eq("failed ppm store helper rc",
                            dsd_rtl_stream_test_ppm_store_if_applied(-5, 11, &ppm_after_failure), 0);
    failed |= expect_int_eq("failed ppm keeps previous runtime ppm", ppm_after_failure, 3);

    /*
     * Clear-output and reacquire helpers coordinate the sample ring, cached
     * symbol count, FSK modem history, and generation counters used by the
     * demodulator thread.
     */
    cache_pending = -1;
    rc = rtl_stream_test_clear_output(7U, 3, &used_after, &cache_pending, &generation_before, &generation_after);
    failed |= expect_int_eq("clear output helper rc", rc, 0);
    failed |= expect_generation_changed("clear output bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("clear output clears queued ring", used_after, 0U);
    failed |= expect_int_eq("clear output resets cached symbols", cache_pending, 0);

    int have_prev_after_clear = -1;
    int reset_consumed = -1;
    int have_prev_after_consume = -1;
    rc = rtl_stream_test_clear_output_fsk_reset(7U, &have_prev_after_clear, &reset_consumed, &have_prev_after_consume);
    failed |= expect_int_eq("clear output fsk reset helper rc", rc, 0);
    failed |= expect_int_eq("clear output leaves fsk modem for demod thread", have_prev_after_clear, 1);
    failed |= expect_int_eq("clear output fsk reset consumed", reset_consumed, 1);
    failed |= expect_int_eq("clear output fsk reset clears modem history", have_prev_after_consume, 0);

    double fsk_cfo_hz = 0.0;
    int fsk_cfo_after_generation_bump = -1;
    int fsk_cfo_after_reset = -1;
    const double fsk_dc_rad_per_sample = 0.0125;
    rc = rtl_stream_test_fsk_cfo_snapshot(fsk_dc_rad_per_sample, 48000, &fsk_cfo_hz, &fsk_cfo_after_generation_bump,
                                          &fsk_cfo_after_reset);
    failed |= expect_int_eq("fsk cfo snapshot helper rc", rc, 0);
    failed |= expect_double_near(
        "fsk cfo snapshot conversion", fsk_cfo_hz,
        -static_cast<double>(static_cast<float>(fsk_dc_rad_per_sample)) * 48000.0 / 6.28318530717958647692, 1e-6);
    failed |= expect_int_eq("fsk cfo snapshot generation bump invalidates estimate", fsk_cfo_after_generation_bump, 0);
    failed |= expect_int_eq("fsk cfo snapshot reset invalidates estimate", fsk_cfo_after_reset, 0);

    int request_rc = -1;
    int consumed = -1;
    cache_pending = -1;
    rc = rtl_stream_test_fsk_reacquire(RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR, 9U, 4, &used_after, &cache_pending,
                                       &generation_before, &generation_after, &request_rc, &consumed);
    failed |= expect_int_eq("fsk discriminator reacquire helper rc", rc, 0);
    failed |= expect_int_eq("fsk discriminator reacquire request queued", request_rc, 1);
    failed |= expect_int_eq("fsk discriminator reacquire consumed", consumed, 1);
    failed |=
        expect_generation_changed("fsk discriminator reacquire bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("fsk discriminator reacquire clears queued ring", used_after, 0U);
    failed |= expect_int_eq("fsk discriminator reacquire resets cached symbols", cache_pending, 0);

    request_rc = -1;
    consumed = -1;
    cache_pending = -1;
    rc = rtl_stream_test_fsk_reacquire(RTL_STREAM_OUTPUT_AUDIO_MONITOR, 9U, 4, &used_after, &cache_pending,
                                       &generation_before, &generation_after, &request_rc, &consumed);
    failed |= expect_int_eq("inactive fsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("inactive fsk reacquire no-op", request_rc, 0);
    failed |= expect_int_eq("inactive fsk reacquire not consumed", consumed, 0);
    failed |= expect_generation_eq("inactive fsk reacquire keeps generation", generation_before, generation_after);
    failed |= expect_size_eq("inactive fsk reacquire leaves queued ring", used_after, 9U);
    failed |= expect_int_eq("inactive fsk reacquire leaves cached symbols", cache_pending, 4);

    int first_profile = -1;
    int second_profile = -1;
    uint32_t first_freq_hz = 0U;
    uint32_t second_freq_hz = 0U;
    uint32_t first_request_id = 0U;
    uint32_t second_request_id = 0U;
    rc = rtl_stream_test_retune_profile_request_binding(&first_profile, &second_profile, &first_freq_hz,
                                                        &second_freq_hz, &first_request_id, &second_request_id);
    failed |= expect_int_eq("retune profile request binding helper rc", rc, 0);
    failed |= expect_int_eq("first retune keeps CQPSK profile", first_profile, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |= expect_int_eq("second retune keeps C4FM profile", second_profile, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    failed |= expect_int_eq("first retune profile keeps frequency", (int)first_freq_hz, 855000000);
    failed |= expect_int_eq("second retune profile keeps frequency", (int)second_freq_hz, 851000000);
    failed |= expect_int_eq("first retune profile keeps request id", (int)first_request_id, 1);
    failed |= expect_int_eq("second retune profile keeps request id", (int)second_request_id, 2);

    /*
     * Retune requests also preserve profile-specific gain, autogain, and Soapy
     * settings fields so a coalesced manual retune cannot silently discard user
     * capture preferences.
     */
    int coalesced_profile = -1;
    uint32_t coalesced_profile_freq_hz = 0U;
    uint32_t coalesced_manual_freq_hz = 0U;
    uint32_t coalesced_request_id = 0U;
    uint32_t coalesced_returned_request_id = 0U;
    rc = rtl_stream_test_retune_profile_coalesced_no_profile(&coalesced_profile, &coalesced_profile_freq_hz,
                                                             &coalesced_manual_freq_hz, &coalesced_request_id,
                                                             &coalesced_returned_request_id);
    failed |= expect_int_eq("coalesced retune profile helper rc", rc, 0);
    failed |= expect_int_eq("coalesced no-profile retune keeps profile", coalesced_profile,
                            RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |=
        expect_int_eq("coalesced no-profile retune keeps profile frequency", (int)coalesced_profile_freq_hz, 855000000);
    failed |=
        expect_int_eq("coalesced no-profile retune keeps manual frequency", (int)coalesced_manual_freq_hz, 855000000);
    failed |= expect_int_eq("coalesced no-profile retune keeps request id", (int)coalesced_request_id, 1);
    failed |= expect_int_eq("coalesced no-profile retune returns coalesced request id",
                            (int)coalesced_returned_request_id, 1);

    int gain_is_set = -1;
    int gain_tenth_db = -1;
    int gain_is_auto = -1;
    int autogain_is_set = -1;
    int autogain_on = -1;
    rc = rtl_stream_test_retune_profile_gain_binding(&gain_is_set, &gain_tenth_db, &gain_is_auto, &autogain_is_set,
                                                     &autogain_on);
    failed |= expect_int_eq("retune gain profile helper rc", rc, 0);
    failed |= expect_int_eq("retune gain profile keeps gain-is-set", gain_is_set, 1);
    failed |= expect_int_eq("retune gain profile keeps manual gain", gain_tenth_db, 270);
    failed |= expect_int_eq("retune gain profile keeps manual mode", gain_is_auto, 0);
    failed |= expect_int_eq("retune gain profile keeps autogain-is-set", autogain_is_set, 1);
    failed |= expect_int_eq("retune gain profile keeps autogain off", autogain_on, 0);

    int settings_seen = -1;
    failed |= expect_int_eq("legacy Soapy config settings visibility helper",
                            rtl_device_test_soapy_config_settings_visibility(RTL_SOAPY_CONFIG_LEGACY_SIZE,
                                                                             "biasT_ctrl=false", &settings_seen),
                            0);
    failed |= expect_int_eq("legacy Soapy config ignores appended settings", settings_seen, 0);
    failed |= expect_int_eq(
        "sized Soapy config settings visibility helper",
        rtl_device_test_soapy_config_settings_visibility(RTL_SOAPY_CONFIG_SIZE, "biasT_ctrl=false", &settings_seen), 0);
    failed |= expect_int_eq("sized Soapy config observes appended settings", settings_seen, 1);

    /*
     * USB apply/readback checks keep retry behavior explicit: apply failures are
     * retried, readback failures are retried only when verification is enabled,
     * and zero readback remains invalid.
     */
    int apply_calls = -1;
    int verify_calls = -1;
    int used_attempts = -1;
    rc = rtl_device_test_usb_apply_retry(1, 10, 1, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry first-attempt rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry first-attempt apply calls", apply_calls, 1);
    failed |= expect_int_eq("rtl usb retry first-attempt verify calls", verify_calls, 1);
    failed |= expect_int_eq("rtl usb retry first-attempt used attempts", used_attempts, 1);

    rc = rtl_device_test_usb_apply_retry(1, 10, 3, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry apply succeeds late rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry apply succeeds late apply calls", apply_calls, 3);
    failed |= expect_int_eq("rtl usb retry apply succeeds late verify calls", verify_calls, 1);
    failed |= expect_int_eq("rtl usb retry apply succeeds late used attempts", used_attempts, 3);

    rc = rtl_device_test_usb_apply_retry(1, 10, 1, 3, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry verify succeeds late rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry verify succeeds late apply calls", apply_calls, 3);
    failed |= expect_int_eq("rtl usb retry verify succeeds late verify calls", verify_calls, 3);
    failed |= expect_int_eq("rtl usb retry verify succeeds late used attempts", used_attempts, 3);

    rc = rtl_device_test_usb_apply_retry(1, 10, 11, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry exhausted rc", rc, -1);
    failed |= expect_int_eq("rtl usb retry exhausted apply calls", apply_calls, 10);
    failed |= expect_int_eq("rtl usb retry exhausted verify calls", verify_calls, 0);
    failed |= expect_int_eq("rtl usb retry exhausted used attempts", used_attempts, 10);

    rc = rtl_device_test_usb_apply_retry(0, 10, 1, 10, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry disabled rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry disabled apply calls", apply_calls, 1);
    failed |= expect_int_eq("rtl usb retry disabled verify calls", verify_calls, 0);
    failed |= expect_int_eq("rtl usb retry disabled used attempts", used_attempts, 1);

    uint32_t actual_rate = 0U;
    rc = rtl_device_test_usb_sample_rate_readback(960000U, 960000U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback exact rc", rc, 0);
    failed |= expect_int_eq("rtl sample-rate readback exact actual", (int)actual_rate, 960000);
    actual_rate = 99U;
    rc = rtl_device_test_usb_sample_rate_readback(1024000U, 960000U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback accepts quantized rc", rc, 0);
    failed |= expect_int_eq("rtl sample-rate readback returns quantized actual", (int)actual_rate, 960000);
    rc = rtl_device_test_usb_sample_rate_readback(1024000U, 0U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback rejects zero rc", rc, -1);

    /*
     * Gain setup intentionally records non-fatal AGC failures while still treating
     * gain-mode failures as fatal, matching the live-device setup sequence.
     */
    int agc_calls = -1;
    int gain_mode_calls = -1;
    int gain_calls = -1;
    int recorded_agc_rc = 0;
    rc =
        rtl_device_test_usb_manual_gain_controls(-5, 0, 0, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain ignores AGC failure rc", rc, 0);
    failed |= expect_int_eq("manual gain AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain mode still applied", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain value still applied", gain_calls, 1);
    failed |= expect_int_eq("manual gain records AGC failure", recorded_agc_rc, -5);

    rc =
        rtl_device_test_usb_manual_gain_controls(0, -8, 0, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain mode failure remains fatal", rc, -8);
    failed |= expect_int_eq("manual gain mode failure AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain mode failure call count", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain skips value after mode failure", gain_calls, 0);
    failed |= expect_int_eq("manual gain mode failure records no AGC failure", recorded_agc_rc, 0);

    rc =
        rtl_device_test_usb_manual_gain_controls(0, 0, -9, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain value failure remains fatal", rc, -9);
    failed |= expect_int_eq("manual gain value failure AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain value failure mode call count", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain value failure call count", gain_calls, 1);
    failed |= expect_int_eq("manual gain value failure records no AGC failure", recorded_agc_rc, 0);

    rc = rtl_device_test_usb_auto_gain_controls(-6, 0, &agc_calls, &gain_mode_calls, &recorded_agc_rc);
    failed |= expect_int_eq("auto gain ignores AGC failure rc", rc, 0);
    failed |= expect_int_eq("auto gain AGC call count", agc_calls, 1);
    failed |= expect_int_eq("auto gain mode still applied", gain_mode_calls, 1);
    failed |= expect_int_eq("auto gain records AGC failure", recorded_agc_rc, -6);

    rc = rtl_device_test_usb_auto_gain_controls(0, -7, &agc_calls, &gain_mode_calls, &recorded_agc_rc);
    failed |= expect_int_eq("auto gain mode failure remains fatal", rc, -7);
    failed |= expect_int_eq("auto gain mode failure call count", gain_mode_calls, 1);
    failed |= expect_int_eq("auto gain skips AGC after mode failure", agc_calls, 0);
    failed |= expect_int_eq("auto gain mode failure records no AGC failure", recorded_agc_rc, 0);

    // Fragmented mute spans are coalesced so capture metadata stays IQ-pair aligned.
    uint64_t pending_mute = 0U;
    uint64_t emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 1U, 2U);
    failed |= expect_size_eq("odd mute fragment held", (size_t)emitted, 0U);
    failed |= expect_size_eq("odd mute pending byte", (size_t)pending_mute, 1U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 1U, 2U);
    failed |= expect_size_eq("second odd mute completes pair", (size_t)emitted, 2U);
    failed |= expect_size_eq("completed mute clears pending", (size_t)pending_mute, 0U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 3U, 2U);
    failed |= expect_size_eq("larger odd mute emits aligned part", (size_t)emitted, 2U);
    failed |= expect_size_eq("larger odd mute retains one byte", (size_t)pending_mute, 1U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 5U, 2U);
    failed |= expect_size_eq("fragmented mute coalesces with prior pending", (size_t)emitted, 6U);
    failed |= expect_size_eq("fragmented mute pending clears", (size_t)pending_mute, 0U);

    failed |= expect_int_eq("odd reconfigure hold completes empty mute",
                            rtl_device_test_complete_fragmented_capture_discard(0, 1U), 1);
    failed |= expect_int_eq("odd reconfigure hold extends even mute",
                            rtl_device_test_complete_fragmented_capture_discard(4, 1U), 5);
    failed |= expect_int_eq("odd reconfigure hold keeps odd mute",
                            rtl_device_test_complete_fragmented_capture_discard(5, 1U), 5);
    failed |= expect_int_eq("aligned reconfigure hold leaves mute",
                            rtl_device_test_complete_fragmented_capture_discard(4, 0U), 4);

    int hold = -1;
    int mute = -1;
    int mute_byte_phase = -1;
    rtl_device_test_end_capture_reconfigure_with_odd_carry(&hold, &mute, &mute_byte_phase);
    failed |= expect_int_eq("end reconfigure clears hold after completion", hold, 0);
    failed |= expect_int_eq("end reconfigure schedules odd carry mute byte", mute, 1);
    failed |= expect_int_eq("end reconfigure preserves carry until scheduled mute drains", mute_byte_phase, 1);

    hold = -1;
    failed |= expect_int_eq("begin reconfigure without writer helper",
                            rtl_device_test_begin_capture_reconfigure_without_writer(&hold), 0);
    failed |= expect_int_eq("begin reconfigure without writer activates hold", hold, 1);

    int native_formats[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t capture_counts[5] = {99U, 99U, 99U, 99U, 99U};
    rc = rtl_device_test_public_capture_policy(native_formats, 8U, capture_counts, 5U);
    failed |= expect_int_eq("public capture policy helper rc", rc, 0);
    failed |= expect_int_eq("native format null", native_formats[0], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("native format usb", native_formats[1], DSD_IQ_FORMAT_CU8);
    failed |= expect_int_eq("native format tcp", native_formats[2], DSD_IQ_FORMAT_CU8);
    failed |= expect_int_eq("native format Soapy CF32", native_formats[3], DSD_IQ_FORMAT_CF32);
    failed |= expect_int_eq("native format Soapy CS16", native_formats[4], DSD_IQ_FORMAT_CS16);
    failed |= expect_int_eq("native format Soapy none", native_formats[5], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("native format replay", native_formats[6], DSD_IQ_FORMAT_CS16);
    failed |= expect_int_eq("native format unknown backend", native_formats[7], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("capture count null", (int)capture_counts[0], 0);
    failed |= expect_int_eq("capture count starts zero", (int)capture_counts[1], 0);
    failed |= expect_int_eq("capture count ignores missing writer", (int)capture_counts[2], 0);
    failed |= expect_int_eq("capture count increments with writer", (int)capture_counts[3], 2);
    failed |= expect_int_eq("capture count detach resets", (int)capture_counts[4], 0);

    size_t held_ring_used = 99U;
    failed |= expect_int_eq("usb reconfigure discard helper",
                            rtl_device_test_usb_reconfigure_discards_samples(16U, &held_ring_used), 0);
    failed |= expect_size_eq("usb reconfigure discards samples", held_ring_used, 0U);

    // Replay RESET events clear phase/carry state only after pending output drains.
    int replay_phase = 3;
    int replay_have_carry = 1;
    uint8_t replay_carry_byte = 42U;
    rtl_device_test_replay_dispatch_reset_event_state(&replay_phase, &replay_have_carry, &replay_carry_byte);
    failed |= expect_int_eq("reset event clears replay fs4 phase", replay_phase, 0);
    failed |= expect_int_eq("reset event clears replay cu8 carry", replay_have_carry, 0);
    failed |= expect_int_eq("reset event clears replay carry byte", (int)replay_carry_byte, 0);
    failed |= expect_int_eq("reset event waits for queued ring samples",
                            rtl_device_test_replay_event_boundary_drained(2U, 3U, 3U), 0);
    failed |= expect_int_eq("reset event waits for reserved demod generation",
                            rtl_device_test_replay_event_boundary_drained(0U, 3U, 2U), 0);
    failed |=
        expect_int_eq("reset event boundary drained", rtl_device_test_replay_event_boundary_drained(0U, 3U, 3U), 1);

    /*
     * Replay input-level snapshots preserve source identity for USB/TCP CU8 and
     * accept CF32 only from the post-driver capture stage with aligned input and
     * sufficient scratch space.
     */
    const int rtl_test_backend_usb = 0;
    const int rtl_test_backend_tcp = 1;
    int input_level_rc = -99;
    int input_level_source = -99;
    uint64_t input_level_count = 0U;
    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CU8, rtl_test_backend_usb, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CU8 USB snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CU8 USB snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CU8 USB source", input_level_source, DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    failed |= expect_size_eq("replay CU8 USB sample count", (size_t)input_level_count, 8U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CU8, rtl_test_backend_tcp, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CU8 TCP snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CU8 TCP snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CU8 TCP source", input_level_source, DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 16U, 4U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CF32 source", input_level_source, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32);
    failed |= expect_size_eq("replay CF32 sample count", (size_t)input_level_count, 4U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CS16, rtl_test_backend_usb, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CS16 snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CS16 snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CS16 source", input_level_source, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16);
    failed |= expect_size_eq("replay CS16 sample count", (size_t)input_level_count, 4U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CS16, rtl_test_backend_usb, "", 6U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CS16 rejects misaligned helper rc", rc, 0);
    failed |= expect_int_eq("replay CS16 rejects misaligned bytes", input_level_rc, -1);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb, "pre_ring", 16U, 4U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects wrong stage helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects wrong stage", input_level_rc, -1);
    failed |= expect_int_eq("replay CF32 wrong stage clears source", input_level_source, -1);
    failed |= expect_size_eq("replay CF32 wrong stage clears count", (size_t)input_level_count, 0U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 10U, 4U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects misaligned helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects misaligned bytes", input_level_rc, -1);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 16U, 3U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects small scratch helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects small scratch", input_level_rc, -1);

    /*
     * Replay sample conversion is stateful across chunks: CU8 preserves odd
     * carry bytes, while CF32 post-driver blocks optionally apply J/4 rotation.
     */
    float converted[8] = {};
    int convert_phase = -1;
    int convert_have_carry = -1;
    uint8_t convert_carry_byte = 0U;
    const uint8_t cu8_with_carry[] = {128U, 130U, 132U};
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CU8, "", 0, 0, cu8_with_carry, sizeof(cu8_with_carry), 8U, 2, 1, 126U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CU8 carry conversion count", rc, 4);
    failed |= expect_double_near("replay CU8 carried I sample", converted[0], (126.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 carried Q sample", converted[1], (128.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 aligned I sample", converted[2], (130.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 aligned Q sample", converted[3], (132.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_int_eq("replay CU8 no-rotate preserves phase", convert_phase, 2);
    failed |= expect_int_eq("replay CU8 carry consumed", convert_have_carry, 0);
    failed |= expect_int_eq("replay CU8 consumed carry byte unchanged", (int)convert_carry_byte, 126);

    const uint8_t cu8_odd_tail[] = {127U, 128U, 129U};
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CU8, "", 0, 0, cu8_odd_tail, sizeof(cu8_odd_tail), 8U, 0, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CU8 odd tail conversion count", rc, 2);
    failed |= expect_int_eq("replay CU8 odd tail retained", convert_have_carry, 1);
    failed |= expect_int_eq("replay CU8 odd tail byte", (int)convert_carry_byte, 129);

    float cf32_samples[] = {1.0f, 2.0f, 3.0f, 4.0f};
    uint8_t cf32_raw[sizeof(cf32_samples)] = {};
    DSD_MEMCPY(cf32_raw, cf32_samples, sizeof(cf32_samples));
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 1, 0, cf32_raw, sizeof(cf32_raw),
                                   8U, 1, 0, 0U, converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CF32 rotated conversion count", rc, 4);
    failed |= expect_double_near("replay CF32 phase1 rotated I", converted[0], -2.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase1 rotated Q", converted[1], 1.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase2 rotated I", converted[2], -3.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase2 rotated Q", converted[3], -4.0, 1e-6);
    failed |= expect_int_eq("replay CF32 rotation advances phase", convert_phase, 3);
    failed |= expect_int_eq("replay CF32 leaves carry clear", convert_have_carry, 0);

    rc = call_replay_convert_block(DSD_IQ_FORMAT_CF32, "pre_ring", 1, 0, cf32_raw, sizeof(cf32_raw), 8U, 1, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CF32 rejects wrong stage conversion", rc, -1);

    rc = call_replay_convert_block(DSD_IQ_FORMAT_CS16, "", 0, 0, cu8_odd_tail, sizeof(cu8_odd_tail), 8U, 0, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay converter rejects unsupported format", rc, -1);

    /*
     * CU8 callback chunking must preserve odd trailing bytes between callbacks,
     * and full-ring drops must keep both drop accounting and FS/4 phase aligned.
     */
    size_t u8_used = 0U;
    int u8_phase = -1;
    int u8_carry_valid = -1;
    uint8_t u8_carry_byte = 0U;
    int first_status = -1;
    int second_status = -1;
    rc = rtl_device_test_u8_odd_carry_bridge(&u8_used, &u8_phase, &u8_carry_valid, &u8_carry_byte, &first_status,
                                             &second_status);
    failed |= expect_int_eq("u8 odd-carry helper rc", rc, 0);
    failed |= expect_int_eq("u8 odd-carry first chunk accepted", first_status, 0);
    failed |= expect_int_eq("u8 odd-carry second chunk accepted", second_status, 0);
    failed |= expect_size_eq("u8 odd-carry bridges one IQ pair", u8_used, 2U);
    failed |= expect_int_eq("u8 odd-carry advances one FS/4 pair", u8_phase, 1);
    failed |= expect_int_eq("u8 odd-carry retains final byte", u8_carry_valid, 1);
    failed |= expect_int_eq("u8 odd-carry byte preserved", (int)u8_carry_byte, 126);

    uint64_t u8_drops = 0U;
    uint64_t full_events = 0U;
    int full_status = -1;
    rc = rtl_device_test_u8_full_ring_drop(&u8_used, &u8_drops, &full_events, &u8_phase, &full_status);
    failed |= expect_int_eq("u8 full-ring helper rc", rc, 0);
    failed |= expect_int_eq("u8 full-ring reports exhaustion", full_status, 1);
    failed |= expect_size_eq("u8 full-ring keeps one pair", u8_used, 2U);
    failed |= expect_size_eq("u8 full-ring drops aligned remainder", (size_t)u8_drops, 4U);
    failed |= expect_size_eq("u8 full-ring counts reserve exhaustion", (size_t)full_events, 1U);
    failed |= expect_int_eq("u8 full-ring advances phase over committed and dropped pairs", u8_phase, 3);

    int stale_dev_carry_valid = -1;
    int stale_local_carry_valid = -1;
    rc = rtl_device_test_u8_generation_stale_drop(&u8_drops, &u8_phase, &stale_dev_carry_valid,
                                                  &stale_local_carry_valid, &full_status);
    failed |= expect_int_eq("u8 stale-generation helper rc", rc, 0);
    failed |= expect_int_eq("u8 stale-generation reports stale", full_status, 1);
    failed |= expect_size_eq("u8 stale-generation drops produced and aligned remainder", (size_t)u8_drops, 6U);
    failed |= expect_int_eq("u8 stale-generation advances phase over aligned remainder", u8_phase, 1);
    failed |= expect_int_eq("u8 stale-generation clears device carry", stale_dev_carry_valid, 0);
    failed |= expect_int_eq("u8 stale-generation clears local carry", stale_local_carry_valid, 0);

    /*
     * Miscellaneous RTL helper contracts are pure formatting/alignment rules:
     * tuner-type labels stay stable, capture event reasons are bounded, Soapy
     * setting text is trimmed, and rtl_tcp buffers are page-aligned upward.
     */
    char tuner_labels[96] = {};
    char reason_small[8] = {};
    char reason_null[4] = {'x', 'x', 'x', '\0'};
    char trimmed[32] = {};
    size_t rounded_pages[4] = {};
    rc = rtl_device_test_misc_string_helpers(tuner_labels, sizeof(tuner_labels), reason_small, sizeof(reason_small),
                                             reason_null, sizeof(reason_null), trimmed, sizeof(trimmed), rounded_pages,
                                             sizeof(rounded_pages) / sizeof(rounded_pages[0]));
    failed |= expect_int_eq("rtl misc string helper rc", rc, 0);
    if (std::strcmp(tuner_labels, "E4000|FC0012|FC0013|FC2580|R820T|R828D|unknown") != 0) {
        DSD_FPRINTF(stderr, "FAIL: tuner labels got='%s'\n", tuner_labels);
        failed = 1;
    }
    if (std::strcmp(reason_small, "capture") != 0) {
        DSD_FPRINTF(stderr, "FAIL: truncated reason got='%s'\n", reason_small);
        failed = 1;
    }
    if (std::strcmp(reason_null, "") != 0) {
        DSD_FPRINTF(stderr, "FAIL: null reason got='%s'\n", reason_null);
        failed = 1;
    }
    if (std::strcmp(trimmed, "gain = 12.5") != 0) {
        DSD_FPRINTF(stderr, "FAIL: trimmed setting got='%s'\n", trimmed);
        failed = 1;
    }
    failed |= expect_size_eq("round zero page", rounded_pages[0], 0U);
    failed |= expect_size_eq("round one page", rounded_pages[1], 4096U);
    failed |= expect_size_eq("round exact page", rounded_pages[2], 4096U);
    failed |= expect_size_eq("round next page", rounded_pages[3], 8192U);
    failed |= expect_int_eq("rtl misc helper rejects short arrays",
                            rtl_device_test_misc_string_helpers(tuner_labels, sizeof(tuner_labels), reason_small,
                                                                sizeof(reason_small), reason_null, sizeof(reason_null),
                                                                trimmed, sizeof(trimmed), rounded_pages, 3U),
                            -1);

    size_t tcp_bufsz[8] = {};
    int tcp_waitall[4] = {};
    uint64_t tcp_deltas[2] = {};
    int agc_want = -1;
    rc = rtl_device_test_tcp_policy_helpers(tcp_bufsz, sizeof(tcp_bufsz) / sizeof(tcp_bufsz[0]), tcp_waitall,
                                            sizeof(tcp_waitall) / sizeof(tcp_waitall[0]), tcp_deltas,
                                            sizeof(tcp_deltas) / sizeof(tcp_deltas[0]), &agc_want);
    failed |= expect_int_eq("rtl tcp policy helper rc", rc, 0);
    failed |= expect_size_eq("rtl tcp null default buffer", tcp_bufsz[0], 65536U);
    failed |= expect_size_eq("rtl tcp backend default buffer", tcp_bufsz[1], 16384U);
    failed |= expect_size_eq("rtl usb zero-rate default buffer", tcp_bufsz[2], 65536U);
    failed |= expect_size_eq("rtl low-rate minimum buffer", tcp_bufsz[3], 16384U);
    failed |= expect_size_eq("rtl rate-scaled buffer", tcp_bufsz[4], 38400U);
    failed |= expect_size_eq("rtl high-rate capped buffer", tcp_bufsz[5], 262144U);
    failed |= expect_size_eq("rtl config buffer override", tcp_bufsz[6], 12345U);
    failed |= expect_size_eq("rtl invalid config buffer falls back", tcp_bufsz[7], 262144U);
    failed |= expect_int_eq("rtl null waitall default", tcp_waitall[0], 1);
    failed |= expect_int_eq("rtl tcp waitall default", tcp_waitall[1], 0);
    failed |= expect_int_eq("rtl waitall explicit enable", tcp_waitall[2], 1);
    failed |= expect_int_eq("rtl waitall explicit disable", tcp_waitall[3], 0);
    failed |= expect_size_eq("rtl counter delta monotonic", (size_t)tcp_deltas[0], 60U);
    failed |= expect_size_eq("rtl counter delta clamps wrap/reorder", (size_t)tcp_deltas[1], 0U);
    failed |= expect_int_eq("rtl default agc config", agc_want, 1);

    failed |= expect_int_eq("rtl tcp policy helper rejects short buffers",
                            rtl_device_test_tcp_policy_helpers(tcp_bufsz, 7U, tcp_waitall,
                                                               sizeof(tcp_waitall) / sizeof(tcp_waitall[0]), tcp_deltas,
                                                               sizeof(tcp_deltas) / sizeof(tcp_deltas[0]), &agc_want),
                            -1);

    return failed ? 1 : 0;
}
