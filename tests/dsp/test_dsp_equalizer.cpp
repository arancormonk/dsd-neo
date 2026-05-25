// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdio>
#include <dsd-neo/dsp/equalizer.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_close(float got, float want, float tol, const char* label) {
    if (std::fabs(got - want) > tol) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", label, got, want);
        return 1;
    }
    return 0;
}

static unsigned int
lcg_next(unsigned int* s) {
    *s = (*s * 1664525U) + 1013904223U;
    return *s;
}

static float
mean_abs_modulus_error(const float* iq, int pairs, int start, float modulus) {
    double acc = 0.0;
    int n = 0;
    for (int i = start; i < pairs; i++) {
        const float I = iq[(size_t)i * 2U];
        const float Q = iq[(size_t)i * 2U + 1U];
        const float mag2 = I * I + Q * Q;
        acc += std::fabs((double)mag2 - (double)modulus);
        n++;
    }
    return (n > 0) ? (float)(acc / (double)n) : 0.0f;
}

static int
test_reset_initializes_center_spike(void) {
    dsd_cqpsk_cma_equalizer_state_t st;
    DSD_MEMSET(&st, 0x5a, sizeof(st));

    dsd_cqpsk_cma_equalizer_init(&st, 8);
    if (st.taps != 9) {
        DSD_FPRINTF(stderr, "init: even tap count should round up to 9, got %d\n", st.taps);
        return 1;
    }
    int rc = 0;
    for (int k = 0; k < st.taps; k++) {
        const float want = (k == (st.taps / 2)) ? 1.0f : 0.0f;
        rc |= expect_close(st.taps_r[k], want, 0.0f, "init taps_r");
        rc |= expect_close(st.taps_i[k], 0.0f, 0.0f, "init taps_i");
    }

    dsd_cqpsk_cma_equalizer_metrics_t m;
    DSD_MEMSET(&m, 0x5a, sizeof(m));
    dsd_cqpsk_cma_equalizer_get_metrics(&st, &m);
    rc |= expect_close(m.tap_energy, 1.0f, 0.0f, "init tap_energy");
    rc |= expect_close(m.center_tap_mag, 1.0f, 0.0f, "init center_tap_mag");
    rc |= expect_close(m.max_side_tap_mag, 0.0f, 0.0f, "init max_side_tap_mag");
    if (m.initialized != 1 || m.taps != 9 || m.symbols != 0U) {
        DSD_FPRINTF(stderr, "init metrics mismatch: initialized=%d taps=%d symbols=%u\n", m.initialized, m.taps,
                    m.symbols);
        return 1;
    }
    return rc;
}

static int
test_cma_reduces_constant_modulus_error_on_isi(void) {
    enum : unsigned short { kSymbols = 8192 };

    float clean[kSymbols * 2];
    float rx[kSymbols * 2];
    float eq[kSymbols * 2];

    unsigned int seed = 0x12345678U;
    for (int n = 0; n < kSymbols; n++) {
        unsigned int v = lcg_next(&seed) & 3U;
        float I = 0.0f;
        float Q = 0.0f;
        switch (v) {
            case 0: I = 1.0f; break;
            case 1: Q = 1.0f; break;
            case 2: I = -1.0f; break;
            default: Q = -1.0f; break;
        }
        clean[(size_t)n * 2U] = I;
        clean[(size_t)n * 2U + 1U] = Q;
    }

    const float echo_r = 0.46f;
    const float echo_i = 0.25f;
    const float norm = 1.0f / std::sqrt(1.0f + echo_r * echo_r + echo_i * echo_i);
    for (int n = 0; n < kSymbols; n++) {
        float I = clean[(size_t)n * 2U];
        float Q = clean[(size_t)n * 2U + 1U];
        if (n > 0) {
            const float pI = clean[(size_t)(n - 1) * 2U];
            const float pQ = clean[(size_t)(n - 1) * 2U + 1U];
            I += echo_r * pI - echo_i * pQ;
            Q += echo_r * pQ + echo_i * pI;
        }
        rx[(size_t)n * 2U] = I * norm;
        rx[(size_t)n * 2U + 1U] = Q * norm;
        eq[(size_t)n * 2U] = rx[(size_t)n * 2U];
        eq[(size_t)n * 2U + 1U] = rx[(size_t)n * 2U + 1U];
    }

    dsd_cqpsk_cma_equalizer_state_t st;
    dsd_cqpsk_cma_equalizer_init(&st, 9);
    dsd_cqpsk_cma_equalizer_apply(&st, eq, kSymbols * 2, 9, 0.0015f, 1.0f);

    const int start = kSymbols / 2;
    float raw_err = mean_abs_modulus_error(rx, kSymbols, start, 1.0f);
    float eq_err = mean_abs_modulus_error(eq, kSymbols, start, 1.0f);

    if (!(eq_err < raw_err * 0.85f)) {
        DSD_FPRINTF(stderr, "CMA did not reduce modulus error enough: raw=%.6f eq=%.6f\n", raw_err, eq_err);
        return 1;
    }
    if (st.symbols < 1000U) {
        DSD_FPRINTF(stderr, "CMA did not adapt enough symbols: %u\n", st.symbols);
        return 1;
    }

    dsd_cqpsk_cma_equalizer_metrics_t m;
    dsd_cqpsk_cma_equalizer_get_metrics(&st, &m);
    if (m.symbols != st.symbols || m.taps != st.taps || !(m.tap_energy > 0.1f) || !(m.max_side_tap_mag > 0.001f)) {
        DSD_FPRINTF(stderr, "CMA metrics mismatch: symbols=%u/%u taps=%d/%d energy=%.6f side=%.6f\n", m.symbols,
                    st.symbols, m.taps, st.taps, m.tap_energy, m.max_side_tap_mag);
        return 1;
    }

    return 0;
}

static int
test_cma_defaults_reduce_scaled_isi(void) {
    enum : unsigned short { kSymbols = 8192 };

    float clean[kSymbols * 2];
    float rx[kSymbols * 2];
    float eq[kSymbols * 2];
    const float amp = std::sqrt(DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS);

    unsigned int seed = 0x9abcdef0U;
    for (int n = 0; n < kSymbols; n++) {
        unsigned int v = lcg_next(&seed) & 3U;
        float I = 0.0f;
        float Q = 0.0f;
        switch (v) {
            case 0: I = amp; break;
            case 1: Q = amp; break;
            case 2: I = -amp; break;
            default: Q = -amp; break;
        }
        clean[(size_t)n * 2U] = I;
        clean[(size_t)n * 2U + 1U] = Q;
    }

    const float echo_r = 0.38f;
    const float echo_i = -0.22f;
    const float norm = 1.0f / std::sqrt(1.0f + echo_r * echo_r + echo_i * echo_i);
    for (int n = 0; n < kSymbols; n++) {
        float I = clean[(size_t)n * 2U];
        float Q = clean[(size_t)n * 2U + 1U];
        if (n > 0) {
            const float pI = clean[(size_t)(n - 1) * 2U];
            const float pQ = clean[(size_t)(n - 1) * 2U + 1U];
            I += echo_r * pI - echo_i * pQ;
            Q += echo_r * pQ + echo_i * pI;
        }
        rx[(size_t)n * 2U] = I * norm;
        rx[(size_t)n * 2U + 1U] = Q * norm;
        eq[(size_t)n * 2U] = rx[(size_t)n * 2U];
        eq[(size_t)n * 2U + 1U] = rx[(size_t)n * 2U + 1U];
    }

    dsd_cqpsk_cma_equalizer_state_t st;
    dsd_cqpsk_cma_equalizer_init(&st, DSD_CQPSK_CMA_EQ_DEFAULT_TAPS);
    dsd_cqpsk_cma_equalizer_apply(&st, eq, kSymbols * 2, DSD_CQPSK_CMA_EQ_DEFAULT_TAPS, DSD_CQPSK_CMA_EQ_DEFAULT_MU,
                                  DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS);

    const int start = kSymbols / 2;
    float raw_err = mean_abs_modulus_error(rx, kSymbols, start, DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS);
    float eq_err = mean_abs_modulus_error(eq, kSymbols, start, DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS);

    if (!(eq_err < raw_err * 0.88f)) {
        DSD_FPRINTF(stderr, "CMA defaults did not reduce scaled ISI enough: raw=%.6f eq=%.6f\n", raw_err, eq_err);
        return 1;
    }
    if (st.taps != DSD_CQPSK_CMA_EQ_DEFAULT_TAPS || st.symbols < 1000U) {
        DSD_FPRINTF(stderr, "CMA defaults state mismatch: taps=%d symbols=%u\n", st.taps, st.symbols);
        return 1;
    }

    return 0;
}

int
main(void) {
    int rc = 0;
    rc |= test_reset_initializes_center_spike();
    rc |= test_cma_reduces_constant_modulus_error_on_isi();
    rc |= test_cma_defaults_reduce_scaled_isi();
    return rc;
}
