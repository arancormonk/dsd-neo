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
 * Airspy's controls. A quiet request at the channel is measured there, and a retune moves the output generation. After
 * a retune the samples its mute discards never reach the tap: the new capture starts with the first transfer past the
 * mute, and a short request's lead counts from there. A retune the device refuses logs a warning that names the
 * native Airspy backend.
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
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/signal_search.h>
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

/* One read of @p ctx's output that waits at most 100 ms for samples (DSD_RTL_READ_WANT_IDLE_BOUND); whether it read any. */
bool
read_bounded(RtlSdrContext* ctx, float* block, size_t count) {
    int got = 0;
    return rtl_stream_read_ex2(ctx, block, nullptr, count, &got, DSD_RTL_READ_WANT_IDLE_BOUND) == 0 && got > 0;
}

/* Read @p ctx's output until @p done, for at most kWaitNs (each read bounded, so a stalled stream meets the deadline). */
template <typename Done>
void
read_until(RtlSdrContext* ctx, Done done) {
    const uint64_t start_ns = dsd_realtime_mono_ns();
    float block[2048];
    while (!done()) {
        CHECK(dsd_realtime_mono_ns() - start_ns < kWaitNs);
        if (!read_bounded(ctx, block, sizeof(block) / sizeof(block[0]))) {
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

/* Reads @p ctx's output on its own thread until destroyed, so the demod never waits on a full output ring while the test
   feeds the SDK's callback itself. Each read gives up after 100 ms without samples, so the destructor's join returns
   while the stream still runs: destroy the reader before stopping the stream, which no read may overlap. */
class OutputReader {
  public:
    explicit OutputReader(RtlSdrContext* ctx) : ctx_(ctx), thread_([this] { run(); }) {}

    ~OutputReader() {
        stop_.store(true);
        thread_.join();
    }

    OutputReader(const OutputReader&) = delete;
    OutputReader& operator=(const OutputReader&) = delete;

  private:
    void
    run() {
        float block[2048];
        while (!stop_.load()) {
            if (!read_bounded(ctx_, block, sizeof(block) / sizeof(block[0]))) {
                dsd_sleep_ms(1);
            }
        }
    }

    RtlSdrContext* ctx_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/* One transfer of @p pairs from @p iq into the receive callback, then until the demod came back for input after it
   (the paced feeder's boundary: the tap has taken what the transfer committed). */
void
deliver_paced(const float* iq, int pairs) {
    const uint64_t start_ns = dsd_realtime_mono_ns();
    while (!deliver(iq, pairs)) {
        CHECK(dsd_realtime_mono_ns() - start_ns < kWaitNs);
        dsd_sleep_ms(1);
    }
    const uint64_t produced = live_sequence().input_produced;
    while (live_sequence().demod_idle_consumed < produced) {
        CHECK(dsd_realtime_mono_ns() - start_ns < kWaitNs);
        dsd_sleep_ms(1);
    }
}

/* Whether the tap's capture is the one of the live output generation, centred on @p channel_hz, into @p capture. */
bool
tap_on(uint32_t channel_hz, dsd_signal_capture* capture) {
    return rtl_signal_capture_get(capture) && capture->generation == rtl_stream_output_generation()
           && capture->capture_hz == channel_hz;
}

/* A short quiet request after a native Airspy's retune (issue #834, D-E): the SDK hands over transfers the retune's
   mute discards, a bright carrier at the new channel, then noise. None of the muted pairs reaches the tap as the new
   channel's capture, which first shows with a noise transfer; a one-slice request made then, with the engine's 10 ms
   lead, completes only once the noise after that block holds the lead and its slice, within the transfer that brings
   the last of them, and reads the noise, not the carrier. */
void
test_retune_lead_after_the_mute() {
    constexpr int kPairs = kTransferPairs;
    auto opts = airspy_opts();
    RtlSdrContext* ctx = nullptr;
    CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
    CHECK(rtl_stream_start(ctx) == 0);
    const uint32_t capture_rate = rate;
    std::vector<float> carrier(2 * (size_t)kPairs);
    for (int i = 0; i < kPairs; ++i) {
        carrier[2 * (size_t)i] = 0.5f; /* a carrier at the capture centre: the new channel */
    }
    std::vector<std::vector<float>> noises;
    for (unsigned seed = 40; seed < 56; ++seed) {
        noises.push_back(noise(kPairs, seed));
    }
    auto reader = std::make_unique<OutputReader>(ctx);
    {
        dsd_signal_capture capture{};
        for (size_t i = 0; !tap_on(kChannelHz, &capture); ++i) {
            deliver_paced(noises[i % noises.size()].data(), kPairs);
        }
        const uint32_t moved_hz = kChannelHz + 1000000U;
        CHECK(rtl_stream_tune(ctx, moved_hz) == 0);
        /* What the retune's last mute discards (rtl_device_mute(): two bytes a pair), as the stream sizes it. */
        const int mute_bytes =
            rtl_stream_test_retune_mute_bytes_for_device(rtl_stream_test_device(), capture_rate, 1, 0);
        int muted = (mute_bytes + 1) / 2;
        CHECK(muted > 0);
        while (muted > 0) {
            const int pairs = muted < kPairs ? muted : kPairs;
            deliver_paced(carrier.data(), pairs);
            muted -= pairs;
            CHECK(!tap_on(moved_hz, &capture)); /* no muted pair reached the tap as the new capture */
        }
        /* The first noise transfer, or the next: a retune's input purge that the demod, waiting on the empty ring
           through the mute, takes only once samples come again drops what it read with it. */
        for (size_t t = 0; t < 2 && !tap_on(moved_hz, &capture); ++t) {
            deliver_paced(noises[t].data(), kPairs);
        }
        CHECK(tap_on(moved_hz, &capture));
        CHECK(capture.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED && capture.rate_hz == capture_rate);

        dsd_signal_quiet_request request{};
        request.request_id = 41U;
        request.frequency_hz = moved_hz;
        request.classes = DSD_SIGNAL_QUIET_CLASS_12K5;
        request.windows = 1U;
        request.lead_pairs = capture_rate / 100U;
        request.capture = capture;
        uint32_t ticket = 0U;
        CHECK(rtl_signal_quiet_request(&request, &ticket, nullptr) == 0);
        const size_t n = (size_t)dsd_signal_fft_size_for_width(capture_rate, DSD_SIGNAL_QUIET_WIDTH_12K5_HZ);
        CHECK(n > 0);
        const size_t slice = n * ((capture_rate / 100U + n - 1) / n);
        const size_t need = request.lead_pairs + slice;
        const size_t transfers = (need + kPairs - 1) / kPairs;
        for (size_t t = 2; t < 2 + transfers; ++t) {
            CHECK(rtl_signal_quiet_status(ticket) == 0); /* nothing before the lead and the slice are in */
            deliver_paced(noises[t % noises.size()].data(), kPairs);
        }
        CHECK(rtl_signal_quiet_status(ticket) != 0);
        dsd_signal_quiet_result result{};
        CHECK(rtl_signal_quiet_take(ticket, &result) == 1);
        CHECK(result.failure == DSD_SIGNAL_MEASURE_OK && result.valid);
        CHECK(result.capture.generation == capture.generation);
        CHECK(result.max_ratio_db < 10.0f); /* the noise's reading: the carrier would stand tens of dB over it */
    }
    reader.reset();
    CHECK(rtl_stream_stop(ctx) == 0);
    CHECK(rtl_stream_destroy(ctx) == 0);
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
    test_retune_lead_after_the_mute();
    return 0;
}
