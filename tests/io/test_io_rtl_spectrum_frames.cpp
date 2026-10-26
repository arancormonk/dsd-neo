// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The narrow spectrum's publication (rtl_metrics.cpp): the demod folds each frame out of place into a buffer no reader
 * holds and makes it current, so the smoothed bins a reader gets must be the exponential average of the frames folded,
 * whichever buffers they went through:
 *  - average_carries_across_buffers: frames X, Y, X, Y, X, Y, X (more than there are buffers) read back as the average
 *    of the raw frames, 0.2 of each new one over 0.8 of the last, the first seeding it;
 *  - held_spares_fold_into_the_writers_own: with every buffer but the current one held by readers, a frame is folded
 *    but not published (a reader still gets the frame before it), and the next frame, once a buffer is free, blends
 *    with the unpublished one.
 */

#include <dsd-neo/io/rtl_metrics.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

namespace {

constexpr int kBins = 256;
constexpr int kPairs = 512;
constexpr int kRateHz = 48000;
constexpr float kNewWeight = 0.2f; /* rtl_metrics.cpp kSpecEmaNewWeight */
constexpr float kTolDb = 1e-3f;

int g_failures = 0;

void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* A tone at @p cycles per block with a little deterministic noise. */
std::vector<float>
make_iq(double cycles, uint32_t seed) {
    std::vector<float> iq(2U * kPairs);
    uint32_t lcg = seed;
    for (int k = 0; k < kPairs; k++) {
        lcg = (lcg * 1664525U) + 1013904223U;
        const float noise = ((float)((lcg >> 16) & 0xFFU) - 127.5f) / 2048.0f;
        const double ph = 2.0 * 3.14159265358979323846 * cycles * (double)k / (double)kPairs;
        iq[2U * (size_t)k] = (float)(0.5 * cos(ph)) + noise;
        iq[(2U * (size_t)k) + 1U] = (float)(0.5 * sin(ph)) - noise;
    }
    return iq;
}

std::vector<float>
read_bins(const char* label) {
    std::vector<float> bins(kBins, 0.0f);
    int rate = 0;
    const int n = rtl_stream_spectrum_get(bins.data(), kBins, &rate);
    check(label, n == kBins && rate == kRateHz);
    return bins;
}

/* The raw (seeded) frame of @p iq. */
std::vector<float>
raw_frame(const std::vector<float>& iq) {
    rtl_metrics_test_spectrum_reset();
    rtl_metrics_update_spectrum_from_iq(iq.data(), (int)iq.size(), kRateHz);
    return read_bins("raw frame read");
}

void
blend(std::vector<float>* avg, const std::vector<float>& frame) {
    for (int k = 0; k < kBins; k++) {
        (*avg)[(size_t)k] = (kNewWeight * frame[(size_t)k]) + ((1.0f - kNewWeight) * (*avg)[(size_t)k]);
    }
}

int
same_bins(const std::vector<float>& a, const std::vector<float>& b) {
    for (int k = 0; k < kBins; k++) {
        if (fabsf(a[(size_t)k] - b[(size_t)k]) > kTolDb) {
            return 0;
        }
    }
    return 1;
}

void
test_average_carries_across_buffers(const std::vector<float>& x, const std::vector<float>& y,
                                    const std::vector<float>& raw_x, const std::vector<float>& raw_y) {
    rtl_metrics_test_spectrum_reset();
    std::vector<float> want = raw_x;
    rtl_metrics_update_spectrum_from_iq(x.data(), (int)x.size(), kRateHz);
    check("average: first frame seeds", same_bins(read_bins("average: read"), want));
    for (int f = 1; f < 7; f++) {
        const std::vector<float>& iq = (f & 1) ? y : x;
        rtl_metrics_update_spectrum_from_iq(iq.data(), (int)iq.size(), kRateHz);
        blend(&want, (f & 1) ? raw_y : raw_x);
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "average: frame %d blends with the last", f + 1);
        check(label, same_bins(read_bins("average: read"), want));
    }
}

void
test_held_spares_fold_into_the_writers_own(const std::vector<float>& x, const std::vector<float>& y,
                                           const std::vector<float>& raw_x, const std::vector<float>& raw_y) {
    rtl_metrics_test_spectrum_reset();
    rtl_metrics_update_spectrum_from_iq(x.data(), (int)x.size(), kRateHz);
    std::vector<float> want = raw_x;
    check("held: spares held", rtl_metrics_test_spectrum_hold_spares(1) == 3);
    rtl_metrics_update_spectrum_from_iq(y.data(), (int)y.size(), kRateHz);
    check("held: the frame before is still the one published", same_bins(read_bins("held: read"), raw_x));
    blend(&want, raw_y); /* folded into the writer's own buffer */
    check("held: spares let go", rtl_metrics_test_spectrum_hold_spares(0) == 3);
    rtl_metrics_update_spectrum_from_iq(x.data(), (int)x.size(), kRateHz);
    blend(&want, raw_x);
    check("held: the next frame blends with the unpublished one", same_bins(read_bins("held: read"), want));
}

} // namespace

int
main(void) {
    (void)rtl_stream_spectrum_set_size(kBins);
    const std::vector<float> x = make_iq(37.0, 1U);
    const std::vector<float> y = make_iq(-91.0, 2U);
    const std::vector<float> raw_x = raw_frame(x);
    const std::vector<float> raw_y = raw_frame(y);
    check("raw frames differ", !same_bins(raw_x, raw_y));
    test_average_carries_across_buffers(x, y, raw_x, raw_y);
    test_held_spares_fold_into_the_writers_own(x, y, raw_x, raw_y);
    rtl_metrics_test_spectrum_reset();
    if (g_failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: spectrum frames\n");
    return 0;
}
