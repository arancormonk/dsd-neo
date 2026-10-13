// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <algorithm>
#include <cassert>
#include <cmath>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
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
    // A completed report is invalidated too: a gain change must not publish old power.
    rtl_signal_measure_feed(off_grid.data(), 1, &r.capture);
    assert(rtl_signal_measure_request(&r) == 0);
    rtl_signal_measure_feed(off_grid.data(), 30000, &r.capture);
    auto moved = r.capture;
    ++moved.gain;
    rtl_signal_measure_feed(off_grid.data(), 1, &moved);
    assert(rtl_signal_measure_get(&invalid) == -1);
    rtl_signal_measure_cancel();
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
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
