// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Signal search tap describes the stream that feeds it, and only while that stream runs. A stopped stream leaves
 * no capture and no latest-sample time behind. Otherwise a search started on the next stream before that stream's
 * first block would read the old capture's age (the whole time the receiver was stopped) as a stalled input and fail
 * at once, without waiting the startup deadline for the new stream's samples. An I/Q replay stands in for the
 * receiver: its demod thread feeds the tap as a live one does.
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
#include <string>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_support.h"

static constexpr uint32_t kRateHz = 1536000U;

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

/* The files the capture writer puts in a fixture directory. */
static const char* const kFixtureFiles[] = {"fixture.iq", "fixture.iq.json", nullptr};

/* Every fixture directory made so far, recorded as soon as it exists so that main() removes it whichever way the test
 * that made it returned. */
static std::vector<std::string> g_fixture_dirs;

/* Remove every fixture directory with the files in it. One that is not empty afterwards fails the test. */
static int
remove_fixture_dirs(void) {
    int rc = 0;
    for (const std::string& dir : g_fixture_dirs) {
        if (dsd_test_remove_temp_dir(dir.c_str(), kFixtureFiles) != 0) {
            DSD_FPRINTF(stderr, "FAIL: could not remove fixture directory %s: %s\n", dir.c_str(), std::strerror(errno));
            rc = 1;
        }
    }
    g_fixture_dirs.clear();
    return rc;
}

static int
make_replay_fixture(char* out_metadata_path, size_t out_size) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_signal_tap")) {
        DSD_FPRINTF(stderr, "FAIL: could not create temporary fixture directory\n");
        return 1;
    }
    g_fixture_dirs.emplace_back(temp_dir);
    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "fixture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "fixture.iq.json") != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not construct fixture paths\n");
        return 1;
    }
    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    DSD_SNPRINTF(cfg.data_path, sizeof(cfg.data_path), "%s", data_path);
    DSD_SNPRINTF(cfg.metadata_path, sizeof(cfg.metadata_path), "%s", metadata_path);
    cfg.format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg.capture_stage, sizeof(cfg.capture_stage), "%s", "post_mute_pre_widen");
    cfg.sample_rate_hz = kRateHz;
    cfg.center_frequency_hz = 851375000ULL;
    cfg.capture_center_frequency_hz = 851759000ULL;
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = 48000U;
    cfg.fs4_shift_enabled = 1;
    cfg.combine_rotate_enabled = 1;
    cfg.muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg.source_backend, sizeof(cfg.source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg.source_args, sizeof(cfg.source_args), "%s", "dev=0");
    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }
    /* Mid-scale cu8 is silence: nothing here depends on the samples. */
    const std::vector<uint8_t> payload(65536U, 127U);
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size()) != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: could not submit fixture payload\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    if (DSD_SNPRINTF(out_metadata_path, out_size, "%s", metadata_path) >= (int)out_size) {
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

/* The tap holds no capture: rtl_signal_capture_get() finds none and no samples have a time. */
static int
expect_no_tap_capture(const char* label) {
    dsd_signal_capture capture;
    DSD_MEMSET(&capture, 0, sizeof(capture));
    const int published = rtl_signal_capture_get(&capture);
    const uint64_t last_ms = rtl_signal_capture_last_ms();
    if (published || last_ms != 0U) {
        DSD_FPRINTF(stderr, "FAIL: %s: the tap still holds a capture (rate %u Hz, latest samples at %llu ms)\n", label,
                    (unsigned)capture.rate_hz, (unsigned long long)last_ms);
        return 1;
    }
    return 0;
}

/* Start one stream on the replay, read it until its first block has reached the tap, then stop it: the tap publishes
 * that stream's capture while it runs and nothing of it once it has stopped. */
static int
run_one_stream(const char* metadata_path, const char* label) {
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(opts.get(), metadata_path);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not start the replay\n", label);
        if (ctx) {
            (void)rtl_stream_destroy(ctx);
        }
        return 1;
    }
    int rc = 0;
    dsd_signal_capture capture;
    DSD_MEMSET(&capture, 0, sizeof(capture));
    const uint64_t start_ns = dsd_time_monotonic_ns();
    float block[2048];
    /* The demod thread feeds the tap before it writes a block's output, so a block read has reached the tap. */
    while (!rtl_signal_capture_get(&capture)) {
        if (dsd_time_monotonic_ns() - start_ns > 5000ULL * 1000000ULL) {
            DSD_FPRINTF(stderr, "FAIL: %s: no block reached the tap\n", label);
            rc = 1;
            break;
        }
        int got = 0;
        if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0) {
            dsd_sleep_ms(1);
        }
    }
    if (rc == 0) {
        rc |= expect_true("the running stream's capture rate is published", capture.rate_hz == kRateHz);
        rc |= expect_true("the running stream's latest samples have a time", rtl_signal_capture_last_ms() != 0U);
    }
    rc |= expect_true("rtl_stream_stop", rtl_stream_stop(ctx) == 0);
    rc |= expect_no_tap_capture(label);
    (void)rtl_stream_destroy(ctx);
    return rc;
}

int
main(void) {
    char metadata_path[DSD_TEST_PATH_MAX];
    int rc = make_replay_fixture(metadata_path, sizeof(metadata_path));
    if (rc == 0) {
        rc |= expect_no_tap_capture("before any stream");
        rc |= run_one_stream(metadata_path, "first stream");
        /* A later stream publishes its own capture again once its first block arrives. */
        rc |= run_one_stream(metadata_path, "second stream");
    }
    rc |= remove_fixture_dirs();
    return rc ? 1 : 0;
}
