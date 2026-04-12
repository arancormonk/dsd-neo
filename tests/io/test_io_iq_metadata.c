// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/posix_compat.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd-neo/io/iq_types.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%u want=%u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
path_join(char* out, size_t out_size, const char* a, const char* b) {
    if (!out || out_size == 0 || !a || !b) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return 0;
}

static int
write_bytes_file(const char* path, const uint8_t* bytes, size_t len) {
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    if (len > 0 && fwrite(bytes, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t n = strlen(text);
    if (fwrite(text, 1, n, fp) != n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int
mk_temp_dir(char* out_dir, size_t out_dir_size) {
    if (!out_dir || out_dir_size < 32) {
        return -1;
    }
    snprintf(out_dir, out_dir_size, "/tmp/dsdneo_iq_test_XXXXXX");
    if (!dsd_mkdtemp(out_dir)) {
        return -1;
    }
    return 0;
}

static int
write_valid_metadata(const char* metadata_path, const char* data_file_json, const char* sample_format,
                     const char* endianness, const char* capture_stage, uint32_t sample_rate_hz,
                     uint32_t base_decimation, uint32_t post_downsample, uint32_t demod_rate_hz, int contains_retunes,
                     uint64_t data_bytes) {
    char json[4096];
    snprintf(json, sizeof(json),
             "{\n"
             "  \"format\": \"dsd-neo-iq\",\n"
             "  \"version\": 1,\n"
             "  \"sample_format\": \"%s\",\n"
             "  \"iq_order\": \"IQ\",\n"
             "  \"endianness\": \"%s\",\n"
             "  \"capture_stage\": \"%s\",\n"
             "  \"sample_rate_hz\": %u,\n"
             "  \"center_frequency_hz\": 851375000,\n"
             "  \"capture_center_frequency_hz\": 851759000,\n"
             "  \"ppm\": 0,\n"
             "  \"tuner_gain_tenth_db\": 270,\n"
             "  \"rtl_dsp_bw_khz\": 48,\n"
             "  \"base_decimation\": %u,\n"
             "  \"post_downsample\": %u,\n"
             "  \"demod_rate_hz\": %u,\n"
             "  \"offset_tuning_enabled\": false,\n"
             "  \"fs4_shift_enabled\": true,\n"
             "  \"combine_rotate_enabled\": true,\n"
             "  \"muted_bytes_excluded\": true,\n"
             "  \"contains_retunes\": %s,\n"
             "  \"capture_retune_count\": %u,\n"
             "  \"source_backend\": \"rtl\",\n"
             "  \"source_args\": \"dev=0\",\n"
             "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
             "  \"data_file\": \"%s\",\n"
             "  \"data_bytes\": %" PRIu64 ",\n"
             "  \"capture_drops\": 0,\n"
             "  \"capture_drop_blocks\": 0,\n"
             "  \"input_ring_drops\": 0,\n"
             "  \"notes\": \"\"\n"
             "}\n",
             sample_format, endianness, capture_stage, sample_rate_hz, base_decimation, post_downsample, demod_rate_hz,
             contains_retunes ? "true" : "false", contains_retunes ? 1U : 0U, data_file_json, data_bytes);
    return write_text_file(metadata_path, json);
}

static int
test_metadata_round_trip_capture_open_close(void) {
    int rc = 0;
    char dir[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr, "failed to create temp dir\n");
        return 1;
    }

    char data_path[512];
    char metadata_path[512];
    path_join(data_path, sizeof(data_path), dir, "capture.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "capture.iq.json");

    dsd_iq_capture_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    snprintf(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    snprintf(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.ppm = 0;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32;
    cfg.post_downsample = 1;
    cfg.demod_rate_hz = 48000;
    cfg.offset_tuning_enabled = 0;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    snprintf(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    snprintf(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=\\\"0\\\"\\\\tcp");

    dsd_iq_capture_writer* writer = NULL;
    char err[256];
    int open_rc = dsd_iq_capture_open(&cfg, &writer, err, sizeof(err));
    rc |= expect_int("capture open", open_rc, DSD_IQ_OK);
    rc |= expect_true("writer non-null", writer != NULL);
    if (!writer) {
        return rc;
    }

    uint8_t bytes[3] = {0x10, 0x20, 0x30};
    rc |= expect_int("capture submit odd bytes", dsd_iq_capture_submit(writer, bytes, sizeof(bytes)), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.input_ring_drops = 7;
    stats.retune_count = 2;
    dsd_iq_capture_close(writer, &stats);

    struct stat st;
    rc |= expect_true("data file stat", stat(data_path, &st) == 0);
    if (stat(data_path, &st) == 0) {
        rc |= expect_true("data size aligned", ((uint64_t)st.st_size % 2ULL) == 0ULL);
    }

    dsd_iq_replay_config replay_cfg;
    memset(&replay_cfg, 0, sizeof(replay_cfg));
    int prc = dsd_iq_replay_read_metadata(metadata_path, &replay_cfg, err, sizeof(err));
    rc |= expect_int("replay read metadata", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_int("format cu8", (int)replay_cfg.format, (int)DSD_IQ_FORMAT_CU8);
        rc |= expect_u32("sample_rate_hz", replay_cfg.sample_rate_hz, 1536000);
        rc |= expect_u32("base_decimation", replay_cfg.base_decimation, 32);
        rc |= expect_u32("post_downsample", replay_cfg.post_downsample, 1);
        rc |= expect_u32("demod_rate_hz", replay_cfg.demod_rate_hz, 48000);
        rc |= expect_u64("input_ring_drops", replay_cfg.input_ring_drops, 7);
        rc |= expect_int("contains_retunes", replay_cfg.contains_retunes, 1);
        rc |= expect_u32("capture_retune_count", replay_cfg.capture_retune_count, 2);
        rc |= expect_true("resolved data path uses metadata directory",
                          strstr(replay_cfg.data_path, "/capture.iq") != NULL);
        rc |= expect_true("capture_drops reflects odd-byte carry drop", replay_cfg.capture_drops >= 1);
    }

    return rc;
}

static int
test_missing_field_reports_clear_error(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    char meta[512];
    char data[512];
    path_join(meta, sizeof(meta), dir, "missing.iq.json");
    path_join(data, sizeof(data), dir, "missing.iq");

    uint8_t bytes[2] = {1, 2};
    if (write_bytes_file(data, bytes, sizeof(bytes)) != 0) {
        return 1;
    }

    const char* json_missing = "{\n"
                               "  \"format\": \"dsd-neo-iq\",\n"
                               "  \"version\": 1,\n"
                               "  \"sample_format\": \"cu8\",\n"
                               "  \"iq_order\": \"IQ\",\n"
                               "  \"endianness\": \"none\",\n"
                               "  \"capture_stage\": \"post_mute_pre_widen\",\n"
                               "  \"center_frequency_hz\": 851375000,\n"
                               "  \"capture_center_frequency_hz\": 851759000,\n"
                               "  \"ppm\": 0,\n"
                               "  \"tuner_gain_tenth_db\": 270,\n"
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
                               "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                               "  \"data_file\": \"missing.iq\",\n"
                               "  \"data_bytes\": 2,\n"
                               "  \"capture_drops\": 0,\n"
                               "  \"capture_drop_blocks\": 0,\n"
                               "  \"input_ring_drops\": 0,\n"
                               "  \"notes\": \"\"\n"
                               "}\n";
    if (write_text_file(meta, json_missing) != 0) {
        return 1;
    }

    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
    rc |= expect_int("missing field parse rc", prc, DSD_IQ_ERR_INVALID_META);
    rc |= expect_true("missing field message", strstr(err, "sample_rate_hz") != NULL);
    return rc;
}

static int
test_json_unescape_and_control_rejection(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char subdir[512];
    path_join(subdir, sizeof(subdir), dir, "sub");
    if (dsd_mkdir(subdir, 0700) != 0) {
        return 1;
    }

    char meta[512];
    char data[512];
    path_join(meta, sizeof(meta), dir, "escaped.iq.json");
    path_join(data, sizeof(data), subdir, "test.iq");
    uint8_t b[2] = {1, 2};
    if (write_bytes_file(data, b, sizeof(b)) != 0) {
        return 1;
    }

    const char* json_escaped = "{\n"
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
                               "  \"tuner_gain_tenth_db\": 270,\n"
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
                               "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                               "  \"data_file\": \"sub\\/test.iq\",\n"
                               "  \"data_bytes\": 2,\n"
                               "  \"capture_drops\": 0,\n"
                               "  \"capture_drop_blocks\": 0,\n"
                               "  \"input_ring_drops\": 0,\n"
                               "  \"notes\": \"q=\\\" b=\\\\ s=\\/ n=\\n t=\\t u=\\u001f\"\n"
                               "}\n";
    if (write_text_file(meta, json_escaped) != 0) {
        return 1;
    }

    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
    rc |= expect_int("escaped metadata parses", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_true("notes unescaped quote", strstr(cfg.notes, "q=\"") != NULL);
        rc |= expect_true("notes unescaped slash", strstr(cfg.notes, "s=/") != NULL);
        rc |= expect_true("resolved relative data path", strstr(cfg.data_path, "/sub/test.iq") != NULL);
    }

    char bad1[512];
    path_join(bad1, sizeof(bad1), dir, "bad_control.iq.json");
    const char* bad_control = "{\n"
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
                              "  \"tuner_gain_tenth_db\": 270,\n"
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
                              "  \"source_backend\": \"rt\\u0001l\",\n"
                              "  \"source_args\": \"dev=0\",\n"
                              "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                              "  \"data_file\": \"sub/test.iq\",\n"
                              "  \"data_bytes\": 2,\n"
                              "  \"capture_drops\": 0,\n"
                              "  \"capture_drop_blocks\": 0,\n"
                              "  \"input_ring_drops\": 0,\n"
                              "  \"notes\": \"\"\n"
                              "}\n";
    write_text_file(bad1, bad_control);
    memset(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(bad1, &cfg, err, sizeof(err));
    rc |= expect_int("operational control-byte reject", prc, DSD_IQ_ERR_INVALID_META);

    char bad2[512];
    path_join(bad2, sizeof(bad2), dir, "bad_nul.iq.json");
    const char* bad_nul = "{\n"
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
                          "  \"tuner_gain_tenth_db\": 270,\n"
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
                          "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                          "  \"data_file\": \"sub\\u0000/test.iq\",\n"
                          "  \"data_bytes\": 2,\n"
                          "  \"capture_drops\": 0,\n"
                          "  \"capture_drop_blocks\": 0,\n"
                          "  \"input_ring_drops\": 0,\n"
                          "  \"notes\": \"\"\n"
                          "}\n";
    write_text_file(bad2, bad_nul);
    memset(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(bad2, &cfg, err, sizeof(err));
    rc |= expect_int("embedded nul reject", prc, DSD_IQ_ERR_INVALID_META);

    return rc;
}

static int
test_invalid_json_shapes_and_number_types(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char data[512];
    path_join(data, sizeof(data), dir, "x.iq");
    uint8_t b[2] = {1, 2};
    write_bytes_file(data, b, sizeof(b));

    char trunc_meta[512];
    path_join(trunc_meta, sizeof(trunc_meta), dir, "trunc.iq.json");
    write_text_file(trunc_meta, "{\"format\":\"dsd-neo-iq\"");
    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(trunc_meta, &cfg, err, sizeof(err));
    rc |= expect_int("truncated json reject", prc, DSD_IQ_ERR_INVALID_META);

    char nested_meta[512];
    path_join(nested_meta, sizeof(nested_meta), dir, "nested.iq.json");
    const char* nested_json = "{\n"
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
                              "  \"tuner_gain_tenth_db\": 270,\n"
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
                              "  \"source_backend\": {\"nested\": true},\n"
                              "  \"source_args\": \"dev=0\",\n"
                              "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                              "  \"data_file\": \"x.iq\",\n"
                              "  \"data_bytes\": 2,\n"
                              "  \"capture_drops\": 0,\n"
                              "  \"capture_drop_blocks\": 0,\n"
                              "  \"input_ring_drops\": 0,\n"
                              "  \"notes\": \"\"\n"
                              "}\n";
    write_text_file(nested_meta, nested_json);
    memset(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(nested_meta, &cfg, err, sizeof(err));
    rc |= expect_int("nested object reject", prc, DSD_IQ_ERR_INVALID_META);

    char float_meta[512];
    path_join(float_meta, sizeof(float_meta), dir, "float.iq.json");
    const char* float_json = "{\n"
                             "  \"format\": \"dsd-neo-iq\",\n"
                             "  \"version\": 1.5,\n"
                             "  \"sample_format\": \"cu8\",\n"
                             "  \"iq_order\": \"IQ\",\n"
                             "  \"endianness\": \"none\",\n"
                             "  \"capture_stage\": \"post_mute_pre_widen\",\n"
                             "  \"sample_rate_hz\": 1536000,\n"
                             "  \"center_frequency_hz\": 851375000,\n"
                             "  \"capture_center_frequency_hz\": 851759000,\n"
                             "  \"ppm\": 0,\n"
                             "  \"tuner_gain_tenth_db\": 270,\n"
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
                             "  \"capture_started_utc\": \"2026-04-11T12:34:56Z\",\n"
                             "  \"data_file\": \"x.iq\",\n"
                             "  \"data_bytes\": 2,\n"
                             "  \"capture_drops\": 0,\n"
                             "  \"capture_drop_blocks\": 0,\n"
                             "  \"input_ring_drops\": 0,\n"
                             "  \"notes\": \"\"\n"
                             "}\n";
    write_text_file(float_meta, float_json);
    memset(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(float_meta, &cfg, err, sizeof(err));
    rc |= expect_int("non-integer field reject", prc, DSD_IQ_ERR_INVALID_META);

    return rc;
}

static int
test_rate_chain_validation(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char data[512];
    path_join(data, sizeof(data), dir, "r.iq");
    uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    write_bytes_file(data, b, sizeof(b));

    char m1[512];
    char m2[512];
    char m3[512];
    path_join(m1, sizeof(m1), dir, "rate1.iq.json");
    path_join(m2, sizeof(m2), dir, "rate2.iq.json");
    path_join(m3, sizeof(m3), dir, "rate3.iq.json");

    write_valid_metadata(m1, "r.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 3, 1, 512000, 0, 8);
    write_valid_metadata(m2, "r.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 0, 0, 0, 8);
    write_valid_metadata(m3, "r.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 12345, 0, 8);

    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(m1, &cfg, err, sizeof(err));
    rc |= expect_int("non-power-two base_decimation", prc, DSD_IQ_ERR_RATE_CHAIN);
    prc = dsd_iq_replay_read_metadata(m2, &cfg, err, sizeof(err));
    rc |= expect_int("zero post_downsample", prc, DSD_IQ_ERR_RATE_CHAIN);
    prc = dsd_iq_replay_read_metadata(m3, &cfg, err, sizeof(err));
    rc |= expect_int("demod mismatch", prc, DSD_IQ_ERR_RATE_CHAIN);
    return rc;
}

static int
test_relative_data_resolution_info_and_open_validation(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char subdir[512];
    path_join(subdir, sizeof(subdir), dir, "meta");
    if (dsd_mkdir(subdir, 0700) != 0) {
        return 1;
    }

    char meta[512];
    char data[512];
    path_join(meta, sizeof(meta), subdir, "capture.iq.json");
    path_join(data, sizeof(data), subdir, "capture.iq");
    uint8_t buf[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    write_bytes_file(data, buf, sizeof(buf));
    write_valid_metadata(meta, "capture.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 48000, 0, 12);

    dsd_iq_replay_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
    rc |= expect_int("relative data path parse", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_true("relative data path resolved under metadata dir", strcmp(cfg.data_path, data) == 0);
    }

    FILE* out = tmpfile();
    FILE* warn = tmpfile();
    if (!out || !warn) {
        if (out) {
            fclose(out);
        }
        if (warn) {
            fclose(warn);
        }
        return 1;
    }
    rc |= expect_int("info print", dsd_iq_info_print(&cfg, meta, 10, out, warn), DSD_IQ_OK);
    fflush(out);
    fseek(out, 0, SEEK_SET);
    char out_buf[4096];
    size_t out_n = fread(out_buf, 1, sizeof(out_buf) - 1, out);
    out_buf[out_n] = '\0';
    rc |= expect_true("info has header", strstr(out_buf, "IQ Capture Info:") != NULL);
    rc |= expect_true("info has replay compatibility", strstr(out_buf, "Replay compatible:") != NULL);

    fflush(warn);
    fseek(warn, 0, SEEK_SET);
    char warn_buf[1024];
    size_t warn_n = fread(warn_buf, 1, sizeof(warn_buf) - 1, warn);
    warn_buf[warn_n] = '\0';
    rc |= expect_true("info mismatch warning present", strstr(warn_buf, "metadata data_bytes") != NULL);
    fclose(out);
    fclose(warn);

    char bad_meta[512];
    char bad_data[512];
    path_join(bad_meta, sizeof(bad_meta), subdir, "bad_align.iq.json");
    path_join(bad_data, sizeof(bad_data), subdir, "bad_align.iq");
    uint8_t one[1] = {7};
    write_bytes_file(bad_data, one, sizeof(one));
    write_valid_metadata(bad_meta, "bad_align.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 48000, 0, 1);
    dsd_iq_replay_source* src = NULL;
    prc = dsd_iq_replay_open(bad_meta, &cfg, &src, err, sizeof(err));
    rc |= expect_int("open rejects zero effective bytes", prc, DSD_IQ_ERR_ALIGNMENT);
    rc |= expect_true("source not created on failure", src == NULL);

    char retune_meta[512];
    path_join(retune_meta, sizeof(retune_meta), subdir, "retune.iq.json");
    write_valid_metadata(retune_meta, "capture.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 48000, 1, 10);
    prc = dsd_iq_replay_open(retune_meta, &cfg, &src, err, sizeof(err));
    rc |= expect_int("open rejects contains_retunes", prc, DSD_IQ_ERR_RETUNE_REJECT);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_metadata_round_trip_capture_open_close();
    rc |= test_missing_field_reports_clear_error();
    rc |= test_json_unescape_and_control_rejection();
    rc |= test_invalid_json_shapes_and_number_types();
    rc |= test_rate_chain_validation();
    rc |= test_relative_data_resolution_info_and_open_validation();
    return rc ? 1 : 0;
}
