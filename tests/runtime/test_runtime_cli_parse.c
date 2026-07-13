// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/secret_redaction.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/crypto/ecdsa.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <inttypes.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void
noCarrier(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

static void
close_parse_outputs(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    if (opts->wav_out_f) {
        opts->wav_out_f = close_wav_file(opts->wav_out_f);
    }
    if (opts->wav_out_fR) {
        opts->wav_out_fR = close_wav_file(opts->wav_out_fR);
    }
    if (opts->wav_out_raw) {
        opts->wav_out_raw = close_wav_file(opts->wav_out_raw);
    }
}

static void
test_redirect_stdout_to_null(void) {
#if defined(_WIN32)
    (void)freopen("NUL", "w", stdout);
#else
    (void)freopen("/dev/null", "w", stdout);
#endif
}

static int
read_file_to_buffer(const char* path, char* out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';

    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    size_t nread = fread(out, 1, out_size - 1U, fp);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    out[nread] = '\0';
    fclose(fp);
    return 0;
}

static int
parse_args_capture_stderr(int argc, char** argv, dsd_opts* opts, dsd_state* state, int* argc_effective, int* exit_rc,
                          char* out, size_t out_size) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "runtime_cli_parse") != 0) {
        return DSD_PARSE_ERROR;
    }

    int rc = dsd_parse_args(argc, argv, opts, state, argc_effective, exit_rc);

    if (dsd_test_capture_stderr_end(&cap) != 0) {
        (void)remove(cap.path);
        return DSD_PARSE_ERROR;
    }
    if (read_file_to_buffer(cap.path, out, out_size) != 0) {
        (void)remove(cap.path);
        return DSD_PARSE_ERROR;
    }
    (void)remove(cap.path);
    return rc;
}

static int
test_help_returns_one_shot_and_does_not_exit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;

    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ONE_SHOT) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ONE_SHOT, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected exit_rc=0, got %d\n", exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_invalid_option_returns_error_and_does_not_exit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-H";
    char arg2[] = "ZZ";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = 0;

    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected exit_rc=1, got %d\n", exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_unknown_option_returns_error_and_does_not_exit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-?";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = 0;

    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected exit_rc=1, got %d\n", exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_frontend_terminal_option_sets_terminal_kind(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--frontend";
    char arg2[] = "terminal";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = 0;

    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        test_rc = 1;
    }
    if (opts->frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "expected --frontend terminal to set terminal kind, got frontend_kind=%d\n",
                    opts->frontend_kind);
        test_rc = 1;
    }
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
expect_numeric_parse_error(const char* option, const char* value) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char* argv[] = {arg0, (char*)option, (char*)value, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    int failed = (rc != DSD_PARSE_ERROR || exit_rc != 1);
    if (failed) {
        DSD_FPRINTF(stderr, "expected numeric parse error for %s %s, got rc=%d exit_rc=%d\n", option, value, rc,
                    exit_rc);
    }

    close_parse_outputs(opts);
    freeState(state);
    free(opts);
    free(state);
    return failed;
}

static int
test_numeric_options_reject_trailing_junk(void) {
    int rc = 0;
    rc |= expect_numeric_parse_error("--rdio-upload-timeout-ms", "5000junk");
    rc |= expect_numeric_parse_error("--rdio-upload-retries", "3junk");
    rc |= expect_numeric_parse_error("--input-volume", "4junk");
    rc |= expect_numeric_parse_error("--input-level-warn-db", "-12.5junk");
    rc |= expect_numeric_parse_error("-U", "4532junk");
    rc |= expect_numeric_parse_error("-s", "48000junk");
    rc |= expect_numeric_parse_error("-b", "12junk");
    rc |= expect_numeric_parse_error("-D", "4junk");
    rc |= expect_numeric_parse_error("-R", "12junk");
    rc |= expect_numeric_parse_error("-_", "12junk");
    rc |= expect_numeric_parse_error("-g", "-1");
    return rc;
}

static int
test_H_loads_aes256_key_for_both_slots(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-H";
    char arg2[] = "20029736A5D91042 C923EB0697484433 005EFC58A1905195 E28E9C7836AA2DB8";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const unsigned long long k1 = 0x20029736A5D91042ULL;
    const unsigned long long k2 = 0xC923EB0697484433ULL;
    const unsigned long long k3 = 0x005EFC58A1905195ULL;
    const unsigned long long k4 = 0xE28E9C7836AA2DB8ULL;

    if (state->A1[0] != k1 || state->A2[0] != k2 || state->A3[0] != k3 || state->A4[0] != k4 || state->A1[1] != k1
        || state->A2[1] != k2 || state->A3[1] != k3 || state->A4[1] != k4) {
        DSD_FPRINTF(stderr,
                    "expected A1..A4 to match key segments, got slot0=%016llX %016llX %016llX %016llX slot1=%016llX "
                    "%016llX %016llX %016llX\n",
                    (unsigned long long)state->A1[0], (unsigned long long)state->A2[0],
                    (unsigned long long)state->A3[0], (unsigned long long)state->A4[0],
                    (unsigned long long)state->A1[1], (unsigned long long)state->A2[1],
                    (unsigned long long)state->A3[1], (unsigned long long)state->A4[1]);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->aes_key_loaded[0] != 1 || state->aes_key_loaded[1] != 1) {
        DSD_FPRINTF(stderr, "expected aes_key_loaded[0..1]=1, got %d/%d\n", state->aes_key_loaded[0],
                    state->aes_key_loaded[1]);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (opts->dmr_mute_encL != 0 || opts->dmr_mute_encR != 0) {
        DSD_FPRINTF(stderr, "expected -H to unmute encrypted DMR audio, got L/R=%d/%d\n", opts->dmr_mute_encL,
                    opts->dmr_mute_encR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const uint8_t expect_bytes[32] = {
        0x20, 0x02, 0x97, 0x36, 0xA5, 0xD9, 0x10, 0x42, 0xC9, 0x23, 0xEB, 0x06, 0x97, 0x48, 0x44, 0x33,
        0x00, 0x5E, 0xFC, 0x58, 0xA1, 0x90, 0x51, 0x95, 0xE2, 0x8E, 0x9C, 0x78, 0x36, 0xAA, 0x2D, 0xB8,
    };
    if (memcmp(state->aes_key, expect_bytes, sizeof(expect_bytes)) != 0) {
        DSD_FPRINTF(stderr, "expected aes_key bytes to match key, got mismatch\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_H_zero_key_keeps_dmr_encrypted_audio_muted(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->dmr_mute_encL = 0;
    opts->dmr_mute_encR = 0;

    char arg0[] = "dsd-neo";
    char arg1[] = "-H";
    char arg2[] = "0000000000";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->K1 != 0ULL || state->K2 != 0ULL || state->K3 != 0ULL || state->K4 != 0ULL) {
        DSD_FPRINTF(stderr, "expected zero -H key, got K1..K4=%llX/%llX/%llX/%llX\n", state->K1, state->K2, state->K3,
                    state->K4);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (opts->dmr_mute_encL != 1 || opts->dmr_mute_encR != 1) {
        DSD_FPRINTF(stderr, "expected zero -H key to keep encrypted DMR muted, got L/R=%d/%d\n", opts->dmr_mute_encL,
                    opts->dmr_mute_encR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->keyloader != 0) {
        DSD_FPRINTF(stderr, "expected -H to disable keyloader, got %d\n", state->keyloader);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
expect_H_log_key_material(const char* key_arg, int show_keys, const char* expected, const char* unexpected) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg_show[] = "--show-keys";
    char arg_h[] = "-H";
    char key_buf[128];
    DSD_SNPRINTF(key_buf, sizeof key_buf, "%s", key_arg);
    key_buf[sizeof key_buf - 1U] = '\0';
    char* argv_show[] = {arg0, arg_show, arg_h, key_buf, NULL};
    char* argv_hidden[] = {arg0, arg_h, key_buf, NULL};
    char** argv = show_keys ? argv_show : argv_hidden;
    int argc = show_keys ? 4 : 3;

    char output[2048];
    int argc_effective = 0;
    int exit_rc = -1;
    int rc = parse_args_capture_stderr(argc, argv, opts, state, &argc_effective, &exit_rc, output, sizeof(output));

    int test_rc = 0;
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d for -H %s, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, key_buf, rc,
                    exit_rc);
        test_rc = 1;
    }
    if (expected != NULL && strstr(output, expected) == NULL) {
        DSD_FPRINTF(stderr, "expected -H log to contain \"%s\", got \"%s\"\n", expected, output);
        test_rc = 1;
    }
    if (unexpected != NULL && strstr(output, unexpected) != NULL) {
        DSD_FPRINTF(stderr, "expected -H log to hide \"%s\", got \"%s\"\n", unexpected, output);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_H_show_keys_log_reveals_key_material(void) {
    int rc = 0;
    rc |= expect_H_log_key_material("0123456789", 1, "0123456789", DSD_SECRET_REDACTED);
    rc |= expect_H_log_key_material("736B9A9C5645288B 243AD5CB8701EF8A", 1, "736B9A9C5645288B 243AD5CB8701EF8A",
                                    DSD_SECRET_REDACTED);
    rc |= expect_H_log_key_material("20029736A5D91042 C923EB0697484433 005EFC58A1905195 E28E9C7836AA2DB8", 1,
                                    "E28E9C7836AA2DB8", DSD_SECRET_REDACTED);
    rc |= expect_H_log_key_material("0123456789", 0, DSD_SECRET_REDACTED, "0123456789");
    return rc;
}

static int
test_b_loads_basic_privacy_key_and_unmutes_dmr(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-b";
    char arg2[] = "42";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->K != 42ULL || opts->dmr_mute_encL != 0 || opts->dmr_mute_encR != 0) {
        DSD_FPRINTF(stderr, "expected -b 42 to set K=42 and unmute DMR, got K=%llu L/R=%d/%d\n", state->K,
                    opts->dmr_mute_encL, opts->dmr_mute_encR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_b_zero_key_keeps_dmr_encrypted_audio_muted(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->dmr_mute_encL = 0;
    opts->dmr_mute_encR = 0;

    char arg0[] = "dsd-neo";
    char arg1[] = "-b";
    char arg2[] = "0";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->K != 0ULL || opts->dmr_mute_encL != 1 || opts->dmr_mute_encR != 1) {
        DSD_FPRINTF(stderr, "expected -b 0 to set K=0 and keep DMR muted, got K=%llu L/R=%d/%d\n", state->K,
                    opts->dmr_mute_encL, opts->dmr_mute_encR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_b_clamps_to_basic_privacy_table_max(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-b";
    char arg2[] = "999";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->K != 255ULL || opts->dmr_mute_encL != 0 || opts->dmr_mute_encR != 0) {
        DSD_FPRINTF(stderr, "expected -b 999 to clamp K=255 and unmute DMR, got K=%llu L/R=%d/%d\n", state->K,
                    opts->dmr_mute_encL, opts->dmr_mute_encR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_2_loads_tyt_basic_privacy_key_and_truncates_to_16_bits(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-2";
    char arg2[] = "12345";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->tyt_bp != 1 || state->H != 0x2345ULL) {
        DSD_FPRINTF(stderr, "expected -2 12345 to enable TYT BP and truncate H=2345, got tyt_bp=%d H=%llX\n",
                    state->tyt_bp, state->H);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_1_loads_rc4_key_for_both_slots_and_allows_spaces(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-1";
    char arg2[] = "12 34 56 78 91";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const unsigned long long expect = 0x1234567891ULL;
    if (state->R != expect || state->RR != expect) {
        DSD_FPRINTF(stderr, "expected R/RR=%010llX, got %010llX/%010llX\n", expect, state->R, state->RR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_1_loads_rc4_key_allows_0x_prefix(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-1";
    char arg2[] = "0x1234567891";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const unsigned long long expect = 0x1234567891ULL;
    if (state->R != expect || state->RR != expect) {
        DSD_FPRINTF(stderr, "expected R/RR=%010llX, got %010llX/%010llX\n", expect, state->R, state->RR);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_R_loads_nxdn_scrambler_key_and_disables_keyloader(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);
    state->keyloader = 1;

    char arg0[] = "dsd-neo";
    char arg1[] = "-R";
    char arg2[] = "40000";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->R != 0x7FFFULL || state->keyloader != 0 || opts->symbol_out_file[0] != '\0') {
        DSD_FPRINTF(stderr, "expected -R to clamp R and disable keyloader, got R=%llX keyloader=%d symbol='%s'\n",
                    state->R, state->keyloader, opts->symbol_out_file);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
expect_pn95_seed_arg(const char* value, uint16_t want) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-_";
    char arg2[32];
    DSD_SNPRINTF(arg2, sizeof(arg2), "%s", value);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int parse_rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    int failed = 0;
    if (parse_rc != DSD_PARSE_CONTINUE || state->nxdn_pn95_seed != want) {
        DSD_FPRINTF(stderr, "expected -_ %s to set seed %u, got rc=%d exit_rc=%d seed=%u\n", value, (unsigned)want,
                    parse_rc, exit_rc, (unsigned)state->nxdn_pn95_seed);
        failed = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return failed;
}

static int
test_nxdn_pn95_seed_option_matches_reference_bounds(void) {
    int rc = 0;
    rc |= expect_pn95_seed_arg("1", 1U);
    rc |= expect_pn95_seed_arg("0", 228U);
    rc |= expect_pn95_seed_arg("512", 511U);
    return rc;
}

static const char*
test_tmp_dir(void) {
    const char* dir = getenv("TMPDIR");
#if DSD_PLATFORM_WIN_NATIVE
    if (!dir || !*dir) {
        dir = getenv("TEMP");
    }
    if (!dir || !*dir) {
        dir = getenv("TMP");
    }
#else
    if (!dir || !*dir) {
        dir = "/tmp";
    }
#endif
    if (!dir || !*dir) {
        dir = ".";
    }
    return dir;
}

static char
test_path_sep(void) {
#if DSD_PLATFORM_WIN_NATIVE
    return '\\';
#else
    return '/';
#endif
}

static int
test_create_temp_ini_with_contents(const char* contents, char* out_path, size_t out_path_size) {
    if (!contents || !out_path || out_path_size == 0) {
        return -1;
    }

    char tmpl[1024];
    DSD_SNPRINTF(tmpl, sizeof tmpl, "%s", "dsdneo_bootstrap_XXXXXX");

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (DSD_SNPRINTF(out_path, out_path_size, "%s.ini", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = dsd_fopen_private(out_path, "w");
    if (!fp) {
        (void)remove(out_path);
        return -1;
    }

    fputs(contents, fp);
    fclose(fp);
    return 0;
}

static int
test_create_temp_ini_in_tmpdir_with_contents(const char* contents, char* out_path, size_t out_path_size) {
    if (!contents || !out_path || out_path_size == 0) {
        return -1;
    }

    char tmpl[1024];
    if (dsd_test_path_join(tmpl, sizeof tmpl, test_tmp_dir(), "dsdneo_bootstrap_abs_XXXXXX") != 0) {
        return -1;
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (DSD_SNPRINTF(out_path, out_path_size, "%s.ini", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = dsd_fopen_private(out_path, "w");
    if (!fp) {
        (void)remove(out_path);
        return -1;
    }

    fputs(contents, fp);
    fclose(fp);
    return 0;
}

static int
test_create_temp_ini(char* out_path, size_t out_path_size) {
    if (!out_path || out_path_size == 0) {
        return -1;
    }
    return test_create_temp_ini_with_contents("[input]\n"
                                              "source = \"rtl\"\n"
                                              "rtl_device = 0\n"
                                              "rtl_freq = \"100000000\"\n"
                                              "\n"
                                              "[trunking]\n"
                                              "enabled = true\n",
                                              out_path, out_path_size);
}

static int
test_create_temp_csv_with_contents(const char* contents, char* out_path, size_t out_path_size) {
    if (!contents || !out_path || out_path_size == 0) {
        return -1;
    }

    char tmpl[1024];
    DSD_SNPRINTF(tmpl, sizeof tmpl, "%s", "dsdneo_bootstrap_csv_XXXXXX");

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (DSD_SNPRINTF(out_path, out_path_size, "%s.csv", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = dsd_fopen_private(out_path, "w");
    if (!fp) {
        (void)remove(out_path);
        return -1;
    }

    fputs(contents, fp);
    fclose(fp);
    return 0;
}

static int
test_create_temp_raw_pcm_file(const char* prefix, const short* samples, size_t sample_count, const char* suffix,
                              char* out_path, size_t out_path_size) {
    if (!prefix || !samples || sample_count == 0 || !suffix || !out_path || out_path_size == 0) {
        return -1;
    }

    const char sep = test_path_sep();
    const char* tdir = test_tmp_dir();

    char tmpl[1024];
    size_t tdir_len = strlen(tdir);
    if (tdir_len > 0 && (tdir[tdir_len - 1] == '/' || tdir[tdir_len - 1] == '\\')) {
        DSD_SNPRINTF(tmpl, sizeof tmpl, "%s%s_XXXXXX", tdir, prefix);
    } else {
        DSD_SNPRINTF(tmpl, sizeof tmpl, "%s%c%s_XXXXXX", tdir, sep, prefix);
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (DSD_SNPRINTF(out_path, out_path_size, "%s%s", tmpl, suffix) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = dsd_fopen_private(out_path, "wb");
    if (!fp) {
        (void)remove(out_path);
        return -1;
    }

    size_t nwritten = fwrite(samples, sizeof(samples[0]), sample_count, fp);
    fclose(fp);
    if (nwritten != sample_count) {
        (void)remove(out_path);
        return -1;
    }

    return 0;
}

static int
test_create_temp_vertex_ks_csv(char* out_path, size_t out_path_size, int malformed) {
    if (!out_path || out_path_size == 0) {
        return -1;
    }

    char tmpl[1024];
    DSD_SNPRINTF(tmpl, sizeof tmpl, "%s", "dsdneo_vertex_ks_XXXXXX");

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (DSD_SNPRINTF(out_path, out_path_size, "%s.csv", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = dsd_fopen_private(out_path, "w");
    if (!fp) {
        (void)remove(out_path);
        return -1;
    }

    if (malformed) {
        fputs("key_hex,keystream_spec\n"
              "1234567891,broken\n",
              fp);
    } else {
        fputs("key_hex,keystream_spec\n"
              "1234567891,8:F0:2:3\n",
              fp);
    }

    fclose(fp);
    return 0;
}

static int
test_create_temp_iq_fixture(char* out_metadata_path, size_t out_metadata_path_size, char* out_data_path,
                            size_t out_data_path_size) {
    if (!out_metadata_path || out_metadata_path_size == 0 || !out_data_path || out_data_path_size == 0) {
        return -1;
    }

    /*
     * Create the binary IQ payload first, then write metadata that references it
     * by basename. This keeps fixture paths portable while still exercising the
     * replay loader's relative-path resolution.
     */
    const char sep = test_path_sep();
    const char* tdir = test_tmp_dir();

    char tmpl[1024];
    size_t tdir_len = strlen(tdir);
    if (tdir_len > 0 && (tdir[tdir_len - 1] == '/' || tdir[tdir_len - 1] == '\\')) {
        DSD_SNPRINTF(tmpl, sizeof tmpl, "%sdsdneo_iq_cli_XXXXXX", tdir);
    } else {
        DSD_SNPRINTF(tmpl, sizeof tmpl, "%s%c%s", tdir, sep, "dsdneo_iq_cli_XXXXXX");
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    char data_path[1024];
    if (DSD_SNPRINTF(data_path, sizeof data_path, "%s.iq", tmpl) >= (int)sizeof(data_path)) {
        (void)remove(tmpl);
        return -1;
    }
    if (rename(tmpl, data_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* data_fp = dsd_fopen_private(data_path, "wb");
    if (!data_fp) {
        (void)remove(data_path);
        return -1;
    }
    static const unsigned char kData[16] = {0x7f, 0x80, 0x10, 0xf0, 0x20, 0xe0, 0x30, 0xd0,
                                            0x40, 0xc0, 0x50, 0xb0, 0x60, 0xa0, 0x70, 0x90};
    if (fwrite(kData, 1, sizeof(kData), data_fp) != sizeof(kData)) {
        fclose(data_fp);
        (void)remove(data_path);
        return -1;
    }
    fclose(data_fp);

    char metadata_path[1024];
    if (DSD_SNPRINTF(metadata_path, sizeof metadata_path, "%s.json", data_path) >= (int)sizeof(metadata_path)) {
        (void)remove(data_path);
        return -1;
    }
    FILE* meta_fp = dsd_fopen_private(metadata_path, "w");
    if (!meta_fp) {
        (void)remove(data_path);
        return -1;
    }

    const char* data_file = strrchr(data_path, '/');
    const char* data_file_win = strrchr(data_path, '\\');
    if (data_file_win && (!data_file || data_file_win > data_file)) {
        data_file = data_file_win;
    }
    data_file = data_file ? (data_file + 1) : data_path;

    // The bootstrap tests only need a small valid v1 capture descriptor.
    int n = DSD_FPRINTF(meta_fp,
                        "{\n"
                        "  \"format\": \"dsd-neo-iq\",\n"
                        "  \"version\": 1,\n"
                        "  \"sample_format\": \"cu8\",\n"
                        "  \"iq_order\": \"IQ\",\n"
                        "  \"endianness\": \"none\",\n"
                        "  \"capture_stage\": \"post_mute_pre_widen\",\n"
                        "  \"sample_rate_hz\": 1536000,\n"
                        "  \"center_frequency_hz\": 851375000,\n"
                        "  \"capture_center_frequency_hz\": 851759000,\n"
                        "  \"ppm\": 0,\n"
                        "  \"tuner_gain_tenth_db\": 220,\n"
                        "  \"rtl_dsp_bw_khz\": 48,\n"
                        "  \"base_decimation\": 32,\n"
                        "  \"post_downsample\": 1,\n"
                        "  \"demod_rate_hz\": 48000,\n"
                        "  \"offset_tuning_enabled\": false,\n"
                        "  \"fs4_shift_enabled\": true,\n"
                        "  \"combine_rotate_enabled\": true,\n"
                        "  \"muted_bytes_excluded\": true,\n"
                        "  \"contains_retunes\": false,\n"
                        "  \"capture_retune_count\": 0,\n"
                        "  \"source_backend\": \"rtl\",\n"
                        "  \"source_args\": \"dev=0\",\n"
                        "  \"capture_started_utc\": \"2026-01-01T00:00:00Z\",\n"
                        "  \"data_file\": \"%s\",\n"
                        "  \"data_bytes\": 16,\n"
                        "  \"capture_drops\": 0,\n"
                        "  \"capture_drop_blocks\": 0,\n"
                        "  \"input_ring_drops\": 0,\n"
                        "  \"notes\": \"\"\n"
                        "}\n",
                        data_file);
    fclose(meta_fp);
    if (n <= 0) {
        (void)remove(metadata_path);
        (void)remove(data_path);
        return -1;
    }

    DSD_SNPRINTF(out_data_path, out_data_path_size, "%s", data_path);
    DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path);
    return 0;
}

static int
test_bootstrap_treats_lone_ini_as_config(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    // Make test deterministic: avoid env-config interference and skip bootstrap UI.
    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char cfg_path[1024];
    if (test_create_temp_ini(cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char* argv[] = {arg0, cfg_path, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(2, argv, opts, state, &argc_effective, &exit_rc);

    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    // Ensure it behaves like "--config <path>" by compacting the effective argc down to argv[0] only.
    if (argc_effective != 1) {
        DSD_FPRINTF(stderr, "expected argc_effective=1, got %d\n", argc_effective);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (!state->config_autosave_enabled || strcmp(state->config_autosave_path, cfg_path) != 0) {
        DSD_FPRINTF(stderr, "expected config_autosave_path=%s, got %s (enabled=%d)\n", cfg_path,
                    state->config_autosave_path, state->config_autosave_enabled);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (opts->trunk_enable != 1) {
        DSD_FPRINTF(stderr, "expected trunking enabled from config, got trunk_enable=%d\n", opts->trunk_enable);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (strncmp(opts->audio_in_dev, "rtl:", 4) != 0) {
        DSD_FPRINTF(stderr, "expected RTL input from config, got audio_in_dev=%s\n", opts->audio_in_dev);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_bootstrap_accepts_explicit_config_path_outside_cwd(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[input]\n"
                                                     "source = \"rtl\"\n"
                                                     "rtl_device = 0\n"
                                                     "rtl_freq = \"100000000\"\n"
                                                     "\n"
                                                     "[trunking]\n"
                                                     "enabled = true\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        DSD_FPRINTF(stderr, "failed to create external temp ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(3, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected external config rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc,
                    exit_rc);
        test_rc = 1;
    }
    if (!state->config_autosave_enabled || strcmp(state->config_autosave_path, cfg_path) != 0) {
        DSD_FPRINTF(stderr, "expected external config_autosave_path=%s, got %s (enabled=%d)\n", cfg_path,
                    state->config_autosave_path, state->config_autosave_enabled);
        test_rc = 1;
    }
    if (opts->trunk_enable != 1) {
        DSD_FPRINTF(stderr, "expected trunking from external config, got trunk_enable=%d\n", opts->trunk_enable);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_config_trunking_preserves_terminal_frontend(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[input]\n"
                                                     "source = \"rtl\"\n"
                                                     "rtl_device = 0\n"
                                                     "rtl_freq = \"100000000\"\n"
                                                     "\n"
                                                     "[trunking]\n"
                                                     "enabled = true\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        DSD_FPRINTF(stderr, "failed to create external temp ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--frontend";
    char arg4[] = "terminal";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected terminal frontend bootstrap continue, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (argc_effective != 1 || !state->cli_argv) {
        DSD_FPRINTF(stderr, "expected compacted CLI to consume --frontend terminal, argc=%d\n", argc_effective);
        test_rc = 1;
    }
    if (opts->trunk_enable != 1) {
        DSD_FPRINTF(stderr, "expected config trunking preserved with --frontend terminal, got trunk_enable=%d\n",
                    opts->trunk_enable);
        test_rc = 1;
    }
    if (opts->frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "expected --frontend terminal to set terminal kind, got frontend_kind=%d\n",
                    opts->frontend_kind);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_missing_explicit_config_keeps_autosave_path(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("", cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp ini for missing-path test\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    (void)remove(cfg_path);

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(3, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected missing explicit config to continue, got rc=%d (exit_rc=%d)\n", rc, exit_rc);
        test_rc = 1;
    }
    if (!state->config_autosave_enabled || strcmp(state->config_autosave_path, cfg_path) != 0) {
        DSD_FPRINTF(stderr, "expected missing config_autosave_path=%s, got %s (enabled=%d)\n", cfg_path,
                    state->config_autosave_path, state->config_autosave_enabled);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_rejects_too_long_explicit_config_path(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char too_long_path[3072];
    DSD_MEMSET(too_long_path, 'a', sizeof too_long_path);
    too_long_path[sizeof too_long_path - 1] = '\0';

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char* argv[] = {arg0, arg1, too_long_path, NULL};

    int argc_effective = 99;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(3, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected too-long config path error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (state->config_autosave_enabled || state->config_autosave_path[0] != '\0') {
        DSD_FPRINTF(stderr, "expected invalid config path to leave autosave disabled, got enabled=%d path=%s\n",
                    state->config_autosave_enabled, state->config_autosave_path);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_guard_rejects_invalid_arguments(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char* argv[] = {arg0, NULL};

    int exit_rc = -1;
    int test_rc = 0;
    int rc = dsd_runtime_bootstrap(1, argv, NULL, state, NULL, &exit_rc);
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected NULL opts guard error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    exit_rc = -1;
    rc = dsd_runtime_bootstrap(1, argv, opts, NULL, NULL, &exit_rc);
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected NULL state guard error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    exit_rc = -1;
    rc = dsd_runtime_bootstrap(-1, argv, opts, state, NULL, &exit_rc);
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected negative argc guard error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    exit_rc = -1;
    rc = dsd_runtime_bootstrap(1, NULL, opts, state, NULL, &exit_rc);
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected NULL argv guard error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_validate_config_accepts_external_path(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[input]\n"
                                                     "source = \"pulse\"\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--validate-config";
    char arg2[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(3, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected external validate to exit 0, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_validate_config_reports_trunk_scan_diagnostics(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[trunk_scan]\n"
                                                     "enabled = true\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--validate-config";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected validate-config diagnostics exit, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_validate_config_rejects_invalid_included_versions(void) {
    struct persisted_config_case {
        const char* label;
        const char* contents;
    } cases[] = {
        {"unsupported included version", "version = 2\n"},
        {"non-integer included version", "version = old\n"},
    };

    int test_rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char included_path[1024];
        if (test_create_temp_ini_in_tmpdir_with_contents(cases[i].contents, included_path, sizeof included_path) != 0) {
            return 1;
        }

        char root_ini[1152];
        DSD_SNPRINTF(root_ini, sizeof root_ini, "include = \"%s\"\n", included_path);
        char root_path[1024];
        if (test_create_temp_ini_in_tmpdir_with_contents(root_ini, root_path, sizeof root_path) != 0) {
            (void)remove(included_path);
            return 1;
        }

        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            (void)remove(root_path);
            (void)remove(included_path);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }
        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[] = "--validate-config";
        char arg2[1024];
        DSD_SNPRINTF(arg2, sizeof arg2, "%s", root_path);
        char* argv[] = {arg0, arg1, arg2, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_runtime_bootstrap(3, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 1) {
            DSD_FPRINTF(stderr, "%s should exit with validation failure, got rc=%d exit_rc=%d\n", cases[i].label, rc,
                        exit_rc);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
        (void)remove(root_path);
        (void)remove(included_path);
    }
    return test_rc;
}

static int
test_bootstrap_validate_config_strict_warning_exits_two(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[input]\n"
                                                     "source = \"pulse\"\n"
                                                     "unknown_key = true\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--validate-config";
    char arg2[1024];
    char arg3[] = "--strict-config";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 2) {
        DSD_FPRINTF(stderr, "expected strict warning validate exit 2, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_list_profiles_accepts_external_config_path(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("[profile.demo]\n"
                                                     "mode.decode = \"dmr\"\n",
                                                     cfg_path, sizeof cfg_path)
        != 0) {
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--list-profiles";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected external list-profiles to exit 0, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_list_profiles_reports_empty_config(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char cfg_path[1024];
    if (test_create_temp_ini_in_tmpdir_with_contents("", cfg_path, sizeof cfg_path) != 0) {
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--list-profiles";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    test_redirect_stdout_to_null();
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected empty profile list to exit 0, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_print_config_normalizes_soapy_shorthand(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    // Keep bootstrap deterministic and isolate from host configuration.
    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char arg0[] = "dsd-neo";
    char arg1[] = "-i";
    char arg2[] = "soapy:driver=airspy,serial=ABC123:851.375M:22:-2:24:0:2";
    char arg3[] = "--print-config";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    test_redirect_stdout_to_null();
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected rc=%d and exit_rc=0, got rc=%d exit_rc=%d\n", DSD_BOOTSTRAP_EXIT, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "expected normalized soapy args, got audio_in_dev=%s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 22 || opts->rtlsdr_ppm_error != -2
        || opts->rtl_dsp_bw_khz != 24 || opts->rtl_squelch_level != 0.0 || opts->rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "unexpected normalized tuning values freq=%u gain=%d ppm=%d bw=%d sql=%.6f vol=%d\n",
                    opts->rtlsdr_center_freq, opts->rtl_gain_value, opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz,
                    opts->rtl_squelch_level, opts->rtl_volume_multiplier);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_profile_preserves_trunking_with_ncurses_cli(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.p25_trunk]\n"
                             "input.source = \"rtl\"\n"
                             "input.rtl_device = 0\n"
                             "input.rtl_freq = \"100000000\"\n"
                             "trunking.enabled = true\n"
                             "trunk_scan.enabled = true\n"
                             "trunk_scan.targets_csv = \"targets.csv\"\n"
                             "trunk_scan.idle_dwell_ms = 500\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp profile ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--profile";
    char arg4[] = "p25_trunk";
    char arg5[] = "--frontend";
    char arg6[] = "terminal";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(7, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->trunk_enable != 1) {
        DSD_FPRINTF(stderr, "expected profiled trunking to stay enabled, got trunk_enable=%d\n", opts->trunk_enable);
        test_rc = 1;
    }
    if (opts->trunk_scan_enabled != 1 || strcmp(opts->trunk_scan_targets_csv, "targets.csv") != 0) {
        DSD_FPRINTF(stderr, "expected profiled trunk scan to stay enabled, got enabled=%d targets=%s\n",
                    opts->trunk_scan_enabled, opts->trunk_scan_targets_csv);
        test_rc = 1;
    }
    if (opts->frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "expected --frontend terminal to remain applied, got frontend_kind=%d\n",
                    opts->frontend_kind);
        test_rc = 1;
    }
    if (strncmp(opts->audio_in_dev, "rtl:", 4) != 0) {
        DSD_FPRINTF(stderr, "expected profile RTL input, got audio_in_dev=%s\n", opts->audio_in_dev);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_inherited_trunk_scan_preserves_ui_only_short_options(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"targets.csv\"\n"
                             "idle_dwell_ms = 500\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--frontend";
    char arg4[] = "terminal";
    char arg5[] = "-v";
    char arg6[] = "3";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(7, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected UI-only CLI options to continue, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (opts->trunk_scan_enabled != 1 || strcmp(opts->trunk_scan_targets_csv, "targets.csv") != 0) {
        DSD_FPRINTF(stderr, "expected inherited trunk scan to stay enabled, got enabled=%d targets=%s\n",
                    opts->trunk_scan_enabled, opts->trunk_scan_targets_csv);
        test_rc = 1;
    }
    if (opts->frontend_kind != DSD_FRONTEND_TERMINAL) {
        DSD_FPRINTF(stderr, "expected --frontend terminal to remain applied, got frontend_kind=%d\n",
                    opts->frontend_kind);
        test_rc = 1;
    }
    if (opts->use_pbf != 1 || opts->use_lpf != 1 || opts->use_hpf != 0 || opts->use_hpf_d != 0) {
        DSD_FPRINTF(stderr, "expected -v 3 to apply PBF/LPF only, got pbf=%d lpf=%d hpf=%d hpfd=%d\n", opts->use_pbf,
                    opts->use_lpf, opts->use_hpf, opts->use_hpf_d);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_inherited_trunk_scan_allows_cli_channel_map(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"targets.csv\"\n"
                             "idle_dwell_ms = 500\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    static const char* chan_csv = "ChannelNumber(dec),frequency(Hz),note\n"
                                  "99,851012500,test\n";
    char chan_path[1024];
    if (test_create_temp_csv_with_contents(chan_csv, chan_path, sizeof chan_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp channel CSV\n");
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-C";
    char arg4[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    DSD_SNPRINTF(arg4, sizeof arg4, "%s", chan_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected inherited trunk scan to allow CLI -C, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (opts->trunk_scan_enabled != 0) {
        DSD_FPRINTF(stderr, "expected inherited trunk scan to be disabled for CLI run, got %d\n",
                    opts->trunk_scan_enabled);
        test_rc = 1;
    }
    if (strcmp(opts->chan_in_file, chan_path) != 0 || state->trunk_chan_map[99] != 851012500) {
        DSD_FPRINTF(stderr, "expected CLI channel map import, got file=%s freq=%ld\n", opts->chan_in_file,
                    state->trunk_chan_map[99]);
        test_rc = 1;
    }

    (void)remove(chan_path);
    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_inherited_trunk_scan_disables_for_positional_input(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"targets.csv\"\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "input.amb";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected positional input bootstrap continue, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (opts->trunk_scan_enabled != 0) {
        DSD_FPRINTF(stderr, "expected positional input to disable inherited trunk scan, got %d\n",
                    opts->trunk_scan_enabled);
        test_rc = 1;
    }
    if (argc_effective != 2 || strcmp(argv[1], "input.amb") != 0) {
        DSD_FPRINTF(stderr, "expected positional playback arg to survive compaction, argc=%d arg1=%s\n", argc_effective,
                    argc_effective > 1 ? argv[1] : "(missing)");
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_inherited_trunk_scan_disables_for_long_only_runtime_mode(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"targets.csv\"\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temporary IQ fixture\n");
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--iq-replay";
    char arg4[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    DSD_SNPRINTF(arg4, sizeof arg4, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (opts->trunk_scan_enabled != 0) {
        DSD_FPRINTF(stderr, "expected inherited trunk scan disabled for long-only runtime args, got %d\n",
                    opts->trunk_scan_enabled);
        test_rc = 1;
    }
#ifdef USE_RADIO
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0 || !opts->iq_replay_requested) {
        DSD_FPRINTF(stderr, "expected IQ replay bootstrap continue, got rc=%d exit_rc=%d requested=%d\n", rc, exit_rc,
                    opts->iq_replay_requested);
        test_rc = 1;
    }
#else
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected no-radio IQ replay bootstrap error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
#endif

    (void)remove(metadata_path);
    (void)remove(data_path);
    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_inherited_trunk_scan_preserves_timing_overrides(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"targets.csv\"\n"
                             "idle_dwell_ms = 3000\n"
                             "activity_hold_ms = 1200\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--trunk-scan-dwell-ms";
    char arg4[] = "500";
    char arg5[] = "--trunk-scan-activity-hold-ms=800";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(6, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected timing-only trunk scan override to continue, got rc=%d exit_rc=%d\n", rc,
                    exit_rc);
        test_rc = 1;
    }
    if (opts->trunk_scan_enabled != 1 || strcmp(opts->trunk_scan_targets_csv, "targets.csv") != 0
        || opts->trunk_scan_idle_dwell_ms != 500 || opts->trunk_scan_activity_hold_ms != 800) {
        DSD_FPRINTF(stderr, "expected trunk scan timing override, got enabled=%d targets=%s dwell=%d hold=%d\n",
                    opts->trunk_scan_enabled, opts->trunk_scan_targets_csv, opts->trunk_scan_idle_dwell_ms,
                    opts->trunk_scan_activity_hold_ms);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_config_one_shots_skip_trunk_scan_runtime_validation(void) {
    static const char* ini = "[trunk_scan]\n"
                             "enabled = true\n"
                             "\n"
                             "[profile.demo]\n"
                             "mode.decode = \"dmr\"\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp trunk scan ini\n");
        return 1;
    }

    const char* one_shots[] = {"--print-config", "--list-profiles", "--dump-config-template"};
    int test_rc = 0;
    test_redirect_stdout_to_null();
    for (size_t i = 0; i < sizeof(one_shots) / sizeof(one_shots[0]); i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            test_rc = 1;
            break;
        }

        initOpts(opts);
        initState(state);

        (void)dsd_unsetenv("DSD_NEO_CONFIG");
        (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

        char arg0[] = "dsd-neo";
        char arg1[] = "--config";
        char arg2[1024];
        char arg3[64];
        DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
        DSD_SNPRINTF(arg3, sizeof arg3, "%s", one_shots[i]);
        char* argv[] = {arg0, arg1, arg2, arg3, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_BOOTSTRAP_EXIT || exit_rc != 0) {
            DSD_FPRINTF(stderr, "expected %s to bypass trunk-scan runtime validation, got rc=%d exit_rc=%d\n",
                        one_shots[i], rc, exit_rc);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    (void)remove(cfg_path);
    return test_rc;
}

static int
test_bootstrap_profile_disables_autosave(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[profile.p25_trunk]\n"
                             "input.source = \"rtl\"\n"
                             "input.rtl_device = 0\n"
                             "input.rtl_freq = \"100000000\"\n"
                             "trunking.enabled = true\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp profile ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--profile";
    char arg4[] = "p25_trunk";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (state->config_autosave_enabled != 0) {
        DSD_FPRINTF(stderr, "expected autosave disabled for profiled config, got enabled=%d\n",
                    state->config_autosave_enabled);
        test_rc = 1;
    }
    if (strcmp(state->config_autosave_path, cfg_path) != 0) {
        DSD_FPRINTF(stderr, "expected profiled config path retained as %s, got %s\n", cfg_path,
                    state->config_autosave_path);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_missing_profile_errors_without_applying_config_or_cli(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 7\n"
                             "rtl_freq = \"851.0125M\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = true\n"
                             "\n"
                             "[profile.valid]\n"
                             "output.frontend = terminal\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp profile ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "--profile";
    char arg4[] = "missing";
    char arg5[] = "--frontend";
    char arg6[] = "terminal";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, NULL};

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "runtime_cli_missing_profile") != 0) {
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(7, argv, opts, state, &argc_effective, &exit_rc);

    int capture_failed = dsd_test_capture_stderr_end(&cap);
    char stderr_buf[2048];
    stderr_buf[0] = '\0';
    if (!capture_failed) {
        capture_failed = read_file_to_buffer(cap.path, stderr_buf, sizeof stderr_buf);
    }
    (void)remove(cap.path);

    int test_rc = 0;
    if (capture_failed) {
        DSD_FPRINTF(stderr, "failed to capture missing-profile diagnostics\n");
        test_rc = 1;
    }
    if (rc != DSD_BOOTSTRAP_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected missing profile bootstrap error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (!strstr(stderr_buf, "Profile 'missing' not found")) {
        DSD_FPRINTF(stderr, "expected missing profile diagnostic, got:\n%s\n", stderr_buf);
        test_rc = 1;
    }
    if (opts->frontend_kind != DSD_FRONTEND_NONE) {
        DSD_FPRINTF(stderr, "missing profile should stop before CLI frontend applies, got frontend_kind=%d\n",
                    opts->frontend_kind);
        test_rc = 1;
    }
    if (strncmp(opts->audio_in_dev, "rtl:", 4) == 0 || opts->trunk_enable != 0) {
        DSD_FPRINTF(stderr, "missing profile should not apply config: input=%s trunk=%d\n", opts->audio_in_dev,
                    opts->trunk_enable);
        test_rc = 1;
    }
    if (state->cli_argc_effective != 0) {
        DSD_FPRINTF(stderr, "missing profile should stop before recording effective CLI args, got %d\n",
                    state->cli_argc_effective);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_cli_call_alert_restores_all_config_filtered_events(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[alerts]\n"
                             "enabled = false\n"
                             "voice_end = false\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp alerts ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-a";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->call_alert != 1) {
        DSD_FPRINTF(stderr, "expected -a to enable call alerts, got %d\n", opts->call_alert);
        test_rc = 1;
    }
    if (opts->call_alert_events != DSD_CALL_ALERT_EVENT_ALL) {
        DSD_FPRINTF(stderr, "expected -a to restore all alert events, got %u\n", (unsigned)opts->call_alert_events);
        test_rc = 1;
    }
    if (!dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_START)
        || !dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)
        || !dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_DATA)) {
        DSD_FPRINTF(stderr, "expected -a to enable start, end, and data alert events\n");
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_r_playback_optind_is_first_file_regardless_of_option_order(void) {
    int test_rc = 0;
    char wav_path_a[1024];
    char wav_path_b[1024];
    const char* tdir = test_tmp_dir();
    const char sep = test_path_sep();
    if (DSD_SNPRINTF(wav_path_a, sizeof wav_path_a, "%s%c%s", tdir, sep, "dsdneo_cli_parse_a.wav")
            >= (int)sizeof(wav_path_a)
        || DSD_SNPRINTF(wav_path_b, sizeof wav_path_b, "%s%c%s", tdir, sep, "dsdneo_cli_parse_b.wav")
               >= (int)sizeof(wav_path_b)) {
        DSD_FPRINTF(stderr, "temp path too long\n");
        return 1;
    }
    (void)remove(wav_path_a);
    (void)remove(wav_path_b);

    /*
     * Exercise both orderings around -w because the compacted argv seen by
     * short-option parsing must leave optind on the playback filename. Each
     * block owns a fresh opts/state pair so parser side effects cannot leak.
     */
    {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }
        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[] = "-r";
        char arg2[] = "play_first.amb";
        char arg3[] = "-w";
        char* argv[] = {arg0, arg1, arg2, arg3, wav_path_a, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_CONTINUE || opts->playfiles != 1) {
            DSD_FPRINTF(stderr, "expected parse continue with playfiles=1, got rc=%d playfiles=%d exit_rc=%d\n", rc,
                        opts->playfiles, exit_rc);
            test_rc = 1;
        } else if (state->optind < 1 || state->optind >= argc_effective) {
            DSD_FPRINTF(stderr, "invalid optind for playback: optind=%d argc_effective=%d\n", state->optind,
                        argc_effective);
            test_rc = 1;
        } else if (strcmp(argv[state->optind], "play_first.amb") != 0) {
            DSD_FPRINTF(stderr, "expected first playback arg to be play_first.amb, got %s\n", argv[state->optind]);
            test_rc = 1;
        }

        close_parse_outputs(opts);
        freeState(state);
        free(opts);
        free(state);
    }

    {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }
        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[] = "-w";
        char* arg2 = wav_path_b;
        char arg3[] = "-r";
        char arg4[] = "play_last.amb";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_CONTINUE || opts->playfiles != 1) {
            DSD_FPRINTF(stderr, "expected parse continue with playfiles=1, got rc=%d playfiles=%d exit_rc=%d\n", rc,
                        opts->playfiles, exit_rc);
            test_rc = 1;
        } else if (state->optind < 1 || state->optind >= argc_effective) {
            DSD_FPRINTF(stderr, "invalid optind for playback: optind=%d argc_effective=%d\n", state->optind,
                        argc_effective);
            test_rc = 1;
        } else if (strcmp(argv[state->optind], "play_last.amb") != 0) {
            DSD_FPRINTF(stderr, "expected first playback arg to be play_last.amb, got %s\n", argv[state->optind]);
            test_rc = 1;
        }

        close_parse_outputs(opts);
        freeState(state);
        free(opts);
        free(state);
    }

    (void)remove(wav_path_a);
    (void)remove(wav_path_b);

    return test_rc;
}

static int
test_open_mbe_missing_file_leaves_stream_null(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char missing_path[1024];
    const char* tdir = test_tmp_dir();
    const char sep = test_path_sep();
    if (DSD_SNPRINTF(missing_path, sizeof missing_path, "%s%c%s", tdir, sep, "dsdneo_missing_playback_input.amb")
        >= (int)sizeof(missing_path)) {
        DSD_FPRINTF(stderr, "temp path too long\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    (void)remove(missing_path);

    DSD_SNPRINTF(opts->mbe_in_file, sizeof opts->mbe_in_file, "%s", missing_path);
    state->mbe_file_type = 7;
    openMbeInFile(opts, state);
    if (opts->mbe_in_f != NULL) {
        DSD_FPRINTF(stderr, "expected missing input open to leave mbe_in_f NULL\n");
        fclose(opts->mbe_in_f);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->mbe_file_type != -1) {
        DSD_FPRINTF(stderr, "expected mbe_file_type=-1 on missing input, got %d\n", state->mbe_file_type);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_sdrtrunk_json_forced_dmr_algid_uses_talkgroup_key(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }
    initOpts(opts);
    initState(state);

    FILE* fp = tmpfile();
    if (!fp) {
        DSD_FPRINTF(stderr, "failed to create temporary SDRTrunk JSON input\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    static const char json[] =
        "{\"protocol\":\"DMR\",\"call_type\":\"INDIVIDUAL\",\"encrypted\":\"false\",\"to\":\"1234\",\"from\":\"5678\","
        "\"time\":\"1700000000000\"}";
    if (fwrite(json, 1, sizeof(json) - 1U, fp) != sizeof(json) - 1U) {
        DSD_FPRINTF(stderr, "failed to write temporary SDRTrunk JSON input\n");
        fclose(fp);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "failed to rewind temporary SDRTrunk JSON input\n");
        fclose(fp);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    opts->mbe_in_f = fp;

    const unsigned long long tg_key = 0x0102030405ULL;
    state->M = 0x21;
    state->keyloader = 1;
    state->payload_mi = 0xAABBCCDDULL;
    state->payload_algid = 0x99;
    state->payload_keyid = 0x88;
    state->R = 0x0A0B0C0D0EULL;
    state->aes_key_loaded[0] = 1;
    state->rkey_array[1234] = tg_key;

    read_sdrtrunk_json_format(opts, state);

    int test_rc = 0;
    if (state->payload_mi != 0) {
        DSD_FPRINTF(stderr, "expected forced SDRTrunk JSON playback to reset payload_mi, got 0x%llX\n",
                    state->payload_mi);
        test_rc = 1;
    }
    if (state->payload_algid != 0x21) {
        DSD_FPRINTF(stderr, "expected forced SDRTrunk JSON ALGID 0x21, got 0x%02X\n", state->payload_algid);
        test_rc = 1;
    }
    if (state->R != tg_key) {
        DSD_FPRINTF(stderr, "expected forced SDRTrunk JSON TG key 0x%llX, got 0x%llX\n", tg_key, state->R);
        test_rc = 1;
    }
    if (state->aes_key_loaded[0] != 0) {
        DSD_FPRINTF(stderr, "expected keyloader SDRTrunk JSON reset to clear AES loaded state\n");
        test_rc = 1;
    }
    if (state->lasttg != 1234U || state->lastsrc != 5678U || state->gi[0] != 1) {
        DSD_FPRINTF(stderr, "unexpected SDRTrunk JSON call metadata tg=%d src=%d gi=%d\n", state->lasttg,
                    state->lastsrc, state->gi[0]);
        test_rc = 1;
    }

    fclose(fp);
    opts->mbe_in_f = NULL;
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_rdio_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rdio-mode";
    char arg2[] = "both";
    char arg3[] = "--rdio-system-id";
    char arg4[] = "42";
    char arg5[] = "--rdio-api-url";
    char arg6[] = "http://127.0.0.1:3000";
    char arg7[] = "--rdio-api-key";
    char arg8[] = "test-key";
    char arg9[] = "--rdio-upload-timeout-ms";
    char arg10[] = "2500";
    char arg11[] = "--rdio-upload-retries";
    char arg12[] = "4";
    char arg13[] = "--rdio-api-delete-after-upload";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(14, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->rdio_mode != DSD_RDIO_MODE_BOTH) {
        DSD_FPRINTF(stderr, "expected rdio_mode=%d, got %d\n", DSD_RDIO_MODE_BOTH, opts->rdio_mode);
        test_rc = 1;
    }
    if (opts->rdio_system_id != 42) {
        DSD_FPRINTF(stderr, "expected rdio_system_id=42, got %d\n", opts->rdio_system_id);
        test_rc = 1;
    }
    if (strcmp(opts->rdio_api_url, "http://127.0.0.1:3000") != 0) {
        DSD_FPRINTF(stderr, "unexpected rdio_api_url=%s\n", opts->rdio_api_url);
        test_rc = 1;
    }
    if (strcmp(opts->rdio_api_key, "test-key") != 0) {
        DSD_FPRINTF(stderr, "unexpected rdio_api_key=%s\n", opts->rdio_api_key);
        test_rc = 1;
    }
    if (opts->rdio_upload_timeout_ms != 2500) {
        DSD_FPRINTF(stderr, "expected timeout=2500, got %d\n", opts->rdio_upload_timeout_ms);
        test_rc = 1;
    }
    if (opts->rdio_upload_retries != 4) {
        DSD_FPRINTF(stderr, "expected retries=4, got %d\n", opts->rdio_upload_retries);
        test_rc = 1;
    }
    if (opts->rdio_api_delete_after_upload != 1) {
        DSD_FPRINTF(stderr, "expected rdio_api_delete_after_upload=1, got %d\n", opts->rdio_api_delete_after_upload);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_frame_log_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--frame-log";
    char arg2[] = "frames.log";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->frame_log_file, "frames.log") != 0) {
        DSD_FPRINTF(stderr, "unexpected frame_log_file=%s\n", opts->frame_log_file);
        test_rc = 1;
    }
    if (opts->payload != 0) {
        DSD_FPRINTF(stderr, "expected payload to remain off, got %d\n", opts->payload);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_p25_sm_log_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--p25-sm-log";
    char arg2[] = "p25-sm.log";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->p25_sm_log_file, "p25-sm.log") != 0) {
        DSD_FPRINTF(stderr, "unexpected p25_sm_log_file=%s\n", opts->p25_sm_log_file);
        test_rc = 1;
    }
    if (opts->payload != 0) {
        DSD_FPRINTF(stderr, "expected payload to remain off, got %d\n", opts->payload);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_dmr_debug_burst_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-debug-burst";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->dmr_debug_burst != 1) {
        DSD_FPRINTF(stderr, "expected dmr_debug_burst=1, got %u\n", (unsigned int)opts->dmr_debug_burst);
        test_rc = 1;
    }
    if (opts->payload != 0) {
        DSD_FPRINTF(stderr, "expected payload to remain off, got %d\n", opts->payload);
        test_rc = 1;
    }
    if (opts->use_dsp_output != 0 || opts->dsp_out_file[0] != '\0') {
        DSD_FPRINTF(stderr, "expected -Q output to remain off, got use_dsp_output=%d path=\"%s\"\n",
                    opts->use_dsp_output, opts->dsp_out_file);
        test_rc = 1;
    }
    if (argc_effective != 1) {
        DSD_FPRINTF(stderr, "expected compacted argc=1, got %d\n", argc_effective);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_show_keys_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--show-keys";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->show_keys != 1U) {
        DSD_FPRINTF(stderr, "expected show_keys=1, got %u\n", (unsigned int)opts->show_keys);
        test_rc = 1;
    }
    if (argc_effective != 1) {
        DSD_FPRINTF(stderr, "expected compacted argc=1, got %d\n", argc_effective);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_show_keys_after_option_terminator_remains_positional(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--";
    char arg2[] = "--show-keys";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->show_keys != 0U) {
        DSD_FPRINTF(stderr, "expected show_keys to remain redacted, got %u\n", (unsigned int)opts->show_keys);
        test_rc = 1;
    }
    if (argc_effective != 3) {
        DSD_FPRINTF(stderr, "expected compacted argc=3, got %d\n", argc_effective);
        test_rc = 1;
    }
    if (argv[1] == NULL || strcmp(argv[1], "--") != 0 || argv[2] == NULL || strcmp(argv[2], "--show-keys") != 0) {
        DSD_FPRINTF(stderr, "expected terminator and positional --show-keys to remain in argv\n");
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_input_source_arg_roundtrip(const char* input_spec) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-i";
    char arg2[2048];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", input_spec ? input_spec : "");
    arg2[sizeof arg2 - 1] = '\0';
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d for -i %s, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, arg2, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, arg2) != 0) {
        DSD_FPRINTF(stderr, "expected audio_in_dev=%s, got %s\n", arg2, opts->audio_in_dev);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_input_source_soapy_roundtrip(void) {
    return test_input_source_arg_roundtrip("soapy");
}

static int
test_input_source_soapy_args_roundtrip(void) {
    return test_input_source_arg_roundtrip("soapy:driver=airspy,serial=ABC123");
}

static int
test_input_source_rtl_roundtrip(void) {
    return test_input_source_arg_roundtrip("rtl:0:851.375M:30:5:16:-50:2");
}

static int
test_input_source_rtltcp_roundtrip(void) {
    return test_input_source_arg_roundtrip("rtltcp:127.0.0.1:1234:851.375M:30:5:16:-50:2");
}

static int
test_input_source_tcp_ipv4_roundtrip(void) {
    return test_input_source_arg_roundtrip("tcp:192.168.1.50:7355");
}

static int
test_iq_capture_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-capture";
    char arg2[] = "capture.iq";
    char arg3[] = "--iq-capture-format=cf32";
    char arg4[] = "--iq-capture-max-mb";
    char arg5[] = "8";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(6, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!opts->iq_capture_requested) {
        DSD_FPRINTF(stderr, "expected iq_capture_requested=1\n");
        test_rc = 1;
    }
    if (strcmp(opts->iq_capture_path, "capture.iq") != 0) {
        DSD_FPRINTF(stderr, "expected iq_capture_path=capture.iq, got %s\n", opts->iq_capture_path);
        test_rc = 1;
    }
    if (opts->iq_capture_format != DSD_IQ_FORMAT_CF32) {
        DSD_FPRINTF(stderr, "expected iq_capture_format=CF32, got %u\n", (unsigned)opts->iq_capture_format);
        test_rc = 1;
    }
    if (opts->iq_capture_max_bytes != (8ULL * 1024ULL * 1024ULL)) {
        DSD_FPRINTF(stderr, "expected iq_capture_max_bytes=%llu, got %llu\n",
                    (unsigned long long)(8ULL * 1024ULL * 1024ULL), (unsigned long long)opts->iq_capture_max_bytes);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_iq_replay_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temporary IQ fixture\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-replay";
    char arg2[1024];
    char arg3[] = "--iq-replay-rate=realtime";
    char arg4[] = "--iq-loop";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;

#ifdef USE_RADIO
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        test_rc = 1;
        goto out;
    }

    if (!opts->iq_replay_requested || !opts->iq_replay_loop) {
        DSD_FPRINTF(stderr, "expected iq replay requested+loop flags to be set\n");
        test_rc = 1;
    }
    if (opts->iq_replay_rate_mode != DSD_IQ_REPLAY_RATE_REALTIME) {
        DSD_FPRINTF(stderr, "expected iq_replay_rate_mode realtime, got %u\n", (unsigned)opts->iq_replay_rate_mode);
        test_rc = 1;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        DSD_FPRINTF(stderr, "expected audio_in_type=AUDIO_IN_RTL, got %d\n", opts->audio_in_type);
        test_rc = 1;
    }
    if (!dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev)) {
        DSD_FPRINTF(stderr, "expected audio_in_dev to be iqreplay spec, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U) {
        DSD_FPRINTF(stderr, "expected center frequency from metadata, got %u\n", opts->rtlsdr_center_freq);
        test_rc = 1;
    }
    if (opts->rtl_gain_value != 22) {
        DSD_FPRINTF(stderr, "expected tuner gain from metadata in dB, got %d\n", opts->rtl_gain_value);
        test_rc = 1;
    }
    if (!opts->iq_replay_requested || strcmp(opts->iq_replay_path, metadata_path) != 0) {
        DSD_FPRINTF(stderr, "expected iq_replay_path=%s, got %s\n", metadata_path, opts->iq_replay_path);
        test_rc = 1;
    }
    if (openAudioInDevice(opts, state) != 0) {
        DSD_FPRINTF(stderr, "expected iqreplay pseudo-input to be accepted by audio input classifier\n");
        test_rc = 1;
    } else if (opts->audio_in_type != AUDIO_IN_RTL) {
        DSD_FPRINTF(stderr, "expected iqreplay open to keep AUDIO_IN_RTL, got %d\n", opts->audio_in_type);
        test_rc = 1;
    }
#else
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected no-radio iq replay parse error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        DSD_FPRINTF(stderr, "expected no-radio iq replay parse to avoid AUDIO_IN_RTL\n");
        test_rc = 1;
    }
#endif

#ifdef USE_RADIO
out:
#endif
    (void)remove(metadata_path);
    (void)remove(data_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_iq_replay_audio_classifier_respects_radio_guard(void) {
#ifdef USE_RADIO
    return 0;
#else
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);
    opts->iq_replay_requested = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "iqreplay:/tmp/capture.iq.json");

    int test_rc = 0;
    if (openAudioInDevice(opts, state) == 0) {
        DSD_FPRINTF(stderr, "expected no-radio iqreplay classifier to reject input\n");
        test_rc = 1;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        DSD_FPRINTF(stderr, "expected no-radio iqreplay classifier to avoid AUDIO_IN_RTL\n");
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
#endif
}

static int
test_iq_info_returns_one_shot(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temporary IQ fixture\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-info";
    char arg2[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ONE_SHOT || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected iq-info one-shot success, got rc=%d exit_rc=%d\n", rc, exit_rc);
        (void)remove(metadata_path);
        (void)remove(data_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)remove(metadata_path);
    (void)remove(data_path);
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_iq_replay_capture_conflict_returns_error(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temporary IQ fixture\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-capture";
    char arg2[] = "capture.iq";
    char arg3[] = "--iq-replay";
    char arg4[1024];
    DSD_SNPRINTF(arg4, sizeof arg4, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected replay/capture conflict error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        (void)remove(metadata_path);
        (void)remove(data_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)remove(metadata_path);
    (void)remove(data_path);
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_missing_required_long_option_value_returns_error(const char* option_name) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[64];
    DSD_SNPRINTF(arg1, sizeof arg1, "%s", option_name ? option_name : "");
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected missing %s value error, got rc=%d exit_rc=%d\n",
                    option_name ? option_name : "(null)", rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_iq_capture_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-capture");
}

static int
test_iq_capture_format_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-capture-format");
}

static int
test_iq_capture_max_mb_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-capture-max-mb");
}

static int
test_iq_capture_max_mb_rejects_invalid_values(void) {
    const char* invalid_options[] = {
        "--iq-capture-max-mb=",
        "--iq-capture-max-mb=12mb",
        "--iq-capture-max-mb=18446744073709551616",
        "--iq-capture-max-mb=17592186044416",
    };
    int test_rc = 0;

    for (size_t i = 0; i < sizeof(invalid_options) / sizeof(invalid_options[0]); i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }

        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[96];
        DSD_SNPRINTF(arg1, sizeof arg1, "%s", invalid_options[i]);
        char* argv[] = {arg0, arg1, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_ERROR || exit_rc != 1 || opts->iq_capture_max_bytes != 0U) {
            DSD_FPRINTF(stderr,
                        "expected invalid %s to fail without setting max bytes, got rc=%d exit_rc=%d max=%" PRIu64 "\n",
                        invalid_options[i], rc, exit_rc, opts->iq_capture_max_bytes);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    return test_rc;
}

static int
test_iq_replay_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-replay");
}

static int
test_iq_replay_rate_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-replay-rate");
}

static int
test_iq_info_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--iq-info");
}

static int
test_rtl_udp_control_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control";
    char arg2[] = "9911";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->rtl_udp_port != 9911) {
        DSD_FPRINTF(stderr, "expected rtl_udp_port=9911, got %d\n", opts->rtl_udp_port);
        test_rc = 1;
    }
    if (strcmp(opts->rtl_udp_bindaddr, "127.0.0.1") != 0) {
        DSD_FPRINTF(stderr, "expected default rtl_udp_bindaddr=127.0.0.1, got %s\n", opts->rtl_udp_bindaddr);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_rtl_udp_control_missing_port_returns_error(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control";
    char arg2[] = "--auto-ppm";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected parse error for missing --rtl-udp-control value, got rc=%d exit_rc=%d\n", rc,
                    exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (opts->rtl_auto_ppm != 0) {
        DSD_FPRINTF(stderr, "expected --auto-ppm not to be consumed on parse error\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_rtl_udp_control_bind_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control";
    char arg2[] = "9911";
    char arg3[] = "--rtl-udp-control-bind";
    char arg4[] = "0.0.0.0";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = 0;
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        test_rc = 1;
    } else if (opts->rtl_udp_port != 9911 || strcmp(opts->rtl_udp_bindaddr, "0.0.0.0") != 0) {
        DSD_FPRINTF(stderr, "expected rtl UDP control on 0.0.0.0:9911, got %s:%d\n", opts->rtl_udp_bindaddr,
                    opts->rtl_udp_port);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_rtl_udp_control_invalid_bind_returns_error(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control-bind";
    char arg2[] = "localhost";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected invalid --rtl-udp-control-bind error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_rtl_udp_control_rejects_malformed_numeric_binds(void) {
    const char* invalid_binds[] = {"999.1.1.1", "1.2.3.", "1.2.3"};
    int test_rc = 0;

    for (size_t i = 0; i < sizeof(invalid_binds) / sizeof(invalid_binds[0]); i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }

        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[] = "--rtl-udp-control-bind";
        char* argv[] = {arg0, arg1, (char*)invalid_binds[i], NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
            DSD_FPRINTF(stderr, "expected invalid bind %s to fail, got rc=%d exit_rc=%d\n", invalid_binds[i], rc,
                        exit_rc);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    return test_rc;
}

static int
test_rtl_udp_control_port_too_large_returns_error(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--rtl-udp-control=70000";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected invalid --rtl-udp-control port error, got rc=%d exit_rc=%d\n", rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_rtl_udp_control_bind_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--rtl-udp-control-bind");
}

static int
test_dmr_baofeng_pc5_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-baofeng-pc5";
    char arg2[] = "0123456789ABCDEFFEDCBA9876543210";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->baofeng_ap != 1) {
        DSD_FPRINTF(stderr, "expected baofeng_ap=1, got %d\n", state->baofeng_ap);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_baofeng_pc5_256_long_option_uses_ascii_hex_key(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-baofeng-pc5";
    char arg2[] = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->baofeng_ap != 1) {
        DSD_FPRINTF(stderr, "expected baofeng_ap=1, got %d\n", state->baofeng_ap);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    static const char expected[] = "1011101110110010100111001011000101011000011001111";
    char frame[49];
    for (int i = 0; i < 49; i++) {
        frame[i] = (char)((i * 7 + 1) & 1);
    }
    if (baofeng_pc5_apply_frame49(state, frame) != 1) {
        DSD_FPRINTF(stderr, "expected 64-hex PC5 input to enable voice-frame application\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    for (int i = 0; i < 49; i++) {
        if ((frame[i] & 1) != (expected[i] - '0')) {
            DSD_FPRINTF(stderr, "expected 64-hex PC5 input to use ASCII hex OTA schedule (bit %d)\n", i);
            freeState(state);
            free(opts);
            free(state);
            return 1;
        }
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_csi_ee72_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-csi-ee72";
    char arg2[] = "11 22 33 44 55 66 77 88 99";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    const uint8_t expected[9] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    if (state->csi_ee != 1 || memcmp(state->csi_ee_key, expected, sizeof(expected)) != 0) {
        DSD_FPRINTF(stderr, "expected csi_ee=1 and parsed key bytes to match\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_vertex_ks_csv_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char csv_path[1024];
    if (test_create_temp_vertex_ks_csv(csv_path, sizeof csv_path, 0) != 0) {
        freeState(state);
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "failed to create temp vertex csv\n");
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-vertex-ks-csv";
    char* argv[] = {arg0, arg1, csv_path, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_count != 1) {
        DSD_FPRINTF(stderr, "expected vertex_ks_count=1, got %d\n", state->vertex_ks_count);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_key[0] != 0x1234567891ULL || state->vertex_ks_mod[0] != 8
        || state->vertex_ks_frame_mode[0] != 1 || state->vertex_ks_frame_off[0] != 2
        || state->vertex_ks_frame_step[0] != 3) {
        DSD_FPRINTF(stderr, "unexpected parsed vertex mapping fields\n");
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)remove(csv_path);
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_vertex_ks_csv_long_option_rejects_malformed_csv(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char csv_path[1024];
    if (test_create_temp_vertex_ks_csv(csv_path, sizeof csv_path, 1) != 0) {
        freeState(state);
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "failed to create malformed temp vertex csv\n");
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-vertex-ks-csv";
    char* argv[] = {arg0, arg1, csv_path, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected parse error for malformed Vertex KS CSV, got rc=%d exit_rc=%d\n", rc, exit_rc);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_count != 0) {
        DSD_FPRINTF(stderr, "expected vertex_ks_count=0 on malformed CSV, got %d\n", state->vertex_ks_count);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)remove(csv_path);
    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_force_algid_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-force-algid";
    char arg2[] = "0x24";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->M != 0x24) {
        DSD_FPRINTF(stderr, "expected forced ALGID 0x24, got 0x%02X\n", state->M);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_dmr_force_algid_long_option_rejects_invalid_value(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-force-algid=123";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected parse error for invalid forced ALGID, got rc=%d exit_rc=%d\n", rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_m17_signature_public_key_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--m17-signature-public-key";
    char arg2[] = "0x253DD9CE177042A6056F069C096A68F9937E5EC82F76F49BDCB78EE10B691373A "
                  "48911B59C269EAA33BC428FE598CE87ADD4ED6D1B4E0EFAFB2558456DFC35DE";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    static const uint8_t expected[DSD_ECDSA_P256_PUBLIC_KEY_BYTES] = {
        0x25U, 0x3DU, 0xD9U, 0xCEU, 0x17U, 0x70U, 0x42U, 0xA6U, 0x05U, 0x6FU, 0x06U, 0x9CU, 0x09U, 0x6AU, 0x68U, 0xF9U,
        0x93U, 0x7EU, 0x5EU, 0xC8U, 0x2FU, 0x76U, 0xF4U, 0x9BU, 0xDCU, 0xB7U, 0x8EU, 0xE1U, 0x0BU, 0x69U, 0x13U, 0x73U,
        0xA4U, 0x89U, 0x11U, 0xB5U, 0x9CU, 0x26U, 0x9EU, 0xAAU, 0x33U, 0xBCU, 0x42U, 0x8FU, 0xE5U, 0x98U, 0xCEU, 0x87U,
        0xADU, 0xD4U, 0xEDU, 0x6DU, 0x1BU, 0x4EU, 0x0EU, 0xFAU, 0xFBU, 0x25U, 0x58U, 0x45U, 0x6DU, 0xFCU, 0x35U, 0xDEU,
    };
    if (state->m17_signature_public_key_loaded != 1U
        || memcmp(state->m17_signature_public_key, expected, sizeof(expected)) != 0) {
        DSD_FPRINTF(stderr, "expected parsed M17 signature public key bytes to match\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_m17_signature_public_key_accepts_lowercase_spaced_hex(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--m17-signature-public-key";
    char arg2[] = "  0x253dd9ce177042a6056f069c096a68f9937e5ec82f76f49bdcb78ee10b691373a\n"
                  "48911b59c269eaa33bc428fe598ce87add4ed6d1b4e0efafb2558456dfc35de";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    static const uint8_t expected[DSD_ECDSA_P256_PUBLIC_KEY_BYTES] = {
        0x25U, 0x3DU, 0xD9U, 0xCEU, 0x17U, 0x70U, 0x42U, 0xA6U, 0x05U, 0x6FU, 0x06U, 0x9CU, 0x09U, 0x6AU, 0x68U, 0xF9U,
        0x93U, 0x7EU, 0x5EU, 0xC8U, 0x2FU, 0x76U, 0xF4U, 0x9BU, 0xDCU, 0xB7U, 0x8EU, 0xE1U, 0x0BU, 0x69U, 0x13U, 0x73U,
        0xA4U, 0x89U, 0x11U, 0xB5U, 0x9CU, 0x26U, 0x9EU, 0xAAU, 0x33U, 0xBCU, 0x42U, 0x8FU, 0xE5U, 0x98U, 0xCEU, 0x87U,
        0xADU, 0xD4U, 0xEDU, 0x6DU, 0x1BU, 0x4EU, 0x0EU, 0xFAU, 0xFBU, 0x25U, 0x58U, 0x45U, 0x6DU, 0xFCU, 0x35U, 0xDEU,
    };
    if (state->m17_signature_public_key_loaded != 1U
        || memcmp(state->m17_signature_public_key, expected, sizeof(expected)) != 0) {
        DSD_FPRINTF(stderr, "expected lowercase spaced M17 signature public key bytes to match\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_m17_signature_public_key_long_option_rejects_invalid_value(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--m17-signature-public-key=1234";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1 || state->m17_signature_public_key_loaded != 0U) {
        DSD_FPRINTF(stderr, "expected parse error for invalid M17 signature public key, got rc=%d exit_rc=%d\n", rc,
                    exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_m17_signature_public_key_missing_value_returns_error(void) {
    return test_missing_required_long_option_value_returns_error("--m17-signature-public-key");
}

static int
test_dmr_baofeng_pc5_long_option_rejects_invalid_key(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-baofeng-pc5";
    char arg2[] = "1234";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        DSD_FPRINTF(stderr, "expected parse error for invalid PC5 key, got rc=%d exit_rc=%d\n", rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_f_auto_preset_applies_cli_profile(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fa";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dstar && opts->frame_x2tdma && opts->frame_p25p1 && opts->frame_p25p2 && opts->frame_nxdn48
          && opts->frame_nxdn96 && opts->frame_dmr && opts->frame_dpmr && opts->frame_provoice && opts->frame_ysf
          && opts->frame_m17)) {
        DSD_FPRINTF(stderr, "expected -fa to enable all digital frame types\n");
        test_rc = 1;
    }
    if (opts->pulse_digi_out_channels != 2 || opts->dmr_stereo != 1) {
        DSD_FPRINTF(stderr, "unexpected -fa audio settings channels=%d stereo=%d\n", opts->pulse_digi_out_channels,
                    opts->dmr_stereo);
        test_rc = 1;
    }
    if (strcmp(opts->output_name, "AUTO") != 0) {
        DSD_FPRINTF(stderr, "expected output_name=AUTO, got %s\n", opts->output_name);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_f_ysf_preset_applies_cli_profile(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fy";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_ysf == 1 && opts->frame_dstar == 0 && opts->frame_dmr == 0 && opts->frame_p25p1 == 0
          && opts->frame_p25p2 == 0)) {
        DSD_FPRINTF(stderr, "unexpected -fy frame flags\n");
        test_rc = 1;
    }
    if (opts->pulse_digi_out_channels != 1 || opts->dmr_stereo != 0 || state->dmr_stereo != 0) {
        DSD_FPRINTF(stderr, "unexpected -fy audio settings channels=%d stereo=%d state_stereo=%d\n",
                    opts->pulse_digi_out_channels, opts->dmr_stereo, state->dmr_stereo);
        test_rc = 1;
    }
    if (strcmp(opts->output_name, "YSF") != 0) {
        DSD_FPRINTF(stderr, "expected output_name=YSF, got %s\n", opts->output_name);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_f_dpmr_and_m17_presets_match_documented_letters(void) {
    static const struct {
        const char* arg;
        int expect_dpmr;
        int expect_m17;
        const char* output_name;
    } cases[] = {{"-fm", 1, 0, "dPMR"}, {"-fz", 0, 1, "M17"}};

    int test_rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }

        initOpts(opts);
        initState(state);
        char arg0[] = "dsd-neo";
        char arg1[4] = {0};
        DSD_SNPRINTF(arg1, sizeof arg1, "%s", cases[i].arg);
        char* argv[] = {arg0, arg1, NULL};
        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);

        if (rc != DSD_PARSE_CONTINUE || opts->frame_dpmr != cases[i].expect_dpmr
            || opts->frame_m17 != cases[i].expect_m17 || strcmp(opts->output_name, cases[i].output_name) != 0) {
            DSD_FPRINTF(stderr, "unexpected %s preset rc=%d dpmr=%d m17=%d output=%s\n", cases[i].arg, rc,
                        opts->frame_dpmr, opts->frame_m17, opts->output_name);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }
    return test_rc;
}

static int
test_f_edacs_presets_match_reference_modes(void) {
    static const struct {
        const char* arg;
        int ea_mode;
        unsigned short esk_mask;
        int a_bits;
        int f_bits;
        int s_bits;
    } cases[] = {
        {"-fh", 0, 0x00, 4, 4, 3},    {"-fH", 0, 0xA0, 4, 4, 3},    {"-fe", 1, 0x00, 4, 4, 3},
        {"-fE", 1, 0xA0, 4, 4, 3},    {"-fh344", 0, 0x00, 3, 4, 4}, {"-fH434", 0, 0xA0, 4, 3, 4},
        {"-fH999", 0, 0xA0, 4, 4, 3}, {"-fe344", 1, 0x00, 4, 4, 3}, {"-fE434", 1, 0xA0, 4, 4, 3},
    };

    int test_rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            DSD_FPRINTF(stderr, "out of memory\n");
            return 1;
        }

        initOpts(opts);
        initState(state);

        char arg0[] = "dsd-neo";
        char arg1[16] = {0};
        DSD_SNPRINTF(arg1, sizeof arg1, "%s", cases[i].arg);
        char* argv[] = {arg0, arg1, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_CONTINUE) {
            DSD_FPRINTF(stderr, "expected %s rc=%d, got %d (exit_rc=%d)\n", cases[i].arg, DSD_PARSE_CONTINUE, rc,
                        exit_rc);
            test_rc = 1;
        }

        if (opts->frame_provoice != 1 || opts->frame_dmr != 0 || opts->frame_p25p1 != 0 || opts->frame_p25p2 != 0) {
            DSD_FPRINTF(stderr, "unexpected %s frame flags provoice=%d dmr=%d p25p1=%d p25p2=%d\n", cases[i].arg,
                        opts->frame_provoice, opts->frame_dmr, opts->frame_p25p1, opts->frame_p25p2);
            test_rc = 1;
        }
        if (state->ea_mode != cases[i].ea_mode || state->esk_mask != cases[i].esk_mask) {
            DSD_FPRINTF(stderr, "unexpected %s EDACS mode ea=%d esk=0x%X\n", cases[i].arg, state->ea_mode,
                        state->esk_mask);
            test_rc = 1;
        }
        if (state->edacs_a_bits != cases[i].a_bits || state->edacs_f_bits != cases[i].f_bits
            || state->edacs_s_bits != cases[i].s_bits) {
            DSD_FPRINTF(stderr, "unexpected %s AFS bits %d:%d:%d\n", cases[i].arg, state->edacs_a_bits,
                        state->edacs_f_bits, state->edacs_s_bits);
            test_rc = 1;
        }
        if (opts->pulse_digi_rate_out != 8000 || opts->pulse_digi_out_channels != 1 || opts->mod_gfsk != 1
            || state->rf_mod != 2 || strcmp(opts->output_name, "EDACS/PV") != 0) {
            DSD_FPRINTF(stderr, "unexpected %s EDACS profile rate=%d channels=%d gfsk=%d rf_mod=%d output=%s\n",
                        cases[i].arg, opts->pulse_digi_rate_out, opts->pulse_digi_out_channels, opts->mod_gfsk,
                        state->rf_mod, opts->output_name);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    return test_rc;
}

static int
test_f_dmr_preset_selects_gfsk(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fs";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dmr == 1 && opts->mod_c4fm == 0 && opts->mod_qpsk == 0 && opts->mod_gfsk == 1
          && state->rf_mod == 2)) {
        DSD_FPRINTF(stderr, "expected -fs to select DMR/GFSK, got frame_dmr=%d mod=%d/%d/%d rf_mod=%d\n",
                    opts->frame_dmr, opts->mod_c4fm, opts->mod_qpsk, opts->mod_gfsk, state->rf_mod);
        test_rc = 1;
    }
    if (opts->mod_cli_lock != 0) {
        DSD_FPRINTF(stderr, "expected -fs alone to leave demod unlocked, got lock=%d\n", opts->mod_cli_lock);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_mg_before_f_dmr_keeps_gfsk_lock(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-mg";
    char arg2[] = "-fs";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dmr == 1 && opts->mod_cli_lock == 1 && opts->mod_c4fm == 0 && opts->mod_qpsk == 0
          && opts->mod_gfsk == 1 && state->rf_mod == 2)) {
        DSD_FPRINTF(stderr, "expected -mg -fs to keep GFSK lock, got frame_dmr=%d lock=%d mod=%d/%d/%d rf_mod=%d\n",
                    opts->frame_dmr, opts->mod_cli_lock, opts->mod_c4fm, opts->mod_qpsk, opts->mod_gfsk, state->rf_mod);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_mc_before_f_dmr_preserves_c4fm_lock(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-mc";
    char arg2[] = "-fs";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dmr == 1 && opts->mod_cli_lock == 1 && opts->mod_c4fm == 1 && opts->mod_qpsk == 0
          && opts->mod_gfsk == 0 && state->rf_mod == 0)) {
        DSD_FPRINTF(stderr, "expected -mc -fs to preserve C4FM lock, got frame_dmr=%d lock=%d mod=%d/%d/%d rf_mod=%d\n",
                    opts->frame_dmr, opts->mod_cli_lock, opts->mod_c4fm, opts->mod_qpsk, opts->mod_gfsk, state->rf_mod);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_config_file_rate_survives_cli_provoice_preset(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-fp";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/input.wav") != 0) {
        DSD_FPRINTF(stderr, "expected config file input to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        DSD_FPRINTF(stderr, "expected effective file rate to stay 96000, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        DSD_FPRINTF(stderr, "expected ProVoice timing to rescale to 10/4 at 96 kHz, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_compact_s_rate_override_clears_config_file_rate(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-s44100";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_BOOTSTRAP_CONTINUE || exit_rc != 0) {
        DSD_FPRINTF(stderr, "expected compact -s bootstrap continue, got rc=%d exit_rc=%d\n", rc, exit_rc);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 44100 || dsd_opts_effective_input_rate(opts) != 44100) {
        DSD_FPRINTF(stderr, "expected compact -s44100 to keep 44100 Hz, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (opts->staged_file_sample_rate != 0) {
        DSD_FPRINTF(stderr, "expected compact -s44100 to clear staged config rate, got %d\n",
                    opts->staged_file_sample_rate);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_s_8000_keeps_valid_symbol_timing_for_provoice(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fp";
    char arg2[] = "-s";
    char arg3[] = "8000";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->wav_sample_rate != 8000) {
        DSD_FPRINTF(stderr, "expected wav_sample_rate=8000, got %d\n", opts->wav_sample_rate);
        test_rc = 1;
    }
    if (opts->wav_interpolator != 1) {
        DSD_FPRINTF(stderr, "expected wav_interpolator=1 for 8 kHz input, got %d\n", opts->wav_interpolator);
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 5 || state->symbolCenter != 2) {
        DSD_FPRINTF(stderr, "expected ProVoice timing to remain 5/2, got sps=%d center=%d\n", state->samplesPerSymbol,
                    state->symbolCenter);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_m2_low_rate_preserves_p25p2_profile(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-m2";
    char arg2[] = "-s";
    char arg3[] = "11025";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_p25p1 == 1 && opts->frame_p25p2 == 1 && opts->frame_dmr == 1 && opts->frame_ysf == 1)) {
        DSD_FPRINTF(stderr, "standalone -m2 should preserve the default frame candidates\n");
        test_rc = 1;
    }
    if (!(opts->mod_cli_lock == 1 && opts->mod_p25p2_profile_lock == 1 && opts->mod_qpsk == 1 && opts->mod_c4fm == 0
          && opts->mod_gfsk == 0 && state->rf_mod == 1)) {
        DSD_FPRINTF(stderr, "standalone -m2 did not retain the manual P25p2 QPSK mode\n");
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 2 || state->symbolCenter != 0
        || state->sps_hunt_idx != DSD_FRAME_SYNC_SPS_PROFILE_6000_4) {
        DSD_FPRINTF(stderr, "expected low-rate -m2 timing/profile 2/0/%d, got %d/%d/%d\n",
                    DSD_FRAME_SYNC_SPS_PROFILE_6000_4, state->samplesPerSymbol, state->symbolCenter,
                    state->sps_hunt_idx);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_standalone_m3_marks_manual_p25p2_c4fm_path(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-m3";
    char arg2[] = "-s";
    char arg3[] = "96000";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_p25p1 == 1 && opts->frame_p25p2 == 1 && opts->frame_dmr == 1 && opts->frame_ysf == 1)) {
        DSD_FPRINTF(stderr, "standalone -m3 should preserve the default frame candidates\n");
        test_rc = 1;
    }
    if (!(opts->mod_p25p2_c4fm == 1 && opts->mod_cli_lock == 1 && opts->mod_c4fm == 1 && opts->mod_qpsk == 0
          && opts->mod_gfsk == 0 && state->rf_mod == 0)) {
        DSD_FPRINTF(stderr, "standalone -m3 did not retain the manual P25p2 C4FM mode\n");
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 20 || state->symbolCenter != 8) {
        DSD_FPRINTF(stderr, "expected standalone -m3 timing to rescale to 20/8 at 96 kHz, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_m3_override_survives_file_rate_rescale_after_f2(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-f2";
    char arg2[] = "-m3";
    char arg3[] = "-s";
    char arg4[] = "96000";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        DSD_FPRINTF(stderr, "expected 96 kHz file rate after -s, got raw=%d effective=%d\n", opts->wav_sample_rate,
                    dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 20 || state->symbolCenter != 8) {
        DSD_FPRINTF(stderr, "expected -m3 timing override to rescale to 20/8 at 96 kHz, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_trunk_scan_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan";
    char arg2[] = "targets.csv";
    char arg3[] = "--trunk-scan-dwell-ms";
    char arg4[] = "500";
    char arg5[] = "--trunk-scan-activity-hold-ms=800";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};
    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(6, argv, opts, state, &argc_effective, &exit_rc);

    int test_rc = 0;
    if (rc != DSD_PARSE_CONTINUE || opts->trunk_scan_enabled != 1
        || strcmp(opts->trunk_scan_targets_csv, "targets.csv") != 0 || opts->trunk_scan_idle_dwell_ms != 500
        || opts->trunk_scan_activity_hold_ms != 800) {
        DSD_FPRINTF(stderr, "trunk scan long option parse mismatch rc=%d enabled=%d targets=%s dwell=%d hold=%d\n", rc,
                    opts->trunk_scan_enabled, opts->trunk_scan_targets_csv, opts->trunk_scan_idle_dwell_ms,
                    opts->trunk_scan_activity_hold_ms);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_trunk_scan_conflicts_with_scanner_mode(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan";
    char arg2[] = "targets.csv";
    char arg3[] = "-Y";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};
    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = (rc == DSD_PARSE_ERROR && exit_rc == 1) ? 0 : 1;
    if (test_rc) {
        DSD_FPRINTF(stderr, "expected trunk scan/-Y conflict, got rc=%d exit=%d\n", rc, exit_rc);
    }
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_trunk_scan_rejects_global_channel_map(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan=targets.csv";
    char arg2[] = "-C";
    char arg3[] = "chan.csv";
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};
    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(4, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = (rc == DSD_PARSE_ERROR && exit_rc == 1 && opts->chan_in_file[0] == '\0') ? 0 : 1;
    if (test_rc) {
        DSD_FPRINTF(stderr, "expected trunk scan/-C conflict before import, rc=%d exit=%d chan=%s\n", rc, exit_rc,
                    opts->chan_in_file);
    }
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_trunk_scan_cli_clears_inherited_channel_map(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);

    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "inherited.csv");
    opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';

    char arg0[] = "dsd-neo";
    char arg1[] = "--trunk-scan";
    char arg2[] = "targets.csv";
    char* argv[] = {arg0, arg1, arg2, NULL};
    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    int test_rc = (rc == DSD_PARSE_CONTINUE && opts->trunk_scan_enabled == 1
                   && strcmp(opts->trunk_scan_targets_csv, "targets.csv") == 0 && opts->chan_in_file[0] == '\0')
                      ? 0
                      : 1;
    if (test_rc) {
        DSD_FPRINTF(stderr, "expected explicit trunk scan to clear inherited channel map, rc=%d enabled=%d chan=%s\n",
                    rc, opts->trunk_scan_enabled, opts->chan_in_file);
    }
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_trunk_scan_inherited_state_rejects_invalid_runtime_combinations(void) {
    int test_rc = 0;

    for (int scenario = 0; scenario < 3; scenario++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            return 1;
        }
        initOpts(opts);
        initState(state);

        opts->trunk_scan_enabled = 1;
        if (scenario == 0) {
            opts->scanner_mode = 1;
            DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", "targets.csv");
        } else if (scenario == 1) {
            DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof opts->trunk_scan_targets_csv, "%s", "targets.csv");
            DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "global_channels.csv");
        }

        char arg0[] = "dsd-neo";
        char* argv[] = {arg0, NULL};
        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(1, argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
            DSD_FPRINTF(stderr, "expected inherited trunk-scan scenario %d to fail, got rc=%d exit_rc=%d\n", scenario,
                        rc, exit_rc);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    return test_rc;
}

static int
test_trunk_scan_rejects_ms_values_outside_range(void) {
    const char* argv_sets[][3] = {
        {"dsd-neo", "--trunk-scan-dwell-ms", "249"},
        {"dsd-neo", "--trunk-scan-activity-hold-ms=600001", NULL},
    };
    const int argc_values[] = {3, 2};
    int test_rc = 0;

    for (size_t i = 0; i < sizeof(argc_values) / sizeof(argc_values[0]); i++) {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            return 1;
        }
        initOpts(opts);
        initState(state);

        char arg0[32];
        char arg1[64];
        char arg2[32];
        DSD_SNPRINTF(arg0, sizeof arg0, "%s", argv_sets[i][0]);
        DSD_SNPRINTF(arg1, sizeof arg1, "%s", argv_sets[i][1]);
        if (argv_sets[i][2]) {
            DSD_SNPRINTF(arg2, sizeof arg2, "%s", argv_sets[i][2]);
        }
        char* argv[] = {arg0, arg1, argv_sets[i][2] ? arg2 : NULL, NULL};

        int argc_effective = 0;
        int exit_rc = -1;
        int rc = dsd_parse_args(argc_values[i], argv, opts, state, &argc_effective, &exit_rc);
        if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
            DSD_FPRINTF(stderr, "expected trunk-scan millisecond option %s to fail, got rc=%d exit_rc=%d\n", arg1, rc,
                        exit_rc);
            test_rc = 1;
        }

        freeState(state);
        free(opts);
        free(state);
    }

    return test_rc;
}

static int
test_bootstrap_config_file_rate_rescales_manual_m3_override(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-m3";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/input.wav") != 0) {
        DSD_FPRINTF(stderr, "expected config file input to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        DSD_FPRINTF(stderr, "expected effective file rate to stay 96000, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 20 || state->symbolCenter != 8) {
        DSD_FPRINTF(stderr, "expected -m3 timing override to rescale to 20/8 at 96 kHz, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_cli_pulse_override_ignores_config_file_rate_timing(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-i";
    char arg4[] = "pulse";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "pulse") != 0) {
        DSD_FPRINTF(stderr, "expected CLI pulse override to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 48000 || dsd_opts_effective_input_rate(opts) != 48000) {
        DSD_FPRINTF(stderr, "expected pulse override to keep default file rate, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        DSD_FPRINTF(stderr, "expected pulse override to keep 48 kHz timing, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_cli_file_override_ignores_config_file_rate_timing(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-i";
    char arg4[] = "/tmp/other.raw";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/other.raw") != 0) {
        DSD_FPRINTF(stderr, "expected CLI file override to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 48000 || dsd_opts_effective_input_rate(opts) != 48000) {
        DSD_FPRINTF(stderr, "expected CLI file override to keep default file rate, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (opts->staged_file_sample_rate != 0) {
        DSD_FPRINTF(stderr, "expected CLI file override to clear staged file rate, got %d\n",
                    opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        DSD_FPRINTF(stderr, "expected CLI file override to keep 48 kHz timing, got sps=%d center=%d\n",
                    state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_cli_file_override_uses_cli_rate_for_headerless_open(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    /*
     * A CLI file path should override the configured file source completely.
     * The staged config rate is cleared so the headerless raw file opens using
     * the normal default input rate.
     */
    const short samples[] = {1234, -2345, 3456, -4567};
    char raw_path[1024];
    if (test_create_temp_raw_pcm_file("dsdneo_cli_override_input", samples, sizeof samples / sizeof samples[0], ".pcm",
                                      raw_path, sizeof raw_path)
        != 0) {
        DSD_FPRINTF(stderr, "failed to create temp raw pcm file\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-i";
    char arg4[1024];
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    DSD_SNPRINTF(arg4, sizeof arg4, "%s", raw_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    // Bootstrap should preserve the CLI file source while removing staged rate.
    if (opts->staged_file_sample_rate != 0) {
        DSD_FPRINTF(stderr, "expected CLI file override to clear staged file rate, got %d\n",
                    opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (openAudioInDevice(opts, state) != 0) {
        DSD_FPRINTF(stderr, "expected headerless CLI file override to open successfully\n");
        test_rc = 1;
    } else {
        if (opts->audio_in_type != AUDIO_IN_WAV) {
            DSD_FPRINTF(stderr, "expected headerless CLI file override to open as AUDIO_IN_WAV, got %d\n",
                        opts->audio_in_type);
            test_rc = 1;
        }
        if (!opts->audio_in_file_info) {
            DSD_FPRINTF(stderr, "expected audio_in_file_info after headerless open\n");
            test_rc = 1;
        } else {
            if (opts->audio_in_file_info->samplerate != 48000) {
                DSD_FPRINTF(stderr, "expected headerless CLI file override to open at 48000 Hz, got %d\n",
                            opts->audio_in_file_info->samplerate);
                test_rc = 1;
            }
            if ((opts->audio_in_file_info->format & SF_FORMAT_TYPEMASK) != SF_FORMAT_RAW) {
                DSD_FPRINTF(stderr, "expected headerless CLI file override to open as raw PCM, got format=0x%x\n",
                            opts->audio_in_file_info->format);
                test_rc = 1;
            }
        }
    }

    if (opts->audio_in_file) {
        sf_close(opts->audio_in_file);
        opts->audio_in_file = NULL;
    }
    if (opts->audio_in_file_info) {
        free(opts->audio_in_file_info);
        opts->audio_in_file_info = NULL;
    }

    (void)remove(cfg_path);
    (void)remove(raw_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_cli_rate_override_uses_cli_rate_for_headerless_open(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        DSD_FPRINTF(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    /*
     * A CLI sample-rate override wins over the configured file sample rate.
     * Opening the headerless raw file after bootstrap verifies the effective
     * rate passed through to libsndfile.
     */
    const short samples[] = {4321, -3210, 2109, -1098};
    char raw_path[1024];
    if (test_create_temp_raw_pcm_file("dsdneo_cli_rate_override", samples, sizeof samples / sizeof samples[0], ".pcm",
                                      raw_path, sizeof raw_path)
        != 0) {
        DSD_FPRINTF(stderr, "failed to create temp raw pcm file\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char ini[1536];
    DSD_SNPRINTF(ini, sizeof ini,
                 "[input]\n"
                 "source = \"file\"\n"
                 "file_path = \"%s\"\n"
                 "file_sample_rate = 96000\n",
                 raw_path);

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp file-input ini\n");
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-s";
    char arg4[] = "44100";
    DSD_SNPRINTF(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    // The config file source remains, but its staged rate must not override -s.
    if (opts->wav_sample_rate != 44100 || dsd_opts_effective_input_rate(opts) != 44100) {
        DSD_FPRINTF(stderr, "expected CLI rate override to keep 44100 Hz, got raw=%d effective=%d\n",
                    opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (opts->staged_file_sample_rate != 0) {
        DSD_FPRINTF(stderr, "expected CLI rate override to clear staged file rate, got %d\n",
                    opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (openAudioInDevice(opts, state) != 0) {
        DSD_FPRINTF(stderr, "expected headerless CLI rate override to open successfully\n");
        test_rc = 1;
    } else {
        if (opts->audio_in_type != AUDIO_IN_WAV) {
            DSD_FPRINTF(stderr, "expected headerless CLI rate override to open as AUDIO_IN_WAV, got %d\n",
                        opts->audio_in_type);
            test_rc = 1;
        }
        if (!opts->audio_in_file_info) {
            DSD_FPRINTF(stderr, "expected audio_in_file_info after headerless open\n");
            test_rc = 1;
        } else {
            if (opts->audio_in_file_info->samplerate != 44100) {
                DSD_FPRINTF(stderr, "expected headerless CLI rate override to open at 44100 Hz, got %d\n",
                            opts->audio_in_file_info->samplerate);
                test_rc = 1;
            }
            if ((opts->audio_in_file_info->format & SF_FORMAT_TYPEMASK) != SF_FORMAT_RAW) {
                DSD_FPRINTF(stderr, "expected headerless CLI rate override to open as raw PCM, got format=0x%x\n",
                            opts->audio_in_file_info->format);
                test_rc = 1;
            }
        }
    }

    if (opts->audio_in_file) {
        sf_close(opts->audio_in_file);
        opts->audio_in_file = NULL;
    }
    if (opts->audio_in_file_info) {
        free(opts->audio_in_file_info);
        opts->audio_in_file_info = NULL;
    }

    (void)remove(cfg_path);
    (void)remove(raw_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_help_returns_one_shot_and_does_not_exit();
    rc |= test_invalid_option_returns_error_and_does_not_exit();
    rc |= test_unknown_option_returns_error_and_does_not_exit();
    rc |= test_frontend_terminal_option_sets_terminal_kind();
    rc |= test_numeric_options_reject_trailing_junk();
    rc |= test_H_loads_aes256_key_for_both_slots();
    rc |= test_H_zero_key_keeps_dmr_encrypted_audio_muted();
    rc |= test_H_show_keys_log_reveals_key_material();
    rc |= test_b_loads_basic_privacy_key_and_unmutes_dmr();
    rc |= test_b_zero_key_keeps_dmr_encrypted_audio_muted();
    rc |= test_b_clamps_to_basic_privacy_table_max();
    rc |= test_2_loads_tyt_basic_privacy_key_and_truncates_to_16_bits();
    rc |= test_1_loads_rc4_key_for_both_slots_and_allows_spaces();
    rc |= test_1_loads_rc4_key_allows_0x_prefix();
    rc |= test_R_loads_nxdn_scrambler_key_and_disables_keyloader();
    rc |= test_nxdn_pn95_seed_option_matches_reference_bounds();
    rc |= test_bootstrap_treats_lone_ini_as_config();
    rc |= test_bootstrap_accepts_explicit_config_path_outside_cwd();
    rc |= test_bootstrap_config_trunking_preserves_terminal_frontend();
    rc |= test_bootstrap_missing_explicit_config_keeps_autosave_path();
    rc |= test_bootstrap_rejects_too_long_explicit_config_path();
    rc |= test_bootstrap_guard_rejects_invalid_arguments();
    rc |= test_bootstrap_validate_config_accepts_external_path();
    rc |= test_bootstrap_validate_config_strict_warning_exits_two();
    rc |= test_bootstrap_validate_config_reports_trunk_scan_diagnostics();
    rc |= test_bootstrap_validate_config_rejects_invalid_included_versions();
    rc |= test_bootstrap_list_profiles_accepts_external_config_path();
    rc |= test_bootstrap_list_profiles_reports_empty_config();
    rc |= test_bootstrap_print_config_normalizes_soapy_shorthand();
    rc |= test_bootstrap_profile_preserves_trunking_with_ncurses_cli();
    rc |= test_bootstrap_inherited_trunk_scan_preserves_ui_only_short_options();
    rc |= test_bootstrap_inherited_trunk_scan_allows_cli_channel_map();
    rc |= test_bootstrap_inherited_trunk_scan_disables_for_positional_input();
    rc |= test_bootstrap_inherited_trunk_scan_disables_for_long_only_runtime_mode();
    rc |= test_bootstrap_inherited_trunk_scan_preserves_timing_overrides();
    rc |= test_bootstrap_config_one_shots_skip_trunk_scan_runtime_validation();
    rc |= test_bootstrap_profile_disables_autosave();
    rc |= test_bootstrap_missing_profile_errors_without_applying_config_or_cli();
    rc |= test_bootstrap_cli_call_alert_restores_all_config_filtered_events();
    rc |= test_r_playback_optind_is_first_file_regardless_of_option_order();
    rc |= test_open_mbe_missing_file_leaves_stream_null();
    rc |= test_sdrtrunk_json_forced_dmr_algid_uses_talkgroup_key();
    rc |= test_rdio_long_options_parse();
    rc |= test_frame_log_long_option_parse();
    rc |= test_p25_sm_log_long_option_parse();
    rc |= test_dmr_debug_burst_long_option_parse();
    rc |= test_show_keys_long_option_parse();
    rc |= test_show_keys_after_option_terminator_remains_positional();
    rc |= test_input_source_soapy_roundtrip();
    rc |= test_input_source_soapy_args_roundtrip();
    rc |= test_input_source_rtl_roundtrip();
    rc |= test_input_source_rtltcp_roundtrip();
    rc |= test_input_source_tcp_ipv4_roundtrip();
    rc |= test_trunk_scan_long_options_parse();
    rc |= test_trunk_scan_conflicts_with_scanner_mode();
    rc |= test_trunk_scan_rejects_global_channel_map();
    rc |= test_trunk_scan_cli_clears_inherited_channel_map();
    rc |= test_trunk_scan_inherited_state_rejects_invalid_runtime_combinations();
    rc |= test_trunk_scan_rejects_ms_values_outside_range();
    rc |= test_iq_capture_long_options_parse();
    rc |= test_iq_capture_missing_value_returns_error();
    rc |= test_iq_capture_format_missing_value_returns_error();
    rc |= test_iq_capture_max_mb_missing_value_returns_error();
    rc |= test_iq_capture_max_mb_rejects_invalid_values();
    rc |= test_iq_replay_long_options_parse();
    rc |= test_iq_replay_audio_classifier_respects_radio_guard();
    rc |= test_iq_replay_rate_missing_value_returns_error();
    rc |= test_iq_info_returns_one_shot();
    rc |= test_iq_info_missing_value_returns_error();
    rc |= test_iq_replay_capture_conflict_returns_error();
    rc |= test_iq_replay_missing_value_returns_error();
    rc |= test_rtl_udp_control_long_option_parse();
    rc |= test_rtl_udp_control_missing_port_returns_error();
    rc |= test_rtl_udp_control_bind_long_option_parse();
    rc |= test_rtl_udp_control_invalid_bind_returns_error();
    rc |= test_rtl_udp_control_rejects_malformed_numeric_binds();
    rc |= test_rtl_udp_control_port_too_large_returns_error();
    rc |= test_rtl_udp_control_bind_missing_value_returns_error();
    rc |= test_dmr_baofeng_pc5_long_option_parse();
    rc |= test_dmr_baofeng_pc5_256_long_option_uses_ascii_hex_key();
    rc |= test_dmr_csi_ee72_long_option_parse();
    rc |= test_dmr_vertex_ks_csv_long_option_parse();
    rc |= test_dmr_vertex_ks_csv_long_option_rejects_malformed_csv();
    rc |= test_dmr_force_algid_long_option_parse();
    rc |= test_dmr_force_algid_long_option_rejects_invalid_value();
    rc |= test_m17_signature_public_key_long_option_parse();
    rc |= test_m17_signature_public_key_accepts_lowercase_spaced_hex();
    rc |= test_m17_signature_public_key_long_option_rejects_invalid_value();
    rc |= test_m17_signature_public_key_missing_value_returns_error();
    rc |= test_dmr_baofeng_pc5_long_option_rejects_invalid_key();
    rc |= test_f_auto_preset_applies_cli_profile();
    rc |= test_f_ysf_preset_applies_cli_profile();
    rc |= test_f_dpmr_and_m17_presets_match_documented_letters();
    rc |= test_f_edacs_presets_match_reference_modes();
    rc |= test_f_dmr_preset_selects_gfsk();
    rc |= test_mg_before_f_dmr_keeps_gfsk_lock();
    rc |= test_mc_before_f_dmr_preserves_c4fm_lock();
    rc |= test_bootstrap_config_file_rate_survives_cli_provoice_preset();
    rc |= test_bootstrap_compact_s_rate_override_clears_config_file_rate();
    rc |= test_s_8000_keeps_valid_symbol_timing_for_provoice();
    rc |= test_m2_low_rate_preserves_p25p2_profile();
    rc |= test_standalone_m3_marks_manual_p25p2_c4fm_path();
    rc |= test_m3_override_survives_file_rate_rescale_after_f2();
    rc |= test_bootstrap_config_file_rate_rescales_manual_m3_override();
    rc |= test_bootstrap_cli_pulse_override_ignores_config_file_rate_timing();
    rc |= test_bootstrap_cli_file_override_ignores_config_file_rate_timing();
    rc |= test_bootstrap_cli_file_override_uses_cli_rate_for_headerless_open();
    rc |= test_bootstrap_cli_rate_override_uses_cli_rate_for_headerless_open();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
