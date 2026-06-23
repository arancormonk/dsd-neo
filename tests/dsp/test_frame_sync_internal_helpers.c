// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/sockets.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <math.h>
#include <stdint.h>
#include <time.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

dsd_socket_t
Connect(char* hostname, int portno) { // NOLINT(misc-use-internal-linkage)
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
openAudioInput(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return -1;
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

void
getTimeC_buf(char out[9]) { // NOLINT(misc-use-internal-linkage)
    if (out) {
        DSD_SNPRINTF(out, 9, "%s", "00:00:00");
    }
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
dsd_mark_cc_sync(dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
}

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    (void)st;
    (void)sym;
    return 255;
}

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    (void)mean_power;
    return 0.0;
}

void
lpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
hpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
pbf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

static void
reset(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
}

static void
test_sps_hunt_skips_disabled_protocol_rates(void) {
    static const int sym_rate_cycle[] = {4800, 2400, 9600, 6000, 4800};
    static const int levels_cycle[] = {4, 4, 2, 4, 2};
    static const int cycle_count = (int)(sizeof(sym_rate_cycle) / sizeof(sym_rate_cycle[0]));
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dstar = 1;
    state.sps_hunt_idx = 3;
    assert(dsd_frame_sync_test_sps_hunt_next_index(&opts, &state, sym_rate_cycle, levels_cycle, cycle_count) == 4);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    state.sps_hunt_idx = 4;
    assert(dsd_frame_sync_test_sps_hunt_next_index(&opts, &state, sym_rate_cycle, levels_cycle, cycle_count) == 0);

    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 0;
    assert(dsd_frame_sync_test_sps_hunt_next_index(&opts, &state, sym_rate_cycle, levels_cycle, cycle_count) == 1);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    state.sps_hunt_idx = 1;
    assert(dsd_frame_sync_test_sps_hunt_next_index(&opts, &state, sym_rate_cycle, levels_cycle, cycle_count) == 2);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 2;
    assert(dsd_frame_sync_test_sps_hunt_next_index(&opts, &state, sym_rate_cycle, levels_cycle, cycle_count) == 3);
}

static void
test_sps_hunt_profile_updates_timing(void) {
    static const int sym_rate_cycle[] = {4800, 2400, 9600, 6000, 4800};
    static const int levels_cycle[] = {4, 4, 2, 4, 2};
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.wav_decimator = 48000;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;

    dsd_frame_sync_test_apply_sps_hunt_profile(&opts, &state, 1, sym_rate_cycle, levels_cycle);
    int expected_sps = dsd_opts_compute_sps_rate(&opts, sym_rate_cycle[1], dsd_opts_current_input_timing_rate(&opts));
    assert(state.sps_hunt_idx == 1);
    assert(state.samplesPerSymbol == expected_sps);
    assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));

    dsd_frame_sync_test_apply_sps_hunt_profile(&opts, &state, 1, sym_rate_cycle, levels_cycle);
    assert(state.sps_hunt_idx == 1);
    assert(state.samplesPerSymbol == expected_sps);
    assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));
}

static void
test_elapsed_seconds_prefers_monotonic_then_wall_time(void) {
    assert(fabs(dsd_frame_sync_test_elapsed_seconds(12.5, (time_t)20, 10.0, (time_t)3) - 2.5) < 0.000001);
    assert(fabs(dsd_frame_sync_test_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)3) - 17.0) < 0.000001);
    assert(dsd_frame_sync_test_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)0) > 1.0e8);
}

static void
test_p25_slot_activity_honors_ring_and_hangtime(void) {
    static dsd_opts opts;
    static dsd_state state;
    int left_active = 0;
    int right_active = 0;

    reset(&opts, &state);
    opts.trunk_hangtime = 2.0f;
    state.p25_p2_last_mac_active_m[0] = 99.8;
    state.p25_p2_last_mac_active_m[1] = 95.0;
    state.p25_p2_audio_ring_count[0] = 1;
    state.p25_p2_audio_allowed[1] = 1;
    dsd_frame_sync_test_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 0.75, 1.0, &left_active,
                                          &right_active);
    assert(left_active == 1);
    assert(right_active == 1);

    left_active = 0;
    right_active = 0;
    dsd_frame_sync_test_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 0.75, 2.0, &left_active,
                                          &right_active);
    assert(left_active == 1);
    assert(right_active == 0);
}

static void
test_hamming_helpers_find_best_patterns(void) {
    const char* patterns[] = {"012301", "333333", "111111"};

    assert(dsd_frame_sync_test_hamming_distance_pattern("012301", "012301", 6) == 0);
    assert(dsd_frame_sync_test_hamming_distance_pattern("012301", "012300", 6) == 1);
    assert(dsd_frame_sync_test_best_ham_for_patterns("111101", patterns, 3, 6, 6) == 1);
    assert(dsd_frame_sync_test_best_ham_for_patterns("222222", patterns, 3, 6, 3) == 3);
    assert(dsd_frame_sync_test_best_nxdn_scaled_ham("3131331131", 24) == 0);
    assert(dsd_frame_sync_test_best_nxdn_scaled_ham("1313113300", 24) == 5);
}

#ifdef USE_RADIO
static void
test_rtl_symbol_profile_selection(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dstar = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 4800, 2)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(dsd_frame_sync_test_rtl_levels_for_symbol_rate(&opts, 4800, 0) == 2);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 9600, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE);
    assert(dsd_frame_sync_test_rtl_levels_for_symbol_rate(&opts, 9600, 0) == 2);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 6000, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    reset(&opts, &state);
    opts.frame_x2tdma = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 6000, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 4800, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.rf_mod = 0;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 4800, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(&opts, &state, 4800, 0)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    assert(dsd_frame_sync_test_rtl_profile_for_symbol_rate(NULL, NULL, 2400, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);
    assert(dsd_frame_sync_test_rtl_levels_for_symbol_rate(NULL, 4800, 0) == 4);
    assert(dsd_frame_sync_test_rtl_levels_for_symbol_rate(NULL, 4800, 2) == 2);
}
#endif

int
main(void) {
    test_sps_hunt_skips_disabled_protocol_rates();
    test_sps_hunt_profile_updates_timing();
    test_elapsed_seconds_prefers_monotonic_then_wall_time();
    test_p25_slot_activity_honors_ring_and_hangtime();
    test_hamming_helpers_find_best_patterns();
#ifdef USE_RADIO
    test_rtl_symbol_profile_selection();
#endif
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(misc-use-internal-linkage)
