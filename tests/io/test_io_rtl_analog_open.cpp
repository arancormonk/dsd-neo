// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The analog channel is settled at stream start against the demod rate the
 * source actually delivers, not the configured DSP bandwidth the options were
 * first applied at. An IQ replay stands in for a device that forces its rate:
 * the sidecar's demod rate differs from its rtl_bw_khz, so the provisional
 * choice made while the pipeline is configured is wrong until the rate chain
 * is final. Like a live device, the fixtures have no post-demod decimation
 * (post_downsample 1): the channel filter runs at the demod rate it is designed
 * for, which a post_downsample above 1 would break.
 *
 * - a sidecar with post_downsample 3: the channel filter runs at three times
 *   the demod rate the width would be designed and checked at, so an explicit
 *   width fails the start with an actionable message, while the unset default
 *   keeps the legacy design and starts.
 * - demod_rate 16000 with the unset default: 16 kHz does not fit, so the legacy
 *   WIDE design takes over and the channel is published as DSP-limited at the
 *   width the rate leaves (not the 16 kHz picked for the 48 kHz bandwidth).
 * - demod_rate 78125 with the unset default: the 16 kHz design fits and runs.
 * - an explicit 16 kHz width at demod_rate 16000: the start fails with the
 *   validator's text.
 * - a 1 kHz FM tone captured at 78125 Hz: the monitor delivers it at the 48 kHz
 *   output rate once, not resampled a second time (which played it back at
 *   1628 Hz in 0.61 of the capture's duration).
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/log.h>
#include <memory>
#include <vector>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "test_support.h"

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

/* Every error the stream logged since the last reset, concatenated. */
static char g_errors[4096];

static void
capture_errors(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level != LOG_LEVEL_ERROR || !text) {
        return;
    }
    const size_t used = strlen(g_errors);
    if (used + 1U < sizeof g_errors) {
        DSD_SNPRINTF(g_errors + used, sizeof g_errors - used, "%s", text);
    }
}

namespace {
struct ReplayRate {
    uint32_t sample_rate_hz;
    uint32_t base_decimation;
    uint32_t post_downsample;
    uint32_t demod_rate_hz;
};
} // namespace

static void
fill_capture_cfg(dsd_iq_capture_config* cfg, const char* data_path, const char* metadata_path, const ReplayRate& r) {
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    DSD_SNPRINTF(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    DSD_SNPRINTF(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", metadata_path);
    cfg->format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(cfg->capture_stage, sizeof(cfg->capture_stage), "%s", "post_mute_pre_widen");
    cfg->sample_rate_hz = r.sample_rate_hz;
    cfg->center_frequency_hz = 460125000ULL;
    cfg->capture_center_frequency_hz = 460125000ULL;
    cfg->ppm = 0;
    cfg->tuner_gain_tenth_db = 270;
    cfg->rtl_dsp_bw_khz = 48;
    cfg->base_decimation = r.base_decimation;
    cfg->post_downsample = r.post_downsample;
    cfg->demod_rate_hz = r.demod_rate_hz;
    cfg->offset_tuning_enabled = 0;
    cfg->fs4_shift_enabled = 1;
    cfg->combine_rotate_enabled = 1;
    cfg->muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg->source_backend, sizeof(cfg->source_backend), "%s", "rtl");
    DSD_SNPRINTF(cfg->source_args, sizeof(cfg->source_args), "%s", "dev=0");
}

/* @p centred records the carrier on 0 Hz, as the committed analog fixtures do: no fs/4 shift or rotation for the
 * replay to undo. */
static int
make_replay_fixture_with(const ReplayRate& rate, const char* tag, const std::vector<uint8_t>& payload, int centred,
                         char* out_metadata_path, size_t out_size) {
    char temp_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(temp_dir, sizeof(temp_dir), tag)) {
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
    fill_capture_cfg(&cfg, data_path, metadata_path, rate);
    if (centred) {
        cfg.fs4_shift_enabled = 0;
        cfg.combine_rotate_enabled = 0;
    }
    dsd_iq_capture_writer* writer = NULL;
    char err_buf[256] = {0};
    if (dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf)) != DSD_IQ_OK || !writer) {
        DSD_FPRINTF(stderr, "FAIL: could not open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown");
        return 1;
    }
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

static int
make_replay_fixture(const ReplayRate& rate, const char* tag, char* out_metadata_path, size_t out_size) {
    /* Mid-scale cu8 is silence: nothing here depends on the samples. */
    const std::vector<uint8_t> payload(8192U, 127U);
    return make_replay_fixture_with(rate, tag, payload, 0, out_metadata_path, out_size);
}

static constexpr double kToneHz = 1000.0;
static constexpr double kToneDeviationHz = 3000.0;

/* An FM carrier on 0 Hz modulated by a kToneHz tone at kToneDeviationHz deviation, as cu8 at @p rate_hz. */
static std::vector<uint8_t>
fm_tone_cu8(uint32_t rate_hz, double seconds) {
    const size_t count = (size_t)std::lround(seconds * (double)rate_hz);
    std::vector<uint8_t> out(count * 2U);
    const double beta = kToneDeviationHz / kToneHz;
    for (size_t i = 0; i < count; i++) {
        const double phase = beta * std::sin(2.0 * M_PI * kToneHz * (double)i / (double)rate_hz);
        out[2U * i] = (uint8_t)std::lround(127.5 + 114.0 * std::cos(phase));
        out[2U * i + 1U] = (uint8_t)std::lround(127.5 + 114.0 * std::sin(phase));
    }
    return out;
}

static void
prepare_analog_replay_opts(dsd_opts* opts, const char* metadata_path, int nfm_width_hz) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_requested = 1;
    opts->iq_replay_rate_mode = DSD_IQ_REPLAY_RATE_FAST;
    opts->iq_replay_loop = 0;
    DSD_SNPRINTF(opts->iq_replay_path, sizeof(opts->iq_replay_path), "%s", metadata_path);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "iqreplay:%s", metadata_path);
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->analog_nfm_bandwidth_hz = nfm_width_hz;
}

namespace {
struct OpenResult {
    int start_rc;
    int family;
    int kind;
    int width_hz;
    int lpf_on;
};
} // namespace

/* Open the replay, read the analog profile stream start published, and close it again. */
static int
open_analog_replay(const ReplayRate& rate, const char* tag, int nfm_width_hz, OpenResult* out) {
    *out = OpenResult{-1, 0, 0, 0, 0};
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_replay_fixture(rate, tag, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_analog_replay_opts(opts.get(), metadata_path, nfm_width_hz);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx) {
        DSD_FPRINTF(stderr, "FAIL: %s: rtl_stream_create\n", tag);
        if (ctx) {
            rtl_stream_destroy(ctx);
        }
        return 1;
    }
    out->start_rc = rtl_stream_start(ctx);
    if (out->start_rc == 0) {
        out->family = rtl_stream_get_analog_profile(&out->kind, &out->width_hz, &out->lpf_on);
        (void)rtl_stream_stop(ctx);
    }
    (void)rtl_stream_destroy(ctx);
    return 0;
}

namespace {
struct MonitorAudio {
    uint32_t output_rate_hz;
    std::vector<float> samples;
};
} // namespace

/* Replay @p payload through the analog monitor to its end and keep every sample the stream delivered. */
static int
replay_monitor_audio(const ReplayRate& rate, const char* tag, const std::vector<uint8_t>& payload, MonitorAudio* out) {
    out->output_rate_hz = 0;
    out->samples.clear();
    char metadata_path[DSD_TEST_PATH_MAX];
    if (make_replay_fixture_with(rate, tag, payload, 1, metadata_path, sizeof(metadata_path)) != 0) {
        return 1;
    }
    std::unique_ptr<dsd_opts> opts = std::make_unique<dsd_opts>();
    prepare_analog_replay_opts(opts.get(), metadata_path, 0);
    RtlSdrContext* ctx = NULL;
    if (rtl_stream_create(opts.get(), &ctx) != 0 || !ctx || rtl_stream_start(ctx) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: could not start the replay\n", tag);
        if (ctx) {
            rtl_stream_destroy(ctx);
        }
        return 1;
    }
    out->output_rate_hz = rtl_stream_output_rate(ctx);
    const uint64_t start_ns = dsd_time_monotonic_ns();
    float block[2048];
    int rc = 0;
    for (;;) {
        int got = 0;
        if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0) {
            break; /* end of the replay */
        }
        if (got > 0) {
            out->samples.insert(out->samples.end(), block, block + got);
        }
        if (dsd_time_monotonic_ns() - start_ns > 10000ULL * 1000000ULL) {
            DSD_FPRINTF(stderr, "FAIL: %s: timed out draining the replay\n", tag);
            rc = 1;
            break;
        }
    }
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
    return rc;
}

/* Goertzel power of @p x[first, last) at @p hz. */
static double
goertzel_power(const std::vector<float>& x, size_t first, size_t last, double hz, uint32_t rate_hz) {
    const double coeff = 2.0 * std::cos(2.0 * M_PI * hz / (double)rate_hz);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = first; i < last; i++) {
        const double s0 = (double)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* The strongest audio tone between 300 and 3000 Hz (10 Hz steps) in the middle half of @p x. The raw stream still
 * carries the discriminator's wideband noise, which the monitor's voice filters remove later, so a spectral peak is
 * read rather than zero crossings. */
static double
dominant_tone_hz(const std::vector<float>& x, uint32_t rate_hz) {
    const size_t first = x.size() / 4U;
    const size_t last = x.size() - x.size() / 4U;
    if (last <= first || rate_hz == 0U) {
        return 0.0;
    }
    double best_hz = 0.0;
    double best_power = -1.0;
    for (int hz = 300; hz <= 3000; hz += 10) {
        const double power = goertzel_power(x, first, last, (double)hz, rate_hz);
        if (power > best_power) {
            best_power = power;
            best_hz = (double)hz;
        }
    }
    return best_hz;
}

static int
test_forced_rate_monitor_audio_is_resampled_once(void) {
    int rc = 0;
    /* A capture delivered at the 78125 Hz forced rate, already at the demod rate (no decimation). */
    const ReplayRate rate78k = {78125U, 1U, 1U, 78125U};
    const double seconds = 1.0;
    MonitorAudio audio;
    rc |= replay_monitor_audio(rate78k, "dsdneo_analog_open_78k_tone", fm_tone_cu8(rate78k.sample_rate_hz, seconds),
                               &audio);
    rc |= expect_int("78125 Hz tone replay output rate", (int)audio.output_rate_hz, 48000);
    /* Every second of capture is a second of monitor audio at the output rate, give or take the filters' edges. */
    const double expected = seconds * 48000.0;
    const double delivered = (double)audio.samples.size();
    if (delivered < 0.98 * expected || delivered > 1.01 * expected) {
        DSD_FPRINTF(stderr, "FAIL: 78125 Hz tone replay delivered %zu samples, want about %.0f\n", audio.samples.size(),
                    expected);
        rc |= 1;
    }
    const double tone_hz = dominant_tone_hz(audio.samples, audio.output_rate_hz);
    if (std::fabs(tone_hz - kToneHz) > 5.0) {
        DSD_FPRINTF(stderr, "FAIL: 78125 Hz tone replay plays the %.0f Hz tone at %.1f Hz\n", kToneHz, tone_hz);
        rc |= 1;
    }
    return rc;
}

int
main(void) {
    dsd_neo_log_set_tap(capture_errors, NULL);
    int rc = 0;
    OpenResult r;

    /* 512000 / 32 = 16000 Hz: the options' 48 kHz DSP bandwidth made the historical enable decision (filter on) at
     * configuration time, as it does before a live device settles on a lower rate, but the delivered demod rate
     * cannot fit the 16 kHz default. */
    const ReplayRate rate16k = {512000U, 32U, 1U, 16000U};
    rc |= open_analog_replay(rate16k, "dsdneo_analog_open_16k", 0, &r);
    rc |= expect_int("16 kHz default start", r.start_rc, 0);
    rc |= expect_int("16 kHz analog family published", r.family, 1);
    rc |= expect_int("16 kHz FM published", r.kind, DSD_ANALOG_DEMOD_FM);
    rc |= expect_int("16 kHz default runs DSP-limited", r.lpf_on, 0);
    rc |= expect_int("16 kHz legacy WIDE bounds the channel at 13.2 kHz", r.width_hz, 13200);

    /* 2500000 / 32 = 78125 Hz, the Airspy-style forced rate: the 16 kHz default fits the 288-tap capacity. */
    const ReplayRate rate78k = {2500000U, 32U, 1U, 78125U};
    rc |= open_analog_replay(rate78k, "dsdneo_analog_open_78k", 0, &r);
    rc |= expect_int("78125 Hz default start", r.start_rc, 0);
    rc |= expect_int("78125 Hz analog family published", r.family, 1);
    rc |= expect_int("78125 Hz width-driven filter", r.lpf_on, 1);
    rc |= expect_int("78125 Hz default width", r.width_hz, 16000);

    /* An explicit width the delivered rate cannot realize fails the start, with the validator's text. */
    g_errors[0] = '\0';
    rc |= open_analog_replay(rate16k, "dsdneo_analog_open_refused", 16000, &r);
    rc |= expect_int("explicit 16 kHz at 16000 Hz refused", r.start_rc != 0, 1);
    rc |= expect_int("refusal carries the validator text",
                     std::strstr(g_errors, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate") != NULL, 1);

    /* 1536000 / 32 / 3 = 16000 Hz after a post-demod decimation of 3: the channel filter runs at 48 kHz. 12.5 kHz
     * fits a 16 kHz rate, so only the post-decimation rule stands between it and a filter designed for the wrong
     * rate. */
    const ReplayRate post3 = {1536000U, 32U, 3U, 16000U};
    g_errors[0] = '\0';
    rc |= open_analog_replay(post3, "dsdneo_analog_open_post3", 12500, &r);
    rc |= expect_int("explicit width with post_downsample 3 refused", r.start_rc != 0, 1);
    rc |= expect_int("post-decimation refusal names the cause",
                     std::strstr(g_errors, "NFM bandwidth 12.5 kHz cannot be applied to this I/Q replay: "
                                           "post_downsample 3 runs the channel filter at 48000 Hz")
                         != NULL,
                     1);
    g_errors[0] = '\0';
    rc |= open_analog_replay(post3, "dsdneo_analog_open_post3_default", 0, &r);
    rc |= expect_int("default with post_downsample 3 still starts", r.start_rc, 0);
    rc |= expect_int("default with post_downsample 3 is the analog family", r.family, 1);

    rc |= test_forced_rate_monitor_audio_is_resampled_once();

    if (rc == 0) {
        std::printf("IO_RTL_ANALOG_OPEN: OK\n");
    } else {
        DSD_FPRINTF(stderr, "logged errors:\n%s\n", g_errors);
    }
    return rc;
}
