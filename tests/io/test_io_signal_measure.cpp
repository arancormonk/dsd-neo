// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <dsd-neo/core/power.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/signal_search.h>
#include <limits>
#include <random>
#include <stdint.h>
#include <stdlib.h>
#include <thread>
#include <vector>
#include "rtl_fft_cache.h"
#include "rtl_stream_test_support.h"

static std::vector<float>
samples(const dsd_signal_measure_request& r, double frequency, double amplitude, size_t count) {
    std::vector<float> iq(count * 2);
    uint32_t rng = 0x137abc;
    double omega = 6.283185307179586 * (frequency - r.capture.capture_hz + r.capture.rotation_hz) / r.capture.rate_hz;
    for (size_t i = 0; i < count; ++i) {
        for (int q = 0; q < 2; ++q) {
            rng = rng * 1664525 + 1013904223;
            double noise = ((double)rng / UINT32_MAX - 0.5) * 0.002;
            iq[2 * i + q] = (float)(noise + amplitude * (q ? std::sin(omega * i) : std::cos(omega * i)));
        }
    }
    return iq;
}

/* Add an NFM transmitter: carrier @p frequency, @p deviation Hz peak with a @p tone Hz sine. */
static void
add_nfm(std::vector<float>& iq, const dsd_signal_measure_request& r, double frequency, double amplitude,
        double deviation, double tone) {
    double phase = 0;
    for (size_t i = 0; i < iq.size() / 2; ++i) {
        const double offset = frequency - r.capture.capture_hz + r.capture.rotation_hz
                              + deviation * std::sin(6.283185307179586 * tone * i / r.capture.rate_hz);
        phase += 6.283185307179586 * offset / r.capture.rate_hz;
        iq[2 * i] += (float)(amplitude * std::cos(phase));
        iq[2 * i + 1] += (float)(amplitude * std::sin(phase));
    }
}

static dsd_signal_measure_report
run(dsd_signal_detector* d, const dsd_signal_measure_request& r, const std::vector<float>& iq, size_t block) {
    assert(dsd_signal_detector_begin(d, &r) == 0);
    dsd_signal_measure_report report{};
    int rc = 0;
    for (size_t i = 0; i < iq.size() / 2 && !rc; i += block) {
        size_t count = std::min(block, iq.size() / 2 - i);
        rc = dsd_signal_detector_feed(d, iq.data() + 2 * i, count, &r.capture, &report);
    }
    assert(rc == 1);
    return report;
}

/* Exactly the samples one request needs: two windows of whole transforms, each at least 10 ms. */
static size_t
request_samples(const dsd_signal_measure_request& r) {
    const size_t n = (size_t)dsd_signal_search_fft_size(r.capture.rate_hz, &r.range);
    return 2 * n * ((r.capture.rate_hz / 100 + n - 1) / n);
}

/* Add a noise-like FM transmitter: low-passed Gaussian modulation (1.5 kHz) clipped at @p deviation. */
static void
add_voice(std::vector<float>& iq, const dsd_signal_measure_request& r, double frequency, double amplitude,
          double deviation, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0, 1);
    const double a = std::exp(-6.283185307179586 * 1500 / r.capture.rate_hz);
    double phase = 0, m = 0;
    for (size_t i = 0; i < iq.size() / 2; ++i) {
        m = a * m + (1 - a) * gauss(rng) * 6.0;
        phase +=
            6.283185307179586
            * (frequency - r.capture.capture_hz + r.capture.rotation_hz + deviation * std::max(-1.0, std::min(1.0, m)))
            / r.capture.rate_hz;
        iq[2 * i] += (float)(amplitude * std::cos(phase));
        iq[2 * i + 1] += (float)(amplitude * std::sin(phase));
    }
}

namespace {
/* A transmitter in a scene: tone FM (a CW without deviation), or noise-like FM when @c voice is a seed. */
struct transmitter {
    double frequency, amplitude, deviation, tone;
    unsigned voice;
};
} // namespace

/* @p count channels @p step_num / @p step_den apart from @p first, captured at @p capture_hz. */
static dsd_signal_measure_request
grid_request(uint32_t rate, int32_t rotation, uint32_t capture_hz, uint32_t step_num, uint32_t width, uint32_t first,
             uint32_t count) {
    dsd_signal_measure_request g{};
    dsd_signal_search_range_default(&g.range);
    g.range.step_num = step_num;
    g.range.bandwidth_hz = width;
    g.session_id = 3;
    g.request_id = 4;
    g.capture.rate_hz = rate;
    g.capture.rotation_hz = rotation;
    g.capture.capture_hz = capture_hz;
    g.capture.generation = 1;
    g.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        g.frequencies[i] = (uint32_t)(first + (uint64_t)i * step_num);
    }
    return g;
}

/* The frequencies detected in a scene of @p transmitters over a quiet floor. */
static std::vector<uint32_t>
detected(dsd_signal_detector* d, const dsd_signal_measure_request& r, const std::vector<transmitter>& transmitters) {
    auto iq = samples(r, r.frequencies[0], 0, request_samples(r));
    for (const transmitter& t : transmitters) {
        if (t.voice) {
            add_voice(iq, r, t.frequency, t.amplitude, t.deviation, t.voice);
        } else {
            add_nfm(iq, r, t.frequency, t.amplitude, t.deviation, t.tone);
        }
    }
    const auto report = run(d, r, iq, 4096);
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (report.candidates[i].detected) {
            out.push_back(report.candidates[i].frequency_hz);
        }
    }
    return out;
}

/* The grid frequency of @p r nearest @p frequency. */
static uint32_t
nearest(const dsd_signal_measure_request& r, double frequency) {
    uint32_t best = r.frequencies[0];
    for (uint32_t i = 1; i < r.count; ++i) {
        if (std::abs(r.frequencies[i] - frequency) < std::abs(best - frequency)) {
            best = r.frequencies[i];
        }
    }
    return best;
}

/* A transmitter belongs to the grid channel nearest its centre, so it is reported once, there; a weaker transmitter
   beside a stronger one is reported too, on its own channel. */
static void
test_ownership(dsd_signal_detector* d) {
    // Finding 1: a genuine neighbour 6 dB down, 12.5 kHz beside 5 kHz deviation, on either side and at four bin
    // alignments of the 1.5 kHz bins (the stronger one's sidebands meet it with only a shallow valley).
    for (int side : {-1, 1}) {
        for (uint32_t shift : {0U, 375U, 750U, 1125U}) {
            const auto g = grid_request(1536000, 384000, 163450000 + shift, 12500, 16000, 162962500, 9);
            const auto found =
                detected(d, g, {{163012500, 0.3, 5000, 1000, 0}, {163012500 + side * 12500.0, 0.15, 2500, 700, 0}});
            const std::vector<uint32_t> want =
                side < 0 ? std::vector<uint32_t>{163000000, 163012500} : std::vector<uint32_t>{163012500, 163025000};
            assert(found == want);
        }
    }

    // Finding 2: one transmitter on a fine grid (5 and 6.25 kHz steps, 16 kHz windows) at several FFT resolutions and
    // bin alignments, with and without ingest rotation: its sidebands, carrier nulls and horns are its own.
    struct geometry {
        uint32_t rate;
        int32_t rotation;
        uint32_t capture;
    };

    const geometry geometries[] = {{1536000, 384000, 163450000},
                                   {1536000, 384000, 163450250},
                                   {2500000, 0, 162387833},
                                   {6000000, 0, 161512833},
                                   {10000000, 0, 160512833}};
    const transmitter modulations[] = {{0, 0.3, 2500, 1000, 0}, {0, 0.3, 2500, 2000, 0}, {0, 0.3, 5000, 2000, 0},
                                       {0, 0.3, 5000, 2400, 0}, {0, 0.3, 5000, 300, 0},  {0, 0.3, 5000, 0, 7}};
    for (uint32_t step : {5000U, 6250U}) {
        for (const geometry& geo : geometries) {
            const auto g = grid_request(geo.rate, geo.rotation, geo.capture, step, 16000, 163012500 - 4 * step, 9);
            for (transmitter t : modulations) {
                t.frequency = 163012500;
                assert(detected(d, g, {t}) == std::vector<uint32_t>{163012500});
            }
        }
    }
    // Finding 3: a carrier 6 kHz above a 12.5 kHz grid channel (inside its half step, between rounded bin positions),
    // and noise-like FM 2 kHz above a 5 kHz grid channel at four transforms a window.
    {
        const auto g = grid_request(1536000, 384000, 163450000, 12500, 16000, 162962500, 9);
        assert(detected(d, g, {{163018500, 0.3, 0, 1000, 0}}) == std::vector<uint32_t>{163012500});
        const auto fine = grid_request(1536000, 384000, 163450000, 5000, 16000, 162992500, 9);
        assert(detected(d, fine, {{163014500, 0.3, 5000, 0, 1}}) == std::vector<uint32_t>{163012500});
    }
    // Across ownership boundaries: exactly one detection everywhere, at the nearest channel except within a hair of a
    // tie; and on the boundary itself, give or take a hertz.
    for (uint32_t step : {5000U, 6250U, 12500U}) {
        const auto g = grid_request(1536000, 384000, 163450000, step, 16000, 163012500 - 4 * step, 9);
        for (int twentieth = -11; twentieth <= 11; ++twentieth) {
            const double frequency = 163012500 + twentieth * (double)step / 20;
            for (double deviation : {0.0, 2500.0}) {
                const auto found = detected(d, g, {{frequency, 0.3, deviation, 1000, 0}});
                assert(found.size() == 1);
                assert(std::abs(std::abs(twentieth) - 10) <= 1 || found[0] == nearest(g, frequency));
            }
        }
        for (double offset : {-1.0, 0.0, 1.0}) {
            assert(detected(d, g, {{163012500 + step / 2.0 + offset, 0.3, 0, 1000, 0}}).size() == 1);
        }
    }
    // A rational step's grid frequencies are rounded to whole hertz; neighbouring channels still share one boundary.
    {
        dsd_signal_measure_request g = grid_request(1536000, 384000, 100400000, 25000, 12500, 0, 7);
        g.range.start_hz = 100000000;
        g.range.end_hz = 101000000;
        g.range.step_den = 3;
        for (uint32_t i = 0; i < g.count; ++i) {
            g.frequencies[i] = dsd_signal_search_frequency(&g.range, i);
        }
        const double boundary = 100000000 + 2.5 * 25000.0 / 3;
        for (double offset : {-0.5, -0.25, 0.0, 0.25, 0.5}) {
            assert(detected(d, g, {{boundary + offset, 0.3, 0, 1000, 0}}).size() == 1);
        }
    }
    // A grid finer than an analysis bin (250 Hz steps, 25 kHz windows: 65536-point transforms, aggregated): each
    // carrier at its nearest channel, a comb of them one detection each, and one NFM transmitter once.
    {
        dsd_signal_measure_request g{};
        dsd_signal_search_range_default(&g.range);
        g.range.start_hz = 163000000;
        g.range.end_hz = 165000000;
        g.range.step_num = 250;
        g.range.bandwidth_hz = 25000;
        g.range.sensitivity_db = 3;
        g.session_id = 3;
        g.request_id = 4;
        g.capture.rate_hz = 1536000;
        g.capture.rotation_hz = g.capture.capture_offset_hz = 384000;
        g.capture.generation = 1;
        dsd_signal_search_span span{};
        assert(dsd_signal_search_plan(&g.range, 0, g.capture.rate_hz, g.capture.capture_offset_hz, &span) == 1);
        assert(span.count == DSD_SEARCH_MAX_CANDIDATES);
        g.count = span.count;
        g.capture.capture_hz = (uint32_t)(span.tune_hz + g.capture.capture_offset_hz);
        for (uint32_t i = 0; i < g.count; ++i) {
            g.frequencies[i] = span.frequencies[i];
        }
        const uint32_t middle_index = g.count / 2;
        const double middle = g.frequencies[middle_index];
        for (double offset : {0.0, 60.0, 250.0, 375.0, -1100.0}) {
            const auto found = detected(d, g, {{middle + offset, 0.015, 0, 1000, 0}});
            assert(found.size() == 1 && std::abs(found[0] - (middle + offset)) <= 125);
        }
        std::vector<transmitter> comb;
        for (uint32_t f = g.frequencies[0]; f <= g.frequencies[g.count - 1]; f += 25000) {
            comb.push_back({(double)f, 0.015, 0, 1000, 0});
        }
        const auto found = detected(d, g, comb);
        assert(found.size() == comb.size());
        for (size_t i = 0; i < comb.size(); ++i) {
            assert(found[i] == (uint32_t)comb[i].frequency);
        }
        assert(detected(d, g, {{middle, 0.3, 5000, 1000, 0}}) == std::vector<uint32_t>{(uint32_t)middle});
        // Refined on the FFT bins, a tone FM transmitter whose first sidebands outweigh its carrier is centred on its
        // carrier, not on one of those sidebands three channels away.
        assert(detected(d, g, {{163064000, 0.3, 1500, 700, 0}}) == std::vector<uint32_t>{163064000});
    }
    // A transmitter's own sideband lines, decaying one after another, are not neighbours: not where its first
    // sideband's envelope reaches past its own width (250 Hz grid, 8 kHz windows), nor at a weak outer line beyond a
    // stronger one (5 kHz grid, 25 kHz windows).
    {
        dsd_signal_measure_request g{};
        dsd_signal_search_range_default(&g.range);
        g.range.start_hz = 163000000;
        g.range.end_hz = 165000000;
        g.range.step_num = 250;
        g.range.bandwidth_hz = 8000;
        g.range.sensitivity_db = 3;
        g.session_id = 3;
        g.request_id = 4;
        g.capture.rate_hz = 1536000;
        g.capture.rotation_hz = g.capture.capture_offset_hz = 384000;
        g.capture.generation = 1;
        dsd_signal_search_span span{};
        assert(dsd_signal_search_plan(&g.range, 0, g.capture.rate_hz, g.capture.capture_offset_hz, &span) == 1);
        g.count = span.count;
        g.capture.capture_hz = (uint32_t)(span.tune_hz + g.capture.capture_offset_hz);
        for (uint32_t i = 0; i < g.count; ++i) {
            g.frequencies[i] = span.frequencies[i];
        }
        assert(
            (detected(d, g, {{163030103, 0.0669, 2930, 2948.5, 0}}) == std::vector<uint32_t>{nearest(g, 163030103)}));
        const auto wide = grid_request(1536000, 384000, 163450000, 5000, 25000, 162992500, 9);
        assert(detected(d, wide, {{163013200, 0.3, 5000, 3000, 0}}) == std::vector<uint32_t>{163012500});
    }
    // A transmitter whose window wraps FFT bin 0 (just above the capture less the ingest rotation), one below the
    // capture without rotation, and channels just inside the DC and edge guards.
    {
        const auto wrapped = grid_request(1536000, 384000, 163450000, 12500, 16000, 163066000 - 4 * 12500, 9);
        assert(detected(d, wrapped, {{163066000, 0.3, 2500, 1000, 0}}) == std::vector<uint32_t>{163066000});
        const auto below = grid_request(2500000, 0, 163312500, 12500, 16000, 163012500 - 4 * 12500, 9);
        assert(detected(d, below, {{163012500, 0.3, 2500, 1000, 0}}) == std::vector<uint32_t>{163012500});
        // Valid channels are more than 1% of the rate plus three widths from the capture centre, and three widths
        // inside 45% of the rate.
        const uint32_t dc = 163450000 - (15360 + 3 * 16000 + 500);
        const auto near_dc = grid_request(1536000, 384000, 163450000, 12500, 16000, dc - 4 * 12500, 5);
        assert(detected(d, near_dc, {{(double)dc, 0.3, 2500, 1000, 0}}) == std::vector<uint32_t>{dc});
        const uint32_t edge = 163450000 - (691200 - 3 * 16000 - 500);
        const auto near_edge = grid_request(1536000, 384000, 163450000, 12500, 16000, edge, 5);
        assert(detected(d, near_edge, {{(double)edge, 0.3, 2500, 1000, 0}}) == std::vector<uint32_t>{edge});
    }
}

/* Neighbours: a transmitter's presence never hides another's, and two of them never invent one between. */
static void
test_neighbours(dsd_signal_detector* d) {
    // Transmitters two and three channels apart: neither one's noise sidebands hide the other, and the empty channel
    // between them reports nothing.
    const auto g = grid_request(1536000, 384000, 163450000, 12500, 16000, 162950000, 13);
    for (int apart : {2, 3}) {
        for (double weaker : {0.3, 0.15}) {
            const double other = 163012500 + apart * 12500.0;
            assert((detected(d, g, {{163012500, 0.3, 2500, 1000, 0}, {other, weaker, 2500, 700, 0}})
                    == std::vector<uint32_t>{163012500, (uint32_t)other}));
        }
    }
    // Equal transmitters 10 kHz apart on a 5 kHz grid, and equal carriers 5 kHz apart on a 2.5 kHz grid: the
    // envelope that bridges an FM transmitter's sideband lines bridges them too, but neither the gap between them
    // nor its channel is a transmitter.
    const auto fine = grid_request(1536000, 384000, 163450000, 5000, 16000, 162982500, 13);
    assert((detected(d, fine, {{163012500, 0.3, 2500, 1000, 0}, {163022500, 0.3, 2500, 700, 0}})
            == std::vector<uint32_t>{163012500, 163022500}));
    const auto finer = grid_request(1536000, 384000, 163450000, 2500, 16000, 162997500, 13);
    assert((detected(d, finer, {{163010000, 0.3, 0, 1000, 0}, {163015000, 0.3, 0, 1000, 0}})
            == std::vector<uint32_t>{163010000, 163015000}));
    // A transmitter whose modulation is lopsided (1 kHz times sin 700 Hz plus cos 1400 Hz) stands far above its own
    // mirror on one side, but its skirt only falls: no neighbour.
    {
        const auto lopsided = grid_request(1536000, 384000, 163450000, 2500, 8000, 162962500, 41);
        auto iq = samples(lopsided, lopsided.frequencies[0], 0, request_samples(lopsided));
        double phase = 0;
        for (size_t i = 0; i < iq.size() / 2; ++i) {
            const double t = (double)i / lopsided.capture.rate_hz;
            const double deviation =
                1000.0 * (std::sin(6.283185307179586 * 700 * t) + std::cos(6.283185307179586 * 1400 * t));
            phase += 6.283185307179586
                     * (163012500.0 - lopsided.capture.capture_hz + lopsided.capture.rotation_hz + deviation)
                     / lopsided.capture.rate_hz;
            iq[2 * i] += (float)(0.3 * std::cos(phase));
            iq[2 * i + 1] += (float)(0.3 * std::sin(phase));
        }
        const auto report = run(d, lopsided, iq, 4096);
        for (uint32_t i = 0; i < lopsided.count; ++i) {
            assert(report.candidates[i].detected == (lopsided.frequencies[i] == 163012500));
        }
    }
    // A wide FM transmitter with two tones (5 kHz times sin 500 Hz plus sin 1000 Hz) has lobes either side of a
    // valley; one transmitter, at its carrier, not its lobes' channels.
    {
        const auto lobed = grid_request(1536000, 384000, 163450000, 12500, 25000, 162962500, 9);
        auto iq = samples(lobed, lobed.frequencies[0], 0, request_samples(lobed));
        double phase = 0;
        for (size_t i = 0; i < iq.size() / 2; ++i) {
            const double t = (double)i / lobed.capture.rate_hz;
            const double deviation =
                5000.0 * (std::sin(6.283185307179586 * 500 * t) + std::sin(6.283185307179586 * 1000 * t));
            phase += 6.283185307179586
                     * (163012500.0 - lobed.capture.capture_hz + lobed.capture.rotation_hz + deviation)
                     / lobed.capture.rate_hz;
            iq[2 * i] += (float)(0.3 * std::cos(phase));
            iq[2 * i + 1] += (float)(0.3 * std::sin(phase));
        }
        const auto report = run(d, lobed, iq, 4096);
        for (uint32_t i = 0; i < lobed.count; ++i) {
            assert(report.candidates[i].detected == (lobed.frequencies[i] == 163012500));
        }
    }
    // A carrier 6 dB down 5 kHz beside a stronger one, within its 20 kHz window: unmirrored, beyond a deep valley.
    const auto close = grid_request(1536000, 384000, 163450000, 2500, 20000, 163002500, 13);
    assert((detected(d, close, {{163012500, 0.3, 0, 1000, 0}, {163017500, 0.15, 0, 1000, 0}})
            == std::vector<uint32_t>{163012500, 163017500}));
    // A weak neighbour exactly on the boundary between two 5 kHz channels belongs to one of them.
    assert(detected(d, fine, {{163012500, 0.3, 2500, 1000, 0}, {163025000, 0.03, 2500, 700, 0}}).size() == 2);
    // Two weaker transmitters either side of a strong one on a fine grid, 10 kHz out.
    assert(
        (detected(d, fine,
                  {{163012500, 0.3, 2500, 1000, 0}, {163002500, 0.03, 2500, 700, 0}, {163022500, 0.03, 2500, 900, 0}})
         == std::vector<uint32_t>{163002500, 163012500, 163022500}));
    // With an 8 kHz window on a 25 kHz grid, a wide transmitter 11.25 kHz off its channel lies outside every window
    // but its sidebands reach in: at most one detection, at its nearest channel.
    const auto sparse = grid_request(1536000, 384000, 163450000, 25000, 8000, 162912500, 9);
    const auto found = detected(d, sparse, {{163023750, 0.3, 5000, 2400, 0}});
    assert(found.empty() || found == std::vector<uint32_t>{163012500});
}

/* Sustained energy: each window must find the transmitter itself, not another one's skirt that only one window has;
   and a transmitter whose modulation differs between the windows is still sustained. */
static void
test_sustained(dsd_signal_detector* d) {
    struct alternation {
        uint32_t step, first;
        transmitter one, two;
    };

    const alternation cases[] = {
        {12500, 162962500, {163012500, 0.3, 2500, 1000, 0}, {163025000, 0.3, 2500, 1000, 0}},
        {5000, 162982500, {163002500, 0.3, 2500, 1000, 0}, {163022500, 0.3, 2500, 700, 0}},
    };
    for (const alternation& c : cases) {
        const auto g = grid_request(1536000, 384000, 163450000, c.step, 16000, c.first, 9);
        const size_t half = request_samples(g) / 2;
        auto first = samples(g, g.frequencies[0], 0, 2 * half), second = first;
        add_nfm(first, g, c.one.frequency, c.one.amplitude, c.one.deviation, c.one.tone);
        add_nfm(second, g, c.two.frequency, c.two.amplitude, c.two.deviation, c.two.tone);
        std::vector<float> iq(first.begin(), first.begin() + (std::ptrdiff_t)(2 * half));
        iq.insert(iq.end(), second.begin() + (std::ptrdiff_t)(2 * half), second.end());
        const auto report = run(d, g, iq, 4096);
        for (uint32_t i = 0; i < g.count; ++i) {
            assert(!report.candidates[i].detected);
        }
    }
    // One transmitter whose slow tone puts each window on a different side of its carrier is sustained, even where
    // each window's power lies in another channel (45 Hz at 5 kHz deviation on a 6.25 kHz grid).
    const auto slow = grid_request(1536000, 384000, 163450000, 5000, 16000, 162992500, 9);
    assert(detected(d, slow, {{163012500, 0.3, 2500, 50, 0}}) == std::vector<uint32_t>{163012500});
    const auto slower = grid_request(1536000, 384000, 163450000, 6250, 16000, 162987500, 9);
    assert(detected(d, slower, {{163012500, 0.3, 5000, 45, 0}}) == std::vector<uint32_t>{163012500});
    // A clipped slow tone dwells on one side of its carrier at a time. Measured where each window spans a change of
    // side (a quarter cycle on), it holds its power in both windows and is sustained.
    {
        const auto clipped = grid_request(1536000, 384000, 163450000, 2500, 8000, 163012500 - 8 * 2500, 17);
        auto iq = samples(clipped, clipped.frequencies[0], 0, request_samples(clipped));
        double phase = 0;
        for (size_t i = 0; i < iq.size() / 2; ++i) {
            const double t = (double)i / clipped.capture.rate_hz;
            const double deviation = 1000.0 * std::max(-0.7, std::min(0.7, 2.0 * std::cos(6.283185307179586 * 45 * t)));
            phase += 6.283185307179586
                     * (163012500.0 - clipped.capture.capture_hz + clipped.capture.rotation_hz + deviation)
                     / clipped.capture.rate_hz;
            iq[2 * i] += (float)(0.3 * std::cos(phase));
            iq[2 * i + 1] += (float)(0.3 * std::sin(phase));
        }
        const auto report = run(d, clipped, iq, 4096);
        for (uint32_t i = 0; i < clipped.count; ++i) {
            assert(report.candidates[i].detected == (clipped.frequencies[i] == 163012500));
        }
    }
    // Measured where the windows fall on its two dwells, the same transmitter matches, window for window, two carriers
    // 700 Hz either side of 163.0125 MHz that each fill one window: neither holds its power in both windows, so
    // nothing is sustained. A later measurement at another phase finds the transmitter.
    {
        const auto pair = grid_request(1536000, 384000, 163450000, 2500, 8000, 163012500 - 8 * 2500, 17);
        const size_t half = request_samples(pair) / 2;
        auto iq = samples(pair, pair.frequencies[0], 0, 2 * half);
        for (size_t i = 0; i < 2 * half; ++i) {
            const double offset = i < half ? 700.0 : -700.0;
            const double omega = 6.283185307179586
                                 * (163012500.0 + offset - pair.capture.capture_hz + pair.capture.rotation_hz)
                                 / pair.capture.rate_hz;
            iq[2 * i] += (float)(0.3 * std::cos(omega * (double)i));
            iq[2 * i + 1] += (float)(0.3 * std::sin(omega * (double)i));
        }
        const auto report = run(d, pair, iq, 4096);
        for (uint32_t i = 0; i < pair.count; ++i) {
            assert(!report.candidates[i].detected);
        }
    }
    // A carrier in the first window only, a channel beside a continuous transmitter whose skirt reaches its channel in
    // both, is not sustained: the neighbour's power is not the carrier's.
    {
        const auto beside = grid_request(1536000, 384000, 163450000, 6250, 8000, 163012500 - 8 * 6250, 17);
        auto iq = samples(beside, beside.frequencies[0], 0, request_samples(beside));
        add_nfm(iq, beside, 163018750, 0.3, 2500, 700);
        const double omega = 6.283185307179586 * (163012500.0 - beside.capture.capture_hz + beside.capture.rotation_hz)
                             / beside.capture.rate_hz;
        for (size_t i = 0; i < request_samples(beside) / 2; ++i) {
            iq[2 * i] += (float)(0.075 * std::cos(omega * (double)i));
            iq[2 * i + 1] += (float)(0.075 * std::sin(omega * (double)i));
        }
        const auto report = run(d, beside, iq, 4096);
        for (uint32_t i = 0; i < beside.count; ++i) {
            assert(report.candidates[i].detected == (beside.frequencies[i] == 163018750));
        }
    }
}

/* Noise alone at the lowest sensitivity, white or coloured, reports nothing; and the robust local noise reads the
   level of a plain mean over noise alone. */
static void
test_noise(dsd_signal_detector* d) {
    for (uint32_t step : {12500U, 250U}) {
        dsd_signal_measure_request g{};
        dsd_signal_search_range_default(&g.range);
        g.range.start_hz = 163000000;
        g.range.end_hz = 165000000;
        g.range.step_num = step;
        g.range.bandwidth_hz = step == 250 ? 25000 : 16000;
        g.range.sensitivity_db = 3;
        g.session_id = 3;
        g.request_id = 4;
        g.capture.rate_hz = 1536000;
        g.capture.rotation_hz = g.capture.capture_offset_hz = 384000;
        g.capture.generation = 1;
        dsd_signal_search_span span{};
        assert(dsd_signal_search_plan(&g.range, 0, g.capture.rate_hz, g.capture.capture_offset_hz, &span) == 1);
        g.count = span.count;
        g.capture.capture_hz = (uint32_t)(span.tune_hz + g.capture.capture_offset_hz);
        for (uint32_t i = 0; i < g.count; ++i) {
            g.frequencies[i] = span.frequencies[i];
        }
        for (unsigned seed = 1; seed <= 4; ++seed) {
            for (int coloured = 0; coloured < 2; ++coloured) {
                std::vector<float> iq(2 * request_samples(g));
                std::mt19937 rng(seed);
                std::normal_distribution<float> gauss(0, 0.001f);
                for (float& v : iq) {
                    v = gauss(rng);
                }
                for (size_t i = 2; coloured && i < iq.size(); ++i) {
                    iq[i] = 0.8f * iq[i - 2] + 0.2f * iq[i];
                }
                const auto report = run(d, g, iq, 8192);
                double snr = 0;
                for (uint32_t i = 0; i < g.count; ++i) {
                    assert(report.candidates[i].valid && !report.candidates[i].detected);
                    snr += report.candidates[i].power_dbfs - report.candidates[i].noise_dbfs;
                }
                assert(coloured || std::abs(snr / g.count) < 0.5);
            }
        }
    }
}

/* The shared tap publishes a report for the request it was made for, or nothing. */
static void
test_tap_schedules(const dsd_signal_measure_request& r, const std::vector<float>& iq) {
    const size_t pairs = iq.size() / 2;
    dsd_signal_measure_report report{};
    // A request made before the last report was taken retires that report; the next one is its own.
    dsd_signal_measure_request first = r, second = r;
    first.request_id = 101;
    second.request_id = 102;
    assert(rtl_signal_measure_request(&first, nullptr) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_request(&second, nullptr) == 0);
    assert(rtl_signal_measure_get(&report) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 1 && report.request_id == 102);
    assert(rtl_signal_measure_get(&report) == 0);
    // A cancel retires the request: the rest of its samples publish nothing.
    assert(rtl_signal_measure_request(&first, nullptr) == 0);
    rtl_signal_measure_feed(iq.data(), pairs / 4, &r.capture);
    rtl_signal_measure_cancel();
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 0);
    // A completed report taken before a capture change is still delivered; one left waiting is invalidated.
    assert(rtl_signal_measure_request(&first, nullptr) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 1 && report.request_id == 101);
    auto moved = r.capture;
    ++moved.drops;
    rtl_signal_measure_feed(iq.data(), 1, &moved);
    assert(rtl_signal_measure_get(&report) == 0);
    rtl_signal_measure_cancel();
}

/* The demod thread feeding while the decoder thread requests, polls and cancels: every report delivered, complete or
   failed, is for the request outstanding, a cancel leaves nothing to deliver, and polling the capture never waits for
   analysis. The demod loops over three passes of clean samples and one that clips, so requests both complete and
   fail. */
static void
test_tap_threads(const dsd_signal_measure_request& r, const std::vector<float>& clean) {
    std::vector<float> iq;
    for (int pass = 0; pass < 3; ++pass) {
        iq.insert(iq.end(), clean.begin(), clean.end());
    }
    const auto clipping = samples(r, r.frequencies[0], 1.2, clean.size() / 2);
    iq.insert(iq.end(), clipping.begin(), clipping.end());
    std::atomic<bool> stop{false};
    std::thread demod([&] {
        const size_t block = 4096;
        for (size_t at = 0; !stop.load(std::memory_order_acquire); at = (at + block) % (iq.size() / 2 - block)) {
            rtl_signal_measure_feed(iq.data() + 2 * at, block, &r.capture);
        }
    });
    dsd_signal_measure_request request = r;
    int delivered = 0, failed = 0;
    for (uint64_t id = 1; id <= 60; ++id) {
        request.request_id = 1000 + id;
        assert(rtl_signal_measure_request(&request, nullptr) == 0);
        if (id % 4 == 0) {
            rtl_signal_measure_cancel();
            dsd_signal_measure_report none{};
            assert(rtl_signal_measure_get(&none) == 0);
            continue;
        }
        for (int poll = 0; poll < 2000000; ++poll) {
            dsd_signal_measure_report report{};
            dsd_signal_capture capture{};
            assert(rtl_signal_capture_get(&capture) == 1 && capture.rate_hz == r.capture.rate_hz);
            const int rc = rtl_signal_measure_get(&report);
            if (rc != 0) {
                assert(report.session_id == request.session_id && report.request_id == request.request_id);
                assert(rc == 1 ? report.failure == DSD_SIGNAL_MEASURE_OK
                               : rc == -1 && report.failure == DSD_SIGNAL_MEASURE_CLIPPED && report.count == 0);
                ++(rc == 1 ? delivered : failed);
                break;
            }
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    demod.join();
    rtl_signal_measure_cancel();
    assert(delivered > 0 && failed > 0);
}

/* A refused request says why: no detector could be allocated, or the plan is one it does not support. */
static void
test_request_failures(const dsd_signal_measure_request& r) {
    dsd_signal_request_failure failure = DSD_SIGNAL_REQUEST_OK;
    rtl_signal_measure_test_fail_allocation(1);
    assert(rtl_signal_measure_request(&r, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_NO_MEMORY);
    rtl_signal_measure_test_fail_allocation(0);
    auto unsupported = r;
    unsupported.capture.rate_hz = 0;
    assert(rtl_signal_measure_request(&unsupported, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    assert(rtl_signal_measure_request(&r, &failure) == 0 && failure == DSD_SIGNAL_REQUEST_OK);
    assert(rtl_signal_measure_request(&r, nullptr) == 0);
    rtl_signal_measure_cancel();
}

/* A failed measurement names its request and says why, with no candidates. */
static void
assert_failure(const dsd_signal_measure_report& report, const dsd_signal_measure_request& r,
               dsd_signal_measure_failure reason) {
    assert(report.failure == reason);
    assert(report.session_id == r.session_id && report.request_id == r.request_id);
    assert(report.generation == r.capture.generation && report.count == 0);
}

/* The detector's failures, each with its reason and its request's ids. */
static void
test_failure_reasons(dsd_signal_detector* d, const dsd_signal_measure_request& r) {
    const size_t window = request_samples(r) / 2;
    dsd_signal_measure_report report{};
    // Clipped from the start: the first window's check fails it.
    const auto clipping = samples(r, r.frequencies[0], 1.2, 30000);
    assert(dsd_signal_detector_begin(d, &r) == 0);
    assert(dsd_signal_detector_feed(d, clipping.data(), 30000, &r.capture, &report) == -1);
    assert_failure(report, r, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(report.sample_count == window);
    // A clean first window, then a second clipped enough to take the total over 1%: the second window's check.
    auto late = samples(r, r.frequencies[0], 0.1, 2 * window);
    for (size_t i = window; i < 2 * window; i += 25) {
        late[2 * i] = 1.0f;
    }
    assert(dsd_signal_detector_begin(d, &r) == 0);
    assert(dsd_signal_detector_feed(d, late.data(), window, &r.capture, &report) == 0);
    assert(dsd_signal_detector_feed(d, late.data() + 2 * window, window, &r.capture, &report) == -1);
    assert_failure(report, r, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(report.sample_count == 2 * window);
    // A non-finite sample.
    auto poisoned = samples(r, r.frequencies[0], 0.1, 3000);
    poisoned[2 * 1500 + 1] = std::numeric_limits<float>::quiet_NaN();
    assert(dsd_signal_detector_begin(d, &r) == 0);
    assert(dsd_signal_detector_feed(d, poisoned.data(), 3000, &r.capture, &report) == -1);
    assert_failure(report, r, DSD_SIGNAL_MEASURE_NONFINITE);
    // Dropped samples alone, and dropped samples with another change, which is a changed capture.
    for (int retune = 0; retune < 2; ++retune) {
        auto capture = r.capture;
        ++capture.drops;
        capture.capture_hz += (uint32_t)retune;
        assert(dsd_signal_detector_begin(d, &r) == 0);
        assert(dsd_signal_detector_feed(d, poisoned.data(), 1000, &r.capture, &report) == 0);
        assert(dsd_signal_detector_feed(d, poisoned.data(), 1000, &capture, &report) == -1);
        assert_failure(report, r, retune ? DSD_SIGNAL_MEASURE_CAPTURE_CHANGED : DSD_SIGNAL_MEASURE_DROPPED);
        assert(report.sample_count == 1000);
    }
    // A complete report says it is one.
    assert(run(d, r, samples(r, r.frequencies[0], 0.1, 30000), 4096).failure == DSD_SIGNAL_MEASURE_OK);
}

/* The shared tap hands a failed request's report over with its reason and ids, and keeps the first reason. */
static void
test_tap_failures(const dsd_signal_measure_request& r, const std::vector<float>& clean) {
    const auto clipping = samples(r, r.frequencies[0], 1.2, 30000);
    dsd_signal_measure_report report{};
    rtl_signal_measure_feed(clean.data(), 1, &r.capture);
    assert(rtl_signal_measure_request(&r, nullptr) == 0);
    rtl_signal_measure_feed(clipping.data(), 30000, &r.capture);
    // A capture change after the failure does not replace its reason.
    auto moved = r.capture;
    ++moved.gain;
    rtl_signal_measure_feed(clean.data(), 1, &moved);
    assert(rtl_signal_measure_get(&report) == -1);
    assert_failure(report, r, DSD_SIGNAL_MEASURE_CLIPPED);
    assert(report.received_realtime_ns != 0);
    assert(rtl_signal_measure_get(&report) == 0);
    // A completed report left waiting through a capture change is that request's failure, with its ids.
    rtl_signal_measure_feed(clean.data(), 1, &r.capture);
    assert(rtl_signal_measure_request(&r, nullptr) == 0);
    rtl_signal_measure_feed(clean.data(), 30000, &r.capture);
    ++moved.gain;
    rtl_signal_measure_feed(clean.data(), 1, &moved);
    assert(rtl_signal_measure_get(&report) == -1);
    assert_failure(report, r, DSD_SIGNAL_MEASURE_CAPTURE_CHANGED);
    assert(report.sample_count == request_samples(r));
    rtl_signal_measure_cancel();
}

/* @p code as one component of a raw pair, widened by the cu8 conversion @p c at Fs/4 phase @p phase (0 leaves the pair,
   2 mirrors both components) when the capture carries the Fs/4 shift @p fs4. */
static void
widen_code(const rtl_device_test_conversion& c, int fs4, int phase, unsigned char code, int component, float* out) {
    unsigned char raw[8] = {128U, 128U, 128U, 128U, 128U, 128U, 128U, 128U};
    float widened[8];
    const size_t at = 2U * (size_t)phase; // the pair's first component
    raw[at + (size_t)component] = code;
    assert(rtl_device_test_widen_cu8(&c, fs4, raw, at + 2U, widened) == 0);
    out[0] = widened[at];
    out[1] = widened[at + 1U];
}

/* The clip level each source sets: a cu8 conversion's rails count and the codes next to them do not, except byte 1 of
   the two-pass widening, as far from centre as byte 255 there, which counts with byte 0. That byte is the one after the
   two-pass rotation, which mirrors bytes (255 - x): at phase 2 a raw 254 becomes it and counts, and a raw 1 does not.
   A clean tone just under a level measures; one over it clips. */
static void
test_clip_levels(dsd_signal_detector* d, const dsd_signal_measure_request& r) {
    using conversion = rtl_device_test_conversion;
    const conversion usb{RTL_DEVICE_TEST_BACKEND_USB, 1, 0, 0, 0};
    const conversion tcp{RTL_DEVICE_TEST_BACKEND_TCP, 1, 0, 0, 0};
    const conversion usb_two_pass{RTL_DEVICE_TEST_BACKEND_USB, 0, 0, 0, 0};
    const conversion tcp_two_pass{RTL_DEVICE_TEST_BACKEND_TCP, 0, 0, 0, 0};
    const conversion replay{RTL_DEVICE_TEST_BACKEND_IQ_REPLAY, 1, DSD_IQ_FORMAT_CU8, 1, 0};
    const conversion replay_two_pass{RTL_DEVICE_TEST_BACKEND_IQ_REPLAY, 1, DSD_IQ_FORMAT_CU8, 1, 1};
    const conversion replay_cf32{RTL_DEVICE_TEST_BACKEND_IQ_REPLAY, 1, DSD_IQ_FORMAT_CF32, 1, 0};
    const conversion replay_cs16{RTL_DEVICE_TEST_BACKEND_IQ_REPLAY, 1, DSD_IQ_FORMAT_CS16, 1, 0};
    const conversion airspy{RTL_DEVICE_TEST_BACKEND_AIRSPY, 1, 0, 0, 0};
    const conversion soapy{RTL_DEVICE_TEST_BACKEND_SOAPY, 1, 0, 0, 0};

    const struct {
        const conversion* c;
        int fs4;
        float level;
    } levels[] = {
        {&usb, 1, 0.996f},
        {&usb, 0, 0.996f},
        {&tcp, 1, 0.996f},
        {&usb_two_pass, 1, 0.992f},
        {&tcp_two_pass, 1, 0.992f},
        {&usb_two_pass, 0, 0.996f}, // without the shift nothing rotates: the bias-127.5 widening
        {&replay, 0, 0.996f},
        {&replay_two_pass, 0, 0.992f}, // a replay keeps its recorded shift
        {&replay_cf32, 1, DSD_SIGNAL_CLIP_LEVEL_DEFAULT},
        {&replay_cs16, 1, DSD_SIGNAL_CLIP_LEVEL_DEFAULT},
        {&airspy, 1, DSD_SIGNAL_CLIP_LEVEL_DEFAULT},
        {&soapy, 1, DSD_SIGNAL_CLIP_LEVEL_DEFAULT},
        {nullptr, 1, DSD_SIGNAL_CLIP_LEVEL_DEFAULT},
    };

    for (const auto& l : levels) {
        assert(std::abs(rtl_device_test_signal_clip_level(l.c, l.fs4) - l.level) < 1e-6f);
    }

    // Each cu8 conversion's rails and their neighbours, in either component, at 2% of the pairs: counted fails.
    const struct {
        const conversion* c;
        int fs4, phase;
        bool counted[4]; // raw bytes 0, 1, 254, 255
    } conversions[] = {
        {&usb, 1, 0, {true, false, false, true}},
        {&usb, 1, 2, {true, false, false, true}},
        {&usb, 0, 0, {true, false, false, true}},
        {&tcp, 1, 0, {true, false, false, true}},
        {&usb_two_pass, 1, 0, {true, true, false, true}},
        {&usb_two_pass, 1, 2, {true, false, true, true}},
        {&replay_two_pass, 0, 0, {true, true, false, true}},
        {&replay_two_pass, 0, 2, {true, false, true, true}},
    };

    const unsigned char codes[4] = {0U, 1U, 254U, 255U};
    for (const auto& k : conversions) {
        dsd_signal_measure_request q = r;
        q.capture.clip_level = rtl_device_test_signal_clip_level(k.c, k.fs4);
        for (int code = 0; code < 4; ++code) {
            for (int component = 0; component < 2; ++component) {
                float pair[2];
                widen_code(*k.c, k.fs4, k.phase, codes[code], component, pair);
                auto peaks = samples(q, q.frequencies[0], 0.1, 30000);
                for (size_t i = 0; i < peaks.size() / 2; i += 50) {
                    peaks[2 * i] = pair[0];
                    peaks[2 * i + 1] = pair[1];
                }
                dsd_signal_measure_report report{};
                assert(dsd_signal_detector_begin(d, &q) == 0);
                const int rc = dsd_signal_detector_feed(d, peaks.data(), 30000, &q.capture, &report);
                assert(rc == (k.counted[code] ? -1 : 1));
                assert(report.failure == (k.counted[code] ? DSD_SIGNAL_MEASURE_CLIPPED : DSD_SIGNAL_MEASURE_OK));
            }
        }
    }
    // A clean tone peaking just under each level measures and is detected; a level just under its peak clips it. A
    // capture that sets no level (0) has the default one.
    for (float level : {0.0f, DSD_SIGNAL_CLIP_LEVEL_DEFAULT, 0.996f, 0.992f}) {
        const float effective = level > 0.0f ? level : DSD_SIGNAL_CLIP_LEVEL_DEFAULT;
        dsd_signal_measure_request q = r;
        q.capture.clip_level = level;
        const auto tone = samples(q, q.frequencies[0], effective - 0.0025, 30000);
        assert(run(d, q, tone, 4096).candidates[0].detected);
        q.capture.clip_level = effective - 0.004f;
        dsd_signal_measure_report report{};
        assert(dsd_signal_detector_begin(d, &q) == 0);
        assert(dsd_signal_detector_feed(d, tone.data(), 30000, &q.capture, &report) == -1);
        assert_failure(report, q, DSD_SIGNAL_MEASURE_CLIPPED);
    }
}

/* Scan quiet measurement: one landed channel, nine sub-windows, judged in each requested width class. */
namespace {
constexpr uint32_t kQuietChannel = 163012500;
constexpr double kTwoPi = 6.283185307179586;
constexpr float kQuietSigma = 0.01f;
/* The behaviour tests judge with their own threshold, not the shipped one (whose noise-only false-busy rate is up to
   5% by calibration, too often for a fixed-seed test): 8 dB is clear of every noise-only scene here (at most about
   6 dB, at 5 kHz and 2.4 Msps with the lowest block, the estimator that reads noise highest), and every transmitter
   here is at least 12 dB over its noise. The floor guard is out of reach (100 dB) so these scenes judge the ratio
   alone; test_quiet_floor_guard() covers the guard. */
constexpr dsd_signal_quiet_params kQuietTestParams = {8.0f, DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, 100.0f};

/* A request for the channel landed Fs/4 below the tuner centre, as the RTL chain lands it. */
dsd_signal_quiet_request
quiet_request(uint32_t rate, uint8_t classes, uint32_t analog_width = 0) {
    dsd_signal_quiet_request q{};
    q.request_id = 41;
    q.capture.rate_hz = rate;
    q.capture.rotation_hz = (int32_t)(rate / 4);
    q.capture.capture_offset_hz = rate / 4;
    q.capture.capture_hz = kQuietChannel + rate / 4;
    q.capture.generation = 3;
    q.frequency_hz = kQuietChannel;
    q.classes = classes;
    q.analog_width_hz = analog_width;
    return q;
}

double
quiet_narrowest(const dsd_signal_quiet_request& q) {
    double width = 1e9;
    if (q.classes & DSD_SIGNAL_QUIET_CLASS_12K5) {
        width = std::min(width, (double)DSD_SIGNAL_QUIET_CLASS_12K5_HZ);
    }
    if (q.classes & DSD_SIGNAL_QUIET_CLASS_6K25) {
        width = std::min(width, (double)DSD_SIGNAL_QUIET_CLASS_6K25_HZ);
    }
    if (q.classes & DSD_SIGNAL_QUIET_CLASS_ANALOG) {
        width = std::min(width, (double)q.analog_width_hz);
    }
    return width;
}

size_t
quiet_fft_size(const dsd_signal_quiet_request& q) {
    return (size_t)dsd_signal_fft_size_for_width(q.capture.rate_hz, quiet_narrowest(q));
}

/* One sub-window: the smallest whole number of transforms of at least 10 ms. */
size_t
quiet_window_pairs(const dsd_signal_quiet_request& q) {
    const size_t n = quiet_fft_size(q);
    return n * ((q.capture.rate_hz / 100 + n - 1) / n);
}

size_t
quiet_request_pairs(const dsd_signal_quiet_request& q) {
    return DSD_SIGNAL_QUIET_WINDOWS * quiet_window_pairs(q);
}

/* Complex Gaussian noise, @p sigma per component. */
std::vector<float>
quiet_noise(size_t pairs, unsigned seed, float sigma = kQuietSigma) {
    std::vector<float> iq(2 * pairs);
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0, sigma);
    for (float& v : iq) {
        v = gauss(rng);
    }
    return iq;
}

/* The amplitude of a carrier @p snr_db above that noise (kQuietSigma) in @p width_hz. */
double
quiet_amplitude(double snr_db, uint32_t rate, double width_hz) {
    const double noise = 2.0 * kQuietSigma * kQuietSigma * width_hz / rate;
    return std::sqrt(noise * std::pow(10.0, snr_db / 10.0));
}

/* Symbol-stream FM deviation per sample: one of @p levels (Hz) per symbol at @p symbol_rate, through a one-pole filter
   at @p shaping_hz. */
std::vector<double>
fsk_deviation(size_t pairs, uint32_t rate, double symbol_rate, const std::vector<double>& levels, double shaping_hz,
              unsigned seed) {
    std::vector<double> out(pairs);
    std::mt19937 rng(seed);
    const double a = std::exp(-kTwoPi * shaping_hz / rate);
    double level = levels[0], smoothed = 0;
    long long symbol = -1;
    for (size_t i = 0; i < pairs; ++i) {
        const long long at = (long long)((double)i * symbol_rate / rate);
        if (at != symbol) {
            symbol = at;
            level = levels[rng() % levels.size()];
        }
        smoothed = a * smoothed + (1 - a) * level;
        out[i] = smoothed;
    }
    return out;
}

/* Tone FM: @p deviation Hz peak with a @p tone Hz sine. */
std::vector<double>
tone_deviation(size_t pairs, uint32_t rate, double deviation, double tone) {
    std::vector<double> out(pairs);
    for (size_t i = 0; i < pairs; ++i) {
        out[i] = deviation * std::sin(kTwoPi * tone * (double)i / rate);
    }
    return out;
}

/* Add an FM transmitter @p offset_hz from the channel centre: @p deviation Hz per sample (empty: a carrier), scaled by
   @p envelope (empty: steady), over pairs [@p from, @p to). */
void
quiet_add_fm(std::vector<float>& iq, uint32_t rate, double offset_hz, double amplitude,
             const std::vector<double>& deviation, const std::vector<double>& envelope = {}, size_t from = 0,
             size_t to = SIZE_MAX) {
    double phase = 0;
    to = std::min(to, iq.size() / 2);
    for (size_t i = from; i < to; ++i) {
        phase += kTwoPi * (offset_hz + (deviation.empty() ? 0.0 : deviation[i])) / rate;
        const double a = amplitude * (envelope.empty() ? 1.0 : envelope[i]);
        iq[2 * i] += (float)(a * std::cos(phase));
        iq[2 * i + 1] += (float)(a * std::sin(phase));
    }
}

/* Measure @p iq in blocks of @p block pairs: the result, after asserting the detector finished as @p expect says. */
dsd_signal_quiet_result
quiet_run(dsd_signal_quiet_detector* d, const dsd_signal_quiet_request& q, const std::vector<float>& iq, size_t block,
          const dsd_signal_quiet_params* params = nullptr, int expect = 1) {
    assert(dsd_signal_quiet_detector_begin(d, &q, params, nullptr) == 0);
    int rc = 0;
    for (size_t i = 0; i < iq.size() / 2 && !rc; i += block) {
        rc = dsd_signal_quiet_detector_feed(d, iq.data() + 2 * i, std::min(block, iq.size() / 2 - i), &q.capture);
    }
    assert(rc == expect);
    dsd_signal_quiet_result result{};
    assert(dsd_signal_quiet_detector_result(d, &result) == expect);
    assert(result.request_id == q.request_id && result.capture.generation == q.capture.generation);
    assert(result.completed_realtime_ns == 0);
    return result;
}

/* Whether @p iq is quiet, judged with @p params; the verdict is the highest ratio against the threshold. */
bool
quiet_verdict(dsd_signal_quiet_detector* d, const dsd_signal_quiet_request& q, const std::vector<float>& iq,
              const dsd_signal_quiet_params& params = kQuietTestParams) {
    const auto result = quiet_run(d, q, iq, 4096, &params);
    assert(result.failure == DSD_SIGNAL_MEASURE_OK && result.valid);
    assert(result.quiet == (result.max_ratio_db < params.threshold_db));
    assert(std::abs(result.power_dbfs - result.floor_dbfs - result.max_ratio_db) < 1e-3f);
    return result.quiet != 0;
}

const uint32_t kQuietRates[] = {1024000, 1536000, 2400000};
} // namespace

/* Noise alone is quiet, in every class at every rate, and the measurement does not depend on how samples arrive. */
static void
test_quiet_noise(dsd_signal_quiet_detector* d) {
    const struct {
        uint8_t classes;
        uint32_t analog;
    } sets[] = {{DSD_SIGNAL_QUIET_CLASS_12K5, 0},
                {DSD_SIGNAL_QUIET_CLASS_6K25, 0},
                {DSD_SIGNAL_QUIET_CLASS_ANALOG, 5000},
                {DSD_SIGNAL_QUIET_CLASS_ANALOG, 25000},
                {DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25 | DSD_SIGNAL_QUIET_CLASS_ANALOG, 12500}};

    for (uint32_t rate : kQuietRates) {
        for (const auto& set : sets) {
            const auto q = quiet_request(rate, set.classes, set.analog);
            for (unsigned seed = 1; seed <= 3; ++seed) {
                const auto iq = quiet_noise(quiet_request_pairs(q), seed);
                assert(quiet_verdict(d, q, iq));
                if (seed != 1) {
                    continue;
                }
                const auto whole = quiet_run(d, q, iq, iq.size() / 2);
                const auto pieces = quiet_run(d, q, iq, 997);
                assert(std::abs(whole.max_ratio_db - pieces.max_ratio_db) < 1e-4f);
                assert(std::abs(whole.floor_dbfs - pieces.floor_dbfs) < 1e-4f);
            }
        }
    }
}

/* A transmitter well above the threshold is not quiet: NFM, C4FM, 4FSK and GMSK in their classes. */
static void
test_quiet_signals(dsd_signal_quiet_detector* d) {
    for (uint32_t rate : kQuietRates) {
        const auto wide = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
        const auto narrow = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_6K25);
        const size_t pairs = quiet_request_pairs(narrow);
        const double a12 = quiet_amplitude(12, rate, 12500), a6 = quiet_amplitude(12, rate, 6250);
        const std::vector<std::vector<double>> wide_modulations = {
            tone_deviation(pairs, rate, 2500, 1000),                              // NFM
            fsk_deviation(pairs, rate, 4800, {-1800, -600, 600, 1800}, 2400, 11), // P25 C4FM
            fsk_deviation(pairs, rate, 4800, {-1944, -648, 648, 1944}, 2400, 12), // DMR 4FSK
        };
        for (const auto& modulation : wide_modulations) {
            auto iq = quiet_noise(pairs, 5);
            quiet_add_fm(iq, rate, 0, a12, modulation);
            assert(!quiet_verdict(d, wide, iq));
        }
        const std::vector<std::vector<double>> narrow_modulations = {
            fsk_deviation(pairs, rate, 2400, {-1050, -350, 350, 1050}, 1200, 13), // NXDN48 4FSK
            fsk_deviation(pairs, rate, 4800, {-1200, 1200}, 2400, 14),            // D-STAR GMSK
        };
        for (const auto& modulation : narrow_modulations) {
            auto iq = quiet_noise(pairs, 6);
            quiet_add_fm(iq, rate, 0, a6, modulation);
            assert(!quiet_verdict(d, narrow, iq));
        }
        // Analog NFM at the widest and AM-narrowest widths.
        for (uint32_t width : {5000U, 25000U}) {
            const auto analog = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_ANALOG, width);
            auto iq = quiet_noise(quiet_request_pairs(analog), 7);
            quiet_add_fm(iq, rate, 0, quiet_amplitude(12, rate, width),
                         tone_deviation(iq.size() / 2, rate, width / 5.0, 1000));
            assert(!quiet_verdict(d, analog, iq));
        }
    }
}

/* A DMR mobile's single-slot burst train (27.5 ms symbol interval every 60 ms, 1.5 ms ramps outside it) is never quiet,
   at any phase: sample-level steps across the 60 ms and phases that start or end a burst on a transform or sub-window
   boundary. */
static void
test_quiet_dmr_bursts(dsd_signal_quiet_detector* d) {
    for (uint32_t rate : kQuietRates) {
        const auto q = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
        const size_t pairs = quiet_request_pairs(q), n = quiet_fft_size(q);
        const size_t period = rate * 60 / 1000, interval = rate * 275 / 10000, ramp = rate * 15 / 10000;
        std::vector<float> burst(2 * pairs, 0.0f);
        quiet_add_fm(burst, rate, 0, quiet_amplitude(15, rate, 12500),
                     fsk_deviation(pairs, rate, 4800, {-1944, -648, 648, 1944}, 2400, 21));
        const auto noise = quiet_noise(pairs, 22);
        /* A burst train whose symbol interval starts at pair @p start (mod the period). */
        auto train = [&](size_t start) {
            std::vector<float> iq = noise;
            for (size_t i = 0; i < pairs; ++i) {
                const size_t t = (i + period - start % period + ramp) % period;
                double envelope = 0;
                if (t >= ramp && t < ramp + interval) {
                    envelope = 1;
                } else if (t < ramp) {
                    envelope = 0.5 - 0.5 * std::cos(3.141592653589793 * (double)t / ramp);
                } else if (t < 2 * ramp + interval) {
                    envelope = 0.5 + 0.5 * std::cos(3.141592653589793 * (double)(t - ramp - interval) / ramp);
                }
                iq[2 * i] += (float)(envelope * burst[2 * i]);
                iq[2 * i + 1] += (float)(envelope * burst[2 * i + 1]);
            }
            return iq;
        };
        /* Sample-level phases across the period (a prime step, so every alignment to the transforms occurs); then
           bursts starting and ending on every sub-window boundary and, at the default rate, on the transform boundary
           halfway through each sub-window and one pair either side of each. */
        const bool fine = rate == 1536000;
        const size_t window = quiet_window_pairs(q);
        std::vector<size_t> starts;
        for (size_t s = 0; s < period; s += fine ? 2003 : 6007) {
            starts.push_back(s);
        }
        for (size_t at = 0; at <= pairs; at += fine ? window / 2 / n * n : window) {
            for (int nudge = fine ? -1 : 0; nudge <= (fine ? 1 : 0); ++nudge) {
                const size_t boundary = at + period + (size_t)(long long)nudge;
                starts.push_back(boundary);            // a burst starts on the boundary
                starts.push_back(boundary - interval); // a burst ends on it
            }
        }
        for (size_t start : starts) {
            assert(!quiet_verdict(d, q, train(start)));
        }
    }
}

/* Energy in only the first or only the last sub-window is not quiet. */
static void
test_quiet_one_subwindow(dsd_signal_quiet_detector* d) {
    for (uint32_t rate : kQuietRates) {
        const auto q = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
        const size_t window = quiet_window_pairs(q), pairs = quiet_request_pairs(q);
        const auto modulation = fsk_deviation(pairs, rate, 4800, {-1800, -600, 600, 1800}, 2400, 31);
        for (size_t which : {size_t{0}, size_t{DSD_SIGNAL_QUIET_WINDOWS - 1}}) {
            auto iq = quiet_noise(pairs, 32);
            quiet_add_fm(iq, rate, 0, quiet_amplitude(15, rate, 12500), modulation, {}, which * window,
                         (which + 1) * window);
            assert(!quiet_verdict(d, q, iq));
        }
    }
}

/* Interferers on the 12.5 kHz class's noise sidebands only, 21-37 kHz either side: carriers on the 2048-point bin
   centres, so they leak into no bin but their neighbours, at random phases, so they do not add up to a pulse train.
   They raise that class's floor, never the 6.25 kHz class's (9.4-18.75 kHz). */
static void
add_wide_sideband_carriers(std::vector<float>& iq, uint32_t rate, double amplitude, unsigned seed) {
    const double bin = (double)rate / 2048;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> start(0, kTwoPi);
    for (int side = -1; side <= 1; side += 2) {
        for (int k = (int)std::ceil(21000 / bin); k * bin < 37000; ++k) {
            const double phase = start(rng), step = kTwoPi * side * k * bin / rate;
            for (size_t i = 0; i < iq.size() / 2; ++i) {
                iq[2 * i] += (float)(amplitude * std::cos(phase + step * (double)i));
                iq[2 * i + 1] += (float)(amplitude * std::sin(phase + step * (double)i));
            }
        }
    }
}

/* A request judges every class it names and is quiet only when every one is. */
static void
test_quiet_classes(dsd_signal_quiet_detector* d) {
    const uint32_t rate = 1536000;
    const auto both = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25);
    const auto wide = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
    const auto narrow = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_6K25);
    const size_t pairs = quiet_request_pairs(both);
    assert(quiet_request_pairs(wide) != pairs && quiet_request_pairs(narrow) == pairs);
    // A carrier 4.5 kHz off centre: inside the 12.5 kHz channel, beside the 6.25 kHz one.
    auto beside = quiet_noise(pairs, 41);
    quiet_add_fm(beside, rate, 4500, quiet_amplitude(15, rate, 12500), {});
    assert(!quiet_verdict(d, wide, beside) && quiet_verdict(d, narrow, beside) && !quiet_verdict(d, both, beside));
    // A centred carrier 12 dB over the 6.25 kHz noise with the 12.5 kHz class's sidebands raised far above it.
    auto centred = quiet_noise(pairs, 42);
    quiet_add_fm(centred, rate, 0, quiet_amplitude(12, rate, 6250), {});
    add_wide_sideband_carriers(centred, rate, quiet_amplitude(12, rate, 6250), 43);
    assert(quiet_verdict(d, wide, centred) && !quiet_verdict(d, narrow, centred) && !quiet_verdict(d, both, centred));
}

/* Noise 20 dB over kQuietSigma everywhere except the hardware band +0.15..+0.45 Fs of @p q's capture (a bin's hardware
   offset is its sample frequency less the rotation, folded into -Fs/2..Fs/2), shaped in the frequency domain. */
static std::vector<float>
quiet_low_band_noise(const dsd_signal_quiet_request& q, size_t pairs, unsigned seed) {
    const int n = (int)pairs;
    const double rate = q.capture.rate_hz;
    std::vector<float> iq = quiet_noise(pairs, seed);
    dsd_io::FftSetupCache fft;
    auto transform = [&](bool inverse) {
        for (int i = 0; inverse && i < n; ++i) {
            iq[2 * (size_t)i + 1] = -iq[2 * (size_t)i + 1];
        }
        assert(fft.forward(n, iq.data()));
        for (int i = 0; inverse && i < n; ++i) {
            iq[2 * (size_t)i] /= (float)n;
            iq[2 * (size_t)i + 1] = -iq[2 * (size_t)i + 1] / (float)n;
        }
    };
    transform(false);
    for (int k = 0; k < n; ++k) {
        double hardware = (k < n / 2 ? k : k - n) * rate / n - q.capture.rotation_hz;
        hardware -= rate * std::floor((hardware + rate / 2) / rate);
        const float gain = (hardware >= 0.15 * rate && hardware < 0.45 * rate) ? 1.0f : 10.0f;
        iq[2 * (size_t)k] *= gain;
        iq[2 * (size_t)k + 1] *= gain;
    }
    transform(true);
    fft.release();
    return iq;
}

/* The floor guard: a weak channel between transmitters that raise its sideband floor on both sides would read quiet
   on the ratio alone; the floor's rise over the capture-wide noise reference voids it. Noise alone stays valid, and the
   guard's edge falls exactly at the reported rise. */
static void
test_quiet_floor_guard(dsd_signal_quiet_detector* d) {
    const uint32_t rate = 1536000;
    const auto wide = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
    const size_t pairs = quiet_request_pairs(wide);
    const dsd_signal_quiet_params shipped = dsd_signal_quiet_params_default();
    dsd_signal_quiet_params unguarded = shipped;
    unguarded.guard_db = 100.0f;
    // A channel 12 dB over its noise, both sides' sideband blocks raised far above it: quiet on the ratio alone.
    auto between = quiet_noise(pairs, 91);
    quiet_add_fm(between, rate, 0, quiet_amplitude(12, rate, 12500), {});
    add_wide_sideband_carriers(between, rate, quiet_amplitude(12, rate, 6250), 92);
    assert(quiet_verdict(d, wide, between, unguarded));
    const auto guarded = quiet_run(d, wide, between, 4096, &shipped);
    assert(guarded.failure == DSD_SIGNAL_MEASURE_OK && !guarded.valid && !guarded.quiet);
    assert(guarded.floor_rise_db > shipped.guard_db + 10.0f);
    // Noise alone stays valid under the shipped guard, in every class at every rate.
    for (uint32_t r : kQuietRates) {
        for (uint8_t classes : {(uint8_t)DSD_SIGNAL_QUIET_CLASS_12K5, (uint8_t)DSD_SIGNAL_QUIET_CLASS_6K25}) {
            const auto q = quiet_request(r, classes);
            for (unsigned seed = 93; seed < 96; ++seed) {
                const auto noise = quiet_run(d, q, quiet_noise(quiet_request_pairs(q), seed), 4096, &shipped);
                assert(noise.valid && noise.floor_rise_db < shipped.guard_db);
                assert(std::abs(noise.floor_rise_db) < 2.0f);
            }
        }
    }
    // The reference spans the hardware band the tap sees under the ingest rotation (Fs/4 here, as the RTL tap runs):
    // a floor 20 dB up everywhere but hardware +0.15..+0.45 Fs, where the lower quartile of the span must come from,
    // is seen as raised round the channel (at hardware -Fs/4).
    const auto raised = quiet_run(d, wide, quiet_low_band_noise(wide, pairs, 98), 4096, &shipped);
    assert(!raised.valid && !raised.quiet && raised.floor_rise_db > 15.0f);
    // Just under and just over the guard: valid while the rise stays at or under it, void once it exceeds it.
    const auto noise = quiet_noise(pairs, 97);
    const float rise = quiet_run(d, wide, noise, 4096, &unguarded).floor_rise_db;
    dsd_signal_quiet_params edge = unguarded;
    edge.guard_db = rise + 0.01f;
    assert(quiet_run(d, wide, noise, 4096, &edge).valid);
    edge.guard_db = rise - 0.01f;
    const auto over = quiet_run(d, wide, noise, 4096, &edge);
    assert(!over.valid && !over.quiet && std::abs(over.floor_rise_db - rise) < 1e-6f);
}

/* What a request may ask for: the analog width endpoints (NFM 8 and 25 kHz, AM 5 and 20 kHz), the 6.25 kHz class at
   2048 points at 1.536 Msps (an NFM search range of 6.25 kHz is still refused), and nothing malformed. */
static void
test_quiet_requests(dsd_signal_quiet_detector* d) {
    dsd_signal_request_failure failure = DSD_SIGNAL_REQUEST_OK;
    for (uint32_t width : {5000U, 8000U, 20000U, 25000U}) {
        const auto q = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_ANALOG, width);
        assert(dsd_signal_quiet_detector_begin(d, &q, nullptr, &failure) == 0 && failure == DSD_SIGNAL_REQUEST_OK);
        assert(quiet_verdict(d, q, quiet_noise(quiet_request_pairs(q), 51)));
    }
    const auto narrow = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_6K25);
    assert(quiet_fft_size(narrow) == 2048 && quiet_window_pairs(narrow) == size_t{8} * 2048);
    assert(dsd_signal_quiet_detector_begin(d, &narrow, nullptr, &failure) == 0);
    dsd_signal_search_range nfm;
    dsd_signal_search_range_default(&nfm);
    nfm.step_num = nfm.bandwidth_hz = 6250;
    assert(dsd_signal_search_validate(&nfm, nullptr, 0) < 0 && dsd_signal_search_fft_size(1536000, &nfm) == 0);
    std::vector<dsd_signal_quiet_request> refused;
    for (uint32_t width : {4999U, 25001U, 0U}) {
        refused.push_back(quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_ANALOG, width));
    }
    refused.push_back(quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5, 12500)); // a width without its class
    refused.push_back(quiet_request(1536000, 0));
    refused.push_back(quiet_request(1536000, 8));
    refused.push_back(quiet_request(0, DSD_SIGNAL_QUIET_CLASS_12K5));
    refused.push_back(quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5));
    refused.back().request_id = 0;
    for (const auto& q : refused) {
        failure = DSD_SIGNAL_REQUEST_OK;
        assert(dsd_signal_quiet_detector_begin(d, &q, nullptr, &failure) == -1);
        assert(failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    }
    assert(dsd_signal_quiet_detector_begin(d, nullptr, nullptr, &failure) == -1);
    assert(dsd_signal_quiet_detector_begin(nullptr, &narrow, nullptr, &failure) == -1);
    for (const dsd_signal_quiet_params& p :
         {dsd_signal_quiet_params{4.0f, 2, 3.0f}, dsd_signal_quiet_params{4.0f, -1, 3.0f},
          dsd_signal_quiet_params{std::numeric_limits<float>::quiet_NaN(), DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, 3.0f},
          dsd_signal_quiet_params{std::numeric_limits<float>::infinity(), DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, 3.0f},
          dsd_signal_quiet_params{4.0f, DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, std::numeric_limits<float>::quiet_NaN()},
          dsd_signal_quiet_params{4.0f, DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, std::numeric_limits<float>::infinity()}}) {
        assert(dsd_signal_quiet_detector_begin(d, &narrow, &p, &failure) == -1);
        assert(failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    }
    // A refused request leaves nothing armed: samples finish nothing.
    const auto noise = quiet_noise(quiet_request_pairs(narrow), 52);
    assert(dsd_signal_quiet_detector_feed(d, noise.data(), noise.size() / 2, &narrow.capture) == 0);
    // The shipped parameters: the calibrated 4.1 dB over the trimmed-mean floor, guarded at 8.7 dB
    // (docs/signal-search.md).
    const auto shipped = dsd_signal_quiet_params_default();
    assert(std::fabs(shipped.threshold_db - 4.1f) < 1e-6f);
    assert(shipped.floor_estimator == DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN);
    assert(std::fabs(shipped.guard_db - 8.7f) < 1e-6f);
    // The domain is exactly the rendered cells: every rendered rate, and at each the Radio sheet's widths of each kind,
    // rendered as that kind only; nothing between, no other kind.
    const uint32_t rendered_rates[] = {960000u,  1024000u, 1152000u, 1200000u, 1280000u, 1440000u, 1536000u, 1600000u,
                                       1800000u, 1920000u, 2048000u, 2400000u, 2500000u, 3000000u, 6000000u, 10000000u};
    const uint32_t nfm_widths[] = {8000u, 11250u, 12500u, 16000u, 20000u, 25000u};
    const uint32_t am_widths[] = {5000u, 6000u, 8000u, 10000u, 15000u, 20000u};
    for (uint32_t rate : rendered_rates) {
        assert(dsd_signal_quiet_rate_supported(rate) == 1);
        for (uint32_t width : nfm_widths) {
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_FM, width, rate) == 1);
        }
        for (uint32_t width : am_widths) {
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_AM, width, rate) == 1);
        }
        // Widths rendered only as the other kind: NFM 10 and 15 kHz, AM 11.25, 12.5 and 16 kHz.
        for (uint32_t width : {10000u, 15000u}) {
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_FM, width, rate) == 0);
        }
        for (uint32_t width : {11250u, 12500u, 16000u}) {
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_AM, width, rate) == 0);
        }
        for (uint32_t width : {0u, 4999u, 7000u, 9000u, 12000u, 14000u, 18000u, 24999u, 25001u}) {
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_FM, width, rate) == 0);
            assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_AM, width, rate) == 0);
        }
        assert(dsd_signal_quiet_analog_supported(2, 12500u, rate) == 0);
        assert(dsd_signal_quiet_analog_supported(-1, 12500u, rate) == 0);
    }
    for (uint32_t rate : {0u, 250000u, 1000000u, 1023999u, 1024001u, 1400000u, 2000000u, 2560000u, 3200000u}) {
        assert(dsd_signal_quiet_rate_supported(rate) == 0);
        assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_FM, 12500u, rate) == 0);
        assert(dsd_signal_quiet_analog_supported(DSD_ANALOG_DEMOD_AM, 6000u, rate) == 0);
    }
}

/* A channel whose sidebands do not fit the capture (at hardware DC, or past 0.45 Fs) is measured but never valid or
   quiet. */
static void
test_quiet_geometry(dsd_signal_quiet_detector* d) {
    const uint32_t rate = 1536000;
    const int class_sets[] = {DSD_SIGNAL_QUIET_CLASS_12K5, DSD_SIGNAL_QUIET_CLASS_6K25,
                              DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25};
    for (double place : {0.0, 0.44}) {
        for (int classes : class_sets) {
            auto q = quiet_request(rate, (uint8_t)classes);
            q.capture.rotation_hz = 0;
            q.capture.capture_hz = (uint32_t)(kQuietChannel + place * rate);
            const auto result = quiet_run(d, q, quiet_noise(quiet_request_pairs(q), 61), 4096);
            assert(result.failure == DSD_SIGNAL_MEASURE_OK && !result.valid && !result.quiet);
        }
    }
    // One class out of geometry spoils the request: a 6.25 kHz channel fits where a 25 kHz analog one does not.
    auto mixed = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_6K25 | DSD_SIGNAL_QUIET_CLASS_ANALOG, 25000);
    mixed.capture.rotation_hz = 0;
    mixed.capture.capture_hz = kQuietChannel - (uint32_t)(0.42 * rate);
    auto alone = mixed;
    alone.classes = DSD_SIGNAL_QUIET_CLASS_6K25;
    alone.analog_width_hz = 0;
    const auto noise = quiet_noise(quiet_request_pairs(mixed), 62);
    const auto fits = quiet_run(d, alone, noise, 4096, &kQuietTestParams);
    assert(fits.valid && fits.quiet);
    const auto spoiled = quiet_run(d, mixed, noise, 4096);
    assert(!spoiled.valid && !spoiled.quiet);
}

/* More than 1% of the pairs clipped fails the request CLIPPED; fewer still measure. Every other failure keeps its
   reason, the request's id and its capture. */
static void
test_quiet_failures(dsd_signal_quiet_detector* d) {
    const auto q = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5);
    const size_t pairs = quiet_request_pairs(q);
    for (size_t every : {size_t{200}, size_t{50}}) {
        auto iq = quiet_noise(pairs, 71);
        for (size_t i = 0; i < pairs; i += every) {
            iq[2 * i + 1] = -1.0f;
        }
        // The clicks' comb stands in the sideband blocks, so the floor guard would void the result: judge without it.
        const auto result = quiet_run(d, q, iq, 4096, &kQuietTestParams, every == 200 ? 1 : -1);
        assert(result.failure == (every == 200 ? DSD_SIGNAL_MEASURE_OK : DSD_SIGNAL_MEASURE_CLIPPED));
        assert(every == 200 ? result.valid != 0 : !result.valid && !result.quiet);
    }
    // Clipping that takes the request's total over 1% only in its last sub-window.
    auto late = quiet_noise(pairs, 72);
    for (size_t i = pairs - quiet_window_pairs(q); i < pairs; i += 5) {
        late[2 * i] = 1.0f;
    }
    assert(quiet_run(d, q, late, 4096, nullptr, -1).failure == DSD_SIGNAL_MEASURE_CLIPPED);
    const auto noise = quiet_noise(pairs, 73);
    auto poisoned = noise;
    poisoned[size_t{2} * 5000] = std::numeric_limits<float>::infinity();
    assert(quiet_run(d, q, poisoned, 4096, nullptr, -1).failure == DSD_SIGNAL_MEASURE_NONFINITE);
    for (int retune = 0; retune < 2; ++retune) {
        auto moved = q.capture;
        ++moved.drops;
        moved.generation += (uint32_t)retune;
        assert(dsd_signal_quiet_detector_begin(d, &q, nullptr, nullptr) == 0);
        assert(dsd_signal_quiet_detector_feed(d, noise.data(), 1000, &q.capture) == 0);
        assert(dsd_signal_quiet_detector_feed(d, noise.data(), 1000, &moved) == -1);
        dsd_signal_quiet_result result{};
        assert(dsd_signal_quiet_detector_result(d, &result) == -1);
        assert(result.failure == (retune ? DSD_SIGNAL_MEASURE_CAPTURE_CHANGED : DSD_SIGNAL_MEASURE_DROPPED));
        assert(result.request_id == q.request_id && result.capture.drops == q.capture.drops);
        assert(!result.valid && !result.quiet);
        // Later samples finish nothing more.
        assert(dsd_signal_quiet_detector_feed(d, noise.data(), noise.size() / 2, &q.capture) == 0);
    }
    // Nothing measured yet: no result.
    dsd_signal_quiet_detector* fresh = dsd_signal_quiet_detector_create();
    dsd_signal_quiet_result untouched{};
    untouched.request_id = 99;
    assert(dsd_signal_quiet_detector_result(fresh, &untouched) == 0 && untouched.request_id == 99);
    dsd_signal_quiet_detector_destroy(fresh);
}

/* Each sub-window is the smallest whole number of transforms of at least 10 ms and shorter than 13.75 ms (so one lies
   inside any 27.5 ms DMR burst), and the nine span at least 90 ms, at every rate and for every class. */
static void
test_quiet_spans(dsd_signal_quiet_detector* d) {
    const struct {
        uint8_t classes;
        uint32_t analog;
    } sets[] = {{DSD_SIGNAL_QUIET_CLASS_12K5, 0},
                {DSD_SIGNAL_QUIET_CLASS_6K25, 0},
                {DSD_SIGNAL_QUIET_CLASS_ANALOG, 5000},
                {DSD_SIGNAL_QUIET_CLASS_ANALOG, 8000},
                {DSD_SIGNAL_QUIET_CLASS_ANALOG, 25000}};

    for (uint32_t rate : kQuietRates) {
        for (const auto& set : sets) {
            const auto q = quiet_request(rate, set.classes, set.analog);
            const auto iq = quiet_noise(quiet_request_pairs(q) + 10, 81);
            assert(dsd_signal_quiet_detector_begin(d, &q, nullptr, nullptr) == 0);
            size_t used = 0;
            int rc = 0;
            while (!rc) {
                rc = dsd_signal_quiet_detector_feed(d, iq.data() + 2 * used, 1, &q.capture);
                ++used;
            }
            assert(rc == 1 && used % DSD_SIGNAL_QUIET_WINDOWS == 0);
            const size_t window = used / DSD_SIGNAL_QUIET_WINDOWS, n = quiet_fft_size(q);
            assert(window % n == 0 && window * 100 >= rate && (window - n) * 100 < rate);
            assert((uint64_t)window * 100000 < (uint64_t)rate * 1375);
            assert((uint64_t)used * 1000 >= (uint64_t)rate * 90);
            dsd_signal_quiet_result single{};
            assert(dsd_signal_quiet_detector_result(d, &single) == 1);
            assert(std::abs(quiet_run(d, q, iq, 4096).max_ratio_db - single.max_ratio_db) < 1e-4f);
        }
    }
}

/* The two floor estimators on the same samples: the lowest sideband block never reads above the trimmed mean, so it
   never reads a lower ratio; with the trimmed mean, a sub-window measures exactly what Signal search measures. */
static void
test_quiet_estimators(dsd_signal_quiet_detector* d) {
    const uint32_t rate = 1536000;
    const auto q = quiet_request(rate, DSD_SIGNAL_QUIET_CLASS_12K5);
    const dsd_signal_quiet_params trimmed{30.0f, DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN, 100.0f};
    const dsd_signal_quiet_params lowest{30.0f, DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK, 100.0f};
    for (unsigned seed = 1; seed <= 4; ++seed) {
        auto iq = quiet_noise(quiet_request_pairs(q), 90 + seed);
        for (size_t i = 2; seed % 2 == 0 && i < iq.size(); ++i) {
            iq[i] = 0.8f * iq[i - 2] + 0.2f * iq[i]; // a sloping floor
        }
        const auto by_mean = quiet_run(d, q, iq, 4096, &trimmed);
        const auto by_lowest = quiet_run(d, q, iq, 4096, &lowest);
        assert(by_mean.quiet && by_lowest.quiet);
        assert(by_lowest.max_ratio_db > by_mean.max_ratio_db);
    }
    // A carrier only in the second sub-window: the request's highest ratio is that sub-window's, which Signal search
    // measures as its second window of a 12.5 kHz channel.
    const size_t window = quiet_window_pairs(q);
    auto iq = quiet_noise(quiet_request_pairs(q), 99);
    quiet_add_fm(iq, rate, 1000, quiet_amplitude(10, rate, 12500), {}, {}, window, 2 * window);
    const auto quiet_side = quiet_run(d, q, iq, 4096, &trimmed);
    dsd_signal_measure_request search{};
    dsd_signal_search_range_default(&search.range);
    search.range.step_num = search.range.bandwidth_hz = 12500;
    search.session_id = search.request_id = 1;
    search.capture = q.capture;
    search.count = 1;
    search.frequencies[0] = q.frequency_hz;
    assert(dsd_signal_search_fft_size(rate, &search.range) == (int)quiet_fft_size(q));
    dsd_signal_detector* searcher = dsd_signal_detector_create();
    const auto report = run(searcher, search, iq, 4096);
    dsd_signal_detector_destroy(searcher);
    assert(report.candidates[0].valid);
    assert(std::abs(report.candidates[0].power_dbfs - quiet_side.power_dbfs) < 1e-4f);
    assert(std::abs(report.candidates[0].noise_dbfs - quiet_side.floor_dbfs) < 1e-4f);
}

/* The transform sizes Signal search measures with are unchanged by the helper it shares with quiet requests. */
static void
test_search_fft_sizes() {
    const struct {
        uint32_t rate, step, width;
        int n;
    } sizes[] = {{1536000, 25000, 25000, 512},  {1536000, 12500, 16000, 1024}, {1536000, 12500, 12500, 1024},
                 {1536000, 6250, 8000, 2048},   {1024000, 12500, 12500, 1024}, {2400000, 12500, 12500, 2048},
                 {2400000, 25000, 20000, 1024}, {1536000, 250, 25000, 65536}};

    for (const auto& s : sizes) {
        dsd_signal_search_range range;
        dsd_signal_search_range_default(&range);
        range.step_num = s.step;
        range.bandwidth_hz = s.width;
        assert(dsd_signal_search_fft_size(s.rate, &range) == s.n);
    }
    assert(dsd_signal_fft_size_for_width(0, 12500) == 0 && dsd_signal_fft_size_for_width(1536000, 0) == 0);
    assert(dsd_signal_fft_size_for_width(10000000, 100) == 0);
}

/* Feed the shared tap @p pairs of @p iq from @p at in blocks of 4096. */
static void
quiet_tap_feed(const std::vector<float>& iq, size_t at, size_t pairs, const dsd_signal_capture& capture) {
    for (size_t i = at; i < at + pairs; i += 4096) {
        rtl_signal_measure_feed(iq.data() + 2 * i, std::min<size_t>(4096, at + pairs - i), &capture);
    }
}

/* The shared tap's owners: a quiet request is refused while Signal search owns the tuner or holds a request or an
   unread report; a search request supersedes a quiet one at once; search cancels leave a quiet request alone. */
static void
test_quiet_tap_owners(const dsd_signal_measure_request& r) {
    const auto q = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5);
    const auto noise = quiet_noise(quiet_request_pairs(q) + 4096, 101);
    const auto search_iq = samples(r, r.frequencies[0], 0.1, 30000);
    uint32_t ticket = 0;
    dsd_signal_request_failure failure = DSD_SIGNAL_REQUEST_OK;
    dsd_signal_quiet_result result{};
    dsd_signal_measure_report report{};
    // A search request outstanding, then its unread report, refuse a quiet request; reading the report frees the tap.
    assert(rtl_signal_measure_request(&r, nullptr) == 0);
    assert(rtl_signal_quiet_request(&q, &ticket, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    rtl_signal_measure_feed(search_iq.data(), 30000, &r.capture);
    assert(rtl_signal_quiet_request(&q, &ticket, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    assert(rtl_signal_measure_get(&report) == 1);
    assert(rtl_signal_quiet_request(&q, &ticket, &failure) == 0 && failure == DSD_SIGNAL_REQUEST_OK && ticket != 0);
    rtl_signal_quiet_cancel(ticket);
    // A search owning the tuner refuses it too.
    rtl_signal_search_set_owner(9);
    assert(rtl_signal_quiet_request(&q, &ticket, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    rtl_signal_search_set_owner(0);
    // A search cancel or a search report read does not touch a quiet request.
    assert(rtl_signal_quiet_request(&q, &ticket, nullptr) == 0);
    rtl_signal_measure_cancel();
    assert(rtl_signal_measure_get(&report) == 0);
    assert(rtl_signal_quiet_status(ticket) == 0);
    quiet_tap_feed(noise, 0, quiet_request_pairs(q), q.capture);
    assert(rtl_signal_quiet_status(ticket) == 1);
    assert(rtl_signal_measure_get(&report) == 0);
    assert(rtl_signal_quiet_take(ticket, &result) == 1);
    assert(result.request_id == q.request_id && result.failure == DSD_SIGNAL_MEASURE_OK && result.valid);
    assert(result.completed_realtime_ns != 0 && result.capture.generation == q.capture.generation);
    // A search request supersedes a pending quiet request at once, with no further samples, and one done but untaken.
    for (size_t before : {size_t{4096}, quiet_request_pairs(q)}) {
        assert(rtl_signal_quiet_request(&q, &ticket, nullptr) == 0);
        quiet_tap_feed(noise, 0, before, q.capture);
        assert(rtl_signal_quiet_status(ticket) == (before == 4096 ? 0 : 1));
        assert(rtl_signal_measure_request(&r, nullptr) == 0);
        assert(rtl_signal_quiet_status(ticket) == -1);
        result = dsd_signal_quiet_result{};
        assert(rtl_signal_quiet_take(ticket, &result) == -1 && result.failure == DSD_SIGNAL_MEASURE_SUPERSEDED);
        assert(!result.valid && !result.quiet);
        // The search measures on: its report is its own.
        rtl_signal_measure_feed(search_iq.data(), 30000, &r.capture);
        assert(rtl_signal_measure_get(&report) == 1 && report.request_id == r.request_id);
    }
    // A refused search request supersedes it too.
    assert(rtl_signal_quiet_request(&q, &ticket, nullptr) == 0);
    auto unsupported = r;
    unsupported.capture.rate_hz = 0;
    assert(rtl_signal_measure_request(&unsupported, nullptr) == -1);
    assert(rtl_signal_quiet_status(ticket) == -1);
    rtl_signal_measure_cancel();
}

/* Tickets: cancel and reset lose a quiet request at once; a stale ticket never reads, takes or cancels a newer
   request; a taken result is gone; the 30-bit ticket wraps past zero. */
static void
test_quiet_tap_tickets() {
    static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(uint32_t) == sizeof(int),
                  "the quiet status word is a lock-free 32-bit atomic");
    const auto q = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5);
    const size_t pairs = quiet_request_pairs(q);
    const auto noise = quiet_noise(pairs, 111);
    dsd_signal_quiet_result result{};
    uint32_t first = 0, second = 0;
    assert(rtl_signal_quiet_status(0) == -1 && rtl_signal_quiet_take(0, &result) == -1);
    // Cancel: lost at once, and later samples publish nothing.
    assert(rtl_signal_quiet_request(&q, &first, nullptr) == 0);
    quiet_tap_feed(noise, 0, 4096, q.capture);
    rtl_signal_quiet_cancel(first);
    assert(rtl_signal_quiet_status(first) == -1);
    quiet_tap_feed(noise, 0, pairs, q.capture);
    assert(rtl_signal_quiet_status(first) == -1);
    assert(rtl_signal_quiet_take(first, &result) == -1 && result.failure == DSD_SIGNAL_MEASURE_SUPERSEDED);
    // Reset: lost at once.
    assert(rtl_signal_quiet_request(&q, &first, nullptr) == 0);
    rtl_signal_measure_reset();
    assert(rtl_signal_quiet_status(first) == -1);
    quiet_tap_feed(noise, 0, pairs, q.capture);
    assert(rtl_signal_quiet_status(first) == -1);
    // A newer quiet request: the older ticket is lost, and cancelling or taking it does not touch the newer one.
    assert(rtl_signal_quiet_request(&q, &first, nullptr) == 0);
    auto newer = q;
    newer.request_id = 42;
    assert(rtl_signal_quiet_request(&newer, &second, nullptr) == 0 && second != first);
    assert(rtl_signal_quiet_status(first) == -1 && rtl_signal_quiet_status(second) == 0);
    rtl_signal_quiet_cancel(first);
    assert(rtl_signal_quiet_take(first, &result) == -1 && result.failure == DSD_SIGNAL_MEASURE_SUPERSEDED);
    assert(rtl_signal_quiet_status(second) == 0);
    quiet_tap_feed(noise, 0, pairs, q.capture);
    assert(rtl_signal_quiet_status(second) == 1 && rtl_signal_quiet_status(first) == -1);
    rtl_signal_quiet_cancel(first);
    assert(rtl_signal_quiet_status(second) == 1);
    result = dsd_signal_quiet_result{};
    assert(rtl_signal_quiet_take(second, &result) == 1 && result.request_id == 42 && result.valid);
    // The tap measures as the stand-alone detector does with the shipped parameters.
    dsd_signal_quiet_detector* alone = dsd_signal_quiet_detector_create();
    const auto expected = quiet_run(alone, newer, noise, 4096);
    dsd_signal_quiet_detector_destroy(alone);
    assert(std::abs(expected.max_ratio_db - result.max_ratio_db) < 1e-4f && expected.quiet == result.quiet);
    assert(std::abs(expected.floor_dbfs - result.floor_dbfs) < 1e-4f);
    // Taken: the result is gone.
    assert(rtl_signal_quiet_status(second) == -1);
    assert(rtl_signal_quiet_take(second, &result) == -1 && result.failure == DSD_SIGNAL_MEASURE_SUPERSEDED);
    // A pending take leaves the result untouched.
    assert(rtl_signal_quiet_request(&q, &first, nullptr) == 0);
    result.request_id = 77;
    assert(rtl_signal_quiet_take(first, &result) == 0 && result.request_id == 77);
    // A failure is taken as one, with its request's id.
    auto moved = q.capture;
    ++moved.gain;
    rtl_signal_measure_feed(noise.data(), 10, &moved);
    assert(rtl_signal_quiet_status(first) == 1);
    assert(rtl_signal_quiet_take(first, &result) == -1);
    assert(result.failure == DSD_SIGNAL_MEASURE_CAPTURE_CHANGED && result.request_id == q.request_id);
    // A refused quiet request says why.
    rtl_signal_measure_test_fail_allocation(1);
    dsd_signal_request_failure failure = DSD_SIGNAL_REQUEST_OK;
    assert(rtl_signal_quiet_request(&q, &first, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_NO_MEMORY);
    rtl_signal_measure_test_fail_allocation(0);
    auto bad = q;
    bad.classes = 0;
    assert(rtl_signal_quiet_request(&bad, &first, &failure) == -1 && failure == DSD_SIGNAL_REQUEST_UNSUPPORTED);
    // The ticket wraps from the last 30-bit value to 1, never 0.
    rtl_signal_measure_test_set_ticket(0x3FFFFFFEu);
    assert(rtl_signal_quiet_request(&q, &first, nullptr) == 0 && first == 0x3FFFFFFFu);
    assert(rtl_signal_quiet_request(&q, &second, nullptr) == 0 && second == 1);
    assert(rtl_signal_quiet_status(first) == -1 && rtl_signal_quiet_status(second) == 0);
    rtl_signal_quiet_cancel(second);
    assert(rtl_signal_quiet_status(second) == -1);
}

/* The demod thread feeding while the decoder thread requests, cancels, resets and supersedes, and a third thread reads
   status words: every result taken is its own request's, a lost ticket reads lost at once, and status never waits. */
static void
test_quiet_tap_threads(const dsd_signal_measure_request& r) {
    const auto q = quiet_request(1536000, DSD_SIGNAL_QUIET_CLASS_12K5);
    const auto noise = quiet_noise(quiet_request_pairs(q) * 2, 121);
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> live{0};
    std::thread demod([&] {
        const size_t block = 4096, blocks = noise.size() / 2 / block;
        for (size_t at = 0; !stop.load(std::memory_order_acquire); at = (at + 1) % blocks) {
            rtl_signal_measure_feed(noise.data() + 2 * at * block, block, &q.capture);
        }
    });
    std::thread reader([&] {
        uint32_t stale = 1;
        while (!stop.load(std::memory_order_acquire)) {
            const int now = rtl_signal_quiet_status(live.load(std::memory_order_acquire));
            assert(now >= -1 && now <= 1);
            assert(rtl_signal_quiet_status(stale & 0x3FFFFFFFu) >= -1);
            ++stale;
            dsd_signal_capture capture{};
            (void)rtl_signal_capture_get(&capture);
        }
    });
    int taken = 0;
    for (uint64_t id = 1; id <= 48; ++id) {
        auto request = q;
        request.request_id = 2000 + id;
        uint32_t ticket = 0;
        assert(rtl_signal_quiet_request(&request, &ticket, nullptr) == 0);
        live.store(ticket, std::memory_order_release);
        dsd_signal_quiet_result result{};
        switch (id % 4) {
            case 1: rtl_signal_quiet_cancel(ticket); break;
            case 2: assert(rtl_signal_measure_request(&r, nullptr) == 0); break;
            case 3: rtl_signal_measure_reset(); break;
            default: break;
        }
        if (id % 4 != 0) {
            assert(rtl_signal_quiet_status(ticket) == -1);
            assert(rtl_signal_quiet_take(ticket, &result) == -1 && result.failure == DSD_SIGNAL_MEASURE_SUPERSEDED);
            rtl_signal_measure_cancel();
            continue;
        }
        int status = 0;
        for (int poll = 0; poll < 20000000 && status == 0; ++poll) {
            status = rtl_signal_quiet_status(ticket);
            std::this_thread::yield();
        }
        assert(status == 1);
        assert(rtl_signal_quiet_take(ticket, &result) == 1);
        assert(result.request_id == request.request_id && result.failure == DSD_SIGNAL_MEASURE_OK && result.valid);
        ++taken;
    }
    stop.store(true, std::memory_order_release);
    demod.join();
    reader.join();
    rtl_signal_measure_cancel();
    assert(taken == 12);
}

static void
test_quiet(const dsd_signal_measure_request& r) {
    dsd_signal_quiet_detector* d = dsd_signal_quiet_detector_create();
    assert(d);
    test_search_fft_sizes();
    test_quiet_requests(d);
    test_quiet_noise(d);
    test_quiet_signals(d);
    test_quiet_dmr_bursts(d);
    test_quiet_one_subwindow(d);
    test_quiet_classes(d);
    test_quiet_floor_guard(d);
    test_quiet_geometry(d);
    test_quiet_failures(d);
    test_quiet_spans(d);
    test_quiet_estimators(d);
    dsd_signal_quiet_detector_destroy(d);
    test_quiet_tap_owners(r);
    test_quiet_tap_tickets();
    test_quiet_tap_threads(r);
}

int
main() {
    dsd_signal_measure_request r{};
    dsd_signal_search_range_default(&r.range);
    r.range.bandwidth_hz = 12500;
    r.session_id = 17;
    r.request_id = 29;
    r.capture.rate_hz = 1024000;
    r.capture.capture_hz = 100300000;
    r.capture.rotation_hz = 256000;
    r.capture.generation = 5;
    r.count = 4;
    r.frequencies[0] = 100000000;
    r.frequencies[1] = 100012500;
    r.frequencies[2] = r.capture.capture_hz;                                // Hardware DC is never a candidate.
    r.frequencies[3] = r.capture.capture_hz + r.capture.rate_hz / 2 - 1000; // Capture edge.
    dsd_signal_detector* d = dsd_signal_detector_create();
    assert(d);
    auto iq = samples(r, r.frequencies[0], 0.1, 30000);
    auto whole = run(d, r, iq, 30000);
    auto fragmented = run(d, r, iq, 137);
    assert(whole.session_id == 17 && whole.request_id == 29 && whole.generation == 5);
    assert(whole.candidates[0].detected && !whole.candidates[1].detected && !whole.candidates[2].valid);
    assert(!whole.candidates[3].valid);
    assert(std::abs(whole.candidates[0].power_dbfs + 20) < 0.05);
    assert(std::abs(fragmented.candidates[0].power_dbfs - whole.candidates[0].power_dbfs) < 1e-5);
    assert(whole.sample_count >= 20480 && whole.sample_count < 22528);
    auto noise = samples(r, r.frequencies[0], 0, 30000);
    assert(!run(d, r, noise, 53).candidates[0].detected);
    auto colored = noise;
    for (size_t i = 2; i < colored.size(); ++i) {
        colored[i] = 0.8f * colored[i - 2] + 0.2f * noise[i];
    }
    assert(!run(d, r, colored, 227).candidates[0].detected);
    auto off_grid = samples(r, r.frequencies[0] + 2100, 0.1, 30000);
    assert(run(d, r, off_grid, 777).candidates[0].detected);
    // Modulation-independent power: AM audio, NFM audio and shaped 4-FSK symbols.
    for (int modulation = 0; modulation < 3; ++modulation) {
        auto modulated = samples(r, r.frequencies[0], 0, 30000);
        double phase = 0;
        for (size_t i = 0; i < 30000; ++i) {
            double voice = std::sin(6.283185307179586 * 900 * i / r.capture.rate_hz);
            double deviation = modulation == 1 ? 2000 * voice : modulation == 2 ? (int(i / 214) % 4 - 1.5) * 1200 : 0;
            phase += 6.283185307179586
                     * ((double)r.frequencies[0] - r.capture.capture_hz + r.capture.rotation_hz + deviation)
                     / r.capture.rate_hz;
            double amplitude = modulation == 0 ? 0.08 * (1 + 0.6 * voice) : 0.1;
            modulated[2 * i] += (float)(amplitude * std::cos(phase));
            modulated[2 * i + 1] += (float)(amplitude * std::sin(phase));
        }
        assert(run(d, r, modulated, 313).candidates[0].detected);
    }
    // A second transmitter is independently reported and ranks by integrated RF power.
    auto adjacent = samples(r, r.frequencies[1], 0.2, 30000);
    for (size_t i = 0; i < adjacent.size(); ++i) {
        adjacent[i] += off_grid[i];
    }
    auto both = run(d, r, adjacent, 991);
    assert(both.candidates[0].detected && both.candidates[1].detected);
    assert(both.candidates[1].power_dbfs > both.candidates[0].power_dbfs);
    // One strong NFM transmitter on the default 12.5 kHz grid with 16 kHz windows: its skirt and the window's leakage
    // pass the neighbours' thresholds on their own, but they are its energy, not transmitters of their own. A genuinely
    // separate, much weaker neighbour is still reported, and so is an off-grid transmitter's own channel.
    {
        dsd_signal_measure_request grid = r;
        dsd_signal_search_range_default(&grid.range);
        grid.capture.capture_hz = 163450000;
        grid.capture.rotation_hz = 384000;
        grid.capture.rate_hz = 1536000;
        grid.count = 5;
        for (uint32_t i = 0; i < grid.count; ++i) {
            grid.frequencies[i] = 162987500 + 12500 * i; // 163.0125 MHz is index 2
        }
        auto quiet = samples(grid, grid.frequencies[0], 0, 40000);
        auto single = quiet;
        add_nfm(single, grid, 163012500, 0.3, 2500, 1000);
        auto alone = run(d, grid, single, 4096);
        assert(alone.candidates[2].detected);
        assert(alone.candidates[1].power_dbfs - alone.candidates[1].noise_dbfs > 30); // the skirt is there ...
        assert(!alone.candidates[1].detected && !alone.candidates[3].detected);       // ... and is not a detection
        auto wide = quiet;
        add_nfm(wide, grid, 163012500, 0.3, 5000, 1000);
        auto wide_report = run(d, grid, wide, 4096);
        assert(wide_report.candidates[2].detected && !wide_report.candidates[1].detected
               && !wide_report.candidates[3].detected);
        auto off_grid_nfm = quiet;
        add_nfm(off_grid_nfm, grid, 163014600, 0.3, 2500, 1000);
        auto off = run(d, grid, off_grid_nfm, 4096);
        assert(off.candidates[2].detected && !off.candidates[1].detected && !off.candidates[3].detected);
        for (double weaker : {0.1, 0.01, 0.003}) {
            auto pair = single;
            add_nfm(pair, grid, 163000000, weaker, 2500, 700);
            auto both_nfm = run(d, grid, pair, 4096);
            assert(both_nfm.candidates[2].detected && both_nfm.candidates[1].detected);
            assert(!both_nfm.candidates[3].detected);
        }
        // A wide (5 kHz deviation) transmitter off-grid toward the next channel is still one detection, at its
        // nearest channel: neither the window's truncation of it nor its own sideband structure is a neighbour.
        auto wide_off_grid = quiet;
        add_nfm(wide_off_grid, grid, 163015500, 0.3, 5000, 300);
        auto straddle = run(d, grid, wide_off_grid, 4096);
        assert(straddle.candidates[2].detected && !straddle.candidates[1].detected && !straddle.candidates[3].detected);
        // The stronger transmitter need not be a requested channel: at a range or span boundary only its neighbour
        // is measured, and that neighbour's window still holds nothing but its skirt.
        dsd_signal_measure_request edge = grid;
        edge.count = 1;
        edge.frequencies[0] = 163000000;
        assert(!run(d, edge, single, 4096).candidates[0].detected);
        // On a 6.25 kHz grid a 12.5 kHz transmitter fills its neighbours' windows; it is reported once.
        dsd_signal_measure_request dense = grid;
        dense.range.step_num = 6250;
        dense.range.bandwidth_hz = 12500;
        for (uint32_t i = 0; i < dense.count; ++i) {
            dense.frequencies[i] = 163000000 + 6250 * i; // 163.0125 MHz is index 2
        }
        auto dense_report = run(d, dense, single, 4096);
        assert(dense_report.candidates[2].detected);
        assert(!dense_report.candidates[1].detected && !dense_report.candidates[3].detected);
        // Two weaker transmitters either side of a strong one, each a separate signal: the strong transmitter's
        // mirrored side is the other weak one, which must not make either look like leakage.
        dsd_signal_measure_request wide_windows = grid;
        wide_windows.range.bandwidth_hz = 20000;
        auto three = single;
        add_nfm(three, wide_windows, 163000000, 0.03, 2500, 700);
        add_nfm(three, wide_windows, 163025000, 0.03, 2500, 900);
        auto trio = run(d, wide_windows, three, 4096);
        assert(trio.candidates[1].detected && trio.candidates[2].detected && trio.candidates[3].detected);
    }
    auto clipping = samples(r, r.frequencies[0], 1.2, 30000);
    dsd_signal_measure_report invalid{};
    assert(dsd_signal_detector_begin(d, &r) == 0);
    assert(dsd_signal_detector_feed(d, clipping.data(), 30000, &r.capture, &invalid) == -1);
    assert_failure(invalid, r, DSD_SIGNAL_MEASURE_CLIPPED);
    // A sample clips when either component reaches full scale: under 1% of them still measures, over it does not.
    for (int component = 0; component < 2; ++component) {
        for (size_t every : {size_t{200}, size_t{50}}) {
            auto peaks = samples(r, r.frequencies[0], 0.1, 30000);
            for (size_t i = 0; i < peaks.size() / 2; i += every) {
                peaks[2 * i + (size_t)component] = component ? -0.999f : 0.999f;
            }
            assert(dsd_signal_detector_begin(d, &r) == 0);
            assert(dsd_signal_detector_feed(d, peaks.data(), 30000, &r.capture, &invalid) == (every == 200 ? 1 : -1));
        }
    }
    // A non-finite sample in either component retires the request in the call that delivers it, mid-frame.
    for (int component = 0; component < 2; ++component) {
        for (float wild : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()}) {
            auto poisoned = samples(r, r.frequencies[0], 0.1, 3000);
            poisoned[size_t{2} * 1500 + (size_t)component] = wild;
            assert(dsd_signal_detector_begin(d, &r) == 0);
            assert(dsd_signal_detector_feed(d, poisoned.data(), 1000, &r.capture, &invalid) == 0);
            assert(dsd_signal_detector_feed(d, poisoned.data() + 2000, 2000, &r.capture, &invalid) == -1);
            assert_failure(invalid, r, DSD_SIGNAL_MEASURE_NONFINITE);
            assert(dsd_signal_detector_feed(d, poisoned.data(), 1000, &r.capture, &invalid) == 0);
        }
    }
    // A completed report is invalidated too: a gain change must not publish old power.
    rtl_signal_measure_feed(off_grid.data(), 1, &r.capture);
    assert(rtl_signal_measure_request(&r, nullptr) == 0);
    rtl_signal_measure_feed(off_grid.data(), 30000, &r.capture);
    auto moved = r.capture;
    ++moved.gain;
    rtl_signal_measure_feed(off_grid.data(), 1, &moved);
    assert(rtl_signal_measure_get(&invalid) == -1);
    assert(invalid.failure == DSD_SIGNAL_MEASURE_CAPTURE_CHANGED && invalid.request_id == r.request_id);
    rtl_signal_measure_cancel();
    test_tap_schedules(r, off_grid);
    test_tap_threads(r, off_grid);
    test_tap_failures(r, off_grid);
    test_request_failures(r);
    test_failure_reasons(d, r);
    test_clip_levels(d, r);
    test_quiet(r);
    // A burst shorter than the first 10 ms cannot pass two windows.
    for (size_t i = size_t{5000} * 2; i < iq.size(); ++i) {
        iq[i] = noise[i];
    }
    assert(!run(d, r, iq, 137).candidates[0].detected);
    r.range.minimum_set = 1;
    r.range.minimum_dbfs = -10;
    assert(!run(d, r, off_grid, 1000).candidates[0].detected);
    r.range.minimum_set = 0;
    // Every capture discontinuity retires partial samples.
    for (int change = 0; change < 5; ++change) {
        assert(dsd_signal_detector_begin(d, &r) == 0);
        dsd_signal_measure_report out{};
        assert(dsd_signal_detector_feed(d, iq.data(), 1024, &r.capture, &out) == 0);
        auto capture = r.capture;
        if (change == 0) {
            ++capture.generation;
        }
        if (change == 1) {
            ++capture.drops;
        }
        if (change == 2) {
            ++capture.gain;
        }
        if (change == 3) {
            ++capture.capture_hz;
        }
        if (change == 4) {
            ++capture.rate_hz;
        }
        assert(dsd_signal_detector_feed(d, iq.data(), 1024, &capture, &out) == -1);
        assert_failure(out, r, change == 1 ? DSD_SIGNAL_MEASURE_DROPPED : DSD_SIGNAL_MEASURE_CAPTURE_CHANGED);
    }
    r.capture.rotation_hz = 0;
    assert(run(d, r, samples(r, r.frequencies[0], 0.1, 30000), 509).candidates[0].detected);
    r.frequencies[0] = r.capture.capture_hz + 300000;
    assert(run(d, r, samples(r, r.frequencies[0], 0.1, 30000), 509).candidates[0].detected);
    test_ownership(d);
    test_neighbours(d);
    test_sustained(d);
    test_noise(d);
    dsd_signal_detector_destroy(d);
    auto range = r.range;
    range.start_hz = 450000000;
    range.end_hz = 460000000;
    range.step_num = 25000;
    range.step_den = 3;
    assert(dsd_signal_search_channel_count(&range) == 1201);
    assert(dsd_signal_search_frequency(&range, 3) == 450025000);
    assert(dsd_signal_search_frequency(&range, 1200) == 460000000);
    for (int rotation : {0, 256000}) {
        uint32_t visited = 0;
        while (visited < 1201) {
            dsd_signal_search_span span{};
            assert(dsd_signal_search_plan(&range, visited, 1024000, rotation, &span) == 1);
            assert(span.first == visited && span.count > 0 && span.next > visited);
            for (uint32_t c = 0; c < span.count; ++c) {
                assert(span.frequencies[c] == dsd_signal_search_frequency(&range, visited + c));
                double delta = (double)span.frequencies[c] - span.tune_hz - rotation;
                assert(delta > -0.45 * 1024000 + 3 * range.bandwidth_hz);
                assert(delta < -0.01 * 1024000 - 3 * range.bandwidth_hz);
            }
            visited = span.next;
        }
        assert(visited == 1201);
    }
    range.step_num = 1;
    assert(dsd_signal_search_validate(&range, nullptr, 0) < 0);
    // A dynamic squelch takes the margins the receiver applies, never one it would clamp; Level mode uses none.
    dsd_signal_search_range squelch;
    dsd_signal_search_range_default(&squelch);
    for (int mode : {DSD_SQUELCH_MODE_AUTO, DSD_SQUELCH_MODE_NOISE}) {
        squelch.squelch_mode = mode;
        squelch.squelch_margin_db = DSD_SQUELCH_MARGIN_MIN_DB - 1;
        assert(dsd_signal_search_validate(&squelch, nullptr, 0) < 0);
        squelch.squelch_margin_db = DSD_SQUELCH_MARGIN_MAX_DB + 1;
        assert(dsd_signal_search_validate(&squelch, nullptr, 0) < 0);
        squelch.squelch_margin_db = DSD_SQUELCH_MARGIN_MIN_DB;
        assert(dsd_signal_search_validate(&squelch, nullptr, 0) == 0);
    }
    squelch.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    squelch.squelch_margin_db = 0;
    assert(dsd_signal_search_validate(&squelch, nullptr, 0) == 0);
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
