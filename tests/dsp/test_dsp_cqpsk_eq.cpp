// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: CQPSK equalizer identity response and in-place processing. */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

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
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);
    // Ensure LMS is off and identity center tap is active
    st.lms_enable = 0;
    st.num_taps = 5;

    const int pairs = 16;
    int16_t buf[pairs * 2];
    int16_t ref[pairs * 2];
    for (int k = 0; k < pairs; k++) {
        // Simple QPSK-like pattern with varying amplitude to avoid trivial zero cases
        int16_t i = (int16_t)((k & 1) ? 7000 : -7000);
        int16_t q = (int16_t)((k & 2) ? 5000 : -5000);
        buf[2 * k + 0] = i;
        buf[2 * k + 1] = q;
        ref[2 * k + 0] = i;
        ref[2 * k + 1] = q;
    }

    cqpsk_eq_process_block(&st, buf, pairs * 2);
    if (!arrays_equal(buf, ref, pairs * 2)) {
        fprintf(stderr, "CQPSK_EQ identity: output differs from input\n");
        return 1;
    }
    return 0;
}
