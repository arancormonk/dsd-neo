// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The quiet calibration's receiver responses and capture coordinates (tests/io/signal_quiet_scene.h), checked against
 * fixtures written out by hand: the response's edges at known hardware frequencies, and where a tone and the noise
 * land in a rendered record of a shifted (RTL-style, Fs/4) and a centred (native Airspy) capture, read back by a
 * direct DFT that shares no code with the renderer. */

// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "rtl_fft_cache.h"
#include "signal_quiet_scene.h"

namespace qs = dsd_quiet_scene;

namespace {

constexpr double kTwoPi = 6.283185307179586;

double
db(double power) {
    return 10.0 * std::log10(power);
}

void
expect_db(double power, double want_db, double tol_db, const char* what) {
    const double got = db(power);
    if (std::fabs(got - want_db) > tol_db) {
        std::fprintf(stderr, "%s: %.4f dB, want %.4f +- %.4f\n", what, got, want_db, tol_db);
    }
    assert(std::fabs(got - want_db) <= tol_db);
}

qs::Response
response(qs::Shape shape) {
    qs::Response r;
    r.shape = shape;
    return r;
}

/* A shifted capture: tuned Fs/4 above the channel, rotated by +Fs/4 so the channel is at the samples' 0 Hz. */
qs::Placement
shifted(uint32_t rate, double width) {
    return qs::Placement{rate, rate / 4.0, 0.0, width};
}

qs::Placement
centred(uint32_t rate, double width) {
    return qs::Placement{rate, 0.0, 0.0, width};
}

/* The modelled libairspy half-band: 0 dB to 0.42 Fs, -0.35 dB at 0.45 Fs, -6.02 dB (half amplitude) at 0.5 Fs. */
void
test_halfband() {
    expect_db(qs::halfband_power(0.0), 0.0, 1e-9, "half-band at the centre");
    expect_db(qs::halfband_power(0.42), 0.0, 1e-9, "half-band at 0.42 Fs");
    expect_db(qs::halfband_power(-0.42), 0.0, 1e-9, "half-band at -0.42 Fs");
    expect_db(qs::halfband_power(0.45), -0.35, 0.005, "half-band at 0.45 Fs");
    expect_db(qs::halfband_power(-0.45), -0.35, 0.005, "half-band at -0.45 Fs");
    expect_db(qs::halfband_power(0.5), -6.0206, 0.005, "half-band at 0.5 Fs");
    expect_db(qs::halfband_power(-0.5), -6.0206, 0.005, "half-band at -0.5 Fs");
    assert(qs::halfband_power(0.44) < 1.0 && qs::halfband_power(0.44) > qs::halfband_power(0.45));
}

/* Each shape's -3 dB points, at the rates they are fitted for (hardware frequency as a share of the rate). */
void
test_tuner_edges() {
    struct Edge {
        qs::Shape shape;
        uint32_t rate;
        double low, high;
    };

    const Edge edges[] = {
        {qs::Shape::NOMINAL, 2500000, -0.335, 0.302},     {qs::Shape::NOMINAL, 3000000, -0.335, 0.302},
        {qs::Shape::MIRRORED, 2500000, -0.302, 0.335},    {qs::Shape::MIRRORED, 3000000, -0.302, 0.335},
        {qs::Shape::STEEPER, 2500000, -0.28, 0.28},       {qs::Shape::STEEPER, 3000000, -0.28, 0.28},
        {qs::Shape::NOMINAL, 6000000, -0.4365, 0.4035},   {qs::Shape::MIRRORED, 6000000, -0.4035, 0.4365},
        {qs::Shape::STEEPER, 6000000, -0.40, 0.40},       {qs::Shape::NOMINAL, 10000000, -0.4665, 0.4335},
        {qs::Shape::MIRRORED, 10000000, -0.4335, 0.4665}, {qs::Shape::STEEPER, 10000000, -0.40, 0.40},
    };
    for (const Edge& e : edges) {
        qs::TunerEdges t{};
        assert(qs::tuner_edges(e.shape, e.rate, &t));
        expect_db(qs::tuner_power(t, e.low), -3.0103, 0.01, "tuner low -3 dB point");
        expect_db(qs::tuner_power(t, e.high), -3.0103, 0.01, "tuner high -3 dB point");
        expect_db(qs::tuner_power(t, 0.0), 0.0, 1e-9, "tuner at the centre");
        /* Monotone outward, and far past either edge only the stop-band floor is left. */
        assert(qs::tuner_power(t, e.high + 0.01) < qs::tuner_power(t, e.high));
        assert(qs::tuner_power(t, e.low - 0.01) < qs::tuner_power(t, e.low));
        expect_db(qs::tuner_power(t, 0.5), -40.0, 0.01, "tuner stop band");
        expect_db(qs::tuner_power(t, -0.5), -40.0, 0.01, "tuner stop band");
        /* The whole response is the tuner times the half-band, in hardware coordinates. */
        const qs::Placement p = centred(e.rate, 12500);
        const double hw = e.high * e.rate;
        expect_db(qs::response_power(response(e.shape), p, hw), db(qs::halfband_power(e.high)) - 3.0103, 0.01,
                  "response at the high edge");
    }
    qs::TunerEdges t{};
    assert(!qs::tuner_edges(qs::Shape::FLAT, 2500000, &t));
    assert(!qs::tuner_edges(qs::Shape::NOMINAL, 1536000, &t));
    const qs::Response flat = response(qs::Shape::FLAT), nominal = response(qs::Shape::NOMINAL);
    assert(qs::response_supported(flat, 1536000));
    assert(!qs::response_supported(nominal, 1536000));
    assert(qs::response_supported(nominal, 6000000));
    /* FLAT is no response at all: no half-band either. */
    expect_db(qs::response_power(response(qs::Shape::FLAT), centred(2500000, 12500), 0.49 * 2500000), 0.0, 1e-12,
              "flat response");
}

/* Hardware frequency = sample frequency less the rotation, folded into [-Fs/2, Fs/2). */
void
test_hardware_coordinates() {
    const double fs = 2500000;
    assert(std::fabs(qs::hardware_hz(0, fs / 4, fs) - (-625000)) < 1e-6);
    assert(std::fabs(qs::hardware_hz(700000, fs / 4, fs) - 75000) < 1e-6);
    assert(std::fabs(qs::hardware_hz(-1200000, fs / 4, fs) - 675000) < 1e-6); /* -1.825 MHz folds up */
    assert(std::fabs(qs::hardware_hz(1250000, 0, fs) - (-1250000)) < 1e-6);   /* the Nyquist bin is negative */
    assert(std::fabs(qs::hardware_hz(-1250000, 0, fs) - (-1250000)) < 1e-6);
    assert(std::fabs(qs::hardware_hz(1249999, 0, fs) - 1249999) < 1e-6);
    assert(std::fabs(qs::hardware_hz(300000, 0, fs) - 300000) < 1e-6);
}

/* The depression: the stated depth over 3 widths centred on the channel's hardware frequency, none beyond. */
void
test_depression() {
    qs::Response r = response(qs::Shape::NOMINAL);
    r.depression_db = 2.0;
    const double width = 12500;
    for (const qs::Placement& p : {centred(2500000, width), shifted(2500000, width)}) {
        const double channel_hw = qs::hardware_hz(p.channel_sample_hz, p.rotation_hz, p.rate);
        const double base = qs::response_power(response(qs::Shape::NOMINAL), p, channel_hw);
        expect_db(qs::response_power(r, p, channel_hw) / base, -2.0, 1e-9, "depression at the channel");
        expect_db(qs::response_power(r, p, channel_hw + 1.3 * width)
                      / qs::response_power(response(qs::Shape::NOMINAL), p, channel_hw + 1.3 * width),
                  -2.0, 1e-9, "depression inside 1.5 widths");
        expect_db(qs::response_power(r, p, channel_hw - 1.7 * width)
                      / qs::response_power(response(qs::Shape::NOMINAL), p, channel_hw - 1.7 * width),
                  0.0, 1e-9, "no depression past 1.5 widths and the taper");
        /* half depth at the stated 1.5-width edge */
        expect_db(qs::response_power(r, p, channel_hw + 1.5 * width)
                      / qs::response_power(response(qs::Shape::NOMINAL), p, channel_hw + 1.5 * width),
                  -1.0, 1e-9, "depression edge");
    }
}

/* A direct DFT of @p x at @p hz: the complex amplitude of a tone there (no FFT, no window). */
std::complex<double>
dft_at(const std::vector<qs::cf>& x, double hz, double rate) {
    std::complex<double> sum(0, 0), turn(1, 0);
    const std::complex<double> step = std::polar(1.0, -kTwoPi * hz / rate);
    for (size_t i = 0; i < x.size(); ++i) {
        sum += std::complex<double>(x[i].real(), x[i].imag()) * turn;
        turn *= step;
    }
    return sum / (double)x.size();
}

/* A tone of unit amplitude at sample frequency @p sample_hz (on the record's bin grid) through the response, read back
 * at that frequency: its power is the response at the hardware frequency the fixture names. */
double
tone_through(const qs::Response& r, const qs::Placement& p, int n, double sample_hz) {
    qs::Bins bins;
    qs::response_bins(r, p, n, nullptr, &bins);
    std::vector<qs::cf> x((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double a = kTwoPi * sample_hz * i / p.rate;
        x[(size_t)i] = qs::cf((float)std::cos(a), (float)std::sin(a));
    }
    dsd_io::FftSetupCache fft;
    qs::Rng rng(1);
    const bool shaped = qs::shape_record(bins, n, x.data(), true, 0.0, rng, fft);
    assert(shaped);
    fft.release();
    return std::norm(dft_at(x, sample_hz, p.rate));
}

void
test_tone_positions() {
    const uint32_t rate = 2500000;
    const int n = 10000; /* 250 Hz bins: every fixture frequency below is on the grid */
    const qs::Response nominal = response(qs::Shape::NOMINAL);
    /* Shifted: hardware -0.335 Fs is sample -0.085 Fs; hardware +0.302 Fs is sample +0.552 Fs, which folds to
     * -0.448 Fs; the hardware centre is sample +Fs/4. */
    const qs::Placement s = shifted(rate, 12500);
    expect_db(tone_through(nominal, s, n, -0.085 * rate), -3.0103, 0.02, "shifted: low edge");
    expect_db(tone_through(nominal, s, n, -0.448 * rate), -3.0103, 0.02, "shifted: high edge, folded");
    expect_db(tone_through(nominal, s, n, 0.25 * rate), 0.0, 0.01, "shifted: hardware centre");
    /* the channel (sample 0 Hz) sits at hardware -Fs/4, inside the flat part */
    expect_db(tone_through(nominal, s, n, 0.0), 0.0, 0.01, "shifted: channel");
    /* Centred: the sample frequency is the hardware frequency. */
    const qs::Placement c = centred(rate, 12500);
    expect_db(tone_through(nominal, c, n, -0.335 * rate), -3.0103, 0.02, "centred: low edge");
    expect_db(tone_through(nominal, c, n, 0.302 * rate), -3.0103, 0.02, "centred: high edge");
    expect_db(tone_through(nominal, c, n, 0.0), 0.0, 0.01, "centred: channel");
    /* Orientation: the mirrored shape swaps the edges; the nominal one is lower at +0.32 Fs than at -0.32 Fs. */
    const qs::Response mirrored = response(qs::Shape::MIRRORED);
    expect_db(tone_through(mirrored, c, n, 0.335 * rate), -3.0103, 0.02, "centred mirrored: high edge");
    expect_db(tone_through(mirrored, c, n, -0.302 * rate), -3.0103, 0.02, "centred mirrored: low edge");
    assert(tone_through(nominal, c, n, 0.32 * rate) < tone_through(nominal, c, n, -0.32 * rate));
    /* A tone off the response's edges keeps its frequency: nothing appears at its mirror image. */
    qs::Bins bins;
    qs::response_bins(nominal, c, n, nullptr, &bins);
    std::vector<qs::cf> x((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double a = kTwoPi * 0.1 * i;
        x[(size_t)i] = qs::cf((float)std::cos(a), (float)std::sin(a));
    }
    dsd_io::FftSetupCache fft;
    qs::Rng rng(1);
    const bool shaped = qs::shape_record(bins, n, x.data(), true, 0.0, rng, fft);
    assert(shaped);
    fft.release();
    assert(std::norm(dft_at(x, -0.1 * rate, rate)) < 1e-10);
}

/* The noise's density follows the response at the hardware frequency, and the ADC share stays flat. */
double
noise_power_at(const qs::Response& r, const qs::Placement& p, int n, double sample_hz, int trials) {
    qs::Bins bins;
    qs::response_bins(r, p, n, nullptr, &bins);
    dsd_io::FftSetupCache fft;
    qs::Rng rng(7);
    double sum = 0;
    std::vector<qs::cf> x((size_t)n);
    const double bin_hz = (double)p.rate / n;
    for (int t = 0; t < trials; ++t) {
        const bool shaped = qs::shape_record(bins, n, x.data(), false, 1.0, rng, fft);
        assert(shaped);
        /* the DFT at a bin of white noise of unit variance per component has mean power 2/n; five bins centred on
           the frequency, so a straight slope across them cancels */
        for (int k = -2; k <= 2; ++k) {
            sum += std::norm(dft_at(x, sample_hz + k * bin_hz, p.rate)) * n / 2.0;
        }
    }
    fft.release();
    return sum / (5.0 * trials);
}

void
test_noise_density() {
    const uint32_t rate = 2500000;
    const int n = 800; /* 3125 Hz bins */
    const int trials = 3000;
    const qs::Response nominal = response(qs::Shape::NOMINAL);
    const qs::Placement s = shifted(rate, 12500);
    /* Tuner noise only: the hardware centre (sample +Fs/4) reads the full density, the low edge (sample -0.085 Fs)
     * half of it. 15000 exponential readings (3000 trials of five bins): one sigma is 0.035 dB, so +-0.15 dB is over
     * 4 sigma. */
    expect_db(noise_power_at(nominal, s, n, 0.25 * rate, trials), 0.0, 0.15, "noise at the hardware centre");
    expect_db(noise_power_at(nominal, s, n, -0.085 * rate, trials), -3.0103, 0.15, "noise at the low edge");
    /* Far in the tuner's stop band (hardware +0.45 Fs, sample -0.3 Fs) only the flat ADC share is left: with it equal
     * to the tuner's peak density the centre reads 2 and the stop band about 1. */
    qs::Response mix = nominal;
    mix.adc = true;
    mix.adc_db = 0.0;
    expect_db(noise_power_at(mix, s, n, 0.25 * rate, trials), 3.0103, 0.15, "mixture at the hardware centre");
    expect_db(noise_power_at(mix, s, n, -0.3 * rate, trials), 0.0, 0.15, "mixture in the stop band");
    /* The bins report the same: the noise's standard deviation over the base noise's. */
    qs::Bins bins;
    qs::response_bins(mix, s, n, nullptr, &bins);
    assert(std::fabs(bins.noise[(size_t)(n / 4)] * bins.noise[(size_t)(n / 4)] - 2.0) < 1e-6);
    assert(std::fabs(bins.signal[(size_t)(n / 4)] - 1.0) < 1e-6);
}

} // namespace

int
main() {
    test_halfband();
    test_tuner_edges();
    test_hardware_coordinates();
    test_depression();
    test_tone_positions();
    test_noise_density();
    std::printf("IO_SIGNAL_QUIET_SCENE: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
