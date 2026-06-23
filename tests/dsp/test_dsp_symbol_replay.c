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
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_cleanup_calls = 0;

static int
symbol_level_matches(float got, uint8_t dibit) {
    return fabsf(got - dsd_symbol_level_from_dibit(dibit)) <= 1e-6f;
}

void dsd_symbol_test_select_window(int rf_mod, int synctype, int lastsynctype, int freeze_window, int* l_edge,
                                   int* r_edge);
int dsd_symbol_test_adjust_timing_index(int samples_per_symbol, int symbol_center, int rf_mod, int jitter,
                                        int have_sync, int symbol_span, int start_i, int* jitter_after);
int dsd_symbol_test_is_m17_sync(int lastsynctype);
unsigned int dsd_symbol_test_convert_analog_block_to_i16(const float* input, short* output, unsigned int count);
#ifdef USE_RADIO
int dsd_symbol_test_rtl_cache_and_center_contract(int out_values[10]);
int dsd_symbol_test_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out);
#endif

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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
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
raw_pwr_f(const float* samples, int len, int step) {
    (void)samples;
    (void)len;
    (void)step;
    return 0.0;
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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hpf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
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
test_invalid_soft_header_replays_as_legacy_bytes(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);

    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {
        'D', 'S', 'D', 'N', 'S', 'Y', 'M', '2', 3, DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE, 0, 0, 0, 0, 0, 0,
    };
    assert(fwrite(header, 1, sizeof(header), opts.symbolfile) == sizeof(header));
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    float symbol = getSymbol(&opts, &state, 0);
    assert(symbol_level_matches(symbol, (uint8_t)('D' & 3)));
    assert(state.symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_LEGACY);
    assert(state.symbol_replay_header_checked == 1);
    assert(state.symbol_replay_has_soft == 0);
    assert(state.symbolc == ('D' & 3));
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
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

    assert(dsd_symbol_test_is_m17_sync(DSD_SYNC_M17_STR_POS) == 1);
    assert(dsd_symbol_test_is_m17_sync(DSD_SYNC_M17_PKT_NEG) == 1);
    assert(dsd_symbol_test_is_m17_sync(DSD_SYNC_M17_BRT_POS) == 0);
    assert(dsd_symbol_test_is_m17_sync(DSD_SYNC_DMR_BS_VOICE_POS) == 0);

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

int
main(void) {
    exitflag = 0;
    test_soft_symbol_replay_record();
    test_short_file_falls_back_to_legacy_replay();
    test_debug_replay_reopens_and_reprobes();
    test_missing_symbol_file_returns_error_symbol();
    test_invalid_soft_header_replays_as_legacy_bytes();
    test_soft_header_without_record_cleans_up_at_eof();
    test_float_symbol_replay_scales_values();
    test_float_symbol_replay_eof_sets_exitflag();
    test_symbol_helper_window_sync_and_timing_contracts();
    test_symbol_helper_analog_i16_conversion_contract();
    test_symbol_helper_rtl_cache_and_center_contract();
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
