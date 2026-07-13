// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/platform/file_compat.h>
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
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
mk_temp_dir(char* out_dir, size_t out_dir_size) {
    if (!out_dir || out_dir_size == 0) {
        return -1;
    }
    if (!dsd_test_mkdtemp(out_dir, out_dir_size, "dsdneo_iq_capture_writer")) {
        return -1;
    }
    return 0;
}

static int
path_join(char* out, size_t out_size, const char* a, const char* b) {
    return dsd_test_path_join(out, out_size, a, b);
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
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    DSD_SNPRINTF(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    DSD_SNPRINTF(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = fmt;
    DSD_SNPRINTF(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", "post_mute_pre_widen");
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
    cfg->muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
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
        DSD_MEMSET(&stats, 0, sizeof(stats));
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
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);

        dsd_stat_t st;
        rc |= expect_true("stat cu8", dsd_stat_path(data_path, &st) == 0);
        if (dsd_stat_path(data_path, &st) == 0) {
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
        DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_driver_cf32_pre_ring");
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
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);

        dsd_stat_t st;
        rc |= expect_true("stat cf32", dsd_stat_path(data_path, &st) == 0);
        if (dsd_stat_path(data_path, &st) == 0) {
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
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_stat_t st;
        rc |= expect_true("stat odd", dsd_stat_path(data_path, &st) == 0);
        if (dsd_stat_path(data_path, &st) == 0) {
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
        DSD_MEMSET(payload, 0x7f, sizeof(payload));
        rc |= expect_int("submit overflow", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        stats.input_ring_drops = 123;
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_true("overflow dropped bytes > 0", meta.capture_drops > 0);
        rc |= expect_true("overflow dropped blocks > 0", meta.capture_drop_blocks > 0);
        rc |= expect_u64("final input ring drops", meta.input_ring_drops, 123);
        rc |= expect_int("contains_retunes false", meta.contains_retunes, 0);
        rc |= expect_int("capture_retune_count", (int)meta.capture_retune_count, 0);
        dsd_iq_replay_config_clear(&meta);
    }

    return rc;
}

static int
test_retune_stats_without_events_marks_not_replayable(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "retune_stats.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "retune_stats.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open retune stats", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[4] = {1, 2, 3, 4};
    rc |= expect_int("submit retune stats", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    stats.retune_count = 1;
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("retune stats metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_int("retune stats metadata remains v1", (int)meta.metadata_version, 1);
    rc |= expect_int("retune stats contains_retunes true", meta.contains_retunes, 1);
    rc |= expect_int("retune stats capture_retune_count", (int)meta.capture_retune_count, 1);
    rc |= expect_int("retune stats event_count", (int)meta.event_count, 0);
    dsd_iq_replay_config_clear(&meta);

    dsd_iq_replay_source* replay = NULL;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("retune stats replay open rejected",
                     dsd_iq_replay_open(metadata_path, &meta, &replay, err, sizeof(err)), DSD_IQ_ERR_RETUNE_REJECT);
    rc |= expect_true("retune stats replay source null", replay == NULL);
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_open_resolves_single_capture_path(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }

    path_join(data_path, sizeof(data_path), dir, "data_only.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "data_only.iq.json");
    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, "", DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open data-only path", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (writer) {
        uint8_t payload[2] = {1, 2};
        rc |= expect_int("submit data-only path", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }
    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("data-only metadata parse",
                         dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)), DSD_IQ_OK);
        rc |= expect_true("data-only metadata data_path", strcmp(meta.data_path, data_path) == 0);
        dsd_iq_replay_config_clear(&meta);
    }

    path_join(data_path, sizeof(data_path), dir, "metadata_only.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "metadata_only.iq.json");
    fill_base_capture_cfg(&cfg, "", metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;
    writer = NULL;
    rc |= expect_int("open metadata-only path", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (writer) {
        uint8_t payload[2] = {3, 4};
        rc |=
            expect_int("submit metadata-only path", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }
    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("metadata-only parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_true("metadata-only derived data path", strcmp(meta.data_path, data_path) == 0);
        dsd_iq_replay_config_clear(&meta);
    }

    fill_base_capture_cfg(&cfg, "", "", DSD_IQ_FORMAT_CU8);
    writer = NULL;
    rc |= expect_int("open rejects empty paths", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_true("empty paths leaves writer null", writer == NULL);
    return rc;
}

static int
test_rejects_unaligned_mute_event(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "bad_mute.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "bad_mute.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open bad mute", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[2] = {1, 2};
    rc |= expect_int("submit bad mute", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    rc |= expect_int("reject zero-length mute", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_ARG);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 1;
    rc |= expect_int("reject unaligned mute", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_ALIGNMENT);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("bad mute metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_int("bad mute event_count", (int)meta.event_count, 0);
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_rejects_event_control_bytes_and_unknown_kind(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "bad_event_meta.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "bad_event_meta.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open bad event meta", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[2] = {1, 2};
    rc |= expect_int("submit bad event meta", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 2;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "bad\nreason");
    rc |= expect_int("reject mute control reason", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_META);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = cfg.sample_rate_hz;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "bad\treason");
    rc |= expect_int("reject retune control reason", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_META);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = (dsd_iq_event_kind)99;
    rc |= expect_int("reject unknown event kind", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_ARG);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("bad event meta metadata parse",
                     dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("bad event meta event_count", (int)meta.event_count, 0);
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_rejects_unreplayable_retune_reset_events(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "bad_reconfig_events.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "bad_reconfig_events.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open bad reconfig events", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[2] = {1, 2};
    rc |= expect_int("submit bad reconfig events", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = cfg.sample_rate_hz;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |= expect_int("reject zero retune center", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_ARG);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RESET;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |= expect_int("reject zero reset sample rate", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_INVALID_ARG);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = cfg.sample_rate_hz / 2U;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |=
        expect_int("reject retune sample-rate change", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_ERR_RATE_CHAIN);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("bad reconfig metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_int("bad reconfig event_count", (int)meta.event_count, 0);
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_same_offset_mute_events_merge_duration(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "mute_merge.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "mute_merge.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open mute merge", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[2] = {1, 2};
    rc |= expect_int("submit mute merge", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 2;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "first_mute");
    rc |= expect_int("record first merge mute", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 4;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "second_mute");
    rc |= expect_int("record second merge mute", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("mute merge metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_int("mute merge event_count", (int)meta.event_count, 1);
    if (meta.event_count == 1) {
        rc |= expect_int("mute merge kind", (int)meta.events[0].kind, (int)DSD_IQ_EVENT_MUTE);
        rc |= expect_u64("mute merge offset", meta.events[0].byte_offset, 2);
        rc |= expect_u64("mute merge duration", meta.events[0].duration_bytes, 6);
    }
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_retune_event_orders_before_same_offset_mute(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "event_order.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "event_order.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open event order", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t payload[2] = {1, 2};
    rc |= expect_int("submit event order", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 2;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "retune_reconfigure");
    rc |= expect_int("record same-offset mute", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = 1536000;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    rc |= expect_int("record same-offset retune", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("event order metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_int("event order count", (int)meta.event_count, 2);
    if (meta.event_count == 2) {
        rc |= expect_int("retune ordered first", (int)meta.events[0].kind, (int)DSD_IQ_EVENT_RETUNE);
        rc |= expect_int("mute ordered second", (int)meta.events[1].kind, (int)DSD_IQ_EVENT_MUTE);
        rc |= expect_u64("same offset retained", meta.events[0].byte_offset, meta.events[1].byte_offset);
    }
    dsd_iq_replay_config_clear(&meta);
    return rc;
}

static int
test_event_boundary_drops_pending_cu8_carry(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "carry_event.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "carry_event.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = 4;
    cfg.queue_block_count = 2;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open carry event", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        return rc;
    }

    uint8_t one_byte[1] = {0x80};
    rc |= expect_int("submit carry byte", dsd_iq_capture_submit(writer, one_byte, sizeof(one_byte)), DSD_IQ_OK);

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RESET;
    ev.center_frequency_hz = cfg.center_frequency_hz;
    ev.capture_center_frequency_hz = cfg.capture_center_frequency_hz;
    ev.sample_rate_hz = cfg.sample_rate_hz;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "event_boundary");
    rc |= expect_int("record carry boundary reset", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    dsd_stat_t st;
    rc |= expect_true("carry event data stat", dsd_stat_path(data_path, &st) == 0);
    if (dsd_stat_path(data_path, &st) == 0) {
        rc |= expect_u64("carry event writes no partial pair", (uint64_t)st.st_size, 0);
    }

    dsd_iq_replay_config meta;
    DSD_MEMSET(&meta, 0, sizeof(meta));
    rc |= expect_int("carry event metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_u64("carry event dropped byte", meta.capture_drops, 1);
    rc |= expect_u64("carry event drop block", meta.capture_drop_blocks, 1);
    rc |= expect_int("carry event event_count", (int)meta.event_count, 1);
    if (meta.event_count == 1) {
        rc |= expect_int("carry event kind", (int)meta.events[0].kind, (int)DSD_IQ_EVENT_RESET);
        rc |= expect_u64("carry event offset", meta.events[0].byte_offset, 0);
    }
    dsd_iq_replay_config_clear(&meta);
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
        dsd_stat_t st;
        rc |= expect_true("metadata removed", dsd_stat_path(metadata_path, &st) != 0);
    }

    return rc;
}

static int
test_public_error_contracts(void) {
    int rc = 0;
    char data_path[32];
    char metadata_path[32];
    char err[256];
    dsd_iq_capture_writer* writer = NULL;

    DSD_MEMSET(err, 0, sizeof(err));
    rc |= expect_int("derive rejects null path",
                     dsd_iq_capture_derive_paths(NULL, data_path, sizeof(data_path), metadata_path,
                                                 sizeof(metadata_path), err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_true("derive null path sets error", err[0] != '\0');

    rc |= expect_int("derive json suffix",
                     dsd_iq_capture_derive_paths("capture.iq.json", data_path, sizeof(data_path), metadata_path,
                                                 sizeof(metadata_path), err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_true("derive json data path", strcmp(data_path, "capture.iq") == 0);
    rc |= expect_true("derive json metadata path", strcmp(metadata_path, "capture.iq.json") == 0);

    rc |= expect_int("derive rejects empty json data",
                     dsd_iq_capture_derive_paths(".json", data_path, sizeof(data_path), metadata_path,
                                                 sizeof(metadata_path), err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int(
        "derive rejects tiny data buffer",
        dsd_iq_capture_derive_paths("capture.iq", data_path, 4, metadata_path, sizeof(metadata_path), err, sizeof(err)),
        DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int(
        "derive rejects tiny metadata buffer",
        dsd_iq_capture_derive_paths("capture.iq", data_path, sizeof(data_path), metadata_path, 4, err, sizeof(err)),
        DSD_IQ_ERR_INVALID_ARG);

    rc |= expect_int("open rejects null cfg", dsd_iq_capture_open(NULL, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_ARG);

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, "data.iq", "data.iq.json", DSD_IQ_FORMAT_CU8);
    rc |=
        expect_int("open rejects null out", dsd_iq_capture_open(&cfg, NULL, err, sizeof(err)), DSD_IQ_ERR_INVALID_ARG);

    dsd_iq_capture_config bad;
    bad = cfg;
    bad.sample_rate_hz = 0;
    rc |= expect_int("open rejects zero sample rate", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_true("zero sample rate leaves writer null", writer == NULL);

    bad = cfg;
    bad.base_decimation = 3;
    rc |= expect_int("open rejects non-power-two base decimation", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_RATE_CHAIN);

    bad = cfg;
    bad.post_downsample = 0;
    rc |= expect_int("open rejects zero post downsample", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_RATE_CHAIN);

    bad = cfg;
    bad.demod_rate_hz = 0;
    rc |= expect_int("open rejects zero demod rate", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_RATE_CHAIN);

    bad = cfg;
    bad.demod_rate_hz = 24000;
    rc |= expect_int("open rejects inconsistent demod rate", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_RATE_CHAIN);

    bad = cfg;
    bad.format = DSD_IQ_FORMAT_UNKNOWN;
    rc |= expect_int("open rejects unsupported format", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_UNSUPPORTED_FMT);

    bad = cfg;
    bad.capture_stage[0] = '\0';
    rc |= expect_int("open rejects empty capture stage", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_META);

    bad = cfg;
    bad.source_backend[0] = '\0';
    rc |= expect_int("open rejects empty source backend", dsd_iq_capture_open(&bad, &writer, err, sizeof(err)),
                     DSD_IQ_ERR_INVALID_META);

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_submit_small_blocks_and_contents();
    rc |= test_max_bytes_alignment_cu8_and_cf32();
    rc |= test_odd_cu8_preserves_iq_alignment();
    rc |= test_queue_overflow_updates_drop_counters();
    rc |= test_retune_stats_without_events_marks_not_replayable();
    rc |= test_open_resolves_single_capture_path();
    rc |= test_rejects_unaligned_mute_event();
    rc |= test_rejects_event_control_bytes_and_unknown_kind();
    rc |= test_rejects_unreplayable_retune_reset_events();
    rc |= test_same_offset_mute_events_merge_duration();
    rc |= test_retune_event_orders_before_same_offset_mute();
    rc |= test_event_boundary_drops_pending_cu8_carry();
    rc |= test_abort_removes_metadata();
    rc |= test_public_error_contracts();
    return rc ? 1 : 0;
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
