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

void
Rng::gauss_fill(double sigma, int count, cf* out) {
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr int kBatch = 256;
    double u[kBatch], v[kBatch];
    float re[kBatch], im[kBatch];
    for (int base = 0; base < count; base += kBatch) {
        const int len = std::min(kBatch, count - base);
        /* gauss() draws its radius's uniform, then its angle's */
        for (int i = 0; i < len; ++i) {
            u[i] = uniform();
            v[i] = uniform();
        }
        for (int i = 0; i < len; ++i) {
            const double r = sigma * std::sqrt(-2.0 * std::log(u[i])), t = kTwoPi * v[i];
            re[i] = (float)(r * std::cos(t));
            im[i] = (float)(r * std::sin(t));
        }
        for (int i = 0; i < len; ++i) {
            out[base + i] = cf(re[i], im[i]);
        }
    }
}

namespace {

/* Points of the radix stage handled at once: at the records' 5 to 20 parts, a block of every part stays in the core's
   first-level cache. */
constexpr int kRadixBlock = 64;
constexpr int kMaxParts = 32;

/* Whether pffft takes a complex transform of @p m points: a multiple of 16 of the form 2^a 3^b 5^c. */
bool
pffft_takes(int m) {
    if (m < 16 || m % 16 != 0) {
        return false;
    }
    for (int f : {2, 3, 5}) {
        while (m % f == 0) {
            m /= f;
        }
    }
    return m == 1;
}

/* w_n^-e = exp(-2 pi i e / n), from the exact integer phase. */
cf
twiddle(long long e, long long n) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double a = -kTwoPi * (double)(e % n) / (double)n;
    return cf((float)std::cos(a), (float)std::sin(a));
}

} // namespace

SplitTransform::~SplitTransform() { fft_.release(); }

bool
SplitTransform::plan(int n, int max_part) {
    if (n == n_ && max_part == max_part_) {
        return true;
    }
    n_ = 0;
    int r = 1;
    for (int c = 1; c <= kMaxParts; ++c) {
        if (n % c == 0 && n / c <= max_part && pffft_takes(n / c)) {
            r = c;
            break;
        }
    }
    const int m = n / r;
    if (!pffft_takes(m) || !fft_.prepare(m)) {
        return false;
    }
    r_ = r;
    m_ = m;
    max_part_ = max_part;
    /* m / 8 + 1 cache lines: odd, since pffft's m is a multiple of 16, and a multiple of pffft's 16-byte alignment */
    stride_ = r == 1 ? (size_t)m : (size_t)m + 8;
    spectrum_.assign((size_t)r * stride_, cf(0, 0));
    build_tables(n);
    n_ = n;
    return true;
}

/* The radix stage's twiddles, its r-point DFT matrix and its block buffers, for r_ parts of m_ points. */
void
SplitTransform::build_tables(int n) {
    const int r = r_, m = m_;
    const int blocks = (m + kRadixBlock - 1) / kRadixBlock;
    block_twiddles_.resize((size_t)blocks * r);
    step_twiddles_.resize((size_t)r * kRadixBlock);
    dft_re_.resize((size_t)r * r);
    dft_im_.resize((size_t)r * r);
    for (int b = 0; b < blocks; ++b) {
        for (int k2 = 0; k2 < r; ++k2) {
            block_twiddles_[(size_t)b * r + k2] = twiddle((long long)b * kRadixBlock * k2, n);
        }
    }
    for (int k2 = 0; k2 < r; ++k2) {
        for (int j = 0; j < kRadixBlock; ++j) {
            step_twiddles_[(size_t)k2 * kRadixBlock + j] = twiddle((long long)j * k2, n);
        }
    }
    for (int p = 0; p < r; ++p) {
        for (int q = 0; q < r; ++q) {
            const cf w = twiddle((long long)p * q, r);
            dft_re_[(size_t)p * r + q] = w.real();
            dft_im_[(size_t)p * r + q] = w.imag();
        }
    }
    for (std::vector<float>* v : {&in_re_, &in_im_, &out_re_, &out_im_}) {
        v->assign((size_t)r * kRadixBlock, 0.0f);
    }
}

/* out[q][j] = sum over p of w_r^-(p q) in[p][j], for j < len: the r-point DFT of every point of a block at once. */
void
SplitTransform::radix_dft(const float* in_re, const float* in_im, int len, float* out_re, float* out_im) const {
    for (int q = 0; q < r_; ++q) {
        float* ore = out_re + (size_t)q * kRadixBlock;
        float* oim = out_im + (size_t)q * kRadixBlock;
        std::fill(ore, ore + len, 0.0f);
        std::fill(oim, oim + len, 0.0f);
        for (int p = 0; p < r_; ++p) {
            const float wr = dft_re_[(size_t)p * r_ + q], wi = dft_im_[(size_t)p * r_ + q];
            const float* ire = in_re + (size_t)p * kRadixBlock;
            const float* iim = in_im + (size_t)p * kRadixBlock;
            for (int j = 0; j < len; ++j) {
                ore[j] += wr * ire[j] - wi * iim[j];
                oim[j] += wr * iim[j] + wi * ire[j];
            }
        }
    }
}

/* Decimation in frequency: with t = t1 + m t2 and k = r k1 + k2, X[r k1 + k2] is the m-point DFT over t1 of
   w_n^-(t1 k2) sum over t2 of w_r^-(t2 k2) x[t1 + m t2]. This stage leaves that inner sum, twiddled, in part(k2)[t1]. */
void
SplitTransform::radix_forward(const cf* x) {
    for (int t0 = 0, b = 0; t0 < m_; t0 += kRadixBlock, ++b) {
        const int len = std::min(kRadixBlock, m_ - t0);
        for (int p = 0; p < r_; ++p) {
            const cf* src = x + t0 + (size_t)m_ * p;
            for (int j = 0; j < len; ++j) {
                in_re_[(size_t)p * kRadixBlock + j] = src[j].real();
                in_im_[(size_t)p * kRadixBlock + j] = src[j].imag();
            }
        }
        radix_dft(in_re_.data(), in_im_.data(), len, out_re_.data(), out_im_.data());
        for (int q = 0; q < r_; ++q) {
            const cf base = block_twiddles_[(size_t)b * r_ + q];
            const cf* step = step_twiddles_.data() + (size_t)q * kRadixBlock;
            cf* dst = part(q) + t0;
            for (int j = 0; j < len; ++j) {
                dst[j] =
                    cf(out_re_[(size_t)q * kRadixBlock + j], out_im_[(size_t)q * kRadixBlock + j]) * (base * step[j]);
            }
        }
    }
}

/* The same split read the other way: with the m-point DFT of each part(k2) taken, z[t1 + m t2] is the sum over k2 of
   w_r^-(t2 k2) w_n^-(t1 k2) part(k2)[t1]; written conjugated and scaled. */
void
SplitTransform::radix_inverse(cf* out, float scale) {
    for (int t0 = 0, b = 0; t0 < m_; t0 += kRadixBlock, ++b) {
        const int len = std::min(kRadixBlock, m_ - t0);
        for (int p = 0; p < r_; ++p) {
            const cf base = block_twiddles_[(size_t)b * r_ + p];
            const cf* step = step_twiddles_.data() + (size_t)p * kRadixBlock;
            const cf* src = part(p) + t0;
            for (int j = 0; j < len; ++j) {
                const cf v = src[j] * (base * step[j]);
                in_re_[(size_t)p * kRadixBlock + j] = v.real();
                in_im_[(size_t)p * kRadixBlock + j] = v.imag();
            }
        }
        radix_dft(in_re_.data(), in_im_.data(), len, out_re_.data(), out_im_.data());
        for (int q = 0; q < r_; ++q) {
            cf* dst = out + t0 + (size_t)m_ * q;
            const float* re = out_re_.data() + (size_t)q * kRadixBlock;
            const float* im = out_im_.data() + (size_t)q * kRadixBlock;
            for (int j = 0; j < len; ++j) {
                dst[j] = cf(re[j], -im[j]) * scale;
            }
        }
    }
}

bool
SplitTransform::transform_parts() {
    for (int k2 = 0; k2 < r_; ++k2) {
        if (!fft_.forward(m_, reinterpret_cast<float*>(part(k2)))) {
            return false;
        }
    }
    return true;
}

bool
SplitTransform::forward(const cf* x) {
    if (n_ == 0) {
        return false;
    }
    if (r_ == 1) {
        std::copy(x, x + n_, spectrum_.begin());
    } else {
        radix_forward(x);
    }
    return transform_parts();
}

bool
SplitTransform::inverse_from_conjugate(cf* out, float scale) {
    if (n_ == 0 || !transform_parts()) {
        return false;
    }
    if (r_ > 1) {
        radix_inverse(out, scale);
        return true;
    }
    for (int i = 0; i < n_; ++i) {
        out[i] = std::conj(spectrum_[(size_t)i]) * scale;
    }
    return true;
}

SplitTransform*
TransformCache::get(int n) {
    int slot = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[i] && slots_[i]->size() == n) {
            used_[i] = ++clock_;
            return slots_[i].get();
        }
        if (used_[i] < used_[slot]) {
            slot = i;
        }
    }
    if (!slots_[slot]) {
        slots_[slot].reset(new SplitTransform());
    }
    used_[slot] = ++clock_;
    return slots_[slot]->plan(n) ? slots_[slot].get() : nullptr;
}

namespace {

/* The response and its noise on the split spectrum, bin by bin in bin order so the noise takes the same draws as one
   whole-record pass would: a circular Gaussian record of density noise[k]^2, independent bins of variance
   n noise[k]^2 2 sigma^2 before the unscaled inverse and its 1/n, so each sample's variance is 2 sigma^2
   mean(noise^2). The inverse runs through the forward transform on the conjugate, so each bin is stored conjugated. */
void
shape_bins(const Bins& bins, bool signal, double sigma, Rng& rng, SplitTransform& t) {
    const int r = t.parts(), m = t.part_points();
    const float root_n = (float)std::sqrt((double)t.size());
    cf noise[kRadixBlock * kMaxParts];
    for (int k1 = 0; k1 < m; k1 += kRadixBlock) {
        const int len = std::min(kRadixBlock, m - k1);
        if (sigma > 0) {
            rng.gauss_fill(sigma, len * r, noise);
        }
        for (int j = 0; j < len; ++j) {
            for (int k2 = 0; k2 < r; ++k2) {
                const size_t k = (size_t)(k1 + j) * r + k2;
                cf& bin = t.part(k2)[k1 + j];
                cf y = signal ? bin * bins.signal[k] : cf(0, 0);
                if (sigma > 0) {
                    y += noise[j * r + k2] * (root_n * bins.noise[k]);
                }
                bin = std::conj(y);
            }
        }
    }
}

} // namespace

bool
shape_record(const Bins& bins, int n, cf* x, bool signal, double sigma, Rng& rng, TransformCache& transforms) {
    SplitTransform* t = transforms.get(n);
    if (!t || (signal && !t->forward(x))) {
        return false;
    }
    shape_bins(bins, signal, sigma, rng, *t);
    return t->inverse_from_conjugate(x, 1.0f / (float)n);
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
