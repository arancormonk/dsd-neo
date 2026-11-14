// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: audio_lpf_filter and dc_block_filter behavior. */

#include <cmath>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <dsd-neo/runtime/mem.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
monotonic_nondecreasing(const float* x, int n) {
    for (int i = 1; i < n; i++) {
        if (x[i] < x[i - 1]) {
            return 0;
        }
    }
    return 1;
}

typedef float (*sps_filter_fn)(float sample, int samples_per_symbol);

static int
expect_sps_filter_response(const char* name, sps_filter_fn filter, int base_sps, int redesign_sps) {
    init_rrc_filter_memory();
    if (filter(123.0f, 1) != 123.0f) {
        DSD_FPRINTF(stderr, "%s: sps<=1 bypass failed\n", name);
        return 1;
    }

    float base_sum = 0.0f;
    float base_abs = 0.0f;
    for (int i = 0; i < 160; i++) {
        float y = filter(i == 0 ? 1.0f : 0.0f, base_sps);
        if (!std::isfinite(y)) {
            DSD_FPRINTF(stderr, "%s: non-finite base response at %d\n", name, i);
            return 1;
        }
        base_sum += y;
        base_abs += std::fabs(y);
    }
    if (!(base_abs > 0.01f && base_sum > 0.80f && base_sum < 1.20f)) {
        DSD_FPRINTF(stderr, "%s: unexpected base response sum=%f abs=%f\n", name, base_sum, base_abs);
        return 1;
    }

    init_rrc_filter_memory();
    float redesign_sum = 0.0f;
    float redesign_abs = 0.0f;
    for (int i = 0; i < 220; i++) {
        float y = filter(i == 0 ? 1.0f : 0.0f, redesign_sps);
        if (!std::isfinite(y)) {
            DSD_FPRINTF(stderr, "%s: non-finite redesign response at %d\n", name, i);
            return 1;
        }
        redesign_sum += y;
        redesign_abs += std::fabs(y);
    }
    if (!(redesign_abs > 0.01f && redesign_sum > 0.80f && redesign_sum < 1.20f)) {
        DSD_FPRINTF(stderr, "%s: unexpected redesign response sum=%f abs=%f\n", name, redesign_sum, redesign_abs);
        return 1;
    }
    return 0;
}

/*
 * The post-discriminator filters are each mode's receive pulse (issue #834). Two spectra, f in units of the symbol
 * rate, roll-off 0.2 both:
 *   DMR and YSF: the square root raised cosine, ETSI TS 102 361-1 clause 10.2.2.2 (1 to 0.4, -3 dB at 0.5, 0 from 0.6).
 *   NXDN48, NXDN96 and dPMR: that square root raised cosine times the inverse sinc pi f / sin(pi f), NXDN TS 1-A 3.5
 *   and ETSI TS 102 658 clause 14.2.2.2.
 */
static const double kPi = 3.14159265358979323846;
static const double kRollOff = 0.2;

static double
srrc_spectrum(double f) {
    const double lo = (1.0 - kRollOff) / 2.0;
    const double hi = (1.0 + kRollOff) / 2.0;
    if (f <= lo) {
        return 1.0;
    }
    if (f >= hi) {
        return 0.0;
    }
    return std::cos(kPi / (4.0 * kRollOff) * (2.0 * f - (1.0 - kRollOff)));
}

static double
nxdn_rx_spectrum(double f) {
    if (f < 1e-9) {
        return 1.0;
    }
    return srrc_spectrum(f) * (kPi * f) / std::sin(kPi * f);
}

/* The square root raised cosine's impulse response at t symbols, the closed form (t = 0 and |4 a t| = 1 apart). */
static double
srrc_pulse(double t) {
    const double a = kRollOff;
    if (std::fabs(t) < 1e-9) {
        return 1.0 + a * (4.0 / kPi - 1.0);
    }
    if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-9) {
        return a / std::sqrt(2.0)
               * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * a)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * a)));
    }
    return (std::sin(kPi * t * (1.0 - a)) + 4.0 * a * t * std::cos(kPi * t * (1.0 + a)))
           / (kPi * t * (1.0 - 16.0 * a * a * t * t));
}

/* The NXDN receive pulse at t symbols: the inverse transform of nxdn_rx_spectrum(), by the midpoint rule over
   [0, 0.6] with 0.4, where the roll-off starts, on a cell edge. */
static double
nxdn_rx_pulse(double t) {
    const int cells = 6000;
    const double width = 0.6 / cells;
    double sum = 0.0;
    for (int i = 0; i < cells; i++) {
        const double f = (i + 0.5) * width;
        sum += nxdn_rx_spectrum(f) * std::cos(2.0 * kPi * f * t);
    }
    return 2.0 * sum * width;
}

namespace {
struct pulse_case {
    dsd_sps_filter_kind kind;
    int sps;
    const char* modes;
    double (*spectrum)(double f);
    double (*pulse)(double t); /* NULL: only the spectrum is pinned at this rate */
};
} // namespace

/* The filter's impulse response at @p sps, unity DC gain as the design makes it; returns the tap count. */
static int
filter_taps(dsd_sps_filter_kind kind, int sps, double* taps, int max_taps) {
    const int len = 2 * dsd_sps_filter_group_delay(kind, sps) + 1;
    if (len > max_taps) {
        return 0;
    }
    init_rrc_filter_memory();
    for (int n = 0; n < len; n++) {
        taps[n] = dsd_sps_filter_apply(kind, n == 0 ? 1.0f : 0.0f, sps);
    }
    return len;
}

/* |H(f)| of symmetric taps, f in units of the symbol rate. */
static double
taps_magnitude(const double* taps, int len, int sps, double f) {
    const double centre = 0.5 * (len - 1);
    double re = 0.0;
    double im = 0.0;
    for (int n = 0; n < len; n++) {
        const double w = 2.0 * kPi * f * (n - centre) / sps;
        re += taps[n] * std::cos(w);
        im -= taps[n] * std::sin(w);
    }
    return std::sqrt(re * re + im * im);
}

/*
 * The spectrum at points across the pass band and the roll-off, and the stop band's edge. The tolerances are what
 * truncating four symbols either side leaves: the NXDN filter reads 1.384 at 0.4 of the symbol rate against 1.321, and
 * 0.21 at 0.6. The filters these replaced read 0.54-0.97 at 0.6, and the 48 kHz ones 1.0 within 0.03 at half the
 * symbol rate and at 0.4.
 */
static int
expect_spectrum(const pulse_case& c, const double* taps, int len) {
    static const double kPassBand[] = {0.25, 0.4, 0.5};
    static const double kPassTolerance = 0.08;
    static const double kStopEdge = 0.6;
    static const double kStopCeiling = 0.25;
    int rc = 0;
    for (double f : kPassBand) {
        const double got = taps_magnitude(taps, len, c.sps, f);
        const double want = c.spectrum(f);
        if (std::fabs(got - want) > kPassTolerance) {
            DSD_FPRINTF(stderr, "%s sps=%d: |H(%.2f Rs)| = %.3f, the standard's pulse %.3f +/- %.2f\n", c.modes, c.sps,
                        f, got, want, kPassTolerance);
            rc = 1;
        }
    }
    const double stop = taps_magnitude(taps, len, c.sps, kStopEdge);
    if (stop > kStopCeiling) {
        DSD_FPRINTF(stderr, "%s sps=%d: |H(%.2f Rs)| = %.3f, the standard's pulse 0 (at most %.2f)\n", c.modes, c.sps,
                    kStopEdge, stop, kStopCeiling);
        rc = 1;
    }
    return rc;
}

/* Inside its span, the filter is the standard's receive pulse sampled every 1/sps symbol, to float precision. */
static int
expect_pulse(const pulse_case& c, const double* taps, int len) {
    const int centre = (len - 1) / 2;
    const double peak = c.pulse(0.0);
    for (int n = 0; n < len; n++) {
        const double got = taps[n] / taps[centre];
        const double want = c.pulse(static_cast<double>(n - centre) / c.sps) / peak;
        if (std::fabs(got - want) > 1e-4) {
            DSD_FPRINTF(stderr, "%s sps=%d tap %d: %.6f, the standard's pulse %.6f\n", c.modes, c.sps, n - centre, got,
                        want);
            return 1;
        }
    }
    return 0;
}

static int
test_sps_filters_are_each_modes_receive_pulse(void) {
    static const pulse_case kCases[] = {
        {DSD_SPS_FILTER_DMR, 10, "DMR/YSF", srrc_spectrum, srrc_pulse},
        {DSD_SPS_FILTER_DMR, 5, "DMR/YSF", srrc_spectrum, srrc_pulse},
        {DSD_SPS_FILTER_DMR, 20, "DMR/YSF", srrc_spectrum, srrc_pulse},
        {DSD_SPS_FILTER_NXDN, 20, "NXDN48", nxdn_rx_spectrum, nxdn_rx_pulse},
        {DSD_SPS_FILTER_NXDN, 10, "NXDN96", nxdn_rx_spectrum, NULL},
        {DSD_SPS_FILTER_NXDN, 5, "NXDN96", nxdn_rx_spectrum, NULL},
        {DSD_SPS_FILTER_DPMR, 20, "dPMR", nxdn_rx_spectrum, nxdn_rx_pulse},
        {DSD_SPS_FILTER_DPMR, 10, "dPMR", nxdn_rx_spectrum, NULL},
    };
    static double taps[DSD_SPS_FILTER_MAX_TAPS];
    int rc = 0;
    for (const pulse_case& c : kCases) {
        const int len = filter_taps(c.kind, c.sps, taps, DSD_SPS_FILTER_MAX_TAPS);
        if (len < 3) {
            DSD_FPRINTF(stderr, "%s sps=%d: no filter\n", c.modes, c.sps);
            rc = 1;
            continue;
        }
        rc |= expect_spectrum(c, taps, len);
        if (c.pulse) {
            rc |= expect_pulse(c, taps, len);
        }
    }
    return rc;
}

static int
test_sps_filter_wrappers(void) {
    int rc = 0;
    rc |= expect_sps_filter_response("dmr", dmr_filter, 10, 8);
    rc |= expect_sps_filter_response("nxdn", nxdn_filter, 20, 10);
    rc |= expect_sps_filter_response("dpmr", dpmr_filter, 20, 12);
    rc |= expect_sps_filter_response("p25", p25_filter, 10, 8);
    return rc;
}

int
main(void) {
    // Allocate demod_state on heap
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    // Test audio_lpf_filter on step input
    {
        const int N = 64;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 1.0f; // step from 0 (state) to 1.0
        }
        s->audio_lpf_enable = 1;
        s->audio_lpf_alpha = 0.25f;
        s->audio_lpf_state = 0.0f;
        audio_lpf_filter(s);
        if (!monotonic_nondecreasing(s->result, s->result_len)) {
            DSD_FPRINTF(stderr, "audio_lpf_filter: not monotonic nondecreasing on step\n");
            dsd_neo_aligned_free(s);
            return 1;
        }
        // Final value should approach target (allow some residual)
        if (!(s->result[N - 1] >= 0.9f && s->result[N - 1] <= 1.0f)) {
            DSD_FPRINTF(stderr, "audio_lpf_filter: final=%f not near 1.0\n", s->result[N - 1]);
            dsd_neo_aligned_free(s);
            return 1;
        }
    }

    // Test dc_block_filter on DC input: output should trend down from initial value
    {
        const int N = 256;
        s->result_len = N;
        for (int i = 0; i < N; i++) {
            s->result[i] = 0.5f;
        }
        s->dc_avg = 0.0f; // initial DC estimate
        dc_block_filter(s);
        // Should be non-increasing sequence and significantly reduced by end
        for (int i = 1; i < N; i++) {
            if (s->result[i] > s->result[i - 1]) {
                DSD_FPRINTF(stderr, "dc_block_filter: sequence increased at %d\n", i);
                dsd_neo_aligned_free(s);
                return 1;
            }
        }
        float last = s->result[N - 1];
        if (last >= 0.5f) {
            DSD_FPRINTF(stderr, "dc_block_filter: insufficient reduction (last=%f)\n", last);
            dsd_neo_aligned_free(s);
            return 1;
        }
    }

    if (test_sps_filter_wrappers() != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    if (test_sps_filters_are_each_modes_receive_pulse() != 0) {
        dsd_neo_aligned_free(s);
        return 1;
    }

    dsd_neo_aligned_free(s);
    return 0;
}
