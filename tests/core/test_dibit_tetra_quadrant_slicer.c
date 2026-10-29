// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * TETRA's pi/4-DQPSK symbols are arg(z) * 4/pi of the differential phasor: +-1 for +-pi/4, +-3 for +-3pi/4. Their
 * maximum-likelihood decision boundaries are the quadrant edges, 0 and +-2, whatever the adaptive four-level slicer
 * levels (center, umid, lmid, max, min) have drifted to. The soft costs TETRA decodes with are taken against the fixed
 * levels too. All of it on QPSK only: under another modulation the adaptive levels slice TETRA as everything else.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

float
// NOLINTNEXTLINE(misc-use-internal-linkage)
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)state;
    (void)have_sync;
    return 0.0f;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_realtime_mono_ns(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ns(uint64_t ns) {
    (void)ns;
}

void
dsd_neo_config_init(void) {}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

static const int k_tetra_synctypes[4] = {DSD_SYNC_TETRA_NDB_POS, DSD_SYNC_TETRA_NDB_NEG, DSD_SYNC_TETRA_SB_POS,
                                         DSD_SYNC_TETRA_SB_NEG};

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_soft_buf);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000001, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000001, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000001, sizeof(dsd_dibit_soft_t));
    if (state->dibit_buf == NULL || state->dmr_payload_buf == NULL || state->dmr_soft_buf == NULL) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    return 1;
}

static void
rewind_state_buffers(dsd_state* state) {
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
}

/* The adaptive levels measured at TETRA uplink control-burst decodes: the inner/outer decision sits near +-60 degrees
   instead of +-90. */
static void
set_measured_adaptive_levels(dsd_state* state) {
    state->rf_mod = 1;
    state->center = -0.08f;
    state->umid = 1.29f;
    state->lmid = -1.45f;
    state->max = 2.13f;
    state->min = -2.28f;
}

/* The dibit of a symbol's quadrant: +3 -> 1, +1 -> 0, -1 -> 2, -3 -> 3. */
static int
quadrant_dibit(float symbol) {
    if (symbol >= 2.0f) {
        return 1;
    }
    if (symbol >= 0.0f) {
        return 0;
    }
    if (symbol >= -2.0f) {
        return 2;
    }
    return 3;
}

static int
check_digitize(dsd_opts* opts, dsd_state* state, float symbol, int want, const char* what) {
    rewind_state_buffers(state);
    int got = digitize(opts, state, symbol);
    int stored = state->dibit_buf_p[-1];
    int payload = state->dmr_payload_p[-1];
    if (got != want || stored != want || payload != want || state->last_dibit != want) {
        DSD_FPRINTF(stderr,
                    "%s: synctype %d symbol %.4f sliced %d (stored %d, payload %d, last %d), want %d (center %.2f "
                    "umid %.2f lmid %.2f)\n",
                    what, state->synctype, (double)symbol, got, stored, payload, state->last_dibit, want,
                    (double)state->center, (double)state->umid, (double)state->lmid);
        return 1;
    }
    return 0;
}

static int
test_tetra_slices_at_quadrant_boundaries(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_measured_adaptive_levels(&state);

    const float eps = 0.02f;
    /* 15 degrees of phase error is 15/45 of a level step. */
    const float err15 = 15.0f / 45.0f;

    const struct {
        float symbol;
        int dibit;
    } cases[] = {
        {2.0f - eps, 0},    {2.0f + eps, 1},
        {-(2.0f - eps), 2}, {-(2.0f + eps), 3},
        {eps, 0},           {-eps, 2},
        {1.0f + err15, 0},  {-(1.0f + err15), 2},
        {1.0f - err15, 0},  {-(1.0f - err15), 2},
        {3.0f - err15, 1},  {-(3.0f - err15), 3},
        {1.0f, 0},          {3.0f, 1},
        {-1.0f, 2},         {-3.0f, 3},
    };

    int rc = 0;
    for (int s = 0; s < 4; s++) {
        state.synctype = k_tetra_synctypes[s];
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            rc |= check_digitize(&opts, &state, cases[i].symbol, cases[i].dibit, "quadrant boundary");
        }
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_adaptive_levels_do_not_reach_tetra_slicing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    const struct {
        float center, umid, lmid, max, min;
    } skews[] = {
        {0.0f, 2.0f, -2.0f, 3.0f, -3.0f},       /* the nominal levels */
        {-0.08f, 1.29f, -1.45f, 2.13f, -2.28f}, /* measured on the uplink */
        {0.0f, 0.6f, -0.6f, 1.0f, -1.0f},       /* collapsed */
        {0.0f, 2.8f, -2.8f, 3.6f, -3.6f},       /* stretched */
        {0.4f, 1.1f, -2.6f, 2.0f, -3.5f},       /* lopsided, centre high */
        {-0.4f, 2.6f, -1.1f, 3.5f, -2.0f},      /* lopsided, centre low */
    };

    int rc = 0;
    for (size_t k = 0; k < sizeof(skews) / sizeof(skews[0]); k++) {
        state.rf_mod = 1;
        state.center = skews[k].center;
        state.umid = skews[k].umid;
        state.lmid = skews[k].lmid;
        state.max = skews[k].max;
        state.min = skews[k].min;
        for (int s = 0; s < 4; s++) {
            state.synctype = k_tetra_synctypes[s];
            /* Steps of 0.1 offset by 0.05 never land on a boundary. */
            for (int i = 0; i < 80; i++) {
                float symbol = -3.95f + (0.1f * (float)i);
                rc |= check_digitize(&opts, &state, symbol, quadrant_dibit(symbol), "skewed levels");
            }
        }
    }

    /* The adaptive regions still slice every other four-level mode: at the measured levels a non-TETRA +1.33 is +3. */
    set_measured_adaptive_levels(&state);
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    rc |= check_digitize(&opts, &state, 1.0f + (15.0f / 45.0f), 1, "non-TETRA adaptive regions");
    state.synctype = DSD_SYNC_NXDN_POS;
    rc |= check_digitize(&opts, &state, -0.05f, 0, "non-TETRA adaptive centre");

    free_state_buffers(&state);
    return rc;
}

/* Off QPSK (-fT -mc, which no TETRA receiver runs) the symbols are not arg(z) * 4/pi: TETRA's are sliced on the
   adaptive levels, as the frame sync hunt slices them there, and the predicate both read says so. */
static int
test_tetra_off_qpsk_slices_on_adaptive_levels(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    int rc = 0;
    set_measured_adaptive_levels(&state);
    if (dsd_tetra_quadrant_levels(&state) != 1 || dsd_tetra_quadrant_levels(NULL) != 0) {
        DSD_FPRINTF(stderr, "quadrant levels: not on QPSK\n");
        rc = 1;
    }
    for (int mod = 0; mod <= 2; mod += 2) {
        state.rf_mod = mod;
        if (dsd_tetra_quadrant_levels(&state) != 0) {
            DSD_FPRINTF(stderr, "quadrant levels on rf_mod %d\n", mod);
            rc = 1;
        }
        for (int s = 0; s < 4; s++) {
            state.synctype = k_tetra_synctypes[s];
            /* At the measured levels +1.33 is outer and -0.05 positive, where the quadrants say inner and negative. */
            rc |= check_digitize(&opts, &state, 1.0f + (15.0f / 45.0f), 1, "off QPSK: adaptive outer edge");
            rc |= check_digitize(&opts, &state, -0.05f, 0, "off QPSK: adaptive centre");
        }
    }
    free_state_buffers(&state);
    return rc;
}

static int
cost_within(uint16_t got, uint16_t want, int tolerance) {
    int diff = (int)got - (int)want;
    return diff >= -tolerance && diff <= tolerance;
}

static int
test_tetra_soft_costs_at_fixed_levels(void) {
    int rc = 0;

    /* The four-level cost mapping evaluated at the state whose levels are the fixed +-1/+-3 and whose decision edges
       are 0 and +-2: the regions [-4, -2, 0, 2, 4]. */
    static dsd_state fixed;
    DSD_MEMSET(&fixed, 0, sizeof(fixed));
    fixed.min = -4.0f;
    fixed.lmid = -2.0f;
    fixed.center = 0.0f;
    fixed.umid = 2.0f;
    fixed.max = 4.0f;
    for (int i = 0; i <= 200; i++) {
        float symbol = -5.0f + (0.05f * (float)i);
        for (int bit = 0; bit < 2; bit++) {
            uint16_t got = cqpsk_soft_symbol_to_viterbi_cost(symbol, bit);
            uint16_t want = soft_symbol_to_viterbi_cost(symbol, &fixed, bit);
            if (!cost_within(got, want, 1)) {
                DSD_FPRINTF(stderr, "fixed-level cost symbol %.2f bit %d = %u, the mapping at fixed levels %u\n",
                            (double)symbol, bit, got, want);
                rc = 1;
            }
        }
    }

    /* Bit 0 (MSB) is the sign: 0 for the positive pair. Bit 1 (LSB) is inner (0) against outer (1). */
    const float eps = 0.02f;

    const struct {
        float symbol;
        int msb;
        int lsb;
    } cases[] = {
        {eps, 0, 0},
        {-eps, 1, 0},
        {2.0f - eps, 0, 0},
        {2.0f + eps, 0, 1},
        {-(2.0f - eps), 1, 0},
        {-(2.0f + eps), 1, 1},
        {1.0f + (15.0f / 45.0f), 0, 0},
        {-(1.0f + (15.0f / 45.0f)), 1, 0},
        {3.0f - (15.0f / 45.0f), 0, 1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint16_t msb = cqpsk_soft_symbol_to_viterbi_cost(cases[i].symbol, 0);
        uint16_t lsb = cqpsk_soft_symbol_to_viterbi_cost(cases[i].symbol, 1);
        int msb_bit = msb > 32768U;
        int lsb_bit = lsb > 32768U;
        if (msb_bit != cases[i].msb || lsb_bit != cases[i].lsb || msb == 32768U || lsb == 32768U) {
            DSD_FPRINTF(stderr, "soft cost symbol %.4f msb=%u lsb=%u, want bits %d%d\n", (double)cases[i].symbol, msb,
                        lsb, cases[i].msb, cases[i].lsb);
            rc = 1;
        }
    }

    /* On a boundary the bit it decides is a coin toss, and the costs mirror about 0. */
    if (!cost_within(cqpsk_soft_symbol_to_viterbi_cost(0.0f, 0), 32768U, 1)
        || !cost_within(cqpsk_soft_symbol_to_viterbi_cost(2.0f, 1), 32768U, 1)
        || !cost_within(cqpsk_soft_symbol_to_viterbi_cost(-2.0f, 1), 32768U, 1)) {
        DSD_FPRINTF(stderr, "boundary costs not neutral: %u %u %u\n", cqpsk_soft_symbol_to_viterbi_cost(0.0f, 0),
                    cqpsk_soft_symbol_to_viterbi_cost(2.0f, 1), cqpsk_soft_symbol_to_viterbi_cost(-2.0f, 1));
        rc = 1;
    }
    for (int i = 0; i <= 40; i++) {
        float symbol = 0.1f * (float)i;
        uint16_t pos = cqpsk_soft_symbol_to_viterbi_cost(symbol, 0);
        uint16_t neg = cqpsk_soft_symbol_to_viterbi_cost(-symbol, 0);
        uint16_t pos_lsb = cqpsk_soft_symbol_to_viterbi_cost(symbol, 1);
        uint16_t neg_lsb = cqpsk_soft_symbol_to_viterbi_cost(-symbol, 1);
        if (!cost_within((uint16_t)(65535U - pos), neg, 1) || !cost_within(pos_lsb, neg_lsb, 1)) {
            DSD_FPRINTF(stderr, "costs not mirrored at %.1f: msb %u/%u lsb %u/%u\n", (double)symbol, pos, neg, pos_lsb,
                        neg_lsb);
            rc = 1;
        }
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_tetra_slices_at_quadrant_boundaries();
    rc |= test_adaptive_levels_do_not_reach_tetra_slicing();
    rc |= test_tetra_off_qpsk_slices_on_adaptive_levels();
    rc |= test_tetra_soft_costs_at_fixed_levels();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
