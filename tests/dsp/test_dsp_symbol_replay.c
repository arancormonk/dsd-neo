// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/analog_audio.h>
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
#include <dsd-neo/runtime/squelch.h>
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
#include "pcm_tap_synth.h"
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

/* The monitor's audio chain (voice band-pass and gain stage), stubbed to do to a sub-audible tone what the real
   band-pass does: take it out. It records what reached it first, so a test can prove the received-tone tap read the
   block before this chain and left it untouched, and the flags and source each block arrived with. */
static float g_chain_seen[960];
static int g_chain_seen_len = 0;
static int g_chain_removes_tone = 0;
static int g_chain_calls = 0;
static unsigned int g_chain_last_flags = 0U;
/* Every call's length and flags since the last clear, for the auto squelch's runs. */
static size_t g_chain_run_len[64];
static unsigned int g_chain_run_flags[64];
static int g_chain_runs = 0;
static int g_chain_last_source = -1;
static int g_chain_last_rate_hz = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_process_f(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain, float* buf, size_t n,
                           dsd_analog_audio_source source, int rate_hz, unsigned int flags) {
    (void)opts;
    (void)state;
    (void)chain;
    g_chain_calls++;
    g_chain_last_flags = flags;
    if (g_chain_runs < 64) {
        g_chain_run_len[g_chain_runs] = n;
        g_chain_run_flags[g_chain_runs] = flags;
        g_chain_runs++;
    }
    g_chain_last_source = (int)source;
    g_chain_last_rate_hz = rate_hz;
    g_chain_seen_len = 0;
    if (!buf || n == 0U) {
        return 0;
    }
    const size_t cap = sizeof(g_chain_seen) / sizeof(g_chain_seen[0]);
    const size_t keep = n < cap ? n : cap;
    DSD_MEMCPY(g_chain_seen, buf, keep * sizeof(float));
    g_chain_seen_len = (int)keep;
    if (g_chain_removes_tone) {
        DSD_MEMSET(buf, 0, n * sizeof(float));
    }
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_block_begin(const dsd_opts* opts, dsd_state* state, dsd_analog_audio_chain chain) {
    (void)opts;
    (void)state;
    (void)chain;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_analog_audio_note_reception(const dsd_state* state) {
    (void)state;
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

/* A soft record whose symbol is @p raw_symbol, the float's bits. */
static void
write_soft_record_bits(FILE* file, uint8_t dibit, uint8_t reliability, int16_t llr0, int16_t llr1,
                       uint32_t raw_symbol) {
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    DSD_MEMSET(record, 0, sizeof(record));
    record[0] = dibit;
    record[1] = reliability;
    put_le_i16(record + 2, llr0);
    put_le_i16(record + 4, llr1);
    put_le_u32(record + 6, raw_symbol);
    assert(fwrite(record, 1, sizeof(record), file) == sizeof(record));
}

static void
write_soft_record(FILE* file, uint8_t dibit, uint8_t reliability, int16_t llr0, int16_t llr1, float symbol) {
    uint32_t raw_symbol = 0;
    DSD_MEMCPY(&raw_symbol, &symbol, sizeof(raw_symbol));
    write_soft_record_bits(file, dibit, reliability, llr0, llr1, raw_symbol);
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
    FILE* file = dsd_test_fdopen(fd, "wb");
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

/* The bits of the float at @p value. The symbol checks below compare bits: the fast-math build folds isfinite(). */
static uint32_t
stored_float_bits(const float* value) {
    uint32_t bits = 0;
    DSD_MEMCPY(&bits, value, sizeof(bits));
    return bits;
}

/* Stored symbols a file can hold that the reader must not hand on: NaNs, infinities and magnitudes past its bound. */
static const uint32_t kUnusableSymbolBits[] = {
    0x7FC00000u, /* NaN */
    0xFFC00001u, /* a negative NaN with a payload */
    0x7F800000u, /* +Inf */
    0xFF800000u, /* -Inf */
    0x7149F2CAu, /* 1e30 */
    0xF149F2CAu, /* -1e30 */
};

/*
 * A symbol file is external data. A soft record whose stored symbol is Inf, NaN or 2^31 or more in magnitude still
 * decides its dibit, with the record's soft metrics, and its symbol reads as 0, marked unusable: an amplitude with no
 * information must add no confidence of its own, and 0 alone is a confident bit against off-centre thresholds. The symbol used to go on as stored, into the level statistics and the (int)lrintf() of
 * get_dibit_and_analog_signal(), which has no defined result for such a value. The records around it read as written.
 */
static void
test_soft_symbol_replay_bounds_stored_symbols(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);
    write_soft_header(opts.symbolfile);

    const size_t bad_count = sizeof(kUnusableSymbolBits) / sizeof(kUnusableSymbolBits[0]);
    const uint32_t past_bound[] = {0x4F000000u, 0xCF000000u}; /* +-2^31 */
    const uint32_t below_bound = 0x4EFFFFFFu;                 /* the largest float below 2^31 */
    for (size_t i = 0; i < bad_count; i++) {
        write_soft_record_bits(opts.symbolfile, (uint8_t)(i & 3U), (uint8_t)(10U + i), (int16_t)(100 + (int)i),
                               (int16_t)(-100 - (int)i), kUnusableSymbolBits[i]);
        write_soft_record(opts.symbolfile, (uint8_t)((i + 1U) & 3U), 200, 7, -7, 2.5f);
    }
    write_soft_record_bits(opts.symbolfile, 1, 30, 1, -1, past_bound[0]);
    write_soft_record_bits(opts.symbolfile, 2, 31, 2, -2, past_bound[1]);
    write_soft_record_bits(opts.symbolfile, 3, 32, 3, -3, below_bound);
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    for (size_t i = 0; i < bad_count + 1U; i++) {
        const int unusable = i < bad_count;
        for (int pass = 0; pass < (unusable ? 2 : 3); pass++) {
            float symbol = getSymbol(&opts, &state, 0);
            uint8_t dibit = 0;
            uint32_t want = 0;
            if (unusable && pass == 0) {
                dibit = (uint8_t)(i & 3U);
                want = 0x00000000u; /* +0.0 */
                assert(state.symbol_replay_soft.reliability == (uint8_t)(10U + i));
                assert(state.symbol_replay_soft.llr[0] == (int16_t)(100 + (int)i));
                assert(state.symbol_replay_soft.llr[1] == (int16_t)(-100 - (int)i));
            } else if (unusable) {
                dibit = (uint8_t)((i + 1U) & 3U);
                float written = 2.5f;
                want = stored_float_bits(&written);
            } else if (pass < 2) {
                dibit = (uint8_t)(pass + 1);
                want = 0x00000000u; /* +0.0: past the bound */
            } else {
                dibit = 3;
                want = below_bound;
            }
            if (stored_float_bits(&symbol) != want || stored_float_bits(&state.symbol_replay_soft_symbol) != want) {
                DSD_FPRINTF(stderr, "FAIL: soft record %zu/%d read as 0x%08X (stored 0x%08X), not 0x%08X\n", i, pass,
                            (unsigned)stored_float_bits(&symbol),
                            (unsigned)stored_float_bits(&state.symbol_replay_soft_symbol), (unsigned)want);
                assert(0);
            }
            assert(state.symbolc == dibit);
            assert(state.symbol_replay_has_soft == 1);
            const int marked = (unusable && pass == 0) || (!unusable && pass < 2);
            assert(state.symbol_replay_symbol_unusable == marked);
        }
    }
    assert(state.symbol_replay_soft_records == 2U * bad_count + 3U);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
}

/* A float symbol file's unusable symbol (Inf, NaN, or 2^17 or more, which the reader's x10000 would carry past 2^31)
 * reads as 0, in its place, marked unusable; the symbols around it read scaled as written. It used to go on as Inf or
 * NaN. */
static void
test_float_symbol_replay_bounds_stored_symbols(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_symbol_replay_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_SYMBOL_FLT;
    opts.symbolfile = tmpfile();
    assert(opts.symbolfile != NULL);

    const size_t bad_count = sizeof(kUnusableSymbolBits) / sizeof(kUnusableSymbolBits[0]);
    const float quarter = 0.25f;
    uint32_t quarter_bits = stored_float_bits(&quarter);
    for (size_t i = 0; i < bad_count; i++) {
        assert(fwrite(&kUnusableSymbolBits[i], sizeof(uint32_t), 1, opts.symbolfile) == 1);
        assert(fwrite(&quarter_bits, sizeof(uint32_t), 1, opts.symbolfile) == 1);
    }
    const uint32_t edges[] = {0x48000000u, 0xC8000000u, 0x47FFFFFFu}; /* +-2^17, the largest float below 2^17 */
    assert(fwrite(edges, sizeof(edges[0]), sizeof(edges) / sizeof(edges[0]), opts.symbolfile)
           == sizeof(edges) / sizeof(edges[0]));
    rewind(opts.symbolfile);
    g_cleanup_calls = 0;
    exitflag = 0;

    const float scaled_quarter = 0.25f * 10000.0f;
    const float edge_scaled = 131071.9921875f * 10000.0f;
    for (size_t i = 0; i < 2U * bad_count + 3U; i++) {
        float symbol = getSymbol(&opts, &state, 0);
        uint32_t want = 0U; /* every unusable symbol reads as +0 */
        int marked = 1;
        if (i < 2U * bad_count && (i & 1U)) {
            want = stored_float_bits(&scaled_quarter);
            marked = 0;
        } else if (i == 2U * bad_count + 2U) {
            want = stored_float_bits(&edge_scaled);
            marked = 0;
        }
        if (stored_float_bits(&symbol) != want || state.symbol_replay_symbol_unusable != marked) {
            DSD_FPRINTF(stderr, "FAIL: float symbol %zu read as 0x%08X (unusable %d), not 0x%08X (unusable %d)\n", i,
                        (unsigned)stored_float_bits(&symbol), state.symbol_replay_symbol_unusable, (unsigned)want,
                        marked);
            assert(0);
        }
    }
    assert(state.symbolcnt == 2U * bad_count + 3U);
    assert(g_cleanup_calls == 0);

    fclose(opts.symbolfile);
    opts.symbolfile = NULL;
    exitflag = 0;
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
    g_chain_removes_tone = 1;
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
        assert(g_chain_seen_len == 960);
        assert(same_sample_bits(g_chain_seen, block, 960));
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
static int g_rx_tone_d023n_lines = 0;
static int g_rx_tone_d047n_lines = 0;
/* And the tone policy's verdict lines (issue #527). */
static int g_tone_filter_lines = 0;
static int g_tone_filter_allowed_100_lines = 0;
static int g_tone_filter_rejected_no_tone_lines = 0;
static int g_tone_filter_rejected_100_lines = 0;
static int g_tone_filter_allowed_no_tone_lines = 0;
static int g_tone_filter_pending_lost_lines = 0;

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
        g_rx_tone_d023n_lines += strcmp(text, "Received tone: DCS D023N / D047I\n") == 0 ? 1 : 0;
        g_rx_tone_d047n_lines += strcmp(text, "Received tone: DCS D047N / D023I\n") == 0 ? 1 : 0;
    }
    if (level == LOG_LEVEL_INFO && strncmp(text, "Tone filter: ", strlen("Tone filter: ")) == 0) {
        g_tone_filter_lines++;
        g_tone_filter_allowed_100_lines += strcmp(text, "Tone filter: allowed (CTCSS 100.0 Hz)\n") == 0 ? 1 : 0;
        g_tone_filter_rejected_no_tone_lines += strcmp(text, "Tone filter: rejected (no tone)\n") == 0 ? 1 : 0;
        g_tone_filter_rejected_100_lines += strcmp(text, "Tone filter: rejected (CTCSS 100.0 Hz)\n") == 0 ? 1 : 0;
        g_tone_filter_allowed_no_tone_lines += strcmp(text, "Tone filter: allowed (no tone)\n") == 0 ? 1 : 0;
        g_tone_filter_pending_lost_lines += strcmp(text, "Tone filter: pending (tone lost)\n") == 0 ? 1 : 0;
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

    /* The tone gives way to 161.0 Hz, on no table, under the same carrier: 2 s of it say
       "none" once. */
    feed_blocks_at(&opts, &state, 100, 161.0);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_NONE && state.analog_rx.carrier_open == 1);
    assert(g_rx_tone_none_lines == 1 && g_rx_tone_100_lines == 3 && g_rx_tone_lines == 4);
    dsd_state_ext_free_all(&state);
}

/* DCS signalling, as a transmitter sends it: one word repeated at 134.4 bit/s, bit 0 first, a
   one as positive deviation (the inverted polarity sends the complement). */
static double g_dcs_bit_phase = 0.0;

static void
feed_dcs_blocks(dsd_opts* opts, dsd_state* state, int blocks, uint32_t word) {
    float block[960];
    for (int b = 0; b < blocks; b++) {
        for (unsigned int i = 0; i < 960U; i++) {
            const int bit = (int)g_dcs_bit_phase;
            block[i] = ((word >> bit) & 1U) ? 3000.0f : -3000.0f;
            g_dcs_bit_phase += 134.4 / 48000.0;
            if (g_dcs_bit_phase >= 23.0) {
                g_dcs_bit_phase -= 23.0;
            }
        }
        assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
    }
}

static int
rx_code_locked(const dsd_state* state, int code) {
    return state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_DCS && state->analog_rx.dcs_code == code
           && state->analog_rx.dcs_inverted == 0 && state->analog_rx.ctcss_tenths_hz == 0;
}

static int
rx_code_cleared(const dsd_state* state) {
    return state->analog_rx.tone_state != DSD_ANALOG_TONE_STATE_LOCKED
           && state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.dcs_code == 0
           && state->analog_rx.dcs_inverted == 0;
}

/*
 * After a boundary the detector reads the code from scratch: twice in a row, 46 bits (342 ms),
 * before it can lock. Feeds 15 blocks of 20 ms of @p word, fewer than that, and checks after
 * each that nothing is locked and nothing was logged, as a detector that kept its lock across
 * the boundary would at once.
 */
static void
feed_dcs_blocks_before_a_fresh_lock(dsd_opts* opts, dsd_state* state, uint32_t word) {
    const int lines = g_rx_tone_lines;
    for (int b = 0; b < 15; b++) {
        feed_dcs_blocks(opts, state, 1, word);
        assert(rx_code_cleared(state));
        assert(g_rx_tone_lines == lines);
    }
}

/*
 * DCS through the real tap (issue #523): D023N locks, is published as code 023 in normal
 * polarity and logged once as "Received tone: DCS D023N / D047I", both spellings of its signal;
 * the inverted word is published as its normal alias, D047N, and logged as "DCS D047N / D023I".
 * Every boundary that clears a tone clears a code the same way: a retune the RTL stream or the
 * tuning hooks report, and an announced reset. The detector keeps nothing of the lock: the same
 * code running on through the boundary is not shown, nor logged, until it has been read twice
 * from scratch, then logged once more for the new reception; after the announced reset the
 * inverted word locks as D047N and D023N never shows again.
 */
static void
test_rx_tone_dcs_through_the_tap(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint32_t d023n = dsd_dcs_word(0023, 0);
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    g_rx_tone_lines = 0;
    g_rx_tone_d023n_lines = 0;
    g_rx_tone_d047n_lines = 0;
    g_dcs_bit_phase = 0.0;

    /* 1.2 s to lock, then 2 s more: one line. */
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);
    feed_dcs_blocks(&opts, &state, 100, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(g_rx_tone_d023n_lines == 1 && g_rx_tone_lines == 1);

    /* PCM under rigctl: the trunk-tuning generation moves, and the code is gone at once. */
    uint32_t generation = state.analog_rx.generation;
    dsd_trunk_tuning_generation_advance();
    feed_dcs_blocks(&opts, &state, 1, d023n);
    assert(rx_code_cleared(&state) && state.analog_rx.generation != generation);
    feed_dcs_blocks_before_a_fresh_lock(&opts, &state, d023n);
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(g_rx_tone_d023n_lines == 2 && g_rx_tone_lines == 2);

    /* RTL: a manual or UDP-driven retune is a new stream generation. */
    install_fake_rtl_hooks(1);
    opts.audio_in_type = AUDIO_IN_RTL;
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    generation = state.analog_rx.generation;
    const int lines_before_retune = g_rx_tone_d023n_lines;
    g_fake_rtl_generation++;
    feed_dcs_blocks(&opts, &state, 1, d023n);
    assert(rx_code_cleared(&state) && state.analog_rx.generation != generation);
    feed_dcs_blocks_before_a_fresh_lock(&opts, &state, d023n);
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(g_rx_tone_d023n_lines == lines_before_retune + 1);
    install_fake_rtl_hooks(0);
    opts.audio_in_type = AUDIO_IN_WAV;

    /* An announced reset (scan row, target, mode change, stop). */
    generation = state.analog_rx.generation;
    dsd_analog_rx_reset(&state);
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
    assert(rx_code_cleared(&state) && state.analog_rx.carrier_open == 0);
    assert(state.analog_rx.generation != generation);

    /* The inverted word is D047N's signal, and D023N is never shown or logged again. */
    const int d023n_lines = g_rx_tone_d023n_lines;
    feed_dcs_blocks_before_a_fresh_lock(&opts, &state, dsd_dcs_word(0023, 1));
    for (int b = 0; b < 60; b++) {
        feed_dcs_blocks(&opts, &state, 1, dsd_dcs_word(0023, 1));
        assert(!rx_code_locked(&state, 0023));
    }
    assert(rx_code_locked(&state, 0047));
    assert(g_rx_tone_d047n_lines == 1 && g_rx_tone_d023n_lines == d023n_lines);
    dsd_state_ext_free_all(&state);
}

/*
 * DCS is FM signalling, like CTCSS, so the DCS detector runs on the FM monitor only (issues #523, #524). The same D023N
 * monitor audio that locks on the FM monitor publishes no code on the AM monitor, only its carrier, and logs nothing.
 * A live switch to AM forgets the FM lock at the next block without a "Received tone:" line, and the switch back reads
 * the code from scratch, twice in a row, before it shows and logs it again.
 */
static void
test_rx_tone_dcs_is_fm_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    const uint32_t d023n = dsd_dcs_word(0023, 0);
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    g_rx_tone_lines = 0;
    g_rx_tone_d023n_lines = 0;
    g_dcs_bit_phase = 0.0;

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_tone_publishes_no_tone(&state) && state.analog_rx.carrier_open == 1);
    assert(g_rx_tone_lines == 0);

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(g_rx_tone_d023n_lines == 1 && g_rx_tone_lines == 1);

    /* The block that sees the switch (a new published profile) starts a new reception; the next ones keep the AM
       carrier and still no code. */
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    feed_dcs_blocks(&opts, &state, 1, d023n);
    assert(rx_tone_publishes_nothing(&state));
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_tone_publishes_no_tone(&state) && state.analog_rx.carrier_open == 1);
    assert(g_rx_tone_lines == 1);

    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_FM);
    feed_dcs_blocks_before_a_fresh_lock(&opts, &state, d023n);
    feed_dcs_blocks(&opts, &state, 60, d023n);
    assert(rx_code_locked(&state, 0023));
    assert(g_rx_tone_d023n_lines == 2 && g_rx_tone_lines == 2);

    install_fake_rtl_hooks(0);
    dsd_state_ext_free_all(&state);
}

static uint64_t g_fake_now_ms = 0U;

static uint64_t
fake_now_ms(void) {
    return g_fake_now_ms;
}

/* The decoder waiting @p ms in the input read for audio still to arrive (dsd_analog_rx_input_wait_begin()). */
static void
wait_for_input(const dsd_state* state, uint64_t ms) {
    dsd_analog_rx_input_wait_begin(state);
    g_fake_now_ms += ms;
    dsd_analog_rx_input_wait_end(state);
}

/* Blocks as a live stream delivers them: the decoder waits in the input read for each block's 20 ms. */
static void
feed_stream_block(dsd_opts* opts, dsd_state* state, double hz) {
    float block[960];
    wait_for_input(state, 20U);
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
        wait_for_input(state, 20U * (uint64_t)burst_blocks);
    }
    feed_queued_block(opts, state, hz);
}

/* A raw WAV (-6) whose sync to disk holds the decoder for g_raw_wav_sync_stall_ms after each block, as fsync on slow
   storage does (issue #576): decoder time outside the input read. The stall stands in for the sync, so no test waits
   on the disk. */
static uint64_t g_raw_wav_sync_stall_ms = 0U;
static char g_raw_wav_stall_path[DSD_TEST_PATH_MAX];

static void
stall_raw_wav_sync(void) {
    g_fake_now_ms += g_raw_wav_sync_stall_ms;
}

static void
raw_wav_stall_begin(dsd_opts* opts, uint64_t stall_ms) {
    opts->wav_out_raw = open_raw_wav_out(g_raw_wav_stall_path, sizeof(g_raw_wav_stall_path), 48000);
    assert(opts->wav_out_raw != NULL);
    g_raw_wav_sync_stall_ms = stall_ms;
    dsd_symbol_test_set_raw_wav_sync(stall_raw_wav_sync);
}

static void
raw_wav_stall_end(dsd_opts* opts) {
    dsd_symbol_test_set_raw_wav_sync(NULL);
    g_raw_wav_sync_stall_ms = 0U;
    sf_close(opts->wav_out_raw);
    opts->wav_out_raw = NULL;
    assert(remove(g_raw_wav_stall_path) == 0);
}

/* A live block on a decoder the raw WAV sync holds for part of every block: the audio that arrived during the sync is
   queued by the next read, which waits only @p wait_ms for the rest of the block. */
static void
feed_live_block_waiting(dsd_opts* opts, dsd_state* state, double hz, uint64_t wait_ms) {
    wait_for_input(state, wait_ms);
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
 * finds the same backlog queued. With @p sync_stall_ms the raw WAV is on and its sync holds the
 * decoder that long after every block (issue #576): the backlog then takes that long per block
 * to read, but none of it is spent waiting for the input, and it must stay skipped all the same.
 */
static void
run_retune_backlog(int audio_in_type, int boundary, int burst_blocks, uint64_t sync_stall_ms) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = audio_in_type;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1100000U;
    g_tone_phase = 0.0;
    if (sync_stall_ms > 0U) {
        raw_wav_stall_begin(&opts, sync_stall_ms);
    }

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

    if (sync_stall_ms > 0U) {
        raw_wav_stall_end(&opts);
    }
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/*
 * Every live PCM input, every kind of boundary, and 20 ms and 100 ms deliveries. A file is not
 * skipped: it queues nothing from another channel, and after a reset it is heard at once however
 * fast it is read. Nor is a live input with no backlog held back for more than two reads: the
 * first read after a boundary may have waited in the input for any length of time, and only the
 * one after it can show that the input ran dry. A raw WAV sync that holds the decoder 12 or 15 ms
 * per 20 ms block (issue #576) is not waiting for the input, so the backlog stays skipped.
 */
static void
test_rx_tone_retune_skips_the_input_backlog(void) {
    static const int inputs[] = {AUDIO_IN_UDP, AUDIO_IN_TCP, AUDIO_IN_PULSE, AUDIO_IN_STDIN};
    static const int boundaries[] = {BACKLOG_BOUNDARY_RESET, BACKLOG_BOUNDARY_TUNING,
                                     BACKLOG_BOUNDARY_RESET_BEFORE_DETECTION};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        for (size_t k = 0; k < sizeof(boundaries) / sizeof(boundaries[0]); k++) {
            run_retune_backlog(inputs[i], boundaries[k], 1, 0U);
            run_retune_backlog(inputs[i], boundaries[k], 5, 0U);
            run_retune_backlog(inputs[i], boundaries[k], 1, 12U);
            run_retune_backlog(inputs[i], boundaries[k], 1, 15U);
        }
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

/*
 * A retune on a decoder a slow raw WAV sync holds for @p sync_stall_ms of every 20 ms block (issue #576), on UDP
 * input. 100.0 Hz locks on audio read as it arrives, the receiver moves, and the half second of the old channel the
 * input had queued comes in at the decoder's own pace, each block held up only by its sync: none of it is heard. The
 * new channel's 131.8 Hz then arrives live, and the audio that arrives during a sync is queued by the next read, which
 * waits only for the rest of the block: the stalls that must not count as waiting also leave less to wait for. The
 * skip holds back the first @p skipped_live_reads of the new channel's reads, the next one is heard, and 100.0 Hz is
 * never shown after the retune.
 */
static void
run_stalled_retune(uint64_t sync_stall_ms, int skipped_live_reads) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1500000U;
    g_tone_phase = 0.0;
    raw_wav_stall_begin(&opts, sync_stall_ms);
    const uint64_t wait_ms = 20U - sync_stall_ms;

    for (int b = 0; b < 30; b++) {
        feed_live_block_waiting(&opts, &state, 100.0, wait_ms);
    }
    assert(rx_tone_locked_on_100(&state));
    dsd_analog_rx_reset(&state);
    for (int b = 0; b < BACKLOG_QUEUED_BLOCKS; b++) {
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
        assert(state.analog_rx.ctcss_tenths_hz == 0);
    }
    g_tone_phase = 0.3;
    for (int b = 0; b < skipped_live_reads; b++) {
        feed_live_block_waiting(&opts, &state, 131.8, wait_ms);
        assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    }
    feed_live_block_waiting(&opts, &state, 131.8, wait_ms);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    for (int b = 0; b < 40; b++) {
        feed_live_block_waiting(&opts, &state, 131.8, wait_ms);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);

    raw_wav_stall_end(&opts);
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/*
 * The input ran dry once the decoder waited in the read for an eighth of a 20 ms read. Waiting 5 ms of each (a 15 ms
 * sync) or 3 ms (a 17 ms sync) still shows it, so the new channel is heard from its second read, as on a decoder
 * nothing holds; waiting 2 ms of each (an 18 ms sync) is not told from a drain, and the new channel is heard only
 * once DSD_ANALOG_RX_BACKLOG_MAX_MS has been skipped since the retune.
 */
static void
test_rx_tone_backlog_skip_hears_a_stalled_live_input(void) {
    run_stalled_retune(15U, 1);
    run_stalled_retune(17U, 1);
    run_stalled_retune(18U, (DSD_ANALOG_RX_BACKLOG_MAX_MS / 20) - BACKLOG_QUEUED_BLOCKS);
}

/*
 * A backlog read on a busy machine: the decoder is descheduled for @p preempt_ms in the input read of every block while
 * the input still holds the old channel, so each read takes that long though nothing made it wait. Time inside the
 * read alone would count it as waiting; the span still took less than half its input's length to arrive, so the input
 * has not run dry. With @p playback the monitor output also holds the decoder for each block's 20 ms, as synchronous
 * playback does once its buffer is full: the span then takes longer than its input's length, and only leaving the
 * time spent playing out keeps it short of half. 100.0 Hz locks, the receiver moves, and none of the half second of
 * the old channel is heard; the new channel, read as it arrives, is heard from its second read and never shows
 * 100.0 Hz.
 */
static void
run_preempted_backlog(int audio_in_type, uint64_t preempt_ms, int playback) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = audio_in_type;
    if (playback) {
        opts.audio_out = 1;
        opts.audio_out_type = 8;
        dsd_udp_audio_hooks hooks = {0};
        hooks.blast_analog = blocking_playback;
        dsd_udp_audio_hooks_set(hooks);
    }
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1700000U;
    g_tone_phase = 0.0;

    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(rx_tone_locked_on_100(&state));
    dsd_analog_rx_reset(&state);
    for (int b = 0; b < BACKLOG_QUEUED_BLOCKS; b++) {
        wait_for_input(&state, preempt_ms);
        feed_queued_block(&opts, &state, 100.0);
        assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
        assert(state.analog_rx.ctcss_tenths_hz == 0);
    }
    g_tone_phase = 0.3;
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.carrier_open == 0 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_IDLE);
    feed_stream_block(&opts, &state, 131.8);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_ACQUIRING);
    for (int b = 0; b < 40; b++) {
        feed_stream_block(&opts, &state, 131.8);
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
    }
    assert(state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED && state.analog_rx.ctcss_tenths_hz == 1318);

    if (playback) {
        dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    }
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* Every live PCM input, descheduled 3 ms in each backlog read (past the eighth of a read) and 9 ms (just short of
   half of one); and stdin, whose monitor playback is synchronous, with each block's playing time on top. */
static void
test_rx_tone_backlog_skip_survives_preempted_reads(void) {
    static const int inputs[] = {AUDIO_IN_UDP, AUDIO_IN_TCP, AUDIO_IN_PULSE, AUDIO_IN_STDIN};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        run_preempted_backlog(inputs[i], 3U, 0);
        run_preempted_backlog(inputs[i], 9U, 0);
    }
    run_preempted_backlog(AUDIO_IN_STDIN, 3U, 1);
    run_preempted_backlog(AUDIO_IN_STDIN, 9U, 1);
}

/* A UDP producer sending in real time at 48 kHz: sample n arrives at g_udp_base_ms + n / 48 ms. A read returns at once
   what has arrived and otherwise waits for it on the injected clock. The first g_udp_old_samples are the old channel's
   100.0 Hz tone, the rest the new channel's 131.8 Hz. */
static uint64_t g_udp_base_ms = 0U;
static uint64_t g_udp_sample_n = 0U;
static uint64_t g_udp_old_samples = UINT64_MAX;
/* Samples after g_udp_wait_after are watched for a wait: g_udp_first_wait_n is the first one a read had to wait for,
   0 until then. */
static uint64_t g_udp_wait_after = UINT64_MAX;
static uint64_t g_udp_first_wait_n = 0U;
static double g_udp_phase = 0.0;

static int
fake_udp_read_sample(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const uint64_t arrives_ms = g_udp_base_ms + ((g_udp_sample_n * 1000U) / 48000U);
    if (g_fake_now_ms < arrives_ms) {
        g_fake_now_ms = arrives_ms;
        if (g_udp_first_wait_n == 0U && g_udp_sample_n > g_udp_wait_after) {
            g_udp_first_wait_n = g_udp_sample_n;
        }
    }
    const double hz = g_udp_sample_n < g_udp_old_samples ? 100.0 : 131.8;
    *out = (int16_t)lround(3000.0 * cos(g_udp_phase));
    g_udp_phase += 2.0 * M_PI * hz / 48000.0;
    if (g_udp_phase > 2.0 * M_PI) {
        g_udp_phase -= 2.0 * M_PI;
    }
    g_udp_sample_n++;
    return 1;
}

/*
 * The backlog skip through getSymbol() on UDP input, where the symbol path brackets the live read (issue #576). The raw
 * WAV is on and its sync holds the decoder 12 ms after every 20 ms block. 100.0 Hz locks on audio read live, the
 * decoder is then held 400 ms, as for a rigctl retune, while the old channel keeps arriving, and the receiver moves:
 * @p boundary is an announced reset or a trunk-tuning generation move the tap finds on its own. The decoder drains the
 * 400 ms backlog, and the new channel's 131.8 Hz audio queues behind it while the syncs hold the decoder, which gains
 * 8 ms on the producer per block until it reads audio as it arrives. The old tone never comes back, nothing is heard
 * until a read has had to wait for the producer, which is well past the old channel's last sample and well short of
 * DSD_ANALOG_RX_BACKLOG_MAX_MS, and the new tone then locks within the p95 target plus a read.
 */
static void
run_udp_backlog_through_getsymbol(int boundary) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    state.samplesPerSymbol = 1;
    state.symbolCenter = 0;
    state.jitter = -1;
    exitflag = 0;
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.udp_read_sample = fake_udp_read_sample;
    dsd_net_audio_input_hooks_set(hooks);
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 1600000U;
    g_udp_base_ms = g_fake_now_ms;
    g_udp_sample_n = 0U;
    g_udp_old_samples = UINT64_MAX;
    g_udp_wait_after = UINT64_MAX;
    g_udp_first_wait_n = 0U;
    g_udp_phase = 0.0;
    raw_wav_stall_begin(&opts, 12U);

    /* 600 ms of the old channel, read as it arrives. */
    while (g_udp_sample_n < 28800U) {
        (void)getSymbol(&opts, &state, 0);
    }
    assert(rx_tone_locked_on_100(&state));
    const uint32_t generation = state.analog_rx.generation;

    /* Held while the old channel keeps arriving, which ends where the receiver moves. */
    g_fake_now_ms += 400U;
    g_udp_old_samples = ((g_fake_now_ms - g_udp_base_ms) * 48000U) / 1000U;
    const uint64_t boundary_n = g_udp_sample_n;
    g_udp_wait_after = g_udp_old_samples;
    if (boundary == BACKLOG_BOUNDARY_TUNING) {
        dsd_trunk_tuning_generation_advance();
    } else {
        dsd_analog_rx_reset(&state);
    }

    uint64_t heard_n = 0U;
    uint64_t locked_n = 0U;
    const uint64_t limit = boundary_n + (48U * (uint64_t)DSD_ANALOG_RX_BACKLOG_MAX_MS * 2U);
    while (locked_n == 0U && g_udp_sample_n < limit) {
        (void)getSymbol(&opts, &state, 0);
        if (state.analog_rx.generation == generation) {
            /* The tap finds a generation move at its next read, within one read of the boundary. */
            assert(g_udp_sample_n - boundary_n <= 960U);
            continue;
        }
        assert(state.analog_rx.ctcss_tenths_hz != 1000);
        if (heard_n == 0U && state.analog_rx.carrier_open) {
            heard_n = g_udp_sample_n;
        }
        if (state.analog_rx.tone_state == DSD_ANALOG_TONE_STATE_LOCKED) {
            locked_n = g_udp_sample_n;
        }
    }
    assert(g_udp_first_wait_n > g_udp_old_samples);
    assert(heard_n > g_udp_first_wait_n);
    assert(heard_n - boundary_n < 48U * (uint64_t)DSD_ANALOG_RX_BACKLOG_MAX_MS);
    assert(locked_n > heard_n && state.analog_rx.ctcss_tenths_hz == 1318);
    assert(locked_n - heard_n <= 48U * (uint64_t)(DSD_ANALOG_CTCSS_LOCK_P95_MS + DSD_ANALOG_RX_TAP_READ_MS));

    raw_wav_stall_end(&opts);
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

static void
test_rx_tone_backlog_skip_through_getsymbol(void) {
    run_udp_backlog_through_getsymbol(BACKLOG_BOUNDARY_RESET);
    run_udp_backlog_through_getsymbol(BACKLOG_BOUNDARY_TUNING);
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

/* --- Issue #527: the CTCSS/DCS receive policy at the monitor's one sink --- */

/* What the UDP analog socket was handed: the blocks and their bytes. Both live sinks, the local stream and this socket,
   sit behind the same gate (symbol_unsynced_audio_allowed()), so what reaches the socket is what the stream plays. */
static unsigned char g_played[sizeof(short) * 64U * 960U];
static size_t g_played_len;

static void
capture_monitor_block(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    g_monitor_blocks++;
    if (data && g_played_len + nsam <= sizeof(g_played)) {
        DSD_MEMCPY(g_played + g_played_len, data, nsam);
        g_played_len += nsam;
    }
}

static void
start_monitor_capture(dsd_opts* opts) {
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    dsd_udp_audio_hooks monitor = {0};
    monitor.blast_analog = capture_monitor_block;
    dsd_udp_audio_hooks_set(monitor);
    g_monitor_blocks = 0;
    g_played_len = 0;
}

static void
set_tone_policy(dsd_opts* opts, int mode, const char* list) {
    opts->analog_tone_filter = mode;
    assert(dsd_tone_set_parse(list, &opts->analog_tone_set, NULL, 0) == 0);
}

/* Feed @p blocks 20 ms blocks of a @p hz tone through the unsynced finalize step. */
static void
feed_hz_blocks(dsd_opts* opts, dsd_state* state, int blocks, double hz) {
    float block[960];
    for (int b = 0; b < blocks; b++) {
        fill_tone_block(block, 960U, hz, 3000.0);
        assert(dsd_symbol_test_finalize_unsynced_analog_block(opts, state, block, 960U) == 960U);
    }
}

/* An allow list naming the tone on air: muted while it is checked, then played, and within the CTCSS lock target of
   400 ms. The carrier is scan activity from the first block played, never while it is checked, and the verdict is
   logged once. */
static void
test_tone_policy_allow_match_unmutes_after_the_lock(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0/D023N");
    start_monitor_capture(&opts);
    g_tone_filter_lines = 0;
    g_tone_filter_allowed_100_lines = 0;
    int muted = 0;
    for (int b = 0; b < 40; b++) {
        clear_carrier_stamps(&state);
        const int played = g_monitor_blocks;
        feed_tone_blocks(&opts, &state, 1);
        if (g_monitor_blocks == played) {
            assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
            /* Still being checked: no scan activity (issue #527), so a check its carrier ends leaves no tail. */
            assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);
            assert(muted == b); /* muted only before the first block played */
            muted++;
        } else {
            assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED);
            /* Allowed: scan activity, as any carrier the monitor passes. */
            assert(state.last_cc_sync_time != 0 && state.last_vc_sync_time != 0);
        }
    }
    assert(muted >= 1 && muted * 20 <= DSD_ANALOG_CTCSS_LOCK_P95_MS);
    assert(rx_tone_locked_on_100(&state) && state.analog_rx.gate_no_tone == 0);
    assert(g_tone_filter_lines == 1 && g_tone_filter_allowed_100_lines == 1);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* An allow list that does not name the tone on air: never played, rejected once the tone is confirmed, and from then on
   no scan activity. The -6 raw WAV keeps every block: it is a capture ahead of every gate. */
static void
test_tone_policy_reject_mutes_every_sink_but_the_raw_wav(void) {
    char raw_path[DSD_TEST_PATH_MAX];
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.wav_out_raw = open_raw_wav_out(raw_path, sizeof(raw_path), 48000);
    assert(opts.wav_out_raw != NULL);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    start_monitor_capture(&opts);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state) && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED);
    assert(g_monitor_blocks == 0);
    clear_carrier_stamps(&state);
    feed_tone_blocks(&opts, &state, 5);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0 && g_monitor_blocks == 0);
    /* A block list naming it rejects it the same way. */
    set_tone_policy(&opts, DSD_TONE_FILTER_BLOCK, "100.0");
    feed_tone_blocks(&opts, &state, 25);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED && g_monitor_blocks == 0);
    sf_close(opts.wav_out_raw);
    opts.wav_out_raw = NULL;
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(raw_path, SFM_READ, &info);
    assert(wav != NULL);
    assert(info.frames == (sf_count_t)(60 * 960));
    sf_close(wav);
    (void)remove(raw_path);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* No tone on air (a steady voice-band tone): an allow list keeps it muted through the window and rejects it as the
   window ends, never before; a block list lets it play from then on. */
static void
test_tone_policy_no_tone_decides_at_the_window(void) {
    static dsd_opts opts;
    static dsd_state state;
    const int window_blocks = DSD_ANALOG_TONE_WINDOW_MS / 20;
    for (int mode = DSD_TONE_FILTER_ALLOW; mode <= DSD_TONE_FILTER_BLOCK; mode++) {
        install_fake_rtl_hooks(0);
        init_analog_monitor_fixture(&opts, &state);
        set_tone_policy(&opts, mode, "100.0");
        start_monitor_capture(&opts);
        g_tone_filter_rejected_no_tone_lines = 0;
        /* The block that opens the window, then the window: checked, muted. */
        feed_hz_blocks(&opts, &state, window_blocks, 1000.0);
        assert(state.analog_rx.carrier_open == 1 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
        assert(g_monitor_blocks == 0);
        feed_hz_blocks(&opts, &state, 2, 1000.0);
        assert(state.analog_rx.gate_no_tone == 1);
        if (mode == DSD_TONE_FILTER_ALLOW) {
            assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED && g_monitor_blocks == 0);
            assert(g_tone_filter_rejected_no_tone_lines == 1);
        } else {
            assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED && g_monitor_blocks >= 1);
        }
        dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
        dsd_state_ext_free_all(&state);
    }
}

/* Policy off is the monitor as it was: every block plays, byte for byte what a session with no list plays, a list
   left in place by `off` included; and a retune ends a reception, so its verdict never carries to the next channel. */
static void
test_tone_policy_off_is_byte_identical(void) {
    static dsd_opts opts;
    static dsd_state state;
    static unsigned char without[sizeof(g_played)];
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    start_monitor_capture(&opts);
    g_tone_phase = 0.0;
    feed_tone_blocks(&opts, &state, 20);
    assert(g_monitor_blocks == 20 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);
    const size_t without_len = g_played_len;
    DSD_MEMCPY(without, g_played, without_len);
    dsd_state_ext_free_all(&state);

    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_OFF, "67.0/D023N");
    start_monitor_capture(&opts);
    g_tone_phase = 0.0;
    feed_tone_blocks(&opts, &state, 20);
    assert(g_monitor_blocks == 20 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF);
    assert(g_played_len == without_len && memcmp(g_played, without, without_len) == 0);

    /* Allowed, then a retune: the new channel is checked again before a block of it plays. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    feed_tone_blocks(&opts, &state, 30);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED);
    dsd_analog_rx_reset(&state);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    const int played = g_monitor_blocks;
    feed_tone_blocks(&opts, &state, 3);
    assert(g_monitor_blocks == played && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* The AM monitor and the -8 source monitor run no policy: a list in force there gates nothing. */
static void
test_tone_policy_only_on_the_fm_monitor(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    start_monitor_capture(&opts);
    opts.analog_only = 0; /* the -8 monitor under digital decoding */
    feed_tone_blocks(&opts, &state, 10);
    assert(g_monitor_blocks == 10 && dsd_analog_tone_gate_in_force(&opts, &state) == DSD_ANALOG_TONE_GATE_OFF);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);

    /* The AM monitor (issue #524): the tap keeps its carrier, which holds a scan row, but detects no tone, so the list
       judges nothing -- past the window every block still plays, and no verdict is published or logged. */
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    set_monitor_kind(&opts, DSD_ANALOG_DEMOD_AM);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    start_monitor_capture(&opts);
    feed_tone_blocks(&opts, &state, 2);
    g_monitor_blocks = 0;
    g_tone_filter_lines = 0;
    const int blocks = (DSD_ANALOG_TONE_WINDOW_DCS_MS / 20) + 10;
    for (int b = 0; b < blocks; b++) {
        clear_carrier_stamps(&state);
        feed_tone_blocks(&opts, &state, 1);
        assert(state.analog_rx.carrier_open == 1 && state.last_cc_sync_time != 0);
        assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_OFF && state.analog_rx.gate_no_tone == 0);
    }
    assert(g_monitor_blocks == blocks && g_tone_filter_lines == 0);
    assert(dsd_analog_tone_gate_in_force(&opts, &state) == DSD_ANALOG_TONE_GATE_OFF);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
    install_fake_rtl_hooks(0);
}

/* A policy changed mid-reception (a config load, a scan row's own coming on air) judges it afresh, muted for a check
   of its own, and says nothing of a tone lost, since none was; an allowed tone that is lost still says so. */
static void
test_tone_policy_change_is_no_tone_lost(void) {
    static dsd_opts opts;
    static dsd_state state;
    const int window_blocks = DSD_ANALOG_TONE_WINDOW_MS / 20;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_BLOCK, "67.0");
    start_monitor_capture(&opts);
    g_tone_filter_lines = 0;
    g_tone_filter_allowed_no_tone_lines = 0;
    g_tone_filter_pending_lost_lines = 0;
    feed_hz_blocks(&opts, &state, window_blocks + 2, 1000.0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED && state.analog_rx.gate_no_tone == 1);
    assert(g_tone_filter_allowed_no_tone_lines == 1);

    set_tone_policy(&opts, DSD_TONE_FILTER_BLOCK, "67.0/71.9");
    const int played = g_monitor_blocks;
    feed_hz_blocks(&opts, &state, window_blocks - 1, 1000.0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING && g_monitor_blocks == played);
    feed_hz_blocks(&opts, &state, 2, 1000.0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED && g_monitor_blocks > played);
    assert(g_tone_filter_allowed_no_tone_lines == 2 && g_tone_filter_pending_lost_lines == 0);
    assert(g_tone_filter_lines == 2);

    /* Allowed on a listed tone, then the tone goes while the carrier stays: that check is a tone lost. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    feed_tone_blocks(&opts, &state, 30);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED && g_tone_filter_pending_lost_lines == 0);
    int lost_after = -1;
    for (int b = 0; b < DSD_ANALOG_CTCSS_LOSS_CEILING_MS / 20 + 5 && lost_after < 0; b++) {
        feed_hz_blocks(&opts, &state, 1, 1000.0);
        if (state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING) {
            lost_after = b;
        }
    }
    assert(lost_after >= 0 && g_tone_filter_pending_lost_lines == 1);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* A reception rejected for want of a tone that then carries a tone the list does not pass stays muted, and the
   rejection names that tone from then on: in the log, and in the published reason the frontends word. */
static void
test_tone_policy_no_tone_rejection_names_a_later_tone(void) {
    static dsd_opts opts;
    static dsd_state state;
    const int window_blocks = DSD_ANALOG_TONE_WINDOW_MS / 20;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    start_monitor_capture(&opts);
    g_tone_filter_lines = 0;
    g_tone_filter_rejected_no_tone_lines = 0;
    g_tone_filter_rejected_100_lines = 0;
    feed_hz_blocks(&opts, &state, window_blocks + 2, 1000.0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED && state.analog_rx.gate_no_tone == 1);
    assert(g_tone_filter_rejected_no_tone_lines == 1);
    feed_tone_blocks(&opts, &state, 30);
    assert(rx_tone_locked_on_100(&state));
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED && state.analog_rx.gate_no_tone == 0);
    assert(g_tone_filter_rejected_100_lines == 1 && g_tone_filter_lines == 2 && g_monitor_blocks == 0);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* Feed 20 ms blocks of digital silence until the tap's carrier closes (its 200 ms hangover run out), asserting the
   verdict @p gate stands on the carrier until then and no end of a rejected reception is published meanwhile. */
static void
feed_until_carrier_closes(dsd_opts* opts, dsd_state* state, int gate) {
    for (int blocks = 0; state->analog_rx.carrier_open; blocks++) {
        assert(state->analog_rx.gate == gate && state->analog_rx.gate_rejected_ended == 0);
        assert(blocks <= (DSD_ANALOG_CARRIER_HANGOVER_MS / 20) + 1);
        feed_blocks_at(opts, state, 1, 0.0);
    }
}

/* Traffic the policy rejected outlives its carrier for the scanners (issue #527): once the hangover has run out under
   a rejection the verdict is back to checking, with no carrier, but gate_rejected_ended says the reception ended
   rejected until the next carrier opens a reception of its own, so a scanner whose pass comes after it ended still
   moves on. A reception that ended passing leaves nothing, and a retune, a policy change or a paused input's next
   carrier forget it. */
static void
test_tone_policy_rejection_outlives_its_carrier(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    start_monitor_capture(&opts);
    feed_tone_blocks(&opts, &state, 30);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_REJECTED);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING && state.analog_rx.gate_rejected_ended == 1);
    assert(dsd_analog_rx_rejection_ended_now(&opts, &state) == 1);
    /* It lasts through the quiet, and nothing plays ... */
    feed_blocks_at(&opts, &state, 50, 0.0);
    assert(state.analog_rx.gate_rejected_ended == 1 && g_monitor_blocks == 0);
    /* ... until the next carrier opens a reception of its own, checked afresh. */
    feed_tone_blocks(&opts, &state, 1);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    assert(state.analog_rx.gate_rejected_ended == 0 && dsd_analog_rx_rejection_ended_now(&opts, &state) == 0);

    /* That one passes, and ends passing: nothing is left behind. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    feed_tone_blocks(&opts, &state, 30);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_ALLOWED);
    assert(state.analog_rx.gate_rejected_ended == 0);

    /* A retune forgets a rejected reception that ended, as it forgets the carrier. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    feed_tone_blocks(&opts, &state, 30);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_REJECTED);
    assert(state.analog_rx.gate_rejected_ended == 1);
    dsd_analog_rx_reset(&state);
    assert(state.analog_rx.gate_rejected_ended == 0 && dsd_analog_rx_rejection_ended_now(&opts, &state) == 0);
    feed_blocks_at(&opts, &state, 3, 0.0);
    assert(state.analog_rx.gate_rejected_ended == 0);

    /* So does a policy change: the new policy judged nothing. */
    feed_tone_blocks(&opts, &state, 30);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_REJECTED);
    assert(state.analog_rx.gate_rejected_ended == 1);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0/71.9");
    feed_blocks_at(&opts, &state, 1, 0.0);
    assert(state.analog_rx.gate_rejected_ended == 0);

    /* Off the FM monitor detection stops, and with it the policy: nothing is in force to have rejected anything. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    feed_tone_blocks(&opts, &state, 30);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_REJECTED);
    opts.analog_only = 0;
    assert(dsd_analog_rx_rejection_ended_now(&opts, &state) == 0);
    opts.analog_only = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);

    /* An input that pauses ends its reception as the hangover does: the read after the pause, which the tap drops,
       says a rejected reception ended; the carrier that follows opens a new one. */
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "67.0");
    dsd_analog_rx_test_set_clock(fake_now_ms);
    g_fake_now_ms = 200000U;
    for (int b = 0; b < 30; b++) {
        feed_stream_block(&opts, &state, 100.0);
    }
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_REJECTED && state.analog_rx.gate_rejected_ended == 0);
    g_fake_now_ms += 1000U;
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 0 && state.analog_rx.gate_rejected_ended == 1);
    feed_stream_block(&opts, &state, 100.0);
    assert(state.analog_rx.carrier_open == 1 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    assert(state.analog_rx.gate_rejected_ended == 0);
    dsd_analog_rx_test_set_clock(NULL);
    dsd_state_ext_free_all(&state);
}

/* A carrier still being checked is no scan activity (issue #527): only blocks under OFF or ALLOWED stamp the -Y
   hangtime and voice anchors. Short no-tone bursts under an allow list -- kerchunks, noise -- each end before the
   window and before any verdict, and leave no stamp, no played block and no rejection behind them, so they cannot hold
   a scanner on a muted row; each is a check of its own. A block list's no-tone traffic stamps from the block its window
   passes it, and an allowed tone lost under an allow list stamps nothing while it is checked again. */
static void
test_tone_policy_check_is_no_scan_activity(void) {
    static dsd_opts opts;
    static dsd_state state;
    const int window_blocks = DSD_ANALOG_TONE_WINDOW_MS / 20;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    start_monitor_capture(&opts);
    clear_carrier_stamps(&state);
    for (int burst = 0; burst < 3; burst++) {
        feed_hz_blocks(&opts, &state, window_blocks / 2, 1000.0);
        assert(state.analog_rx.carrier_open == 1 && state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
        feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_PENDING);
        assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING && state.analog_rx.gate_rejected_ended == 0);
        assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0 && g_monitor_blocks == 0);
    }

    /* A block list: checked, no stamp; passed for want of a tone once the window ends, stamped from that block. */
    set_tone_policy(&opts, DSD_TONE_FILTER_BLOCK, "100.0");
    int stamped_from = -1;
    for (int b = 0; b < window_blocks + 2; b++) {
        clear_carrier_stamps(&state);
        feed_hz_blocks(&opts, &state, 1, 1000.0);
        const int passed = state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED;
        assert(passed || state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
        assert((state.last_cc_sync_time != 0) == passed && (state.last_vc_sync_time != 0) == passed);
        if (passed && stamped_from < 0) {
            stamped_from = b;
        }
    }
    assert(stamped_from >= window_blocks && state.analog_rx.gate_no_tone == 1 && g_monitor_blocks > 0);
    feed_until_carrier_closes(&opts, &state, DSD_ANALOG_TONE_GATE_ALLOWED);

    /* An allow list: the allowed tone stamps; once it is lost the traffic is checked again, and stamps nothing. */
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    feed_tone_blocks(&opts, &state, 30);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_ALLOWED && state.last_cc_sync_time != 0);
    for (int b = 0; b < DSD_ANALOG_CTCSS_LOSS_CEILING_MS / 20 + 5; b++) {
        feed_hz_blocks(&opts, &state, 1, 1000.0);
        if (state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING) {
            break;
        }
    }
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    clear_carrier_stamps(&state);
    feed_hz_blocks(&opts, &state, 5, 1000.0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    assert(state.last_cc_sync_time == 0 && state.last_vc_sync_time == 0);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* One 20 ms block through the unsynced finalize step: whether the sink played it must be exactly whether the monitor's
   audio chain got it with DSD_ANALOG_AUDIO_PLAYING, and @p want_discard whether it got DSD_ANALOG_AUDIO_DISCARD (the
   sink never asks for DSD_ANALOG_AUDIO_RESET: the chain starts over at a new reception by itself). */
static void
expect_chain_block(dsd_opts* opts, dsd_state* state, int want_played, int want_discard) {
    const int played_before = g_monitor_blocks;
    const int calls_before = g_chain_calls;
    feed_tone_blocks(opts, state, 1);
    const int played = g_monitor_blocks > played_before;
    assert(g_chain_calls == calls_before + 1);
    assert(played == want_played);
    assert(((g_chain_last_flags & DSD_ANALOG_AUDIO_PLAYING) != 0U) == want_played);
    assert(((g_chain_last_flags & DSD_ANALOG_AUDIO_DISCARD) != 0U) == want_discard);
    assert((g_chain_last_flags & DSD_ANALOG_AUDIO_RESET) == 0U);
}

/* Issue #518: the monitor's gain stage adapts to exactly the audio that plays. The chain gets the PLAYING flag for a
   block the sink plays and not for one it holds back -- the squelch closed, the output off, a retune unresolved, the
   tone policy still checking -- and DISCARD for a block that straddles a retune, whose samples are partly the old
   channel's. It always gets the raw block (the tap reads it first, test_rx_tone_tap_reads_raw_block_before_voice_filters)
   with its source and rate. */
static void
test_chain_playing_follows_the_sink(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    start_monitor_capture(&opts);
    dsd_trunk_tuning_requests_reset();

    expect_chain_block(&opts, &state, 1, 0);
    assert(g_chain_last_rate_hz == 48000);
#ifdef USE_RADIO
    /* (A build without radio support has no RTL input, and treats every input as PCM.) */
    assert(g_chain_last_source == DSD_ANALOG_AUDIO_SOURCE_RTL_MONITOR);
#endif
    /* The squelch closed over the block (the RTL path reads its power with the sample: held at 1.0 here). */
    opts.rtl_squelch_level = 2.0;
    expect_chain_block(&opts, &state, 0, 0);
    opts.rtl_squelch_level = 0.0;
    expect_chain_block(&opts, &state, 1, 0);
    /* Output off (-o null or a muted UI): nothing plays, so nothing adapts. */
    opts.audio_out = 0;
    expect_chain_block(&opts, &state, 0, 0);
    opts.audio_out = 1;
    /* A retune in flight, then landed: the block that straddles its completion is dropped from the chain. */
    const uint64_t request = dsd_trunk_tuning_request_begin();
    expect_chain_block(&opts, &state, 0, 0);
    dsd_trunk_tuning_request_complete(request, DSD_TRUNK_TUNE_RESULT_OK);
    expect_chain_block(&opts, &state, 0, 1);
    expect_chain_block(&opts, &state, 1, 0);
    /* A retune nobody announced (a new stream generation): the same. */
    g_fake_rtl_generation++;
    expect_chain_block(&opts, &state, 0, 1);
    expect_chain_block(&opts, &state, 1, 0);
    dsd_trunk_tuning_requests_reset();

    /* PCM input: the PCM source at the input's rate; the tone policy still checking holds the AGC too. */
    install_fake_rtl_hooks(0);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    /* The switch of input is a boundary the tap reads as a new reception: that block is dropped from the chain too. */
    expect_chain_block(&opts, &state, 0, 1);
    expect_chain_block(&opts, &state, 1, 0);
    assert(g_chain_last_source == DSD_ANALOG_AUDIO_SOURCE_PCM16 && g_chain_last_rate_hz == 48000);
    set_tone_policy(&opts, DSD_TONE_FILTER_ALLOW, "100.0");
    expect_chain_block(&opts, &state, 0, 0);
    assert(state.analog_rx.gate == DSD_ANALOG_TONE_GATE_PENDING);
    opts.analog_tone_filter = DSD_TONE_FILTER_OFF;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* One 20 ms block of @p value through the unsynced finalize step with the auto squelch's flags: open over [0, @p open1),
   closed to @p reopen, open to the end. */
static void
feed_flagged_block(dsd_opts* opts, dsd_state* state, float value, unsigned int open1, unsigned int reopen) {
    float block[960];
    uint8_t flags[960];
    for (unsigned int i = 0; i < 960U; i++) {
        block[i] = value;
        flags[i] = (i >= open1 && i < reopen) ? (uint8_t)DSD_SQUELCH_FLAG_CLOSED : (uint8_t)0U;
    }
    g_chain_runs = 0;
    assert(dsd_symbol_test_finalize_unsynced_analog_block_flags(opts, state, block, flags, 960U) == 960U);
}

/* Issue #518 follow-up: under the auto squelch each RTL monitor sample carries its gate. The level comparison is off
   (the level here would close it), the audio chain gets each run of heard and unheard samples with its own playing flag
   (the AGC adapts to exactly what is heard and rolls back at the close), the sink ramps each sample in (5 ms) and out
   (10 ms) after the chain, so a closed stretch is exact silence, and a block with nothing heard is not written. The
   tap's carrier follows the flags, so a silent carrier holds the scan row, which the level squelch's energy floor
   would not. */
static void
test_auto_squelch_gates_each_sample(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(1);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_squelch_margin_db = 10;
    opts.rtl_squelch_level = 2.0; /* above rtl_pwr: the level squelch would close */
    g_chain_removes_tone = 0;
    start_monitor_capture(&opts);
    dsd_trunk_tuning_requests_reset();

    /* Open, closed past the release, open again. */
    feed_flagged_block(&opts, &state, 1000.0f, 200U, 900U);
    assert(g_monitor_blocks == 1 && g_played_len == 960U * sizeof(short));
    assert(g_chain_runs == 3);
    assert(g_chain_run_len[0] == 200U && g_chain_run_flags[0] == DSD_ANALOG_AUDIO_PLAYING);
    assert(g_chain_run_len[1] == 700U && g_chain_run_flags[1] == 0U);
    assert(g_chain_run_len[2] == 60U && g_chain_run_flags[2] == DSD_ANALOG_AUDIO_PLAYING);
    short out[960];
    DSD_MEMCPY(out, g_played, sizeof out);
    /* 5 ms up: 240 samples at 48 kHz; 10 ms down: 480. */
    assert(out[0] > 0 && out[0] < 10 && out[100] > 400 && out[100] < 430 && out[199] > 820 && out[199] < 840);
    for (unsigned int i = 200U + 480U; i < 900U; i++) {
        assert(out[i] == 0);
    }
    assert(out[300] > 0 && out[300] < 830);
    assert(out[959] > 240 && out[959] < 260);

    /* Closed throughout: the release tail is written once, then nothing. */
    feed_flagged_block(&opts, &state, 1000.0f, 0U, 960U);
    assert(g_monitor_blocks == 2 && g_chain_runs == 1 && g_chain_run_flags[0] == 0U);
    feed_flagged_block(&opts, &state, 1000.0f, 0U, 960U);
    assert(g_monitor_blocks == 2);

    /* Several transitions inside one block: one run each. */
    float block[960];
    uint8_t flags[960];
    for (unsigned int i = 0; i < 960U; i++) {
        block[i] = 500.0f;
        flags[i] = ((i / 100U) % 2U) ? (uint8_t)DSD_SQUELCH_FLAG_CLOSED : (uint8_t)0U;
    }
    g_chain_runs = 0;
    assert(dsd_symbol_test_finalize_unsynced_analog_block_flags(&opts, &state, block, flags, 960U) == 960U);
    assert(g_chain_runs == 10);
    for (int r = 0; r < 10; r++) {
        assert(g_chain_run_flags[r] == ((r % 2) ? 0U : DSD_ANALOG_AUDIO_PLAYING));
    }

    /* Through the sample path the symbol reader takes: each sample's flag goes into the block with it. */
    g_chain_runs = 0;
    for (unsigned int i = 0; i < 960U; i++) {
        dsd_symbol_test_push_unsynced_analog_sample_flag(&opts, &state, 700.0f,
                                                         i < 480U ? (uint8_t)0U : (uint8_t)DSD_SQUELCH_FLAG_CLOSED);
    }
    assert(g_chain_runs == 2 && g_chain_run_len[0] == 480U && g_chain_run_flags[0] == DSD_ANALOG_AUDIO_PLAYING);
    assert(g_chain_run_len[1] == 480U && g_chain_run_flags[1] == 0U);

    /* The block gate wins over the per-sample one: a block it rejects (digital sync here; a muted output, a retune
       still landing and the tone policy alike) plays nothing, not even the fade out of the open block before it. */
    feed_flagged_block(&opts, &state, 1000.0f, 960U, 960U);
    const int written_open = g_monitor_blocks;
    state.carrier = 1;
    feed_flagged_block(&opts, &state, 1000.0f, 960U, 960U);
    assert(g_monitor_blocks == written_open);
    state.carrier = 0;
    /* After it the gate opens from silence: the 5 ms ramp, not the old gain. */
    feed_flagged_block(&opts, &state, 1000.0f, 960U, 960U);
    assert(g_monitor_blocks == written_open + 1);
    DSD_MEMCPY(out, g_played, sizeof out);
    assert(out[0] > 0 && out[0] < 10);

    /* Closed past the hangover, so nothing before carries over: then a silent carrier, zero audio with every sample
       open. The tap holds the row on it. */
    for (int b = 0; b < 15; b++) {
        feed_flagged_block(&opts, &state, 0.0f, 0U, 960U);
    }
    assert(state.analog_rx.carrier_open == 0);
    clear_carrier_stamps(&state);
    feed_flagged_block(&opts, &state, 0.0f, 960U, 960U);
    feed_flagged_block(&opts, &state, 0.0f, 960U, 960U);
    assert(state.analog_rx.carrier_open == 1);
    assert(state.last_vc_sync_time != 0 && state.last_cc_sync_time != 0);
    /* Closed long past the hangover: the carrier goes, and stamps stop. */
    for (int b = 0; b < 15; b++) {
        feed_flagged_block(&opts, &state, 0.0f, 0U, 960U);
    }
    assert(state.analog_rx.carrier_open == 0);
    clear_carrier_stamps(&state);
    feed_flagged_block(&opts, &state, 0.0f, 0U, 960U);
    assert(state.last_vc_sync_time == 0 && state.last_cc_sync_time == 0);

    /* Under the level squelch the same silent block is no carrier (its energy floor), and the flags mean nothing. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = 0.0;
    feed_flagged_block(&opts, &state, 0.0f, 960U, 960U);
    feed_flagged_block(&opts, &state, 0.0f, 960U, 960U);
    assert(state.analog_rx.carrier_open == 0);
    const int blocks = g_monitor_blocks;
    feed_flagged_block(&opts, &state, 1000.0f, 0U, 960U);
    assert(g_monitor_blocks == blocks + 1 && g_chain_runs == 1 && g_chain_run_flags[0] == DSD_ANALOG_AUDIO_PLAYING);

    /* AUTO on PCM input resolves to off: no per-sample gate, and the level comparison is off too. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_squelch_level = 2.0;
    install_fake_rtl_hooks(0);
    opts.audio_in_type = AUDIO_IN_WAV;
    feed_flagged_block(&opts, &state, 1000.0f, 0U, 960U); /* the switch of input: a new reception, dropped */
    const int pcm_blocks = g_monitor_blocks;
    feed_flagged_block(&opts, &state, 1000.0f, 0U, 960U);
    assert(g_monitor_blocks == pcm_blocks + 1 && g_chain_runs == 1);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    g_chain_removes_tone = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* Push @p seconds of an SDR program's FM output through the monitor's sample path. */
static void
push_tap(dsd_opts* opts, dsd_state* state, pcm_tap* src, pcm_tap_kind kind, double cnr_db, double seconds) {
    const int n = (int)lround(seconds * (double)PCM_TAP_RATE);
    for (int i = 0; i < n; i++) {
        dsd_symbol_test_push_unsynced_analog_sample(opts, state, pcm_tap_next(src, kind, cnr_db, 1000.0, 0.0));
    }
}

/* The PCM noise squelch (issue #628) on audio input's FM monitor, through the sample path the symbol reader takes: the
   capture attaches each sample's flag, so learning is closed, noise stays shut, a carrier opens the gate, confirms
   the reference and holds the row, and the release is the sink's ramp as under the radio squelches. A reset (a
   retune) restarts its windows and keeps the reference; a new input volume forgets it; a switch to LEVEL and back
   starts the windows over. A source with no room (8 kHz) or no band (low-passed) runs the level path, whose energy
   floor keeps digital silence from holding the row. */
static void
test_pcm_noise_squelch_gates_each_sample(void) {
    static dsd_opts opts;
    static dsd_state state;
    static pcm_tap src;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    opts.rtl_squelch_margin_db = 10;
    opts.rtl_squelch_level = 2.0; /* the level beneath, not in force under NOISE */
    g_chain_removes_tone = 0;
    start_monitor_capture(&opts);
    pcm_tap_init(&src, 31U, 12500.0, 0.0, 0.0);

    /* Noise: closed while learning, then noise reads about 0 dB; nothing plays, nothing holds the row. */
    clear_carrier_stamps(&state);
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 1.0);
    assert(g_monitor_blocks == 0);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_PROVISIONAL);
    assert(state.squelch_noise_active == 1U && state.squelch_auto_active == 1U && state.squelch_noise_measured == 1U);
    assert(state.squelch_noise_quieting_cdb > -300 && state.squelch_noise_quieting_cdb < 300);
    assert(state.squelch_auto_plan_valid == 1U && state.squelch_auto_gate_open == 0U);
    assert(dsd_squelch_dynamic_in_force(&opts, &state));
    assert(state.analog_rx.carrier_open == 0 && state.last_cc_sync_time == 0);

    /* A carrier opens it: it plays, the reference is confirmed, and the carrier holds the row. */
    push_tap(&opts, &state, &src, PCM_TAP_TONE, 20.0, 1.0);
    assert(g_monitor_blocks >= 45);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_KNOWN);
    assert(state.squelch_noise_quieting_cdb > 1500 && state.squelch_auto_gate_open == 1U);
    assert(state.analog_rx.carrier_open == 1 && state.last_cc_sync_time != 0);

    /* Noise again: shut within a window or two, the release tail written once. */
    int before = g_monitor_blocks;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 1.0);
    assert(g_monitor_blocks <= before + 3);
    assert(state.squelch_auto_gate_open == 0U);

    /* A reset (a retune) restarts the windows and keeps the reference: the next carrier plays from its first windows. */
    dsd_analog_rx_reset(&state);
    before = g_monitor_blocks;
    push_tap(&opts, &state, &src, PCM_TAP_TONE, 20.0, 0.4);
    assert(g_monitor_blocks >= before + 12);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_KNOWN);

    /* Another input volume forgets the reference: learning again, closed. */
    opts.input_volume_multiplier = 2;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.02);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_LEARNING);
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.5);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_PROVISIONAL);

    /* LEVEL (off) plays the noise; back to NOISE the windows start over, closed, and the noise is shut again. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = 0.0;
    before = g_monitor_blocks;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.2);
    assert(g_monitor_blocks >= before + 9);
    assert(!dsd_squelch_dynamic_in_force(&opts, &state));
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.1);
    before = g_monitor_blocks;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.5);
    assert(g_monitor_blocks == before);
    assert(dsd_squelch_dynamic_in_force(&opts, &state));

    /* No room: an 8 kHz source (staged to 48 kHz). The squelch is off and the level path runs (off here: the noise
       plays); digital silence is no carrier. */
    opts.wav_sample_rate = 8000;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.2);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_NO_ROOM);
    assert(!dsd_squelch_dynamic_in_force(&opts, &state));
    before = g_monitor_blocks;
    push_tap(&opts, &state, &src, PCM_TAP_NOISE, 0.0, 0.2);
    assert(g_monitor_blocks >= before + 9);
    push_tap(&opts, &state, &src, PCM_TAP_ZERO, 0.0, 0.6);
    assert(state.analog_rx.carrier_open == 0);

    /* No band: a source low-passed at 3 kHz. Shut while the evidence gathers, then off: the level path again. */
    opts.wav_sample_rate = 48000;
    static pcm_tap low;
    pcm_tap_init(&low, 32U, 12500.0, 75.0, 3000.0);
    push_tap(&opts, &state, &low, PCM_TAP_NOISE, 0.0, 2.5);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_NO_BAND);
    assert(!dsd_squelch_dynamic_in_force(&opts, &state));
    before = g_monitor_blocks;
    push_tap(&opts, &state, &low, PCM_TAP_NOISE, 0.0, 0.2);
    assert(g_monitor_blocks >= before + 9);
    push_tap(&opts, &state, &low, PCM_TAP_ZERO, 0.0, 0.6);
    assert(state.analog_rx.carrier_open == 0);

    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    g_chain_removes_tone = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&state);
}

/* The UDP producer for the backlog test: noise, then a carrier, then (from g_pcm_udp_new_n) the new channel's noise,
   each sample arriving at its 48 kHz time on the fake clock. */
static pcm_tap g_pcm_udp_src;
static uint64_t g_pcm_udp_n = 0U;
static uint64_t g_pcm_udp_base_ms = 0U;
static uint64_t g_pcm_udp_carrier_n = UINT64_MAX;
static uint64_t g_pcm_udp_new_n = UINT64_MAX;

static int
fake_pcm_udp_read_sample(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const uint64_t arrives_ms = g_pcm_udp_base_ms + ((g_pcm_udp_n * 1000U) / 48000U);
    if (g_fake_now_ms < arrives_ms) {
        g_fake_now_ms = arrives_ms;
    }
    const int carrier = g_pcm_udp_n >= g_pcm_udp_carrier_n && g_pcm_udp_n < g_pcm_udp_new_n;
    const float v = pcm_tap_next(&g_pcm_udp_src, carrier ? PCM_TAP_TONE : PCM_TAP_NOISE, 20.0, 1000.0, 0.0);
    *out = (int16_t)v;
    g_pcm_udp_n++;
    return 1;
}

/* The PCM noise squelch through getSymbol() on UDP input (issue #628): it learns on noise and a carrier plays; then the
   decoder is held 400 ms, as for a rigctl retune, while the old carrier keeps arriving, and the receiver moves. What the
   input queued meanwhile is the old channel's: the squelch holds the gate closed over it and learns nothing from it,
   so none of it plays, and the new channel's noise stays shut. */
static void
test_pcm_noise_squelch_holds_the_backlog(void) {
    static dsd_opts opts;
    static dsd_state state;
    install_fake_rtl_hooks(0);
    init_analog_monitor_fixture(&opts, &state);
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    opts.rtl_squelch_margin_db = 10;
    g_chain_removes_tone = 0;
    start_monitor_capture(&opts);
    state.samplesPerSymbol = 1;
    state.symbolCenter = 0;
    state.jitter = -1;
    exitflag = 0;
    dsd_net_audio_input_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.udp_read_sample = fake_pcm_udp_read_sample;
    dsd_net_audio_input_hooks_set(hooks);
    dsd_analog_rx_test_set_clock(fake_now_ms);
    pcm_tap_init(&g_pcm_udp_src, 41U, 12500.0, 0.0, 0.0);
    g_fake_now_ms = 2600000U;
    g_pcm_udp_base_ms = g_fake_now_ms;
    g_pcm_udp_n = 0U;
    g_pcm_udp_carrier_n = 48000U;
    g_pcm_udp_new_n = UINT64_MAX;

    /* 1 s of noise, then 600 ms of the carrier, read as they arrive. */
    while (g_pcm_udp_n < 48000U + 28800U) {
        (void)getSymbol(&opts, &state, 0);
    }
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_KNOWN && state.squelch_auto_gate_open == 1U);
    assert(g_monitor_blocks > 20);

    /* Held 400 ms while the old carrier keeps arriving; the receiver moves; the new channel is noise. */
    g_fake_now_ms += 400U;
    g_pcm_udp_new_n = ((g_fake_now_ms - g_pcm_udp_base_ms) * 48000U) / 1000U;
    dsd_analog_rx_reset(&state);
    const int at_boundary = g_monitor_blocks;
    const uint64_t boundary_n = g_pcm_udp_n;
    while (g_pcm_udp_n < g_pcm_udp_new_n + 48000U) {
        (void)getSymbol(&opts, &state, 0);
    }
    /* The backlog (the old carrier) never played, nor the new channel's noise after it: the gate open at the move
       releases over the first 10 ms (480 samples) after it, written once, and then nothing. */
    assert(g_pcm_udp_new_n - boundary_n > 9600U);
    assert(g_monitor_blocks <= at_boundary + 1);
    if (g_monitor_blocks == at_boundary + 1) {
        short last[960];
        assert(g_played_len >= sizeof last);
        DSD_MEMCPY(last, g_played + g_played_len - sizeof last, sizeof last);
        for (int i = 480; i < 960; i++) {
            assert(last[i] == 0);
        }
    }
    assert(state.squelch_auto_gate_open == 0U);
    assert(state.squelch_noise_state == DSD_SQUELCH_NOISE_STATE_KNOWN);
    assert(state.squelch_noise_quieting_cdb > -300 && state.squelch_noise_quieting_cdb < 300);

    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    g_chain_removes_tone = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_analog_rx_test_set_clock(NULL);
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
    test_soft_symbol_replay_bounds_stored_symbols();
    test_float_symbol_replay_bounds_stored_symbols();
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
    test_rx_tone_dcs_through_the_tap();
    test_rx_tone_dcs_is_fm_only();
    test_rx_tone_paused_stream_starts_a_new_reception();
    test_rx_tone_pause_mid_block_inherits_nothing();
    test_rx_tone_pcm_squelch_follows_each_read();
    test_rx_tone_dropped_block_restarts_the_tap();
    test_rx_tone_stalled_radio_stream_goes_stale();
    test_rx_tone_tcp_reconnect_starts_a_new_reception();
    test_rx_tone_retune_skips_the_input_backlog();
    test_rx_tone_backlog_skip_is_bounded();
    test_rx_tone_backlog_skip_ignores_playback_time();
    test_rx_tone_backlog_skip_hears_a_stalled_live_input();
    test_rx_tone_backlog_skip_survives_preempted_reads();
    test_rx_tone_backlog_skip_through_getsymbol();
    test_tone_policy_allow_match_unmutes_after_the_lock();
    test_tone_policy_reject_mutes_every_sink_but_the_raw_wav();
    test_tone_policy_no_tone_decides_at_the_window();
    test_tone_policy_off_is_byte_identical();
    test_tone_policy_only_on_the_fm_monitor();
    test_tone_policy_change_is_no_tone_lost();
    test_tone_policy_no_tone_rejection_names_a_later_tone();
    test_tone_policy_rejection_outlives_its_carrier();
    test_tone_policy_check_is_no_scan_activity();
    test_chain_playing_follows_the_sink();
    test_auto_squelch_gates_each_sample();
    test_pcm_noise_squelch_gates_each_sample();
    test_pcm_noise_squelch_holds_the_backlog();
    return 0;
}

// NOLINTEND(bugprone-unsafe-functions,cert-msc24-c,cert-msc33-c)
