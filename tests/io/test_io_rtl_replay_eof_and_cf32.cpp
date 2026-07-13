// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <memory>
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

static int
make_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, dsd_iq_sample_format format,
                    const char* capture_stage, int fs4_shift_enabled, size_t payload_bytes) {
    if (!out_metadata_path || out_metadata_path_size == 0U || !capture_stage) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_eof")) {
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
    fill_capture_cfg(&cfg, data_path, metadata_path, format, capture_stage, fs4_shift_enabled);

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

    uint8_t payload[4096];
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
    rc |= start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx);
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

    stop_and_destroy_stream(ctx);
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
    rc |= start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx);
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

    stop_and_destroy_stream(ctx);
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
    rc |= start_replay_stream_with_loop(metadata_path, 1, &opts, &ctx);
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

    stop_and_destroy_stream(ctx);
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
        dsd_sleep_ms(2U);
    }

    rc |= expect_true("demod-drained with buffered output seen", saw_tail_window);

    float audio[512];
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

    stop_and_destroy_stream(ctx);
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

int
main(void) {
    int rc = 0;
    rc |= test_cu8_replay_publishes_raw_input_level();
    rc |= test_cu8_replay_legacy_fs4_level_uses_raw_block();
    rc |= test_cf32_replay_fs4_level_uses_raw_block();
    rc |= test_replay_eof_partial_block_drains_before_exit();
    rc |= test_replay_output_tail_available_after_demod_drain();
    rc |= test_eventful_replay_applies_scheduled_events();
    rc |= test_eventful_replay_preserves_ppm_reset_reason();
    rc |= test_loop_replay_reapplies_event_timeline();
    rc |= test_realtime_loop_waits_for_terminal_mute_before_rewind();
    rc |= test_cf32_replay_fs4_policy_changes_output();
    rc |= test_cf32_unknown_capture_stage_rejected();
    return rc ? 1 : 0;
}
