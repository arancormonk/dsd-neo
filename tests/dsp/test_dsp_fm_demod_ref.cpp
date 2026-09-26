// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: dsd_fm_demod (phase-diff path) returns constant for constant dphi, and its sign
   convention: a carrier above the tuned frequency (positive deviation) demodulates to positive
   output, which is what makes a DCS one in normal polarity read as a one (issue #523). */

#include <cmath>
#include <cstdlib>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

/*
 * The discriminator's sign. I = cos, Q = sin rotating counter-clockwise is a carrier above the
 * tuned frequency: positive deviation must read positive, and clockwise negative. Then a DCS
 * word sent as NRZ frequency deviation, a one as +600 Hz (normal polarity, runtime/
 * analog_tones.h), bit 0 first: the demodulated sign over each bit reads the word's bits back,
 * so D023N reads as D023N rather than as its complement, the signal of D047N.
 */
static int
test_sign_convention(demod_state* s) {
    const double fs = 48000.0;
    static float iq[(size_t)4096 * 2];
    for (int dir = -1; dir <= 1; dir += 2) {
        DSD_MEMSET(s, 0, sizeof(*s));
        const double dphi = (double)dir * 2.0 * 3.14159265358979323846 * 600.0 / fs;
        for (int k = 0; k < 256; k++) {
            iq[(size_t)(2 * k) + 0] = (float)(0.8 * cos(k * dphi));
            iq[(size_t)(2 * k) + 1] = (float)(0.8 * sin(k * dphi));
        }
        s->lowpassed = iq;
        s->lp_len = 256 * 2;
        dsd_fm_demod(s);
        for (int i = 1; i < s->result_len; i++) {
            if ((dir > 0 && !(s->result[i] > 0.0f)) || (dir < 0 && !(s->result[i] < 0.0f))) {
                DSD_FPRINTF(stderr, "FM demod sign: deviation %+d Hz read %f at %d\n", dir * 600, s->result[i], i);
                return 1;
            }
        }
    }

    /* D023N: 23 bits at 134.4 bit/s, about 357 samples per bit at 48 kHz. */
    const uint32_t word = dsd_dcs_word(023, 0);
    const int per_bit = 357;
    /* One block per bit; each keeps the previous one's last sample as history. */
    DSD_MEMSET(s, 0, sizeof(*s));
    double phase = 0.0;
    uint32_t read = 0U;
    for (int bit = 0; bit < 23; bit++) {
        const double dev = ((word >> bit) & 1U) ? 600.0 : -600.0;
        const double dphi = 2.0 * 3.14159265358979323846 * dev / fs;
        for (int k = 0; k < per_bit; k++) {
            iq[(size_t)(2 * k) + 0] = (float)(0.8 * cos(phase));
            iq[(size_t)(2 * k) + 1] = (float)(0.8 * sin(phase));
            phase += dphi;
        }
        s->lowpassed = iq;
        s->lp_len = per_bit * 2;
        dsd_fm_demod(s);
        /* The middle of the bit, clear of the step from the bit before. */
        double sum = 0.0;
        for (int i = per_bit / 4; i < (3 * per_bit) / 4; i++) {
            sum += (double)s->result[i];
        }
        if (sum > 0.0) {
            read |= 1U << bit;
        }
    }
    if (read != word) {
        DSD_FPRINTF(stderr, "FM demod DCS polarity: read 0x%06X, sent 0x%06X\n", (unsigned)read, (unsigned)word);
        return 1;
    }
    int code = -1;
    int inverted = -1;
    if (dsd_dcs_match(read, &code, &inverted) != 1 || code != 023 || inverted != 0) {
        DSD_FPRINTF(stderr, "FM demod DCS polarity: read as %03o%c\n", (unsigned)code, inverted ? 'I' : 'N');
        return 1;
    }
    return 0;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));

    // Build a complex tone that advances by constant phase per sample
    const int N = 256; // complex pairs
    static float iq[(size_t)N * 2];
    double Fs = 48000.0;
    double f_dev = 3000.0; // radians per second mapped to dphi = 2*pi*f/Fs
    double dphi = 2.0 * 3.14159265358979323846 * f_dev / Fs;
    double A = 0.8; // normalized amplitude
    for (int k = 0; k < N; k++) {
        double th = k * dphi;
        iq[(size_t)(2 * k) + 0] = (float)(A * cos(th));
        iq[(size_t)(2 * k) + 1] = (float)(A * sin(th));
    }

    s->lowpassed = iq;
    s->lp_len = N * 2;
    s->pre_r = 0;
    s->pre_j = 0;

    dsd_fm_demod(s);

    /* Expected steady-state phase delta based on first two samples.
     * With native float output, the demodulator returns raw radians (not Q14 scaled). */
    float r0 = iq[0], j0 = iq[1], r1 = iq[2], j1 = iq[3];
    double re = (double)r1 * (double)r0 + (double)j1 * (double)j0;
    double im = (double)j1 * (double)r0 - (double)r1 * (double)j0;
    float expect_rad = (float)atan2(im, re);
    if (s->result_len != N) {
        DSD_FPRINTF(stderr, "FM demod ref: result_len=%d want %d\n", s->result_len, N);
        free(s);
        return 1;
    }
    // First sample seeds history; steady-state starts at index 1
    if (fabsf(s->result[0]) > 1e-3f) {
        DSD_FPRINTF(stderr, "FM demod ref: result[0]=%f want 0\n", s->result[0]);
        free(s);
        return 1;
    }
    for (int i = 1; i < s->result_len; i++) {
        float v = s->result[i];
        float d = fabsf(v - expect_rad);
        if (d > 0.01f) { // allow small tolerance for native float output
            DSD_FPRINTF(stderr, "FM demod ref: result[%d]=%f expect~%f\n", i, v, expect_rad);
            free(s);
            return 1;
        }
    }

    DSD_MEMSET(s, 0, sizeof(*s));
    const double small_dphi = 0.08;
    for (int k = 0; k < N; k++) {
        double th = k * small_dphi;
        iq[(size_t)(2 * k) + 0] = (float)(A * cos(th));
        iq[(size_t)(2 * k) + 1] = (float)(A * sin(th));
    }

    s->lowpassed = iq;
    s->lp_len = N * 2;
    dsd_fm_demod(s);

    r0 = iq[0];
    j0 = iq[1];
    r1 = iq[2];
    j1 = iq[3];
    re = (double)r1 * (double)r0 + (double)j1 * (double)j0;
    im = (double)j1 * (double)r0 - (double)r1 * (double)j0;
    expect_rad = (float)atan2(im, re);
    for (int i = 1; i < s->result_len; i++) {
        float v = s->result[i];
        float d = fabsf(v - expect_rad);
        if (d > 1e-4f) {
            DSD_FPRINTF(stderr, "FM demod small-angle: result[%d]=%f expect~%f\n", i, v, expect_rad);
            free(s);
            return 1;
        }
    }

    if (test_sign_convention(s) != 0) {
        free(s);
        return 1;
    }
    free(s);
    return 0;
}
