// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Receiver responses and capture coordinates for the scan quiet check's calibration driver
 * (tests/io/signal_quiet_calibrate.cpp), kept apart so tests/io/test_io_signal_quiet_scene.cpp can check them against
 * fixtures of its own.
 *
 * A response is fixed in hardware coordinates: hardware frequency = the samples' frequency less the capture's ingest
 * rotation, folded into [-Fs/2, Fs/2) (hardware_hz()). A shifted capture (the RTL tap's geometry, rotation Fs/4) puts
 * the channel at hardware -Fs/4; a native Airspy's centred one (rotation 0) puts it at the hardware centre.
 *
 * Every shape but FLAT (no response at all) is a modelled Airspy passband: libairspy's host half-band times the R820T
 * tuner's IF filter, as a power response.
 *   half-band  modelled from figures computed from libairspy's taps, not taken from its code: 0 dB to +-0.42 Fs, then
 *              an amplitude [(1 + cos(pi u)) / 2]^p, u = (|f| - 0.42 Fs) / (kHalfbandWidth Fs), whose two parameters
 *              put it at -0.35 dB at +-0.45 Fs and -6.02 dB (half amplitude) at +-0.5 Fs
 *   tuner      flat between its -3 dB points, a raised-cosine power edge kTunerTransition wide centred on each, over
 *              a -40 dB stop band:
 *     NOMINAL   2.5 and 3 MS/s: -0.335 / +0.302 Fs, the one published measurement (an R2 at 2.5 MS/s; 3 MS/s, a
 *               Mini's narrow IF mode, assumed alike), edges 0.10 Fs wide. 6 and 10 MS/s: the vendor's usable spans
 *               (5 of 6 MHz, 9 of 10 MHz) as the edges' mean, +-0.42 and +-0.45 Fs, with the same 0.033 Fs lopsidedness
 *               in the same sense: -0.4365 / +0.4035 Fs and -0.4665 / +0.4335 Fs, edges 0.06 Fs wide
 *     STEEPER   the stress case: -3 dB at +-0.28 Fs (2.5, 3 MS/s) and +-0.40 Fs (6, 10 MS/s), same edge widths
 *     MIRRORED  NOMINAL with the steeper side on the other side of the centre
 * Options on top of a shape:
 *   depression  the power drops by depression_db over 3 channel widths centred on the channel's hardware frequency
 *               (full depth to 1.375 widths each side, a raised-cosine taper to none at 1.625, half at 1.5)
 *   adc         flat ADC noise, adc_db over the tuner noise's peak density, added after the response: the tuner
 *               noise follows the response, the ADC noise does not
 * Wanted signals, neighbours and interferers pass through the response like the tuner noise. */
#ifndef DSD_NEO_TESTS_IO_SIGNAL_QUIET_SCENE_H_
#define DSD_NEO_TESTS_IO_SIGNAL_QUIET_SCENE_H_

#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>
#include "rtl_fft_cache.h"

namespace dsd_quiet_scene {

using cf = std::complex<float>;

enum class Shape : uint8_t { FLAT, NOMINAL, STEEPER, MIRRORED };

struct Response {
    Shape shape = Shape::FLAT;
    double depression_db = 0; /* > 0: the channel-local depression's depth */
    bool adc = false;         /* flat ADC noise added after the response */
    double adc_db = 0;        /* its density over the tuner noise's peak density */
};

/* Where a condition's channel is: the capture rate, the ingest rotation (the channel's sample frequency less its
 * hardware frequency) and the channel's frequency in the samples, with the width the depression is measured in. */
struct Placement {
    uint32_t rate;
    double rotation_hz;
    double channel_sample_hz;
    double width_hz;
};

/* A tuner IF response's -3 dB points (signed hardware frequencies as a share of the rate) and edge width. */
struct TunerEdges {
    double low, high, transition;
};

/* "flat", "nominal", "steeper" or "mirrored"; false for anything else. */
bool parse_shape(const char* text, Shape* out);
const char* shape_text(Shape shape);

/* The modelled half-band's power response at @p fraction of the rate (either sign). */
double halfband_power(double fraction);
/* @p shape's tuner edges at @p rate; false for FLAT or a rate the shape is not fitted for (2.5, 3, 6, 10 MS/s). */
bool tuner_edges(Shape shape, uint32_t rate, TunerEdges* out);
/* The tuner's power response at @p fraction of the rate (signed hardware frequency). */
double tuner_power(const TunerEdges& edges, double fraction);
/* Whether @p r can be rendered at @p rate. */
bool response_supported(const Response& r, uint32_t rate);

/* @p sample_hz less @p rotation_hz, folded into [-rate/2, rate/2). */
double hardware_hz(double sample_hz, double rotation_hz, double rate);
/* The response's power at hardware frequency @p hw_hz: half-band x tuner x depression; 1 for FLAT. */
double response_power(const Response& r, const Placement& p, double hw_hz);

/* Per-bin factors of an n-point record at the capture rate, bin k at k rate / n (the upper half negative): the
 * amplitude a signal is scaled by, sqrt(response), and the noise's standard deviation over the base noise's,
 * sqrt(response x colour + ADC share). @p colour, when given, multiplies the tuner noise's density at a sample
 * frequency (a sloped or humped floor). */
struct Bins {
    std::vector<float> signal, noise;
};

void response_bins(const Response& r, const Placement& p, int n, const std::function<double(double)>* colour,
                   Bins* out);

/* xoshiro256** seeded by splitmix64: every trial's samples come from its own seed. */
struct Rng {
    uint64_t s[4];

    static uint64_t
    splitmix(uint64_t* x) {
        uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    explicit Rng(uint64_t seed) {
        for (uint64_t& v : s) {
            v = splitmix(&seed);
        }
    }

    uint64_t
    next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    static uint64_t
    rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    double
    uniform() { /* (0, 1] */
        return ((next() >> 11) + 1) * (1.0 / 9007199254740992.0);
    }

    /* Complex Gaussian, each component sigma (Box-Muller). */
    cf
    gauss(double sigma) {
        constexpr double kTwoPi = 6.28318530717958647692;
        const double r = sigma * std::sqrt(-2.0 * std::log(uniform())), t = kTwoPi * uniform();
        return cf((float)(r * std::cos(t)), (float)(r * std::sin(t)));
    }
};

/* Inverse transform through the forward one: ifft(X) = conj(fft(conj(X))), unscaled; false when the transform rejects
 * @p n. */
bool inverse(dsd_io::FftSetupCache& fft, int n, cf* data);

/* One record through the response, in place: @p x holds n time-domain samples of every transmitter (@p signal) or
 * none (its contents are ignored), and becomes them through the response plus the response's noise, sigma per
 * component at a bin whose noise factor is 1 (sigma 0 adds none). The record is circular: a transmitter's start and
 * end smear by the response's impulse response, so callers keep its edges out of a trial. False (@p x unchanged when
 * there is no signal, or transformed) when the transform rejects @p n. */
bool shape_record(const Bins& bins, int n, cf* x, bool signal, double sigma, Rng& rng, dsd_io::FftSetupCache& fft);

/* The smallest record length of at least @p count points shape_record()'s transform takes (2^a 3^b 5^c, a >= 4). */
int record_size(int count);

} // namespace dsd_quiet_scene

#endif
