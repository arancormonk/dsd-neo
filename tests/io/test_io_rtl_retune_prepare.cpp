// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#error "DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS must be enabled for this test."
#endif

#include <cstdint>
#include <cstdio>
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
extern "C" int rtl_device_test_usb_manual_gain_controls(int agc_rc, int gain_mode_rc, int gain_rc, int* out_agc_calls,
                                                        int* out_gain_mode_calls, int* out_gain_calls,
                                                        int* out_recorded_agc_rc);
extern "C" int rtl_device_test_usb_auto_gain_controls(int agc_rc, int gain_mode_rc, int* out_agc_calls,
                                                      int* out_gain_mode_calls, int* out_recorded_agc_rc);
extern "C" int dsd_rtl_stream_test_tune_completion_result(int wait_result, int completion_result);
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

    cache_pending = -1;
    rc = rtl_stream_test_clear_output(7U, 3, &used_after, &cache_pending, &generation_before, &generation_after);
    failed |= expect_int_eq("clear output helper rc", rc, 0);
    failed |= expect_generation_changed("clear output bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("clear output clears queued ring", used_after, 0U);
    failed |= expect_int_eq("clear output resets cached symbols", cache_pending, 0);

    int request_rc = -1;
    int consumed = -1;
    cache_pending = -1;
    rc = rtl_stream_test_fsk_reacquire(RTL_STREAM_OUTPUT_SYMBOL_FSK, 9U, 4, &used_after, &cache_pending,
                                       &generation_before, &generation_after, &request_rc, &consumed);
    failed |= expect_int_eq("fsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("fsk reacquire request queued", request_rc, 1);
    failed |= expect_int_eq("fsk reacquire consumed", consumed, 1);
    failed |= expect_generation_changed("fsk reacquire bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("fsk reacquire clears queued ring", used_after, 0U);
    failed |= expect_int_eq("fsk reacquire resets cached symbols", cache_pending, 0);

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

    return failed ? 1 : 0;
}
