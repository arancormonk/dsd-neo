// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lightweight test-only helper to invoke LCW decoder with minimal state.
 */

#include <string.h>

#include <dsd-neo/core/dsd.h>

void
p25_test_invoke_lcw(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    opts.p25_trunk = 1;
    opts.p25_lcw_retune = (enable_retune != 0) ? 1 : 0;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_enc_calls = 0;

    state.p25_cc_freq = cc_freq;

    uint8_t buf[72];
    memset(buf, 0, sizeof(buf));
    int n = len;
    if (n > 72) {
        n = 72;
    }
    for (int i = 0; i < n; i++) {
        buf[i] = (uint8_t)(lcw_bits[i] & 1);
    }
    p25_lcw(&opts, &state, buf, 0);
}
