// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for SIMD u8->s16 widening and 90° rotate+widen. */

#include <dsd-neo/dsp/simd_widen.h>
#include <stdio.h>

static int
arrays_equal(const int16_t* a, const int16_t* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    // 4 complex samples (8 bytes)
    const unsigned char src[8] = {127, 127, 130, 130, 255, 0, 0, 255};
    int16_t dst[8] = {0};
    int16_t ref[8] = {0};

    // widen center 127 → s16 scaled by 256 with saturation
    widen_u8_to_s16_bias127(src, dst, 8);
    for (int i = 0; i < 8; i++) {
        int v = (int)src[i] - 127; // [-127,128]
        if (v > 127) {
            v = 127; // avoid 128*256 overflow
        }
        v <<= 8; // scale
        if (v > 32767) {
            v = 32767; // saturate (paranoia)
        }
        if (v < -32768) {
            v = -32768;
        }
        ref[i] = (int16_t)v;
    }
    if (!arrays_equal(dst, ref, 8)) {
        fprintf(stderr, "SIMD widen: mismatch\n");
        return 1;
    }

    // rotate 90° with pattern per implementation: (I0,Q0),(I1,Q1)->(-Q1, I1),(I2,Q2)->(-I2,-Q2),(I3,Q3)->(Q3,-I3)
    for (int i = 0; i < 8; i++) {
        dst[i] = 0;
    }
    widen_rotate90_u8_to_s16_bias127(src, dst, 8);
    int i0 = (int)src[0] - 127, q0 = (int)src[1] - 127;
    int i1 = (int)src[2] - 127, q1 = (int)src[3] - 127;
    int i2 = (int)src[4] - 127, q2 = (int)src[5] - 127;
    int i3 = (int)src[6] - 127, q3 = (int)src[7] - 127;
    int rr[8] = {i0, q0, -q1, i1, -i2, -q2, q3, -i3};
    int16_t ref_rot[8];
    for (int k = 0; k < 8; k++) {
        int v = rr[k];
        if (v > 127) {
            v = 127;
        }
        if (v < -128) {
            v = -128;
        }
        v <<= 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        ref_rot[k] = (int16_t)v;
    }
    if (!arrays_equal(dst, ref_rot, 8)) {
        fprintf(stderr, "SIMD rotate+widen: mismatch\n");
        return 1;
    }

    return 0;
}
