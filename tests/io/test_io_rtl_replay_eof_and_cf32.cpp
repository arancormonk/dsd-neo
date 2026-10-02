// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "dsd-neo/core/input_level.h"
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

/* A replay hook's context, in static storage. The stream keeps the pointer a test installs a hook with
 * (rtl_stream_test_set_replay_stage_hook(), rtl_stream_test_set_replay_block_hook()) in a global until the hook is
 * cleared, so the context must not live in the test's stack frame. Each call ends the T the last caller had and builds
 * a new one in its place, so a test starts from a fresh context whatever ran before it; a test uses one T at a time. */
template <typename T>
static T&
fresh_hook_context(void) {
    static T ctx;
    ctx.~T();
    ::new (static_cast<void*>(&ctx)) T();
    return ctx;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_u64_ge(const char* label, uint64_t got, uint64_t want_min) {
    if (got < want_min) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%llu want>=%llu\n", label, (unsigned long long)got,
                    (unsigned long long)want_min);
        return 1;
    }
    return 0;
}

enum : uint8_t {
    kReplayResetReasonFrequency = 0,
    kReplayResetReasonPpmCorrection = 2,
};

namespace {
/* A fixture directory and the two files a fixture writes in it. */
struct FixtureDir {
    std::string dir;
    std::string data_file;
    std::string metadata_file;
};
} // namespace

/* Every fixture directory made so far, recorded as soon as it exists so that main() removes it whichever way the test
 * that made it returned. */
static std::vector<FixtureDir> g_fixture_dirs;

static void
track_fixture_dir(const char* dir, const char* data_file, const char* metadata_file) {
    g_fixture_dirs.push_back(FixtureDir{dir, data_file, metadata_file});
}

/* Remove every fixture directory with the files in it. One that is not empty afterwards fails the test. */
static int
remove_fixture_dirs(void) {
    int rc = 0;
    for (const FixtureDir& fixture : g_fixture_dirs) {
        const char* const files[] = {fixture.data_file.c_str(), fixture.metadata_file.c_str(), nullptr};
        if (dsd_test_remove_temp_dir(fixture.dir.c_str(), files) != 0) {
            DSD_FPRINTF(stderr, "FAIL: could not remove fixture directory %s: %s\n", fixture.dir.c_str(),
                        std::strerror(errno));
            rc = 1;
        }
    }
    g_fixture_dirs.clear();
    return rc;
}

static int
write_bytes_file(const char* path, const uint8_t* bytes, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    if (len > 0 && std::fwrite(bytes, 1, len, fp) != len) {
        std::fclose(fp);
        return -1;
    }
    std::fclose(fp);
    return 0;
}

static int
write_text_file(const char* path, const char* text) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t n = std::strlen(text);
    if (n > 0 && std::fwrite(text, 1, n, fp) != n) {
        std::fclose(fp);
        return -1;
    }
    std::fclose(fp);
    return 0;
}

static int
rewrite_as_historical_two_pass_capture(const char* metadata_path) {
    FILE* fp = dsd_fopen_private(metadata_path, "rb");
    if (!fp) {
        return -1;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return -1;
    }
    long size = std::ftell(fp);
    if (size < 0 || std::fseek(fp, 0, SEEK_SET) != 0) {
        std::fclose(fp);
        return -1;
    }
    std::string metadata((size_t)size, '\0');
    if (size > 0 && std::fread(&metadata[0], 1, (size_t)size, fp) != (size_t)size) {
        std::fclose(fp);
        return -1;
    }
    std::fclose(fp);

    const std::string combined = "\"combine_rotate_enabled\": true";
    size_t pos = metadata.find(combined);
    if (pos == std::string::npos) {
        return -1;
    }
    metadata.replace(pos, combined.size(), "\"combine_rotate_enabled\": false");
    return write_bytes_file(metadata_path, reinterpret_cast<const uint8_t*>(metadata.data()), metadata.size());
}

static void
fill_capture_cfg(dsd_iq_capture_config* cfg, const char* data_path, const char* metadata_path,
                 dsd_iq_sample_format format, const char* capture_stage, int fs4_shift_enabled) {
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    DSD_SNPRINTF(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    DSD_SNPRINTF(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = format;
    DSD_SNPRINTF(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", capture_stage);
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
    cfg->fs4_shift_enabled = fs4_shift_enabled ? 1 : 0;
    cfg->combine_rotate_enabled = 1;
    cfg->muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
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

static void
prepare_replay_opts_with_loop(dsd_opts* opts, const char* metadata_path, int loop) {
    prepare_replay_opts(opts, metadata_path);
    opts->iq_replay_loop = loop ? 1 : 0;
}

static void
prepare_replay_opts_with_loop_and_rate(dsd_opts* opts, const char* metadata_path, int loop, int realtime) {
    prepare_replay_opts_with_loop(opts, metadata_path, loop);
    opts->iq_replay_rate_mode = realtime ? DSD_IQ_REPLAY_RATE_REALTIME : DSD_IQ_REPLAY_RATE_FAST;
}

/* make_replay_fixture() with the tuner gain the sidecar records (0: none, as an auto-gain capture can leave it). */
static int
make_replay_fixture_with_gain(char* out_metadata_path, size_t out_metadata_path_size, dsd_iq_sample_format format,
                              const char* capture_stage, int fs4_shift_enabled, size_t payload_bytes,
                              int tuner_gain_tenth_db) {
    if (!out_metadata_path || out_metadata_path_size == 0U || !capture_stage) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_eof")) {
        DSD_FPRINTF(stderr, "FAIL: could not create temporary fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "fixture.iq", "fixture.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "fixture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "fixture.iq.json") != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not construct fixture paths\n");
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, format, capture_stage, fs4_shift_enabled);
    cfg.tuner_gain_tenth_db = tuner_gain_tenth_db;

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    int submit_rc = DSD_IQ_OK;
    if (format == DSD_IQ_FORMAT_CU8) {
        if ((payload_bytes & 1U) != 0U) {
            payload_bytes += 1U;
        }
        std::vector<uint8_t> payload(payload_bytes);
        for (size_t i = 0; i < payload.size(); i++) {
            payload[i] = static_cast<uint8_t>(i & 0xFFU);
        }
        submit_rc = dsd_iq_capture_submit(writer, payload.data(), payload.size());
    } else if (format == DSD_IQ_FORMAT_CF32) {
        if ((payload_bytes & 7U) != 0U) {
            payload_bytes &= ~(size_t)7U;
        }
        if (payload_bytes < 8U) {
            payload_bytes = 8U;
        }
        size_t float_count = payload_bytes / sizeof(float);
        if ((float_count & 1U) != 0U) {
            float_count--;
        }
        std::vector<float> payload(float_count);
        size_t complex_count = float_count / 2U;
        for (size_t i = 0; i < complex_count; i++) {
            float t = static_cast<float>(i) * 0.03125f;
            payload[i * 2U + 0U] = std::cos(t);
            payload[i * 2U + 1U] = std::sin(t);
        }
        submit_rc = dsd_iq_capture_submit(writer, payload.data(), payload.size() * sizeof(float));
    } else {
        submit_rc = DSD_IQ_ERR_INVALID_ARG;
    }

    if (submit_rc != DSD_IQ_OK) {
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

static int
make_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, dsd_iq_sample_format format,
                    const char* capture_stage, int fs4_shift_enabled, size_t payload_bytes) {
    return make_replay_fixture_with_gain(out_metadata_path, out_metadata_path_size, format, capture_stage,
                                         fs4_shift_enabled, payload_bytes, 270);
}

static int
make_midrange_cu8_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, size_t payload_bytes) {
    if (!out_metadata_path || out_metadata_path_size == 0U) {
        return 1;
    }
    if ((payload_bytes & 1U) != 0U) {
        payload_bytes++;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_level")) {
        DSD_FPRINTF(stderr, "FAIL: could not create input-level fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "level.iq", "level.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "level.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "level.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open input-level IQ capture writer: %s\n",
                    err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    std::vector<uint8_t> payload(payload_bytes);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>(96U + ((i * 17U) % 65U));
    }
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size()) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

static int
make_historical_cu8_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, uint8_t sample,
                                   size_t payload_bytes) {
    if (!out_metadata_path || out_metadata_path_size == 0U) {
        return 1;
    }
    if ((payload_bytes & 1U) != 0U) {
        payload_bytes++;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_level_cu8")) {
        DSD_FPRINTF(stderr, "FAIL: could not create CU8 input-level fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "level_cu8.iq", "level_cu8.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "level_cu8.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "level_cu8.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open CU8 input-level IQ capture writer: %s\n",
                    err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    std::vector<uint8_t> payload(payload_bytes, sample);
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size()) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (rewrite_as_historical_two_pass_capture(metadata_path) != 0) {
        return 1;
    }

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

static int
make_constant_cf32_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, float i_sample,
                                  float q_sample, size_t complex_count) {
    if (!out_metadata_path || out_metadata_path_size == 0U || complex_count == 0U) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_level_cf32")) {
        DSD_FPRINTF(stderr, "FAIL: could not create CF32 input-level fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "level_cf32.iq", "level_cf32.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "level_cf32.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "level_cf32.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 1);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open CF32 input-level IQ capture writer: %s\n",
                    err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    std::vector<float> payload(complex_count * 2U);
    for (size_t i = 0; i < complex_count; i++) {
        payload[i * 2U + 0U] = i_sample;
        payload[i * 2U + 1U] = q_sample;
    }
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size() * sizeof(float)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

static int
make_eventful_replay_fixture_with_reset_reason(char* out_metadata_path, size_t out_metadata_path_size,
                                               size_t payload_bytes, const char* reset_reason) {
    if (!out_metadata_path || out_metadata_path_size == 0U) {
        return 1;
    }
    if (payload_bytes < 8192U) {
        payload_bytes = 8192U;
    }
    if ((payload_bytes & 1U) != 0U) {
        payload_bytes++;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_events")) {
        DSD_FPRINTF(stderr, "FAIL: could not create event fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "events.iq", "events.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "events.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "events.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open event IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    std::vector<uint8_t> payload(payload_bytes);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>((i * 3U) & 0xFFU);
    }

    const size_t first = 4096U;
    if (dsd_iq_capture_submit(writer, payload.data(), first) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RETUNE;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = 1536000U;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "frequency");
    if (dsd_iq_capture_record_event(writer, &ev) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = 4096U;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "retune_mute");
    if (dsd_iq_capture_record_event(writer, &ev) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_RESET;
    ev.center_frequency_hz = 851500000ULL;
    ev.capture_center_frequency_hz = 851884000ULL;
    ev.sample_rate_hz = 1536000U;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", reset_reason ? reset_reason : "frequency");
    if (dsd_iq_capture_record_event(writer, &ev) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    if (dsd_iq_capture_submit(writer, payload.data() + first, payload.size() - first) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

static int
make_eventful_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, size_t payload_bytes) {
    return make_eventful_replay_fixture_with_reset_reason(out_metadata_path, out_metadata_path_size, payload_bytes,
                                                          "frequency");
}

static int
make_terminal_mute_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, uint64_t mute_bytes) {
    if (!out_metadata_path || out_metadata_path_size == 0U) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_terminal_mute")) {
        DSD_FPRINTF(stderr, "FAIL: could not create terminal mute fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "terminal.iq", "terminal.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "terminal.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "terminal.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open terminal mute writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }

    /* Each pass of the loop must make output for its reader to see (stop_loop_reader()): every rewind starts the
       channel FIR over, which holds back its first 67 samples at 48 kHz (135 taps), so a pass needs more than 67 x 32
       complex samples. 8192 bytes make 128 at the demod rate, 61 of them output. */
    uint8_t payload[8192];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = static_cast<uint8_t>(i & 0xFFU);
    }
    if (dsd_iq_capture_submit(writer, payload, sizeof(payload)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_event ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.kind = DSD_IQ_EVENT_MUTE;
    ev.duration_bytes = mute_bytes;
    DSD_SNPRINTF(ev.reason, sizeof(ev.reason), "%s", "terminal_mute");
    if (dsd_iq_capture_record_event(writer, &ev) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

static int
start_replay_stream(const char* metadata_path, std::unique_ptr<dsd_opts>* out_opts, RtlSdrContext** out_ctx) {
    if (!metadata_path || !out_opts || !out_ctx) {
        return 1;
    }
    out_opts->reset(new dsd_opts());
    prepare_replay_opts(out_opts->get(), metadata_path);

    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(out_opts->get(), &ctx) != 0 || !ctx) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_create failed\n");
        return 1;
    }
    if (rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_start failed\n");
        rtl_stream_destroy(ctx);
        return 1;
    }
    *out_ctx = ctx;
    return 0;
}

static int
start_replay_stream_with_loop_and_rate(const char* metadata_path, int loop, int realtime,
                                       std::unique_ptr<dsd_opts>* out_opts, RtlSdrContext** out_ctx) {
    if (!metadata_path || !out_opts || !out_ctx) {
        return 1;
    }
    out_opts->reset(new dsd_opts());
    prepare_replay_opts_with_loop_and_rate(out_opts->get(), metadata_path, loop, realtime);

    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(out_opts->get(), &ctx) != 0 || !ctx) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_create failed\n");
        return 1;
    }
    if (rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: rtl_stream_start failed\n");
        rtl_stream_destroy(ctx);
        return 1;
    }
    *out_ctx = ctx;
    return 0;
}

static int
start_replay_stream_with_loop(const char* metadata_path, int loop, std::unique_ptr<dsd_opts>* out_opts,
                              RtlSdrContext** out_ctx) {
    return start_replay_stream_with_loop_and_rate(metadata_path, loop, 0, out_opts, out_ctx);
}

static void
stop_and_destroy_stream(RtlSdrContext* ctx) {
    if (!ctx) {
        return;
    }
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
}

static int
drain_stream_to_eof(RtlSdrContext* ctx, uint64_t timeout_ms, uint64_t* out_total_samples) {
    if (!ctx || !out_total_samples) {
        return 1;
    }
    *out_total_samples = 0;

    float audio[2048];
    uint64_t start_ns = dsd_time_monotonic_ns();
    uint64_t timeout_ns = timeout_ms * 1000000ULL;
    for (;;) {
        int got = 0;
        int rc = rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);
        if (rc == 0) {
            if (got > 0) {
                *out_total_samples += (uint64_t)got;
            }
        } else {
            return 0;
        }
        if (dsd_time_monotonic_ns() - start_ns > timeout_ns) {
            DSD_FPRINTF(stderr, "FAIL: timed out draining replay stream\n");
            return 1;
        }
    }
}

namespace {
/* A decoder reading a looping replay on its own thread until told to stop (start_loop_reader()). The decoder paces an
 * I/Q replay, so a replay nobody reads never gets past its first block. */
struct LoopReader {
    RtlSdrContext* ctx = nullptr;
    dsd_thread_t thread{};
    std::atomic<int> stop{0};
    std::atomic<int> done{0};
    std::atomic<uint64_t> samples{0U};
    int started = 0;
    int rescued = 0;
};
} // namespace

static DSD_THREAD_RETURN_TYPE
loop_reader_fn(void* arg) {
    LoopReader* reader = static_cast<LoopReader*>(arg);
    float audio[1024];
    while (!reader->stop.load(std::memory_order_acquire)) {
        int got = 0;
        if (rtl_stream_read(reader->ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
            break;
        }
        if (got > 0) {
            reader->samples.fetch_add((uint64_t)got, std::memory_order_relaxed);
        }
    }
    reader->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

static int
start_loop_reader(LoopReader* reader, RtlSdrContext* ctx) {
    reader->ctx = ctx;
    if (dsd_thread_create(&reader->thread, loop_reader_fn, reader) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not start the decoder thread\n");
        return 1;
    }
    reader->started = 1;
    return 0;
}

/* Stop the reader before the stream stops: a looping replay's next output ends its read. One still reading after 2 s
 * is ended with the global exit flag, which the caller clears once the stream is stopped (reader->rescued). */
static void
stop_loop_reader(LoopReader* reader) {
    if (!reader->started) {
        return;
    }
    reader->stop.store(1, std::memory_order_release);
    for (unsigned int waited = 0U; waited < 2000U && !reader->done.load(std::memory_order_acquire); waited++) {
        dsd_sleep_ms(1U);
    }
    if (!reader->done.load(std::memory_order_acquire)) {
        DSD_FPRINTF(stderr, "FAIL: the looping replay's reader did not stop within 2 s\n");
        reader->rescued = 1;
        dsd_exitflag_store(1);
    }
    (void)dsd_thread_join(reader->thread);
    reader->started = 0;
}

namespace {
/* The input level as the demod found it when it took its first block (level_before_first_block_stage()). */
struct FirstBlockLevel {
    std::atomic<int> checked{0};
    std::atomic<int> published{0};
    LoopReader reader; /* the decoder, which the demod waits for before it takes a block */
};
} // namespace

static void
level_before_first_block_stage(int stage, size_t count, void* ctx) {
    (void)count;
    FirstBlockLevel* first = static_cast<FirstBlockLevel*>(ctx);
    if (stage != RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE || first->checked.exchange(1, std::memory_order_acq_rel)) {
        return;
    }
    dsd_input_level_snapshot level;
    DSD_MEMSET(&level, 0, sizeof(level));
    if (rtl_stream_get_input_level(&level) == 0 && level.sample_count > 0U) {
        first->published.store(1, std::memory_order_release);
    }
}

/* Start a looping replay for an input-level test, with a decoder reading it. The demod publishes a chunk's input level
 * when it starts the chunk's block, so none is published before it takes its first block: the hook looks then. */
static int
start_level_replay(const char* metadata_path, FirstBlockLevel* first, std::unique_ptr<dsd_opts>* out_opts,
                   RtlSdrContext** out_ctx) {
    dsd_input_level_snapshot stale;
    (void)rtl_stream_get_input_level(&stale); /* with no stream running, this drops a level an earlier replay left */
    rtl_stream_test_set_replay_stage_hook(level_before_first_block_stage, first);
    if (start_replay_stream_with_loop(metadata_path, 1, out_opts, out_ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        return 1;
    }
    if (start_loop_reader(&first->reader, *out_ctx) != 0) {
        stop_and_destroy_stream(*out_ctx);
        *out_ctx = NULL;
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        return 1;
    }
    return 0;
}

/* Stop a replay start_level_replay() started, and check the demod found no level published before its first block. */
static int
finish_level_replay(const char* label, RtlSdrContext* ctx, FirstBlockLevel* first) {
    stop_loop_reader(&first->reader);
    stop_and_destroy_stream(ctx);
    if (first->reader.rescued) {
        dsd_exitflag_store(0);
    }
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    int rc = first->reader.rescued;
    if (!first->checked.load(std::memory_order_acquire)) {
        DSD_FPRINTF(stderr, "FAIL: %s: the demod took no block\n", label);
        rc = 1;
    } else if (first->published.load(std::memory_order_acquire)) {
        DSD_FPRINTF(stderr, "FAIL: %s: an input level was published before the demod took its first block\n", label);
        rc = 1;
    }
    return rc;
}

static int
test_cu8_replay_publishes_raw_input_level(void) {
    int rc = 0;

    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_midrange_cu8_replay_fixture(metadata_path, sizeof(metadata_path), 131072U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    FirstBlockLevel& first = fresh_hook_context<FirstBlockLevel>();
    rc |= start_level_replay(metadata_path, &first, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    dsd_input_level_snapshot level;
    DSD_MEMSET(&level, 0, sizeof(level));
    int saw_level = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    while (dsd_time_monotonic_ns() - start_ns < 3000ULL * 1000000ULL) {
        rc |= expect_int_eq("replay input level snapshot", rtl_stream_get_input_level(&level), 0);
        if (rc != 0) {
            break;
        }
        if (level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8 && level.sample_count > 0U) {
            saw_level = 1;
            break;
        }
        dsd_sleep_ms(1U);
    }

    rc |= expect_true("cu8 replay raw input level published", saw_level);
    if (saw_level) {
        rc |= expect_int_eq("cu8 replay input level source", (int)level.source, (int)DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
        rc |= expect_true("cu8 replay input level has samples", level.sample_count > 0U);
        rc |= expect_true("cu8 replay input level is unclipped", level.clip_pct == 0.0);
    }

    rc |= finish_level_replay("cu8 replay input level", ctx, &first);
    return rc;
}

static int
test_cu8_replay_legacy_fs4_level_uses_raw_block(void) {
    int rc = 0;

    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_historical_cu8_replay_fixture(metadata_path, sizeof(metadata_path), 96U, 131072U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    FirstBlockLevel& first = fresh_hook_context<FirstBlockLevel>();
    rc |= start_level_replay(metadata_path, &first, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    dsd_input_level_snapshot level;
    DSD_MEMSET(&level, 0, sizeof(level));
    int saw_level = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    while (dsd_time_monotonic_ns() - start_ns < 3000ULL * 1000000ULL) {
        rc |= expect_int_eq("legacy CU8 replay input level snapshot", rtl_stream_get_input_level(&level), 0);
        if (rc != 0) {
            break;
        }
        if (level.source == DSD_INPUT_LEVEL_SOURCE_RTL_CU8 && level.sample_count > 0U) {
            saw_level = 1;
            break;
        }
        dsd_sleep_ms(1U);
    }

    rc |= expect_true("legacy CU8 replay raw input level published", saw_level);
    if (saw_level) {
        rc |= expect_true("legacy CU8 replay raw DC level remains near zero RMS", level.rms_dbfs < -100.0);
    }

    rc |= finish_level_replay("legacy CU8 replay input level", ctx, &first);
    return rc;
}

static int
test_cf32_replay_fs4_level_uses_raw_block(void) {
    int rc = 0;

    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_constant_cf32_replay_fixture(metadata_path, sizeof(metadata_path), 0.5f, 0.5f, 16384U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    FirstBlockLevel& first = fresh_hook_context<FirstBlockLevel>();
    rc |= start_level_replay(metadata_path, &first, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    dsd_input_level_snapshot level;
    DSD_MEMSET(&level, 0, sizeof(level));
    int saw_level = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    while (dsd_time_monotonic_ns() - start_ns < 3000ULL * 1000000ULL) {
        rc |= expect_int_eq("CF32 replay input level snapshot", rtl_stream_get_input_level(&level), 0);
        if (rc != 0) {
            break;
        }
        if (level.source == DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32 && level.sample_count > 0U) {
            saw_level = 1;
            break;
        }
        dsd_sleep_ms(1U);
    }

    rc |= expect_true("CF32 replay raw input level published", saw_level);
    if (saw_level) {
        rc |= expect_true("CF32 replay raw DC level remains near zero RMS", level.rms_dbfs < -100.0);
        rc |= expect_true("CF32 replay raw peak still reflects DC offset",
                          level.peak_dbfs > -7.0 && level.peak_dbfs < -5.0);
    }

    rc |= finish_level_replay("CF32 replay input level", ctx, &first);
    return rc;
}

static int
test_replay_eof_partial_block_drains_before_exit(void) {
    int rc = 0;

    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              65536U + 2048U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream(metadata_path, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    float audio[1024];
    uint64_t start_ns = dsd_time_monotonic_ns();
    int saw_eof = 0;
    while (dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        rc |= expect_int_eq("replay state snapshot", dsd_rtl_stream_test_get_replay_state(&state), 0);
        if (state.should_exit) {
            rc |= expect_true("should_exit implies input ring drained", state.input_ring_used == 0U);
        }

        int got = 0;
        int read_rc = rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);
        if (read_rc == 0) {
            continue;
        }
        saw_eof = 1;
        break;
    }

    rc |= expect_true("replay reached EOF", saw_eof);

    rtl_stream_test_replay_state final_state;
    DSD_MEMSET(&final_state, 0, sizeof(final_state));
    rc |= expect_int_eq("final replay state snapshot", dsd_rtl_stream_test_get_replay_state(&final_state), 0);
    rc |= expect_int_eq("replay_input_eof", final_state.replay_input_eof, 1);
    rc |= expect_int_eq("replay_input_drained", final_state.replay_input_drained, 1);
    rc |= expect_int_eq("replay_demod_drained", final_state.replay_demod_drained, 1);
    rc |= expect_int_eq("replay_output_drained", final_state.replay_output_drained, 1);
    rc |= expect_int_eq("should_exit", final_state.should_exit, 1);
    rc |= expect_true("input ring empty at EOF exit", final_state.input_ring_used == 0U);
    rc |= expect_true("consume gen reaches EOF submit gen",
                      final_state.replay_last_consume_gen >= final_state.replay_last_submit_gen_at_eof);

    stop_and_destroy_stream(ctx);
    return rc;
}

static int
test_replay_output_tail_available_after_demod_drain(void) {
    int rc = 0;

    char metadata_path[DSD_TEST_PATH_MAX];
    rc |=
        make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1, 262144U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream(metadata_path, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    /* The decoder paces the replay: read a sample at a time until the demod has drained the input with the last
       block's output still in the ring. */
    int saw_tail_window = 0;
    uint64_t wait_start_ns = dsd_time_monotonic_ns();
    while (dsd_time_monotonic_ns() - wait_start_ns < 5000ULL * 1000000ULL) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) != 0) {
            rc |= 1;
            break;
        }
        if (state.replay_demod_drained && state.output_ring_used > 0U && !state.should_exit) {
            saw_tail_window = 1;
            break;
        }
        float sample = 0.0f;
        int got_one = 0;
        if (rtl_stream_read(ctx, &sample, 1U, &got_one) != 0) {
            break;
        }
    }

    rc |= expect_true("demod-drained with buffered output seen", saw_tail_window);

    float audio[1];
    int got = 0;
    int read_rc = rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);
    rc |= expect_int_eq("tail read rc", read_rc, 0);
    rc |= expect_true("tail read returned samples", got > 0);

    uint64_t drained_samples = 0;
    rc |= drain_stream_to_eof(ctx, 5000U, &drained_samples);
    rc |= expect_true("drained remaining replay output", drained_samples > 0U);

    stop_and_destroy_stream(ctx);
    return rc;
}

/* Issue #572: the read that takes a replay's last samples must not end the stream, even when the demod has already
 * drained by then, as it has in a fast replay and not in a realtime one still pacing. The decoder decodes those samples
 * after the read, and what it asks the stream meanwhile (whether it is active, the input level, the decode health) must
 * not depend on that timing. Here the decoder takes one sample of the capture's only block, waits for the demod to
 * drain, then takes the rest: the stream must stay open until the read after it, which finds the ring empty. */
static int
test_replay_last_read_leaves_stream_open(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1, 65536U)
        != 0) {
        return 1;
    }
    dsd_input_level_snapshot stale;
    (void)rtl_stream_get_input_level(&stale); /* with no stream running, this drops a level an earlier replay left */

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    float first = 0.0f;
    int got = 0;
    rc |= expect_int_eq("last read: first read rc", rtl_stream_read(ctx, &first, 1U, &got), 0);
    rc |= expect_int_eq("last read: first read takes one sample", got, 1);

    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int drained = 0;
    for (unsigned int waited = 0U; rc == 0 && waited < 5000U; waited++) {
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_demod_drained) {
            drained = 1;
            break;
        }
        dsd_sleep_ms(1U);
    }
    rc |= expect_true("last read: the demod drained with the block's output still in the ring",
                      drained && state.output_ring_used > 0U);

    std::vector<float> rest(state.output_ring_used + 64U);
    got = 0;
    rc |= expect_int_eq("last read: rest rc", rtl_stream_read(ctx, rest.data(), rest.size(), &got), 0);
    rc |= expect_int_eq("last read: rest takes the ring", got, (int)state.output_ring_used);

    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int_eq("last read: state after the last samples", dsd_rtl_stream_test_get_replay_state(&state), 0);
    rc |= expect_int_eq("last read: ring empty", (int)state.output_ring_used, 0);
    rc |= expect_int_eq("last read: output not drained yet", state.replay_output_drained, 0);
    rc |= expect_int_eq("last read: should_exit not set yet", state.should_exit, 0);
    rc |= expect_int_eq("last read: stream still active", rtl_stream_is_active(), 1);
    dsd_input_level_snapshot level;
    DSD_MEMSET(&level, 0, sizeof(level));
    rc |= expect_int_eq("last read: input level rc", rtl_stream_get_input_level(&level), 0);
    rc |= expect_true("last read: input level kept", level.sample_count > 0U);
    rtl_stream_p25p1_ber_update(3, 1);
    rtl_stream_decode_health health;
    DSD_MEMSET(&health, 0, sizeof(health));
    rc |= expect_int_eq("last read: decode health rc", rtl_stream_get_decode_health(&health), 0);
    rc |= expect_int_eq("last read: decode health taken", health.valid, 1);
    rc |= expect_int_eq("last read: decode health FEC ok", (int)health.p25p1_fec_ok, 3);

    float after = 0.0f;
    got = 0;
    rc |= expect_true("last read: the read after ends the stream", rtl_stream_read(ctx, &after, 1U, &got) != 0);
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int_eq("last read: state at the end", dsd_rtl_stream_test_get_replay_state(&state), 0);
    rc |= expect_int_eq("last read: output drained at the end", state.replay_output_drained, 1);
    rc |= expect_int_eq("last read: should_exit at the end", state.should_exit, 1);
    rc |= expect_int_eq("last read: stream inactive at the end", rtl_stream_is_active(), 0);

    stop_and_destroy_stream(ctx);
    return rc;
}

static int
test_eventful_replay_applies_scheduled_events(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_eventful_replay_fixture(metadata_path, sizeof(metadata_path), 131072U);
    if (rc != 0) {
        return 1;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[256] = {0};
    int prc = dsd_iq_replay_open(metadata_path, &cfg, NULL, err, sizeof(err));
    rc |= expect_int_eq("eventful replay metadata opens", prc, DSD_IQ_OK);
    if (prc == DSD_IQ_OK) {
        rc |= expect_int_eq("eventful metadata version", (int)cfg.metadata_version, 2);
        rc |= expect_int_eq("eventful event count", (int)cfg.event_count, 3);
    }
    dsd_iq_replay_config_clear(&cfg);
    if (rc != 0) {
        return rc;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream(metadata_path, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    uint64_t drained_samples = 0;
    rc |= drain_stream_to_eof(ctx, 5000U, &drained_samples);

    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int_eq("eventful replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
    rc |= expect_int_eq("scheduled retune count", (int)state.replay_event_retune_count, 1);
    rc |= expect_int_eq("scheduled mute count", (int)state.replay_event_mute_count, 1);
    rc |= expect_int_eq("scheduled reset count", (int)state.replay_event_reset_count, 1);
    rc |= expect_int_eq("scheduled last frequency", (int)state.replay_event_last_frequency_hz, 851500000);
    rc |= expect_int_eq("scheduled reset reason", state.replay_event_last_reset_reason, kReplayResetReasonFrequency);
    /* The reset's wait for its input purge is right only for exactly one request. */
    rc |= expect_true("scheduled reset made exactly one purge request",
                      state.replay_event_last_reset_purge_requests == 1ULL);
    rc |= expect_int_eq("scheduled reset purge mismatches", (int)state.replay_event_reset_purge_mismatch_count, 0);
    rc |= expect_true("scheduled mute duration recorded", state.replay_event_last_mute_bytes == 4096ULL);

    uint32_t applied_freq = 0;
    rc |= expect_int_eq("last applied freq hook", rtl_stream_get_last_applied_freq(&applied_freq), 0);
    rc |= expect_int_eq("last applied freq after reset", (int)applied_freq, 851500000);

    stop_and_destroy_stream(ctx);
    return rc;
}

static int
test_eventful_replay_preserves_ppm_reset_reason(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |=
        make_eventful_replay_fixture_with_reset_reason(metadata_path, sizeof(metadata_path), 131072U, "ppm-correction");
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream(metadata_path, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    uint64_t drained_samples = 0;
    rc |= drain_stream_to_eof(ctx, 5000U, &drained_samples);

    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int_eq("ppm replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
    rc |= expect_int_eq("ppm scheduled reset count", (int)state.replay_event_reset_count, 1);
    rc |= expect_int_eq("ppm scheduled reset reason", state.replay_event_last_reset_reason,
                        kReplayResetReasonPpmCorrection);
    rc |= expect_true("ppm scheduled reset made exactly one purge request",
                      state.replay_event_last_reset_purge_requests == 1ULL);
    rc |= expect_int_eq("ppm scheduled reset purge mismatches", (int)state.replay_event_reset_purge_mismatch_count, 0);

    stop_and_destroy_stream(ctx);
    return rc;
}

static int
test_loop_replay_reapplies_event_timeline(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_eventful_replay_fixture(metadata_path, sizeof(metadata_path), 32768U);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    float audio[1024];
    int saw_repeated_events = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    while (dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL) {
        int got = 0;
        (void)rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);

        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) != 0) {
            rc |= 1;
            break;
        }
        if (state.replay_event_retune_count >= 2U && state.replay_event_mute_count >= 2U
            && state.replay_event_reset_count >= 2U) {
            saw_repeated_events = 1;
            break;
        }
        dsd_sleep_ms(2U);
    }

    rc |= expect_true("loop replay reapplies scheduled events", saw_repeated_events);
    rtl_stream_test_replay_state final_state;
    DSD_MEMSET(&final_state, 0, sizeof(final_state));
    rc |= expect_int_eq("loop replay final state", dsd_rtl_stream_test_get_replay_state(&final_state), 0);
    rc |= expect_true("loop replay restored initial state", final_state.replay_loop_restart_count >= 1U);
    rc |= expect_int_eq("loop replay restored initial frequency",
                        (int)final_state.replay_loop_restart_last_frequency_hz, 851375000);
    stop_and_destroy_stream(ctx);
    return rc;
}

static int
test_realtime_loop_waits_for_terminal_mute_before_rewind(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    const uint64_t terminal_mute_bytes = 307200ULL;
    rc |= make_terminal_mute_replay_fixture(metadata_path, sizeof(metadata_path), terminal_mute_bytes);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    rc |= start_replay_stream_with_loop_and_rate(metadata_path, 1, 1, &opts, &ctx);
    if (rc != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }
    /* A decoder reads the replay on its own thread, so this one sees the events as they happen. */
    LoopReader reader;
    if (start_loop_reader(&reader, ctx) != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    uint64_t mute_seen_ns = 0;
    uint64_t restart_seen_ns = 0;
    uint64_t deadline_ns = dsd_time_monotonic_ns() + 3000ULL * 1000000ULL;
    while (dsd_time_monotonic_ns() < deadline_ns) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        rc |= expect_int_eq("terminal mute replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
        if (rc != 0) {
            break;
        }

        uint64_t now_ns = dsd_time_monotonic_ns();
        if (mute_seen_ns == 0ULL && state.replay_event_mute_count >= 1U) {
            mute_seen_ns = now_ns;
        }
        if (mute_seen_ns != 0ULL && state.replay_loop_restart_count >= 1U) {
            restart_seen_ns = now_ns;
            break;
        }
        dsd_sleep_ms(1U);
    }

    rc |= expect_true("terminal mute event observed", mute_seen_ns != 0ULL);
    rc |= expect_true("loop restart observed after terminal mute", restart_seen_ns != 0ULL);
    if (mute_seen_ns != 0ULL && restart_seen_ns != 0ULL) {
        uint64_t elapsed_ms = (restart_seen_ns - mute_seen_ns) / 1000000ULL;
        rc |= expect_u64_ge("realtime loop terminal mute interval", elapsed_ms, 40ULL);
    }

    stop_loop_reader(&reader);
    stop_and_destroy_stream(ctx);
    if (reader.rescued) {
        dsd_exitflag_store(0);
        rc |= 1;
    }
    return rc;
}

static int
collect_replay_samples(const char* metadata_path, size_t target_samples, std::vector<float>* out_samples) {
    if (!metadata_path || !out_samples || target_samples == 0U) {
        return 1;
    }
    out_samples->clear();

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        stop_and_destroy_stream(ctx);
        return 1;
    }

    float buf[1024];
    uint64_t start_ns = dsd_time_monotonic_ns();
    while (out_samples->size() < target_samples && dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL) {
        int got = 0;
        int rc = rtl_stream_read(ctx, buf, sizeof(buf) / sizeof(buf[0]), &got);
        if (rc != 0) {
            break;
        }
        if (got > 0) {
            size_t take = std::min((size_t)got, target_samples - out_samples->size());
            out_samples->insert(out_samples->end(), buf, buf + take);
        }
    }

    stop_and_destroy_stream(ctx);
    return 0;
}

static int
test_cf32_replay_fs4_policy_changes_output(void) {
    int rc = 0;

    char meta_fs4_off[DSD_TEST_PATH_MAX];
    char meta_fs4_on[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(meta_fs4_off, sizeof(meta_fs4_off), DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 0,
                              1048576U);
    rc |= make_replay_fixture(meta_fs4_on, sizeof(meta_fs4_on), DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 1,
                              1048576U);
    if (rc != 0) {
        return 1;
    }

    std::vector<float> a;
    std::vector<float> b;
    rc |= collect_replay_samples(meta_fs4_off, 1024U, &a);
    rc |= collect_replay_samples(meta_fs4_on, 1024U, &b);
    rc |= expect_true("cf32 replay fs4=off produced samples", a.size() >= 128U);
    rc |= expect_true("cf32 replay fs4=on produced samples", b.size() >= 128U);
    if (rc != 0) {
        return rc;
    }

    size_t n = std::min(a.size(), b.size());
    double sad = 0.0;
    for (size_t i = 0; i < n; i++) {
        sad += std::fabs((double)a[i] - (double)b[i]);
    }
    rc |= expect_true("cf32 replay output differs with fs4 policy", sad > 1e-3 * (double)n);
    return rc;
}

static int
test_cf32_unknown_capture_stage_rejected(void) {
    int rc = 0;

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_stage")) {
        DSD_FPRINTF(stderr, "FAIL: could not create temporary metadata directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "bad.iq", "bad.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    rc |= expect_int_eq("join data path", dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "bad.iq"), 0);
    rc |= expect_int_eq("join meta path", dsd_test_path_join(meta_path, sizeof(meta_path), temp_dir, "bad.iq.json"), 0);
    if (rc != 0) {
        return rc;
    }

    uint8_t data[16] = {0};
    rc |= expect_int_eq("write cf32 data", write_bytes_file(data_path, data, sizeof(data)), 0);

    const char* json = "{\n"
                       "  \"format\": \"dsd-neo-iq\",\n"
                       "  \"version\": 1,\n"
                       "  \"sample_format\": \"cf32\",\n"
                       "  \"iq_order\": \"IQ\",\n"
                       "  \"endianness\": \"little\",\n"
                       "  \"capture_stage\": \"post_driver_cf32_after_ring\",\n"
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
                       "  \"capture_started_utc\": \"2026-04-12T00:00:00Z\",\n"
                       "  \"data_file\": \"bad.iq\",\n"
                       "  \"data_bytes\": 16,\n"
                       "  \"capture_drops\": 0,\n"
                       "  \"capture_drop_blocks\": 0,\n"
                       "  \"input_ring_drops\": 0,\n"
                       "  \"notes\": \"\"\n"
                       "}\n";
    rc |= expect_int_eq("write bad metadata", write_text_file(meta_path, json), 0);
    if (rc != 0) {
        return rc;
    }

    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[256] = {0};
    int prc = dsd_iq_replay_read_metadata(meta_path, &cfg, err, sizeof(err));
    rc |= expect_int_eq("unknown cf32 capture_stage rejected", prc, DSD_IQ_ERR_UNSUPPORTED_FMT);
    return rc;
}

/* The stream's own output clear (rtl_sdr_fm.cpp), which a CQPSK toggle or a reacquire runs from the decoder thread. */
extern "C" void dsd_rtl_stream_clear_output(void);

/* One replay reader chunk (rtl_device.cpp replay_thread_fn()): a capture this size reaches the demod as one block. */
static const size_t kReplayChunkBytes = 65536U;

namespace {
/* The decoder's side of a replay, run on its own thread so a test can bound how long the end of the stream takes to
 * reach it. */
struct ReplayReader {
    RtlSdrContext* ctx = nullptr;
    std::atomic<int> done{0};
    std::atomic<uint64_t> samples{0U};
};
} // namespace

static DSD_THREAD_RETURN_TYPE
replay_reader_fn(void* arg) {
    ReplayReader* reader = static_cast<ReplayReader*>(arg);
    float audio[2048];
    for (;;) {
        int got = 0;
        if (rtl_stream_read(reader->ctx, audio, sizeof(audio) / sizeof(audio[0]), &got) != 0) {
            break;
        }
        if (got > 0) {
            reader->samples.fetch_add((uint64_t)got, std::memory_order_relaxed);
        }
    }
    reader->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Read the replay on a decoder thread until a read reports the end of the stream. A stream that has not ended after
 * @p timeout_ms is ended from outside with the global exit flag, which every replay wait checks, so the test fails
 * instead of hanging; *out_rescued is then 1, and finish_replay() has to stop the stream before the flag clears. */
static int
read_replay_to_end(RtlSdrContext* ctx, uint64_t timeout_ms, const char* label, uint64_t* out_samples,
                   int* out_rescued) {
    *out_samples = 0U;
    *out_rescued = 0;
    ReplayReader reader;
    reader.ctx = ctx;
    dsd_thread_t thread;
    if (dsd_thread_create(&thread, replay_reader_fn, &reader) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not start the decoder thread\n", label);
        return 1;
    }
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + timeout_ms * 1000000ULL;
    while (!reader.done.load(std::memory_order_acquire) && dsd_time_monotonic_ns() < deadline_ns) {
        dsd_sleep_ms(1U);
    }
    int rc = 0;
    if (!reader.done.load(std::memory_order_acquire)) {
        DSD_FPRINTF(stderr, "FAIL: %s: the end of the replay did not reach the decoder within %llu ms\n", label,
                    (unsigned long long)timeout_ms);
        *out_rescued = 1;
        dsd_exitflag_store(1);
        rc = 1;
    }
    (void)dsd_thread_join(thread);
    *out_samples = reader.samples.load(std::memory_order_relaxed);
    return rc;
}

/* Stop and destroy a replay that read_replay_to_end() ran, then clear the exit flag a rescue raised. */
static void
finish_replay(RtlSdrContext* ctx, int rescued) {
    stop_and_destroy_stream(ctx);
    if (rescued) {
        dsd_exitflag_store(0);
    }
}

/* Wait up to @p timeout_ms for @p flag to become nonzero. */
static int
wait_for_flag(const std::atomic<int>* flag, unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited < timeout_ms; waited++) {
        if (flag->load(std::memory_order_acquire)) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return flag->load(std::memory_order_acquire) ? 1 : 0;
}

/* Wait up to @p timeout_ms for the demod to report the replay input drained. */
static int
wait_for_demod_drained(unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited < timeout_ms; waited++) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_demod_drained) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* Wait up to @p timeout_ms for the replay reader thread to leave. It stores that after the EOF sequence, so the
 * decoder can see the stream end first; a test that asserts the reader left waits for it here. */
static int
wait_for_reader_exited(unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_reader_exited) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

namespace {
/* One run of the EOF drain race on a one-chunk capture (test_replay_eof_drain_delivers_final_block()). */
struct EofDrainRace {
    std::atomic<int> reserve_delay_ms{0};
    std::atomic<int> demod_holding{0};    /* the demod holds the block before it publishes the block's output */
    std::atomic<int> decoder_parked{0};   /* the decoder found the ring empty while the demod held the block */
    std::atomic<int> decoder_released{0}; /* ... and went on once the demod was reported drained */
    std::atomic<int> demod_decisions{0};
    std::atomic<int> reader_decisions{0};
    std::atomic<int> output_writes{0};
    std::atomic<uint64_t> written{0U};
};
} // namespace

/* The first arrival of each side waits for the other side's first arrival, so both drain decisions are made together. */
static void
eof_drain_race_barrier(std::atomic<int>* mine, const std::atomic<int>* other) {
    if (mine->fetch_add(1, std::memory_order_acq_rel) == 0) {
        (void)wait_for_flag(other, 1000U);
    }
}

static void
eof_drain_race_stage(int stage, size_t count, void* ctx) {
    EofDrainRace* race = static_cast<EofDrainRace*>(ctx);
    switch (stage) {
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE: {
            int delay_ms = race->reserve_delay_ms.load(std::memory_order_relaxed);
            if (delay_ms > 0) {
                dsd_sleep_ms((unsigned int)delay_ms);
            }
            break;
        }
        case RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE:
            /* The only block of a one-chunk capture: hold its output until the decoder has looked at the empty ring. */
            if (race->output_writes.load(std::memory_order_acquire) == 0) {
                race->demod_holding.store(1, std::memory_order_release);
                (void)wait_for_flag(&race->decoder_parked, 2000U);
            }
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_OUTPUT_WRITE:
            race->written.fetch_add((uint64_t)count, std::memory_order_relaxed);
            race->output_writes.fetch_add(1, std::memory_order_release);
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_DRAIN_DECISION:
            eof_drain_race_barrier(&race->demod_decisions, &race->reader_decisions);
            break;
        case RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECISION:
            eof_drain_race_barrier(&race->reader_decisions, &race->demod_decisions);
            break;
        case RTL_STREAM_TEST_REPLAY_DECODER_OUTPUT_EMPTY:
            /* The decoder has seen the ring empty: let the demod publish the final block and be reported drained before
               the decoder decides whether the stream has ended. */
            if (race->demod_holding.load(std::memory_order_acquire)
                && !race->decoder_parked.load(std::memory_order_acquire)) {
                race->decoder_parked.store(1, std::memory_order_release);
                if (wait_for_demod_drained(2000U)) {
                    race->decoder_released.store(1, std::memory_order_release);
                }
            }
            break;
        default: break;
    }
}

/* Issue #572: the decoder decides that the replay ended from what the demod reports once it drained the input, and
 * that report comes after the demod publishes its last output. A decoder that looks at the ring before it reads the
 * report can find the ring empty, then read "drained" once the final block landed, and end the stream with that block
 * unread. The drain decisions themselves are made at once on both sides (demod and replay reader), which must not lose
 * the wakeup either. Every sample the demod wrote must reach the decoder. */
static int
test_replay_eof_drain_delivers_final_block(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    for (int rep = 0; rep < 20; rep++) {
        EofDrainRace& race = fresh_hook_context<EofDrainRace>();
        race.reserve_delay_ms.store((rep & 1) ? 5 : 0, std::memory_order_relaxed);
        rtl_stream_test_set_replay_stage_hook(eof_drain_race_stage, &race);

        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            stop_and_destroy_stream(ctx);
            return 1;
        }
        uint64_t delivered = 0U;
        int rescued = 0;
        rc |= read_replay_to_end(ctx, 5000U, "EOF drain race", &delivered, &rescued);

        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        rc |= expect_int_eq("EOF drain race replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
        const uint64_t written = race.written.load(std::memory_order_relaxed);
        rc |= expect_true("EOF drain race: the decoder looked at the empty ring while the demod held the block",
                          race.decoder_parked.load());
        rc |= expect_true("EOF drain race: the demod was reported drained while the decoder waited",
                          race.decoder_released.load());
        rc |= expect_true("EOF drain race: both sides made a drain decision",
                          race.demod_decisions.load() > 0 && race.reader_decisions.load() > 0);
        rc |= expect_true("EOF drain race: the demod published output", written > 0U);
        if (delivered != written) {
            DSD_FPRINTF(stderr,
                        "FAIL: EOF drain race rep %d: the decoder read %llu of the %llu samples the demod wrote\n", rep,
                        (unsigned long long)delivered, (unsigned long long)written);
            rc |= 1;
        }
        rc |= expect_int_eq("EOF drain race demod drained", state.replay_demod_drained, 1);
        rc |= expect_int_eq("EOF drain race output drained", state.replay_output_drained, 1);

        finish_replay(ctx, rescued);
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    }
    return rc;
}

namespace {
/* test_replay_read_survives_clear_at_eof(): clear the output once the decoder has seen the final block queued. */
struct ClearAtEof {
    std::atomic<int> cleared{0};
};
} // namespace

static void
clear_at_eof_stage(int stage, size_t count, void* ctx) {
    (void)count;
    ClearAtEof* clear = static_cast<ClearAtEof*>(ctx);
    if (stage != RTL_STREAM_TEST_REPLAY_DECODER_OUTPUT_FOUND || clear->cleared.load(std::memory_order_acquire)) {
        return;
    }
    /* Nothing is written after this block: once the demod reports itself drained, the clear leaves an empty ring
       nothing will fill again. */
    if (wait_for_demod_drained(2000U)) {
        dsd_rtl_stream_clear_output();
        clear->cleared.store(1, std::memory_order_release);
    }
}

/* Issue #572: an output clear (a CQPSK toggle, a reacquire) can land between the decoder's look at the ring and its
 * copy. At the end of a replay nothing fills the ring again, so a read that then waits for samples must still see
 * that the stream ended, rather than wait forever. */
static int
test_replay_read_survives_clear_at_eof(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    ClearAtEof& clear = fresh_hook_context<ClearAtEof>();
    rtl_stream_test_set_replay_stage_hook(clear_at_eof_stage, &clear);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 3000U, "clear at EOF", &delivered, &rescued);
    rc |= expect_true("clear at EOF: the output was cleared under the decoder's read", clear.cleared.load());
    finish_replay(ctx, rescued);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    return rc;
}

/* Issue #572: a capture that cannot be read ends the replay the way its end does. The demod and the decoder drain
 * what the reader delivered, the stream reports its end to the decoder, the read error is reported as a file input
 * failure, and nothing raises the global exit flag. */
static int
test_replay_read_error_ends_stream(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              8U * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    dsd_input_failure_clear();
    rtl_device_test_replay_inject_read_error(3, DSD_IQ_ERR_IO);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_device_test_replay_inject_read_error(-1, 0);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 5000U, "read error", &delivered, &rescued);

    (void)wait_for_reader_exited(2000U);
    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    rc |= expect_int_eq("read error replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
    rc |= expect_true("read failure: what was read before the error reached the decoder", delivered > 0U);
    rc |= expect_int_eq("read failure: input EOF", state.replay_input_eof, 1);
    rc |= expect_int_eq("read failure: demod drained", state.replay_demod_drained, 1);
    rc |= expect_int_eq("read failure: output drained", state.replay_output_drained, 1);
    rc |= expect_int_eq("read failure: the replay reader left", state.replay_reader_exited, 1);
    if (!rescued) {
        rc |= expect_int_eq("read failure: no global exit", (int)dsd_exitflag_load(), 0);
    }
    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    rc |= expect_int_eq("read error reported as a file input failure", failure.kind, DSD_INPUT_FAILURE_FILE);
    rc |= expect_int_eq("read error code reported", failure.native_code, DSD_IQ_ERR_IO);

    finish_replay(ctx, rescued);
    rtl_device_test_replay_inject_read_error(-1, 0);
    dsd_input_failure_clear();
    return rc;
}

namespace {
struct ReplayStart {
    RtlSdrContext* ctx = nullptr;
    std::atomic<int> done{0};
    int rc = 0;
};
} // namespace

static DSD_THREAD_RETURN_TYPE
replay_start_fn(void* arg) {
    ReplayStart* start = static_cast<ReplayStart*>(arg);
    start->rc = rtl_stream_start(start->ctx);
    start->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* Issue #572: the replay reader failing to start after the demod thread started unwinds the start in bounded time,
 * without the global exit flag (the application keeps running), and leaves nothing behind: the next replay runs. */
static int
test_replay_reader_start_failure_unwinds(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    std::unique_ptr<dsd_opts> opts(new dsd_opts());
    prepare_replay_opts(opts.get(), metadata_path);
    ReplayStart start;
    if (rtl_stream_create(opts.get(), &start.ctx) != 0 || !start.ctx) {
        DSD_FPRINTF(stderr, "FAIL: reader start failure: rtl_stream_create failed\n");
        return 1;
    }
    rtl_device_test_replay_fail_start(1);
    dsd_thread_t thread;
    if (dsd_thread_create(&thread, replay_start_fn, &start) != 0) {
        rtl_device_test_replay_fail_start(0);
        rtl_stream_destroy(start.ctx);
        return 1;
    }
    int finished = wait_for_flag(&start.done, 5000U);
    rc |= expect_true("reader start failure: the start returned within 5 s", finished);
    if (finished) {
        rc |= expect_int_eq("reader start failure: no global exit", (int)dsd_exitflag_load(), 0);
    } else {
        dsd_exitflag_store(1);
    }
    (void)dsd_thread_join(thread);
    rtl_device_test_replay_fail_start(0);
    rc |= expect_true("reader start failure: the start failed", start.rc != 0);
    rtl_stream_destroy(start.ctx);
    dsd_exitflag_store(0);
    rc |= expect_int_eq("reader start failure: no stream resources left", rtl_stream_test_has_resources(), 0);

    std::unique_ptr<dsd_opts> next_opts;
    RtlSdrContext* next = NULL;
    rc |= start_replay_stream(metadata_path, &next_opts, &next);
    if (next) {
        uint64_t delivered = 0U;
        int rescued = 0;
        rc |= read_replay_to_end(next, 5000U, "replay after a reader start failure", &delivered, &rescued);
        rc |= expect_true("replay after a reader start failure delivered output", delivered > 0U);
        finish_replay(next, rescued);
    }
    return rc;
}

namespace {
/* test_replay_eof_wait_does_not_spin(): the demod holds the capture's only block once the reader reached EOF. */
struct SlowLastBlock {
    std::atomic<int> held{0};
};
} // namespace

static void
slow_last_block_stage(int stage, size_t count, void* ctx) {
    (void)count;
    SlowLastBlock* slow = static_cast<SlowLastBlock*>(ctx);
    if (stage != RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE || slow->held.exchange(1, std::memory_order_acq_rel)) {
        return;
    }
    for (unsigned int waited = 0U; waited < 2000U; waited++) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_input_eof) {
            break;
        }
        dsd_sleep_ms(1U);
    }
    dsd_sleep_ms(300U);
}

/* Issue #572: at EOF the replay reader waits for the demod to take the rest of the input ring. While a slow demod
 * holds the last block (300 ms here), that wait must sleep on the ring, not spin: a 50 ms timed wait looks at the
 * ring a handful of times. */
static int
test_replay_eof_wait_does_not_spin(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    SlowLastBlock& slow = fresh_hook_context<SlowLastBlock>();
    rtl_stream_test_set_replay_stage_hook(slow_last_block_stage, &slow);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 5000U, "slow last block", &delivered, &rescued);
    const uint64_t iterations = rtl_device_test_replay_eof_wait_iterations();
    rc |= expect_true("slow last block: the demod held the block", slow.held.load());
    rc |= expect_true("slow last block: the block reached the decoder", delivered > 0U);
    if (iterations > 50U) {
        DSD_FPRINTF(stderr, "FAIL: the EOF input wait looked at the ring %llu times in about 300 ms (want <= 50)\n",
                    (unsigned long long)iterations);
        rc |= 1;
    }
    finish_replay(ctx, rescued);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    return rc;
}

/* One replay reader chunk in floats: cu8 widens each byte to a float, cf32 carries a float in every 4 bytes. */
static const size_t kCu8ChunkFloats = kReplayChunkBytes;
static const size_t kCf32ChunkFloats = kReplayChunkBytes / sizeof(float);
/* fill_capture_cfg()'s sample rate. */
static const uint64_t kFixtureSampleRateHz = 1536000U;

/* Capture time of @p complex_samples at the fixtures' sample rate, rounded down. */
static uint64_t
fixture_media_ns(uint64_t complex_samples) {
    return (complex_samples * 1000000000ULL) / kFixtureSampleRateHz;
}

namespace {
/* One demod block as the block hook reported it. */
struct LoggedBlock {
    uint64_t sequence;
    uint64_t submit_gen;
    uint64_t media_start_ns;
    uint64_t media_end_ns;
    int have_input_level;
    size_t float_count;
};

/* Every block the demod took during a replay, with the floats they held when keep_samples is set. The demod thread
 * writes it; the test reads it once the stream is stopped. */
struct BlockLog {
    int keep_samples = 0;
    std::vector<LoggedBlock> blocks;
    std::vector<float> samples;
};

/* A capture chunk the replay reader hands the demod as one block. */
struct ExpectedChunk {
    size_t float_count;
    uint64_t start_complex; /* capture position of its first sample, samples a MUTE omitted included */
};
} // namespace

static void
block_log_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    BlockLog* log = static_cast<BlockLog*>(ctx);
    LoggedBlock entry = {block->sequence,     block->submit_gen,       block->media_start_ns,
                         block->media_end_ns, block->have_input_level, block->float_count};
    log->blocks.push_back(entry);
    if (!log->keep_samples) {
        return;
    }
    if (block->p1 && block->n1 > 0U) {
        log->samples.insert(log->samples.end(), block->p1, block->p1 + block->n1);
    }
    if (block->p2 && block->n2 > 0U) {
        log->samples.insert(log->samples.end(), block->p2, block->p2 + block->n2);
    }
}

/* One block a chunk: each block has its chunk's length, sequence and submit generation (both count chunks from 1 here)
 * and media span. */
static int
expect_blocks_match_chunks(const char* label, const BlockLog& log, const std::vector<ExpectedChunk>& chunks) {
    int rc = 0;
    if (log.blocks.size() != chunks.size()) {
        DSD_FPRINTF(stderr, "FAIL: %s: the demod took %zu blocks for the capture's %zu chunks\n", label,
                    log.blocks.size(), chunks.size());
        rc = 1;
    }
    const size_t n = std::min(log.blocks.size(), chunks.size());
    for (size_t i = 0; i < n; i++) {
        const LoggedBlock& got = log.blocks[i];
        const ExpectedChunk& want = chunks[i];
        const uint64_t want_start_ns = fixture_media_ns(want.start_complex);
        const uint64_t want_end_ns = fixture_media_ns(want.start_complex + want.float_count / 2U);
        if (got.float_count != want.float_count || got.sequence != i + 1U || got.submit_gen != i + 1U
            || got.media_start_ns != want_start_ns || got.media_end_ns != want_end_ns) {
            DSD_FPRINTF(stderr,
                        "FAIL: %s: block %zu has %zu floats, sequence %llu, generation %llu, media %llu-%llu ns; "
                        "chunk %zu has %zu floats, media %llu-%llu ns\n",
                        label, i + 1U, got.float_count, (unsigned long long)got.sequence,
                        (unsigned long long)got.submit_gen, (unsigned long long)got.media_start_ns,
                        (unsigned long long)got.media_end_ns, i + 1U, want.float_count,
                        (unsigned long long)want_start_ns, (unsigned long long)want_end_ns);
            rc = 1;
        }
    }
    return rc;
}

/* Wait up to @p timeout_ms for the replay reader to catch up with the demod: it has read chunk @p seq and waits for the
 * demod to take the one before it, or it has already read the whole capture (a reader that runs ahead). */
static int
wait_for_reader_caught_up(const std::atomic<uint64_t>* reader_waiting_seq, uint64_t seq, unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (reader_waiting_seq->load(std::memory_order_acquire) >= seq) {
            return 1;
        }
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_input_eof) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

namespace {
/* test_replay_blocks_follow_capture_chunks(): hold the first block after the events until the reader caught up. */
struct ChunkFraming {
    std::atomic<int> blocks{0};
    std::atomic<uint64_t> reader_waiting_seq{0U};
    std::atomic<int> held{0};
};
} // namespace

static void
chunk_framing_stage(int stage, size_t count, void* ctx) {
    ChunkFraming* framing = static_cast<ChunkFraming*>(ctx);
    if (stage == RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT) {
        framing->reader_waiting_seq.store((uint64_t)count, std::memory_order_release);
        return;
    }
    /* Block 1 is the chunk before the events, and the RESET among them waits for the demod to finish it. Hold block 2,
       the first after the events: a reader that runs ahead has queued the rest of the capture by the time it goes on,
       and the demod would take all of it as one block. */
    if (stage == RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE
        && framing->blocks.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
        framing->held.store(wait_for_reader_caught_up(&framing->reader_waiting_seq, 3U, 2000U),
                            std::memory_order_release);
    }
}

/* Issue #572: every demod block is exactly one capture chunk, a whole 64 KiB read or the shorter read an event cut,
 * and carries the chunk's sequence, submit generation and media span (time a MUTE omitted included), in fast and
 * realtime replay alike. A demod that takes whatever the input ring holds makes its blocks, and so its output, depend on
 * how far ahead the reader got. */
static int
test_replay_blocks_follow_capture_chunks(int realtime) {
    const char* label = realtime ? "chunk framing (realtime)" : "chunk framing (fast)";
    const size_t first_bytes = 4096U; /* make_eventful_replay_fixture(): the events follow the first 4096 bytes */
    const uint64_t mute_complex = 4096U / 2U; /* ... and its MUTE omits 4096 bytes */
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_eventful_replay_fixture(metadata_path, sizeof(metadata_path), first_bytes + 3U * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    std::vector<ExpectedChunk> chunks;
    chunks.push_back(ExpectedChunk{first_bytes, 0U});
    uint64_t position = first_bytes / 2U + mute_complex;
    for (int i = 0; i < 3; i++) {
        chunks.push_back(ExpectedChunk{kCu8ChunkFloats, position});
        position += kCu8ChunkFloats / 2U;
    }

    BlockLog& log = fresh_hook_context<BlockLog>();
    ChunkFraming& framing = fresh_hook_context<ChunkFraming>();
    rtl_stream_test_set_replay_block_hook(block_log_hook, &log);
    rtl_stream_test_set_replay_stage_hook(chunk_framing_stage, &framing);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream_with_loop_and_rate(metadata_path, 0, realtime, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 5000U, label, &delivered, &rescued);
    finish_replay(ctx, rescued);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);

    if (!framing.held.load()) {
        DSD_FPRINTF(stderr, "FAIL: %s: the reader did not catch up while the demod held the block after the events\n",
                    label);
        rc |= 1;
    }
    rc |= expect_blocks_match_chunks(label, log, chunks);
    for (size_t i = 0; i < log.blocks.size(); i++) {
        if (!log.blocks[i].have_input_level) {
            DSD_FPRINTF(stderr, "FAIL: %s: block %zu carries no input level\n", label, i + 1U);
            rc |= 1;
        }
    }
    return rc;
}

namespace {
/* The reader's side of the acknowledgement tests: every chunk after the first is held numbered (its submit generation
 * bumped) but uncommitted until the demod has acknowledged the block before it (hold_numbered_chunk()). */
struct NumberedChunkHold {
    std::atomic<uint64_t> reader_holding_gen{0U}; /* the chunk the reader holds numbered, not committed */
    std::atomic<int> windows{0};                  /* chunks held until the demod acknowledged the block before them */
    std::atomic<int> timeouts{0};                 /* holds the demod never answered */
    std::atomic<int> violations{0};
    std::atomic<uint64_t> violation_consumed{0U};
    std::atomic<uint64_t> violation_gen{0U};
};
} // namespace

static uint64_t
replay_consumed_generation(void) {
    rtl_stream_test_replay_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    return dsd_rtl_stream_test_get_replay_state(&state) == 0 ? state.replay_last_consume_gen : 0U;
}

static int
wait_for_u64_at_least(const std::atomic<uint64_t>* value, uint64_t want, unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        if (value->load(std::memory_order_acquire) >= want) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

/* Chunk @p gen is numbered and outside the ring: hold it there until the demod acknowledged block gen-1, and record
 * whether the demod acknowledged chunk gen, which it cannot have read. */
static void
hold_numbered_chunk(NumberedChunkHold* hold, uint64_t gen) {
    hold->reader_holding_gen.store(gen, std::memory_order_release);
    for (unsigned int waited = 0U; waited <= 2000U; waited++) {
        const uint64_t consumed = replay_consumed_generation();
        if (consumed >= gen - 1U) {
            if (consumed >= gen) {
                hold->violation_consumed.store(consumed, std::memory_order_relaxed);
                hold->violation_gen.store(gen, std::memory_order_relaxed);
                hold->violations.fetch_add(1, std::memory_order_acq_rel);
            } else {
                hold->windows.fetch_add(1, std::memory_order_acq_rel);
            }
            return;
        }
        dsd_sleep_ms(1U);
    }
    hold->timeouts.fetch_add(1, std::memory_order_acq_rel);
}

static int
expect_numbered_chunk_holds(const char* label, const NumberedChunkHold& hold, int want_windows) {
    int rc = 0;
    if (hold.violations.load() != 0) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: the demod acknowledged generation %llu while the reader held chunk %llu numbered but "
                    "not committed\n",
                    label, (unsigned long long)hold.violation_consumed.load(),
                    (unsigned long long)hold.violation_gen.load());
        rc = 1;
    }
    if (hold.windows.load() != want_windows) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: %d chunks were held uncommitted while the demod acknowledged the block before them "
                    "(want %d)\n",
                    label, hold.windows.load(), want_windows);
        rc = 1;
    }
    if (hold.timeouts.load() != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d reader holds timed out\n", label, hold.timeouts.load());
        rc = 1;
    }
    return rc;
}

namespace {
/* test_replay_demod_never_acknowledges_an_unread_chunk(). */
struct AckWindow {
    uint64_t chunks = 0U;
    std::atomic<uint64_t> blocks{0U};
    std::atomic<uint64_t> reader_waiting_seq{0U};
    NumberedChunkHold hold;
    std::atomic<int> demod_timeouts{0}; /* demod waits the reader never answered */
};
} // namespace

static void
ack_window_stage(int stage, size_t count, void* ctx) {
    AckWindow* window = static_cast<AckWindow*>(ctx);
    switch (stage) {
        case RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT:
            window->reader_waiting_seq.store((uint64_t)count, std::memory_order_release);
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE: {
            /* Block k: let the reader get to chunk k+1 first. A reader that runs ahead numbers it before the demod
               looks at a generation; one that waits for the demod has read it and waits for the ring to empty. */
            const uint64_t block = window->blocks.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            if (block >= window->chunks) {
                break;
            }
            int reached = 0;
            for (unsigned int waited = 0U; !reached && waited <= 2000U; waited++) {
                reached = window->reader_waiting_seq.load(std::memory_order_acquire) > block
                          || window->hold.reader_holding_gen.load(std::memory_order_acquire) > block;
                if (!reached) {
                    dsd_sleep_ms(1U);
                }
            }
            if (!reached) {
                window->demod_timeouts.fetch_add(1, std::memory_order_acq_rel);
            }
            break;
        }
        case RTL_STREAM_TEST_REPLAY_DEMOD_DRAIN_DECISION: {
            /* Acknowledge block k only while the reader holds chunk k+1 numbered but not committed. */
            const uint64_t block = window->blocks.load(std::memory_order_acquire);
            if (block < window->chunks && !wait_for_u64_at_least(&window->hold.reader_holding_gen, block + 1U, 2000U)) {
                window->demod_timeouts.fetch_add(1, std::memory_order_acq_rel);
            }
            break;
        }
        case RTL_STREAM_TEST_REPLAY_READER_BEFORE_COMMIT:
            if (count >= 2U) {
                hold_numbered_chunk(&window->hold, (uint64_t)count);
            }
            break;
        default: break;
    }
}

/* Issue #572: the replay reader numbers a chunk (bumps the submit generation) just before it commits the chunk to the
 * input ring. A demod that acknowledges the newest submitted generation, not the one its own block carries, can
 * acknowledge a chunk it never read, and at EOF the reader then takes the demod for drained while it still holds the
 * last chunk. Every chunk after the first is held numbered but uncommitted until the demod acknowledged the block before
 * it, and a reader that runs ahead gets to number it before the demod takes the generation it acknowledges. The
 * whole-chunk handoff keeps the reader from running ahead; the two tests after this one cover the windows where the
 * demod releases a block's input before it takes that generation. */
static int
test_replay_demod_never_acknowledges_an_unread_chunk(void) {
    const uint64_t chunks = 4U;
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              (size_t)chunks * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    AckWindow& window = fresh_hook_context<AckWindow>();
    window.chunks = chunks;
    rtl_stream_test_set_replay_stage_hook(ack_window_stage, &window);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 10000U, "unread chunk", &delivered, &rescued);
    finish_replay(ctx, rescued);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);

    rc |= expect_numbered_chunk_holds("unread chunk", window.hold, (int)(chunks - 1U));
    rc |= expect_int_eq("unread chunk: demod waits that timed out", window.demod_timeouts.load(), 0);
    rc |= expect_true("unread chunk: the replay delivered output", delivered > 0U);
    return rc;
}

namespace {
/* run_early_release_window(): the demod releases one block's input before it takes the generation it acknowledges. */
struct EarlyReleaseWindow {
    int stage = 0;                               /* the demod's early-release stage this run holds at */
    std::atomic<uint64_t> block_sequence{0U};    /* the chunk the demod's current block holds */
    std::atomic<uint64_t> released_sequence{0U}; /* the chunk whose block released its input early */
    std::atomic<int> early_releases{0};
    NumberedChunkHold hold;
    std::atomic<int> demod_timeouts{0};
};
} // namespace

static void
early_release_block_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    static_cast<EarlyReleaseWindow*>(ctx)->block_sequence.store(block->sequence, std::memory_order_release);
}

static void
early_release_stage(int stage, size_t count, void* ctx) {
    EarlyReleaseWindow* window = static_cast<EarlyReleaseWindow*>(ctx);
    if (stage == window->stage) {
        /* The block's input is released, so the reader may number the next chunk. Hold the demod, before it takes the
           generation it acknowledges, until the reader holds that chunk numbered but uncommitted. */
        const uint64_t sequence = window->block_sequence.load(std::memory_order_acquire);
        window->released_sequence.store(sequence, std::memory_order_release);
        window->early_releases.fetch_add(1, std::memory_order_acq_rel);
        if (!wait_for_u64_at_least(&window->hold.reader_holding_gen, sequence + 1U, 2000U)) {
            window->demod_timeouts.fetch_add(1, std::memory_order_acq_rel);
        }
        return;
    }
    if (stage == RTL_STREAM_TEST_REPLAY_READER_BEFORE_COMMIT && count >= 2U) {
        hold_numbered_chunk(&window->hold, (uint64_t)count);
    }
}

/* Replay @p metadata_path (@p chunks chunks) holding the demod at @p stage, the one early release of chunk
 * @p released_sequence, until the reader holds the next chunk numbered; the demod must acknowledge its own chunk. */
static int
run_early_release_window(const char* label, const char* metadata_path, int stage, uint64_t chunks,
                         uint64_t released_sequence) {
    int rc = 0;
    EarlyReleaseWindow& window = fresh_hook_context<EarlyReleaseWindow>();
    window.stage = stage;
    rtl_stream_test_set_replay_block_hook(early_release_block_hook, &window);
    rtl_stream_test_set_replay_stage_hook(early_release_stage, &window);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 10000U, label, &delivered, &rescued);
    finish_replay(ctx, rescued);
    rtl_stream_test_set_replay_stage_hook(NULL, NULL);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);

    if (window.early_releases.load() != 1 || window.released_sequence.load() != released_sequence) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d early releases, the last for chunk %llu (want 1, for chunk %llu)\n", label,
                    window.early_releases.load(), (unsigned long long)window.released_sequence.load(),
                    (unsigned long long)released_sequence);
        rc |= 1;
    }
    if (window.demod_timeouts.load() != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: the reader never numbered the chunk after the early release\n", label);
        rc |= 1;
    }
    rc |= expect_numbered_chunk_holds(label, window.hold, (int)(chunks - 1U));
    rc |= expect_true(label, delivered > 0U);
    return rc;
}

/* Issue #572: a block that wraps the ring end is copied out of the ring and its input released before the demod
 * processes it, so the reader can number the next chunk while the demod still works on this one. The demod must
 * acknowledge the wrapped chunk's own generation, not the newest the reader numbered. */
static int
test_replay_wrapped_block_acknowledges_its_own_chunk(void) {
    /* The events after the first 4096 bytes put every later chunk 4096 floats off the 64 KiB grid, so chunk 33 straddles
       the end of the 2,097,152-float input ring (stream_open_init_pipeline()), and chunk 34 follows it. */
    const uint64_t chunks = 34U;
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_eventful_replay_fixture(metadata_path, sizeof(metadata_path), 4096U + (chunks - 1U) * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }
    return run_early_release_window("wrapped block", metadata_path, RTL_STREAM_TEST_REPLAY_DEMOD_WRAPPED_RELEASED,
                                    chunks, 33U);
}

/* Issue #572: a block the demod discards (a controller gate closing on it) has its input released before the demod
 * acknowledges it. The demod must acknowledge the discarded chunk's own generation, not the newest the reader
 * numbered. */
static int
test_replay_discarded_block_acknowledges_its_own_chunk(void) {
    const uint64_t chunks = 4U;
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              (size_t)chunks * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }
    rtl_stream_test_replay_discard_chunk(2U);
    rc |= run_early_release_window("discarded block", metadata_path, RTL_STREAM_TEST_REPLAY_DEMOD_DISCARD_RELEASED,
                                   chunks, 2U);
    rtl_stream_test_replay_discard_chunk(0U);
    return rc;
}

namespace {
/* test_replay_multi_chunk_eof_delivers_final_block(). */
struct FinalBlockHold {
    size_t total_floats = 0U;
    std::atomic<int> blocks{0};
    std::atomic<size_t> taken{0U};
    std::atomic<uint64_t> reader_waiting_seq{0U};
    std::atomic<int> final_block{0}; /* the demod took the capture's last floats */
    std::atomic<int> held{0};
    std::atomic<int> reader_reported_drained{0};
    std::atomic<int> reader_decided{0};
    std::atomic<int> decided_while_held{0};
    std::atomic<uint64_t> written{0U};
};
} // namespace

/* Wait up to @p timeout_ms for the decoder to take the end of the replay (output drained). */
static int
wait_for_output_drained(unsigned int timeout_ms) {
    for (unsigned int waited = 0U; waited <= timeout_ms; waited++) {
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (dsd_rtl_stream_test_get_replay_state(&state) == 0 && state.replay_output_drained) {
            return 1;
        }
        dsd_sleep_ms(1U);
    }
    return 0;
}

static void
final_block_hold_stage(int stage, size_t count, void* ctx) {
    FinalBlockHold* hold = static_cast<FinalBlockHold*>(ctx);
    switch (stage) {
        case RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT:
            hold->reader_waiting_seq.store((uint64_t)count, std::memory_order_release);
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_RESERVE: {
            const int block = hold->blocks.fetch_add(1, std::memory_order_acq_rel) + 1;
            const size_t taken = hold->taken.fetch_add(count, std::memory_order_acq_rel) + count;
            /* A reader that runs ahead numbers the whole capture before the demod goes on with its first block. */
            if (block == 1) {
                (void)wait_for_reader_caught_up(&hold->reader_waiting_seq, 2U, 2000U);
            }
            if (taken >= hold->total_floats) {
                hold->final_block.store(1, std::memory_order_release);
            }
            break;
        }
        case RTL_STREAM_TEST_REPLAY_DEMOD_BEFORE_OUTPUT_WRITE:
            /* Hold the final block's output until the reader, which sees the input ring empty now, has decided whether
               the demod drained. A reader that reported it drained lets the decoder end the stream: hold on until it
               has, so the loss shows. */
            if (hold->final_block.load(std::memory_order_acquire) && !hold->held.exchange(1)) {
                hold->decided_while_held.store(wait_for_flag(&hold->reader_decided, 2000U), std::memory_order_release);
                if (hold->reader_reported_drained.load(std::memory_order_acquire)) {
                    (void)wait_for_output_drained(2000U);
                }
            }
            break;
        case RTL_STREAM_TEST_REPLAY_DEMOD_AFTER_OUTPUT_WRITE:
            hold->written.fetch_add((uint64_t)count, std::memory_order_relaxed);
            break;
        case RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECIDED:
            hold->reader_reported_drained.store(count ? 1 : 0, std::memory_order_release);
            hold->reader_decided.store(1, std::memory_order_release);
            break;
        default: break;
    }
}

/* Issue #572: at the end of a multi-chunk fast replay, the reader decides the demod drained once the input ring is
 * empty and every chunk it numbered is acknowledged. A demod that acknowledged a chunk before it took it lets that
 * decision come while the demod still holds the final block, and the decoder then ends the stream with that block
 * unwritten. The demod holds the final block's output until the reader has decided; every sample it wrote must still
 * reach the decoder. */
static int
test_replay_multi_chunk_eof_delivers_final_block(void) {
    const size_t chunks = 4U;
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              chunks * kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    for (int rep = 0; rep < 5; rep++) {
        FinalBlockHold& hold = fresh_hook_context<FinalBlockHold>();
        hold.total_floats = chunks * kCu8ChunkFloats;
        rtl_stream_test_set_replay_stage_hook(final_block_hold_stage, &hold);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
            rtl_stream_test_set_replay_stage_hook(NULL, NULL);
            stop_and_destroy_stream(ctx);
            return 1;
        }
        uint64_t delivered = 0U;
        int rescued = 0;
        rc |= read_replay_to_end(ctx, 5000U, "final block", &delivered, &rescued);

        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        rc |= expect_int_eq("final block replay state", dsd_rtl_stream_test_get_replay_state(&state), 0);
        finish_replay(ctx, rescued);
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);

        const uint64_t written = hold.written.load(std::memory_order_relaxed);
        rc |= expect_true("final block: the demod held the final block", hold.held.load());
        rc |= expect_true("final block: the reader decided while the demod held the final block",
                          hold.decided_while_held.load());
        rc |= expect_true("final block: the demod published output", written > 0U);
        if (delivered != written) {
            DSD_FPRINTF(stderr, "FAIL: final block rep %d: the decoder read %llu of the %llu samples the demod wrote\n",
                        rep, (unsigned long long)delivered, (unsigned long long)written);
            rc |= 1;
        }
        rc |= expect_int_eq("final block: demod drained", state.replay_demod_drained, 1);
        rc |= expect_int_eq("final block: output drained", state.replay_output_drained, 1);
    }
    return rc;
}

namespace {
/* test_loop_replay_media_time_runs_on(): the first blocks of a looping replay, kept on the demod thread. */
struct LoopBlocks {
    static const int kWanted = 3;
    LoggedBlock blocks[kWanted] = {};
    std::atomic<int> count{0};
};
} // namespace

static void
loop_blocks_hook(const rtl_stream_test_replay_block* block, void* ctx) {
    LoopBlocks* loop = static_cast<LoopBlocks*>(ctx);
    const int index = loop->count.load(std::memory_order_relaxed);
    if (index >= LoopBlocks::kWanted) {
        return;
    }
    loop->blocks[index] = LoggedBlock{block->sequence,     block->submit_gen,       block->media_start_ns,
                                      block->media_end_ns, block->have_input_level, block->float_count};
    loop->count.store(index + 1, std::memory_order_release);
}

/* Issue #572: a loop rewinds the capture, not the replay's timeline. Each pass's chunk keeps counting the sequence
 * and the submit generation, and its media time follows the pass before it, so capture time never runs backwards. */
static int
test_loop_replay_media_time_runs_on(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                              kReplayChunkBytes);
    if (rc != 0) {
        return 1;
    }

    LoopBlocks& loop = fresh_hook_context<LoopBlocks>();
    rtl_stream_test_set_replay_block_hook(loop_blocks_hook, &loop);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx) != 0) {
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    float audio[1024];
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + 5000ULL * 1000000ULL;
    while (loop.count.load(std::memory_order_acquire) < LoopBlocks::kWanted && dsd_time_monotonic_ns() < deadline_ns) {
        int got = 0;
        (void)rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);
    }
    stop_and_destroy_stream(ctx);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);

    BlockLog log;
    log.blocks.assign(loop.blocks, loop.blocks + loop.count.load(std::memory_order_acquire));
    std::vector<ExpectedChunk> chunks;
    chunks.reserve(LoopBlocks::kWanted);
    for (int pass = 0; pass < LoopBlocks::kWanted; pass++) {
        chunks.push_back(ExpectedChunk{kCu8ChunkFloats, (uint64_t)pass * (kCu8ChunkFloats / 2U)});
    }
    rc |= expect_blocks_match_chunks("loop media time", log, chunks);
    return rc;
}

/* A cf32 capture whose floats count up from 0 (payload[i] == i), with no fs/4 shift, so the demod gets them as written.
 */
static int
make_indexed_cf32_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, size_t float_count) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_indexed_cf32")) {
        DSD_FPRINTF(stderr, "FAIL: could not create indexed CF32 fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "indexed.iq", "indexed.iq.json");

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "indexed.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "indexed.iq.json") != 0) {
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 0);
    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open indexed CF32 capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }
    std::vector<float> payload(float_count);
    for (size_t i = 0; i < float_count; i++) {
        payload[i] = static_cast<float>(i);
    }
    if (dsd_iq_capture_submit(writer, payload.data(), payload.size() * sizeof(float)) != DSD_IQ_OK) {
        dsd_iq_capture_abort(writer);
        return 1;
    }
    dsd_iq_capture_final_stats stats;
    DSD_MEMSET(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);
    if (DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        return 1;
    }
    return 0;
}

/* Issue #572: a capture source can return fewer bytes than asked. The reader fills each chunk before it converts it,
 * so a cf32 read that stops inside a complex sample is completed, not skipped, and the chunks after it stay on the
 * sample grid: the demod takes every float of the capture, in order, one chunk a block. */
static int
test_cf32_replay_short_reads_stay_aligned(void) {
    const size_t float_count = 3U * kCf32ChunkFloats + kCf32ChunkFloats / 2U;
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_indexed_cf32_replay_fixture(metadata_path, sizeof(metadata_path), float_count);
    if (rc != 0) {
        return 1;
    }

    std::vector<ExpectedChunk> chunks;
    for (size_t start = 0U; start < float_count; start += kCf32ChunkFloats) {
        chunks.push_back(ExpectedChunk{std::min(kCf32ChunkFloats, float_count - start), start / 2U});
    }

    BlockLog& log = fresh_hook_context<BlockLog>();
    log.keep_samples = 1;
    rtl_stream_test_set_replay_block_hook(block_log_hook, &log);
    rtl_device_test_replay_limit_read(4092U); /* each read stops half a complex sample short of 4 KiB */
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        rtl_device_test_replay_limit_read(0U);
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        stop_and_destroy_stream(ctx);
        return 1;
    }
    uint64_t delivered = 0U;
    int rescued = 0;
    rc |= read_replay_to_end(ctx, 5000U, "cf32 short reads", &delivered, &rescued);
    finish_replay(ctx, rescued);
    rtl_device_test_replay_limit_read(0U);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);

    rc |= expect_blocks_match_chunks("cf32 short reads", log, chunks);
    if (log.samples.size() != float_count) {
        DSD_FPRINTF(stderr, "FAIL: cf32 short reads: the demod took %zu of the capture's %zu floats\n",
                    log.samples.size(), float_count);
        rc |= 1;
    }
    const size_t n = std::min(log.samples.size(), float_count);
    for (size_t i = 0; i < n; i++) {
        if (std::fabs(log.samples[i] - static_cast<float>(i)) > 0.25f) {
            DSD_FPRINTF(stderr, "FAIL: cf32 short reads: float %zu the demod took is %.1f, not %zu\n", i,
                        (double)log.samples[i], i);
            rc |= 1;
            break;
        }
    }
    return rc;
}

/* Replay @p metadata_path with every capture read limited to @p read_bytes, and with the read after @p error_after
 * reads that returned data replaced by one that returns @p error_code and no bytes. The demod's blocks go to @p log,
 * the samples the decoder read to @p delivered, and the input failure the replay reported (DSD_INPUT_FAILURE_NONE
 * when it reached its end) to @p failure. */
static int
replay_with_failed_read(const char* label, const char* metadata_path, size_t read_bytes, int error_after,
                        int error_code, BlockLog* log, uint64_t* delivered, dsd_input_failure* failure) {
    dsd_input_failure_clear();
    rtl_stream_test_set_replay_block_hook(block_log_hook, log);
    rtl_device_test_replay_limit_read(read_bytes);
    rtl_device_test_replay_inject_read_error(error_after, error_code);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    int rc = 0;
    *delivered = 0U;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        stop_and_destroy_stream(ctx);
        rc = 1;
    } else {
        int rescued = 0;
        rc |= read_replay_to_end(ctx, 5000U, label, delivered, &rescued);
        finish_replay(ctx, rescued);
    }
    rtl_device_test_replay_inject_read_error(-1, 0);
    rtl_device_test_replay_limit_read(0U);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    dsd_input_failure_get(failure);
    dsd_input_failure_clear();
    return rc;
}

/* The demod took exactly floats 0 .. @p float_count - 1 of an indexed cf32 capture, in order. */
static int
expect_indexed_floats(const char* label, const BlockLog& log, size_t float_count) {
    if (log.samples.size() != float_count) {
        DSD_FPRINTF(stderr, "FAIL: %s: the demod took %zu floats, not %zu\n", label, log.samples.size(), float_count);
        return 1;
    }
    for (size_t i = 0; i < float_count; i++) {
        if (std::fabs(log.samples[i] - static_cast<float>(i)) > 0.25f) {
            DSD_FPRINTF(stderr, "FAIL: %s: float %zu the demod took is %.1f, not %zu\n", label, i,
                        (double)log.samples[i], i);
            return 1;
        }
    }
    return 0;
}

/* Issue #572: a read that fails after part of a chunk was read still hands the demod the part it got, as one block
 * (the replay ends after the samples already read), and the replay then ends with the failure reported. The part is
 * cut to whole complex samples: a cf32 chunk with half a sample at its end could not be converted at all. The same
 * holds when the capture ends inside a sample. */
static int
test_replay_read_failure_mid_chunk_delivers_what_was_read(void) {
    /* Sizes in size_t from the start: the reads, and the whole cf32 samples they leave (2 floats each). */
    const size_t cu8_read_bytes = 4096U;
    const size_t cf32_read_bytes = 4092U;
    const size_t cf32_whole_floats = 2U * static_cast<size_t>(1534U);
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];

    /* cu8, three whole 4096-byte reads, then the failure. */
    if (make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                            2U * kReplayChunkBytes)
        != 0) {
        return 1;
    }
    {
        BlockLog& log = fresh_hook_context<BlockLog>();
        uint64_t delivered = 0U;
        dsd_input_failure failure;
        rc |= replay_with_failed_read("read failure mid-chunk", metadata_path, cu8_read_bytes, 3, DSD_IQ_ERR_IO, &log,
                                      &delivered, &failure);
        std::vector<ExpectedChunk> chunks;
        chunks.push_back(ExpectedChunk{3U * cu8_read_bytes, 0U});
        rc |= expect_blocks_match_chunks("read failure mid-chunk", log, chunks);
        rc |= expect_true("read failure mid-chunk: the part read reached the decoder", delivered > 0U);
        rc |= expect_int_eq("read failure mid-chunk: reported as a file input failure", failure.kind,
                            DSD_INPUT_FAILURE_FILE);
        rc |= expect_int_eq("read failure mid-chunk: code reported", failure.native_code, DSD_IQ_ERR_IO);
    }

    /* cf32, three 4092-byte reads (12276 bytes: 1534 complex samples and half of the next), then the failure. The
     * 1534 whole samples reach the demod, and the failure is still reported. */
    if (make_indexed_cf32_replay_fixture(metadata_path, sizeof(metadata_path), 2U * kCf32ChunkFloats) != 0) {
        return 1;
    }
    {
        BlockLog& log = fresh_hook_context<BlockLog>();
        log.keep_samples = 1;
        uint64_t delivered = 0U;
        dsd_input_failure failure;
        rc |= replay_with_failed_read("cf32 read failure mid-sample", metadata_path, cf32_read_bytes, 3, DSD_IQ_ERR_IO,
                                      &log, &delivered, &failure);
        std::vector<ExpectedChunk> chunks;
        chunks.push_back(ExpectedChunk{cf32_whole_floats, 0U});
        rc |= expect_blocks_match_chunks("cf32 read failure mid-sample", log, chunks);
        rc |= expect_indexed_floats("cf32 read failure mid-sample", log, cf32_whole_floats);
        rc |= expect_int_eq("cf32 read failure mid-sample: reported as a file input failure", failure.kind,
                            DSD_INPUT_FAILURE_FILE);
        rc |= expect_int_eq("cf32 read failure mid-sample: code reported", failure.native_code, DSD_IQ_ERR_IO);
    }

    /* cf32 ending inside a sample. The fourth read returns no bytes, as a source does at its end, after 12276 bytes:
     * 1534 complex samples and half of the 1535th. The capture's last 4 bytes, the other half, follow, and the real
     * end comes after them. The 1534 whole samples reach the demod, the halves are dropped, and the replay ends at
     * its end, with no failure. */
    if (make_indexed_cf32_replay_fixture(metadata_path, sizeof(metadata_path), cf32_whole_floats + 2U) != 0) {
        return 1;
    }
    {
        BlockLog& log = fresh_hook_context<BlockLog>();
        log.keep_samples = 1;
        uint64_t delivered = 0U;
        dsd_input_failure failure;
        rc |= replay_with_failed_read("cf32 end mid-sample", metadata_path, cf32_read_bytes, 3, DSD_IQ_OK, &log,
                                      &delivered, &failure);
        std::vector<ExpectedChunk> chunks;
        chunks.push_back(ExpectedChunk{cf32_whole_floats, 0U});
        rc |= expect_blocks_match_chunks("cf32 end mid-sample", log, chunks);
        rc |= expect_indexed_floats("cf32 end mid-sample", log, cf32_whole_floats);
        rc |= expect_int_eq("cf32 end mid-sample: the replay reached its end", failure.kind, DSD_INPUT_FAILURE_NONE);
    }
    return rc;
}

/* A sidecar written by hand, for a sample format or capture stage the capture writer does not produce, over
 * @p payload_bytes of data. */
static int
make_hand_written_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, const char* sample_format,
                                 const char* endianness, const char* capture_stage, size_t payload_bytes) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_format")) {
        DSD_FPRINTF(stderr, "FAIL: could not create a fixture directory\n");
        return 1;
    }
    track_fixture_dir(temp_dir, "format.iq", "format.iq.json");
    char data_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "format.iq") != 0
        || dsd_test_path_join(out_metadata_path, out_metadata_path_size, temp_dir, "format.iq.json") != 0) {
        return 1;
    }
    std::vector<uint8_t> payload(payload_bytes);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>((i * 7U) & 0xFFU);
    }
    if (write_bytes_file(data_path, payload.data(), payload.size()) != 0) {
        return 1;
    }
    char json[2048];
    DSD_SNPRINTF(json, sizeof(json),
                 "{\n"
                 "  \"format\": \"dsd-neo-iq\",\n"
                 "  \"version\": 1,\n"
                 "  \"sample_format\": \"%s\",\n"
                 "  \"iq_order\": \"IQ\",\n"
                 "  \"endianness\": \"%s\",\n"
                 "  \"capture_stage\": \"%s\",\n"
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
                 "  \"fs4_shift_enabled\": false,\n"
                 "  \"combine_rotate_enabled\": true,\n"
                 "  \"muted_bytes_excluded\": true,\n"
                 "  \"contains_retunes\": false,\n"
                 "  \"capture_retune_count\": 0,\n"
                 "  \"source_backend\": \"soapy\",\n"
                 "  \"source_args\": \"driver=x\",\n"
                 "  \"capture_started_utc\": \"2026-04-12T00:00:00Z\",\n"
                 "  \"data_file\": \"format.iq\",\n"
                 "  \"data_bytes\": %zu,\n"
                 "  \"capture_drops\": 0,\n"
                 "  \"capture_drop_blocks\": 0,\n"
                 "  \"input_ring_drops\": 0,\n"
                 "  \"notes\": \"\"\n"
                 "}\n",
                 sample_format, endianness, capture_stage, payload_bytes);
    return write_text_file(out_metadata_path, json);
}

/* Start a replay of @p metadata_path, with --iq-loop when @p loop, without reporting a refused start: 0 when it
 * started, 1 when it was refused. */
static int
start_replay_stream_quietly(const char* metadata_path, int loop, std::unique_ptr<dsd_opts>* out_opts,
                            RtlSdrContext** out_ctx) {
    *out_ctx = NULL;
    out_opts->reset(new dsd_opts());
    prepare_replay_opts_with_loop(out_opts->get(), metadata_path, loop);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(out_opts->get(), &ctx) != 0 || !ctx) {
        return 1;
    }
    if (rtl_stream_start(ctx) != 0) {
        rtl_stream_destroy(ctx);
        return 1;
    }
    *out_ctx = ctx;
    return 0;
}

/* Issue #572: replay converts cu8 and cf32 captured as the driver delivered it, before the input ring, and nothing
 * else. A cs16 sidecar, or a cf32 capture stamped with the cu8 stage (which the capture writer accepts), is refused at
 * the start, as an unreadable sidecar is, and leaves nothing behind. Such a capture used to start, and the reader then
 * dropped every chunk it could not convert: the replay ended with nothing delivered and no failure. */
static int
test_replay_refuses_a_capture_it_cannot_convert(void) {
    int rc = 0;
    for (int cf32 = 0; cf32 <= 1; cf32++) {
        const char* label = cf32 ? "cf32 at the cu8 stage" : "cs16";
        char metadata_path[DSD_TEST_PATH_MAX];
        const int made = cf32 ? make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CF32,
                                                    "post_mute_pre_widen", 0, 2U * kReplayChunkBytes)
                              : make_hand_written_replay_fixture(metadata_path, sizeof(metadata_path), "cs16", "little",
                                                                 "post_mute_pre_widen", 3U * kReplayChunkBytes);
        if (made != 0) {
            return 1;
        }
        dsd_input_failure_clear();
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        char what[160];
        const int refused = start_replay_stream_quietly(metadata_path, 0, &opts, &ctx) != 0;
        if (!refused) {
            uint64_t delivered = 0U;
            int rescued = 0;
            rc |= read_replay_to_end(ctx, 5000U, label, &delivered, &rescued);
            finish_replay(ctx, rescued);
            dsd_input_failure failure;
            dsd_input_failure_get(&failure);
            DSD_FPRINTF(stderr, "  %s: the replay started, delivered %llu samples, input failure kind %d\n", label,
                        (unsigned long long)delivered, (int)failure.kind);
        }
        DSD_SNPRINTF(what, sizeof(what), "%s: the replay start is refused", label);
        rc |= expect_true(what, refused);
        DSD_SNPRINTF(what, sizeof(what), "%s: no stream resources left", label);
        rc |= expect_true(what, rtl_stream_test_has_resources() == 0);
        dsd_input_failure_clear();
    }
    return rc;
}

/* Issue #572: a chunk the reader cannot convert ends the replay as a failed read does, after the chunks already
 * delivered, with the failure reported (dsd_engine_run_with_lifecycle() then returns 1). The open refuses every
 * capture the converter cannot handle, so the conversion failure is injected: the third of four chunks. It used to be
 * dropped as an empty chunk, and the replay went on and ended with no failure. */
static int
test_replay_conversion_failure_ends_with_the_failure(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                            4U * kReplayChunkBytes)
        != 0) {
        return 1;
    }
    dsd_input_failure_clear();
    BlockLog& log = fresh_hook_context<BlockLog>();
    rtl_stream_test_set_replay_block_hook(block_log_hook, &log);
    rtl_device_test_replay_override_conversion(2, -1);
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    uint64_t delivered = 0U;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        stop_and_destroy_stream(ctx);
        rc = 1;
    } else {
        int rescued = 0;
        rc |= read_replay_to_end(ctx, 5000U, "conversion failure", &delivered, &rescued);
        finish_replay(ctx, rescued);
    }
    rtl_device_test_replay_override_conversion(-1, 0);
    rtl_stream_test_set_replay_block_hook(NULL, NULL);
    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    dsd_input_failure_clear();

    std::vector<ExpectedChunk> chunks;
    chunks.push_back(ExpectedChunk{kCu8ChunkFloats, 0U});
    chunks.push_back(ExpectedChunk{kCu8ChunkFloats, kCu8ChunkFloats / 2U});
    rc |= expect_blocks_match_chunks("conversion failure", log, chunks);
    rc |= expect_true("conversion failure: the chunks before it reached the decoder", delivered > 0U);
    rc |= expect_int_eq("conversion failure: reported as a file input failure", failure.kind, DSD_INPUT_FAILURE_FILE);
    rc |= expect_int_eq("conversion failure: code reported", failure.native_code, DSD_IQ_ERR_UNSUPPORTED_FMT);
    return rc;
}

namespace {
/* test_replay_of_a_data_file_cut_short(): the data file a replay reads, cut to its first keep_bytes once the reader
 * has read chunk at_chunk. */
struct FileCut {
    std::string data_path;
    size_t keep_bytes = 0U;
    size_t at_chunk = 0U;
    std::atomic<int> cut{0};
    std::atomic<int> cut_ok{0};
};
} // namespace

/* Write @p path again with only its first @p keep_bytes, as a file cut short under a reader that holds it open. */
static int
cut_file_to(const char* path, size_t keep_bytes) {
    std::vector<uint8_t> kept(keep_bytes);
    FILE* fp = dsd_fopen_private(path, "rb");
    if (!fp) {
        return -1;
    }
    const size_t got = std::fread(kept.data(), 1, keep_bytes, fp);
    std::fclose(fp);
    if (got != keep_bytes) {
        return -1;
    }
    return write_bytes_file(path, kept.data(), kept.size());
}

static void
file_cut_stage(int stage, size_t count, void* ctx) {
    FileCut* cut = static_cast<FileCut*>(ctx);
    /* The reader has read chunk at_chunk and waits for the demod to take the one before it. */
    if (stage == RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT && count == cut->at_chunk
        && cut->cut.exchange(1, std::memory_order_acq_rel) == 0) {
        cut->cut_ok.store(cut_file_to(cut->data_path.c_str(), cut->keep_bytes) == 0, std::memory_order_release);
    }
}

/* Issue #572: a data file cut short while it replays ends the replay as a failed read does: the reader hands over
 * every whole sample still in the file, then the read that finds the file's end before the capture's is a failure,
 * reported as such (dsd_engine_run_with_lifecycle() then returns 1). It used to end as the capture's end, with no
 * failure. The cut lands on the reader's thread once it has read chunk 2 of 4: at the end of chunk 2 (cu8), and inside
 * a complex sample 4092 bytes into chunk 3 (cf32), whose 511 whole samples reach the demod. */
static int
test_replay_of_a_data_file_cut_short(void) {
    int rc = 0;
    for (int cf32 = 0; cf32 <= 1; cf32++) {
        const char* label = cf32 ? "cf32 data file cut inside a sample" : "cu8 data file cut at a chunk's end";
        char metadata_path[DSD_TEST_PATH_MAX];
        const int made =
            cf32 ? make_indexed_cf32_replay_fixture(metadata_path, sizeof(metadata_path), 4U * kCf32ChunkFloats)
                 : make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen",
                                       1, 4U * kReplayChunkBytes);
        if (made != 0) {
            return 1;
        }
        FileCut& cut = fresh_hook_context<FileCut>();
        cut.data_path = metadata_path;
        cut.data_path.resize(cut.data_path.size() - std::strlen(".json"));
        cut.at_chunk = 2U;
        cut.keep_bytes = 2U * kReplayChunkBytes + (cf32 ? 4092U : 0U);
        std::vector<ExpectedChunk> chunks;
        if (cf32) {
            chunks.push_back(ExpectedChunk{kCf32ChunkFloats, 0U});
            chunks.push_back(ExpectedChunk{kCf32ChunkFloats, kCf32ChunkFloats / 2U});
            chunks.push_back(ExpectedChunk{2U * static_cast<size_t>(511U), kCf32ChunkFloats});
        } else {
            chunks.push_back(ExpectedChunk{kCu8ChunkFloats, 0U});
            chunks.push_back(ExpectedChunk{kCu8ChunkFloats, kCu8ChunkFloats / 2U});
        }

        dsd_input_failure_clear();
        BlockLog& log = fresh_hook_context<BlockLog>();
        log.keep_samples = cf32;
        rtl_stream_test_set_replay_block_hook(block_log_hook, &log);
        rtl_stream_test_set_replay_stage_hook(file_cut_stage, &cut);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        uint64_t delivered = 0U;
        if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
            stop_and_destroy_stream(ctx);
            rc = 1;
        } else {
            int rescued = 0;
            rc |= read_replay_to_end(ctx, 5000U, label, &delivered, &rescued);
            finish_replay(ctx, rescued);
        }
        rtl_stream_test_set_replay_stage_hook(NULL, NULL);
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        dsd_input_failure failure;
        dsd_input_failure_get(&failure);
        dsd_input_failure_clear();

        char what[160];
        DSD_SNPRINTF(what, sizeof(what), "%s: the file was cut under the reader", label);
        rc |= expect_true(what, cut.cut.load() && cut.cut_ok.load());
        rc |= expect_blocks_match_chunks(label, log, chunks);
        if (cf32) {
            rc |= expect_indexed_floats(label, log, 2U * kCf32ChunkFloats + 2U * static_cast<size_t>(511U));
        } else {
            DSD_SNPRINTF(what, sizeof(what), "%s: what was read reached the decoder", label);
            rc |= expect_true(what, delivered > 0U);
        }
        DSD_SNPRINTF(what, sizeof(what), "%s: reported as a file input failure", label);
        rc |= expect_int_eq(what, failure.kind, DSD_INPUT_FAILURE_FILE);
        DSD_SNPRINTF(what, sizeof(what), "%s: code reported", label);
        rc |= expect_int_eq(what, failure.native_code, DSD_IQ_ERR_IO);
    }
    return rc;
}

/* Issue #572: an --iq-loop pass that hands the demod no chunk is not rewound: the replay ends there, as at the end of a
 * replay without --iq-loop, instead of rewinding in a tight loop through the reconfigure gate and a full finalize on
 * every pass with nothing ever reaching the decoder. Every chunk is made to convert to no sample (the shape a capture
 * the open now refuses had), from the first pass, and from the second after a first pass that delivered its chunk. */
static int
test_loop_pass_that_submits_nothing_ends(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_replay_fixture(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen", 1,
                            kReplayChunkBytes)
        != 0) {
        return 1;
    }
    for (int passes_delivered = 0; passes_delivered <= 1; passes_delivered++) {
        const char* label = passes_delivered ? "loop, second pass submits nothing" : "loop, first pass submits nothing";
        char what[160];
        dsd_input_failure_clear();
        BlockLog& log = fresh_hook_context<BlockLog>();
        rtl_stream_test_set_replay_block_hook(block_log_hook, &log);
        rtl_device_test_replay_override_conversion(passes_delivered, 0);
        std::unique_ptr<dsd_opts> opts;
        RtlSdrContext* ctx = NULL;
        uint64_t delivered = 0U;
        rtl_stream_test_replay_state state;
        DSD_MEMSET(&state, 0, sizeof(state));
        if (start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx) != 0) {
            stop_and_destroy_stream(ctx);
            rc = 1;
        } else {
            int rescued = 0;
            rc |= read_replay_to_end(ctx, 3000U, label, &delivered, &rescued);
            (void)dsd_rtl_stream_test_get_replay_state(&state);
            finish_replay(ctx, rescued);
        }
        rtl_device_test_replay_override_conversion(-1, 0);
        rtl_stream_test_set_replay_block_hook(NULL, NULL);
        dsd_input_failure failure;
        dsd_input_failure_get(&failure);
        dsd_input_failure_clear();

        DSD_SNPRINTF(what, sizeof(what), "%s: loop rewinds", label);
        rc |= expect_int_eq(what, (int)state.replay_loop_restart_count, passes_delivered);
        DSD_SNPRINTF(what, sizeof(what), "%s: blocks the demod took", label);
        rc |= expect_int_eq(what, (int)log.blocks.size(), passes_delivered);
        DSD_SNPRINTF(what, sizeof(what), "%s: the replay reached its end", label);
        rc |= expect_true(what, state.replay_input_eof && state.replay_output_drained);
        DSD_SNPRINTF(what, sizeof(what), "%s: ended as at the capture's end, with no failure", label);
        rc |= expect_int_eq(what, failure.kind, DSD_INPUT_FAILURE_NONE);
        if (passes_delivered) {
            DSD_SNPRINTF(what, sizeof(what), "%s: the first pass reached the decoder", label);
            rc |= expect_true(what, delivered > 0U);
        }
    }
    return rc;
}

/* An I/Q replay has no tuner gain to adjust, so the supervisory tuner autogain stays off under it even when the runtime
 * config enables it (DSD_NEO_TUNER_AUTOGAIN) and the capture records no gain, which is where a live open turns it on:
 * its hold and throttle windows run on real time, and its adjustments would only log on a wall-clock cadence. A flag
 * raised during the replay (a menu toggle, a retune profile) supervises nothing. Runs last: it leaves the runtime
 * config initialized. */
static int
test_replay_keeps_tuner_autogain_off(void) {
    int rc = 0;
    char metadata_path[DSD_TEST_PATH_MAX];
    rc |= make_replay_fixture_with_gain(metadata_path, sizeof(metadata_path), DSD_IQ_FORMAT_CU8, "post_mute_pre_widen",
                                        1, kReplayChunkBytes * 8U, 0);
    if (rc != 0) {
        return 1;
    }
    (void)dsd_setenv("DSD_NEO_TUNER_AUTOGAIN", "1", 1);
    dsd_neo_config_init();

    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = NULL;
    if (start_replay_stream(metadata_path, &opts, &ctx) != 0) {
        stop_and_destroy_stream(ctx);
        (void)dsd_unsetenv("DSD_NEO_TUNER_AUTOGAIN");
        dsd_neo_config_init();
        return 1;
    }
    rc |= expect_int_eq("tuner autogain off at a replay open", rtl_stream_get_tuner_autogain(), 0);
    const uint64_t supervised_before = rtl_stream_test_autogain_supervised_blocks();
    rtl_stream_set_tuner_autogain(1);
    uint64_t drained_samples = 0;
    rc |= drain_stream_to_eof(ctx, 5000U, &drained_samples);
    rc |= expect_true("replay produced output", drained_samples > 0U);
    rc |= expect_true("tuner autogain supervised no replay block",
                      rtl_stream_test_autogain_supervised_blocks() == supervised_before);
    rtl_stream_set_tuner_autogain(0);
    stop_and_destroy_stream(ctx);
    (void)dsd_unsetenv("DSD_NEO_TUNER_AUTOGAIN");
    dsd_neo_config_init();
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_replay_eof_drain_delivers_final_block();
    rc |= test_replay_read_survives_clear_at_eof();
    rc |= test_replay_read_error_ends_stream();
    rc |= test_replay_reader_start_failure_unwinds();
    rc |= test_replay_eof_wait_does_not_spin();
    rc |= test_replay_blocks_follow_capture_chunks(0);
    rc |= test_replay_blocks_follow_capture_chunks(1);
    rc |= test_loop_replay_media_time_runs_on();
    rc |= test_replay_demod_never_acknowledges_an_unread_chunk();
    rc |= test_replay_wrapped_block_acknowledges_its_own_chunk();
    rc |= test_replay_discarded_block_acknowledges_its_own_chunk();
    rc |= test_replay_multi_chunk_eof_delivers_final_block();
    rc |= test_cf32_replay_short_reads_stay_aligned();
    rc |= test_replay_read_failure_mid_chunk_delivers_what_was_read();
    rc |= test_replay_refuses_a_capture_it_cannot_convert();
    rc |= test_replay_conversion_failure_ends_with_the_failure();
    rc |= test_loop_pass_that_submits_nothing_ends();
    rc |= test_replay_of_a_data_file_cut_short();
    rc |= test_cu8_replay_publishes_raw_input_level();
    rc |= test_cu8_replay_legacy_fs4_level_uses_raw_block();
    rc |= test_cf32_replay_fs4_level_uses_raw_block();
    rc |= test_replay_eof_partial_block_drains_before_exit();
    rc |= test_replay_output_tail_available_after_demod_drain();
    rc |= test_replay_last_read_leaves_stream_open();
    rc |= test_eventful_replay_applies_scheduled_events();
    rc |= test_eventful_replay_preserves_ppm_reset_reason();
    rc |= test_loop_replay_reapplies_event_timeline();
    rc |= test_realtime_loop_waits_for_terminal_mute_before_rewind();
    rc |= test_cf32_replay_fs4_policy_changes_output();
    rc |= test_cf32_unknown_capture_stage_rejected();
    rc |= test_replay_keeps_tuner_autogain_off();
    rc |= remove_fixture_dirs();
    return rc ? 1 : 0;
}
