// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The auto squelch inside full_demod() (issue #518 follow-up): the tracker runs on the channel-filtered samples with
 * the plan of the channel filter in force, flags each sample instead of zeroing anything, and the flags follow the
 * samples through the AM detector, the post-decimator (both paths) and the resampler. The same input cut into other
 * blocks gives the same flags. It also runs on the 9600 bit/s FSK path EDACS analog voice reads and on the M17
 * encoder's monitor (issue #625), and closes within DSD_SQUELCH_CLOSE_DELAY_MS of a carrier dropping. Without a step
 * its plan models the whole half-band cascade, so white noise through 4 to 8 passes stays shut from cold start, and a
 * pass-count or width change moves the floor on the cascade's gain (issue #899).
 */

#include <cmath>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_step_model.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "demod_pipeline_test_support.h"
#include "dsd-neo/core/safe_api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

struct Rng {
    uint64_t s;

    double
    uniform() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (double)((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
    }

    double
    gauss() {
        double u1 = uniform();
        if (u1 < 1e-300) {
            u1 = 1e-300;
        }
        return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * uniform());
    }
};

/* Noise throughout, and a carrier (FM with a 1 kHz tone at 2.5 kHz deviation, or AM at 50 %) over the spans that
   @p spans lists as {start s, end s}. */
std::vector<float>
make_spans(int rate, double total_s, const double (*spans)[2], int span_count, int am, uint64_t seed) {
    Rng rng{seed};
    const double sd = sqrt(1e-3 / 2.0);
    const double amp = sqrt(1e-3 * 100.0);
    const int n = (int)(total_s * rate);
    std::vector<float> iq((size_t)n * 2U);
    double mod = 0.0;
    for (int k = 0; k < n; k++) {
        double re = sd * rng.gauss();
        double im = sd * rng.gauss();
        int on = 0;
        for (int i = 0; i < span_count; i++) {
            on |= k >= (int)(spans[i][0] * rate) && k < (int)(spans[i][1] * rate);
        }
        if (on) {
            mod += 2.0 * M_PI * 1000.0 / (double)rate;
            if (am) {
                re += amp * (1.0 + 0.5 * cos(mod));
            } else {
                const double ph = 2.5 * sin(mod);
                re += amp * cos(ph);
                im += amp * sin(ph);
            }
        }
        iq[(size_t)k * 2U] = (float)re;
        iq[((size_t)k * 2U) + 1U] = (float)im;
    }
    return iq;
}

/* Noise, then a carrier, then noise. */
std::vector<float>
make_signal(int rate, double noise_s, double carrier_s, double tail_s, int am, uint64_t seed) {
    const double span[1][2] = {{noise_s, noise_s + carrier_s}};
    return make_spans(rate, noise_s + carrier_s + tail_s, span, carrier_s > 0.0 ? 1 : 0, am, seed);
}

demod_state*
new_monitor(int am, int rate_out, int post_downsample, int width_hz) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return NULL;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    s->mode_demod = am ? &dsd_am_demod : &dsd_fm_demod;
    s->analog_family = 1;
    s->analog_demod = am ? DSD_ANALOG_DEMOD_AM : DSD_ANALOG_DEMOD_FM;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_enable = 1;
    s->channel_lpf_width_hz = width_hz;
    s->rate_in = rate_out * post_downsample;
    s->rate_out = rate_out;
    s->complex_rate = rate_out * post_downsample;
    s->post_downsample = post_downsample;
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    s->squelch_margin_db = 10;
    s->squelch_context.freq_hz = 162475000;
    s->squelch_context.rate_hz = rate_out * post_downsample;
    return s;
}

/* Frees a demod from new_monitor() with what full_demod() allocated for it (the post-decimator's taps and history) and
   a live rate step a test installed with its squelch model. */
void
free_monitor(demod_state* s) {
    if (!s) {
        return;
    }
    dsd_neo_aligned_free(s->post_polydecim_taps);
    dsd_neo_aligned_free(s->post_polydecim_hist);
    dsd_live_iq_step_disable(&s->live_iq_step);
    dsd_squelch_step_model_release(&s->squelch_step_model);
    dsd_neo_aligned_free(s);
}

struct Block {
    std::vector<float> lowpassed; /* the channel filter's output */
    std::vector<float> result;
    std::vector<uint8_t> flags;
};

/* One block of @p pairs complex samples from @p iq through full_demod(), and what it left. */
Block
run_block(demod_state* s, const float* iq, int pairs) {
    static float buf[(size_t)MAXIMUM_BUF_LENGTH];
    DSD_MEMCPY(buf, iq, (size_t)pairs * 2U * sizeof(float));
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    full_demod(s);
    Block b;
    if (s->lowpassed && s->lp_len > 0 && !s->front_end_empty) {
        b.lowpassed.assign(s->lowpassed, s->lowpassed + s->lp_len);
    }
    if (s->result_len > 0) {
        b.result.assign(s->result, s->result + s->result_len);
        b.flags.assign(s->result_flags, s->result_flags + s->result_len);
    }
    return b;
}

/* The covariance white complex noise of unit variance at the capture rate shows behind the half-band cascade of
   @p passes (hb31, then hb15, each decimating by 2, as full_demod_apply_halfband_decimation() runs them) and the
   channel taps, worked out here on its own (issue #899): the cascade is one filter at the capture rate,
   H31(z) H15(z^2) ... H15(z^(2^(passes - 1))), read every 2^passes inputs, so R_y(m) = sum_i h(i) h(i + 2^passes m),
   then R(m) = sum_d R_c(d) R_y(m - d) with R_c the taps' autocorrelation. Scaled by 2^(passes - 1), the noise_gain
   convention. */
std::vector<double>
cascade_noise_cov(int passes, const float* taps, int len) {
    std::vector<double> h(1, 1.0);
    for (int i = 0; i < passes; i++) {
        const float* g = i == 0 ? hb31_q15_taps : hb_q15_taps;
        const int glen = i == 0 ? 31 : HB_TAPS;
        const size_t up = (size_t)1 << i;
        std::vector<double> next(h.size() + ((size_t)(glen - 1) * up), 0.0);
        for (size_t a = 0; a < h.size(); a++) {
            for (int b = 0; b < glen; b++) {
                next[a + ((size_t)b * up)] += h[a] * (double)g[b];
            }
        }
        h.swap(next);
    }
    const size_t step = (size_t)1 << passes;
    const int hb_lags = (int)(h.size() / step) + 1;
    std::vector<double> ry((size_t)hb_lags, 0.0);
    for (int m = 0; m < hb_lags; m++) {
        for (size_t i = 0; i + (step * (size_t)m) < h.size(); i++) {
            ry[(size_t)m] += h[i] * h[i + (step * (size_t)m)];
        }
        ry[(size_t)m] = ldexp(ry[(size_t)m], passes > 0 ? passes - 1 : 0);
    }
    std::vector<double> cov((size_t)(hb_lags + len - 1), 0.0);
    for (int m = 0; m < (int)cov.size(); m++) {
        for (int d = -(len - 1); d <= len - 1; d++) {
            const int at = m - d < 0 ? d - m : m - d;
            if (at >= hb_lags) {
                continue;
            }
            double rc = 0.0;
            const int ad = d < 0 ? -d : d;
            for (int t = 0; t + ad < len; t++) {
                rc += (double)taps[t] * (double)taps[t + ad];
            }
            cov[(size_t)m] += rc * ry[(size_t)at];
        }
    }
    return cov;
}

/* The floor plan of that covariance (cascade_noise_cov()) for the channel taps (NULL or 0: none), at a rate 25 divides
   (the 40 ms window a whole number of samples). */
dsd_squelch_floor_plan
cascade_plan(int passes, const float* taps, int len, int rate) {
    static const float k_unit[1] = {1.0f};
    const std::vector<double> cov = cascade_noise_cov(passes, len > 0 ? taps : k_unit, len > 0 ? len : 1);
    dsd_squelch_floor_periodic_noise noise;
    noise.period = 1;
    noise.lags = (int)cov.size();
    noise.cov = cov.data();
    dsd_squelch_floor_plan plan;
    (void)dsd_squelch_floor_plan_design_periodic(&plan, len > 0 ? taps : NULL, len, &noise, rate);
    return plan;
}

int
fail(const char* what) {
    DSD_FPRINTF(stderr, "squelch auto demod: %s\n", what);
    return 1;
}

/* The tracker in full_demod() flags exactly as a tracker of its own does on the same channel samples, with the plan of
   the channel filter in force; nothing is zeroed, and the level gate stays out of it. */
int
fm_flags_follow_the_tracker(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 1.0, 0, 0x5EEDULL);
    const int total = (int)(iq.size() / 2U);
    dsd_squelch_floor ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    dsd_squelch_floor_set_margin(&ref, 10);
    int rc = 0;
    int ref_ready = 0;
    long opened = 0;
    long closed = 0;
    long closed_nonzero = 0;
    std::vector<uint8_t> want;
    for (int at = 0; at < total; at += 1000) {
        const int pairs = total - at < 1000 ? total - at : 1000;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        if (s->channel_squelched || !s->squelch_gate_open) {
            rc |= fail("the level gate ran under AUTO");
        }
        if (b.result.empty()) {
            continue;
        }
        if (!s->result_flags_active) {
            rc |= fail("no flags under AUTO on the monitor");
            break;
        }
        if (!ref_ready) {
            dsd_squelch_floor_plan plan;
            if (dsd_squelch_floor_plan_design(&plan, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, NULL, 0,
                                              rate)
                    != 0
                || !dsd_squelch_floor_plan_equal(&plan, &s->squelch_floor.plan)) {
                rc |= fail("the tracker's plan is not the channel filter's");
            }
            dsd_squelch_floor_set_plan(&ref, &plan);
            dsd_squelch_floor_change_context(&ref, NULL, NULL, &s->squelch_context);
            ref_ready = 1;
        }
        want.assign(b.result.size(), 0U);
        dsd_squelch_floor_process(&ref, b.lowpassed.data(), (int)(b.lowpassed.size() / 2U), want.data());
        if (want != b.flags) {
            rc |= fail("flags differ from a tracker of its own");
            break;
        }
        for (size_t k = 0; k < b.flags.size(); k++) {
            if (b.flags[k] & DSD_SQUELCH_FLAG_CLOSED) {
                closed++;
                closed_nonzero += fabsf(b.result[k]) > 0.0f;
            } else {
                opened++;
            }
        }
    }
    dsd_squelch_floor_status st;
    dsd_squelch_floor_get_status(&s->squelch_floor, &st);
    if (st.state != DSD_SQUELCH_FLOOR_KNOWN || opened < rate / 3 || closed < rate) {
        rc |= fail("the carrier did not open the gate, or the noise did not close it");
    }
    if (closed_nonzero < closed / 2) {
        rc |= fail("closed samples were zeroed in the demod");
    }
    /* LEVEL: no flags, the level gate as before. */
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    (void)run_block(s, iq.data(), 1000);
    if (s->result_flags_active || s->squelch_auto_ran) {
        rc |= fail("flags under LEVEL");
    }
    /* AUTO off the analog monitor (a digital session's monitor output, say): off. */
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    s->analog_family = 0;
    (void)run_block(s, iq.data(), 1000);
    if (s->result_flags_active) {
        rc |= fail("flags off the analog monitor");
    }
    free_monitor(s);
    return rc;
}

/* The plan follows the channel: another width keeps the floor, rescaled; one half-band stage, two and no channel filter
   design their own plans. */
int
plan_follows_the_channel(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    const std::vector<float> iq = make_signal(rate, 1.0, 0.0, 0.0, 0, 0x91A4ULL);
    for (int at = 0; at < rate; at += 1000) {
        (void)run_block(s, &iq[(size_t)at * 2U], 1000);
    }
    int rc = 0;
    const double floor_a = s->squelch_floor.floor_power;
    const double gain_a = s->squelch_floor.plan.noise_gain;
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
        rc |= fail("no floor at 12.5 kHz");
    }
    /* Less than a window, so no window moves the floor before the check. */
    s->channel_lpf_width_hz = 6000;
    (void)run_block(s, iq.data(), 500);
    dsd_squelch_floor_plan want;
    (void)dsd_squelch_floor_plan_design(&want, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, NULL, 0, rate);
    if (!dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)
        || s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN
        || fabs(s->squelch_floor.floor_power - floor_a * want.noise_gain / gain_a) > 1e-9 * floor_a) {
        rc |= fail("a width change did not rescale the floor to the new plan");
    }
    s->downsample_passes = 1;
    (void)run_block(s, iq.data(), 1000);
    (void)dsd_squelch_floor_plan_design(&want, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, hb31_q15_taps,
                                        31, rate);
    if (!dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)) {
        rc |= fail("the plan left out the half-band stage");
    }
    /* Two passes: the plan of both stages, hb31 and then hb15 (issue #899). */
    s->downsample_passes = 2;
    (void)run_block(s, iq.data(), 1000);
    want = cascade_plan(2, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, rate);
    if (!want.valid || !dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)) {
        rc |= fail("the plan did not take both half-band stages");
    }
    s->downsample_passes = 0;
    s->channel_lpf_enable = 0;
    (void)run_block(s, iq.data(), 1000);
    (void)dsd_squelch_floor_plan_design(&want, NULL, 0, NULL, 0, rate);
    if (!dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)) {
        rc |= fail("no channel filter, but the plan has taps");
    }
    free_monitor(s);
    return rc;
}

/* A new context (a retune) moves the floor through the cache: back on the first channel, its floor seeds the tracker. */
int
context_moves_the_floor(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    const std::vector<float> iq = make_signal(rate, 1.0, 0.0, 0.0, 0, 0xC0DEULL);
    for (int at = 0; at < rate; at += 1000) {
        (void)run_block(s, &iq[(size_t)at * 2U], 1000);
    }
    int rc = 0;
    const double floor_a = s->squelch_floor.floor_power;
    s->squelch_context.freq_hz = 172475000;
    (void)run_block(s, iq.data(), 1000);
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_LEARNING) {
        rc |= fail("an unknown channel did not start learning");
    }
    s->squelch_context.freq_hz = 162475000;
    (void)run_block(s, iq.data(), 10);
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_SEEDED
        || fabs(s->squelch_floor.floor_power - floor_a) > 1e-9 * floor_a) {
        rc |= fail("the first channel's floor did not come back");
    }
    /* A spell under the level squelch and back at the same context keeps the floor. */
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    (void)run_block(s, iq.data(), 1000);
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    (void)run_block(s, iq.data(), 10);
    if (s->squelch_floor.state == DSD_SQUELCH_FLOOR_LEARNING) {
        rc |= fail("a spell under the level squelch lost the floor");
    }
    /* Its windows start over: the gap is not part of them. */
    if (s->squelch_floor.samples > 10U) {
        rc |= fail("windows ran on across a spell under the level squelch");
    }
    /* A stream reset part-way through a window (a reopen, a retune back to the same channel, a replay's RESET) keeps
       the floor and starts the windows over: no window holds samples from both sides of it. */
    (void)run_block(s, iq.data(), 700);
    dsd_demod_reset_filter_state(s);
    /* Long enough for the restarted channel filter to fill its look-ahead and hand the tracker samples. */
    (void)run_block(s, &iq[1400], 300);
    if (s->squelch_floor.state == DSD_SQUELCH_FLOOR_LEARNING) {
        rc |= fail("a stream reset at the same context lost the floor");
    }
    if (s->squelch_floor.samples == 0U || s->squelch_floor.samples > 300U) {
        rc |= fail("windows ran on across a stream reset");
    }
    free_monitor(s);
    return rc;
}

/* @p iq through full_demod() in blocks of @p block pairs: the samples flagged open, and in @p first the index of the
   first of them among the outputs (-1: none). */
long
count_opens(demod_state* s, const std::vector<float>& iq, int block, long* first) {
    long opens = 0;
    long outputs = 0;
    *first = -1;
    const int total = (int)(iq.size() / 2U);
    for (int at = 0; at < total; at += block) {
        const int pairs = total - at < block ? total - at : block;
        const Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        for (size_t k = 0; k < b.flags.size(); k++, outputs++) {
            if (!(b.flags[k] & DSD_SQUELCH_FLAG_CLOSED)) {
                *first = *first < 0 ? outputs : *first;
                opens++;
            }
        }
    }
    return opens;
}

/* @p iq with its power scaled by @p db. */
std::vector<float>
scaled(std::vector<float> iq, double db) {
    const float k = (float)pow(10.0, db / 20.0);
    for (float& v : iq) {
        v *= k;
    }
    return iq;
}

/* A stream restart at the same context (issue #899 B9: Stop during a carrier and Start on an idle channel, a
   same-frequency restart, an rtl_tcp reconnect) goes through dsd_demod_reset_filter_state(): the new stream's first
   outputs are not flagged open from the gate the old one left, and idle noise louder than the kept floor opens
   nothing. */
int
stream_restart_closes_the_gate(void) {
    const int rate = 24000;
    int rc = 0;
    long first = -1;
    const std::vector<float> idle = make_signal(rate, 0.2, 0.0, 0.0, 0, 0x899B01ULL);
    for (int scenario = 0; scenario < 2; scenario++) {
        /* A known floor; then a carrier (the gate open at Stop) or not (closed); the restart into idle noise as loud
           as before, or 13 dB louder. */
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        const std::vector<float> before = make_signal(rate, 1.0, scenario == 0 ? 0.5 : 0.0, 0.0, 0, 0x899B02ULL);
        (void)count_opens(s, before, 1000, &first);
        if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN || s->squelch_floor.gate_open != (scenario == 0)) {
            rc |= fail("no floor, or the gate not as the stream left it, before the restart");
        }
        dsd_demod_reset_filter_state(s);
        const long opens = count_opens(s, scenario == 0 ? idle : scaled(idle, 13.0), 1000, &first);
        if (opens != 0) {
            DSD_FPRINTF(stderr, "squelch auto demod: %ld samples open on %s idle noise after a stream restart\n", opens,
                        scenario == 0 ? "the same" : "louder");
            rc |= 1;
        }
        if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
            rc |= fail("a stream restart lost the floor");
        }
        free_monitor(s);
    }
    {
        /* LEARNING, open on a carrier window, then the restart into idle noise. */
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        const std::vector<float> carrier = make_signal(rate, 0.0, 0.3, 0.0, 0, 0x899B03ULL);
        (void)count_opens(s, carrier, 1000, &first);
        if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_LEARNING || !s->squelch_floor.gate_open) {
            rc |= fail("not learning and open on the carrier before the restart");
        }
        dsd_demod_reset_filter_state(s);
        const long opens = count_opens(s, idle, 1000, &first);
        if (opens != 0) {
            DSD_FPRINTF(stderr, "squelch auto demod: %ld samples open on idle noise after a restart while learning\n",
                        opens);
            rc |= 1;
        }
        free_monitor(s);
    }
    return rc;
}

/* After a stream restart through dsd_demod_reset_filter_state() (issue #899 B9), learning windows from before it do
   not make a floor with one after it, and a carrier right after it opens within one window more than mid-stream. */
int
stream_restart_starts_the_evidence_over(void) {
    const int rate = 24000;
    int rc = 0;
    long first = -1;
    {
        /* Four NOISE windows, the restart, one louder NOISE window: no floor. */
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        const std::vector<float> noise = make_signal(rate, 0.5, 0.0, 0.0, 0, 0x899B04ULL);
        const std::vector<float> louder = scaled(make_signal(rate, 0.5, 0.0, 0.0, 0, 0x899B05ULL), 5.5);
        for (int at = 0; s->squelch_floor.windows < 4U && at + 48 <= (int)(noise.size() / 2U); at += 48) {
            (void)run_block(s, &noise[(size_t)at * 2U], 48);
        }
        int noise_windows = 0;
        for (int i = 0; i < s->squelch_floor.learn_count; i++) {
            noise_windows += s->squelch_floor.learn_class[i] == DSD_SQUELCH_WINDOW_NOISE;
        }
        if (s->squelch_floor.windows != 4U || noise_windows != 4) {
            rc |= fail("not four NOISE windows before the restart");
        }
        dsd_demod_reset_filter_state(s);
        for (int at = 0; s->squelch_floor.windows < 5U && at + 48 <= (int)(louder.size() / 2U); at += 48) {
            (void)run_block(s, &louder[(size_t)at * 2U], 48);
        }
        if (s->squelch_floor.windows != 5U || s->squelch_floor.last_class != DSD_SQUELCH_WINDOW_NOISE) {
            rc |= fail("no NOISE window after the restart");
        }
        if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_LEARNING) {
            DSD_FPRINTF(stderr, "squelch auto demod: windows on both sides of a restart made a floor of %.6f\n",
                        s->squelch_floor.floor_power);
            rc |= 1;
        }
        free_monitor(s);
    }
    {
        /* A known floor, the restart, a carrier from its first sample: open within one window more than a stream
           with no restart (20 ms), 60 ms in all, and open from then on. */
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        (void)count_opens(s, make_signal(rate, 1.0, 0.0, 0.0, 0, 0x899B06ULL), 1000, &first);
        dsd_demod_reset_filter_state(s);
        const std::vector<float> carrier = make_signal(rate, 0.0, 0.5, 0.0, 0, 0x899B07ULL);
        const long opens = count_opens(s, carrier, 1000, &first);
        const long outputs = (long)s->squelch_floor.samples;
        if (first < 0 || first > (60L * rate) / 1000L || opens != outputs - first) {
            DSD_FPRINTF(stderr, "squelch auto demod: a carrier after a restart opened at output %ld, %ld open\n", first,
                        opens);
            rc |= 1;
        } else {
            printf("DSP_SQUELCH_AUTO_DEMOD: a carrier right after a stream restart opens after %.1f ms\n",
                   (double)first * 1000.0 / (double)rate);
        }
        free_monitor(s);
    }
    return rc;
}

/* The restart hold and the floor cache (issue #899): a new stream onto a channel whose floor comes from the cache
   (learn A, visit B, Stop, Start on A: the IO layer marks the open, squelch_new_stream, and resets the filters) holds
   like one onto a floor it kept, so idle noise louder than A's floor opens nothing; a restart on A, then a retune to B
   before a window classified, leaves A's hold behind, and B's cached floor opens on a carrier at its first sub-window;
   and a move to a cached floor with no gap holds nothing. The open's mark waits for the tracker: a level block on the
   reopened channel before Auto comes in leaves it for Auto's first context, and a retune to another channel before
   that (dsd_demod_squelch_note_retune(), as the IO layer calls it) drops it, so B's cached floor gates at once. */
int
stream_restart_hold_follows_the_context(void) {
    const int rate = 24000;
    const int64_t freq_a = 162475000;
    const int64_t freq_b = 172475000;
    int rc = 0;
    long first = -1;
    const std::vector<float> noise = make_signal(rate, 1.0, 0.0, 0.0, 0, 0x899B10ULL);
    const std::vector<float> idle = make_signal(rate, 0.2, 0.0, 0.0, 0, 0x899B11ULL);
    {
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        (void)count_opens(s, noise, 1000, &first);
        s->squelch_context.freq_hz = freq_b;
        (void)count_opens(s, noise, 1000, &first);
        if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
            rc |= fail("no floor on the second channel");
        }
        s->squelch_context.freq_hz = freq_a;
        s->squelch_new_stream = 1;
        dsd_demod_reset_filter_state(s);
        const long opens = count_opens(s, scaled(idle, 13.0), 1000, &first);
        if (opens != 0) {
            DSD_FPRINTF(stderr,
                        "squelch auto demod: %ld samples open on louder noise after a new stream on a cached floor\n",
                        opens);
            rc |= 1;
        }
        free_monitor(s);
    }
    for (int retune = 0; retune < 2; retune++) {
        /* Learn A and B, Stop on B, reopen (on A, or with a retune to come, on B) under the level squelch (one block),
           then Auto: with no retune between, A's cached floor holds; reopened on B and retuned to A before Auto, A's
           cached floor gates a carrier at once. */
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        s->squelch_context.freq_hz = freq_a;
        (void)count_opens(s, noise, 1000, &first);
        s->squelch_context.freq_hz = freq_b;
        (void)count_opens(s, noise, 1000, &first);
        s->squelch_context.freq_hz = retune ? freq_b : freq_a;
        s->squelch_new_stream = 1;
        dsd_demod_reset_filter_state(s);
        s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
        (void)run_block(s, idle.data(), 1000);
        if (!s->squelch_new_stream) {
            rc |= fail("a level block before the tracker took the open's mark");
        }
        s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
        if (retune) {
            dsd_demod_squelch_note_retune(s);
            s->squelch_context.freq_hz = freq_a;
            dsd_demod_reset_filter_state(s);
            const std::vector<float> carrier = make_signal(rate, 0.0, 0.3, 0.0, 0, 0x899B13ULL);
            (void)count_opens(s, carrier, 1000, &first);
            if (first < 0 || first > (30L * rate) / 1000L) {
                DSD_FPRINTF(stderr,
                            "squelch auto demod: a carrier on a cached floor after a reopen and a retune opened at "
                            "%ld\n",
                            first);
                rc |= 1;
            }
        } else {
            const long opens = count_opens(s, scaled(idle, 13.0), 1000, &first);
            if (opens != 0) {
                DSD_FPRINTF(stderr,
                            "squelch auto demod: %ld samples open on louder noise after a reopen, a level block and "
                            "Auto on a cached floor\n",
                            opens);
                rc |= 1;
            }
        }
        if (s->squelch_new_stream) {
            rc |= fail("the tracker's first context left the open's mark");
        }
        free_monitor(s);
    }
    {
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        s->squelch_context.freq_hz = freq_b;
        (void)count_opens(s, noise, 1000, &first);
        s->squelch_context.freq_hz = freq_a;
        (void)count_opens(s, noise, 1000, &first);
        if (s->squelch_floor.restart_hold || s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
            rc |= fail("a move to a cached floor with no gap held the gate, or found no floor");
        }
        dsd_demod_reset_filter_state(s);
        (void)run_block(s, noise.data(), 300);
        if (!s->squelch_floor.restart_hold) {
            rc |= fail("no hold after the restart");
        }
        s->squelch_context.freq_hz = freq_b;
        dsd_demod_reset_filter_state(s);
        const std::vector<float> carrier = make_signal(rate, 0.0, 0.3, 0.0, 0, 0x899B12ULL);
        (void)count_opens(s, carrier, 1000, &first);
        if (first < 0 || first > (30L * rate) / 1000L) {
            DSD_FPRINTF(stderr,
                        "squelch auto demod: a carrier on a cached floor after a restart and a retune opened at %ld\n",
                        first);
            rc |= 1;
        }
        free_monitor(s);
    }
    return rc;
}

/* The per-channel floors age with the stream in every mode: under the level squelch and off the analog monitor (a
   digital channel) the cache's clock runs on, so a floor cached before is stale when its channel comes back. */
int
cache_ages_in_every_mode(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    const std::vector<float> iq = make_signal(rate, 1.0, 0.0, 0.0, 0, 0xA6EULL);
    int rc = 0;
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    for (int at = 0; at < rate / 2; at += 1000) {
        (void)run_block(s, &iq[(size_t)at * 2U], 1000);
    }
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    s->analog_family = 0;
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    for (int at = rate / 2; at < rate; at += 1000) {
        (void)run_block(s, &iq[(size_t)at * 2U], 1000);
    }
    /* The clock counts the channel filter's output, which starts its look-ahead behind the input. */
    if (s->squelch_auto_ran || s->squelch_cache.clock_s > 1.0 + 1e-9 || s->squelch_cache.clock_s < 0.99) {
        DSD_FPRINTF(stderr, "squelch auto demod: the cache clock read %.6f s after 1 s of level and digital blocks\n",
                    s->squelch_cache.clock_s);
        rc |= 1;
    }
    free_monitor(s);
    return rc;
}

/* The floor a tracker kept through a spell without running ages like a cached one: back on its channel after more than
   the staleness under the level squelch, it is learned again; back on another channel, it is not stored for its own
   with a fresh stamp. A short spell keeps it. */
int
idle_floor_goes_stale(void) {
    const int rate = 24000;
    const std::vector<float> iq = make_signal(rate, 1.0, 0.0, 0.0, 0, 0x57A1EULL);
    int rc = 0;
    for (int other_channel = 0; other_channel < 2; other_channel++) {
        for (int long_spell = 0; long_spell < 2; long_spell++) {
            demod_state* s = new_monitor(0, rate, 1, 12500);
            if (!s) {
                return fail("alloc");
            }
            for (int at = 0; at < rate; at += 1000) {
                (void)run_block(s, &iq[(size_t)at * 2U], 1000);
            }
            if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
                rc |= fail("no floor learned before the spell");
            }
            /* The spell: level-squelch blocks, then the time it lasted (as that many blocks would count it). */
            s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
            (void)run_block(s, iq.data(), 1000);
            dsd_squelch_floor_cache_advance(&s->squelch_cache,
                                            long_spell ? (double)DSD_SQUELCH_FLOOR_STALE_S + 60.0 : 600.0);
            s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
            if (other_channel) {
                s->squelch_context.freq_hz = 172475000;
                (void)run_block(s, iq.data(), 1000);
                s->squelch_context.freq_hz = 162475000;
            }
            (void)run_block(s, iq.data(), 10);
            const int kept = s->squelch_floor.state != DSD_SQUELCH_FLOOR_LEARNING;
            if (kept == long_spell) {
                DSD_FPRINTF(stderr, "squelch auto demod: after a %s spell%s the floor was %s\n",
                            long_spell ? "long" : "short", other_channel ? " via another channel" : "",
                            kept ? "kept" : "lost");
                rc |= 1;
            }
            free_monitor(s);
        }
    }
    return rc;
}

/* The AM detector gates per sample: a closed sample is silence, and the first open sample after a long closed run
   starts the carrier estimate from its own open run, not from the noise. */
int
am_detector_follows_the_flags(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(1, rate, 1, 6000);
    if (!s) {
        return fail("alloc");
    }
    /* A transmission after a second of noise, then a 60 ms break (shorter than the hold), then more of it. */
    const double spans[2][2] = {{1.0, 1.5}, {1.56, 1.9}};
    const std::vector<float> iq = make_spans(rate, 2.1, spans, 2, 1, 0xA11ULL);
    const int total = (int)(iq.size() / 2U);
    int rc = 0;
    int prev_closed = 0;
    int onsets = 0;
    int open_after_closed = 0;
    float worst_onset = 0.0f;
    for (int at = 0; at < total; at += 1000) {
        const int pairs = total - at < 1000 ? total - at : 1000;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        for (size_t k = 0; k < b.flags.size(); k++) {
            if (b.flags[k] & DSD_SQUELCH_FLAG_CLOSED) {
                prev_closed = 1;
                if (fabsf(b.result[k]) > 0.0f) {
                    rc |= fail("a closed AM sample is not silence");
                    break;
                }
                continue;
            }
            if (prev_closed) {
                onsets++;
                open_after_closed = 0;
                prev_closed = 0;
            }
            if (onsets > 0 && open_after_closed < 240) {
                open_after_closed++;
                worst_onset = fmaxf(worst_onset, fabsf(b.result[k]));
            }
        }
    }
    /* 50 % at the 0.25 detector gain: 0.125 peak. An estimate that followed the noise while closed would read the
       carrier as up to +2 (clamped), 0.5 out, at either onset. */
    if (onsets != 2 || worst_onset > 0.2f) {
        DSD_FPRINTF(stderr, "squelch auto demod: AM onsets %d, onset peak %.3f\n", onsets, (double)worst_onset);
        rc |= 1;
    }
    free_monitor(s);
    return rc;
}

/* Through the post-decimator: each output takes the flag of the input K/2 before the latest (polyphase), or of its
   group's middle sample (the fallback); checked against a tracker of its own on the channel samples. */
int
post_decimation_maps_flags(int fallback) {
    /* 24,030 Hz at the channel: the tracker's 20 ms boundaries fall at every phase of the decimator's groups of 3. */
    const int rate_out = 8010;
    const int post = 3;
    dsd_demod_test_fail_post_polydecim_alloc(fallback);
    demod_state* s = new_monitor(0, rate_out, post, 0);
    if (!s) {
        dsd_demod_test_fail_post_polydecim_alloc(0);
        return fail("alloc");
    }
    /* Six bursts at offsets a few ms apart: a dozen transitions, at every phase of the groups, or the paths' rules
       would not tell apart. */
    double spans[6][2];
    for (int i = 0; i < 6; i++) {
        spans[i][0] = 1.0 + (0.45 * i) + (0.007 * i);
        spans[i][1] = spans[i][0] + 0.2 + (0.003 * i);
    }
    const std::vector<float> iq = make_spans(rate_out * post, 4.0, spans, 6, 0, 0xDEC1ULL);
    const int total = (int)(iq.size() / 2U);
    dsd_squelch_floor ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    dsd_squelch_floor_set_margin(&ref, 10);
    int rc = 0;
    int ready = 0;
    std::vector<uint8_t> inputs;
    std::vector<uint8_t> got;
    for (int at = 0; at < total; at += 999) {
        const int pairs = total - at < 999 ? total - at : 999;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        if (b.lowpassed.empty()) {
            continue;
        }
        if (!ready) {
            if (s->squelch_floor.plan.rate_hz != rate_out * post) {
                rc |= fail("the tracker runs at the post-decimated rate");
            }
            dsd_squelch_floor_set_plan(&ref, &s->squelch_floor.plan);
            dsd_squelch_floor_change_context(&ref, NULL, NULL, &s->squelch_context);
            ready = 1;
        }
        std::vector<uint8_t> f(b.lowpassed.size() / 2U);
        dsd_squelch_floor_process(&ref, b.lowpassed.data(), (int)f.size(), f.data());
        inputs.insert(inputs.end(), f.begin(), f.end());
        got.insert(got.end(), b.flags.begin(), b.flags.end());
    }
    /* The expected outputs: one per post inputs, the first after the post-th. */
    std::vector<uint8_t> want;
    const int delay = 8; /* K/2, K = 16 */
    for (size_t n = (size_t)post - 1U; n < inputs.size(); n += (size_t)post) {
        if (fallback) {
            want.push_back(inputs[n - (size_t)(post - 1) + (size_t)((post - 1) / 2)]);
        } else {
            want.push_back(n >= (size_t)delay ? inputs[n - (size_t)delay] : (uint8_t)DSD_SQUELCH_FLAG_CLOSED);
        }
    }
    int transitions = 0;
    for (size_t k = 1; k < got.size(); k++) {
        transitions += got[k] != got[k - 1];
    }
    if (got != want || transitions < 12) {
        DSD_FPRINTF(stderr, "squelch auto demod: %s post-decimation flags: %zu got, %zu want, %d transitions\n",
                    fallback ? "fallback" : "polyphase", got.size(), want.size(), transitions);
        rc |= 1;
    }
    free_monitor(s);
    dsd_demod_test_fail_post_polydecim_alloc(0);
    return rc;
}

/* Through the resampler: each output takes the flag of the input K/2 before the latest, the same however the input is
   cut into blocks; a resampler that is off passes them through. */
int
resampler_maps_flags(void) {
    int rc = 0;
    const int ratios[][2] = {{2, 1}, {3, 2}, {1, 1}, {4, 5}};
    for (size_t r = 0; r < sizeof ratios / sizeof ratios[0]; r++) {
        demod_state* a = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
        demod_state* b = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
        if (!a || !b) {
            dsd_neo_aligned_free(a);
            dsd_neo_aligned_free(b);
            return fail("alloc");
        }
        DSD_MEMSET(a, 0, sizeof(*a));
        DSD_MEMSET(b, 0, sizeof(*b));
        const int L = ratios[r][0];
        const int M = ratios[r][1];
        if (L != M) {
            resamp_design(a, L, M);
            resamp_design(b, L, M);
        }
        const int n = 3000;
        std::vector<float> in((size_t)n, 0.0f);
        std::vector<uint8_t> in_flags((size_t)n);
        for (int k = 0; k < n; k++) {
            in[(size_t)k] = sinf((float)k * 0.01f);
            in_flags[(size_t)k] = ((k / 377) % 2) ? (uint8_t)DSD_SQUELCH_FLAG_CLOSED : (uint8_t)0U;
        }
        /* One call... */
        static float out_a[(size_t)MAXIMUM_BUF_LENGTH];
        static uint8_t flags_a[(size_t)MAXIMUM_BUF_LENGTH];
        const int phase0 = a->resamp_phase;
        const int len_a = resamp_process_block_flags(a, in.data(), in_flags.data(), n, out_a, flags_a);
        /* ...and the same in uneven pieces. */
        static float out_b[(size_t)MAXIMUM_BUF_LENGTH];
        static uint8_t flags_b[(size_t)MAXIMUM_BUF_LENGTH];
        int len_b = 0;
        for (int at = 0, piece = 1; at < n; at += piece, piece = piece * 3 % 701 + 1) {
            const int m = n - at < piece ? n - at : piece;
            len_b += resamp_process_block_flags(b, &in[(size_t)at], &in_flags[(size_t)at], m, out_b + len_b,
                                                flags_b + len_b);
        }
        if (len_a != len_b || memcmp(flags_a, flags_b, (size_t)len_a) != 0) {
            rc |= fail("resampler flags depend on the block cuts");
        }
        /* ...and one sample a call: when decimating, many calls make no output, and their samples' flags must still
           enter the history. */
        demod_state* c = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
        if (!c) {
            dsd_neo_aligned_free(a);
            dsd_neo_aligned_free(b);
            return fail("alloc");
        }
        DSD_MEMSET(c, 0, sizeof(*c));
        if (L != M) {
            resamp_design(c, L, M);
        }
        static float out_c[(size_t)MAXIMUM_BUF_LENGTH];
        static uint8_t flags_c[(size_t)MAXIMUM_BUF_LENGTH];
        int len_c = 0;
        for (int at = 0; at < n; at++) {
            len_c += resamp_process_block_flags(c, &in[(size_t)at], &in_flags[(size_t)at], 1, out_c + len_c,
                                                flags_c + len_c);
        }
        if (len_a != len_c || memcmp(flags_a, flags_c, (size_t)len_a) != 0) {
            DSD_FPRINTF(stderr, "squelch auto demod: resampler %d/%d flags differ one sample a call\n", L, M);
            rc |= 1;
        }
        dsd_neo_aligned_free(c->resamp_taps);
        dsd_neo_aligned_free(c->resamp_hist);
        dsd_neo_aligned_free(c);
        /* The rule: the same recurrence, the flag K/2 back (closed before the first input). */
        std::vector<uint8_t> want;
        if (L == M) {
            want = in_flags;
        } else {
            const int delay = a->resamp_taps_per_phase / 2;
            int phase = phase0;
            for (int k = 0; k < n; k++) {
                const uint8_t f = k >= delay ? in_flags[(size_t)(k - delay)] : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
                while (phase < L) {
                    want.push_back(f);
                    phase += M;
                }
                phase -= L;
            }
        }
        if ((int)want.size() != len_a || memcmp(want.data(), flags_a, want.size()) != 0) {
            DSD_FPRINTF(stderr, "squelch auto demod: resampler %d/%d flags off the rule (%d out, %zu want)\n", L, M,
                        len_a, want.size());
            rc |= 1;
        }
        dsd_neo_aligned_free(a->resamp_taps);
        dsd_neo_aligned_free(a->resamp_hist);
        dsd_neo_aligned_free(b->resamp_taps);
        dsd_neo_aligned_free(b->resamp_hist);
        dsd_neo_aligned_free(a);
        dsd_neo_aligned_free(b);
    }
    return rc;
}

/* Through low_pass_real() (rate_out2): each output takes its last input's flag. */
int
low_pass_real_maps_flags(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    /* 24 kHz to 9 kHz: groups of 2 and 3 samples. */
    s->rate_out2 = 9000;
    const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 0.5, 0, 0x10E2ULL);
    const int total = (int)(iq.size() / 2U);
    dsd_squelch_floor ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    dsd_squelch_floor_set_margin(&ref, 10);
    int ready = 0;
    std::vector<uint8_t> inputs;
    std::vector<uint8_t> got;
    for (int at = 0; at < total; at += 1000) {
        const int pairs = total - at < 1000 ? total - at : 1000;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        if (b.lowpassed.empty()) {
            continue;
        }
        if (!ready) {
            dsd_squelch_floor_set_plan(&ref, &s->squelch_floor.plan);
            dsd_squelch_floor_change_context(&ref, NULL, NULL, &s->squelch_context);
            ready = 1;
        }
        std::vector<uint8_t> f(b.lowpassed.size() / 2U);
        dsd_squelch_floor_process(&ref, b.lowpassed.data(), (int)f.size(), f.data());
        inputs.insert(inputs.end(), f.begin(), f.end());
        got.insert(got.end(), b.flags.begin(), b.flags.end());
    }
    std::vector<uint8_t> want;
    int acc = 0;
    for (size_t n = 0U; n < inputs.size(); n++) {
        acc += s->rate_out2;
        if (acc < rate) {
            continue;
        }
        acc -= rate;
        want.push_back(inputs[n]);
    }
    free_monitor(s);
    return got == want ? 0 : fail("low_pass_real() flags");
}

/* The same input cut into other blocks: the same flags, sample for sample. */
int
flags_do_not_depend_on_block_cuts(void) {
    const int rate = 24000;
    const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 1.0, 0, 0xB10CULL);
    const int total = (int)(iq.size() / 2U);
    std::vector<uint8_t> runs[2];
    const int sizes[2] = {1000, 613};
    for (int v = 0; v < 2; v++) {
        demod_state* s = new_monitor(0, rate, 1, 12500);
        if (!s) {
            return fail("alloc");
        }
        for (int at = 0; at < total; at += sizes[v]) {
            const int pairs = total - at < sizes[v] ? total - at : sizes[v];
            Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
            runs[v].insert(runs[v].end(), b.flags.begin(), b.flags.end());
        }
        free_monitor(s);
    }
    int transitions = 0;
    for (size_t k = 1; k < runs[0].size(); k++) {
        transitions += runs[0][k] != runs[0][k - 1];
    }
    if (runs[0] != runs[1] || transitions < 2) {
        return fail("flags depend on the block cuts");
    }
    return 0;
}

/* A demod on the 9600 bit/s, two-level FSK path EDACS analog voice reads (issue #625): off the analog family, the FSK
   discriminator's output, the PROVOICE channel filter, or none with the channel LPF off (its profile label then stays
   WIDE). */
demod_state*
new_fsk9600(int rate_out, int lpf, int mode) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return NULL;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    s->symbol_rate_hz = 9600;
    s->symbol_levels = 2;
    s->channel_lpf_profile = lpf ? DSD_CH_LPF_PROFILE_PROVOICE : DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_enable = lpf;
    s->rate_in = rate_out;
    s->rate_out = rate_out;
    s->complex_rate = rate_out;
    s->post_downsample = 1;
    s->squelch_mode = mode;
    s->squelch_margin_db = 10;
    s->squelch_context.freq_hz = 851012500;
    s->squelch_context.rate_hz = rate_out;
    return s;
}

/* The FSK path runs the tracker under AUTO, and under NOISE as AUTO: the flags are a tracker's own on the channel
   samples, one per discriminator output, open over the carrier and closed on the noise around it, and the
   discriminator's output is the same sample for sample as with the squelch off. Every other FSK profile runs none. */
int
fsk9600_path_runs_the_tracker(void) {
    const int cases[4][2] = {{24000, 1}, {48000, 1}, {16000, 0}, {12000, 0}};
    int rc = 0;
    for (int c = 0; c < 4; c++) {
        const int rate = cases[c][0];
        const int lpf = cases[c][1];
        demod_state* s = new_fsk9600(rate, lpf, DSD_SQUELCH_MODE_AUTO);
        demod_state* noise = new_fsk9600(rate, lpf, DSD_SQUELCH_MODE_NOISE);
        demod_state* off = new_fsk9600(rate, lpf, DSD_SQUELCH_MODE_LEVEL);
        if (!s || !noise || !off) {
            free_monitor(s);
            free_monitor(noise);
            free_monitor(off);
            return fail("alloc");
        }
        const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 1.0, 0, 0xED4C5ULL + (uint64_t)c);
        const int total = (int)(iq.size() / 2U);
        dsd_squelch_floor ref;
        DSD_MEMSET(&ref, 0, sizeof ref);
        dsd_squelch_floor_set_margin(&ref, 10);
        int ref_ready = 0;
        long opened = 0;
        long closed = 0;
        int case_rc = 0;
        std::vector<uint8_t> want;
        for (int at = 0; at < total && case_rc == 0; at += 960) {
            const int pairs = total - at < 960 ? total - at : 960;
            Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
            Block bn = run_block(noise, &iq[(size_t)at * 2U], pairs);
            Block bo = run_block(off, &iq[(size_t)at * 2U], pairs);
            if (b.result.empty()) {
                continue;
            }
            if (!s->result_flags_active || !s->squelch_auto_ran) {
                case_rc |= fail("no tracker on the 9600 bit/s FSK path under AUTO");
                break;
            }
            if (b.result.size() * 2U != b.lowpassed.size()) {
                case_rc |= fail("the FSK path made other than one output per channel sample");
                break;
            }
            if (!ref_ready) {
                dsd_squelch_floor_plan plan;
                if (dsd_squelch_floor_plan_design(&plan, lpf ? s->channel_lpf_plan_taps : NULL,
                                                  lpf ? s->channel_lpf_plan_taps_len : 0, NULL, 0, rate)
                        != 0
                    || !dsd_squelch_floor_plan_equal(&plan, &s->squelch_floor.plan)) {
                    case_rc |= fail("the FSK path's tracker plan is not its channel's");
                }
                dsd_squelch_floor_set_plan(&ref, &plan);
                dsd_squelch_floor_change_context(&ref, NULL, NULL, &s->squelch_context);
                ref_ready = 1;
            }
            want.assign(b.result.size(), 0U);
            dsd_squelch_floor_process(&ref, b.lowpassed.data(), (int)(b.lowpassed.size() / 2U), want.data());
            if (want != b.flags) {
                case_rc |= fail("the FSK path's flags differ from a tracker of its own");
            }
            if (!noise->result_flags_active || noise->squelch_noise_ran || bn.flags != b.flags) {
                case_rc |= fail("NOISE does not run as AUTO on the FSK path");
            }
            if (off->result_flags_active || bo.result != b.result) {
                case_rc |= fail("the squelch changed the FSK path's output");
            }
            for (size_t k = 0; k < b.flags.size(); k++) {
                if (b.flags[k] & DSD_SQUELCH_FLAG_CLOSED) {
                    closed++;
                } else {
                    opened++;
                }
            }
        }
        if (case_rc == 0 && (opened < rate / 3 || closed < rate)) {
            case_rc |= fail("the FSK path's carrier did not open the gate, or its noise did not close it");
        }
        if (case_rc) {
            DSD_FPRINTF(stderr, "squelch auto demod: the FSK case at %d Hz, channel LPF %s\n", rate,
                        lpf ? "on" : "off");
        }
        rc |= case_rc;
        free_monitor(s);
        free_monitor(noise);
        free_monitor(off);
    }
    /* Other FSK profiles: none. */
    const int others[4][3] = {{4800, 4, DSD_CH_LPF_PROFILE_12K5},
                              {4800, 2, DSD_CH_LPF_PROFILE_6K25},
                              {2400, 2, DSD_CH_LPF_PROFILE_6K25},
                              {6000, 4, DSD_CH_LPF_PROFILE_12K5}};
    const std::vector<float> iq = make_signal(24000, 0.5, 0.0, 0.0, 0, 0x07E5ULL);
    for (int c = 0; c < 4; c++) {
        demod_state* s = new_fsk9600(24000, 1, DSD_SQUELCH_MODE_AUTO);
        if (!s) {
            return fail("alloc");
        }
        s->symbol_rate_hz = others[c][0];
        s->symbol_levels = others[c][1];
        s->channel_lpf_profile = others[c][2];
        const int total = (int)(iq.size() / 2U);
        for (int at = 0; at < total; at += 960) {
            (void)run_block(s, &iq[(size_t)at * 2U], total - at < 960 ? total - at : 960);
        }
        if (s->result_flags_active || s->squelch_auto_ran) {
            DSD_FPRINTF(stderr, "squelch auto demod: the tracker ran on %d/%d FSK\n", others[c][0], others[c][1]);
            rc |= 1;
        }
        free_monitor(s);
    }
    return rc;
}

/* The M17 encoder's monitor (issue #625): off the analog family, the FM discriminator's audio on its WIDE channel. The
   tracker runs there as on the analog monitor, and nowhere else off the family. */
int
encoder_monitor_runs_the_tracker(void) {
    const int rate = 48000;
    demod_state* s = new_monitor(0, rate, 1, 0);
    if (!s) {
        return fail("alloc");
    }
    s->analog_family = 0;
    s->m17_encoder_monitor = 1;
    const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 1.0, 0, 0x317ULL);
    const int total = (int)(iq.size() / 2U);
    int rc = 0;
    long opened = 0;
    long closed = 0;
    for (int at = 0; at < total; at += 960) {
        const int pairs = total - at < 960 ? total - at : 960;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        if (b.result.empty()) {
            continue;
        }
        if (!s->result_flags_active || !s->squelch_auto_ran) {
            rc |= fail("no tracker on the M17 encoder's monitor under AUTO");
            break;
        }
        for (size_t k = 0; k < b.flags.size(); k++) {
            if (b.flags[k] & DSD_SQUELCH_FLAG_CLOSED) {
                closed++;
            } else {
                opened++;
            }
        }
    }
    if (rc == 0 && (opened < rate / 3 || closed < rate)) {
        rc |= fail("the encoder's carrier did not open the gate, or its noise did not close it");
    }
    if (!dsd_demod_encoder_monitor_active(s) || !dsd_demod_dynamic_squelch_path(s)) {
        rc |= fail("the encoder's monitor is not a dynamic squelch path");
    }
    s->cqpsk_enable = 1;
    if (dsd_demod_encoder_monitor_active(s)) {
        rc |= fail("the encoder's monitor under CQPSK is still one");
    }
    s->cqpsk_enable = 0;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_12K5;
    if (dsd_demod_encoder_monitor_active(s)) {
        rc |= fail("the encoder's monitor off its WIDE channel is still one");
    }
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->m17_encoder_monitor = 0;
    (void)run_block(s, iq.data(), 960);
    if (s->result_flags_active) {
        rc |= fail("flags on a monitor off the family that is not the encoder's");
    }
    free_monitor(s);
    return rc;
}

/* The gate closes within DSD_SQUELCH_CLOSE_DELAY_MS of a carrier dropping, at every phase of the 40 ms windows, while
   the floor is learned (landing mid-carrier) and with a known one, on the EDACS plans at the presets' 24 kHz and at
   48 kHz (issue #625: EDACS budgets this delay in its release hold). */
int
gate_closes_within_the_delay_bound(void) {
    int rc = 0;
    int worst_ms = 0;
    const int rates[2] = {24000, 48000};
    for (int r = 0; r < 2; r++) {
        const int rate = rates[r];
        const int bound = (DSD_SQUELCH_CLOSE_DELAY_MS * rate) / 1000;
        for (int known = 0; known < 2; known++) {
            for (int phase_ms = 0; phase_ms < 40; phase_ms += 2) {
                const double start = known ? 1.0 : 0.0;
                const double drop = start + 1.0 + (double)phase_ms / 1000.0;
                const double span[1][2] = {{start, drop}};
                const std::vector<float> iq =
                    make_spans(rate, drop + 0.3, span, 1, 0, 0xD10ULL + (uint64_t)(phase_ms + (100 * known) + rate));
                demod_state* s = new_fsk9600(rate, 1, DSD_SQUELCH_MODE_AUTO);
                if (!s) {
                    return fail("alloc");
                }
                std::vector<uint8_t> flags;
                const int total = (int)(iq.size() / 2U);
                for (int at = 0; at < total; at += 960) {
                    const int pairs = total - at < 960 ? total - at : 960;
                    Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
                    flags.insert(flags.end(), b.flags.begin(), b.flags.end());
                }
                free_monitor(s);
                const size_t d = (size_t)(drop * rate);
                if (flags.size() <= d + (size_t)bound || (flags[d - 1U] & DSD_SQUELCH_FLAG_CLOSED)) {
                    DSD_FPRINTF(stderr, "squelch auto demod: no open carrier before the drop (%d Hz, %d ms, %s)\n",
                                rate, phase_ms, known ? "known floor" : "learning");
                    rc |= 1;
                    continue;
                }
                size_t k = d;
                while (k < flags.size() && !(flags[k] & DSD_SQUELCH_FLAG_CLOSED)) {
                    k++;
                }
                const int delay = (int)(k - d);
                const int delay_ms = (delay * 1000 + rate - 1) / rate;
                worst_ms = delay_ms > worst_ms ? delay_ms : worst_ms;
                if (delay > bound) {
                    DSD_FPRINTF(stderr, "squelch auto demod: the gate closed %d ms after the drop (%d Hz, %d ms, %s)\n",
                                delay_ms, rate, phase_ms, known ? "known floor" : "learning");
                    rc |= 1;
                }
            }
        }
    }
    if (rc == 0) {
        printf("DSP_SQUELCH_AUTO_DEMOD: worst closing delay %d ms (bound %d ms)\n", worst_ms,
               DSD_SQUELCH_CLOSE_DELAY_MS);
    }
    return rc;
}

/* At an Airspy's rates the live I/Q rate step sits between the cascade and the channel filter (issue #806): the
   tracker, and under NOISE the noise squelch, take their plans from the step's noise model, keep the gate shut on
   noise, open it on a carrier and close it after the carrier drops. 2.5 MS/s (78,125 Hz after five passes, L/M
   384/625) and 3 MS/s (93,750 Hz, 64/125) to 48 kHz, NFM 12.5 kHz. */
int
airspy_rates_run_the_step_model(void) {
    const struct {
        int device;
        int hb_out;
        int L;
        int M;
    } chains[] = {{2500000, 78125, 384, 625}, {3000000, 93750, 64, 125}};

    const int rate = 48000;
    int rc = 0;
    for (const auto& c : chains) {
        for (int mode = DSD_SQUELCH_MODE_AUTO; mode <= DSD_SQUELCH_MODE_NOISE; mode++) {
            demod_state* s = new_monitor(0, rate, 1, 12500);
            if (!s) {
                return fail("alloc");
            }
            s->downsample_passes = 5;
            s->squelch_mode = mode;
            dsd_live_iq_step_plan plan;
            DSD_MEMSET(&plan, 0, sizeof plan);
            plan.enabled = 1;
            plan.passes = 5;
            plan.hb_out_num = c.hb_out;
            plan.L = c.L;
            plan.M = c.M;
            plan.target_hz = rate;
            plan.protect_hz = 21600;
            plan.stop_hz = rate - 21600;
            int step_rc = dsd_demod_live_step_prepare(s, &plan);
            step_rc = step_rc == DSD_LIVE_IQ_STEP_OK ? dsd_live_iq_step_commit(&s->live_iq_step) : step_rc;
            dsd_live_iq_step_finish(&s->live_iq_step);
            if (step_rc != DSD_LIVE_IQ_STEP_OK || dsd_demod_squelch_step_model_sync(s) != 0) {
                free_monitor(s);
                return fail("the step or its squelch model did not install");
            }
            /* Noise at the device rate (variance 2^(passes - 1) x 1e-3: 1e-3 of the model's unit), and an FM carrier
               20 dB over the channel's noise from 1.5 s to 2.5 s. */
            const double var = 16.0e-3;
            Rng rng{0xA125ULL + (uint64_t)c.device + (uint64_t)mode};
            std::vector<uint8_t> flags;
            std::vector<float> block((size_t)MAXIMUM_BUF_LENGTH);
            const int pairs_per_block = 65536;
            const long total = (long)(3.5 * c.device);
            double mod = 0.0;
            double amp = 0.0;
            for (long at = 0; at < total; at += pairs_per_block) {
                const int n = total - at < pairs_per_block ? (int)(total - at) : pairs_per_block;
                for (int k = 0; k < n; k++) {
                    const double t = (double)(at + k) / (double)c.device;
                    double re = sqrt(var / 2.0) * rng.gauss();
                    double im = sqrt(var / 2.0) * rng.gauss();
                    if (t >= 1.5 && t < 2.5) {
                        mod += 2.0 * M_PI * 1000.0 / (double)c.device;
                        re += amp * cos(2.5 * sin(mod));
                        im += amp * sin(2.5 * sin(mod));
                    }
                    block[(size_t)k * 2U] = (float)re;
                    block[(size_t)k * 2U + 1U] = (float)im;
                }
                Block b = run_block(s, block.data(), n);
                flags.insert(flags.end(), b.flags.begin(), b.flags.end());
                if (amp == 0.0 && s->squelch_step_model.ready) {
                    dsd_squelch_floor_plan fp;
                    (void)dsd_squelch_step_model_floor_plan(&s->squelch_step_model, &fp, s->channel_lpf_plan_taps,
                                                            s->channel_lpf_plan_taps_len, rate);
                    amp = sqrt(100.0 * (var / 16.0) * fp.noise_gain);
                }
            }
            if (mode == DSD_SQUELCH_MODE_AUTO) {
                dsd_squelch_floor_plan want;
                (void)dsd_squelch_step_model_floor_plan(&s->squelch_step_model, &want, s->channel_lpf_plan_taps,
                                                        s->channel_lpf_plan_taps_len, rate);
                if (!want.valid || !dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)) {
                    rc |= fail("the tracker's plan behind the step is not the step model's");
                }
            } else {
                dsd_noise_squelch_plan want;
                (void)dsd_squelch_step_model_noise_plan(&s->squelch_step_model, &want, s->channel_lpf_plan_taps,
                                                        s->channel_lpf_plan_taps_len, rate);
                if (!want.valid || !dsd_noise_squelch_plan_equal(&want, &s->noise_squelch.plan)
                    || !s->squelch_noise_ran) {
                    rc |= fail("the noise squelch's plan behind the step is not the step model's");
                }
            }
            /* The chain delays the carrier by a few ms (cascade, step and channel filter): the noise spans are checked
               from 0.5 s (a floor learned) and from the drop plus the close delay and 10 ms. */
            long noise_open = 0;
            long carrier_open = 0;
            const size_t carrier_from = (size_t)(1.6 * rate);
            const size_t carrier_to = (size_t)(2.5 * rate);
            const size_t tail_from = (size_t)(2.5 * rate) + (size_t)((DSD_SQUELCH_CLOSE_DELAY_MS + 10) * rate / 1000);
            for (size_t k = (size_t)(0.5 * rate); k < flags.size(); k++) {
                const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
                if (k < (size_t)(1.5 * rate) || k >= tail_from) {
                    noise_open += open;
                } else if (k >= carrier_from && k < carrier_to) {
                    carrier_open += open;
                }
            }
            printf("DSP_SQUELCH_AUTO_DEMOD: %d Hz stepped to %d Hz, %s: %ld noise samples open, carrier %.1f %% open\n",
                   c.device, rate, mode == DSD_SQUELCH_MODE_AUTO ? "AUTO" : "NOISE", noise_open,
                   100.0 * (double)carrier_open / (double)(carrier_to - carrier_from));
            if (flags.size() < tail_from + (size_t)rate / 4U || noise_open != 0
                || carrier_open < (long)((carrier_to - carrier_from) * 95U / 100U)) {
                rc |= fail("behind the step the gate opened on noise or missed the carrier");
            }
            free_monitor(s);
        }
    }
    return rc;
}

/* ---------------------------------------------------------------------- the cascade from cold start (issue #899) */

/* A chain with no step: the cascade of passes ahead of the channel filter at width (0: none) and the channel rate. */
struct CascadeChain {
    int passes;
    int rate;
    int width;
    int mode;
};

/* A monitor on @p c's chain under its mode, margin 10 dB, its context's chain the cascade's (as the IO layer keys it). */
demod_state*
new_cascade_monitor(const CascadeChain& c) {
    demod_state* s = new_monitor(0, c.rate, 1, c.width);
    if (!s) {
        return NULL;
    }
    s->channel_lpf_enable = c.width > 0 ? 1 : 0;
    s->downsample_passes = c.passes;
    s->squelch_mode = c.mode;
    s->squelch_context.chain = c.passes;
    return s;
}

/* Complex Gaussian noise of variance @p var at the capture rate, and an FM carrier (1 kHz tone, 1.5 kHz deviation) of
   amplitude @p amp from @p on_s to @p off_s seconds: one block of @p pairs from capture sample @p at. */
struct CaptureNoise {
    Rng rng;
    double sd;
    double amp;
    double on_s;
    double off_s;
    double mod;

    void
    fill(float* out, long at, int pairs, int capture) {
        for (int k = 0; k < pairs; k++) {
            const double t = (double)(at + k) / (double)capture;
            double re = sd * rng.gauss();
            double im = sd * rng.gauss();
            if (t >= on_s && t < off_s) {
                mod += 2.0 * M_PI * 1000.0 / (double)capture;
                re += amp * cos(1.5 * sin(mod));
                im += amp * sin(1.5 * sin(mod));
            }
            out[(size_t)k * 2U] = (float)re;
            out[((size_t)k * 2U) + 1U] = (float)im;
        }
    }
};

/* @p seconds of the capture through @p s in 10 ms blocks; each block's flags appended to @p flags (may be NULL). The
   first time the tracker has closed 8 windows, whether its floor is known lands in @p known_by_8 (may be NULL). */
void
run_capture(demod_state* s, CaptureNoise* src, long* at, double seconds, std::vector<uint8_t>* flags, int* known_by_8) {
    const int capture = s->complex_rate << s->downsample_passes;
    const int pairs = (s->complex_rate / 100) << s->downsample_passes;
    std::vector<float> block((size_t)pairs * 2U);
    const long end = *at + (long)(seconds * (double)capture);
    while (*at < end) {
        const int n = end - *at < pairs ? (int)(end - *at) : pairs;
        src->fill(block.data(), *at, n, capture);
        *at += n;
        Block b = run_block(s, block.data(), n);
        if (flags) {
            flags->insert(flags->end(), b.flags.begin(), b.flags.end());
        }
        dsd_squelch_floor_status st;
        dsd_squelch_floor_get_status(&s->squelch_floor, &st);
        if (known_by_8 && *known_by_8 < 0 && st.windows >= 8U) {
            *known_by_8 = st.state == DSD_SQUELCH_FLOOR_KNOWN ? 1 : 0;
        }
    }
}

/* White complex noise through the real cascade with no step, from cold start on a fixed seed (issue #899; RTL-SDR and
   rtl_tcp run 4 to 8 passes): not one sample opens on the idle channel, the floor is known by the 8th window, an FM
   carrier 25 dB over the most noise the channel can pass (the capture's variance over 2^passes) opens the gate inside
   it, the gate closes within the close delay bound (and 10 ms of chain delay) after it drops, and not one sample opens
   on the idle channel after. Unfiltered NFM at 16 kHz (6 passes), 12 and 8 kHz (7), NFM 12.5k at 24 kHz (6) and NFM 25k
   at 48 kHz (5) under AUTO; NOISE at 16 kHz unfiltered, where the noise squelch has no band and the tracker runs it. */
int
cold_start_stays_shut(void) {
    const CascadeChain chains[] = {
        {6, 16000, 0, DSD_SQUELCH_MODE_AUTO},     {7, 12000, 0, DSD_SQUELCH_MODE_AUTO},
        {7, 8000, 0, DSD_SQUELCH_MODE_AUTO},      {6, 24000, 12500, DSD_SQUELCH_MODE_AUTO},
        {5, 48000, 25000, DSD_SQUELCH_MODE_AUTO}, {6, 16000, 0, DSD_SQUELCH_MODE_NOISE},
    };

    int rc = 0;
    for (const CascadeChain& c : chains) {
        demod_state* s = new_cascade_monitor(c);
        if (!s) {
            return fail("alloc");
        }
        const double var = 1e-3;
        CaptureNoise src{Rng{0x899B0000ULL + (uint64_t)c.rate + (uint64_t)c.passes + (uint64_t)c.mode},
                         sqrt(var / 2.0),
                         sqrt(pow(10.0, 2.5) * ldexp(var, -c.passes)),
                         1.0,
                         1.5,
                         0.0};
        long at = 0;
        std::vector<uint8_t> flags;
        int known_by_8 = -1;
        run_capture(s, &src, &at, 3.0, &flags, &known_by_8);
        const size_t burst_from = (size_t)(1.05 * c.rate);
        const size_t burst_to = (size_t)(1.5 * c.rate);
        const size_t tail_from = burst_to + (size_t)((DSD_SQUELCH_CLOSE_DELAY_MS + 10) * c.rate / 1000);
        long idle_open = 0;
        long burst_open = 0;
        long tail_open = 0;
        for (size_t k = 0; k < flags.size(); k++) {
            const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
            if (k < (size_t)c.rate) {
                idle_open += open;
            } else if (k >= burst_from && k < burst_to) {
                burst_open += open;
            } else if (k >= tail_from) {
                tail_open += open;
            }
        }
        const char* mode = c.mode == DSD_SQUELCH_MODE_AUTO ? "AUTO" : "NOISE";
        printf("DSP_SQUELCH_AUTO_DEMOD: %d passes to %d Hz, width %d, %s from cold start: %ld idle samples open, known "
               "by window 8 %d, carrier %.1f %% open, %ld samples open after the drop\n",
               c.passes, c.rate, c.width, mode, idle_open, known_by_8,
               100.0 * (double)burst_open / (double)(burst_to - burst_from), tail_open);
        if (flags.size() < tail_from + (size_t)c.rate || idle_open != 0 || known_by_8 != 1
            || burst_open < (long)((burst_to - burst_from) * 95U / 100U) || tail_open != 0) {
            DSD_FPRINTF(stderr,
                        "squelch auto demod: %d passes to %d Hz, width %d, %s: the idle channel opened, or the "
                        "carrier did not\n",
                        c.passes, c.rate, c.width, mode);
            rc |= 1;
        }
        if (c.mode == DSD_SQUELCH_MODE_NOISE && (s->noise_squelch.plan.valid || !s->squelch_auto_ran)) {
            rc |= fail("NOISE at 16 kHz unfiltered did not run as auto");
        }
        free_monitor(s);
    }
    return rc;
}

/* A pass-count change is a context change (the IO layer's chain key, issue #899): the plan of the new cascade is taken
   first, rescaling the old floor, and the floor stored for the context left is that floor over the new plan's gain, so
   the gain cancels; the new context learns, and back on the first its floor seeds from the stored density times its
   gain, the floor it left. 6 passes to 16 kHz unfiltered (noise_gain 0.23852161 for unit capture variance x 32) and 7. */
int
pass_change_moves_the_floor(void) {
    const CascadeChain a = {6, 16000, 0, DSD_SQUELCH_MODE_AUTO};
    demod_state* s = new_cascade_monitor(a);
    if (!s) {
        return fail("alloc");
    }
    int rc = 0;
    CaptureNoise src{Rng{0x899C0DEULL}, sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0, 0.0};
    long at = 0;
    run_capture(s, &src, &at, 1.0, NULL, NULL);
    const dsd_squelch_floor_key key_a = s->squelch_context;
    const double floor_a = s->squelch_floor.floor_power;
    const double gain_a = s->squelch_floor.plan.noise_gain;
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN || fabs(gain_a / 0.23852161 - 1.0) > 1e-6) {
        rc |= fail("6 passes to 16 kHz: no floor, or the plan's gain is not the cascade's");
    }
    /* Seven passes: 100 channel samples, under a window, so no window moves the floor. */
    s->downsample_passes = 7;
    s->squelch_context.chain = 7;
    run_capture(s, &src, &at, 100.0 / 16000.0, NULL, NULL);
    const dsd_squelch_floor_plan want_b = cascade_plan(7, NULL, 0, 16000);
    double density = 0.0;
    const int stored = dsd_squelch_floor_cache_find(&s->squelch_cache, &key_a, &density);
    if (!want_b.valid || !dsd_squelch_floor_plan_equal(&want_b, &s->squelch_floor.plan)
        || s->squelch_floor.state != DSD_SQUELCH_FLOOR_LEARNING || !stored
        || fabs(density - (floor_a / gain_a)) > 1e-12 * (floor_a / gain_a)) {
        rc |= fail("7 passes: not the 7-pass plan, not learning, or the 6-pass floor stored off its density");
    }
    /* Back to six: seeded from the density times the 6-pass gain. */
    s->downsample_passes = 6;
    s->squelch_context.chain = 6;
    run_capture(s, &src, &at, 100.0 / 16000.0, NULL, NULL);
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_SEEDED
        || fabs(s->squelch_floor.floor_power - floor_a) > 1e-12 * floor_a) {
        rc |= fail("back to 6 passes: the floor did not seed from its density");
    }
    free_monitor(s);
    return rc;
}

/* A width change at the same context keeps a known floor, rescaled by the plans' gains, both the cascade's (6 passes
   to 24 kHz, NFM 12.5k to 20k; issue #899). The plan is designed once for each key: a second of blocks at one key
   designs it once, the width change once more. */
int
width_change_rescales_the_cascade_floor(void) {
    const CascadeChain c = {6, 24000, 12500, DSD_SQUELCH_MODE_AUTO};
    demod_state* s = new_cascade_monitor(c);
    if (!s) {
        return fail("alloc");
    }
    int rc = 0;
    CaptureNoise src{Rng{0x899D0DEULL}, sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0, 0.0};
    long at = 0;
    run_capture(s, &src, &at, 1.0, NULL, NULL);
    const double floor_a = s->squelch_floor.floor_power;
    const double gain_a = s->squelch_floor.plan.noise_gain;
    const dsd_squelch_floor_plan want_a =
        cascade_plan(6, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, 24000);
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN
        || !dsd_squelch_floor_plan_equal(&want_a, &s->squelch_floor.plan) || s->squelch_plan_designs != 1U) {
        rc |=
            fail("NFM 12.5k at 24 kHz: no floor, not the cascade's plan, or a block of the same key designed it again");
    }
    s->channel_lpf_width_hz = 20000;
    run_capture(s, &src, &at, 100.0 / 24000.0, NULL, NULL);
    if (s->squelch_plan_designs != 2U) {
        rc |= fail("a width change did not design the plan once");
    }
    const dsd_squelch_floor_plan want = cascade_plan(6, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, 24000);
    if (!want.valid || !dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)
        || s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN
        || fabs(s->squelch_floor.floor_power - (floor_a * want.noise_gain / gain_a)) > 1e-12 * floor_a) {
        rc |= fail("a width change did not keep the floor rescaled to the cascade's new plan");
    }
    free_monitor(s);
    return rc;
}
} // namespace

int
main(void) {
    int rc = 0;
    rc |= fm_flags_follow_the_tracker();
    rc |= plan_follows_the_channel();
    rc |= context_moves_the_floor();
    rc |= stream_restart_closes_the_gate();
    rc |= stream_restart_starts_the_evidence_over();
    rc |= stream_restart_hold_follows_the_context();
    rc |= cache_ages_in_every_mode();
    rc |= idle_floor_goes_stale();
    rc |= am_detector_follows_the_flags();
    rc |= post_decimation_maps_flags(0);
    rc |= post_decimation_maps_flags(1);
    rc |= resampler_maps_flags();
    rc |= low_pass_real_maps_flags();
    rc |= flags_do_not_depend_on_block_cuts();
    rc |= fsk9600_path_runs_the_tracker();
    rc |= encoder_monitor_runs_the_tracker();
    rc |= gate_closes_within_the_delay_bound();
    rc |= airspy_rates_run_the_step_model();
    rc |= cold_start_stays_shut();
    rc |= pass_change_moves_the_floor();
    rc |= width_change_rescales_the_cascade_floor();
    if (rc == 0) {
        printf("DSP_SQUELCH_AUTO_DEMOD: OK\n");
    }
    return rc;
}
