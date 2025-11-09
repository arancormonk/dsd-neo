// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit tests for FLL mix/update helpers. */

#include <dsd-neo/dsp/fll.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
arrays_close(const int16_t* a, const int16_t* b, int n, int tol) {
    for (int i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) {
            d = -d;
        }
        if (d > tol) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    // Test 1: mix with freq=0 should be a no-op
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 0; // no rotation

        int16_t x[20];
        for (int i = 0; i < 20; i++) {
            x[i] = (int16_t)(i * 17 - 100);
        }
        int16_t y[20];
        memcpy(y, x, sizeof(x));

        fll_mix_and_update(&cfg, &st, x, 20);
        if (!arrays_close(x, y, 20, 1)) {
            fprintf(stderr, "FLL mix (fast): freq=0 deviated >1 LSB\n");
            return 1;
        }

        /* Re-run once more to ensure determinism */
        memcpy(x, y, sizeof(x));
        fll_mix_and_update(&cfg, &st, x, 20);
        if (!arrays_close(x, y, 20, 1)) {
            fprintf(stderr, "FLL mix: freq=0 deviated >1 LSB\n");
            return 1;
        }
    }

    // Test 1b: phase accumulation wraps correctly on mix
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;

        fll_state_t st;
        fll_init_state(&st);

        // Choose a small positive freq, run enough complex samples to wrap
        st.freq_q15 = 1234;     // Q15 increment per complex sample
        const int pairs = 1000; // complex samples
        int16_t x[2 * pairs];
        memset(x, 0, sizeof(x)); // content doesn't matter for phase advance

        fll_mix_and_update(&cfg, &st, x, 2 * pairs);

        int expected = (pairs * st.freq_q15) & 0x7FFF; // started at 0
        if (st.phase_q15 != expected) {
            fprintf(stderr, "FLL mix: phase wrap mismatch, got %d expected %d\n", st.phase_q15, expected);
            return 1;
        }
    }

    // Test 1c: disabled config leaves buffers/state unchanged
    {
        fll_config_t cfg = {0};
        cfg.enabled = 0;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 500;
        st.phase_q15 = 300;

        int16_t x[8] = {10, 20, 30, 40, 50, 60, 70, 80};
        int16_t y[8];
        memcpy(y, x, sizeof(x));

        fll_mix_and_update(&cfg, &st, x, 8);
        if (!arrays_close(x, y, 8, 0) || st.phase_q15 != 300) {
            fprintf(stderr, "FLL mix: disabled mode altered output/state\n");
            return 1;
        }

        // update_error should also not change freq/int when disabled
        st.int_q15 = 123;
        fll_update_error(&cfg, &st, x, 8);
        if (!(st.freq_q15 == 500 && st.int_q15 == 123)) {
            fprintf(stderr, "FLL update: disabled mode altered control state\n");
            return 1;
        }
    }

    // Test 2: update_error should move freq in sign of observed CFO
    {
        const int N = 100; // even
        int16_t iq[2 * N];
        double r = 12000.0;                   // arbitrary radius within int16 range
        double dtheta = (2.0 * M_PI) / 200.0; // small positive rotation per complex sample
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            int16_t i = (int16_t)lrint(r * cos(th));
            int16_t q = (int16_t)lrint(r * sin(th));
            iq[2 * k + 0] = i;
            iq[2 * k + 1] = q;
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 1200; // small gains
        cfg.beta_q15 = 800;
        cfg.deadband_q14 = 0;     // respond to small errors
        cfg.slew_max_q15 = 32767; // effectively unlimited for test

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL update: expected positive freq correction, got %d\n", st.freq_q15);
            return 1;
        }

        // Negative rotation
        for (int k = 0; k < N; k++) {
            double th = -k * dtheta;
            int16_t i = (int16_t)lrint(r * cos(th));
            int16_t q = (int16_t)lrint(r * sin(th));
            iq[2 * k + 0] = i;
            iq[2 * k + 1] = q;
        }
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 >= 0) {
            fprintf(stderr, "FLL update: expected negative freq correction, got %d\n", st.freq_q15);
            return 1;
        }
    }

    // Test 2b: update_error early return when N=2 and prev=0, integrator unchanged
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 5000;
        cfg.beta_q15 = 5000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 100;

        fll_state_t st;
        fll_init_state(&st);
        st.int_q15 = 777;

        int16_t one[2] = {1234, -5678};
        fll_update_error(&cfg, &st, one, 2);
        if (!(st.freq_q15 == 0 && st.int_q15 == 777)) {
            fprintf(stderr, "FLL small-N adj: unexpected change on first call\n");
            return 1;
        }
        if (!(st.prev_r == 1234 && st.prev_j == -5678)) {
            fprintf(stderr, "FLL small-N adj: prev sample not latched\n");
            return 1;
        }
    }

    // Test 3: deadband holds control, integrator not advanced (leak only)
    {
        // Build constant sample stream -> zero phase difference (err=0)
        int16_t iq[16];
        for (int i = 0; i < 16; i += 2) {
            iq[i + 0] = 10000;
            iq[i + 1] = 0;
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 10000;
        cfg.beta_q15 = 10000;
        cfg.deadband_q14 = 10; // any small nonzero value
        cfg.slew_max_q15 = 32767;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 777;
        st.int_q15 = 1000; // within clamp range

        fll_update_error(&cfg, &st, iq, 16);
        if (st.freq_q15 != 777) {
            fprintf(stderr, "FLL deadband: freq changed unexpectedly\n");
            return 1;
        }
        // With clamp=2048 and leak >>12, value may not change due to quantization
        if (st.int_q15 != 1000) {
            fprintf(stderr, "FLL deadband: integrator changed unexpectedly (%d)\n", st.int_q15);
            return 1;
        }
    }

    // Test 4: slew limiting constrains per-update delta frequency
    {
        const int N = 64;
        int16_t iq[2 * N];
        double r = 15000.0;
        double dtheta = (2.0 * M_PI) / 20.0; // large rotation to drive controller
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 20000;
        cfg.beta_q15 = 20000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 25; // tight slew per update

        fll_state_t st;
        fll_init_state(&st);

        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 != 25) {
            fprintf(stderr, "FLL slew: first step %d, want 25\n", st.freq_q15);
            return 1;
        }
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 != 50) {
            fprintf(stderr, "FLL slew: second step %d, want 50\n", st.freq_q15);
            return 1;
        }
    }

    // Test 5: clamp bounds integrator and absolute frequency
    {
        const int N = 64;
        int16_t iq[2 * N];
        double r = 16000.0;
        double dtheta = (2.0 * M_PI) / 8.0; // very large
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 30000;
        cfg.beta_q15 = 30000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 32767; // no slew limit, rely on absolute clamp

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 > 2048) {
            fprintf(stderr, "FLL clamp: freq exceeded 2048 (%d)\n", st.freq_q15);
            return 1;
        }
        if (st.int_q15 > 2048) {
            fprintf(stderr, "FLL clamp: integrator exceeded 2048 (%d)\n", st.int_q15);
            return 1;
        }
    }

    // Test 6: small-N behavior uses prev sample across calls
    {
        // First call: only one complex sample -> no update
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 12000;
        cfg.beta_q15 = 8000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 32767;

        fll_state_t st;
        fll_init_state(&st);

        int16_t b1[2] = {16000, 0};
        fll_update_error(&cfg, &st, b1, 2);
        if (!(st.freq_q15 == 0 && st.prev_r == 16000 && st.prev_j == 0)) {
            fprintf(stderr, "FLL small-N: first call state wrong\n");
            return 1;
        }

        // Second call: one more sample with positive phase -> expect freq > 0
        int16_t b2[2] = {0, 16000}; // +90 deg relative to previous
        fll_update_error(&cfg, &st, b2, 2);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL small-N: expected positive update after carry-over\n");
            return 1;
        }
    }

    // Test 7: QPSK symbol-spaced update tracks CFO sign with transitions
    {
        const int sps = 4;   // samples per symbol (complex)
        const int syms = 16; // number of symbols
        const int Npairs = sps * syms;
        int16_t iq[2 * Npairs];
        double r = 14000.0;
        double dtheta = (2.0 * M_PI) / 180.0; // modest CFO per sample
        // Deterministic QPSK phase pattern
        // Balanced sequence to keep average symbol-phase difference near zero
        double qpsk_ph[4] = {0.0, M_PI_2, 0.0, -M_PI_2};
        for (int k = 0; k < Npairs; k++) {
            int m = k / sps;
            double sym = qpsk_ph[m % 4];
            double th = k * dtheta + sym;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 15000;
        cfg.beta_q15 = 12000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 32767;

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL QPSK: expected positive freq for +CFO\n");
            return 1;
        }

        // Negative CFO
        for (int k = 0; k < Npairs; k++) {
            int m = k / sps;
            double sym = qpsk_ph[m % 4];
            double th = -k * dtheta + sym;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (st.freq_q15 >= 0) {
            fprintf(stderr, "FLL QPSK: expected negative freq for -CFO\n");
            return 1;
        }
    }

    // Test 8: QPSK fallback (sps<2) behaves like adjacent-sample update in sign
    {
        const int N = 80;
        int16_t iq[2 * N];
        double r = 12000.0;
        double dtheta = (2.0 * M_PI) / 120.0;
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 12000;
        cfg.beta_q15 = 10000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 32767;

        fll_state_t st1, st2;
        fll_init_state(&st1);
        fll_init_state(&st2);
        fll_update_error(&cfg, &st1, iq, 2 * N);
        fll_update_error_qpsk(&cfg, &st2, iq, 2 * N, 1); // fallback path
        if (!((st1.freq_q15 > 0 && st2.freq_q15 > 0) || (st1.freq_q15 < 0 && st2.freq_q15 < 0))) {
            fprintf(stderr, "FLL QPSK fallback: sign mismatch adj vs fallback\n");
            return 1;
        }
    }

    // Test 9: magnitude preserved by pure rotation (energy invariant)
    {
        const int N = 64;
        int16_t iq[2 * N];
        double r = 17000.0;
        double dtheta = (2.0 * M_PI) / 64.0;
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 2000; // some nonzero rotation per complex sample

        // Compute energy before
        long long e0 = 0, e1 = 0;
        for (int i = 0; i < 2 * N; i++) {
            long v = iq[i];
            e0 += v * v;
        }
        fll_mix_and_update(&cfg, &st, iq, 2 * N);
        for (int i = 0; i < 2 * N; i++) {
            long v = iq[i];
            e1 += v * v;
        }
        // Allow small numeric drift due to rounding (<0.2%)
        long long diff = (e0 > e1) ? (e0 - e1) : (e1 - e0);
        if (diff > (e0 / 500)) {
            fprintf(stderr, "FLL mix: energy changed too much (|d|=%lld)\n", diff);
            return 1;
        }
    }

    // Test 10: QPSK early return when N<4 leaves state unchanged
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 10000;
        cfg.beta_q15 = 10000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 200;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = 42;
        st.int_q15 = 314;
        int16_t one[2] = {2000, 0};
        fll_update_error_qpsk(&cfg, &st, one, 2, 4);
        if (!(st.freq_q15 == 42 && st.int_q15 == 314)) {
            fprintf(stderr, "FLL QPSK small-N: state changed unexpectedly\n");
            return 1;
        }
    }

    // Test 11: QPSK sps too large for window (count==0) leaves state unchanged
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 10000;
        cfg.beta_q15 = 10000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 300;

        const int Npairs = 10;
        int16_t iq[2 * Npairs];
        for (int i = 0; i < 2 * Npairs; i++) {
            iq[i] = (i & 1) ? 1000 : 5000;
        }

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = -21;
        st.int_q15 = -123;
        int sps = 16; // stride_elems = 32 > N=20 elements
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (!(st.freq_q15 == -21 && st.int_q15 == -123)) {
            fprintf(stderr, "FLL QPSK sps-too-large: state changed unexpectedly\n");
            return 1;
        }
    }

    // Test 12: AWGN robustness for adjacent-sample discriminator (sign only)
    {
        const int N = 240; // complex samples
        int16_t iq[2 * N];
        double r = 12000.0;
        double dtheta = (2.0 * M_PI) / 60.0; // moderate CFO per sample
        int noise = 3000;                    // +/-3000 uniform noise on each axis
        srand(1);
        for (int k = 0; k < N; k++) {
            double th = k * dtheta;
            int ni = (rand() % (2 * noise)) - noise;
            int nq = (rand() % (2 * noise)) - noise;
            long vi = lrint(r * cos(th)) + ni;
            long vq = lrint(r * sin(th)) + nq;
            if (vi > 32767) {
                vi = 32767;
            }
            if (vi < -32768) {
                vi = -32768;
            }
            if (vq > 32767) {
                vq = 32767;
            }
            if (vq < -32768) {
                vq = -32768;
            }
            iq[2 * k + 0] = (int16_t)vi;
            iq[2 * k + 1] = (int16_t)vq;
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 8000;
        cfg.beta_q15 = 6000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 500;

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error(&cfg, &st, iq, 2 * N);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL AWGN adj: expected positive freq\n");
            return 1;
        }
        if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
            fprintf(stderr, "FLL AWGN adj: freq out of clamp (%d)\n", st.freq_q15);
            return 1;
        }
    }

    // Test 13: AWGN robustness for QPSK symbol-spaced update (sign only)
    {
        const int sps = 4;
        const int syms = 40;
        const int Npairs = sps * syms;
        int16_t iq[2 * Npairs];
        double r = 11000.0;
        double dtheta = (2.0 * M_PI) / 90.0; // modest CFO per sample
        int noise = 2500;
        srand(2);
        double qpsk_ph[4] = {0.0, M_PI_2, 0.0, -M_PI_2};
        for (int k = 0; k < Npairs; k++) {
            int m = k / sps;
            double sym = qpsk_ph[m % 4];
            double th = k * dtheta + sym;
            int ni = (rand() % (2 * noise)) - noise;
            int nq = (rand() % (2 * noise)) - noise;
            long vi = lrint(r * cos(th)) + ni;
            long vq = lrint(r * sin(th)) + nq;
            if (vi > 32767) {
                vi = 32767;
            }
            if (vi < -32768) {
                vi = -32768;
            }
            if (vq > 32767) {
                vq = 32767;
            }
            if (vq < -32768) {
                vq = -32768;
            }
            iq[2 * k + 0] = (int16_t)vi;
            iq[2 * k + 1] = (int16_t)vq;
        }

        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 8000;
        cfg.beta_q15 = 6000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 500;

        fll_state_t st;
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL AWGN QPSK: expected positive freq\n");
            return 1;
        }
        if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
            fprintf(stderr, "FLL AWGN QPSK: freq out of clamp (%d)\n", st.freq_q15);
            return 1;
        }
    }

    // Test 14: phase accumulation wraps with negative frequency
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;

        fll_state_t st;
        fll_init_state(&st);
        st.freq_q15 = -1234;
        const int pairs = 1000;
        int16_t x[2 * pairs];
        memset(x, 0, sizeof(x));
        fll_mix_and_update(&cfg, &st, x, 2 * pairs);
        int expected = ((-1234 * pairs) & 0x7FFF);
        if (st.phase_q15 != expected) {
            fprintf(stderr, "FLL mix neg: phase wrap mismatch, got %d expected %d\n", st.phase_q15, expected);
            return 1;
        }
    }

    // Test 15: QPSK update with wrong sps (3) still tracks CFO sign
    {
        const int sps_gen = 4;   // actual samples per symbol in the waveform
        const int sps_wrong = 3; // intentionally wrong for the estimator
        const int syms = 64;
        const int Npairs = sps_gen * syms;
        int16_t iq[2 * Npairs];
        double r = 12000.0;
        double dtheta = (2.0 * M_PI) / 120.0; // modest CFO per sample
        // Balanced QPSK sequence to keep average symbol-induced phase ~0
        double qpsk_ph[4] = {0.0, M_PI_2, 0.0, -M_PI_2};

        // Positive CFO
        for (int k = 0; k < Npairs; k++) {
            int m = k / sps_gen;
            double sym = qpsk_ph[m % 4];
            double th = k * dtheta + sym;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 10000;
        cfg.beta_q15 = 8000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 500;
        fll_state_t st;
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps_wrong);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL QPSK wrong-sps: expected positive freq for +CFO\n");
            return 1;
        }

        // Negative CFO
        for (int k = 0; k < Npairs; k++) {
            int m = k / sps_gen;
            double sym = qpsk_ph[m % 4];
            double th = -k * dtheta + sym;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps_wrong);
        if (st.freq_q15 >= 0) {
            fprintf(stderr, "FLL QPSK wrong-sps: expected negative freq for -CFO\n");
            return 1;
        }
    }

    // Test 16: QPSK minimal-count path (exactly one measurement) updates sign correctly
    {
        const int sps = 2; // stride_elems=4
        // Need N elements >= 6 (3 complex samples) to get one measurement
        const int Npairs = 3;
        int16_t iq[2 * Npairs];
        double r = 13000.0;
        double dtheta = (2.0 * M_PI) / 200.0; // small CFO per sample
        // Constant symbol phase (no transitions) for clarity
        for (int k = 0; k < Npairs; k++) {
            double th = k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        cfg.alpha_q15 = 12000;
        cfg.beta_q15 = 10000;
        cfg.deadband_q14 = 0;
        cfg.slew_max_q15 = 500;
        fll_state_t st;
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (st.freq_q15 <= 0) {
            fprintf(stderr, "FLL QPSK minimal: expected positive freq for +CFO\n");
            return 1;
        }
        // Negative CFO case
        for (int k = 0; k < Npairs; k++) {
            double th = -k * dtheta;
            iq[2 * k + 0] = (int16_t)lrint(r * cos(th));
            iq[2 * k + 1] = (int16_t)lrint(r * sin(th));
        }
        fll_init_state(&st);
        fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
        if (st.freq_q15 >= 0) {
            fprintf(stderr, "FLL QPSK minimal: expected negative freq for -CFO\n");
            return 1;
        }
    }

    // Test 17: AWGN multi-seed, multi-SNR for adjacent and QPSK (sign and clamp)
    {
        const int seeds[] = {1, 2, 3, 12345};
        const int nseeds = (int)(sizeof(seeds) / sizeof(seeds[0]));
        const int noises[] = {1000, 3000};
        const int nnoises = (int)(sizeof(noises) / sizeof(noises[0]));

        // Adjacent-sample discriminator
        for (int ni = 0; ni < nnoises; ni++) {
            int noise = noises[ni];
            for (int si = 0; si < nseeds; si++) {
                int seed = seeds[si];
                const int N = 480; // complex samples
                int16_t iq[2 * N];
                double r = 12000.0;
                double dtheta = (2.0 * M_PI) / 80.0; // moderate CFO per sample
                srand(seed);
                for (int k = 0; k < N; k++) {
                    double th = k * dtheta;
                    int niu = (rand() % (2 * noise)) - noise;
                    int nqu = (rand() % (2 * noise)) - noise;
                    long vi = lrint(r * cos(th)) + niu;
                    long vq = lrint(r * sin(th)) + nqu;
                    if (vi > 32767) {
                        vi = 32767;
                    }
                    if (vi < -32768) {
                        vi = -32768;
                    }
                    if (vq > 32767) {
                        vq = 32767;
                    }
                    if (vq < -32768) {
                        vq = -32768;
                    }
                    iq[2 * k + 0] = (int16_t)vi;
                    iq[2 * k + 1] = (int16_t)vq;
                }
                fll_config_t cfg = {0};
                cfg.enabled = 1;
                cfg.alpha_q15 = 8000;
                cfg.beta_q15 = 6000;
                cfg.deadband_q14 = 0;
                cfg.slew_max_q15 = 500;
                fll_state_t st;
                fll_init_state(&st);
                fll_update_error(&cfg, &st, iq, 2 * N);
                if (st.freq_q15 <= 0) {
                    fprintf(stderr, "FLL AWGN adj(se=%d,n=%d): want positive freq, got %d\n", seed, noise, st.freq_q15);
                    return 1;
                }
                if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
                    fprintf(stderr, "FLL AWGN adj(se=%d,n=%d): out of clamp (%d)\n", seed, noise, st.freq_q15);
                    return 1;
                }

                // Negative CFO
                srand(seed);
                for (int k = 0; k < N; k++) {
                    double th = -k * dtheta;
                    int niu = (rand() % (2 * noise)) - noise;
                    int nqu = (rand() % (2 * noise)) - noise;
                    long vi = lrint(r * cos(th)) + niu;
                    long vq = lrint(r * sin(th)) + nqu;
                    if (vi > 32767) {
                        vi = 32767;
                    }
                    if (vi < -32768) {
                        vi = -32768;
                    }
                    if (vq > 32767) {
                        vq = 32767;
                    }
                    if (vq < -32768) {
                        vq = -32768;
                    }
                    iq[2 * k + 0] = (int16_t)vi;
                    iq[2 * k + 1] = (int16_t)vq;
                }
                fll_init_state(&st);
                fll_update_error(&cfg, &st, iq, 2 * N);
                if (st.freq_q15 >= 0) {
                    fprintf(stderr, "FLL AWGN adj-(se=%d,n=%d): want negative freq, got %d\n", seed, noise,
                            st.freq_q15);
                    return 1;
                }
                if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
                    fprintf(stderr, "FLL AWGN adj-(se=%d,n=%d): out of clamp (%d)\n", seed, noise, st.freq_q15);
                    return 1;
                }
            }
        }

        // QPSK symbol-spaced discriminator
        for (int ni = 0; ni < nnoises; ni++) {
            int noise = (ni == 0) ? 1000 : 2500; // keep QPSK noise a bit lower
            for (int si = 0; si < nseeds; si++) {
                int seed = seeds[si];
                const int sps = 4;
                const int syms = 60;
                const int Npairs = sps * syms;
                int16_t iq[2 * Npairs];
                double r = 11000.0;
                double dtheta = (2.0 * M_PI) / 90.0;
                double qpsk_ph[4] = {0.0, M_PI_2, 0.0, -M_PI_2};

                // Positive CFO
                srand(seed);
                for (int k = 0; k < Npairs; k++) {
                    int m = k / sps;
                    double sym = qpsk_ph[m % 4];
                    double th = k * dtheta + sym;
                    int niu = (rand() % (2 * noise)) - noise;
                    int nqu = (rand() % (2 * noise)) - noise;
                    long vi = lrint(r * cos(th)) + niu;
                    long vq = lrint(r * sin(th)) + nqu;
                    if (vi > 32767) {
                        vi = 32767;
                    }
                    if (vi < -32768) {
                        vi = -32768;
                    }
                    if (vq > 32767) {
                        vq = 32767;
                    }
                    if (vq < -32768) {
                        vq = -32768;
                    }
                    iq[2 * k + 0] = (int16_t)vi;
                    iq[2 * k + 1] = (int16_t)vq;
                }
                fll_config_t cfg = {0};
                cfg.enabled = 1;
                cfg.alpha_q15 = 8000;
                cfg.beta_q15 = 6000;
                cfg.deadband_q14 = 0;
                cfg.slew_max_q15 = 500;
                fll_state_t st;
                fll_init_state(&st);
                fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
                if (st.freq_q15 <= 0) {
                    fprintf(stderr, "FLL AWGN QPSK(se=%d,n=%d): want positive, got %d\n", seed, noise, st.freq_q15);
                    return 1;
                }
                if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
                    fprintf(stderr, "FLL AWGN QPSK(se=%d,n=%d): out of clamp (%d)\n", seed, noise, st.freq_q15);
                    return 1;
                }

                // Negative CFO
                srand(seed);
                for (int k = 0; k < Npairs; k++) {
                    int m = k / sps;
                    double sym = qpsk_ph[m % 4];
                    double th = -k * dtheta + sym;
                    int niu = (rand() % (2 * noise)) - noise;
                    int nqu = (rand() % (2 * noise)) - noise;
                    long vi = lrint(r * cos(th)) + niu;
                    long vq = lrint(r * sin(th)) + nqu;
                    if (vi > 32767) {
                        vi = 32767;
                    }
                    if (vi < -32768) {
                        vi = -32768;
                    }
                    if (vq > 32767) {
                        vq = 32767;
                    }
                    if (vq < -32768) {
                        vq = -32768;
                    }
                    iq[2 * k + 0] = (int16_t)vi;
                    iq[2 * k + 1] = (int16_t)vq;
                }
                fll_init_state(&st);
                fll_update_error_qpsk(&cfg, &st, iq, 2 * Npairs, sps);
                if (st.freq_q15 >= 0) {
                    fprintf(stderr, "FLL AWGN QPSK-(se=%d,n=%d): want negative, got %d\n", seed, noise, st.freq_q15);
                    return 1;
                }
                if (st.freq_q15 < -2048 || st.freq_q15 > 2048) {
                    fprintf(stderr, "FLL AWGN QPSK-(se=%d,n=%d): out of clamp (%d)\n", seed, noise, st.freq_q15);
                    return 1;
                }
            }
        }
    }

    return 0;
}
