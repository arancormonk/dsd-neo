// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * End-to-end CQPSK pipeline smoke test.
 *
 * Drives full_demod() with a small synthetic CQPSK-like waveform and asserts
 * that, when cqpsk_enable=1, the pipeline produces an I-channel symbol stream
 * via qpsk_i_demod instead of the FM discriminator path.
 *
 * The test configures the demod_state so that:
 *  - Decimation reduces to a no-op low_pass() (downsample=1).
 *  - DC block, matched filter, FLL, TED, IQ balance, and squelch are disabled.
 *  - CQPSK equalizer runs in its default identity configuration.
 *  - Costas is skipped by setting mode_demod=&raw_demod (as allowed by the
 *    pipeline guard for unit tests).
 *
 * Under these conditions the CQPSK branch effectively reduces to:
 *   low_pass -> cqpsk_process_block (identity EQ) -> qpsk_i_demod,
 * so the output should be the I component of the input complex baseband.
 */

#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global flag expected by demod_pipeline.cpp when linking without RTL front-end. */
int use_halfband_decimator = 0;

static int
arrays_equal_i16(const int16_t* a, const int16_t* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void
init_cqpsk_common(demod_state* s, const int16_t* iq_src, int pairs, int sps) {
    memset(s, 0, sizeof(*s));
    /* Copy synthetic baseband into hb_workbuf and point lowpassed at it. */
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

    /* Disable auxiliary processing shared across variants. */
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

    /* Use default identity CQPSK EQ (no LMS/DFE/WL). */
    s->cqpsk_eq_initialized = 0;
    s->cqpsk_lms_enable = 0;

    (void)sps; /* reserved for future SPS-dependent defaults */
}

static int
run_identity_variant(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));

    /* Synthetic CQPSK-like sequence: four symbols on the unit circle (scaled). */
    const int pairs = 4;
    const int16_t amp = 8000;
    int16_t* iq = s->hb_workbuf; /* reuse internal aligned storage */
    iq[0] = amp;
    iq[1] = amp; /* 45 deg */
    iq[2] = -amp;
    iq[3] = amp; /* 135 deg */
    iq[4] = -amp;
    iq[5] = -amp; /* 225 deg */
    iq[6] = amp;
    iq[7] = -amp; /* 315 deg */

    s->lowpassed = iq;
    s->lp_len = pairs * 2;

    /* Configure demod_state for CQPSK branch with minimal processing. */
    s->cqpsk_enable = 1;
    s->downsample_passes = 0; /* use low_pass() */
    s->downsample = 1;        /* 1:1 copy in low_pass */
    s->now_r = 0;
    s->now_j = 0;
    s->prev_index = 0;

    /* Disable auxiliary processing to keep path simple and deterministic. */
    s->iq_dc_block_enable = 0;
    s->fm_agc_enable = 0;
    s->fm_limiter_enable = 0;
    s->fm_cma_enable = 0;
    s->fll_enabled = 0;
    s->ted_enabled = 0;
    s->squelch_level = 0;
    s->iqbal_enable = 0;
    s->post_downsample = 1;

    /* Skip Costas in this unit test as permitted by the CQPSK branch guard. */
    s->mode_demod = &raw_demod;

    /* Ensure CQPSK equalizer is in its default identity configuration. */
    s->cqpsk_eq_initialized = 0;

    /* Run full pipeline. */
    full_demod(s);

    /* For CQPSK, qpsk_i_demod should produce one real symbol per complex sample. */
    if (s->result_len != pairs) {
        fprintf(stderr, "CQPSK_PIPELINE_IDENTITY: result_len=%d want=%d\n", s->result_len, pairs);
        free(s);
        return 1;
    }

    /* Expected I-channel symbols (one per complex input sample). */
    int16_t expect[pairs];
    expect[0] = amp;
    expect[1] = -amp;
    expect[2] = -amp;
    expect[3] = amp;

    if (!arrays_equal_i16(s->result, expect, pairs)) {
        fprintf(stderr, "CQPSK_PIPELINE_IDENTITY: I-channel mismatch\n");
        fprintf(stderr, "  got:    ");
        for (int i = 0; i < pairs; i++) {
            fprintf(stderr, "%d ", s->result[i]);
        }
        fprintf(stderr, "\n  expect: ");
        for (int i = 0; i < pairs; i++) {
            fprintf(stderr, "%d ", expect[i]);
        }
        fprintf(stderr, "\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}

/* Helper: sign of int16 treated as +/-1, zero treated as mismatch later. */
static inline int
sgn_i16(int16_t v) {
    return (v >= 0) ? 1 : -1;
}

static int
run_rrc_costas_variant(void) {
    /* CQPSK-like sequence with distinct I/Q patterns to disambiguate rotation. */
    const int sps = 4;
    const int nsym = 6;
    const int16_t amp = 7000;
    int I_sym[nsym] = {1, -1, -1, 1, 1, -1};
    int Q_sym[nsym] = {1, 1, -1, -1, 1, 1};

    const int pairs = nsym * sps;
    int16_t base_iq[2 * pairs];
    for (int k = 0; k < nsym; k++) {
        for (int n = 0; n < sps; n++) {
            int idx = k * sps + n;
            base_iq[2 * idx + 0] = (int16_t)(I_sym[k] * amp);
            base_iq[2 * idx + 1] = (int16_t)(Q_sym[k] * amp);
        }
    }

    demod_state* s_ref = (demod_state*)malloc(sizeof(demod_state));
    demod_state* s_rrc = (demod_state*)malloc(sizeof(demod_state));
    if (!s_ref || !s_rrc) {
        fprintf(stderr, "alloc demod_state failed (RRC)\n");
        free(s_ref);
        free(s_rrc);
        return 1;
    }
    init_cqpsk_common(s_ref, base_iq, pairs, sps);
    init_cqpsk_common(s_rrc, base_iq, pairs, sps);

    /* Reference: CQPSK branch without MF/Costas (raw passthrough I-channel). */
    s_ref->cqpsk_mf_enable = 0;
    s_ref->ted_sps = sps;
    s_ref->mode_demod = &raw_demod;

    /* RRC+Costas: enable MF and use non-raw mode_demod to activate Costas. */
    s_rrc->cqpsk_mf_enable = 1;
    s_rrc->cqpsk_rrc_enable = 1;
    s_rrc->ted_sps = sps;
    /* Non-trivial RRC configuration: alpha≈0.25, span≈6 symbols total. */
    s_rrc->cqpsk_rrc_alpha_q15 = (int)(0.25 * 32768.0 + 0.5);
    s_rrc->cqpsk_rrc_span_syms = 3;
    s_rrc->mode_demod = &dsd_fm_demod;

    full_demod(s_ref);
    full_demod(s_rrc);

    if (s_ref->result_len != pairs || s_rrc->result_len != pairs) {
        fprintf(stderr, "CQPSK_PIPELINE_RRC: result_len ref=%d rrc=%d want=%d\n", s_ref->result_len, s_rrc->result_len,
                pairs);
        free(s_ref);
        free(s_rrc);
        return 1;
    }

    /* RRC/MF should alter the waveform relative to the reference path. */
    if (arrays_equal_i16(s_ref->result, s_rrc->result, pairs)) {
        fprintf(stderr, "CQPSK_PIPELINE_RRC: MF+RRC output identical to reference (unexpected)\n");
        free(s_ref);
        free(s_rrc);
        return 1;
    }

    /* Costas loop should have run for the RRC variant only. */
    if (s_ref->costas_e4_prev_set != 0) {
        fprintf(stderr, "CQPSK_PIPELINE_RRC: Costas state updated for reference path\n");
        free(s_ref);
        free(s_rrc);
        return 1;
    }
    if (s_rrc->costas_e4_prev_set == 0) {
        fprintf(stderr, "CQPSK_PIPELINE_RRC: Costas state not updated for RRC path\n");
        free(s_ref);
        free(s_rrc);
        return 1;
    }

    free(s_ref);
    free(s_rrc);
    return 0;
}

int
main(void) {
    if (run_identity_variant() != 0) {
        return 1;
    }
    if (run_rrc_costas_variant() != 0) {
        return 1;
    }
    return 0;
}
