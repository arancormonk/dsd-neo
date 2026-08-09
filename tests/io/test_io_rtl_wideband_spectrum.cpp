// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the wideband spectrum tap must stay silent until enabled,
 * clamp its FFT size, publish fftshifted bins whose argmax lands where the
 * injected tone actually is, throttle its publish rate, and drop the frame on
 * clear() so a consumer never labels bins with a stale center frequency.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <dsd-neo/io/rtl_stream_c.h>

#include "rtl_wideband_spectrum.h"

namespace {

constexpr uint32_t kCaptureRateHz = 1536000;
constexpr uint32_t kCenterHz = 851000000;
constexpr double kToneOffsetHz = 200000.0;
constexpr int kPairs = 4096;

/* Interleaved float I/Q carrying a single complex tone at `offset_hz`. */
std::vector<float>
make_tone(double offset_hz, uint32_t rate_hz, int pairs) {
    std::vector<float> iq(static_cast<size_t>(pairs) * 2);
    const double w = 2.0 * M_PI * offset_hz / static_cast<double>(rate_hz);
    for (int n = 0; n < pairs; n++) {
        iq[static_cast<size_t>(n) * 2] = static_cast<float>(cos(w * n));
        iq[static_cast<size_t>(n) * 2 + 1] = static_cast<float>(sin(w * n));
    }
    return iq;
}

int
argmax_bin(const float* db, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (db[i] > db[best]) {
            best = i;
        }
    }
    return best;
}

/* Force the next update past the throttle and feed one block. */
void
publish_block(const std::vector<float>& iq, uint32_t center_hz) {
    rtl_wideband_spectrum_test_reset_throttle();
    rtl_wideband_spectrum_maybe_update(iq.data(), static_cast<int>(iq.size()), kCaptureRateHz, center_hz);
}

void
test_size_clamping(void) {
    assert(rtl_stream_wideband_spectrum_set_size(100) == 256);
    assert(rtl_stream_wideband_spectrum_get_size() == 256);
    assert(rtl_stream_wideband_spectrum_set_size(3000) == 2048);
    assert(rtl_stream_wideband_spectrum_get_size() == 2048);
    assert(rtl_stream_wideband_spectrum_set_size(500) == 512);
    assert(rtl_stream_wideband_spectrum_get_size() == 512);
    assert(rtl_stream_wideband_spectrum_set_size(1024) == 1024);
}

void
test_disabled_publishes_nothing(void) {
    assert(rtl_stream_wideband_spectrum_enabled() == 0);

    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) == 0);
}

void
test_tone_orientation(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    assert(rtl_stream_wideband_spectrum_enabled() == 1);

    const int n = rtl_stream_wideband_spectrum_get_size();
    assert(n == 1024);

    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    const int got = rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span);
    assert(got == n);
    assert(center == kCenterHz);
    assert(span == kCaptureRateHz);

    /* Bins are fftshifted: index n/2 is DC, so a +200 kHz tone must sit above it. */
    const int expected = (n / 2) + static_cast<int>(lrint(kToneOffsetHz / kCaptureRateHz * n));
    const int peak = argmax_bin(bins.data(), got);
    assert(peak >= expected - 1 && peak <= expected + 1);

    /* The peak must actually stand out from the noise floor, not just win a tie. */
    const int far = (n / 4);
    assert(bins[static_cast<size_t>(peak)] - bins[static_cast<size_t>(far)] > 20.0f);
}

void
test_publish_throttle(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    const uint32_t moved_hz = kCenterHz + 1000000u;

    /* Without a throttle reset the block is dropped: the frame keeps its center. */
    rtl_wideband_spectrum_maybe_update(iq.data(), static_cast<int>(iq.size()), kCaptureRateHz, moved_hz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) > 0);
    assert(center == kCenterHz);

    /* Once the throttle is cleared the same block publishes. */
    publish_block(iq, moved_hz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) > 0);
    assert(center == moved_hz);
}

void
test_clear_invalidates_frame(void) {
    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) > 0);

    rtl_wideband_spectrum_clear();
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) == 0);

    /* ...and stays empty until the demod thread publishes again. */
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) > 0);
    assert(center == kCenterHz);
}

void
test_short_block_is_skipped(void) {
    /* Fewer complex pairs than the FFT size must not publish a padded frame. */
    rtl_wideband_spectrum_clear();
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, 128);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span) == 0);
}

void
test_disable_drops_frame(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr) > 0);

    rtl_stream_wideband_spectrum_set_enabled(0);
    assert(rtl_stream_wideband_spectrum_enabled() == 0);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr) == 0);

    /* Re-enabling must not resurrect the pre-close frame. */
    rtl_stream_wideband_spectrum_set_enabled(1);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr) == 0);
    rtl_stream_wideband_spectrum_set_enabled(0);
}

void
test_max_bins_truncation(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(64, 0.0f);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), 64, nullptr, nullptr) == 64);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), 0, nullptr, nullptr) == 0);
    assert(rtl_stream_wideband_spectrum_get(nullptr, 64, nullptr, nullptr) == 0);
    rtl_stream_wideband_spectrum_set_enabled(0);
}

} // namespace

int
main(void) {
    test_size_clamping();
    test_disabled_publishes_nothing();
    test_tone_orientation();
    test_publish_throttle();
    test_clear_invalidates_frame();
    test_short_block_is_skipped();
    test_disable_drops_frame();
    test_max_bins_truncation();
    (void)fprintf(stdout, "IO_RTL_WIDEBAND_SPECTRUM: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
