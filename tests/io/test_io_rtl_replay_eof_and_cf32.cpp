// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#error "DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS must be enabled for this test."
#endif

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/io/iq_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "test_support.h"

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
write_bytes_file(const char* path, const uint8_t* bytes, size_t len) {
    FILE* fp = std::fopen(path, "wb");
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
    FILE* fp = std::fopen(path, "wb");
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

static void
fill_capture_cfg(dsd_iq_capture_config* cfg, const char* data_path, const char* metadata_path,
                 dsd_iq_sample_format format, const char* capture_stage, int fs4_shift_enabled) {
    std::memset(cfg, 0, sizeof(*cfg));
    std::snprintf(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    std::snprintf(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = format;
    std::snprintf(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", capture_stage);
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
    std::snprintf(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    std::snprintf(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
}

static void
prepare_replay_opts(dsd_opts* opts, const char* metadata_path) {
    std::memset(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = 0;
    std::snprintf(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    std::snprintf(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
}

static int
make_replay_fixture(char* out_metadata_path, size_t out_metadata_path_size, dsd_iq_sample_format format,
                    const char* capture_stage, int fs4_shift_enabled, size_t payload_bytes) {
    if (!out_metadata_path || out_metadata_path_size == 0U || !capture_stage) {
        return 1;
    }

    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), "dsdneo_replay_eof")) {
        std::fprintf(stderr, "FAIL: could not create temporary fixture directory\n");
        return 1;
    }

    char data_path[DSD_TEST_PATH_MAX];
    char metadata_path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(data_path, sizeof(data_path), temp_dir, "fixture.iq") != 0
        || dsd_test_path_join(metadata_path, sizeof(metadata_path), temp_dir, "fixture.iq.json") != 0) {
        std::fprintf(stderr, "FAIL: could not construct fixture paths\n");
        return 1;
    }

    dsd_iq_capture_config cfg;
    fill_capture_cfg(&cfg, data_path, metadata_path, format, capture_stage, fs4_shift_enabled);

    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        std::fprintf(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
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
        std::fprintf(stderr, "FAIL: could not submit fixture payload\n");
        dsd_iq_capture_abort(writer);
        return 1;
    }

    dsd_iq_capture_final_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    dsd_iq_capture_close(writer, &stats);

    if (std::snprintf(out_metadata_path, out_metadata_path_size, "%s", metadata_path) >= (int)out_metadata_path_size) {
        std::fprintf(stderr, "FAIL: metadata path overflow\n");
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
    if (rtl_stream_create_mirrored(out_opts->get(), &ctx) != 0 || !ctx) {
        std::fprintf(stderr, "FAIL: rtl_stream_create_mirrored failed\n");
        return 1;
    }
    if (rtl_stream_start(ctx) != 0) {
        std::fprintf(stderr, "FAIL: rtl_stream_start failed\n");
        rtl_stream_destroy(ctx);
        return 1;
    }
    *out_ctx = ctx;
    return 0;
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
            std::fprintf(stderr, "FAIL: timed out draining replay stream\n");
            return 1;
        }
    }
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

    uint64_t total_samples = 0;
    float audio[1024];
    uint64_t start_ns = dsd_time_monotonic_ns();
    int saw_eof = 0;
    while (dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL) {
        rtl_stream_test_replay_state state;
        std::memset(&state, 0, sizeof(state));
        rc |= expect_int_eq("replay state snapshot", rtl_stream_test_get_replay_state(ctx, &state), 0);
        if (state.should_exit) {
            rc |= expect_true("should_exit implies input ring drained", state.input_ring_used == 0U);
        }

        int got = 0;
        int read_rc = rtl_stream_read(ctx, audio, sizeof(audio) / sizeof(audio[0]), &got);
        if (read_rc == 0) {
            if (got > 0) {
                total_samples += (uint64_t)got;
            }
            continue;
        }
        saw_eof = 1;
        break;
    }

    rc |= expect_true("replay reached EOF", saw_eof);

    rtl_stream_test_replay_state final_state;
    std::memset(&final_state, 0, sizeof(final_state));
    rc |= expect_int_eq("final replay state snapshot", rtl_stream_test_get_replay_state(ctx, &final_state), 0);
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
        std::memset(&state, 0, sizeof(state));
        if (rtl_stream_test_get_replay_state(ctx, &state) != 0) {
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
        std::fprintf(stderr, "FAIL: could not create temporary metadata directory\n");
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
    std::memset(&cfg, 0, sizeof(cfg));
    char err[256] = {0};
    int prc = dsd_iq_replay_read_metadata(meta_path, &cfg, err, sizeof(err));
    rc |= expect_int_eq("unknown cf32 capture_stage rejected", prc, DSD_IQ_ERR_UNSUPPORTED_FMT);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_replay_eof_partial_block_drains_before_exit();
    rc |= test_replay_output_tail_available_after_demod_drain();
    rc |= test_cf32_replay_fs4_policy_changes_output();
    rc |= test_cf32_unknown_capture_stage_rejected();
    return rc ? 1 : 0;
}
