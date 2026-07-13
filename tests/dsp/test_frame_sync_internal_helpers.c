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
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_test_support.h"

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

int
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return 0;
}

void
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out,
                          size_t out_size) { // NOLINT(misc-use-internal-linkage)
    (void)timestamp;
    (void)format;
    return out ? DSD_SNPRINTF(out, out_size, "%s", "00:00:00") >= 0 : 0;
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
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol, const dsd_dibit_soft_t* soft) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
    (void)soft;
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
    static dsd_opts opts;
    static dsd_state state;

    static const int expected_rates[] = {4800, 2400, 9600, 6000, 4800};
    static const int expected_levels[] = {4, 4, 2, 4, 2};
    assert(dsd_frame_sync_test_sps_hunt_profile_count() == 5);
    for (int i = 0; i < 5; i++) {
        assert(dsd_frame_sync_test_sps_hunt_profile_rate(i) == expected_rates[i]);
        assert(dsd_frame_sync_test_sps_hunt_profile_levels(i) == expected_levels[i]);
    }

    reset(&opts, &state);
    opts.frame_dstar = 1;
    state.sps_hunt_idx = 3;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 4);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    state.sps_hunt_idx = 4;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 0);

    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 0;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 1);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    state.sps_hunt_idx = 1;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 2;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 3);

    reset(&opts, &state);
    state.sps_hunt_idx = 2;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 2);
}

static void
test_sps_hunt_profile_updates_timing(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.wav_decimator = 48000;
    for (int profile_index = 0; profile_index < dsd_frame_sync_test_sps_hunt_profile_count(); profile_index++) {
        state.sps_hunt_idx = (profile_index + 1) % dsd_frame_sync_test_sps_hunt_profile_count();
        state.samplesPerSymbol = -1;
        state.symbolCenter = -1;
        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        int symbol_rate = dsd_frame_sync_test_sps_hunt_profile_rate(profile_index);
        int expected_sps = dsd_opts_compute_sps_rate(&opts, symbol_rate, dsd_opts_current_input_timing_rate(&opts));
        assert(state.sps_hunt_idx == profile_index);
        assert(state.samplesPerSymbol == expected_sps);
        assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));

        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        assert(state.samplesPerSymbol == expected_sps);
        assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));
    }

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    state.sps_hunt_idx = 4;
    state.sps_hunt_counter = 100;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == 4);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);

    /* -mg keeps the GFSK demodulator locked while alternating same-timing
     * four-level and binary gates, so D-STAR remains reachable. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_gfsk = 1;
    state.rf_mod = 2;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) - 1;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(state.sps_hunt_counter == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.rf_mod == 2);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    /* -mc likewise preserves the C4FM demodulator while exposing D-STAR's
     * same-rate binary matcher gate. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) - 1;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(state.sps_hunt_counter == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.rf_mod == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    /* At 16 kHz, 4800 and 6000 symbols/s both round to 3 SPS. Generic demodulator
     * locks still rotate gates, but the profile-specific -m2/M selection must not. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) - 1;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.min = -3.0f;
    state.max = 3.0f;
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;
    assert(dsd_opts_compute_sps_rate(&opts, 4800, 0) == state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.sps_hunt_counter == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* A known FDMA CC retune supersedes the P25p2 helper's original profile choice. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) - 1;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 4800, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_opts_compute_sps_rate(&opts, 6000, 0) == state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) - 1;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == 0);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 8;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 3);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == 8);
}

static void
test_sps_hunt_reconciles_external_timing(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 3);
    assert(state.samplesPerSymbol == 8);
    assert(state.symbolCenter == 3);
    assert(state.rf_mod == 1);
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* The normal -f2 preset selects QPSK and 6000-rate timing without a CLI modulation lock. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p2 = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 1);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* Manual -m2 carries profile 3 explicitly because low input rates can round
     * the 4800- and 6000-symbol timings to the same samples-per-symbol value. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 11025;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 11025);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    assert(dsd_opts_compute_sps_rate(&opts, 4800, 11025) == state.samplesPerSymbol);
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 7;
    state.symbolCenter = 3;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 7);
    assert(state.symbolCenter == 3);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = 3;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
}

static void
test_binary_profiles_override_unlocked_qpsk(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_provoice = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    frame_sync_apply_sps_hunt_profile(&opts, &state, 2, 0);
    assert(state.sps_hunt_idx == 2);
    assert(state.rf_mod == 2);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    state.sps_hunt_idx = 4;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.rf_mod = 1;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 4);
    assert(state.rf_mod == 2);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    frame_sync_apply_sps_hunt_profile(&opts, &state, 4, 0);
    assert(state.sps_hunt_idx == 4);
    assert(state.rf_mod == 1);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
}

static void
test_four_level_profiles_reset_inherited_modulation(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.rf_mod == 0);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    opts.frame_provoice = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 0);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.rf_mod == 2);
}

static void
test_nxdn_variant_follows_active_profile(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_NONE);
    assert(dsd_frame_sync_active_nxdn_variant(NULL, &state) == DSD_NXDN_VARIANT_NONE);

    opts.frame_nxdn48 = 1;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, NULL) == DSD_NXDN_VARIANT_48);
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 1;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, NULL) == DSD_NXDN_VARIANT_96);

    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_96);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_48);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_NONE);
}

static void
test_bounded_symbol_history_readiness_and_wrap(void) {
    static const int window_lengths[] = {8, 10, 12, 16, 20, 24, 32, 48};
    char symbols[80];
    char out[49];
    for (int i = 0; i < (int)sizeof(symbols); i++) {
        symbols[i] = (char)('0' + (i & 3));
    }

    for (size_t i = 0; i < sizeof(window_lengths) / sizeof(window_lengths[0]); i++) {
        const int length = window_lengths[i];
        DSD_MEMSET(out, 'x', sizeof(out));
        assert(dsd_frame_sync_test_history_window(symbols, length - 1, length, out, (int)sizeof(out)) == 0);
        assert(dsd_frame_sync_test_history_window(symbols, length, length, out, (int)sizeof(out)) == 1);
        assert(memcmp(out, symbols, (size_t)length) == 0);
        assert(out[length] == '\0');

        assert(dsd_frame_sync_test_history_window(symbols, (int)sizeof(symbols), length, out, (int)sizeof(out)) == 1);
        assert(memcmp(out, symbols + sizeof(symbols) - (size_t)length, (size_t)length) == 0);
        assert(out[length] == '\0');
    }
}

static void
test_provoice_candidate_does_not_shadow_dstar_or_nxdn(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_dstar = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_dstar = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 1;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

static void
test_symbol_replay_bypasses_sps_profile_gating(void) {
    static const int symbol_input_types[] = {AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT};
    static dsd_opts opts;
    static dsd_state state;

    for (size_t i = 0; i < sizeof(symbol_input_types) / sizeof(symbol_input_types[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_p25p2 = 1;
        state.sps_hunt_idx = 0;
        state.min = -3.0f;
        state.max = 3.0f;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    }

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 0;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_NONE);
}

static void
test_symbol_replay_requires_explicit_nxdn_variant(void) {
    static const int symbol_input_types[] = {AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT};
    static dsd_opts opts;
    static dsd_state state;

    for (size_t i = 0; i < sizeof(symbol_input_types) / sizeof(symbol_input_types[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn48 = 1;
        opts.frame_nxdn96 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);

        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn48 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
        assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_48);

        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn96 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
        assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_96);
    }
}

static void
test_manual_p25p2_c4fm_bypasses_profile_gating(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    opts.mod_c4fm = 1;
    opts.mod_cli_lock = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 8;
    state.min = -3.0f;
    state.max = 3.0f;

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == 8);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_NONE);

    opts.mod_p25p2_c4fm = 1;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
}

static void
test_locked_p25p2_c4fm_survives_sync(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    opts.mod_c4fm = 1;
    opts.mod_cli_lock = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.min = -3.0f;
    state.max = 3.0f;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(state.rf_mod == 0);
}

static void
test_m17_auto_preamble_disambiguation_preserves_forced_tolerance(void) {
    static dsd_opts opts;
    static dsd_state state;
    char one_error_preamble[9];
    DSD_MEMCPY(one_error_preamble, M17_PRE, sizeof(one_error_preamble));
    one_error_preamble[0] = one_error_preamble[0] == '1' ? '3' : '1';

    reset(&opts, &state);
    opts.frame_m17 = 1;
    state.sps_hunt_idx = 0;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, one_error_preamble, 8) == DSD_SYNC_M17_PRE_POS);

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dmr = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = 0;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, one_error_preamble, 8) == DSD_SYNC_NONE);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_PRE, 8) == DSD_SYNC_M17_PRE_POS);
}

static void
test_short_m17_window_estimates_levels_without_warm_start_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    float levels[8];

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.msize = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 8; i++) {
        levels[i] = M17_PRE[i] == '3' ? -3.0f : 3.0f;
    }

    assert(state.symbol_history == NULL);
    assert(dsd_frame_sync_test_eval_window(&opts, &state, M17_PRE, levels, 8) == DSD_SYNC_M17_PRE_POS);
    assert(fabsf(state.min - (-1.5f)) < 0.0001f);
    assert(fabsf(state.max - 1.5f) < 0.0001f);
}

static void
test_m17_preamble_requires_context_when_dstar_is_enabled(void) {
    static const char* const dstar_patterns[] = {DSTAR_SYNC, INV_DSTAR_SYNC, DSTAR_HD, INV_DSTAR_HD};
    static const char repeated_pre[] = M17_PRE M17_PRE;
    static const char repeated_piv[] = M17_PIV M17_PIV;
    static dsd_opts opts;
    static dsd_state state;

    for (size_t pattern_index = 0; pattern_index < sizeof(dstar_patterns) / sizeof(dstar_patterns[0]);
         pattern_index++) {
        for (int symbol_count = 8; symbol_count <= 24; symbol_count++) {
            reset(&opts, &state);
            opts.frame_m17 = 1;
            opts.frame_dstar = 1;
            state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
            assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, dstar_patterns[pattern_index], symbol_count)
                   == DSD_SYNC_NONE);
        }
    }

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, repeated_pre, 16) == DSD_SYNC_M17_PRE_POS);

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, repeated_piv, 16) == DSD_SYNC_M17_PRE_NEG);
}

static void
test_elapsed_seconds_prefers_monotonic_then_wall_time(void) {
    assert(fabs(frame_sync_elapsed_seconds(12.5, (time_t)20, 10.0, (time_t)3) - 2.5) < 0.000001);
    assert(fabs(frame_sync_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)3) - 17.0) < 0.000001);
    assert(frame_sync_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)0) > 1.0e8);
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
    frame_sync_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 0.75, 1.0, &left_active, &right_active);
    assert(left_active == 1);
    assert(right_active == 1);

    left_active = 0;
    right_active = 0;
    frame_sync_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 0.75, 2.0, &left_active, &right_active);
    assert(left_active == 1);
    assert(right_active == 0);
}

static void
test_hamming_helpers_find_best_patterns(void) {
    const char* patterns[] = {"012301", "333333", "111111"};

    assert(frame_sync_hamming_distance_pattern("012301", "012301", 6) == 0);
    assert(frame_sync_hamming_distance_pattern("012301", "012300", 6) == 1);
    assert(frame_sync_best_ham_for_patterns("111101", patterns, 3, 6, 6) == 1);
    assert(frame_sync_best_ham_for_patterns("222222", patterns, 3, 6, 3) == 3);
    assert(frame_sync_best_nxdn_scaled_ham("3131331131", 24) == 0);
    assert(frame_sync_best_nxdn_scaled_ham("1313113300", 24) == 5);
}

#ifdef USE_RADIO
static double g_snr_c4fm = -100.0;
static double g_snr_c4fm_eye = -100.0;
static double g_snr_cqpsk = -100.0;
static double g_snr_gfsk = -100.0;
static double g_snr_qpsk_const = -100.0;
static int g_frame_sync_tick_calls = 0;
static int g_profile_set_calls = 0;
static int g_profile_rate = 0;
static int g_profile_levels = 0;
static int g_profile_channel = 0;
static int g_profile_cqpsk = 0;
static int g_profile_ted_sps = 0;

static double
fake_snr_c4fm_db(void) {
    return g_snr_c4fm;
}

static double
fake_snr_c4fm_eye_db(void) {
    return g_snr_c4fm_eye;
}

static double
fake_snr_cqpsk_db(void) {
    return g_snr_cqpsk;
}

static double
fake_snr_gfsk_db(void) {
    return g_snr_gfsk;
}

static double
fake_snr_qpsk_const_db(void) {
    return g_snr_qpsk_const;
}

static unsigned int
fake_output_rate_hz(void) {
    return 48000U;
}

static int
fake_apply_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    g_profile_set_calls++;
    g_profile_cqpsk = cqpsk_enable;
    g_profile_rate = symbol_rate_hz;
    g_profile_levels = levels;
    g_profile_channel = channel_profile;
    g_profile_ted_sps = ted_sps;
    return 0;
}

static void
reset_fake_profile_capture(void) {
    g_profile_set_calls = 0;
    g_profile_cqpsk = -1;
    g_profile_rate = 0;
    g_profile_levels = 0;
    g_profile_channel = 0;
    g_profile_ted_sps = 0;
}

static void
fake_p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_frame_sync_tick_calls++;
}

static void
set_fake_snr(double c4fm, double c4fm_eye, double cqpsk, double qpsk_const) {
    g_snr_c4fm = c4fm;
    g_snr_c4fm_eye = c4fm_eye;
    g_snr_cqpsk = cqpsk;
    g_snr_gfsk = -100.0;
    g_snr_qpsk_const = qpsk_const;
}

static void
install_fake_snr_hooks(void) {
    dsd_rtl_stream_metrics_hooks hooks = {
        .snr_c4fm_db = fake_snr_c4fm_db,
        .snr_c4fm_eye_db = fake_snr_c4fm_eye_db,
        .snr_cqpsk_db = fake_snr_cqpsk_db,
        .snr_gfsk_db = fake_snr_gfsk_db,
        .snr_qpsk_const_db = fake_snr_qpsk_const_db,
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

static void
test_rtl_symbol_profile_selection(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dstar = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 4) == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 2) == DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.frame_dmr = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 3) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    reset(&opts, &state);
    opts.frame_x2tdma = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 3) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.rf_mod = 0;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    reset(&opts, &state);
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 1) == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);
}

static void
test_rtl_p25p2_timing_reconciliation_preserves_cqpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p2 = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_rate == 6000);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_ted_sps == 8);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_unlocked_rtl_p25p2_sync_switches_demod_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p2 = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 6000);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_ted_sps == 8);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_rtl_sps_profiles_apply_and_lock_on_sync(void) {
    static const int expected_rates[] = {4800, 2400, 9600, 6000, 4800};
    static const int expected_levels[] = {4, 4, 2, 4, 2};
    static const int expected_channels[] = {
        DSD_RTL_STREAM_CHANNEL_PROFILE_12K5,     DSD_RTL_STREAM_CHANNEL_PROFILE_6K25,
        DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE, DSD_RTL_STREAM_CHANNEL_PROFILE_12K5,
        DSD_RTL_STREAM_CHANNEL_PROFILE_6K25,
    };
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dmr = 1;
    opts.frame_dpmr = 1;
    opts.frame_provoice = 1;
    opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    state.rf_mod = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    for (int profile_index = 0; profile_index < 5; profile_index++) {
        state.sps_hunt_idx = (profile_index + 1) % 5;
        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        assert(state.sps_hunt_idx == profile_index);
        assert(g_profile_rate == expected_rates[profile_index]);
        assert(g_profile_levels == expected_levels[profile_index]);
        assert(g_profile_channel == expected_channels[profile_index]);
        assert(state.samplesPerSymbol == dsd_opts_compute_sps_rate(&opts, expected_rates[profile_index], 48000));
    }
    assert(g_profile_set_calls == 5);

    const int locked_sps = state.samplesPerSymbol;
    const int locked_center = state.symbolCenter;
    const int calls_before_sync = g_profile_set_calls;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);
    assert(state.sps_hunt_idx == 4);
    assert(state.samplesPerSymbol == locked_sps);
    assert(state.symbolCenter == locked_center);
    assert(g_profile_set_calls == calls_before_sync);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 2);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_dmr_sync_applies_gfsk_rtl_profile(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_BS_DATA_SYNC, 24) == DSD_SYNC_DMR_BS_DATA_POS);
    assert(state.rf_mod == 2);
    assert(g_profile_set_calls == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_profile_cqpsk == 0);
    assert(g_profile_ted_sps == 10);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_active_profile_metrics_power_gate_and_votes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_pwr = 0.25;
    opts.rtl_squelch_level = 0.5;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dstar = 1;
    opts.frame_dmr = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dpmr = 1;
    g_snr_c4fm = 101.0;
    g_snr_cqpsk = 102.0;
    g_snr_gfsk = 103.0;
    install_fake_snr_hooks();

    state.sps_hunt_idx = 0;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 0);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 101.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    state.rf_mod = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 102.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    state.sps_hunt_idx = 4;
    state.rf_mod = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 103.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);

    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 102.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);
    opts.mod_cli_lock = 0;
    opts.mod_qpsk = 0;

    opts.audio_in_type = AUDIO_IN_WAV;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_mixed_profile_snr_recovers_from_gfsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    set_fake_snr(0.0, -100.0, 20.0, -100.0);
    install_fake_snr_hooks();
    dsd_frame_sync_reset_mod_state();
    reset_fake_profile_capture();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 1);
    assert(gfsk_votes == 0);
    assert(g_profile_set_calls == 0);

    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_ted_sps == 10);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    lastt = 24;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    lastt = 24;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_snr_squelch_only_applies_to_rtl_input(void) {
    static const int non_radio_inputs[] = {
        AUDIO_IN_WAV, AUDIO_IN_TCP, AUDIO_IN_UDP, AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT,
    };
    static dsd_opts opts;
    static dsd_state state;

    set_fake_snr(-100.0, -100.0, -100.0, -100.0);
    install_fake_snr_hooks();
    (void)dsd_setenv("DSD_NEO_SNR_SQL_DB", "10", 1);
    dsd_neo_config_init();

    for (size_t i = 0; i < sizeof(non_radio_inputs) / sizeof(non_radio_inputs[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = non_radio_inputs[i];
        opts.frame_dmr = 1;
        opts.mod_cli_lock = 1;
        opts.mod_gfsk = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        state.rf_mod = 2;
        assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);
    }

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_gfsk = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);

    (void)dsd_unsetenv("DSD_NEO_SNR_SQL_DB");
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_nxdn_only_profiles_use_gfsk_snr_gate(void) {
    static dsd_opts opts;
    static dsd_state state;

    install_fake_snr_hooks();
    (void)dsd_setenv("DSD_NEO_SNR_SQL_DB", "10", 1);
    dsd_neo_config_init();

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 0;
    g_snr_c4fm = 0.0;
    g_snr_gfsk = 20.0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    g_snr_c4fm = 20.0;
    g_snr_gfsk = 0.0;
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 0);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    (void)dsd_unsetenv("DSD_NEO_SNR_SQL_DB");
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_modulation_snr_fallback_votes_and_dwell(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.carrier = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    set_fake_snr(-100.0, 4.0, -100.0, 12.0);
    install_fake_snr_hooks();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 0);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 1);
    assert(gfsk_votes == 0);

    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    lastt = 24;
    set_fake_snr(25.0, -100.0, 5.0, -100.0);
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_modulation_cli_lock_prevents_votes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    set_fake_snr(-100.0, -100.0, 20.0, -100.0);
    install_fake_snr_hooks();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 0);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_hamming_override_can_select_qpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_frame_sync_test_set_recent_hamming(10, 1, 24);

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);
}

static void
test_p25_trunk_tick_recency(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.trunk_enable = 1;
    state.p25_p2_active_slot = -1;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    g_frame_sync_tick_calls = 0;
    dsd_frame_sync_test_reset_p25_trunk_tick_state();
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){
        .p25_sm_try_tick = fake_p25_sm_try_tick,
    });

    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)100);
    assert(g_frame_sync_tick_calls == 1);
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)100);
    assert(g_frame_sync_tick_calls == 1);

    state.lastsynctype = DSD_SYNC_NONE;
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)101);
    assert(g_frame_sync_tick_calls == 2);
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)105);
    assert(g_frame_sync_tick_calls == 2);

    state.p25_p2_active_slot = 0;
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)106);
    assert(g_frame_sync_tick_calls == 3);

    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});
}
#endif

int
main(void) {
    test_sps_hunt_skips_disabled_protocol_rates();
    test_sps_hunt_profile_updates_timing();
    test_sps_hunt_reconciles_external_timing();
    test_binary_profiles_override_unlocked_qpsk();
    test_four_level_profiles_reset_inherited_modulation();
    test_nxdn_variant_follows_active_profile();
    test_bounded_symbol_history_readiness_and_wrap();
    test_provoice_candidate_does_not_shadow_dstar_or_nxdn();
    test_symbol_replay_bypasses_sps_profile_gating();
    test_symbol_replay_requires_explicit_nxdn_variant();
    test_manual_p25p2_c4fm_bypasses_profile_gating();
    test_locked_p25p2_c4fm_survives_sync();
    test_m17_auto_preamble_disambiguation_preserves_forced_tolerance();
    test_short_m17_window_estimates_levels_without_warm_start_history();
    test_m17_preamble_requires_context_when_dstar_is_enabled();
    test_elapsed_seconds_prefers_monotonic_then_wall_time();
    test_p25_slot_activity_honors_ring_and_hangtime();
    test_hamming_helpers_find_best_patterns();
#ifdef USE_RADIO
    test_rtl_symbol_profile_selection();
    test_rtl_p25p2_timing_reconciliation_preserves_cqpsk();
    test_unlocked_rtl_p25p2_sync_switches_demod_family();
    test_rtl_sps_profiles_apply_and_lock_on_sync();
    test_dmr_sync_applies_gfsk_rtl_profile();
    test_active_profile_metrics_power_gate_and_votes();
    test_mixed_profile_snr_recovers_from_gfsk();
    test_snr_squelch_only_applies_to_rtl_input();
    test_nxdn_only_profiles_use_gfsk_snr_gate();
    test_modulation_snr_fallback_votes_and_dwell();
    test_modulation_cli_lock_prevents_votes();
    test_hamming_override_can_select_qpsk();
    test_p25_trunk_tick_recency();
#endif
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(misc-use-internal-linkage)
