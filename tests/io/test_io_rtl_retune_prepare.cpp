// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#error "DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS must be enabled for this test."
#endif

#include <cstdint>
#include <cstdio>
#include <dsd-neo/io/rtl_stream_c.h>
#include "dsd-neo/core/safe_api.h"

extern "C" uint64_t rtl_device_test_coalesce_capture_mute_duration(uint64_t* pending_bytes, uint64_t duration_bytes,
                                                                   size_t alignment);
extern "C" int rtl_device_test_complete_fragmented_capture_discard(int byte_count, unsigned int partial_byte_count);
extern "C" void rtl_device_test_end_capture_reconfigure_with_odd_carry(int* out_hold, int* out_mute,
                                                                       int* out_mute_byte_phase);
extern "C" void rtl_device_test_replay_dispatch_reset_event_state(int* phase, int* have_carry, uint8_t* carry_byte);
extern "C" int rtl_device_test_replay_event_boundary_drained(size_t ring_used, uint64_t submitted_gen,
                                                             uint64_t consumed_gen);

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
    failed |= expect_size_eq("queued output preserved for drain policy", used_after, 8U);
    failed |= expect_generation_eq("output generation left for drain policy", generation_before, generation_after);

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
