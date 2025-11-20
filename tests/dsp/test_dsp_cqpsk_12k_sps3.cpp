// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Synthetic CQPSK wiring test for 12 kHz / SPS=3.
 *
 * Verifies that, for a CQPSK-enabled demod_state configured with:
 *   - Fs = 12 kHz complex baseband
 *   - TED enabled with ted_sps = 3
 *   - CQPSK matched filter (RRC) enabled
 *   - mode_demod = qpsk_differential_demod (CQPSK path)
 *
 * a single call to full_demod() will:
 *   - Initialize the CQPSK equalizer with taps derived from SPS
 *     (5 taps for SPS=3) and sym_stride == ted_sps.
 *   - Run the Costas loop (costas_state.initialized becomes 1).
 *   - Advance the TED fractional phase accumulator (mu_q20 changes).
 *
 * This is an end-to-end wiring sanity check rather than a convergence test.
 */

#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/ted.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global flag referenced by demod_pipeline.cpp; disable HB decimator for tests. */
int use_halfband_decimator = 0;

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));

    /* Nominal complex baseband rate for CQPSK path. */
    const int Fs = 12000;
    s->rate_in = Fs;
    s->rate_out = Fs;

    /* Disable decimator cascade; use low_pass() with downsample=1. */
    s->downsample_passes = 0;
    s->downsample = 1;
    s->post_downsample = 1;
    s->squelch_gate_open = 1;

    /* Synthetic CQPSK-like waveform: nsym symbols, each repeated sps times. */
    const int sps = 3;
    const int nsym = 8;
    const int pairs = nsym * sps;
    const int16_t amp = 9000;
    int I_sym[nsym] = {1, -1, -1, 1, 1, -1, 1, -1};
    int Q_sym[nsym] = {1, 1, -1, -1, 1, 1, -1, -1};

    for (int k = 0; k < nsym; k++) {
        for (int n = 0; n < sps; n++) {
            int idx = k * sps + n;
            s->hb_workbuf[2 * idx + 0] = (int16_t)(I_sym[k] * amp);
            s->hb_workbuf[2 * idx + 1] = (int16_t)(Q_sym[k] * amp);
        }
    }
    s->lowpassed = s->hb_workbuf;
    s->lp_len = pairs * 2;

    /* Enable CQPSK path with RRC matched filter and TED. */
    s->cqpsk_enable = 1;
    s->cqpsk_mf_enable = 1;
    s->cqpsk_rrc_enable = 1;
    s->cqpsk_rrc_alpha_q15 = (int)(0.25 * 32768.0 + 0.5); /* alpha ~0.25 */
    s->cqpsk_rrc_span_syms = 6;                           /* ~12-symbol span */

    s->ted_enabled = 1;
    s->ted_force = 0;
    s->ted_sps = sps;
    s->ted_gain_q20 = 64;
    ted_init_state(&s->ted_state);

    /* Use CQPSK demodulator so TED runs (mode_demod != &dsd_fm_demod)
       and Costas is active (mode_demod != &raw_demod). */
    s->mode_demod = &qpsk_differential_demod;

    /* Ensure CQPSK EQ will be initialized on first block. */
    s->cqpsk_eq_initialized = 0;
    s->cqpsk_lms_enable = 0;

    int mu0 = s->ted_state.mu_q20;

    full_demod(s);

    int rc = 0;

    if (!s->cqpsk_eq_initialized) {
        fprintf(stderr, "CQPSK_12K_SPS3: cqpsk_eq not initialized\n");
        rc = 1;
    } else {
        if (s->cqpsk_eq.num_taps != 5) {
            fprintf(stderr, "CQPSK_12K_SPS3: num_taps=%d expected=5 for sps=%d\n", s->cqpsk_eq.num_taps, sps);
            rc = 1;
        }
        if (s->cqpsk_eq.sym_stride != sps) {
            fprintf(stderr, "CQPSK_12K_SPS3: sym_stride=%d expected=%d\n", s->cqpsk_eq.sym_stride, sps);
            rc = 1;
        }
    }

    /* Costas loop should have touched its internal state. */
    if (!s->costas_state.initialized) {
        fprintf(stderr, "CQPSK_12K_SPS3: Costas state not updated (initialized=0)\n");
        rc = 1;
    }

    /* TED should have advanced fractional phase. */
    if (s->ted_state.mu_q20 == mu0) {
        fprintf(stderr, "CQPSK_12K_SPS3: TED mu_q20 not advanced (mu=%d)\n", s->ted_state.mu_q20);
        rc = 1;
    }

    free(s);
    return rc;
}
