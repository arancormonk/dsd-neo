// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DSP_AM_DEMOD: the carrier-normalised AM envelope detector (issue #524).
 *
 * A 1 kHz tone at 50% modulation depth must come out at 0.25 x 0.5 = 0.125 peak whatever the carrier level (the
 * detector divides the envelope by its own carrier estimate), with low distortion and no DC; a frequency offset leaves
 * the envelope alone; a reset (retune, family or FM/AM switch) warm-starts the carrier estimate from the next block
 * rather than fading in from the old channel's level; and a squelched block is silence that holds the estimate, so the
 * audio resumes at its level when the squelch opens. The last cases run the whole monitor pipeline (full_demod()) with
 * the 6 kHz AM channel filter, 30 dB carrier-to-noise (over the full 48 kHz complex band) and the I/Q DC blocker
 * switched on, which the AM path must bypass: it would remove a carrier tuned to 0 Hz.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;
constexpr int kBlockPairs = 960; /* 20 ms, the block the RTL monitor delivers at 48 kHz */
constexpr double kToneHz = 1000.0;
constexpr double kDepth = 0.5;
constexpr double kExpectedPeak = 0.25 * kDepth; /* the detector's 0.25 gain per unit of modulation */

/* Deterministic xorshift64* with Box-Muller, so every run sees the same noise. */
struct Prng {
    uint64_t s;

    double
    uniform() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (double)((s * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
    }

    double
    gauss() {
        double u1 = uniform();
        if (u1 < 1e-300) {
            u1 = 1e-300;
        }
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * uniform());
    }
};

/* A(1 + m cos(2 pi f_tone t)) e^{j 2 pi f_offset t} plus complex white noise at @p cnr_db below the carrier power
 * (A^2) over the full complex band. cnr_db <= 0 means no noise. */
struct AmSource {
    double carrier;
    double offset_hz;
    double cnr_db;
    Prng rng;
    long n;

    void
    fill(float* iq, int pairs) {
        const double sigma = cnr_db > 0.0 ? carrier * std::sqrt(0.5 * std::pow(10.0, -cnr_db / 10.0)) : 0.0;
        for (int k = 0; k < pairs; k++, n++) {
            const double t = (double)n / (double)kRate;
            const double env = carrier * (1.0 + kDepth * std::cos(2.0 * kPi * kToneHz * t));
            const double ph = 2.0 * kPi * offset_hz * t + 0.3;
            double i = env * std::cos(ph);
            double q = env * std::sin(ph);
            if (sigma > 0.0) {
                i += sigma * rng.gauss();
                q += sigma * rng.gauss();
            }
            iq[(size_t)(2 * k)] = (float)i;
            iq[(size_t)(2 * k) + 1] = (float)q;
        }
    }

    /* The detector's ideal output for sample @p index. */
    static double
    ideal(long index) {
        return kExpectedPeak * std::cos(2.0 * kPi * kToneHz * (double)index / (double)kRate);
    }
};

struct ToneFit {
    double peak;
    double dc;
    double thd_n_db; /* residual (everything but DC and the tone) against the tone, in dB */
};

/* Least-squares fit of DC plus the 1 kHz tone over a window of whole tone cycles. */
ToneFit
fit_tone(const std::vector<float>& y, size_t start, size_t count) {
    double a = 0.0;
    double b = 0.0;
    double dc = 0.0;
    for (size_t k = 0; k < count; k++) {
        const double w = 2.0 * kPi * kToneHz * (double)k / (double)kRate;
        const double v = (double)y[start + k];
        a += v * std::cos(w);
        b += v * std::sin(w);
        dc += v;
    }
    a *= 2.0 / (double)count;
    b *= 2.0 / (double)count;
    dc /= (double)count;
    double residual = 0.0;
    for (size_t k = 0; k < count; k++) {
        const double w = 2.0 * kPi * kToneHz * (double)k / (double)kRate;
        const double e = (double)y[start + k] - (dc + a * std::cos(w) + b * std::sin(w));
        residual += e * e;
    }
    residual /= (double)count;
    const double tone_power = 0.5 * (a * a + b * b);
    ToneFit fit;
    fit.peak = std::sqrt(a * a + b * b);
    fit.dc = dc;
    fit.thd_n_db = 10.0 * std::log10((residual > 1e-30 ? residual : 1e-30) / (tone_power > 1e-30 ? tone_power : 1e-30));
    return fit;
}

demod_state*
new_demod(void) {
    /* demod_state carries 64-byte-aligned members, so it needs the aligned allocator. */
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (s) {
        DSD_MEMSET(s, 0, sizeof(*s));
        s->rate_in = kRate;
        s->rate_out = kRate;
        s->post_downsample = 1;
        s->mode_demod = &dsd_am_demod;
    }
    return s;
}

/* Detector alone: one block of @p src through dsd_am_demod(), appended to @p out. */
void
am_block(demod_state* s, AmSource& src, std::vector<float>& out) {
    src.fill(s->input_cb_buf, kBlockPairs);
    s->lowpassed = s->input_cb_buf;
    s->lp_len = kBlockPairs * 2;
    dsd_am_demod(s);
    out.insert(out.end(), s->result, s->result + s->result_len);
}

int
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s\n", what);
        return 1;
    }
    return 0;
}

int
expect_level(const char* what, const ToneFit& fit, double tol_rel, double max_thd_n_db) {
    int rc = 0;
    if (std::fabs(fit.peak - kExpectedPeak) > tol_rel * kExpectedPeak) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s: tone peak %.5f, want %.4f +/- %.1f%%\n", what, fit.peak, kExpectedPeak,
                    tol_rel * 100.0);
        rc = 1;
    }
    if (fit.thd_n_db > max_thd_n_db) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s: THD+N %.2f dB, want <= %.1f dB\n", what, fit.thd_n_db, max_thd_n_db);
        rc = 1;
    }
    if (std::fabs(fit.dc) > 0.002) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s: DC %.5f, want about 0\n", what, fit.dc);
        rc = 1;
    }
    return rc;
}

/* Run @p seconds of @p src through the detector and fit the last whole second. */
ToneFit
run_detector(double carrier, double offset_hz, double cnr_db, double seconds) {
    demod_state* s = new_demod();
    ToneFit fit = {0.0, 0.0, 0.0};
    if (!s) {
        return fit;
    }
    AmSource src = {carrier, offset_hz, cnr_db, {0x9E3779B97F4A7C15ULL}, 0};
    std::vector<float> out;
    const int blocks = (int)(seconds * kRate / kBlockPairs);
    for (int b = 0; b < blocks; b++) {
        am_block(s, src, out);
    }
    fit = fit_tone(out, out.size() - (size_t)kRate, (size_t)kRate);
    dsd_neo_aligned_free(s);
    return fit;
}

/* The level is the modulation depth, not the carrier: 0.01, 0.1 and 1.0, and the same levels scaled by 1/pi (the live
 * RTL path's 1/pi output scaling applied before the detector would be the same story), at 30 dB CNR and without
 * noise. A +1.5 kHz carrier offset changes nothing: the envelope has no phase. */
int
test_level_independence(void) {
    int rc = 0;
    const double carriers[] = {1.0, 0.1, 0.01, 1.0 / kPi, 0.1 / kPi};
    for (double c : carriers) {
        char what[96];
        DSD_SNPRINTF(what, sizeof what, "carrier %.4f, clean", c);
        rc |= expect_level(what, run_detector(c, 0.0, 0.0, 1.5), 0.02, -40.0);
        DSD_SNPRINTF(what, sizeof what, "carrier %.4f, +1.5 kHz offset", c);
        rc |= expect_level(what, run_detector(c, 1500.0, 0.0, 1.5), 0.02, -40.0);
        DSD_SNPRINTF(what, sizeof what, "carrier %.4f, 30 dB CNR", c);
        const ToneFit noisy = run_detector(c, 0.0, 30.0, 1.5);
        if (std::fabs(noisy.peak - kExpectedPeak) > 0.02 * kExpectedPeak) {
            DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s: tone peak %.5f, want %.4f +/- 2%%\n", what, noisy.peak,
                        kExpectedPeak);
            rc = 1;
        }
    }
    const ToneFit x = run_detector(0.5, 0.0, 0.0, 1.5);
    const ToneFit x_pi = run_detector(0.5 / kPi, 0.0, 0.0, 1.5);
    rc |= expect("the same signal at x and x/pi does not give the same level",
                 std::fabs(x.peak - x_pi.peak) <= 1e-4 * kExpectedPeak);
    return rc;
}

/* The worst deviation from the ideal output over @p count samples from @p start (source sample @p first_index). */
double
worst_error(const std::vector<float>& out, size_t start, size_t count, long first_index) {
    double worst = 0.0;
    for (size_t k = 0; k < count; k++) {
        const double e = std::fabs((double)out[start + k] - AmSource::ideal(first_index + (long)k));
        if (e > worst) {
            worst = e;
        }
    }
    return worst;
}

/* A reset (what rtl_demod_reset_audio_monitor_state() does on a retune, family or FM/AM switch) zeroes the carrier
 * estimate. The next block warm-starts it from its own mean magnitude, so a station at another level is at its level
 * from the first sample; carried over, a 1.0 estimate on a 0.01 carrier would read as a full-scale negative step. */
int
test_reset_warm_start(void) {
    demod_state* s = new_demod();
    if (!s) {
        return 1;
    }
    int rc = 0;
    AmSource strong = {1.0, 0.0, 0.0, {1}, 0};
    std::vector<float> out;
    for (int b = 0; b < 25; b++) {
        am_block(s, strong, out);
    }
    rc |= expect("a running detector has a carrier estimate", std::fabs(s->am_carrier - 1.0f) < 0.01f);

    s->am_carrier = 0.0f; /* the reset */
    AmSource weak = {0.01, 1500.0, 0.0, {1}, strong.n};
    const size_t start = out.size();
    am_block(s, weak, out);
    const double worst = worst_error(out, start, (size_t)kBlockPairs, strong.n);
    if (worst > 0.005) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: first block after a reset deviates %.5f from the ideal output\n", worst);
        rc = 1;
    }
    rc |= expect("the reset warm-started the estimate at the new carrier", std::fabs(s->am_carrier - 0.01f) < 0.0002f);
    dsd_neo_aligned_free(s);
    return rc;
}

/* A squelched block (the pipeline zeroes it and sets channel_squelched) is silence, not the -0.25 a zero envelope
 * would read as, and the carrier estimate holds: the first unsquelched block is at its level from its first sample. */
int
test_squelch_hold(void) {
    demod_state* s = new_demod();
    if (!s) {
        return 1;
    }
    int rc = 0;
    AmSource src = {0.2, 0.0, 0.0, {1}, 0};
    std::vector<float> out;
    for (int b = 0; b < 25; b++) {
        am_block(s, src, out);
    }
    const float held = s->am_carrier;
    s->channel_squelched = 1;
    for (int b = 0; b < 5; b++) {
        for (int k = 0; k < kBlockPairs * 2; k++) {
            s->input_cb_buf[k] = 0.0f;
        }
        s->lowpassed = s->input_cb_buf;
        s->lp_len = kBlockPairs * 2;
        dsd_am_demod(s);
        rc |= expect("a squelched block keeps its length", s->result_len == kBlockPairs);
        for (int k = 0; k < s->result_len; k++) {
            if (std::fabs(s->result[k]) > 1e-9f) {
                DSD_FPRINTF(stderr, "DSP_AM_DEMOD: squelched output[%d] = %g, want silence\n", k, (double)s->result[k]);
                rc = 1;
                break;
            }
        }
        src.n += kBlockPairs; /* the source runs on while the squelch holds */
    }
    rc |= expect("the squelch moved the carrier estimate", std::fabs(s->am_carrier - held) < 1e-9f);
    s->channel_squelched = 0;
    const long first = src.n;
    const size_t start = out.size();
    am_block(s, src, out);
    const double worst = worst_error(out, start, (size_t)kBlockPairs, first);
    if (worst > 0.005) {
        DSD_FPRINTF(stderr, "DSP_AM_DEMOD: first block after the squelch opened deviates %.5f\n", worst);
        rc = 1;
    }
    dsd_neo_aligned_free(s);
    return rc;
}

/* No carrier at all (below the estimate floor) is silence, never a division by zero; the envelope clamps at +/-2
 * carrier units, so an impulse cannot drive the output past 0.5. */
int
test_floor_and_clamp(void) {
    demod_state* s = new_demod();
    if (!s) {
        return 1;
    }
    int rc = 0;
    for (int k = 0; k < kBlockPairs * 2; k++) {
        s->input_cb_buf[k] = 0.0f;
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = kBlockPairs * 2;
    dsd_am_demod(s);
    int silent = s->result_len == kBlockPairs;
    for (int k = 0; k < s->result_len; k++) {
        silent &= std::isfinite(s->result[k]) && std::fabs(s->result[k]) <= 1e-9f;
    }
    rc |= expect("no carrier is not silence", silent);

    s->am_carrier = 0.5f;
    for (size_t k = 0; k < 8; k++) {
        s->input_cb_buf[2 * k] = (k == 3) ? 20.0f : 0.5f;
        s->input_cb_buf[(2 * k) + 1] = 0.0f;
    }
    s->lp_len = 16;
    dsd_am_demod(s);
    float peak = 0.0f;
    for (int k = 0; k < s->result_len; k++) {
        peak = std::fabs(s->result[k]) > peak ? std::fabs(s->result[k]) : peak;
    }
    rc |= expect("an impulse passed the envelope clamp", peak <= 0.5f + 1e-6f && peak >= 0.5f - 1e-3f);
    s->lp_len = 0;
    dsd_am_demod(s);
    rc |= expect("an empty block produced samples", s->result_len == 0);
    dsd_neo_aligned_free(s);
    return rc;
}

/* The whole monitor pipeline as the AM front end runs it: the 6 kHz channel filter, the AM detector, no
 * de-emphasis, the audio DC blocker and an open squelch envelope, with the I/Q DC blocker switched on. */
void
prime_am_pipeline(demod_state* s) {
    s->analog_family = 1;
    s->analog_demod = DSD_ANALOG_DEMOD_AM;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_width_hz = DSD_ANALOG_AM_WIDTH_DEFAULT_HZ;
    s->deemph = 0;
    s->dc_block = 1;
    s->rate_out2 = 0;
    s->squelch_env = 1.0f;
    s->squelch_gate_open = 1;
    s->iq_dc_block_enable = 1;
    s->iq_dc_shift = 11;
}

ToneFit
run_pipeline(double carrier, double offset_hz, double cnr_db, float* out_iq_dc_r) {
    demod_state* s = new_demod();
    ToneFit fit = {0.0, 0.0, 0.0};
    if (!s) {
        return fit;
    }
    prime_am_pipeline(s);
    AmSource src = {carrier, offset_hz, cnr_db, {0x2545F4914F6CDD1DULL}, 0};
    std::vector<float> out;
    for (int b = 0; b < 100; b++) { /* 2 s */
        src.fill(s->input_cb_buf, kBlockPairs);
        s->lowpassed = s->input_cb_buf;
        s->lp_len = kBlockPairs * 2;
        full_demod(s);
        out.insert(out.end(), s->result, s->result + s->result_len);
    }
    fit = fit_tone(out, out.size() - (size_t)kRate, (size_t)kRate);
    *out_iq_dc_r = s->iq_dc_avg_r;
    dsd_neo_aligned_free(s);
    return fit;
}

/* A typed digital scan row's symbol profile on an AM session keeps the monitor output but puts the row's channel
 * profile in place of the analog one (dsd_demod_analog_monitor_active() reads 0): the row's digital signal is then
 * FM-demodulated, as under -fA, not run through the AM detector, whose carrier estimate the row leaves alone; the I/Q
 * DC blocker and the output scale apply to it as to FM. */
int
test_typed_row_profile_under_am(void) {
    demod_state* s = new_demod();
    if (!s) {
        return 1;
    }
    int rc = 0;
    prime_am_pipeline(s);
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    s->am_carrier = 0.5f;
    rc |= expect("a typed row under AM reported as the AM detector", dsd_demod_am_active(s) == 0);
    rc |= expect("a typed row under AM keeps the I/Q DC blocker off", dsd_demod_iq_dc_block_active(s) == 1);
    /* A constant 3 kHz frequency deviation: the FM discriminator reads a constant phase step, the AM detector a flat
     * envelope (0). */
    const double dphi = 2.0 * kPi * 3000.0 / (double)kRate;
    for (int k = 0; k < kBlockPairs; k++) {
        s->input_cb_buf[(size_t)(2 * k)] = (float)(0.5 * std::cos(dphi * k));
        s->input_cb_buf[(size_t)(2 * k) + 1] = (float)(0.5 * std::sin(dphi * k));
    }
    s->iq_dc_block_enable = 0;
    s->dc_block = 0;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = kBlockPairs * 2;
    full_demod(s);
    rc |= expect("a typed row under AM produced no samples", s->result_len > 100);
    if (s->result_len > 100) {
        const double got = (double)s->result[s->result_len / 2]; /* past the channel filter's start-up */
        if (std::fabs(got - dphi) > 0.02) {
            DSD_FPRINTF(stderr, "DSP_AM_DEMOD: typed row under AM: output %.4f, want the FM phase step %.4f\n", got,
                        dphi);
            rc = 1;
        }
    }
    rc |= expect("a typed row moved the AM carrier estimate", std::fabs(s->am_carrier - 0.5f) < 1e-9f);
    dsd_neo_aligned_free(s);
    return rc;
}

int
test_pipeline(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return 1;
    }
    prime_am_pipeline(s);
    rc |= expect("the AM detector is not reported as active", dsd_demod_am_active(s) == 1);
    rc |= expect("the I/Q DC blocker is reported as running under AM", dsd_demod_iq_dc_block_active(s) == 0);
    s->mode_demod = &dsd_fm_demod;
    rc |= expect("FM reported as AM", dsd_demod_am_active(s) == 0);
    rc |= expect("the I/Q DC blocker is not reported as running under FM", dsd_demod_iq_dc_block_active(s) == 1);
    s->iq_dc_block_enable = 0;
    rc |= expect("a disabled I/Q DC blocker is reported as running", dsd_demod_iq_dc_block_active(s) == 0);
    rc |= expect("no state reports AM", dsd_demod_am_active(NULL) == 0 && dsd_demod_iq_dc_block_active(NULL) == 0);
    dsd_neo_aligned_free(s);

    rc |= test_typed_row_profile_under_am();

    const double offsets[] = {0.0, 1500.0};
    const double carriers[] = {1.0, 0.1, 0.01};
    for (double off : offsets) {
        for (double c : carriers) {
            float iq_dc = 1.0f;
            char what[96];
            DSD_SNPRINTF(what, sizeof what, "pipeline, carrier %.2f at %+.0f Hz, 30 dB CNR", c, off);
            rc |= expect_level(what, run_pipeline(c, off, 30.0, &iq_dc), 0.02, -30.0);
            /* Bypassed, not merely harmless: the blocker's estimate never moved off the carrier. */
            if (std::fabs(iq_dc) > 1e-12f) {
                DSD_FPRINTF(stderr, "DSP_AM_DEMOD: %s: the I/Q DC blocker ran (estimate %g)\n", what, (double)iq_dc);
                rc = 1;
            }
        }
    }
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    rc |= test_level_independence();
    rc |= test_reset_warm_start();
    rc |= test_squelch_hold();
    rc |= test_floor_and_clamp();
    rc |= test_pipeline();
    if (rc == 0) {
        std::printf("DSP_AM_DEMOD: OK\n");
    }
    return rc;
}
