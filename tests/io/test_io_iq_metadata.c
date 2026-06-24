// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "test_support.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%u want=%u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
path_join(char* out, size_t out_size, const char* a, const char* b) {
    return dsd_test_path_join(out, out_size, a, b);
}

static int
write_bytes_file(const char* path, const uint8_t* bytes, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
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
    FILE* fp = dsd_fopen_private(path, "wb");
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
    if (!out_dir || out_dir_size == 0) {
        return -1;
    }
    if (!dsd_test_mkdtemp(out_dir, out_dir_size, "dsdneo_iq_test")) {
        return -1;
    }
    return 0;
}

static const char*
path_last_sep(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    const char* bslash = path ? strrchr(path, '\\') : NULL;
    if (bslash && (!slash || bslash > slash)) {
        return bslash;
    }
    return slash;
}

static int
path_leaf_is(const char* path, const char* leaf) {
    const char* sep = path_last_sep(path);
    const char* got = sep ? sep + 1 : path;
    return got && leaf && strcmp(got, leaf) == 0;
}

static int
path_ends_with_components(const char* path, const char* parent, const char* leaf) {
    const char* sep = path_last_sep(path);
    if (!sep || !path_leaf_is(path, leaf)) {
        return 0;
    }
    size_t parent_len = strlen(parent);
    const char* parent_end = sep;
    while (parent_end > path && dsd_test_is_path_sep(parent_end[-1])) {
        parent_end--;
    }
    if ((size_t)(parent_end - path) < parent_len) {
        return 0;
    }
    const char* parent_start = parent_end - parent_len;
    if (strncmp(parent_start, parent, parent_len) != 0) {
        return 0;
    }
    return parent_start == path || dsd_test_is_path_sep(parent_start[-1]);
}

static int
write_valid_metadata(const char* metadata_path, const char* data_file_json, const char* sample_format,
                     const char* endianness, const char* capture_stage, uint32_t sample_rate_hz,
                     uint32_t base_decimation, uint32_t post_downsample, uint32_t demod_rate_hz, int contains_retunes,
                     uint64_t data_bytes) {
    char json[4096];
    DSD_SNPRINTF(json, sizeof(json),
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
                 sample_format, endianness, capture_stage, sample_rate_hz, base_decimation, post_downsample,
                 demod_rate_hz, contains_retunes ? "true" : "false", contains_retunes ? 1U : 0U, data_file_json,
                 data_bytes);
    return write_text_file(metadata_path, json);
}

static int
write_metadata_with_events_version(const char* metadata_path, const char* data_file_json, const char* events_json,
                                   uint64_t data_bytes, int contains_retunes, uint32_t version) {
    char json[8192];
    DSD_SNPRINTF(json, sizeof(json),
                 "{\n"
                 "  \"format\": \"dsd-neo-iq\",\n"
                 "  \"version\": %u,\n"
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
                 "  \"notes\": \"\",\n"
                 "  \"events\": %s\n"
                 "}\n",
                 version, contains_retunes ? "true" : "false", contains_retunes ? 1U : 0U, data_file_json, data_bytes,
                 events_json);
    return write_text_file(metadata_path, json);
}

static int
write_v2_metadata_with_events(const char* metadata_path, const char* data_file_json, const char* events_json,
                              uint64_t data_bytes, int contains_retunes) {
    return write_metadata_with_events_version(metadata_path, data_file_json, events_json, data_bytes, contains_retunes,
                                              2U);
}

static int
test_metadata_round_trip_capture_open_close(void) {
    int rc = 0;
    char dir[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        DSD_FPRINTF(stderr, "failed to create temp dir\n");
        return 1;
    }

    char data_path[512];
    char metadata_path[512];
    path_join(data_path, sizeof(data_path), dir, "capture.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "capture.iq.json");

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
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
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=\\\"0\\\"\\\\tcp");

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
    DSD_MEMSET(&stats, 0, sizeof(stats));
    stats.input_ring_drops = 7;
    dsd_iq_capture_close(writer, &stats);

    dsd_stat_t st;
    rc |= expect_true("data file stat", dsd_stat_path(data_path, &st) == 0);
    if (dsd_stat_path(data_path, &st) == 0) {
        rc |= expect_true("data size aligned", ((uint64_t)st.st_size % 2ULL) == 0ULL);
    }

    dsd_iq_replay_config replay_cfg;
    DSD_MEMSET(&replay_cfg, 0, sizeof(replay_cfg));
    int prc = dsd_iq_replay_read_metadata(metadata_path, &replay_cfg, err, sizeof(err));
    rc |= expect_int("replay read metadata", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_int("format cu8", (int)replay_cfg.format, (int)DSD_IQ_FORMAT_CU8);
        rc |= expect_u32("sample_rate_hz", replay_cfg.sample_rate_hz, 1536000);
        rc |= expect_u32("base_decimation", replay_cfg.base_decimation, 32);
        rc |= expect_u32("post_downsample", replay_cfg.post_downsample, 1);
        rc |= expect_u32("demod_rate_hz", replay_cfg.demod_rate_hz, 48000);
        rc |= expect_u64("input_ring_drops", replay_cfg.input_ring_drops, 7);
        rc |= expect_u32("metadata_version", replay_cfg.metadata_version, 1);
        rc |= expect_int("contains_retunes", replay_cfg.contains_retunes, 0);
        rc |= expect_u32("capture_retune_count", replay_cfg.capture_retune_count, 0);
        rc |= expect_true("resolved data path uses metadata directory", strcmp(replay_cfg.data_path, data_path) == 0);
        rc |= expect_true("capture_drops reflects odd-byte carry drop", replay_cfg.capture_drops >= 1);
    }
    dsd_iq_replay_config_clear(&replay_cfg);

    return rc;
}

static int
test_metadata_v2_events_round_trip(void) {
    int rc = 0;
    char dir[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char data_path[512];
    char metadata_path[512];
    path_join(data_path, sizeof(data_path), dir, "events.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "events.iq.json");

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = 1536000;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32;
    cfg.post_downsample = 1;
    cfg.demod_rate_hz = 48000;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");

    dsd_iq_capture_writer* writer = NULL;
    char err[256] = {0};
    rc |= expect_int("event capture open", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t first[4] = {1, 2, 3, 4};
    uint8_t second[2] = {5, 6};
    rc |= expect_int("event submit first", dsd_iq_capture_submit(writer, first, sizeof(first)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = 1536000;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |= expect_int("record retune event", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 4;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "retune_mute");
    rc |= expect_int("record mute event", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RESET;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = 1536000;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |= expect_int("record reset event", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    rc |= expect_int("event submit second", dsd_iq_capture_submit(writer, second, sizeof(second)), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config replay_cfg;
    DSD_MEMSET(&replay_cfg, 0, sizeof(replay_cfg));
    int prc = dsd_iq_replay_read_metadata(metadata_path, &replay_cfg, err, sizeof(err));
    rc |= expect_int("v2 replay read metadata", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("v2 metadata_version", replay_cfg.metadata_version, 2);
        rc |= expect_u32("v2 event_count", replay_cfg.event_count, 3);
        rc |= expect_int("v2 contains_retunes", replay_cfg.contains_retunes, 1);
        rc |= expect_u32("v2 capture_retune_count", replay_cfg.capture_retune_count, 1);
        rc |= expect_int("event0 kind", (int)replay_cfg.events[0].kind, (int)DSD_IQ_EVENT_RETUNE);
        rc |= expect_u64("event0 offset", replay_cfg.events[0].byte_offset, 4);
        rc |= expect_u64("event0 center", replay_cfg.events[0].center_frequency_hz, 851500000ULL);
        rc |= expect_int("event1 kind", (int)replay_cfg.events[1].kind, (int)DSD_IQ_EVENT_MUTE);
        rc |= expect_u64("event1 duration", replay_cfg.events[1].duration_bytes, 4);
        rc |= expect_int("event2 kind", (int)replay_cfg.events[2].kind, (int)DSD_IQ_EVENT_RESET);
    }
    dsd_iq_replay_config_clear(&replay_cfg);

    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&replay_cfg, 0, sizeof(replay_cfg));
    prc = dsd_iq_replay_open(metadata_path, &replay_cfg, &src, err, sizeof(err));
    rc |= expect_int("v2 replay open", prc, DSD_IQ_OK);
    if (src) {
        dsd_iq_replay_close(src);
    }
    dsd_iq_replay_config_clear(&replay_cfg);
    return rc;
}

static int
test_metadata_reuse_after_explicit_clear(void) {
    int rc = 0;
    char dir[256];
    char err[256] = {0};
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char data_path[512];
    char meta1[512];
    char meta2[512];
    path_join(data_path, sizeof(data_path), dir, "events.iq");
    path_join(meta1, sizeof(meta1), dir, "events1.iq.json");
    path_join(meta2, sizeof(meta2), dir, "events2.iq.json");

    uint8_t bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    rc |= expect_int("write reuse data", write_bytes_file(data_path, bytes, sizeof(bytes)), 0);
    rc |= expect_int("write first reuse metadata",
                     write_v2_metadata_with_events(
                         meta1, "events.iq",
                         "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"first\"}]", 8, 0),
                     0);
    rc |= expect_int("write second reuse metadata",
                     write_v2_metadata_with_events(
                         meta2, "events.iq",
                         "[{\"kind\":\"MUTE\",\"byte_offset\":4,\"duration_bytes\":4,\"reason\":\"second\"}]", 8, 0),
                     0);
    if (rc != 0) {
        return rc;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta1, &cfg, err, sizeof(err));
    rc |= expect_int("first metadata reuse read", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("first reuse event_count", cfg.event_count, 1);
        rc |= expect_u64("first reuse duration", cfg.events[0].duration_bytes, 2);
    }

    dsd_iq_replay_config_clear(&cfg);
    prc = dsd_iq_replay_read_metadata(meta2, &cfg, err, sizeof(err));
    rc |= expect_int("second metadata reuse read", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("second reuse event_count", cfg.event_count, 1);
        rc |= expect_u64("second reuse offset", cfg.events[0].byte_offset, 4);
        rc |= expect_u64("second reuse duration", cfg.events[0].duration_bytes, 4);
    }
    dsd_iq_replay_config_clear(&cfg);

    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(meta1, &cfg, &src, err, sizeof(err));
    rc |= expect_int("first open reuse read", prc, DSD_IQ_OK);
    dsd_iq_replay_close(src);
    src = NULL;
    dsd_iq_replay_config_clear(&cfg);
    prc = dsd_iq_replay_open(meta2, &cfg, &src, err, sizeof(err));
    rc |= expect_int("second open reuse read", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("second open reuse event_count", cfg.event_count, 1);
        rc |= expect_u64("second open reuse duration", cfg.events[0].duration_bytes, 4);
    }
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

static int
test_metadata_output_is_write_only_on_first_success(void) {
    int rc = 0;
    char dir[256];
    char err[256] = {0};
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char data_path[512];
    char meta[512];
    path_join(data_path, sizeof(data_path), dir, "events.iq");
    path_join(meta, sizeof(meta), dir, "events.iq.json");

    uint8_t bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    rc |= expect_int("write write-only data", write_bytes_file(data_path, bytes, sizeof(bytes)), 0);
    rc |= expect_int("write write-only metadata",
                     write_v2_metadata_with_events(
                         meta, "events.iq",
                         "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"first\"}]", 8, 0),
                     0);
    if (rc != 0) {
        return rc;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0xA5, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
    rc |= expect_int("metadata read ignores prior output bytes", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("write-only read event_count", cfg.event_count, 1);
        dsd_iq_replay_config_clear(&cfg);
    }

    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&cfg, 0xA5, sizeof(cfg));
    prc = dsd_iq_replay_open(meta, &cfg, &src, err, sizeof(err));
    rc |= expect_int("replay open ignores prior output bytes", prc, DSD_IQ_OK);
    if (src) {
        dsd_iq_replay_close(src);
    }
    if (prc == DSD_IQ_OK) {
        rc |= expect_u32("write-only open event_count", cfg.event_count, 1);
        dsd_iq_replay_config_clear(&cfg);
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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

    /*
     * Use a metadata file beside a nested data file so unescaping and relative
     * path resolution are covered in the same parse. The rejection cases below
     * then prove decoded control bytes cannot reach operational string fields.
     */
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
    rc |= expect_int("escaped metadata parses", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_true("notes unescaped quote", strstr(cfg.notes, "q=\"") != NULL);
        rc |= expect_true("notes unescaped slash", strstr(cfg.notes, "s=/") != NULL);
        rc |= expect_true("resolved relative data path", path_ends_with_components(cfg.data_path, "sub", "test.iq"));
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(bad1, &cfg, err, sizeof(err));
    rc |= expect_int("operational control-byte reject", prc, DSD_IQ_ERR_INVALID_META);

    // Embedded NULs in paths must be rejected after JSON unescape, not truncated.
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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

    /*
     * Metadata rejects syntax errors, nested values where strings are required,
     * and floating-point numbers in integer-only fields. These cases protect the
     * replay loader from silently coercing malformed capture descriptors.
     */
    char data[512];
    path_join(data, sizeof(data), dir, "x.iq");
    uint8_t b[2] = {1, 2};
    write_bytes_file(data, b, sizeof(b));

    char trunc_meta[512];
    path_join(trunc_meta, sizeof(trunc_meta), dir, "trunc.iq.json");
    write_text_file(trunc_meta, "{\"format\":\"dsd-neo-iq\"");
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(float_meta, &cfg, err, sizeof(err));
    rc |= expect_int("non-integer field reject", prc, DSD_IQ_ERR_INVALID_META);

    return rc;
}

static int
test_event_timeline_validation(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    /*
     * Event metadata is parsed before replay open, so this test separates schema
     * validation from replay compatibility checks. Invalid timelines reject at
     * metadata load time; retune summaries reject later when replay is opened.
     */
    char data[512];
    path_join(data, sizeof(data), dir, "events_bad.iq");
    uint8_t bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    write_bytes_file(data, bytes, sizeof(bytes));

    struct {
        const char* name;
        const char* events_json;
        uint64_t data_bytes;
        int want;
    } cases[] = {
        {"unknown_kind", "[{\"kind\":\"BOGUS\",\"byte_offset\":2,\"reason\":\"x\"}]", 8, DSD_IQ_ERR_INVALID_META},
        {"missing_duration", "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"reason\":\"x\"}]", 8, DSD_IQ_ERR_INVALID_META},
        {"negative_duration", "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":-2,\"reason\":\"x\"}]", 8,
         DSD_IQ_ERR_INVALID_META},
        {"negative_offset", "[{\"kind\":\"MUTE\",\"byte_offset\":-2,\"duration_bytes\":2,\"reason\":\"x\"}]", 8,
         DSD_IQ_ERR_INVALID_META},
        {"unsorted_offsets",
         "[{\"kind\":\"MUTE\",\"byte_offset\":4,\"duration_bytes\":2,\"reason\":\"x\"},"
         "{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"x\"}]",
         8, DSD_IQ_ERR_INVALID_META},
        {"offset_past_eof", "[{\"kind\":\"MUTE\",\"byte_offset\":10,\"duration_bytes\":2,\"reason\":\"x\"}]", 8,
         DSD_IQ_ERR_INVALID_META},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char meta[512];
        char file_name[64];
        DSD_SNPRINTF(file_name, sizeof(file_name), "%s.iq.json", cases[i].name);
        path_join(meta, sizeof(meta), dir, file_name);
        if (write_v2_metadata_with_events(meta, "events_bad.iq", cases[i].events_json, cases[i].data_bytes, 0) != 0) {
            return 1;
        }
        dsd_iq_replay_config cfg;
        DSD_MEMSET(&cfg, 0, sizeof(cfg));
        int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
        rc |= expect_int(cases[i].name, prc, cases[i].want);
        dsd_iq_replay_config_clear(&cfg);
    }

    // Version 1 files cannot opt into event arrays after a successful parse.
    {
        char meta[512];
        path_join(meta, sizeof(meta), dir, "events_on_v1.iq.json");
        if (write_metadata_with_events_version(
                meta, "events_bad.iq", "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"x\"}]",
                8, 0, 1U)
            != 0) {
            return 1;
        }
        dsd_iq_replay_config cfg;
        DSD_MEMSET(&cfg, 0, sizeof(cfg));
        int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
        rc |= expect_int("v1 events rejected after parse", prc, DSD_IQ_ERR_UNSUPPORTED_VER);
        dsd_iq_replay_config_clear(&cfg);
    }

    {
        char meta[512];
        char json[8192];
        path_join(meta, sizeof(meta), dir, "events_then_bad_field.iq.json");
        DSD_SNPRINTF(json, sizeof(json),
                     "{\n"
                     "  \"format\": \"dsd-neo-iq\",\n"
                     "  \"version\": 2,\n"
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
                     "  \"data_file\": \"events_bad.iq\",\n"
                     "  \"data_bytes\": 8,\n"
                     "  \"capture_drops\": 0,\n"
                     "  \"capture_drop_blocks\": 0,\n"
                     "  \"input_ring_drops\": 0,\n"
                     "  \"notes\": \"\",\n"
                     "  \"events\": [{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"x\"}],\n"
                     "  \"sample_rate_hz\": \"bad\"\n"
                     "}\n");
        if (write_text_file(meta, json) != 0) {
            return 1;
        }
        dsd_iq_replay_config cfg;
        DSD_MEMSET(&cfg, 0, sizeof(cfg));
        int prc = dsd_iq_replay_read_metadata(meta, &cfg, err, sizeof(err));
        rc |= expect_int("events parsed before malformed field", prc, DSD_IQ_ERR_INVALID_META);
        dsd_iq_replay_config_clear(&cfg);
    }

    char retuned_v1[512];
    path_join(retuned_v1, sizeof(retuned_v1), dir, "retuned_v1.iq.json");
    write_valid_metadata(retuned_v1, "events_bad.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 48000, 1,
                         8);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_read_metadata(retuned_v1, &cfg, err, sizeof(err));
    rc |= expect_int("retuned v1 metadata-only read allowed", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_int("retuned v1 metadata contains retunes", cfg.contains_retunes, 1);
        rc |= expect_u32("retuned v1 metadata event_count", cfg.event_count, 0);
    }
    dsd_iq_replay_config_clear(&cfg);
    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(retuned_v1, &cfg, &src, err, sizeof(err));
    rc |= expect_int("retuned v1 replay open rejected", prc, DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retuned v1 rejected source null", src == NULL);
    dsd_iq_replay_config_clear(&cfg);

    // Replay open rejects retune-bearing captures even when metadata reads cleanly.
    char retuned_mute_only[512];
    path_join(retuned_mute_only, sizeof(retuned_mute_only), dir, "retuned_mute_only.iq.json");
    if (write_v2_metadata_with_events(retuned_mute_only, "events_bad.iq",
                                      "[{\"kind\":\"MUTE\",\"byte_offset\":2,\"duration_bytes\":2,\"reason\":\"x\"}]",
                                      8, 1)
        != 0) {
        return 1;
    }
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(retuned_mute_only, &cfg, err, sizeof(err));
    rc |= expect_int("retuned mute-only metadata read allowed", prc, DSD_IQ_OK);
    dsd_iq_replay_config_clear(&cfg);
    src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(retuned_mute_only, &cfg, &src, err, sizeof(err));
    rc |= expect_int("retuned mute-only replay open rejected", prc, DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retuned mute-only rejected source null", src == NULL);
    dsd_iq_replay_config_clear(&cfg);

    char retuned_retune_only[512];
    path_join(retuned_retune_only, sizeof(retuned_retune_only), dir, "retuned_retune_only.iq.json");
    if (write_v2_metadata_with_events(
            retuned_retune_only, "events_bad.iq",
            "[{\"kind\":\"RETUNE\",\"byte_offset\":2,\"center_frequency_hz\":851500000,"
            "\"capture_center_frequency_hz\":851884000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"}]",
            8, 1)
        != 0) {
        return 1;
    }
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(retuned_retune_only, &cfg, err, sizeof(err));
    rc |= expect_int("retuned retune-only metadata read allowed", prc, DSD_IQ_OK);
    dsd_iq_replay_config_clear(&cfg);
    src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(retuned_retune_only, &cfg, &src, err, sizeof(err));
    rc |= expect_int("retuned retune-only replay open rejected", prc, DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retuned retune-only rejected source null", src == NULL);
    dsd_iq_replay_config_clear(&cfg);

    char retune_flag_false[512];
    path_join(retune_flag_false, sizeof(retune_flag_false), dir, "retune_flag_false.iq.json");
    if (write_v2_metadata_with_events(
            retune_flag_false, "events_bad.iq",
            "[{\"kind\":\"RETUNE\",\"byte_offset\":2,\"center_frequency_hz\":851500000,"
            "\"capture_center_frequency_hz\":851884000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"}]",
            8, 0)
        != 0) {
        return 1;
    }
    src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(retune_flag_false, &cfg, &src, err, sizeof(err));
    rc |= expect_int("retune with false summary replay open rejected", prc, DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retune with false summary source null", src == NULL);
    dsd_iq_replay_config_clear(&cfg);

    char retuned_count_mismatch[512];
    path_join(retuned_count_mismatch, sizeof(retuned_count_mismatch), dir, "retuned_count_mismatch.iq.json");
    if (write_v2_metadata_with_events(
            retuned_count_mismatch, "events_bad.iq",
            "[{\"kind\":\"RETUNE\",\"byte_offset\":2,\"center_frequency_hz\":851500000,"
            "\"capture_center_frequency_hz\":851884000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"},"
            "{\"kind\":\"RESET\",\"byte_offset\":2,\"center_frequency_hz\":851500000,"
            "\"capture_center_frequency_hz\":851884000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"},"
            "{\"kind\":\"RETUNE\",\"byte_offset\":4,\"center_frequency_hz\":851600000,"
            "\"capture_center_frequency_hz\":851984000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"},"
            "{\"kind\":\"RESET\",\"byte_offset\":4,\"center_frequency_hz\":851600000,"
            "\"capture_center_frequency_hz\":851984000,\"sample_rate_hz\":1536000,\"reason\":\"frequency\"}]",
            8, 1)
        != 0) {
        return 1;
    }
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_open(retuned_count_mismatch, &cfg, &src, err, sizeof(err));
    rc |= expect_int("retuned count mismatch replay open rejected", prc, DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retuned count mismatch source null", src == NULL);
    dsd_iq_replay_config_clear(&cfg);
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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

    /*
     * Relative data paths are resolved from the metadata directory, then reused
     * by the info printer and replay opener. Later cases keep the path valid but
     * vary byte counts and retune flags to exercise open-time rejection paths.
     */
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
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
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

    // Warnings are written to the warning stream while the info output remains usable.
    fflush(warn);
    fseek(warn, 0, SEEK_SET);
    char warn_buf[1024];
    size_t warn_n = fread(warn_buf, 1, sizeof(warn_buf) - 1, warn);
    warn_buf[warn_n] = '\0';
    rc |= expect_true("info mismatch warning present", strstr(warn_buf, "metadata data_bytes") != NULL);
    fclose(out);
    fclose(warn);

    char truncated_meta[512];
    char truncated_data[512];
    path_join(truncated_meta, sizeof(truncated_meta), subdir, "truncated_events.iq.json");
    path_join(truncated_data, sizeof(truncated_data), subdir, "truncated_events.iq");
    uint8_t truncated_bytes[4] = {0, 1, 2, 3};
    write_bytes_file(truncated_data, truncated_bytes, sizeof(truncated_bytes));
    if (write_v2_metadata_with_events(truncated_meta, "truncated_events.iq",
                                      "[{\"kind\":\"MUTE\",\"byte_offset\":6,\"duration_bytes\":2,\"reason\":\"x\"}]",
                                      10, 0)
        != 0) {
        return 1;
    }
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    prc = dsd_iq_replay_read_metadata(truncated_meta, &cfg, err, sizeof(err));
    rc |= expect_int("truncated event metadata read", prc, DSD_IQ_OK);
    out = tmpfile();
    warn = tmpfile();
    if (!out || !warn) {
        if (out) {
            fclose(out);
        }
        if (warn) {
            fclose(warn);
        }
        dsd_iq_replay_config_clear(&cfg);
        return 1;
    }
    rc |= expect_int("truncated event info print", dsd_iq_info_print(&cfg, truncated_meta, 4, out, warn), DSD_IQ_OK);
    fflush(out);
    fseek(out, 0, SEEK_SET);
    out_n = fread(out_buf, 1, sizeof(out_buf) - 1, out);
    out_buf[out_n] = '\0';
    rc |= expect_true("truncated event info not replay compatible", strstr(out_buf, "Replay compatible:   no") != NULL);
    fclose(out);
    fclose(warn);
    dsd_iq_replay_config_clear(&cfg);

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

static int
test_replay_read_partial_eof_and_rewind(void) {
    int rc = 0;
    char dir[256];
    char err[256];
    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    char meta[512];
    char data[512];
    path_join(meta, sizeof(meta), dir, "read.iq.json");
    path_join(data, sizeof(data), dir, "read.iq");
    uint8_t payload[6] = {10, 11, 12, 13, 14, 15};
    if (write_bytes_file(data, payload, sizeof(payload)) != 0) {
        return 1;
    }
    if (write_valid_metadata(meta, "read.iq", "cu8", "none", "post_mute_pre_widen", 1536000, 32, 1, 48000, 0,
                             sizeof(payload))
        != 0) {
        return 1;
    }

    dsd_iq_replay_config cfg;
    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int prc = dsd_iq_replay_open(meta, &cfg, &src, err, sizeof(err));
    rc |= expect_int("replay read open", prc, DSD_IQ_OK);
    rc |= expect_true("replay read source", src != NULL);
    if (prc != DSD_IQ_OK || src == NULL) {
        dsd_iq_replay_config_clear(&cfg);
        return rc;
    }

    uint8_t buf[8];
    size_t got = 99U;
    DSD_MEMSET(buf, 0xA5, sizeof(buf));
    rc |= expect_int("replay read first chunk", dsd_iq_replay_read(src, buf, 4U, &got), DSD_IQ_OK);
    rc |= expect_u64("replay read first size", got, 4U);
    rc |= expect_true("replay read first bytes", memcmp(buf, payload, 4U) == 0);

    DSD_MEMSET(buf, 0xA5, sizeof(buf));
    got = 99U;
    rc |= expect_int("replay read short final chunk", dsd_iq_replay_read(src, buf, 4U, &got), DSD_IQ_OK);
    rc |= expect_u64("replay read final size", got, 2U);
    rc |= expect_true("replay read final bytes", memcmp(buf, payload + 4U, 2U) == 0);
    rc |= expect_int("replay read final tail untouched", buf[2], 0xA5);

    DSD_MEMSET(buf, 0x5A, sizeof(buf));
    got = 99U;
    rc |= expect_int("replay read eof", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
    rc |= expect_u64("replay read eof size", got, 0U);
    rc |= expect_int("replay read eof untouched", buf[0], 0x5A);
    got = 99U;
    rc |= expect_int("replay read repeated eof", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
    rc |= expect_u64("replay read repeated eof size", got, 0U);

    rc |= expect_int("replay rewind", dsd_iq_replay_rewind(src), DSD_IQ_OK);
    DSD_MEMSET(buf, 0, sizeof(buf));
    got = 0U;
    rc |= expect_int("replay read after rewind", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
    rc |= expect_u64("replay read after rewind size", got, sizeof(payload));
    rc |= expect_true("replay read after rewind bytes", memcmp(buf, payload, sizeof(payload)) == 0);

    got = 99U;
    rc |= expect_int("replay read zero max", dsd_iq_replay_read(src, buf, 0U, &got), DSD_IQ_OK);
    rc |= expect_u64("replay read zero max size", got, 0U);
    rc |=
        expect_int("replay read null source", dsd_iq_replay_read(NULL, buf, sizeof(buf), &got), DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("replay rewind null", dsd_iq_replay_rewind(NULL), DSD_IQ_ERR_INVALID_ARG);

    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_metadata_round_trip_capture_open_close();
    rc |= test_metadata_v2_events_round_trip();
    rc |= test_metadata_reuse_after_explicit_clear();
    rc |= test_metadata_output_is_write_only_on_first_success();
    rc |= test_missing_field_reports_clear_error();
    rc |= test_json_unescape_and_control_rejection();
    rc |= test_invalid_json_shapes_and_number_types();
    rc |= test_event_timeline_validation();
    rc |= test_rate_chain_validation();
    rc |= test_relative_data_resolution_info_and_open_validation();
    rc |= test_replay_read_partial_eof_and_rewind();
    return rc ? 1 : 0;
}
