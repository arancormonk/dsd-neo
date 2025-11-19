// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Bit-level CQPSK → legacy dibit integration test.
 *
 * This test exercises the numeric path:
 *   synthetic scalar I symbols -> cqpsk_process_block (identity EQ) -> digitize()
 *
 * It verifies that, for ideal four-level symbols corresponding to the
 * P25 Phase 2 (+) path (synctype=35, rf_mod=1), the recovered dibits
 * match the intended sequence. This guards the wiring between the
 * CQPSK I-channel stream and the legacy slicer.
 */

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

#include <stdio.h>
#include <string.h>

/* External digitizer from src/core/frames/dsd_dibit.c (compiled into this test). */
extern "C" int digitize(dsd_opts* opts, dsd_state* state, int symbol);

/* Stubs for helpers referenced by dsd_dibit.c but not exercised here. */
extern "C" int
comp(const void* a, const void* b) {
    const int* ia = (const int*)a;
    const int* ib = (const int*)b;
    if (*ia == *ib) {
        return 0;
    }
    return (*ia < *ib) ? -1 : 1;
}

extern "C" double
rtl_stream_get_snr_c4fm(void) {
    return 0.0;
}

extern "C" double
rtl_stream_estimate_snr_c4fm_eye(void) {
    return 0.0;
}

extern "C" int
estimate_symbol(int rf_mod, P25Heuristics* heuristics, int previous_dibit, int analog_value, int* dibit) {
    (void)rf_mod;
    (void)heuristics;
    (void)previous_dibit;
    (void)analog_value;
    if (dibit) {
        *dibit = 0;
    }
    return 0; /* heuristics disabled in this test */
}

extern "C" int
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)state;
    (void)have_sync;
    return 0;
}

/* Global expected by demod_pipeline.cpp when linking without RTL front-end. */
int use_halfband_decimator = 0;

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
invert_dibit_test(int dibit) {
    switch (dibit) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 0;
        case 3: return 1;
        default: return -1;
    }
}

/* Map logical dibit (0..3) to ideal four-level scalar symbol. */
static int16_t
symbol_from_dibit(int dibit) {
    /* Thresholds in digitize() use center/lmid/umid; pick clear interior points. */
    switch (dibit) {
        case 0: return 5000;   /* between center and umid */
        case 1: return 20000;  /* above umid */
        case 2: return -5000;  /* between lmid and center */
        case 3: return -20000; /* below lmid */
        default: return 0;
    }
}

static void
init_demod_for_cqpsk(demod_state* s, const int16_t* iq_src, int pairs) {
    memset(s, 0, sizeof(*s));
    /* Copy synthetic I/Q into hb_workbuf and point lowpassed at it. */
    for (int i = 0; i < pairs * 2; i++) {
        s->hb_workbuf[i] = iq_src[i];
    }
    s->lowpassed = s->hb_workbuf;
    s->lp_len = pairs * 2;

    s->cqpsk_enable = 1;
    s->downsample_passes = 0;
    s->downsample = 1;
    s->now_r = 0;
    s->now_j = 0;
    s->prev_index = 0;

    /* Disable auxiliary processing; keep EQ in default identity configuration. */
    s->iq_dc_block_enable = 0;
    s->fm_agc_enable = 0;
    s->fm_limiter_enable = 0;
    s->fm_cma_enable = 0;
    s->fll_enabled = 0;
    s->ted_enabled = 0;
    s->squelch_level = 0;
    s->iqbal_enable = 0;
    s->post_downsample = 1;
    s->blanker_enable = 0;
    s->squelch_gate_open = 1;

    s->cqpsk_eq_initialized = 0;
    s->cqpsk_lms_enable = 0;
}

static void
init_symbol_state(dsd_opts* opts, dsd_state* st) {
    memset(opts, 0, sizeof(*opts));
    memset(st, 0, sizeof(*st));

    opts->use_heuristics = 0;

    /* Configure QPSK RF modulation and P25 Phase 2 (+) synctype. */
    st->rf_mod = 1;
    st->synctype = 35;

    /* Idealized four-level scalar constellation for slicer thresholds. */
    st->min = -30000;
    st->lmid = -10000;
    st->center = 0;
    st->umid = 10000;
    st->max = 30000;

    static int dibit_buf[64];
    static int dmr_payload_buf[64];
    st->dibit_buf = dibit_buf;
    st->dibit_buf_p = dibit_buf;
    st->dmr_payload_buf = dmr_payload_buf;
    st->dmr_payload_p = dmr_payload_buf;
    st->dmr_reliab_p = NULL;
}

int
main(void) {
    /* Known dibit pattern covering all four symbol regions. */
    const int nsym = 8;
    const int expect_dibits[nsym] = {0, 1, 2, 3, 0, 1, 2, 3};

    /* Build synthetic I/Q sequence: one complex sample per symbol, Q=0. */
    int16_t iq[2 * nsym];
    for (int k = 0; k < nsym; k++) {
        int16_t I = symbol_from_dibit(expect_dibits[k]);
        iq[2 * k + 0] = I;
        iq[2 * k + 1] = 0;
    }

    demod_state demod;
    init_demod_for_cqpsk(&demod, iq, nsym);

    /* CQPSK equalizer (identity) then copy I-channel symbols. */
    cqpsk_process_block(&demod);
    demod.result_len = nsym;
    for (int k = 0; k < nsym; k++) {
        demod.result[k] = demod.lowpassed[(size_t)(k << 1)];
    }

    int errors = 0;

    errors += expect_eq_int("RESULT_LEN", demod.result_len, nsym);
    if (errors != 0) {
        return 1;
    }

    /* Feed I-channel symbols into legacy digitizer and compare dibits. */
    dsd_opts opts;
    dsd_state st;
    init_symbol_state(&opts, &st);

    for (int k = 0; k < nsym; k++) {
        int symbol = (int)demod.result[k];
        int dib = digitize(&opts, &st, symbol);
        char tag[64];
        snprintf(tag, sizeof tag, "DIBIT_%d", k);
        errors += expect_eq_int(tag, dib, expect_dibits[k]);
    }

    /* Negative P25P2 case: synctype=36 should yield inverted dibits. */
    init_symbol_state(&opts, &st);
    st.synctype = 36;
    for (int k = 0; k < nsym; k++) {
        int symbol = (int)demod.result[k];
        int dib = digitize(&opts, &st, symbol);
        int want = invert_dibit_test(expect_dibits[k]);
        char tag[64];
        snprintf(tag, sizeof tag, "DIBIT_NEG_%d", k);
        errors += expect_eq_int(tag, dib, want);
    }

    if (errors != 0) {
        fprintf(stderr, "CQPSK_BITS: %d mismatches\n", errors);
        return 1;
    }
    return 0;
}
