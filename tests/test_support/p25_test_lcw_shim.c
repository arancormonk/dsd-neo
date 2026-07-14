// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test-only helper to invoke the LCW decoder with minimal state.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <stdint.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_test_lcw_shim.h"

void
p25_test_invoke_lcw(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq, long lastsrc,
                    long tuner_freq) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return;
    }

    opts->trunk_enable = 1;
    opts->p25_lcw_retune = (enable_retune != 0) ? 1 : 0;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_enc_calls = 0;
    if (tuner_freq > 0) {
        opts->audio_in_type = AUDIO_IN_RTL;
        opts->rtlsdr_center_freq = (uint32_t)tuner_freq;
    }

    state->p25_cc_freq = cc_freq;
    state->lastsrc = lastsrc;
    state->p25_iden_fdma[1].chan_type = 1;
    state->p25_iden_fdma[1].chan_spac = 100;
    state->p25_iden_fdma[1].base_freq = 851000000 / 5;
    state->p25_iden_fdma[1].trust = 2;
    state->p25_iden_fdma[1].populated = 1;
    state->p25_chan_tdma_explicit[1] = 1;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, state);

    uint8_t buf[72];
    DSD_MEMSET(buf, 0, sizeof(buf));
    int n = len;
    if (n > 72) {
        n = 72;
    }
    for (int i = 0; i < n; i++) {
        buf[i] = (uint8_t)(lcw_bits[i] & 1);
    }
    p25_lcw(opts, state, buf, 0);
    dsd_state_ext_free_all(state);
    free(opts);
    free(state);
}
