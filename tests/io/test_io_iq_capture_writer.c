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
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/timing.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

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

/*
 * Remove a capture's files, and its temp directory once it is empty.
 *
 * Every test here calls mk_temp_dir, so without this each run leaves another
 * mkdtemp directory (and its .iq payload) behind in the build tree. Pass a NULL
 * dir when a test writes several captures into one directory and only the last
 * call should try to remove it.
 */
static void
cleanup_capture(const char* dir, const char* data_path, const char* metadata_path) {
    if (data_path && data_path[0] != '\0') {
        (void)remove(data_path);
    }
    if (metadata_path && metadata_path[0] != '\0') {
        (void)remove(metadata_path);
    }
    if (dir && dir[0] != '\0') {
#if DSD_PLATFORM_WIN_NATIVE
        (void)_rmdir(dir);
#else
        (void)rmdir(dir);
#endif
    }
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

static int g_size_limit_calls;
static uint64_t g_size_limit_max_bytes;

static void
count_size_limit(void* user, uint64_t max_bytes) {
    (void)user;
    g_size_limit_calls++;
    g_size_limit_max_bytes = max_bytes;
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
    cfg->combine_rotate_enabled = 1;
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
        cleanup_capture(dir, data_path, metadata_path);
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

    cleanup_capture(dir, data_path, metadata_path);
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
            cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(NULL, data_path, metadata_path);
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
            cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * A burst of submissions far smaller than one queue block must be packed into
 * blocks rather than consuming a block each. The burst is an exact multiple of
 * the block size, so every block is published by the fill path; the close-time
 * flush of a partial block is covered by test_partial_trailing_block_is_flushed.
 *
 * The assertion is timing-independent: with packing, a burst no larger than the
 * whole pool (block_bytes * block_count) can always be staged even if the writer
 * thread never runs, so zero drops is guaranteed. Before packing, each of the
 * 1024 submissions claimed a whole block from a 4-block pool, which made real
 * captures drop roughly half of a 3 MB/s RTL stream.
 */
static int
test_small_submissions_pack_into_blocks(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    enum { kBlockBytes = 4096, kBlockCount = 4, kChunk = 16 };

    enum { kTotal = kBlockBytes * kBlockCount };

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "packed.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "packed.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = kBlockBytes;
    cfg.queue_block_count = kBlockCount;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open packed", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    static uint8_t expected[kTotal];
    for (size_t i = 0; i < (size_t)kTotal; i++) {
        expected[i] = (uint8_t)(i & 0xFFU);
    }
    for (size_t off = 0; off < (size_t)kTotal; off += (size_t)kChunk) {
        if (dsd_iq_capture_submit(writer, expected + off, (size_t)kChunk) != DSD_IQ_OK) {
            rc |= expect_true("submit packed chunk", 0);
            break;
        }
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        static uint8_t got[kTotal + 64];
        size_t got_n = 0;
        rc |= expect_int("read packed file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
        rc |= expect_u64("packed file bytes", got_n, (uint64_t)kTotal);
        rc |=
            expect_true("packed payload order", got_n == (size_t)kTotal && memcmp(got, expected, (size_t)kTotal) == 0);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("packed metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_u64("packed drops", meta.capture_drops, 0);
        rc |= expect_u64("packed drop blocks", meta.capture_drop_blocks, 0);
        rc |= expect_u64("packed data bytes", meta.data_bytes, (uint64_t)kTotal);
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * Hitting --iq-capture-max-mb is not data loss, so it must not inflate
 * capture_drops. A capped capture that reports drops reads as corrupt, and the
 * warning callback firing for the rest of the run hides real writer overruns.
 */
static int
test_size_limit_is_not_counted_as_drops(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "capped.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "capped.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.max_bytes = 64;
    cfg.queue_block_bytes = 4096;
    cfg.queue_block_count = 4;
    g_size_limit_calls = 0;
    g_size_limit_max_bytes = 0;
    cfg.size_limit_cb = count_size_limit;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open capped", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    {
        /* Far more than the cap, submitted the way a dongle does: many small runs. */
        uint8_t payload[256];
        DSD_MEMSET(payload, 0x5a, sizeof(payload));
        for (int i = 0; i < 64; i++) {
            rc |= expect_int("submit capped", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
        }
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        /* The capped bytes only reach the file through the close-time flush, so
         * check the file itself and not just the counter echoed into the JSON. */
        uint8_t got[256];
        size_t got_n = 0;
        rc |= expect_int("read capped file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
        rc |= expect_u64("capped file bytes", got_n, 64);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("capped metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_u64("capped data bytes", meta.data_bytes, 64);
        rc |= expect_u64("capped drops", meta.capture_drops, 0);
        rc |= expect_u64("capped drop blocks", meta.capture_drop_blocks, 0);
        /* Zero drops is only readable as "healthy" if something else says the
         * capture ended on purpose. */
        rc |= expect_int("capped size_limit_reached", meta.size_limit_reached, 1);
        dsd_iq_replay_config_clear(&meta);
    }

    rc |= expect_int("capped size limit callback fires once", g_size_limit_calls, 1);
    rc |= expect_u64("capped size limit callback max_bytes", g_size_limit_max_bytes, 64);

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * A capture that never reaches its limit must not claim it did, and one with no
 * limit at all must report the field as false rather than omitting it.
 */
static int
test_size_limit_not_reached_reports_false(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "under_cap.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "under_cap.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.max_bytes = 4096;
    cfg.queue_block_bytes = 512;
    cfg.queue_block_count = 4;
    g_size_limit_calls = 0;
    cfg.size_limit_cb = count_size_limit;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open under cap", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    {
        uint8_t payload[64];
        DSD_MEMSET(payload, 0x11, sizeof(payload));
        rc |= expect_int("submit under cap", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("under cap metadata parse",
                         dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)), DSD_IQ_OK);
        rc |= expect_u64("under cap data bytes", meta.data_bytes, 64);
        rc |= expect_int("under cap size_limit_reached", meta.size_limit_reached, 0);
        dsd_iq_replay_config_clear(&meta);
    }

    rc |= expect_int("under cap size limit callback silent", g_size_limit_calls, 0);

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * A partial trailing SAMPLE is different from a size-limit truncation. cf32 and
 * cs16 have no carry byte, so the fragment is discarded outright and the stream
 * loses sample alignment -- that is real data loss and must stay in
 * capture_drops even though a capped capture must not.
 */
static int
test_unaligned_tail_is_counted_as_a_drop(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "unaligned.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "unaligned.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CF32);
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_driver_cf32_pre_ring");
    cfg.queue_block_bytes = 4096;
    cfg.queue_block_count = 2;
    /* No size limit, so the only thing that can shorten the write is alignment. */
    cfg.max_bytes = 0;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open unaligned", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    {
        uint8_t payload[12];
        DSD_MEMSET(payload, 0x3c, sizeof(payload));
        rc |= expect_int("submit unaligned", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("unaligned metadata parse",
                         dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)), DSD_IQ_OK);
        rc |= expect_u64("unaligned data bytes", meta.data_bytes, 8);
        rc |= expect_u64("unaligned drops", meta.capture_drops, 4);
        rc |= expect_u64("unaligned drop blocks", meta.capture_drop_blocks, 1);
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * The shape of every real capture: several full blocks published by the fill
 * path, then a remainder that never fills its block and only reaches the file
 * through the close-time flush. Both halves have to land, in order.
 *
 * Deterministic: kTotal is smaller than the whole pool, so the burst stages
 * even if the writer thread never runs before close.
 */
static int
test_partial_trailing_block_is_flushed(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    enum { kBlockBytes = 64, kBlockCount = 8, kChunk = 10, kTotal = 250 };

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "partial.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "partial.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = kBlockBytes;
    cfg.queue_block_count = kBlockCount;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open partial", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    uint8_t payload[kTotal];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xA0U ^ (i & 0xFFU));
    }
    for (size_t off = 0; off < sizeof(payload); off += (size_t)kChunk) {
        size_t n = sizeof(payload) - off;
        if (n > (size_t)kChunk) {
            n = (size_t)kChunk;
        }
        if (dsd_iq_capture_submit(writer, payload + off, n) != DSD_IQ_OK) {
            rc |= expect_true("submit partial chunk", 0);
            break;
        }
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        uint8_t got[kTotal + 64];
        size_t got_n = 0;
        rc |= expect_int("read partial file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
        /* 250 = 3 full 64-byte blocks published + a 58-byte tail flushed at close. */
        rc |= expect_u64("partial file bytes", got_n, sizeof(payload));
        rc |= expect_true("partial payload", got_n == sizeof(payload) && memcmp(got, payload, sizeof(payload)) == 0);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("partial metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_u64("partial data bytes", meta.data_bytes, (uint64_t)sizeof(payload));
        rc |= expect_u64("partial drops", meta.capture_drops, 0);
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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

    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(NULL, data_path, metadata_path);

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
    cleanup_capture(dir, data_path, metadata_path);

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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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
    cleanup_capture(dir, data_path, metadata_path);
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
        cleanup_capture(dir, data_path, metadata_path);
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

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * Packing makes a partly filled block wait for more data. On a stream that goes
 * quiet -- a muted channel, a reconfigure hold -- that wait is otherwise
 * unbounded, and everything staged is lost if the process is killed. The writer
 * thread has to age the block out on its own: no further submission, no close.
 */
static int
test_idle_partial_block_reaches_the_file_before_close(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    enum { kPayload = 8, kFlushMs = 20, kPollMs = 5, kPollTries = 200 };

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "idle.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "idle.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    /* A block far larger than the payload, so only the age-out can publish it. */
    cfg.queue_block_bytes = 4096;
    cfg.queue_block_count = 4;
    cfg.queue_flush_interval_ms = kFlushMs;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open idle", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    uint8_t payload[kPayload] = {1, 2, 3, 4, 5, 6, 7, 8};
    rc |= expect_int("submit idle", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    {
        /* Poll rather than sleep once: the assertion is "it lands without any
         * further help", not "it lands within exactly one interval". */
        uint64_t size_before_close = 0;
        for (int i = 0; i < kPollTries; i++) {
            dsd_stat_t st;
            if (dsd_stat_path(data_path, &st) == 0 && st.st_size >= 0
                && (uint64_t)st.st_size >= (uint64_t)sizeof(payload)) {
                size_before_close = (uint64_t)st.st_size;
                break;
            }
            dsd_sleep_ms(kPollMs);
        }
        rc |= expect_u64("idle block flushed while still capturing", size_before_close, sizeof(payload));
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        uint8_t got[64];
        size_t got_n = 0;
        rc |= expect_int("read idle file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
        rc |= expect_u64("idle file bytes", got_n, sizeof(payload));
        rc |= expect_true("idle payload", got_n == sizeof(payload) && memcmp(got, payload, sizeof(payload)) == 0);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("idle metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        /* Ageing a block out early must not look like a drop. */
        rc |= expect_u64("idle drops", meta.capture_drops, 0);
        rc |= expect_u64("idle data bytes", meta.data_bytes, sizeof(payload));
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

/*
 * A submission larger than the whole pool overruns it: the producer never blocks
 * (see writer_submit_bytes), so the tail past block_bytes * block_count is
 * dropped rather than waited on. What must still hold is the bookkeeping --
 * every byte written or counted exactly once, and the file a clean prefix of the
 * submission rather than an interleaved mess.
 *
 * Deliberately not asserted: how much survives. That depends on whether the
 * writer thread got scheduled during the copy, and in practice for a 64 KiB
 * memcpy against a 1 KiB pool it does not.
 */
static int
test_submission_larger_than_pool_stays_accounted(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    enum { kBlockBytes = 512, kBlockCount = 2, kPoolBytes = kBlockBytes * kBlockCount, kTotal = 64 * 1024 };

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "oversize.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "oversize.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = kBlockBytes;
    cfg.queue_block_count = kBlockCount;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open oversize", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    static uint8_t payload[kTotal];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)((i * 7U) & 0xFFU);
    }
    rc |= expect_int("submit oversize", dsd_iq_capture_submit(writer, payload, sizeof(payload)), DSD_IQ_OK);

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    {
        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("oversize metadata parse", dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)),
                         DSD_IQ_OK);
        rc |= expect_u64("oversize bytes all accounted", meta.data_bytes + meta.capture_drops, (uint64_t)kTotal);
        rc |= expect_true("oversize keeps at least the whole pool", meta.data_bytes >= (uint64_t)kPoolBytes);

        {
            static uint8_t got[kTotal];
            size_t got_n = 0;
            rc |= expect_int("read oversize file", read_file_all(data_path, got, sizeof(got), &got_n), 0);
            rc |= expect_u64("oversize file matches data_bytes", (uint64_t)got_n, meta.data_bytes);
            rc |= expect_true("oversize file is a prefix of the submission",
                              got_n <= sizeof(payload) && memcmp(got, payload, got_n) == 0);
        }
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}

#if !DSD_PLATFORM_WIN_NATIVE && defined(RLIMIT_FSIZE)
/*
 * A capture whose writes start failing part-way through -- the disk filling up
 * mid-run, reproduced here with RLIMIT_FSIZE.
 *
 * Two things used to go wrong. fwrite() is buffered, so bytes it reported as
 * written were counted into data_bytes whether or not they ever reached the
 * disk, leaving metadata that claimed a larger file than exists. And event
 * byte_offset is stamped from accepted bytes, so an event recorded after the
 * failure sat past data_bytes and made iq_replay reject the whole metadata file
 * -- taking the readable part of the capture down with it.
 */
static int
test_write_failure_is_counted_and_metadata_stays_readable(void) {
    int rc = 0;
    char dir[256];
    char data_path[512];
    char metadata_path[512];
    char err[256];

    enum { kFileLimit = 4096, kChunk = 512, kMaxChunks = 200 };

    struct rlimit saved;
    struct rlimit capped;
    if (getrlimit(RLIMIT_FSIZE, &saved) != 0) {
        return 0; /* cannot set up the fixture; not a product failure */
    }
    if (saved.rlim_max != RLIM_INFINITY && saved.rlim_max < (rlim_t)kFileLimit) {
        return 0;
    }

    if (mk_temp_dir(dir, sizeof(dir)) != 0) {
        return 1;
    }
    path_join(data_path, sizeof(data_path), dir, "nospace.iq");
    path_join(metadata_path, sizeof(metadata_path), dir, "nospace.iq.json");

    dsd_iq_capture_config cfg;
    fill_base_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8);
    cfg.queue_block_bytes = kChunk;
    cfg.queue_block_count = 8;
    cfg.queue_flush_interval_ms = 10;

    dsd_iq_capture_writer* writer = NULL;
    rc |= expect_int("open nospace", dsd_iq_capture_open(&cfg, &writer, err, sizeof(err)), DSD_IQ_OK);
    if (!writer) {
        cleanup_capture(dir, data_path, metadata_path);
        return rc;
    }

    /* Exceeding RLIMIT_FSIZE raises SIGXFSZ as well as failing the write; the
     * metadata JSON stays well under the limit, so it is unaffected. */
    void (*saved_xfsz)(int) = signal(SIGXFSZ, SIG_IGN);
    capped = saved;
    capped.rlim_cur = (rlim_t)kFileLimit;
    if (setrlimit(RLIMIT_FSIZE, &capped) != 0) {
        (void)signal(SIGXFSZ, saved_xfsz);
        dsd_iq_capture_abort(writer);
        cleanup_capture(dir, data_path, metadata_path);
        return 0;
    }

    int submit_refused = 0;
    {
        uint8_t payload[kChunk];
        DSD_MEMSET(payload, 0x2b, sizeof(payload));
        for (int i = 0; i < kMaxChunks; i++) {
            if (dsd_iq_capture_submit(writer, payload, sizeof(payload)) != DSD_IQ_OK) {
                submit_refused = 1;
                break;
            }
            dsd_sleep_ms(1);
        }
    }
    rc |= expect_true("nospace capture stops accepting data", submit_refused);

    {
        /* Stamped from accepted bytes, so this lands past whatever reached the
         * file -- exactly the offset that used to poison the metadata. */
        dsd_iq_event ev;
        DSD_MEMSET(&ev, 0, sizeof(ev));
        ev.kind = DSD_IQ_EVENT_RESET;
        ev.center_frequency_hz = cfg.center_frequency_hz;
        ev.capture_center_frequency_hz = cfg.capture_center_frequency_hz;
        ev.sample_rate_hz = cfg.sample_rate_hz;
        DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "after_write_failure");
        rc |= expect_int("record nospace event", dsd_iq_capture_record_event(writer, &ev), DSD_IQ_OK);
    }

    {
        dsd_iq_capture_final_stats stats;
        DSD_MEMSET(&stats, 0, sizeof(stats));
        dsd_iq_capture_close(writer, &stats);
    }

    (void)setrlimit(RLIMIT_FSIZE, &saved);
    (void)signal(SIGXFSZ, saved_xfsz);

    {
        uint64_t on_disk = 0;
        dsd_stat_t st;
        rc |= expect_true("nospace stat", dsd_stat_path(data_path, &st) == 0);
        if (dsd_stat_path(data_path, &st) == 0 && st.st_size >= 0) {
            on_disk = (uint64_t)st.st_size;
        }

        dsd_iq_replay_config meta;
        DSD_MEMSET(&meta, 0, sizeof(meta));
        rc |= expect_int("nospace metadata stays readable",
                         dsd_iq_replay_read_metadata(metadata_path, &meta, err, sizeof(err)), DSD_IQ_OK);
        rc |= expect_u64("nospace data_bytes matches the file", meta.data_bytes, on_disk);
        rc |= expect_true("nospace reports drops", meta.capture_drops > 0);
        rc |= expect_int("nospace event survives", (int)meta.event_count, 1);
        if (meta.event_count == 1) {
            rc |= expect_true("nospace event offset within data", meta.events[0].byte_offset <= meta.data_bytes);
        }
        dsd_iq_replay_config_clear(&meta);
    }

    cleanup_capture(dir, data_path, metadata_path);
    return rc;
}
#endif /* !DSD_PLATFORM_WIN_NATIVE && RLIMIT_FSIZE */

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
    rc |= test_small_submissions_pack_into_blocks();
    rc |= test_size_limit_is_not_counted_as_drops();
    rc |= test_size_limit_not_reached_reports_false();
    rc |= test_unaligned_tail_is_counted_as_a_drop();
    rc |= test_partial_trailing_block_is_flushed();
    rc |= test_idle_partial_block_reaches_the_file_before_close();
    rc |= test_submission_larger_than_pool_stays_accounted();
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
#if !DSD_PLATFORM_WIN_NATIVE && defined(RLIMIT_FSIZE)
    rc |= test_write_failure_is_counted_and_metadata_stays_readable();
#endif
    rc |= test_public_error_contracts();
    return rc ? 1 : 0;
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
