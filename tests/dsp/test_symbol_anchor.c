// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol grid's sync anchor (src/dsp/symbol_anchor.c, issue #729): where a known sync word sits against the grid
 * that decided it, and the move the grid takes from that.
 */

#include <assert.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "symbol_anchor_internal.h"

#define TEST_PI   3.14159265358979323846
#define SPS       10
#define SYNC_LEN  24
#define N_SYMBOLS 40

static const char* k_sync = "131111333113313313113313"; /* DMR BS voice sync */

static double
rc_pulse(double t) {
    const double a = 0.2;
    if (fabs(t) < 1e-9) {
        return 1.0;
    }
    const double s = sin(TEST_PI * t) / (TEST_PI * t);
    const double den = 1.0 - (2.0 * a * t) * (2.0 * a * t);
    if (fabs(den) < 1e-9) {
        return (TEST_PI / 4.0) * s;
    }
    return s * cos(TEST_PI * a * t) / den;
}

static double
level_for(char c) {
    switch (c) {
        case '0': return 1.0;
        case '1': return 3.0;
        case '2': return -1.0;
        default: return -3.0;
    }
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static double
noise(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return ((double)(g_rng >> 11) / 9007199254740992.0) - 0.5;
}

/* N_SYMBOLS symbols of raised-cosine 4FSK whose true centres sit @p offset samples after the grid's centre index
   (k * SPS + SPS/2), @p word (@p len symbols) starting at symbol 8, random symbols elsewhere. */
static void
make_word_signal(const char* word, int len, double offset, double polarity, double noise_amp, float* samples,
                 int* centres, char* dibits) {
    for (int k = 0; k < N_SYMBOLS; k++) {
        dibits[k] = (k >= 8 && k < 8 + len) ? word[k - 8] : (char)('0' + (int)((noise() + 0.5) * 3.999));
    }
    for (int n = 0; n < N_SYMBOLS * SPS; n++) {
        double v = 0.0;
        for (int k = 0; k < N_SYMBOLS; k++) {
            const double centre = (double)(k * SPS) + (0.5 * (double)SPS) + offset;
            v += level_for(dibits[k]) * rc_pulse(((double)n - centre) / SPS);
        }
        samples[n] = (float)(polarity * v + noise_amp * noise());
    }
    for (int k = 0; k < N_SYMBOLS; k++) {
        centres[k] = k * SPS + SPS / 2;
    }
}

/* The DMR BS voice sync at symbol 8. */
static void
make_signal(double offset, double polarity, double noise_amp, float* samples, int* centres, char* dibits) {
    make_word_signal(k_sync, SYNC_LEN, offset, polarity, noise_amp, samples, centres, dibits);
}

static void
test_measures_whole_and_fractional_offsets(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    const double offsets[] = {-3.0, -2.0, -1.0, -0.4, 0.0, 0.4, 1.0, 2.0, 3.0, 1.5};
    for (size_t c = 0; c < sizeof(offsets) / sizeof(offsets[0]); c++) {
        make_signal(offsets[c], 1.0, 0.0, samples, centres, dibits);
        double measured = 99.0;
        assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS,
                                                &measured)
               == 1);
        assert(fabs(measured - offsets[c]) < 0.5);
    }
}

static void
test_inverted_signal_measures_the_same(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(2.0, -1.0, 0.0, samples, centres, dibits);
    double measured = 0.0;
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS, &measured)
           == 1);
    assert(fabs(measured - 2.0) < 0.5);
}

static void
test_noise_does_not_move_a_centred_grid(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    for (int trial = 0; trial < 20; trial++) {
        make_signal(0.0, 1.0, 1.0, samples, centres, dibits); /* noise up to +/-0.5, against +/-3 symbols */
        double measured = 99.0;
        assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS,
                                                &measured)
               == 1);
        assert(fabs(measured) <= 0.5);
    }
}

static void
test_declines(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    double measured = 0.0;
    /* one sample per symbol: no sub-symbol structure */
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, 1, &measured)
           == 0);
    /* centres too near the end for the shifts to read */
    assert(dsd_symbol_anchor_measure_offset(samples, centres[8 + SYNC_LEN - 1] + 2, &centres[8], &dibits[8], SYNC_LEN,
                                            SPS, &measured)
           == 0);
    /* a flat trace */
    static float flat[N_SYMBOLS * SPS];
    DSD_MEMSET(flat, 0, sizeof(flat));
    assert(dsd_symbol_anchor_measure_offset(flat, N_SYMBOLS * SPS, &centres[8], &dibits[8], SYNC_LEN, SPS, &measured)
           == 0);
    /* not a dibit */
    char bad[SYNC_LEN];
    DSD_MEMCPY(bad, &dibits[8], SYNC_LEN);
    bad[3] = 'x';
    assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], bad, SYNC_LEN, SPS, &measured) == 0);
}

/* Through the grid's side: push the samples symbol by symbol, name the sync word, and read the move. */
static int
run_ring(double offset) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(offset, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_symbol_arm_sync_anchor(state);                    /* allocates and starts the recording */
    dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN); /* nothing recorded yet, so not measured */
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    assert(dsd_symbol_anchor_take_slip(anchor) == 0);
    int slip = 0;
    for (int k = 0; k < N_SYMBOLS; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[k * SPS + i], i == SPS / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        if (k == 8 + SYNC_LEN - 1) {
            dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
        }
        if (k == 8 + SYNC_LEN) {
            slip = dsd_symbol_anchor_take_slip(anchor);
        }
    }
    dsd_state_ext_free_all(state);
    free(state);
    return slip;
}

static void
test_ring_queues_the_move_toward_the_symbols(void) {
    assert(run_ring(0.0) == 0);
    assert(run_ring(0.3) == 0);
    assert(run_ring(1.0) == 1);   /* symbols later than the grid: take a sample more */
    assert(run_ring(-1.0) == -1); /* earlier: one fewer */
    assert(run_ring(2.0) == 2);   /* at most two at once */
    assert(run_ring(-3.0) == -2);
}

static void
test_reset_drops_the_pending_move(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(1.0, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_symbol_arm_sync_anchor(state);
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    for (int k = 0; k <= 8 + SYNC_LEN; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[k * SPS + i], i == SPS / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        if (k == 8 + SYNC_LEN - 1) {
            dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
        }
    }
    dsd_symbol_anchor_reset(state); /* the stream the samples came from ended */
    assert(dsd_symbol_anchor_take_slip(anchor) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

/* At a matched filter's switch-on right behind the sync word (M17's LSF, issue #834): measured with no look-ahead
   symbol, after a recording a preamble kept alive past the 64 symbols an armed anchor waits. */
static int
run_switch(double offset, int lead_symbols) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    /* The grid's centre is sample (SPS - 1) / 2 of its span (dsd_opts_symbol_center()), which leaves the half symbol
       the measurement reads past the last centre inside the sync word's own span: one sample before make_signal()'s. */
    make_signal(offset - 1.0, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    dsd_symbol_hold_sync_anchor(state);
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    for (int k = 0; k < lead_symbols; k++) { /* the run, held a symbol at a time */
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, 0.0f, i == (SPS - 1) / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        dsd_symbol_hold_sync_anchor(state);
    }
    for (int k = 0; k < 8 + SYNC_LEN; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[k * SPS + i], i == (SPS - 1) / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
    }
    dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
    const int move = dsd_symbol_anchor_take_switch_offset(state);
    assert(dsd_symbol_anchor_take_switch_offset(state) == 0); /* taken once */
    assert(dsd_symbol_anchor_take_slip(anchor) == 0);         /* nothing queued for the grid behind it */
    dsd_state_ext_free_all(state);
    free(state);
    return move;
}

static void
test_switch_measures_without_a_look_ahead_symbol(void) {
    assert(run_switch(0.0, 0) == 0);
    assert(run_switch(0.3, 0) == 0);
    assert(run_switch(1.0, 0) == 1);
    assert(run_switch(-1.0, 0) == -1);
    assert(run_switch(2.0, 0) == 2);
    assert(run_switch(-2.0, 0) == -2);
    /* A preamble longer than an armed anchor waits: held, the recording is still there for the sync word. */
    assert(run_switch(2.0, 100) == 2);
    assert(run_switch(-1.0, 100) == -1);
}

static void
test_switch_without_a_named_word_moves_nothing(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    assert(dsd_symbol_anchor_take_switch_offset(state) == 0); /* no anchor at all */
    dsd_symbol_arm_sync_anchor(state);
    assert(dsd_symbol_anchor_take_switch_offset(state) == 0); /* recording, no word named */
    dsd_state_ext_free_all(state);
    free(state);
}

/* The whole samples the grid moves for a measured offset, as symbol_anchor.c moves it: to the nearest, at most two,
   none within half a sample. */
static int
slip_for(double offset) {
    const double capped = fmax(-2.0, fmin(2.0, offset));
    return fabs(capped) <= 0.5 ? 0 : (int)lround(capped);
}

/* NXDN's FSW and YSF's frame sync carry inner symbols, named from their second symbol as nxdn_frame.c and ysf.c name
   them (issue #834). YSF's 19 symbols move the grid onto its symbols as an outer word does. The FSW's 9, whose
   crossings are few and whose neighbours reach further into them, leave a grid on its symbols where it is and move one
   off them the right way, to within a sample: held by the hunt-read recording, that sample is measured again on the
   next frame's word. */
static void
test_words_with_inner_symbols_measure(void) {
    static const struct {
        const char* dibits;
        int slack;
    } k_words[] = {
        {"031331121", 1},           /* NXDN FSW from symbol 2 */
        {"1101301302112031031", 0}, /* YSF frame sync from symbol 2 */
    };

    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    const int offsets[] = {-2, -1, 0, 1, 2};
    for (size_t w = 0; w < sizeof(k_words) / sizeof(k_words[0]); w++) {
        const int len = (int)strlen(k_words[w].dibits);
        for (size_t c = 0; c < sizeof(offsets) / sizeof(offsets[0]); c++) {
            for (int polarity = -1; polarity <= 1; polarity += 2) {
                make_word_signal(k_words[w].dibits, len, (double)offsets[c], (double)polarity, 0.0, samples, centres,
                                 dibits);
                double measured = 99.0;
                assert(dsd_symbol_anchor_measure_offset(samples, N_SYMBOLS * SPS, &centres[8], &dibits[8], len, SPS,
                                                        &measured)
                       == 1);
                const int left = offsets[c] - slip_for(measured);
                if (offsets[c] == 0) {
                    assert(left == 0);
                } else {
                    assert(slip_for(measured) != 0 && abs(left) <= k_words[w].slack);
                }
            }
        }
    }
}

/* Push @p symbols symbols of @p samples (from symbol @p first) through the anchor, naming the DMR sync at its end when
   @p name is set. Returns the anchor. */
static dsd_symbol_anchor*
feed_symbols(dsd_state* state, const float* samples, int first, int symbols, int name) {
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    for (int k = first; k < first + symbols; k++) {
        for (int i = 0; i < SPS; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[(k % N_SYMBOLS) * SPS + i], i == SPS / 2);
        }
        dsd_symbol_anchor_end_symbol(anchor, SPS);
        if (name && k == 8 + SYNC_LEN - 1) {
            dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN);
        }
    }
    return anchor;
}

/* A recording a frame arms for the word the hunt will read next holds the grid (keeps the hunt's crossing loop off it)
   only once a measured word has placed the grid: the first frame after acquisition, a recording that lapsed or could
   not be measured, a reset and an M17 preamble's hold all leave the grid to the crossing loop (issue #834). */
static void
test_hunt_recording_holds_only_a_placed_grid(void) {
    static float samples[N_SYMBOLS * SPS];
    static float flat[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    DSD_MEMSET(flat, 0, sizeof(flat));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE; /* an MS burst arms the holds below */

    dsd_symbol_arm_hunt_sync_anchor(state);
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    assert(dsd_symbol_anchor_recording(anchor));
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype)); /* nothing has placed the grid yet */
    (void)feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1);
    assert(!dsd_symbol_anchor_recording(anchor)); /* measured */
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype)); /* the word placed it */
    dsd_symbol_hold_sync_anchor(state);                                /* an M17 preamble's run is the hunt's to time */
    assert(dsd_symbol_anchor_recording(anchor));
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));
    (void)feed_symbols(state, samples, 0, 65, 0); /* no word came: the recording lapses */
    assert(!dsd_symbol_anchor_recording(anchor));
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    (void)feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1); /* placed again */
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));
    (void)feed_symbols(state, flat, 0, 8 + SYNC_LEN + 1, 1); /* a word the trace cannot place */
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    dsd_symbol_arm_sync_anchor(state);
    (void)feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1);
    dsd_symbol_arm_sync_anchor(state); /* a frame's own recording (DMR BS) never holds */
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));
    dsd_symbol_anchor_reset(state); /* the stream ended */
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    dsd_state_ext_free_all(state);
    free(state);
}

/* A word accepted with no recording armed was read on a grid the hunt timed: a frame whose LICH or FICH was refused
   armed nothing, the hunt's crossing loop moved the grid, and the next frame's word lands unrecorded. The grid is no
   longer where a measured word put it, so the next hunt-read recording does not hold it (issue #834). */
static void
test_unrecorded_word_unplaces_the_grid(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    state->lastsynctype = DSD_SYNC_NXDN_POS;

    dsd_symbol_arm_hunt_sync_anchor(state);
    dsd_symbol_anchor* anchor = feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1); /* measured: placed */
    assert(!dsd_symbol_anchor_recording(anchor));
    dsd_symbol_note_sync_anchor(state, k_sync, SYNC_LEN); /* the next frame's word, with nothing armed */
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_recording(anchor));
    assert(!dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    /* The control: the same sequence with the word recorded holds. */
    (void)feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1);
    dsd_symbol_arm_hunt_sync_anchor(state);
    assert(dsd_symbol_anchor_holds_grid(anchor, state->lastsynctype));

    dsd_state_ext_free_all(state);
    free(state);
}

/* A hold belongs to the protocol whose frame armed it. Once the hunt accepts another protocol's sync (lastsynctype
   names it), P25's included, the hold ends for the rest of the recording, so that frame is read on the grid the hunt
   times; a recording armed after any other protocol's frame never holds (issue #834). */
static void
test_hold_ends_at_another_protocols_sync(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    static const int k_others[] = {DSD_SYNC_P25P1_POS,        DSD_SYNC_P25P2_POS,   DSD_SYNC_YSF_POS,
                                   DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_M17_LSF_POS, DSD_SYNC_NONE};
    for (size_t o = 0; o < sizeof(k_others) / sizeof(k_others[0]); o++) {
        dsd_state* state = calloc(1, sizeof(*state));
        assert(state != NULL);
        state->lastsynctype = DSD_SYNC_NXDN_NEG;
        dsd_symbol_arm_sync_anchor(state);
        dsd_symbol_anchor* anchor = feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1); /* placed */
        dsd_symbol_arm_hunt_sync_anchor(state);
        assert(dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_NXDN_POS)); /* the same protocol, either polarity */
        assert(!dsd_symbol_anchor_holds_grid(anchor, k_others[o]));
        assert(!dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_NXDN_NEG)); /* released for good */
        assert(dsd_symbol_anchor_recording(anchor));

        dsd_state_ext_free_all(state);
        free(state);
    }
}

/* A P25 Phase 1 frame read on C4FM arms a hold of its own: the next word, either polarity, is read on the grid the last
   measured word placed, and the hunt accepting any other protocol's sync (or noCarrier's NONE) ends it for good, as a
   P25 sync ends theirs. A recording a P25 frame arms holds nothing for NXDN, YSF or DMR, nor theirs for P25. */
static void
test_p25_hold_is_its_own(void) {
    static float samples[N_SYMBOLS * SPS];
    int centres[N_SYMBOLS];
    char dibits[N_SYMBOLS];
    make_signal(0.0, 1.0, 0.0, samples, centres, dibits);
    static const int k_others[] = {DSD_SYNC_NXDN_POS,         DSD_SYNC_P25P2_POS,   DSD_SYNC_YSF_POS,
                                   DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_M17_LSF_POS, DSD_SYNC_NONE};
    for (size_t o = 0; o < sizeof(k_others) / sizeof(k_others[0]); o++) {
        dsd_state* state = calloc(1, sizeof(*state));
        assert(state != NULL);
        state->lastsynctype = DSD_SYNC_P25P1_POS;
        dsd_symbol_arm_sync_anchor(state);
        dsd_symbol_anchor* anchor = feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1); /* placed */
        dsd_symbol_arm_hunt_sync_anchor(state);
        assert(dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_P25P1_NEG)); /* the same protocol, either polarity */
        assert(dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_P25P1_POS));
        assert(!dsd_symbol_anchor_holds_grid(anchor, k_others[o]));
        assert(!dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_P25P1_POS)); /* released for good */
        assert(dsd_symbol_anchor_recording(anchor));

        (void)feed_symbols(state, samples, 0, 8 + SYNC_LEN + 1, 1); /* placed again */
        state->lastsynctype = k_others[o];                          /* another protocol's frame arms */
        dsd_symbol_arm_hunt_sync_anchor(state);
        assert(!dsd_symbol_anchor_holds_grid(anchor, DSD_SYNC_P25P1_POS));
        dsd_state_ext_free_all(state);
        free(state);
    }
}

/* A P25 word on a grid of @p whole + @p rem / @p den samples a symbol, as the RTL FSK timing alternates spans
   (symbol_rtl_fsk_next_sync()): @p lead symbols of a frame's tail, then the P25p1 sync word, then two more symbols, each
   centred where the grid's span puts it, (span - 1) / 2. @p walk[k] is the grid's phase at the word's symbol k, in
   samples, as the hunt's crossing loop moves it: each change is a slip at that symbol's start, which the symbol's
   samples show (one more or one fewer than its span) and every later centre follows. The transmitter's symbols are
   uniform at its own period (the grid's times 1 + @p ppm 1e-6), @p offset samples after the grid the frame goes on with
   (the word's last centre's). Feeds it through the anchor under @p lastsynctype, names the word from its second symbol
   once read (as the P25 frame sync names it, dsd_symbol_note_p25p1_sync_anchor(), or with @p plain as every other
   caller does), and returns the move the measurement queued, or 99 when it did not measure. */
typedef struct {
    int whole;
    int rem;
    int den;
    double ppm;
    int lead;
    double offset;
    double polarity;
    int lastsynctype;
    int named_from; /* the word's first named symbol: 1, as frame sync names it, or 0 */
    const int* walk;
    int plain; /* named with dsd_symbol_note_sync_anchor(), as NXDN, YSF, DMR and M17 name theirs */
} p25_word_case;

#define P25_WORD    24
#define P25_TAIL    2
#define P25_MAX_LEN (64 + P25_WORD + P25_TAIL)
#define P25_MAX_SPS 24

static int
run_p25_word(const p25_word_case* c) {
    const int total = c->lead + P25_WORD + P25_TAIL;
    assert(total <= P25_MAX_LEN);
    int spans[P25_MAX_LEN];
    int shifts[P25_MAX_LEN];
    char dibits[P25_MAX_LEN];
    int accum = 0;
    for (int k = 0; k < total; k++) {
        spans[k] = c->whole;
        accum += c->rem;
        if (c->den > 0 && accum >= c->den) {
            spans[k]++;
            accum -= c->den;
        }
        const int w = k - c->lead;
        shifts[k] = (w > 0 && w < P25_WORD) ? c->walk[w] - c->walk[w - 1] : 0;
        dibits[k] = (w >= 0 && w < P25_WORD) ? P25P1_SYNC[w] : (char)('0' + (int)((noise() + 0.5) * 3.999));
    }
    /* The grid without the walk, ending at the word's last centre (a span's centre is (span - 1) / 2 into it), where
       the frame's grid goes on; and the samples as the walking grid takes them. */
    const int last = c->lead + P25_WORD - 1;
    int nominal[P25_MAX_LEN];
    int at = 0;
    for (int k = 0; k < total; k++) {
        nominal[k] = at + (spans[k] - 1) / 2;
        at += spans[k];
    }
    int samples_total = 0;
    for (int k = 0; k < total; k++) {
        samples_total += spans[k] + shifts[k];
    }
    int walked_by = 0;
    for (int k = c->lead + 1; k <= last; k++) {
        walked_by += shifts[k];
    }
    /* The transmitter's symbols sit @p offset after the grid's average phase over the word: a rate the grid takes in
       whole samples puts its centres up to half a sample either side of the symbols' own. */
    const double period =
        ((double)c->whole + (c->den > 0 ? (double)c->rem / (double)c->den : 0.0)) * (1.0 + c->ppm * 1e-6);
    double phase = 0.0;
    for (int k = c->lead; k <= last; k++) {
        phase += (double)nominal[k] - ((double)k * period);
    }
    phase = (phase / (double)P25_WORD) + (double)walked_by + c->offset;
    static float samples[P25_MAX_LEN * (P25_MAX_SPS + 2)];
    assert(samples_total <= (int)(sizeof(samples) / sizeof(samples[0])));
    for (int n = 0; n < samples_total; n++) {
        double v = 0.0;
        for (int k = 0; k < total; k++) {
            const double centre = phase + ((double)k * period);
            v += level_for(dibits[k]) * rc_pulse(((double)n - centre) / period);
        }
        samples[n] = (float)(c->polarity * v);
    }

    dsd_state* state = calloc(1, sizeof(*state));
    assert(state != NULL);
    state->lastsynctype = c->lastsynctype;
    dsd_symbol_arm_sync_anchor(state);
    dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    assert(anchor != NULL);
    int n = 0;
    int slip = 99;
    for (int k = 0; k < total; k++) {
        const int centre_at = (spans[k] - 1) / 2 + shifts[k];
        for (int i = 0; i < spans[k] + shifts[k]; i++) {
            dsd_symbol_anchor_push_sample(anchor, samples[n++], i == centre_at);
        }
        dsd_symbol_anchor_end_symbol(anchor, spans[k]);
        if (k == last) {
            if (c->plain) {
                dsd_symbol_note_sync_anchor(state, &P25P1_SYNC[c->named_from], P25_WORD - c->named_from);
            } else {
                dsd_symbol_note_p25p1_sync_anchor(state, &P25P1_SYNC[c->named_from], P25_WORD - c->named_from);
            }
        }
        if (k == last + 1) {
            slip = dsd_symbol_anchor_test_placed(state) ? dsd_symbol_anchor_take_slip(anchor) : 99;
        }
    }
    dsd_state_ext_free_all(state);
    free(state);
    return slip;
}

/* The hunt's crossing loop over a P25 word, as measured on a clean signal: the grid walks off by
   up to three samples over the word's middle and back by its alternating end, a sample past the eye. */
static const int k_p25_walk[P25_WORD] = {0,  0,  0,  0,  -1, -1, 0, -1, -1, -1, -2, -2,
                                         -3, -3, -3, -3, -2, -1, 0, 0,  0,  0,  0,  0};
static const int k_no_walk[P25_WORD] = {0};

/* The P25 Phase 1 word, named from its second symbol as the frame sync names it, measures where the frame's grid sits
   against its symbols, and queues the move that centres it (at most two samples a word): at 24, 48 and 96 kHz (5, 10
   and 20 samples a symbol), at a fractional rate (50 kHz, spans of 10 and 11), with the transmitter's clock 20 ppm off
   ours, either polarity. A grid already centred queues no move, measured all the same. */
static void
test_p25_word_measures_the_frame_grid(void) {
    static const struct {
        int whole;
        int rem;
        int den;
        int max_offset;
    } k_rates[] = {
        {10, 0, 0, 3},       /* 48 kHz */
        {5, 0, 0, 1},        /* 24 kHz: the measurement reaches one sample either way */
        {20, 0, 0, 3},       /* 96 kHz */
        {10, 2000, 4800, 3}, /* 50 kHz */
        {15, 0, 0, 3},       /* 72 kHz */
    };

    for (size_t r = 0; r < sizeof(k_rates) / sizeof(k_rates[0]); r++) {
        for (int offset = -k_rates[r].max_offset; offset <= k_rates[r].max_offset; offset++) {
            for (int polarity = -1; polarity <= 1; polarity += 2) {
                for (int ppm = -20; ppm <= 20; ppm += 40) {
                    const p25_word_case c = {k_rates[r].whole,
                                             k_rates[r].rem,
                                             k_rates[r].den,
                                             (double)ppm,
                                             0,
                                             (double)offset,
                                             (double)polarity,
                                             DSD_SYNC_P25P1_POS,
                                             1,
                                             k_no_walk,
                                             0};
                    const int slip = run_p25_word(&c);
                    const int want = offset > 2 ? 2 : (offset < -2 ? -2 : offset);
                    if (slip != want) {
                        DSD_FPRINTF(stderr, "P25 word, %d+%d/%d sps, offset %d, polarity %d, %d ppm: moved %d\n",
                                    k_rates[r].whole, k_rates[r].rem, k_rates[r].den, offset, polarity, ppm, slip);
                    }
                    assert(slip == want);
                }
            }
        }
    }
}

/* The recording a frame arms at its end starts on the next word's first symbol, so the word is named from its second:
   the measurement needs a symbol of samples before the first centre it reads. Named whole, it never measures (the grid
   stays the hunt's), unless a tail before the word (a TSDU's null pad, which its decoder leaves to the hunt) gives that
   margin. */
static void
test_p25_word_named_from_its_second_symbol(void) {
    p25_word_case c = {10, 0, 0, 0.0, 0, 1.0, 1.0, DSD_SYNC_P25P1_POS, 0, k_no_walk, 0};
    assert(run_p25_word(&c) == 99);
    c.named_from = 1;
    assert(run_p25_word(&c) == 1);
    c.named_from = 0;
    c.lead = 22;
    assert(run_p25_word(&c) == 1);
    c.named_from = 1;
    assert(run_p25_word(&c) == 1);
}

/* The first P25 word after acquisition (a lapse, a reset) is read on a grid nothing holds, so the hunt's crossing loop
   walks it over the word. Measured against the centres it recorded, the fit reads the walk (a sample and more early),
   not the grid the frame goes on with. A P25 word is measured against the grid without the walk, ending at its last
   centre: the move centres the frame, either polarity, at whole and fractional rates (SPS 5's measurement reaches one
   sample either way). Every other caller's word keeps its recorded centres (issue #834's measurement), whatever
   lastsynctype says: M17 names its word before its accept sets lastsynctype, which a P25 frame may have left. */
static void
test_p25_word_read_on_a_walking_grid(void) {
    static const struct {
        int whole;
        int rem;
        int den;
        int reach;
    } k_rates[] = {{10, 0, 0, 2}, {10, 2000, 4800, 2}, {20, 0, 0, 2}, {5, 0, 0, 1}};

    int differs = 0;
    for (size_t r = 0; r < sizeof(k_rates) / sizeof(k_rates[0]); r++) {
        for (int offset = -k_rates[r].reach; offset <= k_rates[r].reach; offset++) {
            for (int polarity = -1; polarity <= 1; polarity += 2) {
                p25_word_case c = {k_rates[r].whole,
                                   k_rates[r].rem,
                                   k_rates[r].den,
                                   0.0,
                                   0,
                                   (double)offset,
                                   (double)polarity,
                                   DSD_SYNC_P25P1_POS,
                                   1,
                                   k_p25_walk,
                                   0};
                const int slip = run_p25_word(&c);
                if (slip != offset) {
                    DSD_FPRINTF(stderr, "P25 word on a walking grid, %d+%d/%d sps, offset %d, polarity %d: moved %d\n",
                                k_rates[r].whole, k_rates[r].rem, k_rates[r].den, offset, polarity, slip);
                }
                assert(slip == offset);
                /* The same samples named by another caller measure on the recorded centres, after a P25 frame (an M17
                   word) as after an NXDN one. */
                c.plain = 1;
                const int after_p25 = run_p25_word(&c);
                c.lastsynctype = DSD_SYNC_NXDN_POS;
                const int after_nxdn = run_p25_word(&c);
                assert(after_p25 == after_nxdn);
                differs += after_p25 != offset;
            }
        }
    }
    assert(differs > 0);
}

int
main(void) {
    test_measures_whole_and_fractional_offsets();
    test_inverted_signal_measures_the_same();
    test_noise_does_not_move_a_centred_grid();
    test_declines();
    test_ring_queues_the_move_toward_the_symbols();
    test_reset_drops_the_pending_move();
    test_switch_measures_without_a_look_ahead_symbol();
    test_switch_without_a_named_word_moves_nothing();
    test_words_with_inner_symbols_measure();
    test_hunt_recording_holds_only_a_placed_grid();
    test_unrecorded_word_unplaces_the_grid();
    test_hold_ends_at_another_protocols_sync();
    test_p25_hold_is_its_own();
    test_p25_word_measures_the_frame_grid();
    test_p25_word_named_from_its_second_symbol();
    test_p25_word_read_on_a_walking_grid();
    printf("symbol anchor: OK\n");
    return 0;
}
