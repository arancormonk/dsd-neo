// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The auto squelch inside full_demod() (issue #518 follow-up): the tracker runs on the channel-filtered samples with
 * the plan of the channel filter in force, flags each sample instead of zeroing anything, and the flags follow the
 * samples through the AM detector, the post-decimator (both paths) and the resampler. The same input cut into other
 * blocks gives the same flags. It also runs on the 9600 bit/s FSK path EDACS analog voice reads and on the M17
 * encoder's monitor (issue #625), and closes within DSD_SQUELCH_CLOSE_DELAY_MS of a carrier dropping.
 */

#include <cmath>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/resampler.h>
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
    s->post_downsample = post_downsample;
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
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

/* The plan follows the channel: another width keeps the floor, rescaled; a half-band stage and no channel filter design
   their own plans. */
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
    s->downsample_passes = 2;
    (void)run_block(s, iq.data(), 1000);
    (void)dsd_squelch_floor_plan_design(&want, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, hb_q15_taps,
                                        HB_TAPS, rate);
    if (!dsd_squelch_floor_plan_equal(&want, &s->squelch_floor.plan)) {
        rc |= fail("the plan took the wrong half-band stage");
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

} // namespace

int
main(void) {
    int rc = 0;
    rc |= fm_flags_follow_the_tracker();
    rc |= plan_follows_the_channel();
    rc |= context_moves_the_floor();
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
    if (rc == 0) {
        printf("DSP_SQUELCH_AUTO_DEMOD: OK\n");
    }
    return rc;
}
