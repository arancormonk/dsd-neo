// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "analog_rx_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "symbol_test_support.h"
#include "test_support.h"

static int g_cleanup_calls = 0;
/* What the Connect() stub returns: 0, a failed connection, unless a test hands it a socket. */
static dsd_socket_t g_connect_socket = 0;

static int
symbol_level_matches(float got, uint8_t dibit) {
    return fabsf(got - dsd_symbol_level_from_dibit(dibit)) <= 1e-6f;
}

dsd_socket_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return g_connect_socket;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
openAudioInput(dsd_opts* opts) {
    (void)opts;
    return -1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) {
    (void)opts;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_cleanup_calls++;
    exitflag = 1;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

double
// NOLINTNEXTLINE(misc-use-internal-linkage)
pwr_to_dB(double mean_power) {
    (void)mean_power;
    return 0.0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

/* The voice high-pass, stubbed to do to a sub-audible tone what the real 960 Hz one does:
   take it out. It records what reached it first, so a test can prove the received-tone tap
   read the block before this filter and left it untouched. */
static float g_hpf_seen[960];
static int g_hpf_seen_len = 0;
static int g_hpf_removes_tone = 0;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    g_hpf_seen_len = 0;
    if (!input || len <= 0) {
        return;
    }
    const int n = len < (int)(sizeof(g_hpf_seen) / sizeof(g_hpf_seen[0]))
                      ? len
                      : (int)(sizeof(g_hpf_seen) / sizeof(g_hpf_seen[0]));
    DSD_MEMCPY(g_hpf_seen, input, (size_t)n * sizeof(float));
    g_hpf_seen_len = n;
    if (g_hpf_removes_tone) {
        DSD_MEMSET(input, 0, (size_t)len * sizeof(float));
    }
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
pbf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

static void
init_symbol_replay_fixture(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts->input_volume_multiplier = 1;
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    state->rf_mod = 0;
    state->jitter = -1;
}

static void
put_le_i16(unsigned char* out, int16_t value) {
    uint16_t u = (uint16_t)value;
    out[0] = (unsigned char)(u & 0xFFU);
    out[1] = (unsigned char)((u >> 8) & 0xFFU);
}

static void
put_le_u32(unsigned char* out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xFFU);
    out[1] = (unsigned char)((value >> 8) & 0xFFU);
    out[2] = (unsigned char)((value >> 16) & 0xFFU);
    out[3] = (unsigned char)((value >> 24) & 0xFFU);
}

static void
write_soft_header(FILE* file) {
    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
        'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', 2, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE, 0, 0, 0, 0, 0, 0,
    };
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
}

static void
write_soft_record(FILE* file, uint8_t dibit, uint8_t reliability, int16_t llr0, int16_t llr1, float symbol) {
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    uint32_t raw_symbol = 0;
    DSD_MEMSET(record, 0, sizeof(record));
    record[0] = dibit;
    record[1] = reliability;
    put_le_i16(record + 2, llr0);
    put_le_i16(record + 4, llr1);
    DSD_MEMCPY(&raw_symbol, &symbol, sizeof(raw_symbol));
    put_le_u32(record + 6, raw_symbol);
    assert(fwrite(record, 1, sizeof(record), file) == sizeof(record));
}

static void
test_soft_symbol_replay_record(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);

    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    write_soft_header(opts.symbolfile);
    write_soft_record(opts.symbolfile, 3, 77, -1234, 2345, 2.5f);
    rewind(opts.symbolfile);

    float symbol = getSymbol(&opts, &state, 0);
    assert(fabsf(symbol - 2.5f) < 0.0001f);
    assert(state.symbolc == 3);
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_SOFT);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 1);
    assert(state.symbol_replay_soft.reliability == 77);
    assert(state.symbol_replay_soft.llr[0] == -1234);
    assert(state.symbol_replay_soft.llr[1] == 2345);
    assert(fabsf(state.symbol_replay_soft_symbol - 2.5f) < 0.0001f);
    assert(state.symbol_replay_soft_records == 1U);
    assert(state.symbolcnt == 1);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

static void
test_short_file_falls_back_to_legacy_replay(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);

    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    assert(fputc(2, opts.symbolfile) != EOF);
    assert(fputc(1, opts.symbolfile) != EOF);
    rewind(opts.symbolfile);

    assert(symbol_level_matches(getSymbol(&opts, &state, 0), 2U));
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_LEGACY);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 0);
    assert(state.symbolc == 2);

    assert(symbol_level_matches(getSymbol(&opts, &state, 0), 1U));
    assert(state.symbolc == 1);
    assert(state.symbolcnt == 2);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

/*
 * dsd_state::symbolcnt free-runs for the life of the process, so it reaches its type's limit
 * after about 5.2 days at 4800 symbols/s. As a signed int that increment was undefined
 * behaviour and aborted the asan-ubsan build (issue #395); unsigned, it wraps to zero and
 * decoding continues. Every reader differences it modulo 2^32, so the wrap costs nothing.
 *
 * This case is a UBSan tripwire, and which preset runs it decides what a revert looks like.
 * Only `ctest --preset asan-ubsan-debug` sets UBSAN_OPTIONS=halt_on_error=1 (in
 * CMakePresets.json), so only there does the signed increment abort and fail the test; under
 * UBSan's default options the "signed integer overflow" diagnostic prints and the binary
 * still exits 0, which ctest reports as Passed. What catches a revert under dev-debug is the
 * build instead: the assertions below compare the field against uint32_t values, which is a
 * -Werror=sign-compare break once it is signed again. Do not read a dev-debug pass as having
 * exercised the overflow.
 */
static void
test_symbol_count_wraps_instead_of_overflowing(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    g_cleanup_calls = 0;

    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    assert(fputc(2, opts.symbolfile) != EOF);
    assert(fputc(1, opts.symbolfile) != EOF);
    rewind(opts.symbolfile);

    /* The value that used to trap: one past INT32_MAX is where the signed increment was
     * undefined. Unsigned, it is just another symbol. */
    state.symbolcnt = (uint32_t)INT32_MAX;
    assert(symbol_level_matches(getSymbol(&opts, &state, 0), 2U));
    assert(state.symbolcnt == (uint32_t)INT32_MAX + 1U);

    /* And the type's own limit rolls over to zero rather than trapping in turn. */
    state.symbolcnt = UINT32_MAX;
    assert(symbol_level_matches(getSymbol(&opts, &state, 0), 1U));
    assert(state.symbolcnt == 0U);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

static void
test_debug_replay_reopens_and_reprobes(void) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_symbol_replay");
    assert(fd >= 0);
    FILE* file = fdopen(fd, "wb");
    assert(file != NULL);
    write_soft_header(file);
    write_soft_record(file, 0, 31, 100, -100, -3.0f);
    assert(fclose(file) == 0);

    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    state.debug_mode = 1;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", path);
    opts.symbolfile = dsd_fopen_existing_regular_file(path, "rb");
    assert(opts.symbolfile != NULL);

    assert(getSymbol(&opts, &state, 0) == -3.0f);
    assert(state.symbol_replay_soft_records == 1U);
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_SOFT);

    state.symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_LEGACY;
    state.symbol_replay_header_checked = 1;
    state.symbol_replay_has_soft = 1;
    assert(getSymbol(&opts, &state, 0) == -3.0f);
    assert(opts.symbolfile != NULL);
    assert(opts.audio_in_type == AUDIO_IN_SYMBOL_BIN);
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_SOFT);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 1);
    assert(state.symbol_replay_soft_records == 2U);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
    remove(path);
}

static void
test_missing_symbol_file_returns_error_symbol(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "missing-symbol.bin");
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(symbol == -1.0f);
    assert(state.symbolcnt == 1);
    assert(state.symbol_replay_header_checked == 0);
    assert(state.symbol_replay_has_soft == 0);
    assert(g_cleanup_calls == 0);
    assert(exitflag == 0);
}

static void
assert_unsupported_soft_header_is_rejected(uint8_t version, uint8_t record_size) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);

    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
        'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', version, record_size, 0, 0, 0, 0, 0, 0,
    };
    assert(fwrite(header, 1, sizeof(header), opts.symbolfile) == sizeof(header));
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(symbol == 0.0f);
    assert(opts.symbolfile == NULL);
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 0);
    assert(g_cleanup_calls == 1);
    assert(exitflag == 1);
}

static void
test_unsupported_soft_headers_are_rejected(void) {
    assert_unsupported_soft_header_is_rejected(3U, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE);
    assert_unsupported_soft_header_is_rejected(2U, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE + 1U);
}

static void
test_soft_header_without_record_cleans_up_at_eof(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "soft-header-only.bin");
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    write_soft_header(opts.symbolfile);
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(symbol == 0.0f);
    assert(opts.symbolfile == NULL);
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_SOFT);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 0);
    assert(state.symbol_replay_soft_records == 0U);
    assert(g_cleanup_calls == 1);
    assert(exitflag == 1);
}

static void
test_float_symbol_replay_scales_values(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_SYMBOL_FLT;
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    float value = 0.25f;
    assert(fwrite(&value, sizeof(value), 1, opts.symbolfile) == 1);
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(fabsf(symbol - 2500.0f) < 0.0001f);
    assert(state.symbolcnt == 1);
    assert(g_cleanup_calls == 0);
    assert(exitflag == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

static void
test_float_symbol_replay_eof_sets_exitflag(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_SYMBOL_FLT;
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(symbol == 0.0f);
    assert(state.symbolcnt == 1);
    assert(g_cleanup_calls == 0);
    assert(exitflag == 1);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

static void
test_symbol_helper_window_sync_and_timing_contracts(void) {
    int l_edge = 0;
    int r_edge = 0;
    dsd_symbol_test_select_window(0, DSD_SYNC_YSF_POS, DSD_SYNC_NONE, 0, &l_edge, &r_edge);
    assert(l_edge == 1);
    assert(r_edge == 2);

    dsd_symbol_test_select_window(0, DSD_SYNC_NONE, DSD_SYNC_NONE, 0, &l_edge, &r_edge);
    assert(l_edge == 2);
    assert(r_edge == 2);

    dsd_symbol_test_select_window(0, DSD_SYNC_YSF_POS, DSD_SYNC_NONE, 1, &l_edge, &r_edge);
    assert(l_edge == 2);
    assert(r_edge == 2);

    dsd_symbol_test_select_window(1, DSD_SYNC_NONE, DSD_SYNC_NONE, 0, &l_edge, &r_edge);
    assert(l_edge == 1);
    assert(r_edge == 2);

    dsd_symbol_test_select_window(2, DSD_SYNC_NONE, DSD_SYNC_NONE, 0, &l_edge, &r_edge);
    assert(l_edge == 1);
    assert(r_edge == 1);

    int jitter_after = 99;
    assert(dsd_symbol_test_adjust_timing_index(20, 9, 0, 8, 0, 20, 0, &jitter_after) == -1);
    assert(jitter_after == -1);
    assert(dsd_symbol_test_adjust_timing_index(20, 9, 0, 12, 0, 20, 0, &jitter_after) == 1);
    assert(jitter_after == -1);

    assert(dsd_symbol_test_adjust_timing_index(10, 4, 1, 3, 0, 10, 0, &jitter_after) == 1);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 1, 7, 0, 10, 0, &jitter_after) == -1);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 2, 4, 0, 10, 0, &jitter_after) == -1);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 2, 6, 0, 10, 0, &jitter_after) == 1);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 0, 4, 0, 10, 0, &jitter_after) == -1);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 0, 6, 0, 10, 0, &jitter_after) == 1);

    jitter_after = 99;
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 0, 4, 1, 10, 0, &jitter_after) == 0);
    assert(jitter_after == 4);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 0, 4, 0, 1, 0, &jitter_after) == 0);
    assert(jitter_after == 4);
    assert(dsd_symbol_test_adjust_timing_index(10, 4, 0, 4, 0, 10, 1, &jitter_after) == 1);
    assert(jitter_after == 4);
}

static void
test_symbol_helper_analog_i16_conversion_contract(void) {
    const float input[] = {40000.0f, -40000.0f, 123.6f, -123.4f, 0.49f, -0.51f};
    short output[sizeof(input) / sizeof(input[0])] = {0};

    assert(dsd_symbol_test_convert_analog_block_to_i16(NULL, output, 1U) == 0U);
    assert(dsd_symbol_test_convert_analog_block_to_i16(input, NULL, 1U) == 0U);
    assert(dsd_symbol_test_convert_analog_block_to_i16(input, output, (unsigned int)(sizeof(input) / sizeof(input[0])))
           == (unsigned int)(sizeof(input) / sizeof(input[0])));
    assert(output[0] == 32767);
    assert(output[1] == -32768);
    assert(output[2] == 124);
    assert(output[3] == -123);
    assert(output[4] == 0);
    assert(output[5] == -1);
}

static void
test_symbol_matched_filter_uses_active_nxdn_variant(void) {
    static dsd_opts opts;
    static dsd_state state;
    const float sample = 1.0f;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.use_cosine_filter = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    state.lastsynctype = DSD_SYNC_NXDN_POS;
    state.samplesPerSymbol = 10;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;

    init_rrc_filter_memory();
    const float expected_nxdn96 = dmr_filter(sample, state.samplesPerSymbol);
    init_rrc_filter_memory();
    const float actual_nxdn96 = dsd_symbol_test_apply_matched_filter(&opts, &state, sample, 0, 0);
    assert(fabsf(actual_nxdn96 - expected_nxdn96) < 1e-6f);

    state.samplesPerSymbol = 20;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    init_rrc_filter_memory();
    const float expected_nxdn48 = nxdn_filter(sample, state.samplesPerSymbol);
    init_rrc_filter_memory();
    const float actual_nxdn48 = dsd_symbol_test_apply_matched_filter(&opts, &state, sample, 0, 0);
    assert(fabsf(actual_nxdn48 - expected_nxdn48) < 1e-6f);

    opts.frame_dpmr = 1;
    state.lastsynctype = DSD_SYNC_DPMR_FS1_POS;
    init_rrc_filter_memory();
    const float expected_dpmr = dpmr_filter(sample, state.samplesPerSymbol);
    init_rrc_filter_memory();
    const float actual_dpmr = dsd_symbol_test_apply_matched_filter(&opts, &state, sample, 0, 0);
    assert(fabsf(actual_dpmr - expected_dpmr) < 1e-6f);
}

static void
test_symbol_helper_rtl_cache_and_center_contract(void) {
#ifdef USE_RADIO
    int values[10] = {0};
    int run_dir = 7;
    int run_len = 3;
    int dir_out = 99;

    assert(dsd_symbol_test_rtl_cache_and_center_contract(NULL) == 0);
    assert(dsd_symbol_test_rtl_cache_and_center_contract(values) == 10);
    assert(values[0] == 1);
    assert(values[1] == 8);
    assert(values[2] == 1);
    assert(values[3] == 0);
    assert(values[4] == 2);
    assert(values[5] == 4800);
    assert(values[6] == 1);
    assert(values[7] == 125);
    assert(values[8] == 1);
    assert(values[9] == 0);

    assert(dsd_symbol_test_auto_center_step_direction(5, 10, NULL, &run_len, &dir_out) == 0);
    assert(dsd_symbol_test_auto_center_step_direction(5, 10, &run_dir, NULL, &dir_out) == 0);
    assert(dsd_symbol_test_auto_center_step_direction(5, 10, &run_dir, &run_len, NULL) == 0);

    assert(dsd_symbol_test_auto_center_step_direction(5, 10, &run_dir, &run_len, &dir_out) == 0);
    assert(run_dir == 0);
    assert(run_len == 0);
    assert(dir_out == 99);

    assert(dsd_symbol_test_auto_center_step_direction(11, 10, &run_dir, &run_len, &dir_out) == 1);
    assert(run_dir == 1);
    assert(run_len == 1);
    assert(dir_out == 1);

    assert(dsd_symbol_test_auto_center_step_direction(12, 10, &run_dir, &run_len, &dir_out) == 1);
    assert(run_dir == 1);
    assert(run_len == 2);
    assert(dir_out == 1);

    assert(dsd_symbol_test_auto_center_step_direction(-12, 10, &run_dir, &run_len, &dir_out) == 1);
    assert(run_dir == -1);
    assert(run_len == 1);
    assert(dir_out == -1);
#endif
}

/* ---- Received-tone tap (issue #522) ----------------------------------------------------- */

static uint32_t g_fake_rtl_generation = 1U;

static unsigned int
fake_rtl_output_rate_hz(void) {
    return 48000U;
}

static int
fake_rtl_output_kind(void) {
    return RTL_STREAM_OUTPUT_AUDIO_MONITOR;
}

static uint32_t
fake_rtl_stream_generation(void) {
    return g_fake_rtl_generation;
}

/* The analog receive profile the fake stream publishes: this kind (NFM unless a case says otherwise) at this width,
   with the channel filter on or not. */
static int g_fake_rtl_analog_kind = DSD_ANALOG_DEMOD_FM;
static int g_fake_rtl_analog_width_hz = 12500;
static int g_fake_rtl_analog_lpf_on = 1;

static int
fake_rtl_analog_profile(int* out_kind, int* out_width_hz, int* out_lpf_on) {
    if (out_kind) {
        *out_kind = g_fake_rtl_analog_kind;
    }
    if (out_width_hz) {
        *out_width_hz = g_fake_rtl_analog_width_hz;
    }
    if (out_lpf_on) {
        *out_lpf_on = g_fake_rtl_analog_lpf_on;
    }
    return 1;
}

static void
install_fake_rtl_hooks(int installed) {
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    if (installed) {
        hooks.output_rate_hz = fake_rtl_output_rate_hz;
        hooks.output_kind = fake_rtl_output_kind;
        hooks.stream_generation = fake_rtl_stream_generation;
        hooks.analog_profile = fake_rtl_analog_profile;
    }
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

/* The analog FM monitor on 48 kHz PCM input, with the block power above the squelch. */
static void
init_analog_monitor_fixture(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->input_volume_multiplier = 1;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->use_hpf = 1;
    opts->rtl_pwr = 1.0;
    opts->rtl_squelch_level = 0.0;
    opts->audio_gainA = 1.0f;
    g_hpf_removes_tone = 1;
}

static double g_tone_phase = 0.0;
/* The rate the generated tone is sampled at; tests that change the input rate change it too. */
static double g_tone_fs = 48000.0;

static void
fill_tone_block(float* block, unsigned int count, double hz, double amp) {
    for (unsigned int i = 0; i < count; i++) {
        block[i] = (float)(amp * cos(g_tone_phase));
        g_tone_phase += 2.0 * M_PI * hz / g_tone_fs;
        if (g_tone_phase > 2.0 * M_PI) {
            g_tone_phase -= 2.0 * M_PI;
        }
    }
}

/* Bit-for-bit equality of two sample blocks: the tap must hand the voice filters the very
   samples it was given, so no tolerance applies. */
static int
same_sample_bits(const float* a, const float* b, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t a_bits = 0U;
        uint32_t b_bits = 0U;
        DSD_MEMCPY(&a_bits, &a[i], sizeof(a_bits));
        DSD_MEMCPY(&b_bits, &b[i], sizeof(b_bits));
        if (a_bits != b_bits) {
            return 0;
        }
    }
    return 1;
}

/* Feed @p blocks 20 ms blocks of a 100 Hz tone through the whole unsynced finalize step and
   check each one reached the voice filters exactly as it went in. */
static void
feed_tone_blocks(dsd_opts* opts, dsd_state* state, int blocks) {
    float block[960];
    for (int b = 0; b < blocks; b++) {
        fill_tone_block(block, 960U, 100.0, 3000.0);
        assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
        assert(g_hpf_seen_len == 960);
        assert(same_sample_bits(g_hpf_seen, block, 960));
    }
}

static int
rx_tone_locked_on_100(const dsd_state* state) {
    return state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS && state->analog_rx.ctcss_tenths_hz == 1000;
}

static void
test_rx_tone_tap_reads_raw_block_before_voice_filters(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);

    /* 600 ms of a 100 Hz tone. The stubbed voice high-pass removes all of it from everything
       after it in the block's path, so the lock can only have come from the tap reading the
       raw block before that filter -- and feed_tone_blocks() checked every block reached the
       filter exactly as it went in. */
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_rx.carrier_open == 1);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);

    /* Detection off (a digital mode): the same blocks reach the filters byte for byte, so
       the tap's presence changes nothing about the audio either way. */
    opts.analog_only = 0;
    feed_tone_blocks(&opts, &state, 2);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    dsd_state_ext_free_all(&state);
}

/* No tone published: the tap never listened for one (the AM monitor), or forgot what it heard. */
static int
rx_tone_publishes_no_tone(const dsd_state* state) {
    return state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.ctcss_tenths_hz == 0
           && state->analog_rx.dcs_code == 0 && state->analog_rx.dcs_inverted == 0;
}

/* Nothing of a reception published, the carrier included. */
static int
rx_tone_publishes_nothing(const dsd_state* state) {
    return rx_tone_publishes_no_tone(state) && state->analog_rx.carrier_open == 0;
}

/* Put the monitor on @p kind, as the -fM or -fA preset and the front end's published profile do together. */
static void
set_monitor_kind(dsd_opts* opts, int kind) {
    opts->analog_demod = kind;
    g_fake_rtl_analog_kind = kind;
}

/* CTCSS and DCS are FM signalling, so received-tone detection runs on the FM monitor only (issue #524). The same
   CTCSS-bearing monitor blocks that lock on the FM monitor publish no tone on the AM monitor, only its carrier, and
   reach the voice filters unchanged either way (feed_tone_blocks()). A live switch to AM forgets the FM lock at the
   next block, and a switch back finds the tone again from scratch. */
static void
test_rx_tone_tap_is_fm_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    assert(dsd_analog_monitor_tap_active(&opts) && !dsd_analog_tone_detection_active(&opts));
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_publishes_no_tone(&state) && state.analog_rx.carrier_open == 1);

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_rx.carrier_open == 1);

    /* The block that sees the switch (a new published profile) starts a new reception; the next ones keep the AM
       carrier and still no tone. */
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    feed_tone_blocks(&opts, &state, 1);
    assert(rx_tone_publishes_nothing(&state));
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_publishes_no_tone(&state) && state.analog_rx.carrier_open == 1);

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));

    /* A switch the decoder makes before the front end publishes it (the profile still FM) forgets the lock at the
       next read too, and one back to FM finds the tone from scratch rather than keep the lock from before AM. */
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    feed_tone_blocks(&opts, &state, 1);
    assert(rx_tone_publishes_no_tone(&state));
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    feed_tone_blocks(&opts, &state, 1);
    assert(!rx_tone_locked_on_100(&state));
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));

    install_fake_rtl_hooks(0);
    dsd_state_ext_free_all(&state);
}

static void
test_rx_tone_clears_on_unannounced_retune(void) {
    static dsd_opts opts;
    static dsd_state state;
    float block[960];

    /* RTL input: a manual or UDP-driven retune shows up only as a new stream generation. */
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    uint32_t generation = state.analog_rx.generation;
    g_fake_rtl_generation++;
    fill_tone_block(block, 960U, 100.0, 3000.0);
    (void)dsd_symbol_test_finalize_unsynced_analog_block(&opts, &state, block, 960U);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    assert(state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state.analog_rx.ctcss_tenths_hz == 0);
    assert(state.analog_rx.generation != generation);
    /* The new channel's own tone locks again from scratch. */
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));

    /* PCM input under rigctl: no stream generation exists, the trunk-tuning one moves. */
    install_fake_rtl_hooks(0);
    opts.audio_in_type = AUDIO_IN_WAV;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    generation = state.analog_rx.generation;
    dsd_trunk_tuning_generation_advance();
    fill_tone_block(block, 960U, 100.0, 3000.0);
    (void)dsd_symbol_test_finalize_unsynced_analog_block(&opts, &state, block, 960U);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    assert(state.analog_rx.ctcss_tenths_hz == 0);
    assert(state.analog_rx.generation != generation);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));

    /* An announced reset (scan row, target, mode change, stop) clears it at once. */
    generation = state.analog_rx.generation;
    dsd_analog_rx_reset(&state);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(state.analog_rx.carrier_open == 0);
    assert(state.analog_rx.ctcss_tenths_hz == 0);
    assert(state.analog_rx.generation != generation);
    dsd_state_ext_free_all(&state);
}

/* Feed one block of the 100 Hz tone after a change of the published analog profile and check the
   tap dropped it and started a new reception, then that the tone locks again from scratch. */
static void
expect_rx_tone_reset_on_profile_change(dsd_opts* opts, dsd_state* state) {
    float block[960];
    const uint32_t generation = state->analog_rx.generation;
    const uint32_t rtl_generation = g_fake_rtl_generation;
    fill_tone_block(block, 960U, 100.0, 3000.0);
    (void)dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U);
    assert(g_fake_rtl_generation == rtl_generation);
    assert(state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    assert(state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state->analog_rx.ctcss_tenths_hz == 0);
    assert(state->analog_rx.generation != generation);
    feed_tone_blocks(opts, state, 30);
    assert(rx_tone_locked_on_100(state));
}

/* An analog profile the RTL stream applies on a running monitor without a family switch (a
   width-only change, or the channel filter turning on or off at the same width) keeps the
   stream's generation and its output ring and publishes the new profile
   (IO_RTL_ANALOG_FAMILY_SWITCH pins all three), so the tap tells that boundary by the profile
   the stream publishes. */
static void
test_rx_tone_clears_on_applied_analog_profile_change(void) {
    static dsd_opts opts;
    static dsd_state state;

    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    g_fake_rtl_analog_width_hz = 12500;
    g_fake_rtl_analog_lpf_on = 1;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    /* The same profile published block after block is no boundary: the lock and its reception hold. */
    const uint32_t generation = state.analog_rx.generation;
    feed_tone_blocks(&opts, &state, 10);
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_rx.generation == generation);

    /* Width only. */
    g_fake_rtl_analog_width_hz = 8000;
    expect_rx_tone_reset_on_profile_change(&opts, &state);
    /* The channel filter only, at the width already published. */
    g_fake_rtl_analog_lpf_on = 0;
    expect_rx_tone_reset_on_profile_change(&opts, &state);

    g_fake_rtl_analog_width_hz = 12500;
    g_fake_rtl_analog_lpf_on = 1;
    install_fake_rtl_hooks(0);
    dsd_state_ext_free_all(&state);
}

/* Samples of the reset test's WAV: two blocks and all but two samples of a third of a 100 Hz
   tone on the old channel, then a block of seeded noise -- a carrier with no tone -- on the
   new one. The mid-block start test also leaves all but one sample of the third block pending,
   so the next sample completes it. */
enum {
    RESET_WAV_RATE = 2500,
    RESET_WAV_BLOCK = 960,
    RESET_WAV_PENDING = RESET_WAV_BLOCK - 2,
    RESET_WAV_PENDING_MAX = RESET_WAV_BLOCK - 1,
    RESET_WAV_TOTAL_MAX = (3 * RESET_WAV_BLOCK) + RESET_WAV_PENDING_MAX,
};

/* Write @p count samples as a mono 16-bit WAV at @p rate into a new private temp file. */
static int
write_mono_wav(char* path, size_t path_size, const char* prefix, int rate, const short* samples, int count) {
    int fd = dsd_test_mkstemp(path, path_size, prefix);
    if (fd < 0) {
        return -1;
    }
    dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    if (wav == NULL) {
        return -1;
    }
    const int ok = sf_write_short(wav, samples, count) == count;
    sf_close(wav);
    return ok ? 0 : -1;
}

/* A 100 Hz tone at 3000 peak on sample @p n of a @p rate input. */
static short
tone_100_sample(int n, int rate) {
    return (short)lround(3000.0 * cos(2.0 * M_PI * 100.0 * (double)n / (double)rate));
}

/* Seeded noise within +/-2048: a carrier with no tone. */
static short
noise_sample(uint32_t* rng) {
    *rng = (*rng * 1664525U) + 1013904223U;
    return (short)((int)(*rng >> 20) - 2048);
}

/* The reset tests' input: the old channel's 100 Hz tone, then the new channel's noise. */
static short g_reset_wav_samples[RESET_WAV_TOTAL_MAX];

/* Write the reset tests' input with @p pending samples of the old channel's third block, which
   the new channel's block then follows. */
static int
write_reset_wav_pending(char* path, size_t path_size, int pending) {
    assert(pending > 0 && pending <= RESET_WAV_PENDING_MAX);
    const int tone = (2 * RESET_WAV_BLOCK) + pending;
    const int total = tone + RESET_WAV_BLOCK;
    uint32_t rng = 0x2500U;
    for (int n = 0; n < total; n++) {
        g_reset_wav_samples[n] = n < tone ? tone_100_sample(n, RESET_WAV_RATE) : noise_sample(&rng);
    }
    return write_mono_wav(path, path_size, "dsdneo_rx_tone_reset", RESET_WAV_RATE, g_reset_wav_samples, total);
}

static int
write_reset_wav(char* path, size_t path_size) {
    return write_reset_wav_pending(path, path_size, RESET_WAV_PENDING);
}

/* Open a new private temp file for the raw WAV the symbol path writes, mono 16-bit at @p rate. */
static SNDFILE*
open_raw_wav_out(char* path, size_t path_size, int rate) {
    int fd = dsd_test_mkstemp(path, path_size, "dsdneo_rx_tone_raw");
    if (fd < 0) {
        return NULL;
    }
    dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = rate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    return sf_open(path, SFM_WRITE, &info);
}

/* Whether the WAV at @p path holds exactly the first @p count samples of the reset tests' input. */
static int
raw_wav_holds_reset_input(const char* path, int count) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    if (wav == NULL) {
        return 0;
    }
    static short got[RESET_WAV_TOTAL_MAX];
    const sf_count_t n = sf_read_short(wav, got, RESET_WAV_TOTAL_MAX);
    sf_close(wav);
    if (info.frames != (sf_count_t)count || n != (sf_count_t)count) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (got[i] != g_reset_wav_samples[i]) {
            return 0;
        }
    }
    return 1;
}

/* The analog monitor reading @p path, a WAV at @p rate, one getSymbol() per sample. */
static void
open_monitor_wav(dsd_opts* opts, dsd_state* state, const char* path, int rate) {
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(opts, state);
    opts->wav_sample_rate = rate;
    opts->rtl_squelch_level = -100.0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", path);
    opts->audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts->audio_in_file_info));
    assert(opts->audio_in_file_info != NULL);
    opts->audio_in_file = sf_open(path, SFM_READ, opts->audio_in_file_info);
    assert(opts->audio_in_file != NULL);
    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    state->jitter = -1;
    exitflag = 0;
}

static void
close_monitor_wav(dsd_opts* opts, dsd_state* state, const char* path) {
    sf_close(opts->audio_in_file);
    opts->audio_in_file = NULL;
    free(opts->audio_in_file_info);
    opts->audio_in_file_info = NULL;
    (void)remove(path);
    dsd_state_ext_free_all(state);
}

/*
 * A reset sets aside the part of the monitor block the symbol path has already assembled,
 * driven through getSymbol() on a 2500 Hz WAV, where one 960-sample block is 384 ms: long
 * enough to lock a tone on its own. The old channel's 100 Hz tone locks, 958 more of its
 * samples wait in the block, the receiver moves, and the new channel -- a carrier with no tone
 * -- completes that block and fills the next. Read as the new reception's opening audio, those
 * 958 samples would lock 100.0 Hz again from the old channel alone; the tap must not read them.
 * The audio must not lose them either: resets run in digital sessions too, at every acquisition
 * reset and lost TCP connection, and the raw WAV has to hold every sample of the input.
 */
static void
test_rx_tone_reset_sets_the_pending_block_aside(void) {
    char wav_path[DSD_TEST_PATH_MAX];
    char raw_path[DSD_TEST_PATH_MAX];
    assert(write_reset_wav(wav_path, sizeof(wav_path)) == 0);
    static dsd_opts opts;
    static dsd_state state;
    open_monitor_wav(&opts, &state, wav_path, RESET_WAV_RATE);
    opts.wav_out_raw = open_raw_wav_out(raw_path, sizeof(raw_path), RESET_WAV_RATE);
    assert(opts.wav_out_raw != NULL);

    for (int n = 0; n < 2 * RESET_WAV_BLOCK; n++) {
        (void)getSymbol(&opts, &state, 0);
    }
    assert(rx_tone_locked_on_100(&state));
    for (int n = 0; n < RESET_WAV_PENDING; n++) {
        (void)getSymbol(&opts, &state, 0);
    }
    assert(state.analog_sample_counter == RESET_WAV_PENDING);
    assert(rx_tone_locked_on_100(&state));

    dsd_analog_rx_reset(&state);
    assert(state.analog_sample_counter == RESET_WAV_PENDING);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);

    for (int n = 0; n < RESET_WAV_BLOCK; n++) {
        (void)getSymbol(&opts, &state, 0);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
    }
    assert(exitflag == 0 && state.analog_sample_counter == RESET_WAV_PENDING);
    /* The new channel was heard -- a carrier, still being evaluated -- and holds no tone, the
       old one's least of all. */
    assert(state.analog_rx.carrier_open == 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    assert(state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state.analog_rx.ctcss_tenths_hz == 0);

    /* Three whole blocks went out, the one the reset fell in with the old channel's 958
       samples in place. */
    sf_close(opts.wav_out_raw);
    opts.wav_out_raw = NULL;
    assert(raw_wav_holds_reset_input(raw_path, 3 * RESET_WAV_BLOCK));
    (void)remove(raw_path);
    close_monitor_wav(&opts, &state, wav_path);
}

enum { MID_BLOCK_START_AFTER_RESET = 1, MID_BLOCK_START_UNANNOUNCED = 0 };

/*
 * The same block pending when detection starts, in a digital mode where it has not run and so
 * has no session to set the samples aside with: after a reset there (@p reset_first), or with
 * no boundary announced at all. The raw WAV keeps every sample across the reset, as it does at
 * each acquisition reset of a digital session, and the tap starts listening when the analog
 * monitor does, at the sample it starts on, never at the start of a block that began before
 * it. Read from there, the @p pending samples of the old channel waiting in the block would
 * lock 100.0 Hz on the new one. With all but one sample of the block pending, the first sample
 * detection hears completes the block, so the tap is handed the whole block at once.
 */
static void
run_detection_starting_mid_block(int pending, int reset_first) {
    char wav_path[DSD_TEST_PATH_MAX];
    char raw_path[DSD_TEST_PATH_MAX];
    assert(write_reset_wav_pending(wav_path, sizeof(wav_path), pending) == 0);
    static dsd_opts opts;
    static dsd_state state;
    open_monitor_wav(&opts, &state, wav_path, RESET_WAV_RATE);
    opts.wav_out_raw = open_raw_wav_out(raw_path, sizeof(raw_path), RESET_WAV_RATE);
    assert(opts.wav_out_raw != NULL);
    opts.analog_only = 0;

    for (int n = 0; n < (2 * RESET_WAV_BLOCK) + pending; n++) {
        (void)getSymbol(&opts, &state, 0);
    }
    assert(dsd_state_ext_get(&state, DSD_STATE_EXT_DSP_ANALOG_RX) == NULL);
    assert(state.analog_sample_counter == pending);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);

    /* A retune, or none, then the switch to the analog monitor. */
    if (reset_first == MID_BLOCK_START_AFTER_RESET) {
        dsd_analog_rx_reset(&state);
        assert(state.analog_sample_counter == pending);
    }
    opts.analog_only = 1;
    for (int n = 0; n < RESET_WAV_BLOCK; n++) {
        (void)getSymbol(&opts, &state, 0);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
    }
    assert(exitflag == 0 && state.analog_sample_counter == pending);
    assert(state.analog_rx.carrier_open == 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    assert(state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state.analog_rx.ctcss_tenths_hz == 0);

    sf_close(opts.wav_out_raw);
    opts.wav_out_raw = NULL;
    assert(raw_wav_holds_reset_input(raw_path, 3 * RESET_WAV_BLOCK));
    (void)remove(raw_path);
    close_monitor_wav(&opts, &state, wav_path);
}

static void
test_rx_tone_detection_starting_mid_block_skips_older_samples(void) {
    static const int pendings[] = {RESET_WAV_PENDING, RESET_WAV_PENDING_MAX};
    for (size_t i = 0; i < sizeof(pendings) / sizeof(pendings[0]); i++) {
        run_detection_starting_mid_block(pendings[i], MID_BLOCK_START_AFTER_RESET);
        run_detection_starting_mid_block(pendings[i], MID_BLOCK_START_UNANNOUNCED);
    }
}

/* The pacing test's 2500 Hz WAV: digital silence (no carrier), then 800 ms of a 100 Hz tone
   starting half-way through the first 960-sample block, then digital silence again, starting
   part-way through another block. */
enum {
    PACE_WAV_RATE = 2500,
    PACE_LEAD = 480,
    PACE_TONE = 2000,
    PACE_TONE_END = PACE_LEAD + PACE_TONE,
    PACE_TOTAL = PACE_TONE_END + 1500,
};

static int
pace_ms_to_samples(int ms) {
    return (ms * PACE_WAV_RATE) / 1000;
}

/*
 * The publication keeps pace with the input however long the monitor block is. On PCM input
 * the symbol path assembles 960-sample blocks whatever the rate, and at 2500 Hz one is 384 ms:
 * read only at block ends, a tone that starts mid-block and locks inside the next one is shown
 * a block late (576 ms after its start here), and a carrier that drops mid-block is forgotten a
 * block or two late (544 ms here). Driven through getSymbol(), sample by sample: the tone must
 * lock within the 400 ms p95 target of its start, and the carrier drop must be forgotten within
 * the hangover and two reads of the tap -- the read the carrier drops in still counts as
 * carrier, and the hangover runs out part-way through the read that ends it.
 */
static void
test_rx_tone_keeps_pace_with_a_long_pcm_block(void) {
    static short samples[PACE_TOTAL];
    for (int n = 0; n < PACE_TOTAL; n++) {
        samples[n] = (n >= PACE_LEAD && n < PACE_TONE_END) ? tone_100_sample(n - PACE_LEAD, PACE_WAV_RATE) : 0;
    }
    char wav_path[DSD_TEST_PATH_MAX];
    assert(write_mono_wav(wav_path, sizeof(wav_path), "dsdneo_rx_tone_pace", PACE_WAV_RATE, samples, PACE_TOTAL) == 0);
    static dsd_opts opts;
    static dsd_state state;
    open_monitor_wav(&opts, &state, wav_path, PACE_WAV_RATE);

    int locked_at = -1;
    int forgotten_at = -1;
    for (int n = 1; n <= PACE_TOTAL; n++) {
        (void)getSymbol(&opts, &state, 0);
        assert(exitflag == 0);
        assert(state.analog_rx.ctcss_tenths_hz == 0 || state.analog_rx.ctcss_tenths_hz == 1000);
        if (locked_at < 0 && rx_tone_locked_on_100(&state)) {
            locked_at = n;
        }
        if (forgotten_at < 0 && n > PACE_TONE_END && state.analog_rx.carrier_open == 0
            && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE) {
            forgotten_at = n;
        }
    }
    assert(locked_at > PACE_LEAD && locked_at < PACE_TONE_END);
    assert(locked_at - PACE_LEAD <= pace_ms_to_samples(DSD_ANALOG_CTCSS_LOCK_P95_MS));
    assert(forgotten_at > PACE_TONE_END);
    assert(forgotten_at - PACE_TONE_END
           <= pace_ms_to_samples(DSD_ANALOG_CARRIER_HANGOVER_MS + (2 * DSD_ANALOG_RX_TAP_READ_MS)));
    close_monitor_wav(&opts, &state, wav_path);
}

/*
 * An input rate the sub-audible front end cannot use (here a 384 kHz WAV): the tap publishes
 * UNAVAILABLE, so the frontends leave the row out instead of reading "no carrier" over a
 * strong carrier, which the tap still keeps for the scanners (issue #526). A reset reads
 * INACTIVE only until the next block, and the tap does not reset itself block after block --
 * nor once detection is switched off, when it reads INACTIVE once and then stays put.
 */
static void
test_rx_tone_unusable_rate_is_unavailable(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.wav_sample_rate = 384000;
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.ctcss_tenths_hz == 0);
    uint32_t generation = state.analog_rx.generation;
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.generation == generation);

    dsd_analog_rx_reset(&state);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(state.analog_rx.generation != generation);
    generation = state.analog_rx.generation;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    assert(state.analog_rx.generation == generation);

    opts.analog_only = 0;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    generation = state.analog_rx.generation;
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(state.analog_rx.generation == generation);

    /* Back on at a usable rate, detection runs again. */
    opts.analog_only = 1;
    opts.wav_sample_rate = 48000;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    dsd_state_ext_free_all(&state);
}

/* What the log tap has counted: the unusable-rate warning, and the received-tone lines the tap
   logs when its verdict changes. */
static int g_unusable_rate_warnings = 0;
static int g_rx_tone_lines = 0;
static int g_rx_tone_100_lines = 0;
static int g_rx_tone_none_lines = 0;

static void
count_rx_tone_log_lines(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (!text) {
        return;
    }
    if (level == LOG_LEVEL_WARN && strstr(text, "Received tone detection inactive") != NULL) {
        g_unusable_rate_warnings++;
    }
    if (level == LOG_LEVEL_INFO && strncmp(text, "Received tone: ", strlen("Received tone: ")) == 0) {
        g_rx_tone_lines++;
        g_rx_tone_100_lines += strcmp(text, "Received tone: CTCSS 100.0 Hz\n") == 0 ? 1 : 0;
        g_rx_tone_none_lines += strcmp(text, "Received tone: none\n") == 0 ? 1 : 0;
    }
}

/*
 * The "detection inactive" warning is said once for each stretch of input at a rate the front
 * end cannot use. More blocks, and a reset at the same rate (a retune, a scan step, another
 * file at that rate), do not repeat it; a stretch at a usable rate ends it, so going back to
 * the unusable rate -- a 384 kHz WAV, a 48 kHz one, then 384 kHz again -- says it again, as
 * does moving straight to a different unusable rate.
 */
static void
test_rx_tone_unusable_rate_warns_once_per_stretch(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    g_unusable_rate_warnings = 0;

    opts.wav_sample_rate = 384000;
    feed_tone_blocks(&opts, &state, 3);
    assert(g_unusable_rate_warnings == 1);
    feed_tone_blocks(&opts, &state, 3);
    dsd_analog_rx_reset(&state);
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    assert(g_unusable_rate_warnings == 1);

    dsd_analog_rx_reset(&state);
    opts.wav_sample_rate = 48000;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(g_unusable_rate_warnings == 1);

    dsd_analog_rx_reset(&state);
    opts.wav_sample_rate = 384000;
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    assert(g_unusable_rate_warnings == 2);

    opts.wav_sample_rate = 2000;
    feed_tone_blocks(&opts, &state, 3);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    assert(g_unusable_rate_warnings == 3);
    dsd_state_ext_free_all(&state);
}

/*
 * A change between two input rates the front end can use (a 48 kHz WAV reopened at 44.1 kHz):
 * the first read at the new rate drops the lock and moves the generation. Nothing says where in
 * that read the rate changed, so its samples are dropped, not heard, and the carrier is
 * evaluated afresh from the next block on. The tone then locks again at the new rate on its own
 * evidence. Like every other reset, it starts a new reception, so the log reports the tone
 * again even though it is the same one.
 */
static void
test_rx_tone_usable_rate_change_drops_lock(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    g_rx_tone_lines = 0;
    g_rx_tone_100_lines = 0;
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 1 && g_rx_tone_lines == 1);
    const uint32_t generation = state.analog_rx.generation;

    opts.wav_sample_rate = 44100;
    g_tone_fs = 44100.0;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
    assert(state.analog_rx.ctcss_tenths_hz == 0 && state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state.analog_rx.generation != generation);
    const uint32_t dropped_generation = state.analog_rx.generation;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING && state.analog_rx.carrier_open == 1);
    assert(state.analog_rx.ctcss_tenths_hz == 0 && state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state.analog_rx.generation == dropped_generation);
    assert(g_rx_tone_lines == 1);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 2 && g_rx_tone_lines == 2);
    g_tone_fs = 48000.0;
    dsd_state_ext_free_all(&state);
}

/*
 * A drop in input rate part-way through a stream, driven sample by sample through the unsynced
 * analog path: the tap reads at the new rate's pace from its first read there. A 100 Hz tone
 * locks at 48 kHz; the input then moves to 2500 Hz, where one 960-sample monitor block lasts
 * 384 ms, and carries 131.8 Hz. Read at the old rate's quota (960 samples), the first read at
 * the new rate waits for the whole block, and the old reception stays on screen for 384 ms.
 * The reception at the old rate must end within one 20 ms read at the new one, well inside the
 * block, and its tone must not come back.
 */
static void
test_rx_tone_rate_drop_mid_stream_keeps_pace(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_sample_counter == 0);
    const uint32_t generation = state.analog_rx.generation;

    opts.wav_sample_rate = PACE_WAV_RATE;
    int ended_at = -1;
    for (int n = 0; n < RESET_WAV_BLOCK - 1; n++) {
        const double v = 3000.0 * cos(2.0 * M_PI * 131.8 * (double)n / (double)PACE_WAV_RATE);
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, (float)v);
        if (ended_at < 0 && state.analog_rx.generation != generation) {
            ended_at = n + 1;
        }
        if (ended_at >= 0) {
            assert(state.analog_rx.ctcss_tenths_hz != 1000);
        }
    }
    assert(state.analog_sample_counter == RESET_WAV_BLOCK - 1);
    assert(ended_at > 0 && ended_at <= pace_ms_to_samples(DSD_ANALOG_RX_TAP_READ_MS));
    assert(state.analog_rx.carrier_open == 1);
    dsd_state_ext_free_all(&state);
}

/*
 * A drop in input rate while the monitor block is part-full: the samples the tap has not read
 * yet arrived at the old rate, and read at the new one they are another signal. A 1920 Hz tone
 * at 48 kHz fills all but two samples of a block -- a carrier with no sub-audible tone, as the
 * tap hears it -- and the input then moves to 2500 Hz, where those 958 samples, taken as 384 ms
 * of input, are a 100 Hz tone. The first read at the new rate must drop them rather than lock
 * 100.0 Hz from them: it ends the reception at the old rate with no tone, and the reception at
 * the new rate starts with the samples after it, where a real 131.8 Hz tone then locks.
 */
static void
test_rx_tone_rate_change_drops_the_unread_samples(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    for (int n = 0; n < 50 * RESET_WAV_BLOCK + RESET_WAV_PENDING; n++) {
        const double v = 3000.0 * cos(2.0 * M_PI * 1920.0 * (double)n / 48000.0);
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, (float)v);
    }
    assert(state.analog_sample_counter == RESET_WAV_PENDING);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_NONE && state.analog_rx.carrier_open == 1);
    const uint32_t generation = state.analog_rx.generation;

    opts.wav_sample_rate = PACE_WAV_RATE;
    dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, 0.0f);
    assert(state.analog_rx.generation != generation);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
    assert(state.analog_rx.ctcss_tenths_hz == 0 && state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);

    int locked_at = -1;
    for (int n = 0; n < PACE_WAV_RATE; n++) {
        const double v = 3000.0 * cos(2.0 * M_PI * 131.8 * (double)n / (double)PACE_WAV_RATE);
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, (float)v);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (locked_at < 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
            locked_at = n + 1;
        }
    }
    assert(locked_at > 0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED);
    assert(state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_CTCSS && state.analog_rx.ctcss_tenths_hz == 1318);
    dsd_state_ext_free_all(&state);
}

/* Feed @p blocks 20 ms blocks of a tone at @p hz, or of digital silence (a closed squelch) for 0. */
static void
feed_blocks_at(dsd_opts* opts, dsd_state* state, int blocks, double hz) {
    float block[960];
    for (int b = 0; b < blocks; b++) {
        if (hz > 0.0) {
            fill_tone_block(block, 960U, hz, 3000.0);
        } else {
            DSD_MEMSET(block, 0, sizeof(block));
        }
        assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
    }
}

/*
 * The received-tone log line is said when the verdict changes, never block by block: once when
 * a tone locks, however long it is held and through a fade shorter than the hangover; once for
 * "none" when the tone stops under a live carrier, however long the carrier stays; and nothing
 * while a verdict is still being reached or the carrier is down. A new reception -- after the
 * carrier hangover or a reset -- reports its tone afresh, even the same tone.
 */
static void
test_rx_tone_logs_on_change_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    g_rx_tone_lines = 0;
    g_rx_tone_100_lines = 0;
    g_rx_tone_none_lines = 0;

    /* 600 ms to lock, then 2 s more of the same tone, a 100 ms fade and 1 s more: one line. */
    feed_blocks_at(&opts, &state, 30, 100.0);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 1 && g_rx_tone_lines == 1);
    feed_blocks_at(&opts, &state, 100, 100.0);
    feed_blocks_at(&opts, &state, 5, 0.0);
    feed_blocks_at(&opts, &state, 50, 100.0);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 1 && g_rx_tone_lines == 1);

    /* The carrier drops for longer than the hangover, silently; the same tone on the next
       reception is reported again. */
    feed_blocks_at(&opts, &state, 15, 0.0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
    assert(g_rx_tone_lines == 1);
    feed_blocks_at(&opts, &state, 30, 100.0);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 2 && g_rx_tone_lines == 2);

    /* A reset (a retune, a scan step) starts a new reception too. */
    dsd_analog_rx_reset(&state);
    assert(g_rx_tone_lines == 2);
    feed_blocks_at(&opts, &state, 30, 100.0);
    assert(rx_tone_locked_on_100(&state));
    assert(g_rx_tone_100_lines == 3 && g_rx_tone_lines == 3);

    /* The tone gives way to 150.0 Hz, on no table, under the same carrier: 2 s of it say
       "none" once. */
    feed_blocks_at(&opts, &state, 100, 150.0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_NONE && state.analog_rx.carrier_open == 1);
    assert(g_rx_tone_none_lines == 1 && g_rx_tone_100_lines == 3 && g_rx_tone_lines == 4);
    dsd_state_ext_free_all(&state);
}

static uint64_t g_fake_now_ms = 0U;

static uint64_t
fake_now_ms(void) {
    return g_fake_now_ms;
}

/* Blocks as a live stream delivers them: the clock moves on by each block's 20 ms. */
static void
feed_stream_block(dsd_opts* opts, dsd_state* state, double hz) {
    float block[960];
    g_fake_now_ms += 20U;
    fill_tone_block(block, 960U, hz, 3000.0);
    assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
}

/*
 * A live stream whose producer squelches by sending nothing (rtl_fm without -E pad, a UDP
 * sender that stops): no block arrives while it is quiet, so the sample-time hangover never
 * runs. The tap stamps each block with a deadline the frontends age the row against, and the
 * first block after a pause past it starts a new reception -- here another channel's, on
 * 131.8 Hz, which must never be shown as the 100.0 Hz the last one carried. That first block
 * spans the pause (its first samples may have arrived before it), so it is dropped, and the new
 * reception starts with the block after it. File input keeps no deadline: it delivers
 * continuously, and a slow reader is not a quiet channel.
 */
static void
test_rx_tone_paused_stream_starts_a_new_reception(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 100000U;

    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    /* A 20 ms block allows its duration plus the 200 ms hangover, floored at the minimum. */
    assert(state.analog_rx.stale_after_ms == g_fake_now_ms + (uint64_t)DSD_ANALOG_STREAM_PAUSE_MIN_MS);

    /* A gap inside the allowance -- squelch flicker, a scheduling delay -- changes nothing. */
    uint32_t generation = state.analog_rx.generation;
    g_fake_now_ms += (uint64_t)DSD_ANALOG_STREAM_PAUSE_MIN_MS - 40U;
    feed_stream_block(&opts, &state, 100.0);
    assert(rx_tone_locked_on_100(&state) && state.analog_rx.generation == generation);

    /* The producer goes quiet for a second: the publication cannot change while the decoder
       waits, but its deadline passes (the frontends now read it as no carrier) ... */
    const uint64_t deadline = state.analog_rx.stale_after_ms;
    g_fake_now_ms += 1000U;
    assert(g_fake_now_ms > deadline);
    /* ... and the next transmission inherits nothing: the old value is never shown again, the
       first block moves the generation and is dropped, the next is evaluated afresh, and its
       own tone locks. */
    g_tone_phase = 0.3;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 131.8);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (b == 0) {
            assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
            assert(state.analog_rx.generation != generation);
            generation = state.analog_rx.generation;
        } else if (b == 1) {
            assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
            assert(state.analog_rx.generation == generation);
        }
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);

    /* File input never goes stale and never resets on a gap, however long. */
    opts.audio_in_type = AUDIO_IN_WAV;
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.stale_after_ms == 0U);
    generation = state.analog_rx.generation;
    g_fake_now_ms += 5000U;
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.ctcss_tenths_hz == 1318 && state.analog_rx.generation == generation);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* A live stream on the injected clock, one sample at a time: sample n arrives n / rate seconds
   after the first, plus every pause the test inserts. */
static uint64_t g_stream_base_ms = 0U;
static uint64_t g_stream_samples = 0U;

enum { MID_PAUSE_RATE = 8192, MID_PAUSE_PENDING = 958 };

static void
push_stream_sample(dsd_opts* opts, dsd_state* state, short sample) {
    g_stream_samples++;
    g_fake_now_ms = g_stream_base_ms + ((g_stream_samples * 1000U) / (uint64_t)MID_PAUSE_RATE);
    dsd_symbol_test_push_unsynced_analog_sample(opts, state, (float)sample);
}

/*
 * A live stream that pauses part-way through a monitor block. The samples already waiting in
 * the block arrived before the pause; what arrives after it may be another transmission or
 * another channel. Driven sample by sample through the unsynced analog path on 8192 Hz UDP
 * input: a 100 Hz tone locks, 958 more of its samples arrive (all but two of a 960-sample
 * block), the producer goes quiet for a second, and then it sends a carrier with no tone, its
 * noise @p noise_shift bits below +/-2048. Read together with the new reception, those 958
 * samples lock 100.0 Hz again over a quiet carrier; none may reach it.
 */
static void
run_pause_mid_block(int noise_shift) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.wav_sample_rate = MID_PAUSE_RATE;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_stream_base_ms = 300000U;
    g_stream_samples = 0U;

    int n = 0;
    for (; n < 9 * RESET_WAV_BLOCK; n++) {
        push_stream_sample(&opts, &state, tone_100_sample(n, MID_PAUSE_RATE));
    }
    assert(rx_tone_locked_on_100(&state));
    for (int i = 0; i < MID_PAUSE_PENDING; i++, n++) {
        push_stream_sample(&opts, &state, tone_100_sample(n, MID_PAUSE_RATE));
    }
    assert(state.analog_sample_counter == MID_PAUSE_PENDING);
    const uint32_t generation = state.analog_rx.generation;

    /* Until the tap reads again the publication still says what it said before the pause (the
       frontends age it by its deadline meanwhile). The first read after the pause starts the
       new reception, within one read of the stream's return, and from then on the old tone
       must never be shown. */
    g_stream_base_ms += 1000U;
    uint32_t rng = 0x8192U;
    int new_reception_at = -1;
    for (int i = 0; i < (3 * MID_PAUSE_RATE) / 2; i++) {
        push_stream_sample(&opts, &state, (short)(noise_sample(&rng) / (1 << noise_shift)));
        if (new_reception_at < 0 && state.analog_rx.generation != generation) {
            new_reception_at = i;
        }
        if (new_reception_at >= 0) {
            assert(state.analog_rx.ctcss_tenths_hz != 1000);
        }
    }
    assert(new_reception_at >= 0 && new_reception_at < (MID_PAUSE_RATE * DSD_ANALOG_RX_TAP_READ_MS) / 1000);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_NONE);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

static void
test_rx_tone_pause_mid_block_inherits_nothing(void) {
    static const int noise_shifts[] = {0, 3, 6, 10};
    for (size_t i = 0; i < sizeof(noise_shifts) / sizeof(noise_shifts[0]); i++) {
        run_pause_mid_block(noise_shifts[i]);
    }
}

/*
 * A squelch set on PCM input follows each read of the tap, not the block. The symbol path
 * measures each whole block's level into opts->rtl_pwr only once the block is complete, so a
 * read taken while the block fills that went by opts->rtl_pwr would be judged on the previous
 * block. Driven sample by sample at 8192 Hz with the squelch at -40 dBFS: a quiet carrier
 * (about -72 dBFS) never opens it, and a tone at about -24 dBFS that starts half-way through a
 * block opens the carrier within one read of its start rather than at the block's end.
 */
static void
test_rx_tone_pcm_squelch_follows_each_read(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.wav_sample_rate = MID_PAUSE_RATE;
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-40.0);
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_stream_base_ms = 700000U;
    g_stream_samples = 0U;

    uint32_t rng = 0x5E1CU;
    const int quiet = (2 * RESET_WAV_BLOCK) + (RESET_WAV_BLOCK / 2);
    for (int i = 0; i < quiet; i++) {
        push_stream_sample(&opts, &state, (short)(noise_sample(&rng) / 256));
        assert(state.analog_rx.carrier_open == 0);
    }
    assert(state.analog_sample_counter == RESET_WAV_BLOCK / 2);
    assert(opts.rtl_pwr < opts.rtl_squelch_level);

    int opened_at = -1;
    for (int n = 0; n < RESET_WAV_BLOCK && opened_at < 0; n++) {
        push_stream_sample(&opts, &state, tone_100_sample(n, MID_PAUSE_RATE));
        if (state.analog_rx.carrier_open == 1) {
            opened_at = n;
        }
    }
    assert(opened_at >= 0 && opened_at < (MID_PAUSE_RATE * DSD_ANALOG_RX_TAP_READ_MS) / 1000);
    assert(state.analog_sample_counter < RESET_WAV_BLOCK);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/*
 * The symbol path can empty its monitor block part-way through: dsd_symbol_analog_block_reset()
 * drops the part-collected block on a receive-family change, and so does a family switch landing
 * on an RTL front end. The tap's next read must then start at the new block's first sample. Here
 * a reset has just set aside the few samples the old block held, fewer than one read, when the
 * block is dropped; the new block opens with as many samples of a tone at about -24 dBFS and then
 * goes silent. Read from its first sample, the first read holds the tone and opens the -40 dBFS
 * squelch. Read on from where the tap had got to in the dropped block, it would skip the tone
 * and, a read later, find only silence.
 */
static void
test_rx_tone_dropped_block_restarts_the_tap(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.wav_sample_rate = MID_PAUSE_RATE;
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-40.0);
    const int read = ((MID_PAUSE_RATE * DSD_ANALOG_RX_TAP_READ_MS) + 999) / 1000;
    const int held = read / 2;

    for (int n = 0; n < held; n++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, 0.0f);
    }
    assert(state.analog_sample_counter == held);
    dsd_analog_rx_reset(&state);
    dsd_symbol_analog_block_reset(&state);
    assert(state.analog_sample_counter == 0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE && state.analog_rx.carrier_open == 0);

    for (int n = 0; n < held + read; n++) {
        const short v = n < held ? tone_100_sample(n, MID_PAUSE_RATE) : 0;
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, (float)v);
    }
    assert(state.analog_sample_counter == held + read);
    assert(state.analog_rx.carrier_open == 1);
    dsd_state_ext_free_all(&state);
}

/*
 * A live radio stream that stops delivering: an rtl_tcp server that went away, whose client
 * then retries the connection without end while the decoder waits in the read (a stalled
 * device does the same). Nothing arrives for the sample-time hangover to count, so as on a
 * paused PCM stream each block stamps the deadline the frontends age the row against, and the
 * first block after the outage is dropped and starts a new reception. IQ replay is a file: it
 * keeps no deadline and never resets on a gap, so a replay reads the same however slowly it is
 * read.
 */
static void
test_rx_tone_stalled_radio_stream_goes_stale(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtltcp_enabled = 1;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 500000U;
    g_tone_phase = 0.0;

    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_rx.stale_after_ms == g_fake_now_ms + (uint64_t)DSD_ANALOG_STREAM_PAUSE_MIN_MS);

    /* A minute without samples: the publication still names the tone, but its deadline has
       passed, so the frontends read it as no carrier. */
    uint32_t generation = state.analog_rx.generation;
    g_fake_now_ms += 60000U;
    assert(g_fake_now_ms > state.analog_rx.stale_after_ms);

    /* The stream returns on another tone: the old one is never shown again, the first block
       is dropped with the generation moved on, and the new tone locks on its own. */
    g_tone_phase = 0.3;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 131.8);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (b == 0) {
            assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
            assert(state.analog_rx.generation != generation);
        }
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);

    /* IQ replay keeps no deadline and carries on across a gap. */
    opts.rtltcp_enabled = 0;
    opts.iq_replay_active = 1;
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.stale_after_ms == 0U);
    generation = state.analog_rx.generation;
    g_fake_now_ms += 5000U;
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.ctcss_tenths_hz == 1318 && state.analog_rx.generation == generation);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* The reconnect test's TCP audio source: the connection drops once, as the reconnect's backoff
   passes on the injected clock, and the new connection then carries a 131.8 Hz tone in real
   time. */
static int g_tcp_ctx_token = 0;
static int g_tcp_opens = 0;
static int g_tcp_drops_left = 0;
static int g_tcp_sample_n = 0;

static tcp_input_ctx*
fake_tcp_open(dsd_socket_t sockfd, int samplerate) {
    (void)sockfd;
    (void)samplerate;
    g_tcp_opens++;
    return (tcp_input_ctx*)&g_tcp_ctx_token;
}

static void
fake_tcp_close(tcp_input_ctx* ctx) {
    (void)ctx;
}

static int
fake_tcp_read_sample(tcp_input_ctx* ctx, int16_t* out) {
    (void)ctx;
    if (g_tcp_drops_left > 0) {
        g_tcp_drops_left--;
        g_fake_now_ms += 300U;
        return 0;
    }
    *out = (int16_t)lround(3000.0 * cos(2.0 * M_PI * 131.8 * (double)g_tcp_sample_n / 48000.0));
    g_tcp_sample_n++;
    /* A live connection: samples arrive at 48 kHz, one millisecond per 48. */
    if (g_tcp_sample_n % 48 == 0) {
        g_fake_now_ms++;
    }
    return 1;
}

/*
 * A TCP audio connection that drops and reconnects inside the read. The reconnect waits only
 * its backoff (300 ms by default), less than the half second after which a quiet input counts
 * as a dropped carrier, and the new connection may carry another source altogether: the
 * interruption itself starts a new reception. Driven through getSymbol() at 48 kHz: 100.0 Hz
 * locks on the first connection, the next read finds it gone, the reconnect succeeds, and the
 * new connection carries 131.8 Hz. The old tone goes as the connection drops, before the new one
 * delivers a read's worth, never comes back, and the new tone locks on its own.
 */
static void
test_rx_tone_tcp_reconnect_starts_a_new_reception(void) {
    static dsd_opts opts;
    static dsd_state state;
    assert(dsd_socket_init() == 0);
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.tcp_sockfd = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    g_connect_socket = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    assert(opts.tcp_sockfd != DSD_INVALID_SOCKET && g_connect_socket != DSD_INVALID_SOCKET);
    opts.tcp_in_ctx = (tcp_input_ctx*)&g_tcp_ctx_token;
    state.samplesPerSymbol = 1;
    state.symbolCenter = 0;
    state.jitter = -1;
    exitflag = 0;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 900000U;
    g_tone_phase = 0.0;

    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    const uint32_t generation = state.analog_rx.generation;

    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.tcp_open = fake_tcp_open;
    hooks.tcp_close = fake_tcp_close;
    hooks.tcp_read_sample = fake_tcp_read_sample;
    dsd_net_audio_input_hooks_set(hooks);
    g_tcp_opens = 0;
    g_tcp_drops_left = 1;
    g_tcp_sample_n = 0;

    /* One read: the connection is gone, the reconnect succeeds, and the new connection's first
       sample comes back. The received tone went with the old connection. */
    (void)getSymbol(&opts, &state, 0);
    assert(exitflag == 0 && g_tcp_opens == 1);
    assert(opts.audio_in_type == AUDIO_IN_TCP && opts.tcp_sockfd == g_connect_socket);
    assert(state.analog_rx.generation != generation);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE && state.analog_rx.ctcss_tenths_hz == 0);
    for (int n = 1; n < 48000; n++) {
        (void)getSymbol(&opts, &state, 0);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
    }
    assert(exitflag == 0 && g_tcp_opens == 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);

    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    (void)dsd_socket_close(opts.tcp_sockfd);
    opts.tcp_sockfd = 0;
    opts.tcp_in_ctx = NULL;
    g_connect_socket = 0;
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
    dsd_socket_cleanup();
}

/* A block a live input had already queued when the decoder reads it: the decoder drains a
   backlog far faster than real time, so no time passes on the clock. */
static void
feed_queued_block(dsd_opts* opts, dsd_state* state, double hz) {
    float block[960];
    fill_tone_block(block, 960U, hz, 3000.0);
    assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
}

/* Block @p b of a live stream that delivers @p burst_blocks 20 ms blocks at a time, as a
   producer sending large datagrams does: each burst arrives after its own duration, all at once. */
static void
feed_burst_block(dsd_opts* opts, dsd_state* state, double hz, int b, int burst_blocks) {
    if (b % burst_blocks == 0) {
        g_fake_now_ms += 20U * (uint64_t)burst_blocks;
    }
    feed_queued_block(opts, state, hz);
}

enum {
    BACKLOG_BOUNDARY_RESET = 0,
    BACKLOG_BOUNDARY_TUNING = 1,
    BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION = 2,
    BACKLOG_QUEUED_BLOCKS = 25
};

/*
 * The audio a live input had queued when the receiver moved. A rigctl retune holds the decoder
 * while the old channel's audio keeps arriving -- in the UDP ring, the TCP socket, the Pulse
 * record buffer or the stdin pipe -- and the decoder then reads that backlog at CPU speed. The
 * reset sets aside what the monitor block holds, but the backlog comes in after it. At 48 kHz
 * on the injected clock: 100.0 Hz locks at real-time pace, the receiver moves (an announced
 * reset, as a scan step makes, or a trunk-tuning generation move, as a hook-driven rigctl
 * retune leaves), half a second of the old channel's tone arrives queued, and the new channel
 * then carries 131.8 Hz, delivered @p burst_blocks 20 ms blocks at a time. Heard, the backlog
 * locks 100.0 Hz again on the new channel. None of it may be heard -- the row reads no carrier
 * throughout -- the old tone must never come back, and the new one must lock within the p95
 * target of its arrival plus the one read the skip still takes once the backlog is gone. With
 * BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION the old channel plays during a digital session, so
 * detection has no session when the reset comes; the switch to the analog monitor that follows
 * finds the same backlog queued.
 */
static void
run_retune_backlog(int audio_in_type, int boundary, int burst_blocks) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = audio_in_type;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1100000U;
    g_tone_phase = 0.0;

    const int before_detection = boundary == BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION;
    opts.analog_only = before_detection ? 0 : 1;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    if (before_detection) {
        assert(dsd_state_ext_get(&state, DSD_STATE_EXT_DSP_ANALOG_RX) == NULL);
        assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    } else {
        assert(rx_tone_locked_on_100(&state));
    }
    const uint32_t generation = state.analog_rx.generation;

    if (boundary == BACKLOG_BOUNDARY_TUNING) {
        dsd_trunk_tuning_generation_advance();
    } else {
        dsd_analog_rx_reset(&state);
    }
    opts.analog_only = 1;
    for (int b = 0; b < BACKLOG_QUEUED_BLOCKS; b++) {
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.generation != generation);
        assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
        assert(state.analog_rx.ctcss_tenths_hz == 0);
    }

    g_tone_phase = 0.3;
    int locked_at = -1;
    for (int b = 0; b < 40; b++) {
        feed_burst_block(&opts, &state, 131.8, b, burst_blocks);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (locked_at < 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
            locked_at = b;
        }
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);
    assert(locked_at >= 0);
    assert((locked_at + 1) * 20 <= DSD_ANALOG_CTCSS_LOCK_P95_MS + DSD_ANALOG_RX_TAP_READ_MS);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/*
 * Every live PCM input, every kind of boundary, and 20 ms and 100 ms deliveries. A file is not
 * skipped: it queues nothing from another channel, and after a reset it is heard at once however
 * fast it is read. Nor is a live input with no backlog held back for more than two reads: the
 * first read after a boundary may have waited in the input for any length of time, and only the
 * one after it can show that the input ran dry.
 */
static void
test_rx_tone_retune_skips_the_input_backlog(void) {
    static const int inputs[] = {AUDIO_IN_UDP, AUDIO_IN_TCP, AUDIO_IN_PULSE, AUDIO_IN_STDIN};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_RESET, 1);
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_TUNING, 1);
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION, 1);
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_RESET, 5);
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_TUNING, 5);
        run_retune_backlog(inputs[i], BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION, 5);
    }

    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1200000U;
    g_tone_phase = 0.0;

    /* A WAV file read at any speed. */
    for (int b = 0; b < 30; b++) {
        feed_queued_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    dsd_analog_rx_reset(&state);
    feed_queued_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    for (int b = 1; b < 30; b++) {
        feed_queued_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));

    /* UDP with nothing queued: the first read after the reset is skipped, the second shows the
       input keeping real time and is skipped too, and the third is heard. After a generation
       move the first read is the one the move is seen on, dropped as before, and the skip
       costs the second only. */
    opts.audio_in_type = AUDIO_IN_UDP;
    dsd_analog_rx_reset(&state);
    feed_stream_block(&opts, &state, 100.0);
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    const uint32_t generation = state.analog_rx.generation;
    dsd_trunk_tuning_generation_advance();
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.generation != generation);
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/*
 * An input that never runs dry -- stdin fed from a file, read as fast as the decoder goes -- is
 * heard again once DSD_ANALOG_RX_BACKLOG_MAX_MS of it has been skipped after a boundary, and its
 * tone then locks within the p95 target.
 */
static void
test_rx_tone_backlog_skip_is_bounded(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_STDIN;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1300000U;
    g_tone_phase = 0.0;

    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    dsd_analog_rx_reset(&state);

    const int skipped_blocks = DSD_ANALOG_RX_BACKLOG_MAX_MS / 20;
    for (int b = 0; b < skipped_blocks; b++) {
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    }
    int locked_at = -1;
    for (int b = 0; b < 40 && locked_at < 0; b++) {
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.carrier_open == 1);
        if (rx_tone_locked_on_100(&state)) {
            locked_at = b;
        }
    }
    assert(locked_at >= 0 && (locked_at + 1) * 20 <= DSD_ANALOG_CTCSS_LOCK_P95_MS);

    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* A synchronous monitor output that holds the decoder for each block's playing time at 48 kHz,
   as a Pulse playback write does once its buffer is full. */
static void
blocking_playback(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    (void)data;
    const uint64_t samples = (uint64_t)(nsam / sizeof(short));
    g_fake_now_ms += (samples * 1000U) / 48000U;
}

/*
 * Playback that paces the decoder. Monitor audio for stdin input is written synchronously, so
 * once the output buffer is full each block's write holds the decoder for the block's playing
 * time, and a backlog the decoder reads after a retune then goes by at real-time pace, as audio
 * arriving live does. The time spent playing is not time spent waiting for input. At 48 kHz on
 * the injected clock, every block's playback taking its 20 ms: 100.0 Hz locks, the receiver
 * moves, and half a second of the old channel's tone that the input had queued is read without
 * waiting for any of it. Heard, it locks 100.0 Hz again on the new channel. The new channel's
 * 131.8 Hz tone, which the decoder does wait for, then locks within the p95 target plus a read.
 */
static void
test_rx_tone_backlog_skip_ignores_playback_time(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_STDIN;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks playback = {0};
    playback.blast_analog = blocking_playback;
    dsd_udp_audio_hooks_set(playback);
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1400000U;
    g_tone_phase = 0.0;

    const uint64_t started = g_fake_now_ms;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    /* Each block waited 20 ms for its input and was then played for 20 ms. */
    assert(g_fake_now_ms == started + ((uint64_t)30U * 40U));
    const uint32_t generation = state.analog_rx.generation;

    dsd_analog_rx_reset(&state);
    for (int b = 0; b < BACKLOG_QUEUED_BLOCKS; b++) {
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.generation != generation);
        assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE && state.analog_rx.carrier_open == 0);
        assert(state.analog_rx.ctcss_tenths_hz == 0);
    }

    g_tone_phase = 0.3;
    int locked_at = -1;
    for (int b = 0; b < 40; b++) {
        feed_stream_block(&opts, &state, 131.8);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (locked_at < 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
            locked_at = b;
        }
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);
    assert(locked_at >= 0);
    assert((locked_at + 1) * 20 <= DSD_ANALOG_CTCSS_LOCK_P95_MS + DSD_ANALOG_RX_TAP_READ_MS);

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* --- Issue #526: the analog carrier stamp and the monitor gate across a retune --- */

static int g_monitor_blocks;

static void
count_monitor_block(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    (void)nsam;
    (void)data;
    g_monitor_blocks++;
}

static void
clear_carrier_stamps(dsd_state* state) {
    state->last_cc_sync_time = 0;
    state->last_cc_sync_time_m = 0.0;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
}

/* The -Y hold anchors on the analog monitor's carrier whether or not audio is played: -o null or a
 * muted UI (audio_out = 0) still stamps, a closed squelch stops stamping once the tap's hangover is
 * over, a trunking state machine keeps the control-channel anchor to itself, and the -8 source
 * monitor under digital decoding keeps its old rule of stamping only what it plays. */
static void
test_carrier_stamp_does_not_need_audio_out(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_out = 0;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks monitor = {0};
    monitor.blast_analog = count_monitor_block;
    dsd_udp_audio_hooks_set(monitor);
    g_monitor_blocks = 0;
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(state.analog_rx.carrier_open == 1);
    assert(state.last_cc_sync_time != 0 && state.last_cc_sync_time_m > 0.0);
    assert(state.last_vc_sync_time != 0 && state.last_vc_sync_time_m > 0.0);
    assert(g_monitor_blocks == 0);

    /* The squelch closes (the block is -23.8 dBFS): stamps stop once the 200 ms hangover is over. */
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-10.0);
    feed_tone_blocks(&opts, &state, 15);
    assert(state.analog_rx.carrier_open == 0);
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);

    /* Open again under a trunking state machine: only the voice anchor moves off a tuned channel. */
    opts.rtl_squelch_level = 0.0;
    opts.trunk_enable = 1;
    feed_tone_blocks(&opts, &state, 2);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time != 0);
    opts.trunk_enable = 0;

    /* The -8 monitor during digital decoding: no tap runs, and its stamp still follows the audio it plays. */
    opts.analog_only = 0;
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 1);
    assert(state.last_cc_sync_time == 0 && g_monitor_blocks == 0);
    opts.audio_out = 1;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.last_cc_sync_time != 0 && g_monitor_blocks == 1);
    /* Nor while a retune is unresolved, when it plays nothing: the carrier is the channel being left. */
    dsd_trunk_tuning_requests_reset();
    (void)dsd_trunk_tuning_request_begin();
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 1);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0 && g_monitor_blocks == 1);
    dsd_trunk_tuning_requests_reset();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* At an input rate the tap's front end cannot run at (a 384 kHz WAV) the tap publishes UNAVAILABLE for the tone and
 * still keeps the carrier, as at every other rate (the squelch open over the block above the level floor, through the
 * 200 ms hangover): an open squelch stamps with audio_out = 0, a closed one stops stamping once the hangover is over,
 * and a retune the tap has not read past still keeps the old channel's carrier off the new row. */
static void
test_carrier_stamp_at_an_unusable_tap_rate(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.wav_sample_rate = 384000;
    opts.audio_out = 0;
    dsd_trunk_tuning_requests_reset();
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE && state.analog_rx.carrier_open == 1);
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    assert(state.last_cc_sync_time != 0 && state.last_cc_sync_time_m > 0.0);
    assert(state.last_vc_sync_time != 0 && state.last_vc_sync_time_m > 0.0);

    /* A completed retune the tap has not read past yet: the carrier it knows of is the old channel's. The read that
       sees the retune is dropped (at this rate a read is a whole block), and the next one opens the new channel's. */
    dsd_trunk_tuning_generation_advance();
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    feed_tone_blocks(&opts, &state, 1);
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    feed_tone_blocks(&opts, &state, 1);
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 1);

    /* The squelch closes over the -23.8 dBFS block: the carrier holds through the 200 ms hangover (76800 samples, 80
       blocks), then nothing stamps. */
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-10.0);
    feed_tone_blocks(&opts, &state, 79);
    assert(state.analog_rx.carrier_open == 1 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.carrier_open == 0 && dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE);
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);
    dsd_trunk_tuning_requests_reset();
    dsd_state_ext_free_all(&state);
}

/* The monitor writes nothing while a retune is in flight (the front end still delivers the channel
 * being left), nor after one that failed once the scanner had moved on, nor from a block that began
 * before a retune or a reset the tap noticed -- one block at most -- and plays the new channel from
 * the next block on. */
static void
test_monitor_muted_across_a_retune(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks monitor = {0};
    monitor.blast_analog = count_monitor_block;
    dsd_udp_audio_hooks_set(monitor);
    dsd_trunk_tuning_requests_reset();
    g_monitor_blocks = 0;
    feed_tone_blocks(&opts, &state, 3);
    assert(g_monitor_blocks == 3);

    /* A retune in flight, then landed: the block that straddles its completion is dropped too. */
    const uint64_t request = dsd_trunk_tuning_request_begin();
    feed_tone_blocks(&opts, &state, 2);
    assert(g_monitor_blocks == 3);
    dsd_trunk_tuning_request_complete(request, DSD_TRUNK_TUNE_RESULT_OK);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 3);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 4);

    /* A retune nobody announced shows up as a new stream generation: until the tap reads the new channel, the
       carrier it published belongs to the old one, and the block that straddles it is dropped. */
    assert(state.analog_rx.carrier_open == 1 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    assert(dsd_analog_rx_block_straddles_boundary(&opts, &state) == 0);
    g_fake_rtl_generation++;
    assert(state.analog_rx.carrier_open == 1 && dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    /* The block completing now holds the channel before the move, even when the move landed after the tap's last read
       of it (the controller finishing a retune while the voice filters run), so it is dropped all the same. */
    assert(dsd_analog_rx_block_straddles_boundary(&opts, &state) == 1);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 4);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 5 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    /* A completed rigctl-style retune moves the trunk-tuning generation the same way. */
    dsd_trunk_tuning_generation_advance();
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    feed_tone_blocks(&opts, &state, 2);
    assert(g_monitor_blocks == 6 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);

    /* An announced reset (a scan row commit) with part of a block collected: the block is dropped. */
    float block[960];
    fill_tone_block(block, 960U, 100.0, 3000.0);
    for (unsigned int i = 0; i < 500U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    dsd_analog_rx_reset(&state);
    for (unsigned int i = 500U; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(g_monitor_blocks == 6);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 7);

    /* The -8 source monitor under digital decoding runs no detection, so a reset part-way through its block drops
       nothing, as before the tap existed, although the analog monitor above left the tap a session to set the
       collected samples aside in. */
    opts.analog_only = 0;
    assert(!dsd_analog_tone_detection_active(&opts));
    for (unsigned int i = 0; i < 500U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    dsd_analog_rx_reset(&state);
    assert(dsd_analog_rx_block_straddles_boundary(&opts, &state) == 0);
    for (unsigned int i = 500U; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(g_monitor_blocks == 8);
    opts.analog_only = 1;

    /* A retune that failed after the scanner had moved on (its tune timed out, the scanner went on to the row, and the
       front end then reported the failure) leaves the receiver on a channel other than the one the scanner shows:
       nothing plays until the scan's end retires the failure or a later retune lands, as no digital frame does. */
    feed_tone_blocks(&opts, &state, 1);
    const int played = g_monitor_blocks;
    const uint64_t failed = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(failed);
    dsd_trunk_tuning_request_publish(failed, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == failed);
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(g_monitor_blocks == played);
    /* Nor does that channel's carrier hold the row the scanner shows, which would keep the -Y hangtime from ever
       running out and the scanner from making the retune that clears the failure. */
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);
    dsd_trunk_tuning_retire_failed_requests();
    assert(dsd_trunk_tuning_pending_request() == 0U);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == played + 1);
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 1 && state.last_cc_sync_time != 0);
    dsd_trunk_tuning_requests_reset();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    install_fake_rtl_hooks(0);
    dsd_state_ext_free_all(&state);
}

/* Detection that starts part-way through a block -- the first nfm row of a scan that began in a digital mode, on PCM
 * input, where no receive-family switch empties the block -- has no word on the samples collected before it started:
 * they can be the channel before a retune it had no session to set aside. The monitor drops that block rather than play
 * them as the new row's, and plays from the next block on. */
static void
test_monitor_drops_the_block_detection_starts_in(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks monitor = {0};
    monitor.blast_analog = count_monitor_block;
    dsd_udp_audio_hooks_set(monitor);
    dsd_trunk_tuning_requests_reset();
    g_monitor_blocks = 0;

    opts.analog_only = 0;
    float block[960];
    fill_tone_block(block, 960U, 100.0, 3000.0);
    for (unsigned int i = 0; i < 800U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(dsd_state_ext_get(&state, DSD_STATE_EXT_DSP_ANALOG_RX) == NULL);
    dsd_analog_rx_reset(&state);
    opts.analog_only = 1;
    for (unsigned int i = 800U; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(state.analog_sample_counter == 0);
    assert(g_monitor_blocks == 0);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 1);

    /* Detection that starts on the block's first sample has nothing before it to drop. */
    opts.analog_only = 0;
    dsd_state_ext_free_all(&state);
    opts.analog_only = 1;
    for (unsigned int i = 0; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(g_monitor_blocks == 2);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* The AM monitor (issue #524) runs no tone detection, but the tap still keeps its carrier and its boundaries, as on the
 * FM monitor (issue #526): the -Y hold stamps on that carrier with audio_out = 0 (-o null, a muted UI) and stops once
 * the squelch has stayed closed past the hangover, nothing about a tone is logged, and the monitor drops the block a
 * retune lands in. So it does the block an nfm row's retune straddles when the blank row of the -fM session it lands on
 * puts the decoder on AM part-way through that block: the FM row's audio is not played as the AM row's. */
static void
test_am_monitor_keeps_its_carrier_and_boundaries(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    opts.audio_out = 0;
    opts.audio_out_type = 8;
    dsd_udp_audio_hooks monitor = {0};
    monitor.blast_analog = count_monitor_block;
    dsd_udp_audio_hooks_set(monitor);
    dsd_trunk_tuning_requests_reset();
    g_monitor_blocks = 0;
    g_rx_tone_lines = 0;
    g_unusable_rate_warnings = 0;

    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(state.analog_rx.carrier_open == 1 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    assert(rx_tone_publishes_no_tone(&state));
    assert(state.last_cc_sync_time != 0 && state.last_cc_sync_time_m > 0.0);
    assert(state.last_vc_sync_time != 0 && state.last_vc_sync_time_m > 0.0);
    assert(g_monitor_blocks == 0);

    /* The squelch closes (on RTL input the receiver power reads below it): the carrier holds through the 200 ms
       hangover, then stamps stop. */
    opts.rtl_squelch_level = 2.0;
    feed_tone_blocks(&opts, &state, 9);
    assert(state.analog_rx.carrier_open == 1);
    feed_tone_blocks(&opts, &state, 6);
    assert(state.analog_rx.carrier_open == 0 && dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 2);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);
    opts.rtl_squelch_level = 0.0;

    /* Played: a retune nobody announced (a new stream generation) drops the block it lands in, and the carrier it
       knew of is the old channel's until the tap reads the new one. */
    opts.audio_out = 1;
    feed_tone_blocks(&opts, &state, 2);
    assert(g_monitor_blocks == 2);
    g_fake_rtl_generation++;
    assert(dsd_analog_rx_carrier_open_now(&opts, &state) == 0);
    assert(dsd_analog_rx_block_straddles_boundary(&opts, &state) == 1);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 2);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == 3 && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);
    assert(g_rx_tone_lines == 0 && g_unusable_rate_warnings == 0);

    /* An nfm row's FM monitor on air... */
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    feed_tone_blocks(&opts, &state, 2);
    const int played = g_monitor_blocks;
    /* ...retunes to a blank row, whose commit puts the decoder on AM part-way through a block. */
    float block[960];
    fill_tone_block(block, 960U, 100.0, 3000.0);
    for (unsigned int i = 0; i < 500U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    dsd_trunk_tuning_generation_advance();
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    for (unsigned int i = 500U; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(&opts, &state, block[i]);
    }
    assert(g_monitor_blocks == played);
    feed_tone_blocks(&opts, &state, 1);
    assert(g_monitor_blocks == played + 1);
    assert(rx_tone_publishes_no_tone(&state) && dsd_analog_rx_carrier_open_now(&opts, &state) == 1);

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    dsd_trunk_tuning_requests_reset();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    install_fake_rtl_hooks(0);
    dsd_state_ext_free_all(&state);
}

int
main(void) {
    exitflag = 0;
    dsd_neo_log_set_tap(count_rx_tone_log_lines, NULL);
    test_soft_symbol_replay_record();
    test_short_file_falls_back_to_legacy_replay();
    test_symbol_count_wraps_instead_of_overflowing();
    test_debug_replay_reopens_and_reprobes();
    test_missing_symbol_file_returns_error_symbol();
    test_unsupported_soft_headers_are_rejected();
    test_soft_header_without_record_cleans_up_at_eof();
    test_float_symbol_replay_scales_values();
    test_float_symbol_replay_eof_sets_exitflag();
    test_symbol_helper_window_sync_and_timing_contracts();
    test_symbol_helper_analog_i16_conversion_contract();
    test_symbol_matched_filter_uses_active_nxdn_variant();
    test_symbol_helper_rtl_cache_and_center_contract();
    test_rx_tone_tap_reads_raw_block_before_voice_filters();
    test_carrier_stamp_does_not_need_audio_out();
    test_carrier_stamp_at_an_unusable_tap_rate();
    test_monitor_muted_across_a_retune();
    test_monitor_drops_the_block_detection_starts_in();
    test_rx_tone_tap_is_fm_only();
    test_am_monitor_keeps_its_carrier_and_boundaries();
    test_rx_tone_clears_on_unannounced_retune();
    test_rx_tone_clears_on_applied_analog_profile_change();
    test_rx_tone_reset_sets_the_pending_block_aside();
    test_rx_tone_detection_starting_mid_block_skips_older_samples();
    test_rx_tone_keeps_pace_with_a_long_pcm_block();
    test_rx_tone_unusable_rate_is_unavailable();
    test_rx_tone_unusable_rate_warns_once_per_stretch();
    test_rx_tone_usable_rate_change_drops_lock();
    test_rx_tone_rate_drop_mid_stream_keeps_pace();
    test_rx_tone_rate_change_drops_the_unread_samples();
    test_rx_tone_logs_on_change_only();
    test_rx_tone_paused_stream_starts_a_new_reception();
    test_rx_tone_pause_mid_block_inherits_nothing();
    test_rx_tone_pcm_squelch_follows_each_read();
    test_rx_tone_dropped_block_restarts_the_tap();
    test_rx_tone_stalled_radio_stream_goes_stale();
    test_rx_tone_tcp_reconnect_starts_a_new_reception();
    test_rx_tone_retune_skips_the_input_backlog();
    test_rx_tone_backlog_skip_is_bounded();
    test_rx_tone_backlog_skip_ignores_playback_time();
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
