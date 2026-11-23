// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A live native Airspy stream on the in-process SDK double (airspy_sdk_double.h): the real stream workers and the
 * native adapter, fed noise transfers through the SDK's receive callback, at the rate an open with none configured
 * programs (an R2's and a Mini's highest on the desktop, their lowest with AIRSPY_SOURCE_PREFER_LOWEST_RATE, as on
 * Android). What reaches the Signal search and quiet tap is the capture of a receiver tuned on the channel: no
 * rotation, no offset, the centred measurement policy, the default clip level, and a gain identity that follows the
 * Airspy's controls. A quiet request at the channel is measured there, and a retune moves the output generation. A
 * retune the device refuses logs a warning that names the native Airspy backend.
 */

#include <atomic>
#include <cmath>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <memory>
#include <mutex>
#include <random>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>
#include "airspy_sdk_double.h"
#include "rtl_stream_test_support.h"

using namespace airspy_double;

namespace {
constexpr uint32_t kChannelHz = 851375000U;
constexpr int kTransferPairs = 65536;
constexpr uint64_t kWaitNs = 10000ULL * 1000000ULL;

std::mutex g_log_mutex;
std::string g_log;

void
log_tap(dsd_neo_log_level_t, const char* text, void*) {
    std::lock_guard<std::mutex> hold(g_log_mutex);
    g_log += text;
}

std::string
log_text() {
    std::lock_guard<std::mutex> hold(g_log_mutex);
    return g_log;
}

void
log_clear() {
    std::lock_guard<std::mutex> hold(g_log_mutex);
    g_log.clear();
}

/* An Airspy stream at the device's default rate (none configured): the highest listed one on the desktop, the lowest
   on Android (AIRSPY_SOURCE_PREFER_LOWEST_RATE). */
std::unique_ptr<dsd_opts>
airspy_opts() {
    auto opts = std::make_unique<dsd_opts>();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = kChannelHz;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_volume_multiplier = 1;
    opts->frame_p25p1 = 1;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "airspy");
    dsd_airspy_config_defaults(&opts->airspy);
    return opts;
}

/* Complex Gaussian noise, well under the clip level. */
std::vector<float>
noise(size_t pairs, unsigned seed) {
    std::vector<float> iq(2 * pairs);
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.0f, 0.01f);
    for (float& v : iq) {
        v = gauss(rng);
    }
    return iq;
}

rtl_stream_test_live_sequence
live_sequence() {
    rtl_stream_test_live_sequence s{};
    rtl_stream_test_get_live_sequence(&s);
    return s;
}

/* libairspy's consumer thread: noise transfers into the receive callback while the stream runs, each once the demod
   came back for input after the one before (or after 50 ms, so a demod held by a retune does not stop the samples). */
class Feeder {
  public:
    Feeder() : thread_([this] { run(); }) {}

    ~Feeder() {
        stop_.store(true);
        thread_.join();
    }

    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

  private:
    void
    run() {
        /* Sixteen different transfers in turn: one transfer repeated is periodic, not noise, and its spectrum fails
           the quiet measurement's occupancy check. */
        std::vector<std::vector<float>> transfers;
        for (unsigned seed = 10; seed < 26; ++seed) {
            transfers.push_back(noise(kTransferPairs, seed));
        }
        size_t next = 0;
        while (!stop_.load()) {
            const std::vector<float>& iq = transfers[next];
            if (!deliver(iq.data(), kTransferPairs)) {
                dsd_sleep_ms(1);
                continue;
            }
            next = (next + 1) % transfers.size();
            const uint64_t produced = live_sequence().input_produced;
            for (int waited = 0; waited < 50 && !stop_.load() && live_sequence().demod_idle_consumed < produced;
                 ++waited) {
                dsd_sleep_ms(1);
            }
        }
    }

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/* Read @p ctx's output until @p done, for at most kWaitNs. */
template <typename Done>
void
read_until(RtlSdrContext* ctx, Done done) {
    const uint64_t start_ns = dsd_time_monotonic_ns();
    float block[2048];
    while (!done()) {
        CHECK(dsd_time_monotonic_ns() - start_ns < kWaitNs);
        int got = 0;
        if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0 || got == 0) {
            dsd_sleep_ms(1);
        }
    }
}

/* Read @p ctx until the tap holds a capture of the live output generation, other than @p old_generation, centred where
   @p channel_hz says, into @p capture. */
void
await_capture(RtlSdrContext* ctx, uint32_t old_generation, uint32_t channel_hz, dsd_signal_capture* capture) {
    read_until(ctx, [&] {
        return rtl_signal_capture_get(capture) && capture->generation != old_generation
               && capture->generation == rtl_stream_output_generation() && capture->capture_hz == channel_hz;
    });
}

/* Read @p ctx until the tap's capture names @p gain, into @p capture. */
void
await_gain(RtlSdrContext* ctx, int gain, dsd_signal_capture* capture) {
    read_until(ctx, [&] { return rtl_signal_capture_get(capture) && capture->gain == gain; });
}

/* A quiet request at @p channel_hz on the tap's capture, read until it finishes; one that met dropped samples or a
   capture change is made again. */
dsd_signal_quiet_result
measure_quiet(RtlSdrContext* ctx, uint32_t channel_hz) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        dsd_signal_quiet_request request{};
        request.request_id = 7U + (uint64_t)attempt;
        request.frequency_hz = channel_hz;
        request.classes = DSD_SIGNAL_QUIET_CLASS_12K5;
        CHECK(rtl_signal_capture_get(&request.capture));
        uint32_t ticket = 0U;
        CHECK(rtl_signal_quiet_request(&request, &ticket, nullptr) == 0);
        read_until(ctx, [&] { return rtl_signal_quiet_status(ticket) != 0; });
        dsd_signal_quiet_result result{};
        const int rc = rtl_signal_quiet_take(ticket, &result);
        if (rc == 1) {
            return result;
        }
        CHECK(
            rc == -1
            && (result.failure == DSD_SIGNAL_MEASURE_DROPPED || result.failure == DSD_SIGNAL_MEASURE_CAPTURE_CHANGED));
    }
    CHECK(false);
    return dsd_signal_quiet_result{};
}

/* Tune @p ctx to @p frequency_hz while the stream's output is read: the tune's result. */
int
tune_reading(RtlSdrContext* ctx, uint32_t frequency_hz) {
    std::atomic<bool> tuned{false};
    int rc = 1;
    std::thread tuner([&] {
        rc = rtl_stream_tune(ctx, frequency_hz);
        tuned.store(true);
    });
    read_until(ctx, [&] { return tuned.load(); });
    tuner.join();
    return rc;
}

/* The capture a native Airspy stream hands the tap, at each device's default rate. */
void
test_tap_capture_geometry() {
    static const uint32_t kDevices[2][2] = {{10000000U, 2500000U}, {6000000U, 3000000U}};
    const uint32_t saved[2] = {rates[0], rates[1]};
    for (const auto& listed : kDevices) {
        rates[0] = listed[0];
        rates[1] = listed[1];
        const uint32_t want_rate = AIRSPY_SOURCE_PREFER_LOWEST_RATE ? listed[1] : listed[0];
        auto opts = airspy_opts();
        RtlSdrContext* ctx = nullptr;
        CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
        CHECK(rtl_stream_start(ctx) == 0);
        CHECK(rate == want_rate);
        {
            Feeder feeder;
            dsd_signal_capture capture{};
            await_capture(ctx, 0U, kChannelHz, &capture);
            CHECK(capture.rate_hz == want_rate);
            CHECK(capture.rotation_hz == 0 && capture.capture_offset_hz == 0);
            CHECK(capture.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED);
            CHECK(std::abs(capture.clip_level - DSD_SIGNAL_CLIP_LEVEL_DEFAULT) < 1e-6f);
            CHECK(dsd_signal_quiet_airspy_centred_rate_supported(capture.rate_hz) == 1);

            // The channel at the capture centre is measured.
            const dsd_signal_quiet_result result = measure_quiet(ctx, kChannelHz);
            CHECK(result.failure == DSD_SIGNAL_MEASURE_OK && result.valid);
            CHECK(result.capture.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED);
            CHECK(std::abs(result.power_dbfs - result.floor_dbfs - result.max_ratio_db) < 1e-3f);

            // The gain identity follows the Airspy's controls, and the same controls again give the same identity.
            const int gain = capture.gain;
            dsd_airspy_config changed = opts->airspy;
            changed.vga_gain = opts->airspy.vga_gain + 1;
            CHECK(rtl_stream_airspy_controls(&changed) == 0);
            read_until(ctx, [&] { return rtl_signal_capture_get(&capture) && capture.gain != gain; });
            CHECK(rtl_stream_airspy_controls(&opts->airspy) == 0);
            await_gain(ctx, gain, &capture);

            // A retune: a new output generation, still tuned on the channel.
            const uint32_t before = capture.generation;
            const uint32_t moved_hz = kChannelHz + 1000000U;
            CHECK(tune_reading(ctx, moved_hz) == 0);
            await_capture(ctx, before, moved_hz, &capture);
            CHECK(capture.generation != before && capture.rate_hz == want_rate);
            CHECK(capture.rotation_hz == 0 && capture.capture_offset_hz == 0);
            CHECK(capture.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED);
        }
        CHECK(rtl_stream_stop(ctx) == 0);
        CHECK(rtl_stream_destroy(ctx) == 0);
    }
    rates[0] = saved[0];
    rates[1] = saved[1];
}

/* B3: a retune the device refuses names the native Airspy in its warning, not an RTL-SDR. */
void
test_retune_warning_names_airspy() {
    auto opts = airspy_opts();
    RtlSdrContext* ctx = nullptr;
    CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
    CHECK(rtl_stream_start(ctx) == 0);
    {
        Feeder feeder;
        dsd_signal_capture capture{};
        await_capture(ctx, 0U, kChannelHz, &capture);
        log_clear();
        failure = 4; /* a frequency change to a new value fails */
        CHECK(tune_reading(ctx, kChannelHz + 1000000U) < 0);
        failure = 0;
    }
    const std::string text = log_text();
    if (text.find("retune warning") == std::string::npos || text.find("backend=airspy ") == std::string::npos) {
        DSD_FPRINTF(stderr, "FAIL: the retune warning does not name the Airspy:\n%s\n", text.c_str());
        CHECK(false);
    }
    CHECK(rtl_stream_stop(ctx) == 0);
    CHECK(rtl_stream_destroy(ctx) == 0);
}
} // namespace

int
main() {
    dsd_neo_log_set_tap(log_tap, nullptr);
    test_tap_capture_geometry();
    test_retune_warning_names_airspy();
    return 0;
}
