// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: the wideband spectrum tap must stay silent until enabled,
 * publish fftshifted bins whose argmax lands where the injected tone actually
 * is, throttle its publish rate, drop the frame on clear() so a consumer never
 * labels bins with a stale center frequency, refuse a buffer too short to hold
 * a whole frame, and serialise frames so a consumer can tell a new one from a
 * re-read. Blocks shorter than the FFT (an I/Q replay's 256 pairs at 48 kHz)
 * add up to the frame one whole block would make, never a zero-padded one, and
 * a clear, a gap or a new centre starts that history over.
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/wideband_spectrum.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/decode_clock.h>

#include "rtl_wideband_spectrum.h"

/* The tap throttles on dsd_realtime_mono_ns(). This test supplies that clock and nothing advances it, so a block
 * offered right after a publish is throttled however long the runner takes between the two calls; a stall past the
 * 66 ms period on a busy runner used to let the block through and fail the throttle case. */
extern "C" uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_realtime_mono_ns(void) {
    return 1000000000ULL;
}

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
test_disabled_publishes_nothing(void) {
    assert(rtl_stream_wideband_spectrum_enabled() == 0);

    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) == 0);
}

void
test_tone_orientation(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    assert(rtl_stream_wideband_spectrum_enabled() == 1);

    const int n = DSD_WIDEBAND_SPECTRUM_BINS;

    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    const int got =
        rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr);
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
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);
    assert(center == kCenterHz);

    /* Once the throttle is cleared the same block publishes. */
    publish_block(iq, moved_hz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);
    assert(center == moved_hz);
}

void
test_clear_invalidates_frame(void) {
    std::vector<float> bins(2048, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);

    rtl_wideband_spectrum_clear();
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) == 0);

    /* ...and stays empty until the demod thread publishes again. */
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);
    assert(center == kCenterHz);
}

/* A short block is a quarter of the FFT, as an I/Q replay's 5.33 ms block is at 48 kHz. */
constexpr int kShortPairs = DSD_WIDEBAND_SPECTRUM_BINS / 4;

/* Offer pairs [first, first + kShortPairs) of @p iq as one block, past the throttle or not. */
void
feed_short_block(const std::vector<float>& iq, int first, uint32_t center_hz, bool past_throttle) {
    if (past_throttle) {
        rtl_wideband_spectrum_test_reset_throttle();
    }
    rtl_wideband_spectrum_maybe_update(&iq[static_cast<size_t>(first) * 2], kShortPairs * 2, kCaptureRateHz, center_hz);
}

/* The published frame's bin count, its bins in @p bins and its serial in @p serial. */
int
read_frame(std::vector<float>* bins, uint32_t* center, uint32_t* serial) {
    bins->assign(DSD_WIDEBAND_SPECTRUM_BINS, 0.0f);
    return rtl_stream_wideband_spectrum_get(bins->data(), static_cast<int>(bins->size()), center, nullptr, serial);
}

bool
frames_match(const std::vector<float>& a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size(); i++) {
        if (fabsf(a[i] - b[i]) > 1e-4f) {
            return false;
        }
    }
    return a.size() == b.size();
}

/* The frame one whole DSD_WIDEBAND_SPECTRUM_BINS-pair block of @p iq makes right after a clear. */
std::vector<float>
whole_block_frame(const std::vector<float>& iq) {
    rtl_wideband_spectrum_clear();
    rtl_wideband_spectrum_test_reset_throttle();
    rtl_wideband_spectrum_maybe_update(iq.data(), DSD_WIDEBAND_SPECTRUM_BINS * 2, kCaptureRateHz, kCenterHz);
    std::vector<float> bins;
    uint32_t center = 0;
    assert(read_frame(&bins, &center, nullptr) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(center == kCenterHz);
    return bins;
}

/*
 * Blocks shorter than the FFT used to be skipped, so a replay under 192 kHz
 * left the Spectrum screen blank. Now four quarter blocks make the frame the
 * whole block of the same pairs makes, and until the fourth arrives nothing is
 * published: no frame padded out with zeros.
 */
void
test_short_blocks_add_up_to_one_frame(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    const std::vector<float> whole = whole_block_frame(iq);

    rtl_wideband_spectrum_clear();
    std::vector<float> bins;
    uint32_t center = 0;
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
        assert(read_frame(&bins, &center, nullptr) == 0);
    }
    feed_short_block(iq, 3 * kShortPairs, kCenterHz, true);
    assert(read_frame(&bins, &center, nullptr) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(center == kCenterHz);
    assert(frames_match(bins, whole));

    /* The tone stands where it is, as in a whole block's frame. */
    const int n = DSD_WIDEBAND_SPECTRUM_BINS;
    const int expected = (n / 2) + static_cast<int>(lrint(kToneOffsetHz / kCaptureRateHz * n));
    const int peak = argmax_bin(bins.data(), n);
    assert(peak >= expected - 1 && peak <= expected + 1);
}

/* A block the throttle holds back still joins the history: the next frame
 * needs its pairs. */
void
test_throttled_short_blocks_fill_the_window(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    const std::vector<float> whole = whole_block_frame(iq);

    /* whole_block_frame() just published, so the first three are throttled. */
    rtl_wideband_spectrum_clear();
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, false);
    }
    feed_short_block(iq, 3 * kShortPairs, kCenterHz, true);
    std::vector<float> bins;
    assert(read_frame(&bins, nullptr, nullptr) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(frames_match(bins, whole));
}

/* A whole block keeps its tail for a short block after it, which then makes a
 * frame at once; in a run of whole blocks only the first does (the rest analyse
 * themselves and copy nothing), so a short block after the run waits for a
 * whole window of short blocks. */
void
test_a_run_of_whole_blocks_keeps_no_history(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    (void)whole_block_frame(iq);
    std::vector<float> bins;
    uint32_t serial = 0;

    /* One whole block (whole_block_frame()'s), then a short one: a frame at once. */
    assert(read_frame(&bins, nullptr, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    uint32_t before = serial;
    feed_short_block(iq, 0, kCenterHz, true);
    assert(read_frame(&bins, nullptr, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(serial != before);

    /* Two whole blocks in a row: each still publishes its own frame. */
    for (int k = 0; k < 2; k++) {
        before = serial;
        rtl_wideband_spectrum_test_reset_throttle();
        rtl_wideband_spectrum_maybe_update(iq.data(), DSD_WIDEBAND_SPECTRUM_BINS * 2, kCaptureRateHz, kCenterHz);
        assert(read_frame(&bins, nullptr, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
        assert(serial != before);
    }
    /* The second kept no history: three short blocks make no frame, the fourth does. */
    before = serial;
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
        assert(read_frame(&bins, nullptr, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
        assert(serial == before);
    }
    feed_short_block(iq, 3 * kShortPairs, kCenterHz, true);
    assert(read_frame(&bins, nullptr, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(serial != before);
}

/* A clear drops the pairs before it: the first frame after it waits for a
 * whole window of pairs offered after it. */
void
test_clear_restarts_the_history(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    const std::vector<float> whole = whole_block_frame(iq);

    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
    }
    rtl_wideband_spectrum_clear();
    std::vector<float> bins;
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
        assert(read_frame(&bins, nullptr, nullptr) == 0);
    }
    feed_short_block(iq, 3 * kShortPairs, kCenterHz, true);
    assert(read_frame(&bins, nullptr, nullptr) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(frames_match(bins, whole));
}

/* A gap, or a block at a new centre, starts the history over too, but leaves
 * the frame already published: a new frame waits for a whole window of pairs
 * from after the break. */
void
test_a_gap_or_a_new_centre_restarts_the_history(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    (void)whole_block_frame(iq);
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
    }
    std::vector<float> bins;
    uint32_t center = 0;
    uint32_t serial = 0;
    assert(read_frame(&bins, &center, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    const uint32_t before = serial;

    rtl_wideband_spectrum_note_gap();
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, true);
        assert(read_frame(&bins, &center, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
        assert(serial == before);
    }
    feed_short_block(iq, 3 * kShortPairs, kCenterHz, true);
    assert(read_frame(&bins, &center, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(serial != before);
    const uint32_t after_gap = serial;

    /* Three blocks here, then the centre moves: they are not the new band's. */
    const uint32_t moved_hz = kCenterHz + 1000000u;
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, kCenterHz, false);
    }
    for (int b = 0; b < 3; b++) {
        feed_short_block(iq, b * kShortPairs, moved_hz, true);
        assert(read_frame(&bins, &center, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
        assert(serial == after_gap);
        assert(center == kCenterHz);
    }
    feed_short_block(iq, 3 * kShortPairs, moved_hz, true);
    assert(read_frame(&bins, &center, &serial) == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(serial != after_gap);
    assert(center == moved_hz);
}

void
test_disable_drops_frame(void) {
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(2048, 0.0f);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, nullptr) > 0);

    rtl_stream_wideband_spectrum_set_enabled(0);
    assert(rtl_stream_wideband_spectrum_enabled() == 0);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, nullptr)
           == 0);

    /* Re-enabling must not resurrect the pre-close frame. */
    rtl_stream_wideband_spectrum_set_enabled(1);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, nullptr)
           == 0);
    rtl_stream_wideband_spectrum_set_enabled(0);
}

/*
 * A frame is all of the span or none of it. Filling a short buffer with the
 * bins that fit would hand back the low end of the capture carrying a center
 * and span that describe the whole of it — an axis wrong by up to a quarter of
 * the span, on data that looks perfectly plausible.
 */
void
test_a_short_buffer_is_refused(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(DSD_WIDEBAND_SPECTRUM_BINS, 0.0f);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), DSD_WIDEBAND_SPECTRUM_BINS, nullptr, nullptr, nullptr)
           == DSD_WIDEBAND_SPECTRUM_BINS);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), DSD_WIDEBAND_SPECTRUM_BINS - 1, nullptr, nullptr, nullptr)
           == 0);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), 64, nullptr, nullptr, nullptr) == 0);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), 0, nullptr, nullptr, nullptr) == 0);
    assert(rtl_stream_wideband_spectrum_get(nullptr, DSD_WIDEBAND_SPECTRUM_BINS, nullptr, nullptr, nullptr) == 0);
    rtl_stream_wideband_spectrum_set_enabled(0);
}

/*
 * The serial is what lets a consumer polling on its own clock tell a new frame
 * from a re-read of the one it already drew. Without it a waterfall scrolls a
 * duplicate row every time the poll wins the race against the publish period.
 */
void
test_the_serial_marks_new_frames_only(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(DSD_WIDEBAND_SPECTRUM_BINS, 0.0f);
    uint32_t first = 0;
    uint32_t again = 0;
    uint32_t next = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, &first) > 0);
    assert(first != 0U);

    /* Re-reading the same published frame must report the same serial... */
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, &again) > 0);
    assert(again == first);

    /* ...and a block the throttle lets through must move it on. */
    publish_block(iq, kCenterHz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), nullptr, nullptr, &next) > 0);
    assert(next != first);

    rtl_stream_wideband_spectrum_set_enabled(0);
}

/*
 * A copy the writer ran through is not a frame: its bins may straddle two
 * publishes and the center it came with may belong to either. The consumer
 * holds its last picture when there is nothing new, so reporting no frame costs
 * a repaint; reporting a torn one puts a mislabelled row in the history.
 */
void
test_a_torn_read_is_not_a_frame(void) {
    rtl_stream_wideband_spectrum_set_enabled(1);
    const std::vector<float> iq = make_tone(kToneOffsetHz, kCaptureRateHz, kPairs);
    publish_block(iq, kCenterHz);

    std::vector<float> bins(DSD_WIDEBAND_SPECTRUM_BINS, 0.0f);
    uint32_t center = 0;
    uint32_t span = 0;
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);

    /* Every attempt tears, so the read never gets a clean copy of anything. */
    const std::vector<float> held = bins;
    rtl_wideband_spectrum_test_set_tear(1);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) == 0);
    rtl_wideband_spectrum_test_set_tear(0);
    /* "No frame" has to mean the caller's buffer is untouched. The consumer holds
     * its last picture across a gap, so a rejected read that had already written
     * over it would leave torn bins on screen under the previous frame's axis. */
    assert(bins == held);

    /* ...and an untorn read still works afterwards. */
    publish_block(iq, kCenterHz);
    assert(rtl_stream_wideband_spectrum_get(bins.data(), static_cast<int>(bins.size()), &center, &span, nullptr) > 0);
    assert(center == kCenterHz);
    rtl_stream_wideband_spectrum_set_enabled(0);
}

} // namespace

int
main(void) {
    test_disabled_publishes_nothing();
    test_tone_orientation();
    test_publish_throttle();
    test_clear_invalidates_frame();
    test_short_blocks_add_up_to_one_frame();
    test_throttled_short_blocks_fill_the_window();
    test_a_run_of_whole_blocks_keeps_no_history();
    test_clear_restarts_the_history();
    test_a_gap_or_a_new_centre_restarts_the_history();
    test_disable_drops_frame();
    test_a_short_buffer_is_refused();
    test_the_serial_marks_new_frames_only();
    test_a_torn_read_is_not_a_frame();
    (void)DSD_FPRINTF(stdout, "IO_RTL_WIDEBAND_SPECTRUM: ok\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
