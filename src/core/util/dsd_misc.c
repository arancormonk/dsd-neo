// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * dsd_misc.c
 * Misc Code that needs to be reorganized and sorted out
 *
 *
 *
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/fec/trellis.h>
#include <dsd-neo/fec/viterbi.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static const int PARITY[] = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
                             1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1};

// trellis_1_2 encode: source is in bits, result in bits
static void
trellis_encode(uint8_t result[], const uint8_t source[], int result_len, unsigned int reg) {
    /* Only the low 5 bits of the shift register are ever consulted, so mask
     * each step to keep the value bounded (avoids signed shift overflow). */
    for (int i = 0; i < result_len; i += 2) {
        reg = ((reg << 1) | source[i >> 1]) & 0x1FU;
        result[i] = PARITY[reg & 0x19];
        result[i + 1] = PARITY[reg & 0x17];
    }
}

// simplified trellis 2:1 decode; source and result in bits
// assumes that encoding was done with NTEST trailing zero bits
// result_len should be set to the actual number of data bits
// in the original unencoded message (excl. these trailing bits)
void
trellis_decode(uint8_t result[], const uint8_t source[], int result_len) {
    unsigned int reg = 0;
    int min_d = 9999;
    int min_bt = 0;

    enum { NTEST = 4, NTESTC = 1 << NTEST };

    uint8_t bt[NTEST];
    uint8_t tt[NTEST * 2];
    for (int p = 0; p < result_len; p++) {
        for (int i = 0; i < NTESTC; i++) {
            bt[0] = (i & 8) >> 3;
            bt[1] = (i & 4) >> 2;
            bt[2] = (i & 2) >> 1;
            bt[3] = (i & 1);
            trellis_encode(tt, bt, NTEST * 2, reg);
            int sum = 0;
            for (int j = 0; j < NTEST * 2; j++) {
                sum += tt[j] ^ source[p * 2 + j];
            }
            if (i == 0 || sum < min_d) {
                min_d = sum;
                min_bt = bt[0];
            }
        }
        result[p] = min_bt;
        reg = ((reg << 1) | (unsigned int)min_bt) & 0x1FU;
    }

    //debug output
}

//Original Copyright/License

/* -*- c++ -*- */
/*
 * NXDN Encoder/Decoder (C) Copyright 2019 Max H. Parke KA1RBI
 *
 * This file is part of OP25
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

//Ripped from libM17
#define K          5              //constraint length
#define NUM_STATES (1 << (K - 1)) //number of states

static uint32_t prevMetrics[NUM_STATES];
static uint32_t currMetrics[NUM_STATES];
static uint32_t prevMetricsData[NUM_STATES];
static uint32_t currMetricsData[NUM_STATES];
static uint16_t viterbi_history[244];

/**
* @brief Decode unpunctured convolutionally encoded data.
*
* @param out Destination array where decoded data is written.
* @param in Input data.
* @param len Input length in bits.
* @return Final Viterbi path metric (lower is better; not a BER count).
*/
uint32_t
viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len) {
    if (len > 244 * 2) {
        DSD_FPRINTF(stderr, "Input size exceeds max history\n");
    }

    viterbi_reset();

    size_t pos = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t s0 = in[i];
        uint16_t s1 = in[i + 1];

        viterbi_decode_bit(s0, s1, pos);
        pos++;
    }
    uint32_t err = viterbi_chainback(out, pos, len / 2);

    //debug

    return err;
}

/**
* @brief Decode punctured convolutionally encoded data.
*
* @param out Destination array where decoded data is written.
* @param in Input data.
* @param punct Puncturing matrix.
* @param in_len Input data length.
* @param p_len Puncturing matrix length (entries).
* @return Path metric with neutral puncture offsets removed.
*/
uint32_t
viterbi_decode_punctured(uint8_t* out, const uint16_t* in, const uint8_t* punct, const uint16_t in_len,
                         const uint16_t p_len) {
    if (in_len > 244 * 2) {
        DSD_FPRINTF(stderr, "Input size exceeds max history\n");
    }

    uint16_t umsg[244 * 2] = {0}; //unpunctured message
    uint8_t p = 0;                //puncturer matrix entry
    uint16_t u = 0;               //bits count - unpunctured message
    uint16_t i = 0;               //bits read from the input message

    while (i < in_len) {
        if (punct[p]) {
            umsg[u] = in[i];
            i++;
        } else {
            umsg[u] = 0x7FFF;
        }

        u++;
        p++;
        p %= p_len;
    }

    //debug

    return viterbi_decode(out, umsg, u) - (u - in_len) * 0x7FFF;
}

/**
* @brief Decode one bit and update trellis.
*
* @param s0 Cost of the first symbol.
* @param s1 Cost of the second symbol.
* @param pos Bit position in history.
*/
void
viterbi_decode_bit(uint16_t s0, uint16_t s1, const size_t pos) {
    static const uint16_t COST_TABLE_0[] = {0, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    static const uint16_t COST_TABLE_1[] = {0, 0xFFFF, 0xFFFF, 0, 0, 0xFFFF, 0xFFFF, 0};

    for (int i = 0; i < NUM_STATES / 2; i++) {
        uint32_t metric = q_abs_diff(COST_TABLE_0[i], s0) + q_abs_diff(COST_TABLE_1[i], s1);

        uint32_t m0 = prevMetrics[i] + metric;
        uint32_t m1 = prevMetrics[i + NUM_STATES / 2] + (0x1FFFE - metric);

        uint32_t m2 = prevMetrics[i] + (0x1FFFE - metric);
        uint32_t m3 = prevMetrics[i + NUM_STATES / 2] + metric;

        int i0 = 2 * i;
        int i1 = i0 + 1;

        if (m0 >= m1) {
            viterbi_history[pos] |= (1 << i0);
            currMetrics[i0] = m1;
        } else {
            viterbi_history[pos] &= ~(1 << i0);
            currMetrics[i0] = m0;
        }

        if (m2 >= m3) {
            viterbi_history[pos] |= (1 << i1);
            currMetrics[i1] = m3;
        } else {
            viterbi_history[pos] &= ~(1 << i1);
            currMetrics[i1] = m2;
        }
    }

    //swap
    uint32_t tmp[NUM_STATES];
    for (int i = 0; i < NUM_STATES; i++) {
        tmp[i] = currMetrics[i];
    }
    for (int i = 0; i < NUM_STATES; i++) {
        currMetrics[i] = prevMetrics[i];
        prevMetrics[i] = tmp[i];
    }
}

/**
* @brief History chainback to obtain final byte array.
*
* @param out Destination byte array for decoded data.
* @param pos Starting position for the chainback.
* @param len Length of the output in bits.
* @return Minimum Viterbi cost at the end of the decode sequence.
*/
uint32_t
viterbi_chainback(uint8_t* out, size_t pos, uint16_t len) {
    /* This decoder path assumes a terminated trellis (tail bits), whose final
     * encoder state is zero. */
    uint8_t state = 0;
    size_t bitPos = len + 4;

    DSD_MEMSET(out, 0, (len - 1) / 8 + 1);

    while (pos > 0) {
        bitPos--;
        pos--;
        uint16_t bit = viterbi_history[pos] & ((1 << (state >> (9 - K))));
        state >>= 1;
        if (bit) {
            state |= 0x80;
            out[bitPos / 8] |= 1 << (7 - (bitPos % 8));
        }
    }

    //debug

    uint32_t best_cost = prevMetrics[0];
    for (size_t i = 1; i < NUM_STATES; i++) {
        if (prevMetrics[i] < best_cost) {
            best_cost = prevMetrics[i];
        }
    }
    return best_cost;
}

/**
 * @brief Reset the decoder state. No args.
 *
 */
void
viterbi_reset(void) {
    DSD_MEMSET((uint8_t*)viterbi_history, 0, sizeof(viterbi_history));
    DSD_MEMSET((uint8_t*)currMetrics, 0, sizeof(currMetrics));
    DSD_MEMSET((uint8_t*)prevMetrics, 0, sizeof(prevMetrics));
    DSD_MEMSET((uint8_t*)currMetricsData, 0, sizeof(currMetricsData));
    DSD_MEMSET((uint8_t*)prevMetricsData, 0, sizeof(prevMetricsData));
}

uint16_t
q_abs_diff(const uint16_t v1, const uint16_t v2) {
    if (v2 > v1) {
        return v2 - v1;
    }
    return v1 - v2;
}

//original sources
//--------------------------------------------------------------------
// M17 C library - decode/viterbi.c
//
// This file contains:
// - the Viterbi decoder
//
// Wojciech Kaczmarski, SP5WWP
// M17 Project, 29 December 2023
//--------------------------------------------------------------------

//--------------------------------------------------------------------
// M17 C library - math/math.c
//
// This file contains:
// - absolute difference value
// - Euclidean norm (L2) calculation for n-dimensional vectors (float)
// - soft-valued arrays to integer conversion (and vice-versa)
// - fixed-valued multiplication and division
//
// Wojciech Kaczmarski, SP5WWP
// M17 Project, 29 December 2023
//--------------------------------------------------------------------

//audio filter stuff sourced from: https://github.com/NedSimao/FilteringLibrary
//no license / information provided in source code
#define PI 3.141592653

static void
LPFilter_Init(LPFilter* filter, float cutoffFreqHz, float sampleTimeS) {

    float RC = 0.0;
    RC = 1.0 / (2 * PI * cutoffFreqHz);
    filter->coef[0] = sampleTimeS / (sampleTimeS + RC);
    filter->coef[1] = RC / (sampleTimeS + RC);

    filter->v_out[0] = 0.0;
    filter->v_out[1] = 0.0;
}

static float
LPFilter_Update(LPFilter* filter, float v_in) {

    filter->v_out[1] = filter->v_out[0];
    filter->v_out[0] = (filter->coef[0] * v_in) + (filter->coef[1] * filter->v_out[1]);

    return (filter->v_out[0]);
}

/********************************************************************************************************
 *                              HIGH PASS FILTER
********************************************************************************************************/
static void
HPFilter_Init(HPFilter* filter, float cutoffFreqHz, float sampleTimeS) {

    float RC = 0.0;
    RC = 1.0 / (2 * PI * cutoffFreqHz);

    filter->coef = RC / (sampleTimeS + RC);

    filter->v_in[0] = 0.0;
    filter->v_in[1] = 0.0;

    filter->v_out[0] = 0.0;
    filter->v_out[1] = 0.0;
}

static float
HPFilter_Update(HPFilter* filter, float v_in) {

    filter->v_in[1] = filter->v_in[0];
    filter->v_in[0] = v_in;

    filter->v_out[1] = filter->v_out[0];
    filter->v_out[0] = filter->coef * (filter->v_in[0] - filter->v_in[1] + filter->v_out[1]);

    return (filter->v_out[0]);
}

void
init_audio_filters(dsd_state* state, int sample_rate_hz) {
    float analog_Fs = (sample_rate_hz > 0) ? (float)sample_rate_hz : 48000.0f;
    float analog_Ts = 1.0f / analog_Fs;
    const float digital_Fs = 8000.0f;
    const float digital_Ts = 1.0f / digital_Fs;

    //still not sure if this is even correct or not, but 48k sounds good now
    LPFilter_Init(&state->RCFilter, 960.0f, analog_Ts);
    HPFilter_Init(&state->HRCFilter, 960.0f, analog_Ts);

    //left and right variants for stereo output testing on digital voice samples
    //Digital voice path runs on 8 kHz frames; keep these filters keyed to that rate.
    LPFilter_Init(&state->RCFilterL, 960.0f, digital_Ts);
    LPFilter_Init(&state->RCFilterR, 960.0f, digital_Ts);
    // The digital voice high-pass runs at its fixed 8 kHz design; an input change starts it over (issue #653).
    dsd_voice_hpf_reset(state, -1);
}

//FUNCTIONS for handing use of above filters

// LPF short path
void
lpf(dsd_state* state, short* input, int len) {
    int i;
    for (i = 0; i < len; i++) {
        input[i] = (short)LPFilter_Update(&state->RCFilter, (float)input[i]);
    }
}

// LPF float path for analog monitor
void
lpf_f(dsd_state* state, float* input, int len) {
    int i;
    for (i = 0; i < len; i++) {
        input[i] = LPFilter_Update(&state->RCFilter, input[i]);
    }
}

static short
clamp_float_to_short(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (short)value;
}

// HPF short path
void
dsd_hpf(dsd_state* state, short* input, int len) {
    int i;
    for (i = 0; i < len; i++) {
        input[i] = (short)HPFilter_Update(&state->HRCFilter, (float)input[i]);
    }
}

// HPF float path for analog monitor
void
hpf_f(dsd_state* state, float* input, int len) {
    int i;
    for (i = 0; i < len; i++) {
        input[i] = HPFilter_Update(&state->HRCFilter, input[i]);
    }
}

/********************************************************************************************************
 *                              DIGITAL VOICE HIGH-PASS (issue #653)
 ********************************************************************************************************/

/* A sample whose magnitude reaches this is not a finite audio sample. */
static const double k_voice_hpf_finite_limit = 1e30;
static const double k_voice_hpf_pi = 3.14159265358979323846;
/* The Butterworth section's Q, 1/sqrt(2). */
static const double k_voice_hpf_q = 0.70710678118654752440;

typedef struct {
    double b0, b1, b2, a1, a2;
} voice_hpf_coefs;

/* RBJ cookbook high-pass with Q = 1/sqrt(2): the bilinear 2nd-order Butterworth, prewarped so its -3 dB corner lands
   at DSD_VOICE_HPF_CORNER_HZ. b0 - b1 + b2 equals a0 - a1 + a2, so the gain at Nyquist is exactly 1, and the
   Butterworth response is monotonic: the voice band passes at unity, with no ripple. */
static voice_hpf_coefs
voice_hpf_design(void) {
    const double w0 = 2.0 * k_voice_hpf_pi * DSD_VOICE_HPF_CORNER_HZ / DSD_VOICE_HPF_RATE_HZ;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2.0 * k_voice_hpf_q);
    const double a0 = 1.0 + alpha;
    voice_hpf_coefs c;
    c.b0 = ((1.0 + cw) / 2.0) / a0;
    c.b1 = -(1.0 + cw) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0 * cw) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

void
dsd_voice_hpf_reset(dsd_state* state, int slot) {
    if (!state) {
        return;
    }
    // The reception the state ran for goes with it, so the next frames start a reception of their own: a call learned
    // partway through them is not taken for a change from the call before the reset.
    for (int s = 0; s < 2; s++) {
        if (slot < 0 || slot == s) {
            state->voice_hpf[s].s1 = 0.0;
            state->voice_hpf[s].s2 = 0.0;
            state->voice_hpf[s].call_epoch = 0U;
            state->voice_hpf[s].protocol_family = 0;
        }
    }
}

void
dsd_voice_hpf_process(dsd_state* state, int slot, float* buf, size_t n) {
    if (!state || !buf || slot < 0 || slot > 1) {
        return;
    }
    const voice_hpf_coefs c = voice_hpf_design();
    dsd_voice_hpf_state* f = &state->voice_hpf[slot];
    for (size_t i = 0; i < n; i++) {
        const double x = (double)buf[i];
        if (!(fabs(x) < k_voice_hpf_finite_limit)) {
            buf[i] = 0.0f;
            continue;
        }
        const double y = (c.b0 * x) + f->s1;
        f->s1 = (c.b1 * x) - (c.a1 * y) + f->s2;
        f->s2 = (c.b2 * x) - (c.a2 * y);
        buf[i] = (float)y;
    }
}

double
dsd_voice_hpf_gain_db(double hz) {
    const voice_hpf_coefs c = voice_hpf_design();
    const double w = 2.0 * k_voice_hpf_pi * hz / DSD_VOICE_HPF_RATE_HZ;
    const double c1 = cos(w);
    const double s1 = -sin(w);
    const double c2 = cos(2.0 * w);
    const double s2 = -sin(2.0 * w);
    const double nr = c.b0 + (c.b1 * c1) + (c.b2 * c2);
    const double ni = (c.b1 * s1) + (c.b2 * s2);
    const double dr = 1.0 + (c.a1 * c1) + (c.a2 * c2);
    const double di = (c.a1 * s1) + (c.a2 * s2);
    const double den = (dr * dr) + (di * di);
    if (!(den > 0.0)) {
        return -300.0;
    }
    const double mag2 = ((nr * nr) + (ni * ni)) / den;
    return mag2 > 1e-30 ? 10.0 * log10(mag2) : -300.0;
}

/* The digital high-pass on a short buffer, slot 1 (left) or slot 2 (right) state, saturating to int16. The voice
   chain (dsd_voice_chain_run()) runs it on the float frame before the gain; these stay for callers that hold int16
   samples. */
static void
voice_hpf_short(dsd_state* state, int slot, short* input, int len) {
    if (!state || !input || len <= 0) {
        return;
    }
    float tmp[DSD_VOICE_FRAME_SAMPLES];
    int done = 0;
    while (done < len) {
        int chunk = len - done;
        if (chunk > DSD_VOICE_FRAME_SAMPLES) {
            chunk = DSD_VOICE_FRAME_SAMPLES;
        }
        for (int i = 0; i < chunk; i++) {
            tmp[i] = (float)input[done + i];
        }
        dsd_voice_hpf_process(state, slot, tmp, (size_t)chunk);
        for (int i = 0; i < chunk; i++) {
            input[done + i] = clamp_float_to_short(tmp[i]);
        }
        done += chunk;
    }
}

//hpf digital left
void
hpf_dL(dsd_state* state, short* input, int len) {
    voice_hpf_short(state, 0, input, len);
}

//hpf digital right
void
hpf_dR(dsd_state* state, short* input, int len) {
    voice_hpf_short(state, 1, input, len);
}

/* The old "PBF" pair, a one-pole 8 kHz high-pass and 12 kHz low-pass that the monitor ran as its voice filter and
   that took 1 kHz 18 dB down (issue #518). The monitor's voice band-pass is now dsd_analog_audio_process_f()'s; these
   stay as pass-through shims so out-of-tree callers still link. */
void
pbf(dsd_state* state, short* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

void
pbf_f(dsd_state* state, float* input, int len) {
    (void)state;
    (void)input;
    (void)len;
}

/*
 * Mean power (RMS^2 proxy) without sqrt, modeled after mean_power() in rtl_sdr_fm.cpp.
 * Computes a DC-corrected average of squares to avoid costly sqrt operations.
 */
double
raw_pwr(const short* samples, int len, int step) {
    double p = 0.0;
    double t = 0.0;
    int count = 0;
    const double kScale = 1.0 / 32768.0;
    for (int i = 0; i < len; i += step) {
        double s = (double)samples[i] * kScale;
        t += s;
        p += s * s;
        count++;
    }
    if (count == 0) {
        return 0.0;
    }
    /* DC-corrected energy ≈ p - (t^2)/count */
    double dc_corr = (t * t) / (double)count;
    double energy = p - dc_corr;
    if (energy < 0.0) {
        energy = 0.0;
    }
    return energy / (double)count;
}

/*
 * Convert a mean power value (RMS^2 proxy on normalized samples) to dB.
 * The input is the DC-corrected mean of squares on normalized samples.
 * Output is clamped to [-120.0 dB, 0.0 dB] for stable display.
 */
double
pwr_to_dB(double mean_power) {
    if (mean_power <= 0) {
        return -120.0;
    }
    double dB = 10.0 * log10(mean_power);
    if (dB > 0.0) {
        dB = 0.0; /* never exceed 0 dB */
    }
    if (dB < -120.0) {
        dB = -120.0; /* floor for readability */
    }
    return dB;
}

/* Inverse of pwr_to_dB: convert dB back to mean power threshold. */
double
dB_to_pwr(double dB) {
    if (dB >= 0.0) {
        return 1.0;
    }
    if (dB < -200.0) {
        dB = -200.0; /* avoid denormals */
    }
    /* Use exp(dB * ln(10)/10) instead of pow(10, dB/10) to avoid generic pow overhead */
    const double kLn10_over_10 = 2.302585092994046 / 10.0; /* ln(10)/10 */
    double pwr = exp(dB * kLn10_over_10);
    if (pwr < 0.0) {
        pwr = 0.0;
    }
    if (pwr > 1.0) {
        pwr = 1.0;
    }
    return pwr;
}

/*
 * Render a squelch threshold for display, saying "off" when it gates nothing.
 *
 * Kept next to pwr_to_dB() because it is that function's display counterpart for
 * the one case pwr_to_dB() cannot express: its -120 dB result means both "a
 * measurement of zero" and "a threshold that was never applied", and only the
 * caller's context separates them. For a threshold, this function does.
 */
int
dsd_squelch_format(double mean_power, const char* unit, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    if (dsd_squelch_is_off(mean_power)) {
        DSD_SNPRINTF(out, out_size, "off");
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%.1f%s", pwr_to_dB(mean_power), unit ? unit : "");
    return 0;
}
