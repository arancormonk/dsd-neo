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
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "rtl_spectrum_kernels.h"
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

void
test_noise_bin_copy_is_bit_exact(void) {
    for (int n : {3, 7, 64, 256, 1024}) {
        std::vector<float> bins((size_t)n), want((size_t)n), got((size_t)n);
        uint32_t random = 19U;
        for (float& value : bins) {
            random = random * 1664525U + 1013904223U;
            DSD_MEMCPY(&value, &random, sizeof value);
        }
        for (int lo : {0, n / 2}) {
            for (int peak = lo; peak < n; peak++) {
                int count = 0;
                for (int k = lo; k < n; k++) {
                    if (k < peak - 2 || k > peak + 2) {
                        want[(size_t)count++] = bins[(size_t)k];
                    }
                }
                const int copied = dsd_io::spectrum_copy_noise_bins(bins.data(), lo, n - 1, peak, got.data());
                check("noise bins: count", copied == count);
                for (int k = 0; k < count; k++) {
                    uint32_t want_bits = 0U, got_bits = 0U;
                    DSD_MEMCPY(&want_bits, &want[(size_t)k], sizeof want_bits);
                    DSD_MEMCPY(&got_bits, &got[(size_t)k], sizeof got_bits);
                    check("noise bins: order and bits", want_bits == got_bits);
                }
            }
        }
    }
}

/* The original bin walk, kept separate from the span implementation so its wrap and smoothing order are pinned. */
void
fold_reference(const float* z, int n, float weight, bool seed, const float* prev, float* out) {
    for (int k = 0; k < n; k++) {
        int kk = k + (n >> 1);
        if (kk >= n) {
            kk -= n;
        }
        const float re = z[2U * (size_t)kk];
        const float im = z[2U * (size_t)kk + 1U];
        const float db = 10.0f * log10f(re * re + im * im + 1e-12f);
        out[k] = seed ? db : weight * db + (1.0f - weight) * prev[k];
    }
}

void
test_fold_is_bit_exact(void) {
    for (int n : {64, 128, 256, 512, 1024}) {
        std::vector<float> z((size_t)n * 2U), previous((size_t)n), want((size_t)n), got((size_t)n);
        uint32_t random = 19U;
        for (float& value : z) {
            random = random * 1664525U + 1013904223U;
            value = (float)((int32_t)(random >> 16) - 32768) / 32768.0f;
        }
        for (int k = 0; k < n; k++) {
            previous[(size_t)k] = (float)(k % 111) - 100.0f;
        }
        for (float weight : {0.2f, 0.6f}) {
            for (bool seed : {false, true}) {
                fold_reference(z.data(), n, weight, seed, previous.data(), want.data());
                dsd_io::spectrum_fold_db(z.data(), n, weight, seed, previous.data(), got.data());
                for (int k = 0; k < n; k++) {
                    uint32_t want_bits = 0U, got_bits = 0U;
                    DSD_MEMCPY(&want_bits, &want[(size_t)k], sizeof want_bits);
                    DSD_MEMCPY(&got_bits, &got[(size_t)k], sizeof got_bits);
                    check("fold: out-of-place bits", want_bits == got_bits);
                }
                got = previous;
                dsd_io::spectrum_fold_db(z.data(), n, weight, seed, got.data(), got.data());
                for (int k = 0; k < n; k++) {
                    uint32_t want_bits = 0U, got_bits = 0U;
                    DSD_MEMCPY(&want_bits, &want[(size_t)k], sizeof want_bits);
                    DSD_MEMCPY(&got_bits, &got[(size_t)k], sizeof got_bits);
                    check("fold: in-place bits", want_bits == got_bits);
                }
            }
        }
    }
}

/* A tone at @p cycles per block with a little deterministic noise. */
std::vector<float>
make_iq(double cycles, uint32_t seed) {
    std::vector<float> iq(static_cast<size_t>(2U) * kPairs);
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

/* size_change_reseeds: a frame never blends bins another FFT size wrote or left unwritten. Silence published at 1024,
 * then 64, then 1024 bins reads -120 dB (the floor of a zero frame) in every bin of each frame. */
void
test_size_change_reseeds(void) {
    rtl_metrics_test_spectrum_reset();
    const std::vector<float> silence(static_cast<size_t>(2U) * 1024U, 0.0f);
    for (int n : {1024, 64, 1024}) {
        check("size change: size set", rtl_stream_spectrum_set_size(n) == n);
        rtl_metrics_update_spectrum_from_iq(silence.data(), (int)silence.size(), kRateHz);
        std::vector<float> bins((size_t)n, 0.0f);
        check("size change: published at the new size", rtl_stream_spectrum_get(bins.data(), n, NULL) == n);
        int floor_bins = 0;
        for (float db : bins) {
            floor_bins += (fabsf(db + 120.0f) <= kTolDb) ? 1 : 0;
        }
        char label[64];
        DSD_SNPRINTF(label, sizeof label, "size change: %d bins all at -120 dB", n);
        check(label, floor_bins == n);
    }
    (void)rtl_stream_spectrum_set_size(kBins);
    rtl_metrics_test_spectrum_reset();
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

void
test_unpublished_first_frame_still_seeds(const std::vector<float>& x, const std::vector<float>& y,
                                         const std::vector<float>& raw_x, const std::vector<float>& raw_y) {
    rtl_metrics_test_spectrum_reset();
    check("unpublished: all buffers held", rtl_metrics_test_spectrum_hold_spares(1) == 4);
    rtl_metrics_update_spectrum_from_iq(x.data(), (int)x.size(), kRateHz);
    rtl_metrics_update_spectrum_from_iq(y.data(), (int)y.size(), kRateHz);
    float bins[kBins];
    check("unpublished: no frame yet", rtl_stream_spectrum_get(bins, kBins, NULL) == 0);
    std::vector<float> want = raw_x;
    blend(&want, raw_y);
    check("unpublished: all buffers released", rtl_metrics_test_spectrum_hold_spares(0) == 4);
    rtl_metrics_update_spectrum_from_iq(x.data(), (int)x.size(), kRateHz);
    blend(&want, raw_x);
    check("unpublished: first publication carries the average", same_bins(read_bins("unpublished: read"), want));
}

struct competing_frames {
    const std::vector<float>* iq = nullptr;
    int attempts = 0;
};

void
publish_after_pin(void* ctx) {
    auto* frames = static_cast<competing_frames*>(ctx);
    frames->attempts++;
    rtl_metrics_update_spectrum_from_iq(frames->iq->data(), (int)frames->iq->size(), kRateHz);
}

void
test_competing_publications_keep_a_frame(const std::vector<float>& iq, const std::vector<float>& raw) {
    rtl_metrics_test_spectrum_reset();
    rtl_metrics_update_spectrum_from_iq(iq.data(), (int)iq.size(), kRateHz);
    /* Heap-held: the hook keeps its context in a global while it is set. */
    const auto frames_owner = std::make_unique<competing_frames>(competing_frames{&iq});
    competing_frames& frames = *frames_owner;
    rtl_metrics_test_spectrum_set_pin_hook(publish_after_pin, &frames);
    const std::vector<float> bins = read_bins("competing: always returns the published frame");
    rtl_metrics_test_spectrum_set_pin_hook(nullptr, nullptr);
    check("competing: bounded pinning", frames.attempts > 1 && frames.attempts <= 5);
    check("competing: complete bins", same_bins(bins, raw));
    check("competing: no leaked holds", rtl_metrics_test_spectrum_hold_count() == 0);
    rtl_metrics_update_spectrum_from_iq(iq.data(), (int)iq.size(), kRateHz);
    check("competing: publication resumes", same_bins(read_bins("competing: next frame"), raw));
    (void)rtl_stream_spectrum_set_size(kBins * 2);
    float enlarged[kBins * 2];
    const int published_bins = rtl_stream_spectrum_get(enlarged, kBins * 2, nullptr);
    check("size change: getter keeps the published bin count", published_bins == kBins);
    (void)rtl_stream_spectrum_set_size(kBins);
}

} // namespace

int
main(void) {
    test_noise_bin_copy_is_bit_exact();
    test_fold_is_bit_exact();
    (void)rtl_stream_spectrum_set_size(kBins);
    const std::vector<float> x = make_iq(37.0, 1U);
    const std::vector<float> y = make_iq(-91.0, 2U);
    const std::vector<float> raw_x = raw_frame(x);
    const std::vector<float> raw_y = raw_frame(y);
    check("raw frames differ", !same_bins(raw_x, raw_y));
    test_competing_publications_keep_a_frame(x, raw_x);
    test_average_carries_across_buffers(x, y, raw_x, raw_y);
    test_held_spares_fold_into_the_writers_own(x, y, raw_x, raw_y);
    test_unpublished_first_frame_still_seeds(x, y, raw_x, raw_y);
    test_size_change_reseeds();
    rtl_metrics_test_spectrum_reset();
    if (g_failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: spectrum frames\n");
    return 0;
}
