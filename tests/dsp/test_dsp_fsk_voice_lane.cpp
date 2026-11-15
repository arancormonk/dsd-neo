// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DSP_FSK_VOICE_LANE: the FSK discriminator's voice lane, which EDACS plays analog calls from (issue #518 B10).
 *
 * The FSK lane the release register reads is the discriminator on its fixed scale, with no de-emphasis. The voice lane
 * is the same discriminator output ahead of that scale, at the FM monitor's output scale and through its own
 * de-emphasis. Through full_demod():
 *
 * - 1 kHz at 3 kHz deviation reads 0.125 peak once dsd_demod_deemph_compensation() makes up the lane's de-emphasis, at
 *   24, 48 and 72 kHz and with no, 50, 75 and 750 us de-emphasis; the compensation is the monitor's for the same
 *   coefficient and rate, and 1 with the lane off.
 * - The FSK samples are bit for bit the same with the lane on or off.
 * - A block the level squelch closes is silence on both lanes, and the lane resumes when it opens; such a block, the
 *   lane's first or the first at a new rate, already compensates the lane's de-emphasis at that rate.
 * - The lane's coefficient follows a change of rate.
 * - With the carrier off centre the lane is centred, and a lane switched on mid-stream is, sample for sample, the lane
 *   of a demodulator that ran it all along, across a squelched block and a reconfiguration at a new rate.
 * - Through the output resampler, the lane makes exactly as many samples as the FSK lane at the same places:
 *   fed the same input, it makes the same samples, block for block, whatever the block sizes, and after a reset.
 *
 * A passing run prints only its OK line.
 */

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 1000.0;
constexpr double kDeviationHz = 3000.0;
constexpr double kReferencePeak = 0.125; /* 1 kHz at 3 kHz deviation at the monitor's scale */

int g_failures = 0;

void
fail(const char* what) {
    DSD_FPRINTF(stderr, "DSP_FSK_VOICE_LANE: %s\n", what);
    g_failures++;
}

/* A carrier frequency-modulated by 1 kHz at 3 kHz deviation, kept across blocks. */
struct FmSource {
    int rate_hz;
    double phase;
    long n;
    double offset_hz = 0.0; /* the carrier's offset from the channel centre */

    void
    fill(float* iq, int pairs) {
        for (int k = 0; k < pairs; k++, n++) {
            const double t = (double)n / (double)rate_hz;
            phase += 2.0 * kPi * (offset_hz + kDeviationHz * std::cos(2.0 * kPi * kToneHz * t)) / (double)rate_hz;
            iq[(size_t)(2 * k)] = (float)(0.5 * std::cos(phase));
            iq[(size_t)(2 * k) + 1] = (float)(0.5 * std::sin(phase));
        }
    }
};

demod_state*
new_fsk_demod(int rate_hz, int lane, int tau_us) {
    /* demod_state carries 64-byte-aligned members, so it needs the aligned allocator. */
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return NULL;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->complex_rate = rate_hz;
    s->post_downsample = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    s->symbol_rate_hz = 9600;
    s->symbol_levels = 2;
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    s->voice_lane_enabled = lane;
    s->voice_lane_tau_us = tau_us;
    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = rate_hz;
    cfg.symbol_rate_hz = 9600;
    cfg.levels = 2;
    dsd_fsk_modem_init(&s->fsk_modem_state, &cfg);
    return s;
}

/* One 10 ms block of @p src through full_demod(). */
void
run_block(demod_state* s, FmSource& src) {
    const int pairs = src.rate_hz / 100;
    src.fill(s->input_cb_buf, pairs);
    s->lowpassed = s->input_cb_buf;
    s->lp_len = pairs * 2;
    full_demod(s);
}

/* The peak of the 1 kHz tone in @p y (whole cycles at @p rate_hz), least squares. */
double
tone_peak(const std::vector<float>& y, int rate_hz) {
    double a = 0.0;
    double b = 0.0;
    const size_t cycle = (size_t)rate_hz / 1000U;
    const size_t count = (y.size() / cycle) * cycle;
    for (size_t k = 0; k < count; k++) {
        const double w = 2.0 * kPi * kToneHz * (double)k / (double)rate_hz;
        a += (double)y[k] * std::cos(w);
        b += (double)y[k] * std::sin(w);
    }
    a *= 2.0 / (double)count;
    b *= 2.0 / (double)count;
    return std::sqrt(a * a + b * b);
}

/* The monitor's compensation for the coefficient the lane runs at @p rate_hz, as dsd_demod_deemph_compensation()
   computes it for the FM monitor. */
double
monitor_compensation(int rate_hz, int tau_us) {
    demod_state* m = new_fsk_demod(rate_hz, 0, 0);
    if (!m) {
        return -1.0;
    }
    m->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    m->mode_demod = &dsd_fm_demod;
    m->deemph = tau_us > 0 ? 1 : 0;
    m->deemph_a = dsd_demod_deemph_alpha(rate_hz, tau_us);
    const double g = dsd_demod_deemph_compensation(m);
    dsd_neo_aligned_free(m);
    return g;
}

void
test_level_at_rates_and_deemphasis(void) {
    static const int kRates[] = {24000, 48000, 72000};
    static const int kTaus[] = {0, 50, 75, 750};
    for (int rate : kRates) {
        for (int tau : kTaus) {
            demod_state* s = new_fsk_demod(rate, 1, tau);
            if (!s) {
                fail("allocation");
                return;
            }
            FmSource src = {rate, 0.0, 0};
            std::vector<float> voice;
            for (int b = 0; b < 60; b++) {
                run_block(s, src);
                if (!s->voice_lane_active || s->result_len != rate / 100) {
                    fail("an FSK block with the lane asked for made no lane");
                    break;
                }
                if (b >= 20) { /* past the de-emphasis and the carrier centring settling */
                    voice.insert(voice.end(), s->voice_result, s->voice_result + s->result_len);
                }
            }
            const double comp = dsd_demod_deemph_compensation(s);
            const double peak = tone_peak(voice, rate) * comp;
            if (std::fabs(peak - kReferencePeak) > 0.01 * kReferencePeak) {
                DSD_FPRINTF(stderr, "  %d Hz, %d us: peak %.5f (compensation %.4f), want %.3f +/-1%%\n", rate, tau,
                            peak, comp, kReferencePeak);
                fail("the reference tone does not read 0.125 peak on the voice lane");
            }
            const double want_comp = monitor_compensation(rate, tau);
            if (std::fabs(comp - want_comp) > 1e-12 || (tau == 0 && std::fabs(comp - 1.0) > 1e-12)) {
                DSD_FPRINTF(stderr, "  %d Hz, %d us: compensation %.6f, the monitor's %.6f\n", rate, tau, comp,
                            want_comp);
                fail("the lane's compensation is not the monitor's for its coefficient and rate");
            }
            s->voice_lane_enabled = 0;
            run_block(s, src);
            if (s->voice_lane_active || std::fabs(dsd_demod_deemph_compensation(s) - 1.0) > 1e-12) {
                fail("with the lane off a block makes it, or the compensation is not 1");
            }
            dsd_neo_aligned_free(s);
        }
    }
}

int
same_bits(const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t x = 0U;
        uint32_t y = 0U;
        DSD_MEMCPY(&x, &a[i], sizeof(x));
        DSD_MEMCPY(&y, &b[i], sizeof(y));
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

void
test_fsk_lane_unchanged(void) {
    demod_state* off = new_fsk_demod(48000, 0, 75);
    demod_state* on = new_fsk_demod(48000, 1, 75);
    if (!off || !on) {
        fail("allocation");
        dsd_neo_aligned_free(off);
        dsd_neo_aligned_free(on);
        return;
    }
    FmSource a = {48000, 0.0, 0};
    FmSource b = {48000, 0.0, 0};
    for (int blk = 0; blk < 50; blk++) {
        /* The lane goes off and on again part-way through. */
        on->voice_lane_enabled = (blk / 10) % 2 == 0 ? 1 : 0;
        run_block(off, a);
        run_block(on, b);
        if (off->result_len != on->result_len || !same_bits(off->result, on->result, off->result_len)) {
            fail("the FSK samples change with the voice lane");
            break;
        }
    }
    dsd_neo_aligned_free(off);
    dsd_neo_aligned_free(on);
}

void
test_squelched_block_is_silence(void) {
    demod_state* s = new_fsk_demod(48000, 1, 75);
    if (!s) {
        fail("allocation");
        return;
    }
    FmSource src = {48000, 0.0, 0};
    for (int b = 0; b < 10; b++) {
        run_block(s, src);
    }
    /* The carrier's power (0.25) under the level: the block is squelched. */
    s->channel_squelch_level.store(1.0f);
    run_block(s, src);
    int silent = s->voice_lane_active && s->channel_squelched && std::fabs(s->voice_lane_deemph_avg) < 1e-12f;
    for (int i = 0; i < s->result_len; i++) {
        silent &= std::fabs(s->voice_result[i]) < 1e-12f && std::fabs(s->result[i]) < 1e-12f;
    }
    if (!silent) {
        fail("a squelched block is not silence on both lanes");
    }
    s->channel_squelch_level.store(0.0f);
    std::vector<float> voice;
    for (int b = 0; b < 30; b++) {
        run_block(s, src);
        if (b >= 10) {
            voice.insert(voice.end(), s->voice_result, s->voice_result + s->result_len);
        }
    }
    const double peak = tone_peak(voice, 48000) * dsd_demod_deemph_compensation(s);
    if (std::fabs(peak - kReferencePeak) > 0.01 * kReferencePeak) {
        fail("the lane does not resume at its level when the squelch opens");
    }
    dsd_neo_aligned_free(s);
}

/* A lane whose first block, or first block at a new rate, the squelch closes already compensates its own de-emphasis
   at that rate, so the gain the stream publishes does not move again when the squelch opens. */
void
test_squelched_block_publishes_the_lane_gain(void) {
    demod_state* s = new_fsk_demod(48000, 1, 75);
    if (!s) {
        fail("allocation");
        return;
    }
    s->channel_squelch_level.store(1.0f);
    FmSource src = {48000, 0.0, 0};
    run_block(s, src);
    const double want48 = monitor_compensation(48000, 75);
    if (!s->channel_squelched || std::fabs(dsd_demod_deemph_compensation(s) - want48) > 1e-12) {
        fail("a lane's first block, squelched, does not publish the lane's gain");
    }
    s->complex_rate = 24000;
    s->rate_in = 24000;
    s->rate_out = 24000;
    FmSource slow = {24000, 0.0, 0};
    run_block(s, slow);
    if (std::fabs(dsd_demod_deemph_compensation(s) - monitor_compensation(24000, 75)) > 1e-12) {
        fail("a squelched block at a new rate publishes the old rate's lane gain");
    }
    dsd_neo_aligned_free(s);
}

void
test_coefficient_follows_rate(void) {
    demod_state* s = new_fsk_demod(48000, 1, 75);
    if (!s) {
        fail("allocation");
        return;
    }
    FmSource src = {48000, 0.0, 0};
    run_block(s, src);
    const float at48 = s->voice_lane_deemph_a;
    s->complex_rate = 24000;
    s->rate_in = 24000;
    s->rate_out = 24000;
    FmSource slow = {24000, 0.0, 0};
    run_block(s, slow);
    if (std::fabs(at48 - dsd_demod_deemph_alpha(48000, 75)) > 1e-9f
        || std::fabs(s->voice_lane_deemph_a - dsd_demod_deemph_alpha(24000, 75)) > 1e-9f) {
        fail("the lane's coefficient does not follow the rate");
    }
    dsd_neo_aligned_free(s);
}

/* The voice lane through the output resampler beside the main lane, fed the same samples in blocks of @p sizes: the
   same counts and the same bits, block for block. */
int
resample_in_step(demod_state* s, const int* sizes, int nsizes, int blocks, uint32_t* seed) {
    static float in[4096];
    for (int b = 0; b < blocks; b++) {
        const int n = sizes[b % nsizes];
        for (int i = 0; i < n; i++) {
            *seed = (*seed * 1664525U) + 1013904223U;
            in[i] = (float)((int)(*seed >> 16) - 32768) / 32768.0f;
        }
        const int phase = s->resamp_phase;
        const int main_n = resamp_process_block(s, in, n, s->resamp_outbuf);
        const int voice_n = resamp_process_voice_lane(s, phase, in, n, s->voice_resamp_outbuf);
        if (main_n != voice_n || !same_bits(s->resamp_outbuf, s->voice_resamp_outbuf, main_n)) {
            return 0;
        }
    }
    return 1;
}

void
test_resampler_keeps_the_lane_in_step(void) {
    demod_state* s = new_fsk_demod(60000, 1, 75);
    if (!s) {
        fail("allocation");
        return;
    }
    s->resamp_enabled = 1;
    if (!resamp_design(s, 4, 5)) {
        fail("resampler design");
        dsd_neo_aligned_free(s);
        return;
    }
    static const int kSizes[] = {37, 960, 1, 513, 4096, 2};
    uint32_t seed = 0x518B10U;
    if (!resample_in_step(s, kSizes, 6, 40, &seed)) {
        fail("the resampled lanes differ in count or samples");
    }
    /* A reset of the main lane's history (a retune) starts the lane's over too, and they stay in step. */
    s->resamp_phase = 0;
    s->resamp_hist_head = 0;
    DSD_MEMSET(s->resamp_hist, 0, (size_t)s->resamp_taps_per_phase * 2U * sizeof(float));
    resamp_clear_voice_lane(s);
    if (!resample_in_step(s, kSizes, 6, 40, &seed)) {
        fail("after a reset the resampled lanes differ");
    }
    /* Another ratio (another taps per phase) lays the lane's history out again. */
    if (!resamp_design(s, 3, 2) || !resample_in_step(s, kSizes, 6, 40, &seed)) {
        fail("after a redesign the resampled lanes differ");
    }
    if (s->resamp_taps) {
        dsd_neo_aligned_free(s->resamp_taps);
    }
    if (s->resamp_hist) {
        dsd_neo_aligned_free(s->resamp_hist);
    }
    dsd_neo_aligned_free(s);
}

/* The mean of @p n samples. */
double
mean_of(const float* x, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += (double)x[i];
    }
    return n > 0 ? sum / (double)n : 0.0;
}

/* A carrier 1.5 kHz off centre, with no de-emphasis: the lane is the modem's centred output, so the offset is centred
   out of it (an uncentred lane would sit 0.0625 off zero), and it is so from the first block of a lane switched on
   mid-stream, sample for sample the lane of a demodulator that ran it all along: no restart of the centring, no step.
   That holds on across a squelched block and a reconfiguration at a new rate, both of which start the modem over. */
void
test_lane_follows_the_modem_centring(void) {
    demod_state* all_along = new_fsk_demod(48000, 1, 0);
    demod_state* switched = new_fsk_demod(48000, 0, 0);
    if (!all_along || !switched) {
        fail("allocation");
        dsd_neo_aligned_free(all_along);
        dsd_neo_aligned_free(switched);
        return;
    }
    FmSource a = {48000, 0.0, 0, 1500.0};
    FmSource b = {48000, 0.0, 0, 1500.0};
    for (int blk = 0; blk < 40; blk++) { /* 0.4 s: the centring (time constant 4000 samples) has settled */
        run_block(all_along, a);
        run_block(switched, b);
    }
    switched->voice_lane_enabled = 1;
    int same = 1;
    double worst_mean = 0.0;
    for (int blk = 0; blk < 30; blk++) {
        if (blk == 10) {
            all_along->channel_squelch_level.store(1.0f);
            switched->channel_squelch_level.store(1.0f);
        } else if (blk == 11) {
            all_along->channel_squelch_level.store(0.0f);
            switched->channel_squelch_level.store(0.0f);
        } else if (blk == 20) {
            /* A new rate: the modem is configured for it, which starts it over. */
            dsd_fsk_modem_config cfg = {};
            cfg.sample_rate_hz = 24000;
            cfg.symbol_rate_hz = 9600;
            cfg.levels = 2;
            for (demod_state* s : {all_along, switched}) {
                s->complex_rate = 24000;
                s->rate_in = 24000;
                s->rate_out = 24000;
                dsd_fsk_modem_configure(&s->fsk_modem_state, &cfg);
            }
            a = FmSource{24000, 0.0, 0, 1500.0};
            b = FmSource{24000, 0.0, 0, 1500.0};
        }
        run_block(all_along, a);
        run_block(switched, b);
        same &= switched->voice_lane_active && all_along->result_len == switched->result_len
                && same_bits(all_along->voice_result, switched->voice_result, switched->result_len);
        if (blk < 10) {
            const double m = std::fabs(mean_of(switched->voice_result, switched->result_len));
            worst_mean = m > worst_mean ? m : worst_mean;
        }
    }
    if (!same) {
        fail("a lane switched on mid-stream is not the lane that ran all along (centring restarted or stepped)");
    }
    if (worst_mean > 0.002) {
        DSD_FPRINTF(stderr, "  carrier 1.5 kHz off: the lane's mean is %.5f\n", worst_mean);
        fail("the lane does not centre a carrier offset out");
    }
    dsd_neo_aligned_free(all_along);
    dsd_neo_aligned_free(switched);
}

} // namespace

int
main(void) {
    test_level_at_rates_and_deemphasis();
    test_fsk_lane_unchanged();
    test_squelched_block_is_silence();
    test_squelched_block_publishes_the_lane_gain();
    test_coefficient_follows_rate();
    test_lane_follows_the_modem_centring();
    test_resampler_keeps_the_lane_in_step();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DSP_FSK_VOICE_LANE: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DSP_FSK_VOICE_LANE: OK\n");
    return 0;
}
