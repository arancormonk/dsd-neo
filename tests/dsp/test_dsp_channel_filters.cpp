// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests for input channel filters in dsd_filters.c: DC preservation. */

#include <stdint.h>
#include <stdio.h>

extern "C" void init_rrc_filter_memory(void);
extern "C" short dmr_filter(short sample);
extern "C" short nxdn_filter(short sample);
extern "C" short dpmr_filter(short sample);
extern "C" short m17_filter(short sample);
extern "C" short p25_c4fm_filter(short sample);

static int
approx_eq(int a, int b, int tol) {
    int d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

static int
dc_pass_check(short (*f)(short), int dc, int warm, int tol) {
    int y = 0;
    for (int i = 0; i < warm; i++) {
        y = f((short)dc);
    }
    return approx_eq(y, dc, tol);
}

int
main(void) {
    init_rrc_filter_memory();
    const int dc = 1000;
    const int warm = 512; // exceed any filter length for steady-state
    // Allow small rounding tolerance
    if (!dc_pass_check(&dmr_filter, dc, warm, 8)) {
        fprintf(stderr, "DMR DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&nxdn_filter, dc, warm, 8)) {
        fprintf(stderr, "NXDN DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&dpmr_filter, dc, warm, 8)) {
        fprintf(stderr, "DPMR DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&m17_filter, dc, warm, 8)) {
        fprintf(stderr, "M17 DC fail\n");
        return 1;
    }
    if (!dc_pass_check(&p25_c4fm_filter, dc, warm, 8)) {
        fprintf(stderr, "P25 DC fail\n");
        return 1;
    }
    return 0;
}
