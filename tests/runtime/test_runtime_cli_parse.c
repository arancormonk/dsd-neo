// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/pc5.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <sndfile.h>
#include <stdint.h>

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>

#undef DSD_NEO_MAIN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

void
noCarrier(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
test_help_returns_one_shot_and_does_not_exit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ONE_SHOT, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 0) {
        fprintf(stderr, "expected exit_rc=0, got %d\n", exit_rc);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 1) {
        fprintf(stderr, "expected exit_rc=1, got %d\n", exit_rc);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d\n", DSD_PARSE_ERROR, rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (exit_rc != 1) {
        fprintf(stderr, "expected exit_rc=1, got %d\n", exit_rc);
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
test_H_loads_aes256_key_for_both_slots(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
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
        fprintf(stderr,
                "expected A1..A4 to match key segments, got slot0=%016llX %016llX %016llX %016llX slot1=%016llX "
                "%016llX %016llX %016llX\n",
                (unsigned long long)state->A1[0], (unsigned long long)state->A2[0], (unsigned long long)state->A3[0],
                (unsigned long long)state->A4[0], (unsigned long long)state->A1[1], (unsigned long long)state->A2[1],
                (unsigned long long)state->A3[1], (unsigned long long)state->A4[1]);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (state->aes_key_loaded[0] != 1 || state->aes_key_loaded[1] != 1) {
        fprintf(stderr, "expected aes_key_loaded[0..1]=1, got %d/%d\n", state->aes_key_loaded[0],
                state->aes_key_loaded[1]);
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
        fprintf(stderr, "expected aes_key bytes to match key, got mismatch\n");
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const unsigned long long expect = 0x1234567891ULL;
    if (state->R != expect || state->RR != expect) {
        fprintf(stderr, "expected R/RR=%010llX, got %010llX/%010llX\n", expect, state->R, state->RR);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    const unsigned long long expect = 0x1234567891ULL;
    if (state->R != expect || state->RR != expect) {
        fprintf(stderr, "expected R/RR=%010llX, got %010llX/%010llX\n", expect, state->R, state->RR);
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

    const char sep = test_path_sep();
    const char* tdir = test_tmp_dir();

    char tmpl[1024];
    size_t tdir_len = strlen(tdir);
    if (tdir_len > 0 && (tdir[tdir_len - 1] == '/' || tdir[tdir_len - 1] == '\\')) {
        snprintf(tmpl, sizeof tmpl, "%sdsdneo_bootstrap_XXXXXX", tdir);
    } else {
        snprintf(tmpl, sizeof tmpl, "%s%c%s", tdir, sep, "dsdneo_bootstrap_XXXXXX");
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (snprintf(out_path, out_path_size, "%s.ini", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = fopen(out_path, "w");
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
    return test_create_temp_ini_with_contents("version = 1\n"
                                              "\n"
                                              "[input]\n"
                                              "source = \"rtl\"\n"
                                              "rtl_device = 0\n"
                                              "rtl_freq = \"100000000\"\n"
                                              "\n"
                                              "[trunking]\n"
                                              "enabled = true\n",
                                              out_path, out_path_size);
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
        snprintf(tmpl, sizeof tmpl, "%s%s_XXXXXX", tdir, prefix);
    } else {
        snprintf(tmpl, sizeof tmpl, "%s%c%s_XXXXXX", tdir, sep, prefix);
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (snprintf(out_path, out_path_size, "%s%s", tmpl, suffix) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = fopen(out_path, "wb");
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

    const char sep = test_path_sep();
    const char* tdir = test_tmp_dir();

    char tmpl[1024];
    size_t tdir_len = strlen(tdir);
    if (tdir_len > 0 && (tdir[tdir_len - 1] == '/' || tdir[tdir_len - 1] == '\\')) {
        snprintf(tmpl, sizeof tmpl, "%sdsdneo_vertex_ks_XXXXXX", tdir);
    } else {
        snprintf(tmpl, sizeof tmpl, "%s%c%s", tdir, sep, "dsdneo_vertex_ks_XXXXXX");
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    if (snprintf(out_path, out_path_size, "%s.csv", tmpl) >= (int)out_path_size) {
        (void)remove(tmpl);
        return -1;
    }

    if (rename(tmpl, out_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* fp = fopen(out_path, "w");
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

    const char sep = test_path_sep();
    const char* tdir = test_tmp_dir();

    char tmpl[1024];
    size_t tdir_len = strlen(tdir);
    if (tdir_len > 0 && (tdir[tdir_len - 1] == '/' || tdir[tdir_len - 1] == '\\')) {
        snprintf(tmpl, sizeof tmpl, "%sdsdneo_iq_cli_XXXXXX", tdir);
    } else {
        snprintf(tmpl, sizeof tmpl, "%s%c%s", tdir, sep, "dsdneo_iq_cli_XXXXXX");
    }

    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);

    char data_path[1024];
    if (snprintf(data_path, sizeof data_path, "%s.iq", tmpl) >= (int)sizeof(data_path)) {
        (void)remove(tmpl);
        return -1;
    }
    if (rename(tmpl, data_path) != 0) {
        (void)remove(tmpl);
        return -1;
    }

    FILE* data_fp = fopen(data_path, "wb");
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
    if (snprintf(metadata_path, sizeof metadata_path, "%s.json", data_path) >= (int)sizeof(metadata_path)) {
        (void)remove(data_path);
        return -1;
    }
    FILE* meta_fp = fopen(metadata_path, "w");
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

    int n = fprintf(meta_fp,
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

    snprintf(out_data_path, out_data_path_size, "%s", data_path);
    snprintf(out_metadata_path, out_metadata_path_size, "%s", metadata_path);
    return 0;
}

static int
test_bootstrap_treats_lone_ini_as_config(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    // Make test deterministic: avoid env-config interference and skip bootstrap UI.
    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char cfg_path[1024];
    if (test_create_temp_ini(cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp ini\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    // Ensure it behaves like "--config <path>" by compacting the effective argc down to argv[0] only.
    if (argc_effective != 1) {
        fprintf(stderr, "expected argc_effective=1, got %d\n", argc_effective);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (!state->config_autosave_enabled || strcmp(state->config_autosave_path, cfg_path) != 0) {
        fprintf(stderr, "expected config_autosave_path=%s, got %s (enabled=%d)\n", cfg_path,
                state->config_autosave_path, state->config_autosave_enabled);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (opts->trunk_enable != 1 || opts->p25_trunk != 1) {
        fprintf(stderr, "expected trunking enabled from config, got trunk_enable=%d p25_trunk=%d\n", opts->trunk_enable,
                opts->p25_trunk);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    if (strncmp(opts->audio_in_dev, "rtl:", 4) != 0) {
        fprintf(stderr, "expected RTL input from config, got audio_in_dev=%s\n", opts->audio_in_dev);
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
test_bootstrap_print_config_normalizes_soapy_shorthand(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d and exit_rc=0, got rc=%d exit_rc=%d\n", DSD_BOOTSTRAP_EXIT, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        fprintf(stderr, "expected normalized soapy args, got audio_in_dev=%s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 22 || opts->rtlsdr_ppm_error != -2
        || opts->rtl_dsp_bw_khz != 24 || opts->rtl_squelch_level != 0.0 || opts->rtl_volume_multiplier != 2) {
        fprintf(stderr, "unexpected normalized tuning values freq=%u gain=%d ppm=%d bw=%d sql=%.6f vol=%d\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.p25_trunk]\n"
                             "input.source = \"rtl\"\n"
                             "input.rtl_device = 0\n"
                             "input.rtl_freq = \"100000000\"\n"
                             "trunking.enabled = true\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp profile ini\n");
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
    char arg5[] = "-N";
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(6, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->trunk_enable != 1 || opts->p25_trunk != 1) {
        fprintf(stderr, "expected profiled trunking to stay enabled, got trunk_enable=%d p25_trunk=%d\n",
                opts->trunk_enable, opts->p25_trunk);
        test_rc = 1;
    }
    if (opts->use_ncurses_terminal != 1) {
        fprintf(stderr, "expected -N to remain applied, got use_ncurses_terminal=%d\n", opts->use_ncurses_terminal);
        test_rc = 1;
    }
    if (strncmp(opts->audio_in_dev, "rtl:", 4) != 0) {
        fprintf(stderr, "expected profile RTL input, got audio_in_dev=%s\n", opts->audio_in_dev);
        test_rc = 1;
    }

    (void)remove(cfg_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_profile_disables_autosave(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[profile.p25_trunk]\n"
                             "input.source = \"rtl\"\n"
                             "input.rtl_device = 0\n"
                             "input.rtl_freq = \"100000000\"\n"
                             "trunking.enabled = true\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp profile ini\n");
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
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (state->config_autosave_enabled != 0) {
        fprintf(stderr, "expected autosave disabled for profiled config, got enabled=%d\n",
                state->config_autosave_enabled);
        test_rc = 1;
    }
    if (strcmp(state->config_autosave_path, cfg_path) != 0) {
        fprintf(stderr, "expected profiled config path retained as %s, got %s\n", cfg_path,
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
test_r_playback_optind_is_first_file_regardless_of_option_order(void) {
    int test_rc = 0;
    char wav_path_a[1024];
    char wav_path_b[1024];
    const char* tdir = test_tmp_dir();
    const char sep = test_path_sep();
    if (snprintf(wav_path_a, sizeof wav_path_a, "%s%c%s", tdir, sep, "dsdneo_cli_parse_a.wav")
            >= (int)sizeof(wav_path_a)
        || snprintf(wav_path_b, sizeof wav_path_b, "%s%c%s", tdir, sep, "dsdneo_cli_parse_b.wav")
               >= (int)sizeof(wav_path_b)) {
        fprintf(stderr, "temp path too long\n");
        return 1;
    }
    (void)remove(wav_path_a);
    (void)remove(wav_path_b);

    {
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
        dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
        if (!opts || !state) {
            free(opts);
            free(state);
            fprintf(stderr, "out of memory\n");
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
            fprintf(stderr, "expected parse continue with playfiles=1, got rc=%d playfiles=%d exit_rc=%d\n", rc,
                    opts->playfiles, exit_rc);
            test_rc = 1;
        } else if (state->optind < 1 || state->optind >= argc_effective) {
            fprintf(stderr, "invalid optind for playback: optind=%d argc_effective=%d\n", state->optind,
                    argc_effective);
            test_rc = 1;
        } else if (strcmp(argv[state->optind], "play_first.amb") != 0) {
            fprintf(stderr, "expected first playback arg to be play_first.amb, got %s\n", argv[state->optind]);
            test_rc = 1;
        }

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
            fprintf(stderr, "out of memory\n");
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
            fprintf(stderr, "expected parse continue with playfiles=1, got rc=%d playfiles=%d exit_rc=%d\n", rc,
                    opts->playfiles, exit_rc);
            test_rc = 1;
        } else if (state->optind < 1 || state->optind >= argc_effective) {
            fprintf(stderr, "invalid optind for playback: optind=%d argc_effective=%d\n", state->optind,
                    argc_effective);
            test_rc = 1;
        } else if (strcmp(argv[state->optind], "play_last.amb") != 0) {
            fprintf(stderr, "expected first playback arg to be play_last.amb, got %s\n", argv[state->optind]);
            test_rc = 1;
        }

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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char missing_path[1024];
    const char* tdir = test_tmp_dir();
    const char sep = test_path_sep();
    if (snprintf(missing_path, sizeof missing_path, "%s%c%s", tdir, sep, "dsdneo_missing_playback_input.amb")
        >= (int)sizeof(missing_path)) {
        fprintf(stderr, "temp path too long\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    (void)remove(missing_path);

    snprintf(opts->mbe_in_file, sizeof opts->mbe_in_file, "%s", missing_path);
    state->mbe_file_type = 7;
    openMbeInFile(opts, state);
    if (opts->mbe_in_f != NULL) {
        fprintf(stderr, "expected missing input open to leave mbe_in_f NULL\n");
        fclose(opts->mbe_in_f);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->mbe_file_type != -1) {
        fprintf(stderr, "expected mbe_file_type=-1 on missing input, got %d\n", state->mbe_file_type);
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
test_rdio_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->rdio_mode != DSD_RDIO_MODE_BOTH) {
        fprintf(stderr, "expected rdio_mode=%d, got %d\n", DSD_RDIO_MODE_BOTH, opts->rdio_mode);
        test_rc = 1;
    }
    if (opts->rdio_system_id != 42) {
        fprintf(stderr, "expected rdio_system_id=42, got %d\n", opts->rdio_system_id);
        test_rc = 1;
    }
    if (strcmp(opts->rdio_api_url, "http://127.0.0.1:3000") != 0) {
        fprintf(stderr, "unexpected rdio_api_url=%s\n", opts->rdio_api_url);
        test_rc = 1;
    }
    if (strcmp(opts->rdio_api_key, "test-key") != 0) {
        fprintf(stderr, "unexpected rdio_api_key=%s\n", opts->rdio_api_key);
        test_rc = 1;
    }
    if (opts->rdio_upload_timeout_ms != 2500) {
        fprintf(stderr, "expected timeout=2500, got %d\n", opts->rdio_upload_timeout_ms);
        test_rc = 1;
    }
    if (opts->rdio_upload_retries != 4) {
        fprintf(stderr, "expected retries=4, got %d\n", opts->rdio_upload_retries);
        test_rc = 1;
    }
    if (opts->rdio_api_delete_after_upload != 1) {
        fprintf(stderr, "expected rdio_api_delete_after_upload=1, got %d\n", opts->rdio_api_delete_after_upload);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->frame_log_file, "frames.log") != 0) {
        fprintf(stderr, "unexpected frame_log_file=%s\n", opts->frame_log_file);
        test_rc = 1;
    }
    if (opts->payload != 0) {
        fprintf(stderr, "expected payload to remain off, got %d\n", opts->payload);
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-i";
    char arg2[2048];
    snprintf(arg2, sizeof arg2, "%s", input_spec ? input_spec : "");
    arg2[sizeof arg2 - 1] = '\0';
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d for -i %s, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, arg2, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, arg2) != 0) {
        fprintf(stderr, "expected audio_in_dev=%s, got %s\n", arg2, opts->audio_in_dev);
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
test_iq_capture_long_options_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!opts->iq_capture_requested) {
        fprintf(stderr, "expected iq_capture_requested=1\n");
        test_rc = 1;
    }
    if (strcmp(opts->iq_capture_path, "capture.iq") != 0) {
        fprintf(stderr, "expected iq_capture_path=capture.iq, got %s\n", opts->iq_capture_path);
        test_rc = 1;
    }
    if (opts->iq_capture_format != DSD_IQ_FORMAT_CF32) {
        fprintf(stderr, "expected iq_capture_format=CF32, got %u\n", (unsigned)opts->iq_capture_format);
        test_rc = 1;
    }
    if (opts->iq_capture_max_bytes != (8ULL * 1024ULL * 1024ULL)) {
        fprintf(stderr, "expected iq_capture_max_bytes=%llu, got %llu\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        fprintf(stderr, "failed to create temporary IQ fixture\n");
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
    snprintf(arg2, sizeof arg2, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        (void)remove(metadata_path);
        (void)remove(data_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!opts->iq_replay_requested || !opts->iq_replay_loop) {
        fprintf(stderr, "expected iq replay requested+loop flags to be set\n");
        test_rc = 1;
    }
    if (opts->iq_replay_rate_mode != DSD_IQ_REPLAY_RATE_REALTIME) {
        fprintf(stderr, "expected iq_replay_rate_mode realtime, got %u\n", (unsigned)opts->iq_replay_rate_mode);
        test_rc = 1;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        fprintf(stderr, "expected audio_in_type=AUDIO_IN_RTL, got %d\n", opts->audio_in_type);
        test_rc = 1;
    }
    if (!dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev)) {
        fprintf(stderr, "expected audio_in_dev to be iqreplay spec, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U) {
        fprintf(stderr, "expected center frequency from metadata, got %u\n", opts->rtlsdr_center_freq);
        test_rc = 1;
    }
    if (!opts->iq_replay_requested || strcmp(opts->iq_replay_path, metadata_path) != 0) {
        fprintf(stderr, "expected iq_replay_path=%s, got %s\n", metadata_path, opts->iq_replay_path);
        test_rc = 1;
    }

    (void)remove(metadata_path);
    (void)remove(data_path);
    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_iq_info_returns_one_shot(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        fprintf(stderr, "failed to create temporary IQ fixture\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--iq-info";
    char arg2[1024];
    snprintf(arg2, sizeof arg2, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    test_redirect_stdout_to_null();
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ONE_SHOT || exit_rc != 0) {
        fprintf(stderr, "expected iq-info one-shot success, got rc=%d exit_rc=%d\n", rc, exit_rc);
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char metadata_path[1024];
    char data_path[1024];
    if (test_create_temp_iq_fixture(metadata_path, sizeof metadata_path, data_path, sizeof data_path) != 0) {
        fprintf(stderr, "failed to create temporary IQ fixture\n");
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
    snprintf(arg4, sizeof arg4, "%s", metadata_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        fprintf(stderr, "expected replay/capture conflict error, got rc=%d exit_rc=%d\n", rc, exit_rc);
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[64];
    snprintf(arg1, sizeof arg1, "%s", option_name ? option_name : "");
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        fprintf(stderr, "expected missing %s value error, got rc=%d exit_rc=%d\n", option_name ? option_name : "(null)",
                rc, exit_rc);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->rtl_udp_port != 9911) {
        fprintf(stderr, "expected rtl_udp_port=9911, got %d\n", opts->rtl_udp_port);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected parse error for missing --rtl-udp-control value, got rc=%d exit_rc=%d\n", rc,
                exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (opts->rtl_auto_ppm != 0) {
        fprintf(stderr, "expected --auto-ppm not to be consumed on parse error\n");
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
test_dmr_baofeng_pc5_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->baofeng_ap != 1) {
        fprintf(stderr, "expected baofeng_ap=1, got %d\n", state->baofeng_ap);
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
test_dmr_baofeng_pc5_256_long_option_decodes_hex_bytes(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);
    memset(&ctxpc5, 0, sizeof(ctxpc5));

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-baofeng-pc5";
    char arg2[] = "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->baofeng_ap != 1) {
        fprintf(stderr, "expected baofeng_ap=1, got %d\n", state->baofeng_ap);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    PC5Context expected;
    memset(&expected, 0, sizeof(expected));
    unsigned char key_bytes[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                   0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                   0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    create_keys_pc5(&expected, key_bytes, sizeof(key_bytes));
    expected.rounds = PC5_NBROUND;

    if (ctxpc5.rounds != expected.rounds || memcmp(ctxpc5.perm, expected.perm, sizeof(expected.perm)) != 0
        || memcmp(ctxpc5.new1, expected.new1, sizeof(expected.new1)) != 0
        || memcmp(ctxpc5.decal, expected.decal, sizeof(expected.decal)) != 0
        || memcmp(ctxpc5.rngxor, expected.rngxor, sizeof(expected.rngxor)) != 0
        || memcmp(ctxpc5.tab, expected.tab, sizeof(expected.tab)) != 0
        || memcmp(ctxpc5.inv, expected.inv, sizeof(expected.inv)) != 0) {
        fprintf(stderr, "expected 64-hex PC5 input to decode to 32 binary key bytes\n");
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
test_dmr_csi_ee72_long_option_parse(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    const uint8_t expected[9] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    if (state->csi_ee != 1 || memcmp(state->csi_ee_key, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "expected csi_ee=1 and parsed key bytes to match\n");
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char csv_path[1024];
    if (test_create_temp_vertex_ks_csv(csv_path, sizeof csv_path, 0) != 0) {
        freeState(state);
        free(opts);
        free(state);
        fprintf(stderr, "failed to create temp vertex csv\n");
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-vertex-ks-csv";
    char* argv[] = {arg0, arg1, csv_path, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_count != 1) {
        fprintf(stderr, "expected vertex_ks_count=1, got %d\n", state->vertex_ks_count);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_key[0] != 0x1234567891ULL || state->vertex_ks_mod[0] != 8
        || state->vertex_ks_frame_mode[0] != 1 || state->vertex_ks_frame_off[0] != 2
        || state->vertex_ks_frame_step[0] != 3) {
        fprintf(stderr, "unexpected parsed vertex mapping fields\n");
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char csv_path[1024];
    if (test_create_temp_vertex_ks_csv(csv_path, sizeof csv_path, 1) != 0) {
        freeState(state);
        free(opts);
        free(state);
        fprintf(stderr, "failed to create malformed temp vertex csv\n");
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--dmr-vertex-ks-csv";
    char* argv[] = {arg0, arg1, csv_path, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_ERROR || exit_rc != 1) {
        fprintf(stderr, "expected parse error for malformed Vertex KS CSV, got rc=%d exit_rc=%d\n", rc, exit_rc);
        (void)remove(csv_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }
    if (state->vertex_ks_count != 0) {
        fprintf(stderr, "expected vertex_ks_count=0 on malformed CSV, got %d\n", state->vertex_ks_count);
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
test_dmr_baofeng_pc5_long_option_rejects_invalid_key(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected parse error for invalid PC5 key, got rc=%d exit_rc=%d\n", rc, exit_rc);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dstar && opts->frame_x2tdma && opts->frame_p25p1 && opts->frame_p25p2 && opts->frame_nxdn48
          && opts->frame_nxdn96 && opts->frame_dmr && opts->frame_dpmr && opts->frame_provoice && opts->frame_ysf
          && opts->frame_m17)) {
        fprintf(stderr, "expected -fa to enable all digital frame types\n");
        test_rc = 1;
    }
    if (opts->pulse_digi_out_channels != 2 || opts->dmr_stereo != 1 || opts->dmr_mono != 0) {
        fprintf(stderr, "unexpected -fa audio settings channels=%d stereo=%d mono=%d\n", opts->pulse_digi_out_channels,
                opts->dmr_stereo, opts->dmr_mono);
        test_rc = 1;
    }
    if (strcmp(opts->output_name, "AUTO") != 0) {
        fprintf(stderr, "expected output_name=AUTO, got %s\n", opts->output_name);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_ysf == 1 && opts->frame_dstar == 0 && opts->frame_dmr == 0 && opts->frame_p25p1 == 0
          && opts->frame_p25p2 == 0)) {
        fprintf(stderr, "unexpected -fy frame flags\n");
        test_rc = 1;
    }
    if (opts->pulse_digi_out_channels != 1 || opts->dmr_stereo != 0 || opts->dmr_mono != 0 || state->dmr_stereo != 0) {
        fprintf(stderr, "unexpected -fy audio settings channels=%d stereo=%d mono=%d state_stereo=%d\n",
                opts->pulse_digi_out_channels, opts->dmr_stereo, opts->dmr_mono, state->dmr_stereo);
        test_rc = 1;
    }
    if (strcmp(opts->output_name, "YSF") != 0) {
        fprintf(stderr, "expected output_name=YSF, got %s\n", opts->output_name);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_f_legacy_fr_mono_still_supported(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fr";
    char* argv[] = {arg0, arg1, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(2, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_dmr == 1 && opts->dmr_mono == 1 && opts->dmr_stereo == 0 && state->dmr_stereo == 0)) {
        fprintf(stderr, "unexpected -fr mono settings frame_dmr=%d mono=%d stereo=%d state_stereo=%d\n",
                opts->frame_dmr, opts->dmr_mono, opts->dmr_stereo, state->dmr_stereo);
        test_rc = 1;
    }
    if (opts->pulse_digi_out_channels != 2 || strcmp(opts->output_name, "DMR-Mono") != 0) {
        fprintf(stderr, "unexpected -fr output channels/name channels=%d name=%s\n", opts->pulse_digi_out_channels,
                opts->output_name);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_f_nxdn48_clears_dmr_mono_after_fr(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    char arg0[] = "dsd-neo";
    char arg1[] = "-fr";
    char arg2[] = "-fi";
    char* argv[] = {arg0, arg1, arg2, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_parse_args(3, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_PARSE_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (!(opts->frame_nxdn48 == 1 && opts->frame_dmr == 0 && opts->dmr_mono == 0)) {
        fprintf(stderr, "expected -fi to clear -fr mono mode (nxdn48=%d dmr=%d mono=%d)\n", opts->frame_nxdn48,
                opts->frame_dmr, opts->dmr_mono);
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-fp";
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/input.wav") != 0) {
        fprintf(stderr, "expected config file input to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        fprintf(stderr, "expected effective file rate to stay 96000, got raw=%d effective=%d\n", opts->wav_sample_rate,
                dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        fprintf(stderr, "expected ProVoice timing to rescale to 10/4 at 96 kHz, got sps=%d center=%d\n",
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
test_s_8000_keeps_valid_symbol_timing_for_provoice(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->wav_sample_rate != 8000) {
        fprintf(stderr, "expected wav_sample_rate=8000, got %d\n", opts->wav_sample_rate);
        test_rc = 1;
    }
    if (opts->wav_interpolator != 1) {
        fprintf(stderr, "expected wav_interpolator=1 for 8 kHz input, got %d\n", opts->wav_interpolator);
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 5 || state->symbolCenter != 2) {
        fprintf(stderr, "expected ProVoice timing to remain 5/2, got sps=%d center=%d\n", state->samplesPerSymbol,
                state->symbolCenter);
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
        fprintf(stderr, "out of memory\n");
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
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_PARSE_CONTINUE, rc, exit_rc);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        fprintf(stderr, "expected 96 kHz file rate after -s, got raw=%d effective=%d\n", opts->wav_sample_rate,
                dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 20 || state->symbolCenter != 8) {
        fprintf(stderr, "expected -m3 timing override to rescale to 20/8 at 96 kHz, got sps=%d center=%d\n",
                state->samplesPerSymbol, state->symbolCenter);
        test_rc = 1;
    }

    freeState(state);
    free(opts);
    free(state);
    return test_rc;
}

static int
test_bootstrap_config_file_rate_rescales_manual_m3_override(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        free(opts);
        free(state);
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    char arg0[] = "dsd-neo";
    char arg1[] = "--config";
    char arg2[1024];
    char arg3[] = "-m3";
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(4, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/input.wav") != 0) {
        fprintf(stderr, "expected config file input to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 96000 || dsd_opts_effective_input_rate(opts) != 96000) {
        fprintf(stderr, "expected effective file rate to stay 96000, got raw=%d effective=%d\n", opts->wav_sample_rate,
                dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 20 || state->symbolCenter != 8) {
        fprintf(stderr, "expected -m3 timing override to rescale to 20/8 at 96 kHz, got sps=%d center=%d\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
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
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "pulse") != 0) {
        fprintf(stderr, "expected CLI pulse override to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 48000 || dsd_opts_effective_input_rate(opts) != 48000) {
        fprintf(stderr, "expected pulse override to keep default file rate, got raw=%d effective=%d\n",
                opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        fprintf(stderr, "expected pulse override to keep 48 kHz timing, got sps=%d center=%d\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
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
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (strcmp(opts->audio_in_dev, "/tmp/other.raw") != 0) {
        fprintf(stderr, "expected CLI file override to survive bootstrap, got %s\n", opts->audio_in_dev);
        test_rc = 1;
    }
    if (opts->wav_sample_rate != 48000 || dsd_opts_effective_input_rate(opts) != 48000) {
        fprintf(stderr, "expected CLI file override to keep default file rate, got raw=%d effective=%d\n",
                opts->wav_sample_rate, dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (opts->staged_file_sample_rate != 0) {
        fprintf(stderr, "expected CLI file override to clear staged file rate, got %d\n",
                opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (state->samplesPerSymbol != 10 || state->symbolCenter != 4) {
        fprintf(stderr, "expected CLI file override to keep 48 kHz timing, got sps=%d center=%d\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    const short samples[] = {1234, -2345, 3456, -4567};
    char raw_path[1024];
    if (test_create_temp_raw_pcm_file("dsdneo_cli_override_input", samples, sizeof samples / sizeof samples[0], ".pcm",
                                      raw_path, sizeof raw_path)
        != 0) {
        fprintf(stderr, "failed to create temp raw pcm file\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"file\"\n"
                             "file_path = \"/tmp/input.wav\"\n"
                             "file_sample_rate = 96000\n";

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
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
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    snprintf(arg4, sizeof arg4, "%s", raw_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->staged_file_sample_rate != 0) {
        fprintf(stderr, "expected CLI file override to clear staged file rate, got %d\n",
                opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (openAudioInDevice(opts, state) != 0) {
        fprintf(stderr, "expected headerless CLI file override to open successfully\n");
        test_rc = 1;
    } else {
        if (opts->audio_in_type != AUDIO_IN_WAV) {
            fprintf(stderr, "expected headerless CLI file override to open as AUDIO_IN_WAV, got %d\n",
                    opts->audio_in_type);
            test_rc = 1;
        }
        if (!opts->audio_in_file_info) {
            fprintf(stderr, "expected audio_in_file_info after headerless open\n");
            test_rc = 1;
        } else {
            if (opts->audio_in_file_info->samplerate != 48000) {
                fprintf(stderr, "expected headerless CLI file override to open at 48000 Hz, got %d\n",
                        opts->audio_in_file_info->samplerate);
                test_rc = 1;
            }
            if ((opts->audio_in_file_info->format & SF_FORMAT_TYPEMASK) != SF_FORMAT_RAW) {
                fprintf(stderr, "expected headerless CLI file override to open as raw PCM, got format=0x%x\n",
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
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    initOpts(opts);
    initState(state);

    const short samples[] = {4321, -3210, 2109, -1098};
    char raw_path[1024];
    if (test_create_temp_raw_pcm_file("dsdneo_cli_rate_override", samples, sizeof samples / sizeof samples[0], ".pcm",
                                      raw_path, sizeof raw_path)
        != 0) {
        fprintf(stderr, "failed to create temp raw pcm file\n");
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    (void)dsd_unsetenv("DSD_NEO_CONFIG");
    (void)dsd_setenv("DSD_NEO_NO_BOOTSTRAP", "1", 1);

    char ini[1536];
    snprintf(ini, sizeof ini,
             "version = 1\n"
             "\n"
             "[input]\n"
             "source = \"file\"\n"
             "file_path = \"%s\"\n"
             "file_sample_rate = 96000\n",
             raw_path);

    char cfg_path[1024];
    if (test_create_temp_ini_with_contents(ini, cfg_path, sizeof cfg_path) != 0) {
        fprintf(stderr, "failed to create temp file-input ini\n");
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
    snprintf(arg2, sizeof arg2, "%s", cfg_path);
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, NULL};

    int argc_effective = 0;
    int exit_rc = -1;
    int rc = dsd_runtime_bootstrap(5, argv, opts, state, &argc_effective, &exit_rc);
    if (rc != DSD_BOOTSTRAP_CONTINUE) {
        fprintf(stderr, "expected rc=%d, got %d (exit_rc=%d)\n", DSD_BOOTSTRAP_CONTINUE, rc, exit_rc);
        (void)remove(cfg_path);
        (void)remove(raw_path);
        freeState(state);
        free(opts);
        free(state);
        return 1;
    }

    int test_rc = 0;
    if (opts->wav_sample_rate != 44100 || dsd_opts_effective_input_rate(opts) != 44100) {
        fprintf(stderr, "expected CLI rate override to keep 44100 Hz, got raw=%d effective=%d\n", opts->wav_sample_rate,
                dsd_opts_effective_input_rate(opts));
        test_rc = 1;
    }
    if (opts->staged_file_sample_rate != 0) {
        fprintf(stderr, "expected CLI rate override to clear staged file rate, got %d\n",
                opts->staged_file_sample_rate);
        test_rc = 1;
    }
    if (openAudioInDevice(opts, state) != 0) {
        fprintf(stderr, "expected headerless CLI rate override to open successfully\n");
        test_rc = 1;
    } else {
        if (opts->audio_in_type != AUDIO_IN_WAV) {
            fprintf(stderr, "expected headerless CLI rate override to open as AUDIO_IN_WAV, got %d\n",
                    opts->audio_in_type);
            test_rc = 1;
        }
        if (!opts->audio_in_file_info) {
            fprintf(stderr, "expected audio_in_file_info after headerless open\n");
            test_rc = 1;
        } else {
            if (opts->audio_in_file_info->samplerate != 44100) {
                fprintf(stderr, "expected headerless CLI rate override to open at 44100 Hz, got %d\n",
                        opts->audio_in_file_info->samplerate);
                test_rc = 1;
            }
            if ((opts->audio_in_file_info->format & SF_FORMAT_TYPEMASK) != SF_FORMAT_RAW) {
                fprintf(stderr, "expected headerless CLI rate override to open as raw PCM, got format=0x%x\n",
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
    rc |= test_H_loads_aes256_key_for_both_slots();
    rc |= test_1_loads_rc4_key_for_both_slots_and_allows_spaces();
    rc |= test_1_loads_rc4_key_allows_0x_prefix();
    rc |= test_bootstrap_treats_lone_ini_as_config();
    rc |= test_bootstrap_print_config_normalizes_soapy_shorthand();
    rc |= test_bootstrap_profile_preserves_trunking_with_ncurses_cli();
    rc |= test_bootstrap_profile_disables_autosave();
    rc |= test_r_playback_optind_is_first_file_regardless_of_option_order();
    rc |= test_open_mbe_missing_file_leaves_stream_null();
    rc |= test_rdio_long_options_parse();
    rc |= test_frame_log_long_option_parse();
    rc |= test_input_source_soapy_roundtrip();
    rc |= test_input_source_soapy_args_roundtrip();
    rc |= test_input_source_rtl_roundtrip();
    rc |= test_input_source_rtltcp_roundtrip();
    rc |= test_iq_capture_long_options_parse();
    rc |= test_iq_capture_missing_value_returns_error();
    rc |= test_iq_capture_format_missing_value_returns_error();
    rc |= test_iq_capture_max_mb_missing_value_returns_error();
    rc |= test_iq_replay_long_options_parse();
    rc |= test_iq_replay_rate_missing_value_returns_error();
    rc |= test_iq_info_returns_one_shot();
    rc |= test_iq_info_missing_value_returns_error();
    rc |= test_iq_replay_capture_conflict_returns_error();
    rc |= test_iq_replay_missing_value_returns_error();
    rc |= test_rtl_udp_control_long_option_parse();
    rc |= test_rtl_udp_control_missing_port_returns_error();
    rc |= test_dmr_baofeng_pc5_long_option_parse();
    rc |= test_dmr_baofeng_pc5_256_long_option_decodes_hex_bytes();
    rc |= test_dmr_csi_ee72_long_option_parse();
    rc |= test_dmr_vertex_ks_csv_long_option_parse();
    rc |= test_dmr_vertex_ks_csv_long_option_rejects_malformed_csv();
    rc |= test_dmr_baofeng_pc5_long_option_rejects_invalid_key();
    rc |= test_f_auto_preset_applies_cli_profile();
    rc |= test_f_ysf_preset_applies_cli_profile();
    rc |= test_f_legacy_fr_mono_still_supported();
    rc |= test_f_nxdn48_clears_dmr_mono_after_fr();
    rc |= test_bootstrap_config_file_rate_survives_cli_provoice_preset();
    rc |= test_s_8000_keeps_valid_symbol_timing_for_provoice();
    rc |= test_m3_override_survives_file_rate_rescale_after_f2();
    rc |= test_bootstrap_config_file_rate_rescales_manual_m3_override();
    rc |= test_bootstrap_cli_pulse_override_ignores_config_file_rate_timing();
    rc |= test_bootstrap_cli_file_override_ignores_config_file_rate_timing();
    rc |= test_bootstrap_cli_file_override_uses_cli_rate_for_headerless_open();
    rc |= test_bootstrap_cli_rate_override_uses_cli_rate_for_headerless_open();
    return rc;
}
