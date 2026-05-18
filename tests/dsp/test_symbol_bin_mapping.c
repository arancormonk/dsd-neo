// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/symbol_levels.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int
expect_float_eq(const char* label, float actual, float expected) {
    const float eps = 1e-6f;
    if (fabsf(actual - expected) > eps) {
        fprintf(stderr, "FAIL: %s: got %.6f expected %.6f\n", label, actual, expected);
        return 0;
    }
    return 1;
}

static int
expect_u8_ge(const char* label, uint8_t actual, uint8_t minimum) {
    if (actual < minimum) {
        fprintf(stderr, "FAIL: %s: got %u expected >= %u\n", label, actual, minimum);
        return 0;
    }
    return 1;
}

static int
expect_u8_le(const char* label, uint8_t actual, uint8_t maximum) {
    if (actual > maximum) {
        fprintf(stderr, "FAIL: %s: got %u expected <= %u\n", label, actual, maximum);
        return 0;
    }
    return 1;
}

static int
expect_int_eq(const char* label, int actual, int expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: got %d expected %d\n", label, actual, expected);
        return 0;
    }
    return 1;
}

int
main(void) {
    int ok = 1;
    ok &= expect_float_eq("dibit 0 -> +1", dsd_symbol_level_from_dibit(0), 1.0f);
    ok &= expect_float_eq("dibit 1 -> +3", dsd_symbol_level_from_dibit(1), 3.0f);
    ok &= expect_float_eq("dibit 2 -> -1", dsd_symbol_level_from_dibit(2), -1.0f);
    ok &= expect_float_eq("dibit 3 -> -3", dsd_symbol_level_from_dibit(3), -3.0f);
    ok &= expect_u8_ge("4-level +1 reliability", dsd_fsk_symbol_reliability(1.0f, 4), 250);
    ok &= expect_u8_ge("4-level +3 reliability", dsd_fsk_symbol_reliability(3.0f, 4), 250);
    ok &= expect_u8_ge("4-level -1 reliability", dsd_fsk_symbol_reliability(-1.0f, 4), 250);
    ok &= expect_u8_ge("4-level -3 reliability", dsd_fsk_symbol_reliability(-3.0f, 4), 250);
    ok &= expect_u8_le("4-level center boundary reliability", dsd_fsk_symbol_reliability(0.0f, 4), 5);
    ok &= expect_u8_le("4-level upper boundary reliability", dsd_fsk_symbol_reliability(2.0f, 4), 5);
    ok &= expect_u8_le("4-level lower boundary reliability", dsd_fsk_symbol_reliability(-2.0f, 4), 5);
    ok &= expect_u8_ge("2-level +1 reliability", dsd_fsk_symbol_reliability(1.0f, 2), 250);
    ok &= expect_u8_ge("2-level -1 reliability", dsd_fsk_symbol_reliability(-1.0f, 2), 250);
    ok &= expect_u8_le("2-level center boundary reliability", dsd_fsk_symbol_reliability(0.0f, 2), 5);
    dsd_fsk_soft_symbol_metrics clipped4 = dsd_fsk_soft_symbol_metrics_from_symbol(4.0f, 4);
    dsd_fsk_soft_symbol_metrics clipped2 = dsd_fsk_soft_symbol_metrics_from_symbol(-2.0f, 2);
    ok &= expect_int_eq("4-level clipped", clipped4.clipped, 1);
    ok &= expect_int_eq("2-level clipped", clipped2.clipped, 1);
    return ok ? 0 : 1;
}
