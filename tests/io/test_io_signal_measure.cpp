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
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/signal_search.h>
#include <limits>
#include <random>
#include <stdint.h>
#include <stdlib.h>
#include <thread>
#include <vector>

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
    assert(rtl_signal_measure_request(&first) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_request(&second) == 0);
    assert(rtl_signal_measure_get(&report) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 1 && report.request_id == 102);
    assert(rtl_signal_measure_get(&report) == 0);
    // A cancel retires the request: the rest of its samples publish nothing.
    assert(rtl_signal_measure_request(&first) == 0);
    rtl_signal_measure_feed(iq.data(), pairs / 4, &r.capture);
    rtl_signal_measure_cancel();
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 0);
    // A completed report taken before a capture change is still delivered; one left waiting is invalidated.
    assert(rtl_signal_measure_request(&first) == 0);
    rtl_signal_measure_feed(iq.data(), pairs, &r.capture);
    assert(rtl_signal_measure_get(&report) == 1 && report.request_id == 101);
    auto moved = r.capture;
    ++moved.drops;
    rtl_signal_measure_feed(iq.data(), 1, &moved);
    assert(rtl_signal_measure_get(&report) == 0);
    rtl_signal_measure_cancel();
}

/* The demod thread feeding while the decoder thread requests, polls and cancels: every report delivered is for the
   request outstanding, a cancel leaves nothing to deliver, and polling the capture never waits for analysis. */
static void
test_tap_threads(const dsd_signal_measure_request& r, const std::vector<float>& iq) {
    std::atomic<bool> stop{false};
    std::thread demod([&] {
        const size_t block = 4096;
        for (size_t at = 0; !stop.load(std::memory_order_acquire); at = (at + block) % (iq.size() / 2 - block)) {
            rtl_signal_measure_feed(iq.data() + 2 * at, block, &r.capture);
        }
    });
    dsd_signal_measure_request request = r;
    int delivered = 0;
    for (uint64_t id = 1; id <= 60; ++id) {
        request.request_id = 1000 + id;
        assert(rtl_signal_measure_request(&request) == 0);
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
            if (rc == 1) {
                assert(report.request_id == request.request_id);
                ++delivered;
                break;
            }
            assert(rc == 0);
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    demod.join();
    rtl_signal_measure_cancel();
    assert(delivered > 0);
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
            assert(dsd_signal_detector_feed(d, poisoned.data(), 1000, &r.capture, &invalid) == 0);
        }
    }
    // A completed report is invalidated too: a gain change must not publish old power.
    rtl_signal_measure_feed(off_grid.data(), 1, &r.capture);
    assert(rtl_signal_measure_request(&r) == 0);
    rtl_signal_measure_feed(off_grid.data(), 30000, &r.capture);
    auto moved = r.capture;
    ++moved.gain;
    rtl_signal_measure_feed(off_grid.data(), 1, &moved);
    assert(rtl_signal_measure_get(&invalid) == -1);
    rtl_signal_measure_cancel();
    test_tap_schedules(r, off_grid);
    test_tap_threads(r, off_grid);
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
