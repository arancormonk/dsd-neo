// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for polar discriminator functions. */

#include <dsd-neo/dsp/polar_disc.h>
#include <stdio.h>
#include <stdlib.h>

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

int
main(void) {
    // Use moderate magnitude to avoid overflow in intermediate math
    const int A = 16384;   // 0.5 scale of int16 full scale
    int ar = A, aj = 0;    // a = 1 + j*0
    int br = 0, bj = A;    // b = 0 + j*1 => +90 degrees vs a
    int br2 = 0, bj2 = -A; // b2 = 0 - j*1 => -90 degrees vs a

    int q14_pi_2 = 1 << 13; // pi/2 in Q14 magnitude

    // Accurate double-based
    int d1 = polar_discriminant(ar, aj, br, bj);
    int d2 = polar_discriminant(ar, aj, br2, bj2);
    if (!approx_eq((d1 < 0 ? -d1 : d1), q14_pi_2, 32)) {
        fprintf(stderr, "polar_discriminant +90: |got|=%d want ~%d\n", d1 < 0 ? -d1 : d1, q14_pi_2);
        return 1;
    }
    if (!approx_eq((d2 < 0 ? -d2 : d2), q14_pi_2, 32)) {
        fprintf(stderr, "polar_discriminant -90: |got|=%d want ~%d\n", d2 < 0 ? -d2 : d2, q14_pi_2);
        return 1;
    }

    // Fast integer atan2
    int f1 = polar_disc_fast(ar, aj, br, bj);
    int f2 = polar_disc_fast(ar, aj, br2, bj2);
    if ((d1 > 0) != (f1 > 0)) {
        fprintf(stderr, "polar_disc_fast sign mismatch vs reference\n");
        return 1;
    }
    if (!approx_eq((f1 < 0 ? -f1 : f1), q14_pi_2, 128)) {
        fprintf(stderr, "polar_disc_fast +90: |got|=%d want ~%d\n", f1 < 0 ? -f1 : f1, q14_pi_2);
        return 1;
    }
    if ((d2 > 0) != (f2 > 0)) {
        fprintf(stderr, "polar_disc_fast sign mismatch vs reference (neg)\n");
        return 1;
    }
    if (!approx_eq((f2 < 0 ? -f2 : f2), q14_pi_2, 128)) {
        fprintf(stderr, "polar_disc_fast -90: |got|=%d want ~%d\n", f2 < 0 ? -f2 : f2, q14_pi_2);
        return 1;
    }

    // LUT-based (falls back to fast if allocation fails)
    (void)atan_lut_init();
    int l1 = polar_disc_lut(ar, aj, br, bj);
    int l2 = polar_disc_lut(ar, aj, br2, bj2);
    if ((f1 > 0) != (l1 > 0)) {
        fprintf(stderr, "polar_disc_lut sign mismatch vs fast\n");
        return 1;
    }
    if (!approx_eq((l1 < 0 ? -l1 : l1), q14_pi_2, 192)) {
        fprintf(stderr, "polar_disc_lut +90: |got|=%d want ~%d\n", l1 < 0 ? -l1 : l1, q14_pi_2);
        return 1;
    }
    if ((f2 > 0) != (l2 > 0)) {
        fprintf(stderr, "polar_disc_lut sign mismatch vs fast (neg)\n");
        return 1;
    }
    if (!approx_eq((l2 < 0 ? -l2 : l2), q14_pi_2, 192)) {
        fprintf(stderr, "polar_disc_lut -90: |got|=%d want ~%d\n", l2 < 0 ? -l2 : l2, q14_pi_2);
        return 1;
    }

    atan_lut_free();
    return 0;
}
