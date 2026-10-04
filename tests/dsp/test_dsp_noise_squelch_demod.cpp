// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The NFM noise squelch inside full_demod() (issue #518 follow-up): under a NOISE setting on the FM monitor it runs on
 * the discriminator's output with the plan of the channel filter in force and flags each sample instead of zeroing
 * anything; on an AM channel or one with no band above voice the floor tracker runs the setting as AUTO; a new context
 * or a stream reset starts it over without touching the tracker's floors; the flags follow the samples through the
 * post-decimator; and the same input cut into other blocks gives the same flags.
 */

#include <cmath>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
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

/* Noise, then a carrier (FM with a 1 kHz tone at 2.5 kHz deviation, or AM at 50 %, 20 dB over the noise before the
   channel filter), then noise. */
std::vector<float>
make_signal(int rate, double noise_s, double carrier_s, double tail_s, int am, uint64_t seed) {
    Rng rng{seed};
    const double sd = sqrt(1e-3 / 2.0);
    const double amp = sqrt(1e-3 * 100.0);
    const int n = (int)((noise_s + carrier_s + tail_s) * rate);
    const int on = (int)(noise_s * rate);
    const int off = (int)((noise_s + carrier_s) * rate);
    std::vector<float> iq((size_t)n * 2U);
    double mod = 0.0;
    for (int k = 0; k < n; k++) {
        double re = sd * rng.gauss();
        double im = sd * rng.gauss();
        if (k >= on && k < off) {
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
    s->post_downsample = post_downsample;
    s->squelch_mode = DSD_SQUELCH_MODE_NOISE;
    s->squelch_margin_db = 10;
    s->squelch_context.freq_hz = 162475000;
    s->squelch_context.rate_hz = rate_out * post_downsample;
    return s;
}

/* Frees a demod from new_monitor() with what full_demod() allocated for it (the post-decimator's taps and history). */
void
free_monitor(demod_state* s) {
    if (!s) {
        return;
    }
    dsd_neo_aligned_free(s->post_polydecim_taps);
    dsd_neo_aligned_free(s->post_polydecim_hist);
    dsd_neo_aligned_free(s);
}

struct Block {
    std::vector<float> lowpassed; /* the channel samples the discriminator ran on */
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

int
fail(const char* what) {
    DSD_FPRINTF(stderr, "noise squelch demod: %s\n", what);
    return 1;
}

/* The discriminator full_demod() runs, in double: the phase step between consecutive channel samples. */
void
reference_disc(const std::vector<float>& iq, float* prev, int* have_prev, std::vector<float>* out) {
    const size_t n = iq.size() / 2U;
    out->resize(n);
    for (size_t k = 0; k < n; k++) {
        const float cr = iq[k * 2U];
        const float cj = iq[(k * 2U) + 1U];
        if (!*have_prev) {
            prev[0] = cr;
            prev[1] = cj;
            *have_prev = 1;
        }
        const double re = ((double)cr * prev[0]) + ((double)cj * prev[1]);
        const double im = ((double)cj * prev[0]) - ((double)cr * prev[1]);
        (*out)[k] = (float)atan2(im, re);
        prev[0] = cr;
        prev[1] = cj;
    }
}

/* The noise squelch in full_demod() reads as one of its own does on the discriminator's outputs for the same channel
   samples, with the plan of the channel filter in force: the same windows, the same quieting within the discriminator's
   rounding. The carrier opens it and the noise closes it; nothing is zeroed, the level gate and the tracker stay out of
   it. */
int
fm_flags_follow_the_noise_squelch(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    const std::vector<float> iq = make_signal(rate, 1.0, 0.5, 1.0, 0, 0x5EEDULL);
    const int total = (int)(iq.size() / 2U);
    dsd_noise_squelch ref;
    DSD_MEMSET(&ref, 0, sizeof ref);
    dsd_noise_squelch_set_threshold(&ref, 10);
    int rc = 0;
    int ref_ready = 0;
    long opened = 0;
    long closed = 0;
    long closed_nonzero = 0;
    double worst = 0.0;
    float prev[2] = {0.0f, 0.0f};
    int have_prev = 0;
    std::vector<float> disc;
    for (int at = 0; at < total; at += 1000) {
        const int pairs = total - at < 1000 ? total - at : 1000;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        if (s->channel_squelched || !s->squelch_gate_open) {
            rc |= fail("the level gate ran under NOISE");
        }
        if (s->squelch_auto_ran) {
            rc |= fail("the tracker ran under NOISE on an FM channel with a band");
        }
        if (b.result.empty()) {
            continue;
        }
        if (!s->result_flags_active || !s->squelch_noise_ran || b.flags.size() != b.lowpassed.size() / 2U) {
            rc |= fail("no noise squelch flags under NOISE on the FM monitor");
            break;
        }
        if (!ref_ready) {
            dsd_noise_squelch_plan plan;
            if (dsd_noise_squelch_plan_design(&plan, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, NULL, 0,
                                              rate)
                    != 0
                || !dsd_noise_squelch_plan_equal(&plan, &s->noise_squelch.plan)) {
                rc |= fail("the noise squelch's plan is not the channel filter's");
            }
            dsd_noise_squelch_set_plan(&ref, &plan);
            ref_ready = 1;
        }
        reference_disc(b.lowpassed, prev, &have_prev, &disc);
        dsd_noise_squelch_process(&ref, disc.data(), b.lowpassed.data(), (int)disc.size(), NULL);
        if (ref.windows != s->noise_squelch.windows) {
            rc |= fail("the noise squelch did not run on every channel sample");
            break;
        }
        const double diff = fabs(ref.quieting_db - s->noise_squelch.quieting_db);
        worst = diff > worst ? diff : worst;
        for (size_t k = 0; k < b.flags.size(); k++) {
            if (b.flags[k] & DSD_SQUELCH_FLAG_CLOSED) {
                closed++;
                closed_nonzero += fabsf(b.result[k]) > 0.0f;
            } else {
                opened++;
            }
        }
    }
    if (worst > 0.05) {
        DSD_FPRINTF(stderr, "noise squelch demod: quieting %.3f dB from its own\n", worst);
        rc |= fail("the noise squelch did not read the discriminator's output");
    }
    if (opened < rate / 3 || opened > rate / 2 + rate / 10 || closed < rate) {
        DSD_FPRINTF(stderr, "noise squelch demod: %ld open, %ld closed\n", opened, closed);
        rc |= fail("the carrier did not open the gate, or the noise did not close it");
    }
    if (closed_nonzero < closed / 2) {
        rc |= fail("closed samples were zeroed in the demod");
    }
    /* LEVEL: no flags, the level gate as before. */
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    (void)run_block(s, iq.data(), 1000);
    if (s->result_flags_active || s->squelch_noise_ran) {
        rc |= fail("flags under LEVEL");
    }
    /* NOISE off the analog monitor: off. */
    s->squelch_mode = DSD_SQUELCH_MODE_NOISE;
    s->analog_family = 0;
    (void)run_block(s, iq.data(), 1000);
    if (s->result_flags_active || s->squelch_noise_ran) {
        rc |= fail("flags off the analog monitor");
    }
    free_monitor(s);
    return rc;
}

/* Where the noise squelch cannot run -- an AM channel, an FM one too narrow for a band above voice -- the tracker runs
   the setting as AUTO with the same N. */
int
noise_falls_back_to_auto(void) {
    const int rate = 24000;
    int rc = 0;

    const struct {
        int am;
        int width;
    } cases[] = {{1, 6000}, {0, 8000}, {0, 10000}};

    for (const auto& c : cases) {
        demod_state* s = new_monitor(c.am, rate, 1, c.width);
        if (!s) {
            return fail("alloc");
        }
        s->squelch_margin_db = 14;
        const std::vector<float> iq = make_signal(rate, 0.5, 0.0, 0.0, c.am, 0xFA11ULL);
        for (int at = 0; at < rate / 2; at += 1000) {
            (void)run_block(s, &iq[(size_t)at * 2U], 1000);
        }
        if (s->squelch_noise_ran || !s->squelch_auto_ran || !s->result_flags_active) {
            rc |= fail("the tracker did not stand in for the noise squelch");
        }
        if (s->squelch_floor.margin_db != 14) {
            rc |= fail("the stand-in did not take N as its margin");
        }
        if (!c.am && s->noise_squelch.plan.valid) {
            rc |= fail("a narrow FM channel has a noise squelch band");
        }
        free_monitor(s);
    }
    return rc;
}

/* A new context (a retune) or a stream reset starts the noise squelch over: closed, filters and windows empty. Runs
   under NOISE leave the tracker's context and floor alone, so a return to AUTO on its channel keeps its floor. */
int
context_restarts_the_noise_squelch(void) {
    const int rate = 24000;
    demod_state* s = new_monitor(0, rate, 1, 12500);
    if (!s) {
        return fail("alloc");
    }
    int rc = 0;
    /* AUTO learns channel A's floor. */
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    const std::vector<float> noise = make_signal(rate, 1.0, 0.0, 0.0, 0, 0xC0DEULL);
    for (int at = 0; at < rate; at += 1000) {
        (void)run_block(s, &noise[(size_t)at * 2U], 1000);
    }
    const double floor_a = s->squelch_floor.floor_power;
    if (s->squelch_floor.state != DSD_SQUELCH_FLOOR_KNOWN) {
        rc |= fail("no floor under AUTO");
    }
    /* NOISE on channel B, a carrier there: open. */
    s->squelch_mode = DSD_SQUELCH_MODE_NOISE;
    s->squelch_context.freq_hz = 172475000;
    const std::vector<float> carrier = make_signal(rate, 0.0, 0.5, 0.0, 0, 0xCA5EULL);
    Block b;
    for (int at = 0; at < rate / 2; at += 1000) {
        b = run_block(s, &carrier[(size_t)at * 2U], 1000);
    }
    if (!s->noise_squelch.gate_open || b.flags.empty() || (b.flags.back() & DSD_SQUELCH_FLAG_CLOSED)) {
        rc |= fail("a carrier did not open the noise squelch");
    }
    /* A retune to channel C: closed from its first sample, until its own first window. */
    s->squelch_context.freq_hz = 182475000;
    b = run_block(s, carrier.data(), 1000);
    if (b.flags.empty() || !(b.flags.front() & DSD_SQUELCH_FLAG_CLOSED) || s->noise_squelch.windows > 1U) {
        rc |= fail("a retune did not start the noise squelch over");
    }
    /* A stream reset at the same context starts it over too. */
    for (int at = 0; at < rate / 4; at += 1000) {
        (void)run_block(s, &carrier[(size_t)at * 2U], 1000);
    }
    dsd_demod_reset_filter_state(s);
    b = run_block(s, &carrier[2000], 1000);
    if (b.flags.empty() || !(b.flags.front() & DSD_SQUELCH_FLAG_CLOSED)) {
        rc |= fail("a stream reset did not start the noise squelch over");
    }
    /* Back to AUTO on channel A: its floor is the tracker's still, under its own context. */
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    s->squelch_context.freq_hz = 162475000;
    (void)run_block(s, noise.data(), 10);
    if (s->squelch_floor.state == DSD_SQUELCH_FLOOR_LEARNING
        || fabs(s->squelch_floor.floor_power - floor_a) > 1e-9 * floor_a) {
        rc |= fail("runs under NOISE disturbed the tracker's floor");
    }
    free_monitor(s);
    return rc;
}

/* An I/Q replay that decimates after the demodulator: the noise squelch runs at the channel rate and its flags reach
   the decimated output, open over the carrier and closed over the noise. */
int
post_decimation_carries_noise_flags(void) {
    const int rate_out = 24000;
    demod_state* s = new_monitor(0, rate_out, 2, 0);
    if (!s) {
        return fail("alloc");
    }
    const int channel_rate = rate_out * 2;
    const std::vector<float> iq = make_signal(channel_rate, 1.0, 1.0, 1.0, 0, 0xDEC1ULL);
    const int total = (int)(iq.size() / 2U);
    std::vector<uint8_t> flags;
    for (int at = 0; at < total; at += 2000) {
        const int pairs = total - at < 2000 ? total - at : 2000;
        Block b = run_block(s, &iq[(size_t)at * 2U], pairs);
        flags.insert(flags.end(), b.flags.begin(), b.flags.end());
    }
    int rc = 0;
    if (!s->noise_squelch.plan.valid || s->noise_squelch.plan.rate_hz != channel_rate) {
        rc |= fail("the noise squelch did not run at the channel rate");
    }
    /* At rate_out: noise 0..1 s, carrier 1..2 s, noise 2..3 s, less the filters' delays. */
    long open_noise = 0;
    long open_carrier = 0;
    for (size_t k = 0; k < flags.size(); k++) {
        const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
        if (k < (size_t)(rate_out * 9 / 10) || k > (size_t)(rate_out * 22 / 10)) {
            open_noise += open;
        } else if (k > (size_t)(rate_out * 11 / 10) && k < (size_t)(rate_out * 19 / 10)) {
            open_carrier += open;
        }
    }
    if (open_noise != 0 || open_carrier < rate_out * 8 / 10 - 1) {
        DSD_FPRINTF(stderr, "noise squelch demod: %ld open over noise, %ld over the carrier\n", open_noise,
                    open_carrier);
        rc |= fail("post-decimated flags do not follow the carrier");
    }
    free_monitor(s);
    return rc;
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

} // namespace

int
main(void) {
    int rc = 0;
    rc |= fm_flags_follow_the_noise_squelch();
    rc |= noise_falls_back_to_auto();
    rc |= context_restarts_the_noise_squelch();
    rc |= post_decimation_carries_noise_flags();
    rc |= flags_do_not_depend_on_block_cuts();
    if (rc == 0) {
        printf("DSP_NOISE_SQUELCH_DEMOD: OK\n");
    }
    return rc;
}
