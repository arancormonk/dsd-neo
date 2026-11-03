// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Adjacent-channel rejection of the digital channel filter on the live chain (issue #806).
 *
 * A complex tone runs through full_demod() as a live capture delivers it: the half-band cascade, the live I/Q rate
 * step where the capture rate is device-forced, then the channel filter of a digital profile, read back as I/Q
 * (raw_demod). The rejection is the tone's output power against a tone inside the channel's passband.
 *
 * - An RTL-SDR at the 72 kHz DSP bandwidth: 288 kS/s through two passes, no step. Every non-TETRA digital profile ran
 *   the 63-tap fallback prototype there, built for 24 kHz and cut at a third of the rate (about 24 kHz for WIDE and
 *   the P25 and ProVoice profiles), so a neighbour 12.5 kHz away passed at full level and a P25 control channel next
 *   to one decoded nothing (DECODE_IQ_P25P1_*_ADJACENT_72K).
 * - Airspy-shaped captures: 78,125 and 93,750 Hz (here 312.5 and 375 kS/s through two passes) stepped to 48 kHz. The
 *   48 kHz designs always fitted; these legs check the step and the channel filter together, including tones above the
 *   48 kHz chain's Nyquist that only the step keeps out.
 *
 * Every neighbour tone sits at least 900 Hz past its profile's cutoff, where the 1200 Hz Blackman design is 50 dB down
 * (test_dsp_channel_filters.cpp, test_width_response()): 9.5, 12.5 and 15.5 kHz either side, the near edge, centre and
 * far edge of a P25 or 12.5 kHz neighbour's main lobe.
 */

#include <cmath>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

#include "dsd-neo/core/safe_api.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
/* The wanted tone, inside every profile's protected passband (6.25 kHz profiles protect 3125 Hz). */
constexpr double kWantedHz = 1000.0;
constexpr double kToneAmplitude = 0.25;
/* Run this long, and measure after the first part: the half-band cascade, the step and a 288-tap channel filter all
   settle within a few milliseconds. */
constexpr double kRunSeconds = 0.24;
constexpr double kSettleSeconds = 0.08;
constexpr double kMinRejectionDb = 50.0;

/* One leg: a capture at capture_hz on a target_hz DSP bandwidth, from an RTL-SDR (programmed rate is the request, no
   step) or a device that forces its own rate (Airspy: the step lands on the target). */
struct Leg {
    const char* label;
    dsd_rate_plan_backend backend;
    int capture_hz;
    int target_hz;
    int stepped;
};

struct Profile {
    const char* name;
    int profile;
};

int
plan_leg(const Leg& leg, dsd_live_iq_step_plan* out) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = leg.backend;
    in.request_hz = (uint32_t)(leg.stepped ? leg.target_hz : leg.capture_hz);
    in.programmed_hz = (uint32_t)leg.capture_hz;
    in.readback_hz = (uint32_t)leg.capture_hz;
    in.target_hz = (uint32_t)leg.target_hz;
    in.max_passes = 10;
    dsd_rate_plan rp;
    if (dsd_rate_plan_compute(&in, &rp) != DSD_RATE_PLAN_OK) {
        DSD_FPRINTF(stderr, "adjacent %s: rate plan refused: %s\n", leg.label, rp.message);
        return 1;
    }
    if (rp.step.enabled != leg.stepped || (int)rp.step_out_hz != leg.target_hz) {
        DSD_FPRINTF(stderr, "adjacent %s: plan step %d to %u Hz, want step %d to %d Hz\n", leg.label, rp.step.enabled,
                    (unsigned)rp.step_out_hz, leg.stepped, leg.target_hz);
        return 1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->enabled = rp.step.enabled;
    out->passes = rp.passes;
    out->hb_out_num = rp.hb_out_num;
    out->hb_out_shift = rp.hb_out_shift;
    out->L = rp.step.L;
    out->M = rp.step.M;
    out->target_hz = (int)rp.target_hz;
    out->protect_hz = rp.step.protect_hz;
    out->stop_hz = rp.step.stop_hz;
    return 0;
}

demod_state*
make_state(const Leg& leg, const dsd_live_iq_step_plan& plan, int profile) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return nullptr;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = leg.target_hz;
    s->rate_out = leg.target_hz;
    s->complex_rate = leg.target_hz;
    s->downsample_passes = plan.passes;
    s->mode_demod = &raw_demod;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile;
    if (plan.enabled) {
        int rc = dsd_demod_live_step_prepare(s, &plan);
        if (rc == DSD_LIVE_IQ_STEP_OK) {
            rc = dsd_live_iq_step_commit(&s->live_iq_step);
        }
        dsd_live_iq_step_finish(&s->live_iq_step);
        if (rc != DSD_LIVE_IQ_STEP_OK) {
            DSD_FPRINTF(stderr, "adjacent %s: step refused (%d)\n", leg.label, rc);
            dsd_live_iq_step_disable(&s->live_iq_step);
            dsd_neo_aligned_free(s);
            return nullptr;
        }
    }
    return s;
}

/* The mean output power of a tone at tone_hz, after the chain settles; negative on failure. */
double
tone_output_power(const Leg& leg, const dsd_live_iq_step_plan& plan, int profile, double tone_hz) {
    demod_state* s = make_state(leg, plan, profile);
    if (!s) {
        return -1.0;
    }
    const int block_pairs = leg.capture_hz / 50;
    const int blocks = (int)(kRunSeconds * 50.0);
    const int settle_outputs = (int)(kSettleSeconds * (double)leg.target_hz);
    const double w = 2.0 * kPi * tone_hz / (double)leg.capture_hz;
    std::vector<float> in((size_t)block_pairs * 2U);
    long long n = 0;
    long long outputs = 0;
    double acc = 0.0;
    long long measured = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < block_pairs; i++, n++) {
            in[(size_t)i * 2U] = (float)(kToneAmplitude * std::cos(w * (double)n));
            in[(size_t)i * 2U + 1U] = (float)(kToneAmplitude * std::sin(w * (double)n));
        }
        DSD_MEMCPY(s->input_cb_buf, in.data(), in.size() * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = block_pairs * 2;
        full_demod(s);
        for (int i = 0; i + 1 < s->result_len; i += 2, outputs++) {
            if (outputs < settle_outputs) {
                continue;
            }
            const double re = (double)s->result[i];
            const double im = (double)s->result[i + 1];
            acc += re * re + im * im;
            measured++;
        }
    }
    dsd_live_iq_step_disable(&s->live_iq_step);
    dsd_neo_aligned_free(s);
    if (measured < (long long)(0.1 * (double)leg.target_hz)) {
        DSD_FPRINTF(stderr, "adjacent %s: only %lld settled outputs\n", leg.label, measured);
        return -1.0;
    }
    return acc / (double)measured;
}

int
check_profile(const Leg& leg, const dsd_live_iq_step_plan& plan, const Profile& p, const std::vector<double>& tones) {
    const double wanted = tone_output_power(leg, plan, p.profile, kWantedHz);
    const double in_power = kToneAmplitude * kToneAmplitude;
    if (!(wanted > 0.0) || std::fabs(10.0 * std::log10(wanted / in_power)) > 1.0) {
        DSD_FPRINTF(stderr, "adjacent %s %s: the %.0f Hz wanted tone passes at %.2f dB, want within 1 dB\n", leg.label,
                    p.name, kWantedHz, wanted > 0.0 ? 10.0 * std::log10(wanted / in_power) : -999.0);
        return 1;
    }
    int rc = 0;
    double worst_db = 0.0;
    for (double tone_hz : tones) {
        const double got = tone_output_power(leg, plan, p.profile, tone_hz);
        if (got < 0.0) {
            return 1;
        }
        const double rejection_db = 10.0 * std::log10(wanted / (got > 1e-30 ? got : 1e-30));
        if (worst_db == 0.0 || rejection_db < worst_db) {
            worst_db = rejection_db;
        }
        if (rejection_db < kMinRejectionDb) {
            DSD_FPRINTF(stderr, "adjacent %s %s: a %+.0f Hz neighbour is only %.1f dB down (want %.0f dB)\n", leg.label,
                        p.name, tone_hz, rejection_db, kMinRejectionDb);
            rc = 1;
        }
    }
    DSD_FPRINTF(stderr, "adjacent %s %s: neighbours at least %.1f dB down\n", leg.label, p.name, worst_db);
    return rc;
}

} // namespace

int
main(void) {
    const Leg legs[] = {
        {"RTL 72 kHz", DSD_RATE_PLAN_BACKEND_RTL_USB, 288000, 72000, 0},
        {"Airspy 78125->48k", DSD_RATE_PLAN_BACKEND_AIRSPY, 312500, 48000, 1},
        {"Airspy 93750->48k", DSD_RATE_PLAN_BACKEND_AIRSPY, 375000, 48000, 1},
    };
    /* Both fallback families (the WIDE prototype: WIDE, ProVoice, P25 and TETRA's WIDE plan; the 6.25/12.5 kHz one). */
    const Profile profiles[] = {
        {"P25 C4FM", DSD_CH_LPF_PROFILE_P25_C4FM},
        {"P25 CQPSK", DSD_CH_LPF_PROFILE_P25_CQPSK},
        {"ProVoice", DSD_CH_LPF_PROFILE_PROVOICE},
        {"WIDE", DSD_CH_LPF_PROFILE_WIDE},
        {"12K5", DSD_CH_LPF_PROFILE_12K5},
        {"6K25", DSD_CH_LPF_PROFILE_6K25},
        {"inactive TETRA", DSD_CH_LPF_PROFILE_TETRA},
    };
    int rc = 0;
    for (const Leg& leg : legs) {
        dsd_live_iq_step_plan plan;
        if (plan_leg(leg, &plan) != 0) {
            rc = 1;
            continue;
        }
        std::vector<double> tones = {9500.0, 12500.0, 15500.0, -9500.0, -12500.0, -15500.0};
        if (leg.stepped) {
            /* Above the 48 kHz stream's 24 kHz Nyquist: only the step's own filter keeps them from aliasing in. */
            tones.push_back(30000.0);
            tones.push_back(-34000.0);
        }
        for (const Profile& p : profiles) {
            rc |= check_profile(leg, plan, p, tones);
        }
    }
    return rc;
}
