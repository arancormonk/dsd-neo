// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * SPS-derived defaults: taps and sym_stride selection.
 */

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
check_case(int sps, int expect_taps, int expect_sym_stride) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 0;
    }
    memset(s, 0, sizeof(*s));
    s->ted_sps = sps;
    cqpsk_init(s);
    int ok = 1;
    if (s->cqpsk_eq.num_taps != expect_taps) {
        fprintf(stderr, "SPS=%d taps=%d expect=%d\n", sps, s->cqpsk_eq.num_taps, expect_taps);
        ok = 0;
    }
    if (s->cqpsk_eq.sym_stride != expect_sym_stride) {
        fprintf(stderr, "SPS=%d sym_stride=%d expect=%d\n", sps, s->cqpsk_eq.sym_stride, expect_sym_stride);
        ok = 0;
    }
    free(s);
    return ok;
}

int
main(void) {
    /* SPS in-range: 6 -> 5 taps, 10 -> 7 taps; sym_stride = sps */
    if (!check_case(6, 5, 6)) {
        return 1;
    }
    if (!check_case(10, 7, 10)) {
        return 1;
    }

    /* SPS out of range: leave defaults (taps=5 from eq init, sym_stride=4 default) */
    if (!check_case(0, 5, 4)) {
        return 1;
    }
    if (!check_case(32, 5, 4)) {
        return 1;
    }

    return 0;
}
