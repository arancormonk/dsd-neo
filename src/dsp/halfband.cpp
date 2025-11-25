// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Half-band FIR decimator implementation for real sequences.
 *
 * Implements float half-band decimation by 2 with persistent history.
 */

#include <stdint.h>
#include <string.h>

#include <dsd-neo/dsp/halfband.h>

/* Half-band coefficients normalized to unity gain (see declaration in header) */
const float hb_q15_taps[HB_TAPS] = {-108.0f / 32768.0f, 0.0f, 1800.0f / 32768.0f, 0.0f, -500.0f / 32768.0f, 0.0f,
                                    7000.0f / 32768.0f, 0.5f, 7000.0f / 32768.0f, 0.0f, -500.0f / 32768.0f, 0.0f,
                                    1800.0f / 32768.0f, 0.0f, -108.0f / 32768.0f};

/*
 * Higher-order half-band coefficients generated via windowed-sinc (Blackman)
 * and renormalized so that the center tap is exactly 0.5 and the sum equals 1.
 * Odd-indexed taps are zero except the center.
 */
const float hb31_q15_taps[31] = {0.0f,
                                 0.0f,
                                 13.0f / 32768.0f,
                                 0.0f,
                                 -73.0f / 32768.0f,
                                 0.0f,
                                 233.0f / 32768.0f,
                                 0.0f,
                                 -587.0f / 32768.0f,
                                 0.0f,
                                 1314.0f / 32768.0f,
                                 0.0f,
                                 -2953.0f / 32768.0f,
                                 0.0f,
                                 10244.0f / 32768.0f,
                                 16386.0f / 32768.0f,
                                 10244.0f / 32768.0f,
                                 0.0f,
                                 -2953.0f / 32768.0f,
                                 0.0f,
                                 1314.0f / 32768.0f,
                                 0.0f,
                                 -587.0f / 32768.0f,
                                 0.0f,
                                 233.0f / 32768.0f,
                                 0.0f,
                                 -73.0f / 32768.0f,
                                 0.0f,
                                 13.0f / 32768.0f,
                                 0.0f,
                                 0.0f};

const float hb23_q15_taps[23] = {0.0f,
                                 0.0f,
                                 38.0f / 32768.0f,
                                 0.0f,
                                 -238.0f / 32768.0f,
                                 0.0f,
                                 865.0f / 32768.0f,
                                 0.0f,
                                 -2559.0f / 32768.0f,
                                 0.0f,
                                 10087.0f / 32768.0f,
                                 16382.0f / 32768.0f,
                                 10087.0f / 32768.0f,
                                 0.0f,
                                 -2559.0f / 32768.0f,
                                 0.0f,
                                 865.0f / 32768.0f,
                                 0.0f,
                                 -238.0f / 32768.0f,
                                 0.0f,
                                 38.0f / 32768.0f,
                                 0.0f,
                                 0.0f};

/**
 * @brief Decimate one real-valued channel by 2 using a half-band FIR.
 *
 * Applies a 15-tap half-band low-pass to input samples and writes every second
 * filtered sample to the output. Maintains a left-wing history across calls to
 * preserve continuity at block boundaries.
 *
 * @param in Pointer to real input samples (length in_len).
 * @param in_len Number of input samples.
 * @param out Output buffer, size must be at least in_len/2.
 * @param hist Persistent history of length HB_TAPS-1 (left wing).
 * @return Number of output samples written (in_len/2).
 */
int
hb_decim2_real(const float* in, int in_len, float* out, float* hist) {
    const int hist_len = HB_TAPS - 1;
    float last = (in_len > 0) ? in[in_len - 1] : 0.0f;
    int out_len = in_len >> 1; /* floor */
    const float c0 = hb_q15_taps[0];
    const float c2 = hb_q15_taps[2];
    const float c4 = hb_q15_taps[4];
    const float c6 = hb_q15_taps[6];
    const float c7 = hb_q15_taps[7];
    for (int n = 0; n < out_len; n++) {
        int center_idx = hist_len + (n << 1);
        auto get_sample = [&](int src_idx) -> float {
            if (src_idx < hist_len) {
                return hist[src_idx];
            } else {
                int rel = src_idx - hist_len;
                return (rel < in_len) ? in[rel] : last;
            }
        };
        float xc = get_sample(center_idx);
        float xm1 = get_sample(center_idx - 1);
        float xp1 = get_sample(center_idx + 1);
        float xm3 = get_sample(center_idx - 3);
        float xp3 = get_sample(center_idx + 3);
        float xm5 = get_sample(center_idx - 5);
        float xp5 = get_sample(center_idx + 5);
        float xm7 = get_sample(center_idx - 7);
        float xp7 = get_sample(center_idx + 7);
        float acc = 0.0f;
        acc += c7 * xc;
        acc += c6 * (xm1 + xp1);
        acc += c4 * (xm3 + xp3);
        acc += c2 * (xm5 + xp5);
        acc += c0 * (xm7 + xp7);
        out[n] = acc;
    }
    if (in_len >= hist_len) {
        memcpy(hist, in + (in_len - hist_len), (size_t)hist_len * sizeof(float));
    } else {
        int need = hist_len - in_len;
        if (need > 0) {
            memmove(hist, hist + in_len, (size_t)need * sizeof(float));
        }
        memcpy(hist + need, in, (size_t)in_len * sizeof(float));
    }
    return out_len;
}
