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
 *
 * A replay describes the geometry it applied, not the live receiver's: a recording made without the Fs/4 shift is not
 * rotated, so the tap reports no rotation; a shifted one is rotated by a quarter of its capture rate before any
 * resampling, so the tap reports that shift in Hz at the rate it is fed, the resampled one included. A tone in the
 * recording is then measured where it is: a quiet request through the tap (the channel and its sidebands, as Signal
 * search measures them, and the capture-wide reference the floor guard folds with the same rotation) finds the tone's
 * channel busy and an empty channel quiet, with the floor inside the guard.
 */

#include <cerrno>
#include <cmath>
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
static constexpr uint64_t kCenterHz = 851375000ULL;
/* A recording at this rate, replayed for an explicit 48 kHz DSP bandwidth, is resampled to kRateHz. */
static constexpr uint32_t kResampledCaptureHz = 1600000U;
/* The tone's hardware offset in a centred recording, and the empty channel's on the other side of the centre. */
static constexpr int32_t kToneOffsetHz = 100000;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* A recording: its capture rate, whether it carries the Fs/4 shift, and its cu8 payload. */
namespace {
struct RecordingSpec {
    uint32_t capture_rate_hz;
    int fs4_shift;
    std::vector<uint8_t> payload;
};
} // namespace

/* The hardware centre a recording of @p spec was captured at: the channel centre, Fs/4 above it when shifted. */
static uint64_t
recording_capture_hz(const RecordingSpec& spec) {
    return kCenterHz + (spec.fs4_shift ? spec.capture_rate_hz / 4U : 0U);
}

static int
write_recording(const RecordingSpec& spec, char* out_metadata_path, size_t out_size) {
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
    cfg.sample_rate_hz = spec.capture_rate_hz;
    cfg.center_frequency_hz = kCenterHz;
    cfg.capture_center_frequency_hz = recording_capture_hz(spec);
    cfg.tuner_gain_tenth_db = 270;
    cfg.rtl_dsp_bw_khz = 48;
    cfg.base_decimation = 32U;
    cfg.post_downsample = 1U;
    cfg.demod_rate_hz = spec.capture_rate_hz / 32U;
    cfg.fs4_shift_enabled = spec.fs4_shift;
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
    if (dsd_iq_capture_submit(writer, spec.payload.data(), spec.payload.size()) != DSD_IQ_OK) {
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

static int
make_replay_fixture(char* out_metadata_path, size_t out_size) {
    RecordingSpec spec;
    spec.capture_rate_hz = kRateHz;
    spec.fs4_shift = 1;
    /* Mid-scale cu8 is silence: nothing here depends on the samples. */
    spec.payload.assign(65536U, 127U);
    return write_recording(spec, out_metadata_path, out_size);
}

/* @p samples cu8 pairs of white noise (about 47 dB under full scale per 12.5 kHz at 1.536 Msps) with a tone at
 * @p tone_hz in the recorded samples (before any rotation), well inside the rails. */
static std::vector<uint8_t>
tone_payload(size_t samples, uint32_t rate_hz, double tone_hz) {
    std::vector<uint8_t> payload(samples * 2U);
    uint32_t lcg = 0x2468ACEU;
    for (size_t i = 0; i < samples; i++) {
        const double phase = 2.0 * M_PI * tone_hz * (double)i / (double)rate_hz;
        lcg = lcg * 1664525U + 1013904223U;
        const int ni = (int)((lcg >> 24) & 0xFU) - 8;
        lcg = lcg * 1664525U + 1013904223U;
        const int nq = (int)((lcg >> 24) & 0xFU) - 8;
        payload[2U * i] = (uint8_t)(lrint(127.5 + 50.0 * std::cos(phase)) + ni);
        payload[2U * i + 1U] = (uint8_t)(lrint(127.5 + 50.0 * std::sin(phase)) + nq);
    }
    return payload;
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

/* Read one block of @p ctx's output, waiting a moment when none is ready, so the demod feeds the tap more. */
static void
read_some(RtlSdrContext* ctx) {
    float block[2048];
    int got = 0;
    if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0) {
        dsd_sleep_ms(1);
    }
}

/* Read @p ctx until a block has reached the tap, with its capture in @p capture. */
static int
wait_for_tap_capture(RtlSdrContext* ctx, const char* label, dsd_signal_capture* capture) {
    const uint64_t start_ns = dsd_time_monotonic_ns();
    while (!rtl_signal_capture_get(capture)) {
        if (dsd_time_monotonic_ns() - start_ns > 5000ULL * 1000000ULL) {
            DSD_FPRINTF(stderr, "FAIL: %s: no block reached the tap\n", label);
            return 1;
        }
        read_some(ctx);
    }
    return 0;
}

/* A 12.5 kHz quiet request at @p frequency_hz through the tap of the running replay @p ctx, read until it finished,
 * into @p out. The tap's capture as the replay settles may still move once: a request that fails on that is made again
 * on the capture after it. */
static int
measure_quiet(RtlSdrContext* ctx, const char* label, uint32_t frequency_hz, dsd_signal_quiet_result* out) {
    static uint64_t next_request_id = 1U;
    for (int attempt = 0; attempt < 3; attempt++) {
        dsd_signal_quiet_request request;
        DSD_MEMSET(&request, 0, sizeof(request));
        request.request_id = next_request_id++;
        request.frequency_hz = frequency_hz;
        request.classes = DSD_SIGNAL_QUIET_CLASS_12K5;
        uint32_t ticket = 0U;
        dsd_signal_request_failure why = DSD_SIGNAL_REQUEST_OK;
        if (!rtl_signal_capture_get(&request.capture) || rtl_signal_quiet_request(&request, &ticket, &why) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: the quiet request at %u Hz was refused (%d)\n", label,
                        (unsigned)frequency_hz, (int)why);
            return 1;
        }
        const uint64_t start_ns = dsd_time_monotonic_ns();
        while (rtl_signal_quiet_status(ticket) == 0) {
            if (dsd_time_monotonic_ns() - start_ns > 5000ULL * 1000000ULL) {
                DSD_FPRINTF(stderr, "FAIL: %s: the quiet request at %u Hz never finished\n", label,
                            (unsigned)frequency_hz);
                rtl_signal_quiet_cancel(ticket);
                return 1;
            }
            read_some(ctx);
        }
        DSD_MEMSET(out, 0, sizeof(*out));
        const int rc = rtl_signal_quiet_take(ticket, out);
        if (rc == 1) {
            return 0;
        }
        if (rc != -1 || out->failure != DSD_SIGNAL_MEASURE_CAPTURE_CHANGED) {
            DSD_FPRINTF(stderr, "FAIL: %s: the quiet request at %u Hz failed (%d, reason %d)\n", label,
                        (unsigned)frequency_hz, rc, (int)out->failure);
            return 1;
        }
    }
    DSD_FPRINTF(stderr, "FAIL: %s: the tap's capture kept changing\n", label);
    return 1;
}

/* One recording the geometry test replays: its capture rate, whether it carries the Fs/4 shift, and whether the replay
 * asks for the 48 kHz DSP bandwidth explicitly, which plans a resampled chain rather than the recorded one. */
namespace {
struct GeometryCase {
    const char* label;
    uint32_t capture_rate_hz;
    int fs4_shift;
    int explicit_bandwidth;
};
} // namespace

/* The tap's @p capture describes what the replay of @p c feeds it: the rate the demodulator takes, the recorded
 * hardware centre, and the rotation the replay applied (none for a centred recording, a quarter of the capture rate
 * in Hz for a shifted one, resampled or not). */
static int
expect_replay_geometry(const GeometryCase& c, const RecordingSpec& spec, const dsd_signal_capture& capture) {
    const int32_t want_rotation_hz = c.fs4_shift ? (int32_t)(c.capture_rate_hz / 4U) : 0;
    if (capture.rate_hz == kRateHz && capture.capture_hz == (uint32_t)recording_capture_hz(spec)
        && capture.rotation_hz == want_rotation_hz) {
        return 0;
    }
    DSD_FPRINTF(
        stderr,
        "FAIL: %s: the tap reports %u Hz at %u Hz rotated %d Hz; the replay feeds %u Hz at %u Hz rotated %d Hz\n",
        c.label, (unsigned)capture.rate_hz, (unsigned)capture.capture_hz, (int)capture.rotation_hz, (unsigned)kRateHz,
        (unsigned)recording_capture_hz(spec), (int)want_rotation_hz);
    return 1;
}

/* A quiet result at @p frequency_hz that is valid (the floor inside the guard) and quiet as @p want_quiet says; a busy
 * one also stands well above its floor. */
static int
expect_quiet_result(const char* label, const char* what, uint32_t frequency_hz, const dsd_signal_quiet_result& r,
                    int want_quiet) {
    const int ok = want_quiet ? (r.valid && r.quiet) : (r.valid && !r.quiet && r.max_ratio_db > 20.0f);
    if (ok) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL: %s: the %s at %u Hz measured valid %u quiet %u, (S+N)/N %.1f dB, floor rise %.1f dB\n",
                label, what, (unsigned)frequency_hz, (unsigned)r.valid, (unsigned)r.quiet, (double)r.max_ratio_db,
                (double)r.floor_rise_db);
    return 1;
}

/* Through the tap of the running replay of @p c: the tone's channel is busy and the empty channel quiet. A centred
 * recording's tone stands kToneOffsetHz above the centre and the empty channel as far below it; a shifted recording's
 * tone is on the landed channel, which the shift puts a quarter of the capture rate below the hardware centre, and the
 * empty channel kToneOffsetHz above it. */
static int
expect_tone_bins(RtlSdrContext* ctx, const GeometryCase& c) {
    const uint32_t busy_hz = (uint32_t)(c.fs4_shift ? kCenterHz : kCenterHz + kToneOffsetHz);
    const uint32_t empty_hz = (uint32_t)(c.fs4_shift ? kCenterHz + kToneOffsetHz : kCenterHz - kToneOffsetHz);
    dsd_signal_quiet_result busy;
    dsd_signal_quiet_result empty;
    if (measure_quiet(ctx, c.label, busy_hz, &busy) != 0 || measure_quiet(ctx, c.label, empty_hz, &empty) != 0) {
        return 1;
    }
    return expect_quiet_result(c.label, "tone's channel", busy_hz, busy, 0)
           | expect_quiet_result(c.label, "empty channel", empty_hz, empty, 1);
}

/* Replay @p c with a tone on one channel: the tap reports the replay's own geometry, and the tone is measured where it
 * is. */
static int
run_geometry_case(const GeometryCase& c) {
    RecordingSpec spec;
    spec.capture_rate_hz = c.capture_rate_hz;
    spec.fs4_shift = c.fs4_shift;
    const double tone_hz = c.fs4_shift ? -(double)c.capture_rate_hz / 4.0 : (double)kToneOffsetHz;
    /* 0.4 s: two quiet requests of nine 10 ms sub-windows each, and the replay's start. */
    spec.payload = tone_payload((size_t)c.capture_rate_hz * 2U / 5U, c.capture_rate_hz, tone_hz);
    char metadata_path[DSD_TEST_PATH_MAX];
    if (write_recording(spec, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_replay_opts(opts.get(), metadata_path);
    if (c.explicit_bandwidth) {
        opts->rtl_dsp_bw_khz = 48;
        opts->rtl_dsp_bw_source = DSD_RTL_DSP_BW_SOURCE_EXPLICIT;
    }
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not start the replay\n", c.label);
        if (ctx) {
            (void)rtl_stream_destroy(ctx);
        }
        return 1;
    }
    dsd_signal_capture capture;
    DSD_MEMSET(&capture, 0, sizeof(capture));
    int rc = wait_for_tap_capture(ctx, c.label, &capture);
    if (rc == 0) {
        rc = expect_replay_geometry(c, spec, capture) | expect_tone_bins(ctx, c);
    }
    rc |= expect_true("rtl_stream_stop", rtl_stream_stop(ctx) == 0);
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
    static const GeometryCase kGeometryCases[] = {
        {"centred recording", kRateHz, 0, 0},
        {"shifted recording", kRateHz, 1, 0},
        {"centred recording, resampled", kResampledCaptureHz, 0, 1},
        {"shifted recording, resampled", kResampledCaptureHz, 1, 1},
    };
    for (const GeometryCase& c : kGeometryCases) {
        rc |= run_geometry_case(c);
    }
    rc |= remove_fixture_dirs();
    return rc ? 1 : 0;
}
