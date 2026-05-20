// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/p25p1_heuristics.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/runtime/config.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_next_dibit;

float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)have_sync;
    state->symbolc = g_next_dibit;
    switch (g_next_dibit & 3) {
        case 0: return 1.0f;
        case 1: return 3.0f;
        case 2: return -1.0f;
        case 3: return -3.0f;
        default: break;
    }
    return 0.0f;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 0;
}

void
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

void
dsd_sleep_ns(uint64_t ns) {
    (void)ns;
}

void
dsd_sleep_us(uint64_t us) {
    (void)us;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

uint8_t
dsd_fsk_symbol_reliability(float symbol, int levels) {
    (void)symbol;
    (void)levels;
    return 255;
}

int
estimate_symbol(int rf_mod, P25Heuristics* heuristics, int previous_dibit, int analog_value, int* dibit) {
    (void)rf_mod;
    (void)heuristics;
    (void)previous_dibit;
    (void)analog_value;
    (void)dibit;
    return 0;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_reliab_buf);
    free(state->dmr_soft_buf);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000, sizeof(int));
    state->dmr_reliab_buf = (uint8_t*)calloc(1000000, sizeof(uint8_t));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000, sizeof(dsd_dibit_soft_t));
    if (state->dibit_buf == NULL || state->dmr_payload_buf == NULL || state->dmr_reliab_buf == NULL
        || state->dmr_soft_buf == NULL) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    return 1;
}

static int
llr_matches_bit(int16_t llr, int bit) {
    return bit ? (llr > 0) : (llr < 0);
}

static int
test_symbol_bin_soft_matches_returned_dibit(int dibit) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        fprintf(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts.use_heuristics = 0;

    state.synctype = DSD_SYNC_P25P1_NEG;
    state.rf_mod = 0;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;
    state.lmid = -2.0f;
    state.umid = 2.0f;

    g_next_dibit = dibit;

    dsd_dibit_soft_t soft;
    memset(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    int rc = 0;
    if (got != dibit) {
        fprintf(stderr, "dibit %d: got returned dibit %d\n", dibit, got);
        rc = 1;
    }
    if (!llr_matches_bit(soft.llr[0], (dibit >> 1) & 1) || !llr_matches_bit(soft.llr[1], dibit & 1)) {
        fprintf(stderr, "dibit %d: soft signs do not match returned dibit (%d,%d)\n", dibit, soft.llr[0], soft.llr[1]);
        rc = 1;
    }
    if (soft.reliability != 255) {
        fprintf(stderr, "dibit %d: expected symbol-bin reliability 255, got %u\n", dibit, soft.reliability);
        rc = 1;
    }
    if (state.dmr_reliab_p[-1] != 255) {
        fprintf(stderr, "dibit %d: previous reliability buffer was not rebuilt\n", dibit);
        rc = 1;
    }

    const dsd_dibit_soft_t previous = state.dmr_soft_p[-1];
    if (previous.llr[0] != soft.llr[0] || previous.llr[1] != soft.llr[1] || previous.reliability != soft.reliability) {
        fprintf(stderr, "dibit %d: previous soft buffer does not match returned soft metric\n", dibit);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    for (int dibit = 0; dibit < 4; dibit++) {
        rc |= test_symbol_bin_soft_matches_returned_dibit(dibit);
    }
    return rc;
}
