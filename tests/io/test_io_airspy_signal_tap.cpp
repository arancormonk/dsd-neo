// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A live native Airspy stream on the in-process SDK double (airspy_sdk_double.h): the real stream workers and the
 * native adapter, fed noise transfers through the SDK's receive callback. A retune the device refuses logs a warning
 * that names the native Airspy backend.
 */

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/rtl_stream_fwd.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <memory>
#include <mutex>
#include <random>
#include <stdint.h>
#include <string>
#include <vector>
#include "airspy_sdk_double.h"
#include "rtl_stream_test_support.h"

using namespace airspy_double;

namespace {
constexpr uint32_t kChannelHz = 851375000U;
constexpr int kTransferPairs = 65536;

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

/* Read some of @p ctx's output, waiting a moment when none is ready. */
void
read_some(RtlSdrContext* ctx) {
    float block[2048];
    int got = 0;
    if (rtl_stream_read(ctx, block, sizeof(block) / sizeof(block[0]), &got) != 0 || got == 0) {
        dsd_sleep_ms(1);
    }
}

/* One transfer of @p iq through the SDK's receive callback, then reading @p ctx until the demod released it. */
void
feed_transfer(RtlSdrContext* ctx, const std::vector<float>& iq) {
    const uint64_t start_ns = dsd_time_monotonic_ns();
    while (!deliver(iq.data(), (int)(iq.size() / 2))) {
        CHECK(dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL);
        dsd_sleep_ms(1);
    }
    const uint64_t produced = live_sequence().input_produced;
    while (live_sequence().demod_idle_consumed < produced) {
        CHECK(dsd_time_monotonic_ns() - start_ns < 5000ULL * 1000000ULL);
        read_some(ctx);
    }
}

/* B3: a retune the device refuses names the native Airspy in its warning, not an RTL-SDR. */
void
test_retune_warning_names_airspy() {
    auto opts = airspy_opts();
    RtlSdrContext* ctx = nullptr;
    CHECK(rtl_stream_create(opts.get(), &ctx) == 0 && ctx);
    CHECK(rtl_stream_start(ctx) == 0);
    feed_transfer(ctx, noise(kTransferPairs, 1));
    log_clear();
    failure = 4; /* a frequency change to a new value fails */
    CHECK(rtl_stream_tune(ctx, kChannelHz + 1000000U) < 0);
    failure = 0;
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
    test_retune_warning_names_airspy();
    return 0;
}
