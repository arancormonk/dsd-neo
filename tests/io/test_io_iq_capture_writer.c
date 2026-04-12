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
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
mk_temp_dir(char* out_dir, size_t out_dir_size) {
    if (!out_dir || out_dir_size < 32) {
        return -1;
    }
    snprintf(out_dir, out_dir_size, "/tmp/dsdneo_iq_capture_writer_XXXXXX");
    if (!dsd_mkdtemp(out_dir)) {
        return -1;
    }
    return 0;
}

static int
path_join(char* out, size_t out_size, const char* a, const char* b) {
    if (!out || out_size == 0 || !a || !b) {
        return -1;
    }
    if (snprintf(out, out_size, "%s/%s", a, b) >= (int)out_size) {
        return -1;
    }
    return 0;
}

static int
read_file_all(const char* path, uint8_t* out, size_t out_cap, size_t* out_n) {
    if (!path || !out || !out_n) {
        return -1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    size_t n = fread(out, 1, out_cap, fp);
    fclose(fp);
    *out_n = n;
    return 0;
}

static void
fill_base_capture_cfg(dsd_iq_capture_config* cfg, const char* data_path, const char* metadata_path,
                      dsd_iq_sample_format fmt) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    snprintf(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = fmt;
    snprintf(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", "post_mute_pre_widen");
    cfg->sample_rate_hz = 1536000;
    cfg->center_frequency_hz = 851375000ULL;
    cfg->capture_center_frequency_hz = 851759000ULL;
    cfg->ppm = 0;
    cfg->tuner_gain_tenth_db = 270;
    cfg->rtl_dsp_bw_khz = 48;
    cfg->base_decimation = 32;
    cfg->post_downsample = 1;
    cfg->demod_rate_hz = 48000;
    cfg->offset_tuning_enabled = 0;
    cfg->fs4_shift_enabled = 1;
    cfg->combine_rotate_enabled = 1;
    cfg->muted_bytes_excluded = 1;
    snprintf(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    snprintf(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
}

static int
test_submit_small_blocks_and_contents(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "small.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "small.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 8;
    cfg.queue_block_count = 4;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    {
        uint8_t b1[4] = {1, 2, 3, 4};
        uint8_t b2[4] = {5, 6, 7, 8};
        rc |= expect_int("submit b1", dsd_iq_capture_submit(writer, b1, sizeof(b1)), DSD_IQ_OK);
        rc |= expect_int("submit b2", dsd_iq_capture_submit(writer, b2, sizeof(b2)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        memset(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        uint8_t got[32];
        size_t got_n = 0;
        uint8_t want[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        rc |= expect_int("read file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
        rc |= expect_u64("small file bytes", got_n, 8);
        rc |= expect_true("small file payload", got_n == sizeof(want) && memcmp(got, want, sizeof(want)) == 0);
    }

    return rc;
}

static int
test_max_bytes_alignment_cu8_and_cf32(void) {
    int rc = 0;
    char dir[256];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    {
        char data_path[512];
        char metadata_path[512];
        path_join(data_path, sizeof(data_path), dir, "max_cu8.iq");
        path_join(metadata_path, sizeof(metadata_path), dir, "max_cu8.iq.json");

        dsd_iq_capture_config cfg;
        fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
        cfg.max_bytes = 5; /* should align down to 4 */
        cfg.queue_block_bytes = 4;
        cfg.queue_block_count = 2;

        dsd_iq_capture_writer* writer = NULL;
        rc |= expect_int("open cu8", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
        if (!writer) {
            return rc;
        }

        uint8_t payload[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        rc |= expect_int("submit cu8", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

        dsd_iq_capture_final_stats stats;
        memset(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);

        struct stat st;
        rc |= expect_true("stat cu8", stat(data_path, &st) == 0);
        if (stat(data_path, &st) == 0) {
            rc |= expect_u64("cu8 max aligned bytes", (uint64_t)st.st_size, 4);
        }
    }

    {
        char data_path[512];
        char metadata_path[512];
        path_join(data_path, sizeof(data_path), dir, "max_cf32.iq");
        path_join(metadata_path, sizeof(metadata_path), dir, "max_cf32.iq.json");

        dsd_iq_capture_config cfg;
        fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CF32);
        snprintf(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_driver_cf32_pre_ring");
        cfg.max_bytes = 11; /* should align down to 8 */
        cfg.queue_block_bytes = 8;
        cfg.queue_block_count = 2;

        dsd_iq_capture_writer* writer = NULL;
        rc |= expect_int("open cf32", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
        if (!writer) {
            return rc;
        }

        uint8_t payload[16] = {0};
        rc |= expect_int("submit cf32", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

        dsd_iq_capture_final_stats stats;
        memset(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);

        struct stat st;
        rc |= expect_true("stat cf32", stat(data_path, &st) == 0);
        if (stat(data_path, &st) == 0) {
            rc |= expect_u64("cf32 max aligned bytes", (uint64_t)st.st_size, 8);
        }
    }

    return rc;
}

static int
test_odd_cu8_preserves_iq_alignment(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "odd.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "odd.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 2;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open odd", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    {
        uint8_t a[1] = {10};
        uint8_t b[3] = {11, 12, 13};
        rc |= expect_int("submit odd a", dsd_iq_capture_submit(writer, a, sizeof(a)), DSD_IQ_OK);
        rc |= expect_int("submit odd b", dsd_iq_capture_submit(writer, b, sizeof(b)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        memset(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        struct stat st;
        rc |= expect_true("stat odd", stat(data_path, &st) == 0);
        if (stat(data_path, &st) == 0) {
            rc |= expect_true("odd file aligned to iq pair", (((uint64_t)st.st_size) % 2ULL) == 0ULL);
        }
    }

    return rc;
}

static int
test_queue_overflow_updates_drop_counters(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "overflow.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "overflow.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 2;
    cfg.queue_block_count = 1;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open overflow", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    {
        uint8_t payload[4096];
        memset(payload, 0x7f, sizeof(payload));
        rc |= expect_int("submit overflow", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        memset(&stats, 0, sizeof(stats));
        stats.input_ring_drops = 123;
        stats.retune_count = 4;
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_iq_replay_config meta;
        memset(&meta, 0, sizeof(meta));
        rc |= expect_int("metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_true("overflow dropped bytes > 0", meta.capture_drops > 0);
        rc |= expect_true("overflow dropped blocks > 0", meta.capture_drop_blocks > 0);
        rc |= expect_u64("final input ring drops", meta.input_ring_drops, 123);
        rc |= expect_int("contains_retunes true", meta.contains_retunes, 1);
        rc |= expect_int("capture_retune_count", (int)meta.capture_retune_count, 4);
    }

    return rc;
}

static int
test_abort_removes_metadata(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "abort.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "abort.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open abort", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    {
        uint8_t payload[8] = {0};
        rc |= expect_int("submit abort", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
    }

    dsd_iq_capture_abort(writer);

    {
        struct stat st;
        rc |= expect_true("metadata removed", stat(metadata_path, &st) != 0);
    }

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_submit_small_blocks_and_contents();
    rc |= test_max_bytes_alignment_cu8_and_cf32();
    rc |= test_odd_cu8_preserves_iq_alignment();
    rc |= test_queue_overflow_updates_drop_counters();
    rc |= test_abort_removes_metadata();
    return rc ? 1 : 0;
}
