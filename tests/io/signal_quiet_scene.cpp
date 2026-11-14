// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "signal_quiet_scene.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace dsd_quiet_scene {

namespace {

constexpr double kPi = 3.14159265358979323846;
/* The half-band model's roll-off (see the header): it starts at kHalfbandFlat; kHalfbandWidth and kHalfbandExponent
 * solve [(1 + cos(pi u)) / 2]^p = 10^(-0.35/20) at 0.45 Fs and 1/2 at 0.5 Fs. */
constexpr double kHalfbandFlat = 0.42;
constexpr double kHalfbandWidth = 0.08274495758136702;
constexpr double kHalfbandExponent = 0.11728905195350056;
constexpr double kTunerStopDb = 40.0;
/* The depression's extent and taper, in channel widths from the channel. */
constexpr double kDepressionHalfWidths = 1.5;
constexpr double kDepressionTaperWidths = 0.25;

struct FittedEdges {
    uint32_t rate;
    Shape shape;
    TunerEdges edges;
};

const FittedEdges kEdges[] = {
    {2500000, Shape::NOMINAL, {-0.335, 0.302, 0.10}},    {2500000, Shape::STEEPER, {-0.28, 0.28, 0.10}},
    {2500000, Shape::MIRRORED, {-0.302, 0.335, 0.10}},   {3000000, Shape::NOMINAL, {-0.335, 0.302, 0.10}},
    {3000000, Shape::STEEPER, {-0.28, 0.28, 0.10}},      {3000000, Shape::MIRRORED, {-0.302, 0.335, 0.10}},
    {6000000, Shape::NOMINAL, {-0.4365, 0.4035, 0.06}},  {6000000, Shape::STEEPER, {-0.40, 0.40, 0.06}},
    {6000000, Shape::MIRRORED, {-0.4035, 0.4365, 0.06}}, {10000000, Shape::NOMINAL, {-0.4665, 0.4335, 0.06}},
    {10000000, Shape::STEEPER, {-0.40, 0.40, 0.06}},     {10000000, Shape::MIRRORED, {-0.4335, 0.4665, 0.06}},
};

/* A raised cosine falling from 1 to 0 as @p u goes from 0 to 1 (clamped). */
double
falling(double u) {
    if (u <= 0) {
        return 1.0;
    }
    if (u >= 1) {
        return 0.0;
    }
    return 0.5 * (1.0 + std::cos(kPi * u));
}

/* The depression's share of its depth at @p from_channel_hz: 1 inside, a raised-cosine taper, 0 beyond. */
double
depression_share(double from_channel_hz, double width_hz) {
    const double d = std::fabs(from_channel_hz) / width_hz;
    return falling((d - (kDepressionHalfWidths - kDepressionTaperWidths / 2)) / kDepressionTaperWidths);
}

} // namespace

bool
parse_shape(const char* text, Shape* out) {
    struct NamedShape {
        const char* name;
        Shape shape;
    };

    static const NamedShape kNames[] = {
        {"flat", Shape::FLAT}, {"nominal", Shape::NOMINAL}, {"steeper", Shape::STEEPER}, {"mirrored", Shape::MIRRORED}};

    const auto* n = std::find_if(std::begin(kNames), std::end(kNames),
                                 [&](const NamedShape& e) { return std::strcmp(text, e.name) == 0; });
    if (n == std::end(kNames)) {
        return false;
    }
    *out = n->shape;
    return true;
}

const char*
shape_text(Shape shape) {
    switch (shape) {
        case Shape::NOMINAL: return "nominal";
        case Shape::STEEPER: return "steeper";
        case Shape::MIRRORED: return "mirrored";
        default: return "flat";
    }
}

double
halfband_power(double fraction) {
    const double amplitude =
        std::pow(falling((std::fabs(fraction) - kHalfbandFlat) / kHalfbandWidth), kHalfbandExponent);
    return amplitude * amplitude;
}

bool
tuner_edges(Shape shape, uint32_t rate, TunerEdges* out) {
    const auto* e = std::find_if(std::begin(kEdges), std::end(kEdges),
                                 [&](const FittedEdges& f) { return f.rate == rate && f.shape == shape; });
    if (e == std::end(kEdges)) {
        return false;
    }
    *out = e->edges;
    return true;
}

double
tuner_power(const TunerEdges& edges, double fraction) {
    const double half = edges.transition / 2;
    const double upper = falling((fraction - (edges.high - half)) / edges.transition);
    const double lower = falling(((edges.low + half) - fraction) / edges.transition);
    const double floor = std::pow(10.0, -kTunerStopDb / 10.0);
    return floor + (1.0 - floor) * std::min(upper, lower);
}

bool
response_supported(const Response& r, uint32_t rate) {
    TunerEdges edges{};
    return r.shape == Shape::FLAT ? r.depression_db <= 0 && !r.adc : tuner_edges(r.shape, rate, &edges);
}

double
hardware_hz(double sample_hz, double rotation_hz, double rate) {
    const double hw = sample_hz - rotation_hz;
    return hw - rate * std::floor((hw + rate / 2) / rate);
}

double
response_power(const Response& r, const Placement& p, double hw_hz) {
    TunerEdges edges{};
    if (r.shape == Shape::FLAT || !tuner_edges(r.shape, p.rate, &edges)) {
        return 1.0;
    }
    const double fraction = hw_hz / p.rate;
    double power = halfband_power(fraction) * tuner_power(edges, fraction);
    if (r.depression_db > 0) {
        const double channel_hw = hardware_hz(p.channel_sample_hz, p.rotation_hz, p.rate);
        /* the channel's distance in hardware coordinates, folded like any other frequency */
        const double from_channel = hardware_hz(hw_hz - channel_hw, 0, p.rate);
        power *= std::pow(10.0, -r.depression_db * depression_share(from_channel, p.width_hz) / 10.0);
    }
    return power;
}

void
response_bins(const Response& r, const Placement& p, int n, const std::function<double(double)>* colour, Bins* out) {
    out->signal.resize((size_t)n);
    out->noise.resize((size_t)n);
    const double adc = r.adc ? std::pow(10.0, r.adc_db / 10.0) : 0.0;
    for (int k = 0; k < n; ++k) {
        const double sample_hz = (k < n / 2 ? k : k - n) * (double)p.rate / n;
        const double power = response_power(r, p, hardware_hz(sample_hz, p.rotation_hz, p.rate));
        const double tuner = power * (colour ? (*colour)(sample_hz) : 1.0);
        out->signal[(size_t)k] = (float)std::sqrt(power);
        out->noise[(size_t)k] = (float)std::sqrt(tuner + adc);
    }
}

bool
inverse(dsd_io::FftSetupCache& fft, int n, cf* data) {
    for (int i = 0; i < n; ++i) {
        data[i] = std::conj(data[i]);
    }
    const bool ok = fft.forward(n, reinterpret_cast<float*>(data));
    for (int i = 0; i < n; ++i) {
        data[i] = std::conj(data[i]);
    }
    return ok;
}

bool
shape_record(const Bins& bins, int n, cf* x, bool signal, double sigma, Rng& rng, dsd_io::FftSetupCache& fft) {
    if (signal && !fft.forward(n, reinterpret_cast<float*>(x))) {
        return false;
    }
    /* A circular Gaussian record of density noise[k]^2: independent bins of variance n noise[k]^2 2 sigma^2 before the
       unscaled inverse and its 1/n, so each sample's variance is 2 sigma^2 mean(noise^2). The inverse runs through
       the forward transform on the conjugate (inverse()), so each bin is stored conjugated here and the samples are
       conjugated back with the 1/n in one pass after it. */
    const float root_n = (float)std::sqrt((double)n);
    for (int k = 0; k < n; ++k) {
        cf y = signal ? x[k] * bins.signal[(size_t)k] : cf(0, 0);
        if (sigma > 0) {
            y += rng.gauss(sigma) * (root_n * bins.noise[(size_t)k]);
        }
        x[k] = std::conj(y);
    }
    if (!fft.forward(n, reinterpret_cast<float*>(x))) {
        return false;
    }
    const float scale = 1.0f / (float)n;
    for (int i = 0; i < n; ++i) {
        x[i] = std::conj(x[i]) * scale;
    }
    return true;
}

/* The smallest record of at least @p count points the transform takes: 2^a 3^b 5^c with a >= 4. */
int
record_size(int count) {
    for (int n = std::max(16, count);; ++n) {
        int m = n;
        int twos = 0;
        while (m % 2 == 0) {
            m /= 2;
            ++twos;
        }
        for (int f : {3, 5}) {
            while (m % f == 0) {
                m /= f;
            }
        }
        if (m == 1 && twos >= 4) {
            return n;
        }
    }
}

} // namespace dsd_quiet_scene
