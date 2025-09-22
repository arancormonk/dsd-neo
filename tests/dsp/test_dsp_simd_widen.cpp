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

    // widen center 127 → s16
    widen_u8_to_s16_bias127(src, dst, 8);
    for (int i = 0; i < 8; i++) {
        ref[i] = (int16_t)src[i] - 127;
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
    int16_t i0 = (int16_t)src[0] - 127, q0 = (int16_t)src[1] - 127;
    int16_t i1 = (int16_t)src[2] - 127, q1 = (int16_t)src[3] - 127;
    int16_t i2 = (int16_t)src[4] - 127, q2 = (int16_t)src[5] - 127;
    int16_t i3 = (int16_t)src[6] - 127, q3 = (int16_t)src[7] - 127;
    int16_t ref_rot[8] = {i0, q0, (int16_t)(-q1), i1, (int16_t)(-i2), (int16_t)(-q2), q3, (int16_t)(-i3)};
    if (!arrays_equal(dst, ref_rot, 8)) {
        fprintf(stderr, "SIMD rotate+widen: mismatch\n");
        return 1;
    }

    return 0;
}
