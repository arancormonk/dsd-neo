// SPDX-License-Identifier: GPL-3.0-or-later
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
        if (st.freq_q15 > 4096 || st.freq_q15 < -4096) {
            fprintf(stderr, "FLL clamp: freq exceeded clamp (%d)\n", st.freq_q15);
            return 1;
        }
        if (st.int_q15 > 4096 || st.int_q15 < -4096) {
            fprintf(stderr, "FLL clamp: integrator exceeded clamp (%d)\n", st.int_q15);
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

    // Test 7: magnitude preserved by pure rotation (energy invariant)
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

    // Test 8: phase accumulation wraps with negative frequency
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

    // Test 9: band-edge FLL integrates phase when error is zero
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        const int pairs = 50;
        int16_t iq[2 * pairs];
        memset(iq, 0, sizeof(iq)); // no error signal

        fll_state_t st;
        fll_init_state(&st);
        double cycles = 0.05; // cycles per sample
        double freq_rad = cycles * 2.0 * M_PI;
        st.be_freq = (float)freq_rad;
        st.freq_q15 = (int)lrint((freq_rad * 32768.0) / (2.0 * M_PI));

        fll_update_error_qpsk(&cfg, &st, iq, 2 * pairs, 5);

        int expected_q15 = (int)lrint((freq_rad * pairs) * (32768.0 / (2.0 * M_PI))) & 0x7FFF;
        int got = st.phase_q15;
        int diff = got - expected_q15;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff > 16) {
            fprintf(stderr, "BE-FLL phase integrate: got %d expected %d (diff=%d)\n", got, expected_q15, diff);
            return 1;
        }
    }

    // Test 10: band-edge FLL responds with opposite signs for +/- tones
    {
        const int sps = 5;
        const int pairs = 200;
        double r = 12000.0;
        double f_cyc = 0.18; // normalized cycles/sample within band-edge passband
        int16_t iq_pos[2 * pairs];
        int16_t iq_neg[2 * pairs];
        for (int k = 0; k < pairs; k++) {
            double th = 2.0 * M_PI * f_cyc * k;
            iq_pos[(size_t)(k << 1)] = (int16_t)lrint(r * cos(th));
            iq_pos[(size_t)(k << 1) + 1] = (int16_t)lrint(r * sin(th));
            iq_neg[(size_t)(k << 1)] = (int16_t)lrint(r * cos(-th));
            iq_neg[(size_t)(k << 1) + 1] = (int16_t)lrint(r * sin(-th));
        }
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        fll_state_t stp, stn;
        fll_init_state(&stp);
        fll_init_state(&stn);
        for (int i = 0; i < 2; i++) {
            fll_update_error_qpsk(&cfg, &stp, iq_pos, 2 * pairs, sps);
            fll_update_error_qpsk(&cfg, &stn, iq_neg, 2 * pairs, sps);
        }
        if (stp.freq_q15 == 0 || stn.freq_q15 == 0 || ((stp.freq_q15 > 0) == (stn.freq_q15 > 0))) {
            fprintf(stderr, "BE-FLL tone sign: expected opposite nonzero freq, got %d and %d\n", stp.freq_q15,
                    stn.freq_q15);
            return 1;
        }
    }

    // Test 11: band-edge FLL clamps to +/-2/sps
    {
        fll_config_t cfg = {0};
        cfg.enabled = 1;
        const int sps = 5;
        int16_t iq[4] = {0};
        fll_state_t st;
        fll_init_state(&st);
        st.be_freq = 100.0f; /* intentionally huge */
        fll_update_error_qpsk(&cfg, &st, iq, 4, sps);
        int clamp_q15 = (int)lrint((2.0 / (double)sps) * 32768.0);
        if (st.freq_q15 > clamp_q15 || st.freq_q15 < -clamp_q15) {
            fprintf(stderr, "BE-FLL clamp: freq %d outside +/- %d\n", st.freq_q15, clamp_q15);
            return 1;
        }
    }

    return 0;
}
