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
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static int
symbol_level_matches(float got, uint8_t dibit) {
    return fabsf(got - dsd_symbol_level_from_dibit(dibit)) <= 1e-6f;
}

dsd_socket_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
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

static void
install_fake_rtl_hooks(int installed) {
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    if (installed) {
        hooks.output_rate_hz = fake_rtl_output_rate_hz;
        hooks.output_kind = fake_rtl_output_kind;
        hooks.stream_generation = fake_rtl_stream_generation;
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

/*
 * An input rate the sub-audible front end cannot use (here a 384 kHz WAV): the tap publishes
 * UNAVAILABLE, so the frontends leave the row out instead of reading "no carrier" over a
 * strong carrier. A reset reads INACTIVE only until the next block, and the tap does not reset
 * itself block after block -- nor once detection is switched off, when it reads INACTIVE once
 * and then stays put.
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
    assert(state.analog_rx.carrier_open == 0 && state.analog_rx.ctcss_tenths_hz == 0);
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

static int g_unusable_rate_warnings = 0;

static void
count_unusable_rate_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_WARN && text && strstr(text, "Received tone detection inactive") != NULL) {
        g_unusable_rate_warnings++;
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
 * the redesign drops the lock and moves the generation on the first block at the new rate,
 * and the tone then locks again at that rate on its own evidence.
 */
static void
test_rx_tone_usable_rate_change_drops_lock(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    const uint32_t generation = state.analog_rx.generation;

    opts.wav_sample_rate = 44100;
    g_tone_fs = 44100.0;
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    assert(state.analog_rx.ctcss_tenths_hz == 0 && state.analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE);
    assert(state.analog_rx.generation != generation);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    g_tone_fs = 48000.0;
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
 * 131.8 Hz, which must never be shown as the 100.0 Hz the last one carried. File input keeps
 * no deadline: it delivers continuously, and a slow reader is not a quiet channel.
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
       first block moves the generation and is evaluated afresh, and its own tone locks. */
    g_tone_phase = 0.3;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 131.8);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (b == 0) {
            assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
            assert(state.analog_rx.generation != generation);
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

int
main(void) {
    exitflag = 0;
    dsd_neo_log_set_tap(count_unusable_rate_warnings, NULL);
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
    test_rx_tone_clears_on_unannounced_retune();
    test_rx_tone_unusable_rate_is_unavailable();
    test_rx_tone_unusable_rate_warns_once_per_stretch();
    test_rx_tone_usable_rate_change_drops_lock();
    test_rx_tone_paused_stream_starts_a_new_reception();
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
