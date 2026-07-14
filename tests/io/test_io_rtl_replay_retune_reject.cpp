// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_support.h"

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static void
fill_capture_cfg(dsd_iq_capture_config* cfg, const char* data_path, const char* metadata_path) {
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    DSD_SNPRINTF(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    DSD_SNPRINTF(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", "post_mute_pre_widen");
    cfg->sample_rate_hz = 1536000U;
    cfg->center_frequency_hz = 851375000ULL;
    cfg->capture_center_frequency_hz = 851759000ULL;
    cfg->ppm = 0;
    cfg->tuner_gain_tenth_db = 270;
    cfg->rtl_dsp_bw_khz = 48;
    cfg->base_decimation = 32U;
    cfg->post_downsample = 1U;
    cfg->demod_rate_hz = 48000U;
    cfg->offset_tuning_enabled = 0;
    cfg->fs4_shift_enabled = 1;
    cfg->combine_rotate_enabled = 1;
    cfg->muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
}

static int
make_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size) {
    if (!out_metadata_path || out_metadata_path_size == 0U) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_retune")) {
        DSD_FPRINTF(stderr, "FAIL: could not create temporary fixture directory\n");
        return 1;
    }

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "fixture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "fixture.iq.json") != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not construct fixture paths\n");
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path);
    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    uint8_t payload[8192];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFFU);
    }
    if (dsd_iq_capture_submit(writer, payload, sizeof(payload)) != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: could not submit fixture payload\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        DSD_FPRINTF(stderr, "FAIL: metadata path overflow\n");
        return 1;
    }
    return 0;
}

static void
prepare_replay_opts(dsd_opts* opts, const char* metadata_path) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = 0;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
}

static int
test_replay_retune_rejected_quickly(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path));
    if (rc != 0) {
        return rc;
    }

    // Keep dsd_opts off the stack for Windows-friendly test stack usage.
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(opts.get(), metadata_path);

    RtlSdrContext* ctx = NULL;
    rc |= expect_int_eq("rtl_stream_create", rtl_stream_create(opts.get(), &ctx), 0);
    if (rc != 0 || !ctx) {
        if (ctx) {
            rtl_stream_destroy(ctx);
        }
        return 1;
    }

    rc |= expect_int_eq("rtl_stream_start", rtl_stream_start(ctx), 0);
    if (rc != 0) {
        rtl_stream_destroy(ctx);
        return 1;
    }

    uint64_t t0 = dsd_time_monotonic_ns();
    int retune_rc = dsd_rtl_stream_test_request_retune(851500000L, 500);
    uint64_t elapsed_ns = dsd_time_monotonic_ns() - t0;
    uint64_t elapsed_ms = elapsed_ns / 1000000ULL;

    rc |= expect_int_eq("retune rejected during replay", retune_rc, -1);
    rc |= expect_true("retune call returned quickly", elapsed_ms < 100ULL);

    t0 = dsd_time_monotonic_ns();
    retune_rc = rtl_stream_tune(ctx, 851625000U);
    elapsed_ns = dsd_time_monotonic_ns() - t0;
    elapsed_ms = elapsed_ns / 1000000ULL;
    rc |= expect_int_eq("rtl_stream_tune reports deferred replay retune", retune_rc, RTL_STREAM_TUNE_DEFERRED);
    rc |= expect_true("rtl_stream_tune deferred quickly", elapsed_ms < 100ULL);

    rc |= expect_int_eq("rtl_stream_stop", rtl_stream_stop(ctx), 0);
    rc |= expect_int_eq("rtl_stream_destroy", rtl_stream_destroy(ctx), 0);
    return rc;
}

int
main(void) {
    return test_replay_retune_rejected_quickly() ? 1 : 0;
}
