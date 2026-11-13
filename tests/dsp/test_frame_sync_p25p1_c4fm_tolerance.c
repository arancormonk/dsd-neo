// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 1 sync words a few symbol signs off on the C4FM chain (dsd_p25p1_sync_kind).
 *
 * The C4FM hunt matched the 24-symbol word exactly, so one symbol past the zero line lost the whole frame. After a
 * validated frame it now accepts a word within DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS sign errors where that frame
 * ends (the anchor). Such a candidate is held until its NID speaks: the levels its accept warm-starts the NID on, the
 * carrier flag, offset and lastsynctype, and the P25p1 frame span that stands NXDN96 and M17 down all go back when the
 * dispatcher drops it, and the hunt goes on from the symbols the word and its NID took, so a real sync word the
 * candidate swallowed whose end is still to come is matched where it is.
 *
 * The windows go through the real matchers (dsd_frame_sync_test_try_protocol_matches()), and the streams through the
 * real getFrameSync() over a symbol source standing in for dsd_symbol.c's read (as FRAME_SYNC_HISTORY_CARRY does). The
 * dispatcher is played by dispatch(): it reads the NID's symbols and, as dsd_dispatch_handle_p25p1() does, reports the
 * frame validated or, behind a tolerant word whose NID failed, drops it. The input is discriminator PCM, and nothing
 * here needs the radio build.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"
#include "frame_sync_test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

enum {
    SYNC = 24,
    NID = 33, /* the NID's 32 dibits and the status symbol after its 11th */
    HEAD = SYNC + NID,
    NAC = 0x1A6,
    TSDU_1 = 180,
    TSDU_2 = 288,
    PAD = 22, /* a one-block TSDU's decoder stops before the null pad of its last segment and the status symbol */
    STREAM_LEN = 8000,
};

/* The level each symbol of the stream is read at; past its end, data. */
static float g_levels[STREAM_LEN];
static int g_len;
static int g_pos;
static uint32_t g_seed;

/* The level a C4FM word's symbol is sent at: off the data's four levels, so a warm start from it shows. */
static const float k_word_level = 2.5f;

static float
data_level(void) {
    static const float levels[4] = {3.0f, 1.0f, -1.0f, -3.0f};
    g_seed = (g_seed * 1103515245U) + 12345U;
    return levels[(g_seed >> 16) & 3U];
}

/* What dsd_symbol.c's read leaves behind: the soft history, the symbol count and the stream's symbol clock. */
float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;
    const float symbol = g_pos < g_len ? g_levels[g_pos] : data_level();
    g_pos++;
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    state->symbol_clock++;
    return symbol;
}

static void
stream_reset(uint32_t seed) {
    g_len = 0;
    g_pos = 0;
    g_seed = seed;
}

static void
append_data(int count) {
    for (int i = 0; i < count && g_len < STREAM_LEN; i++) {
        g_levels[g_len++] = data_level();
    }
}

/* Whether @p flips (-1 ends it) lists symbol @p at. */
static int
flipped(const int* flips, int at) {
    for (int i = 0; flips != NULL && flips[i] >= 0; i++) {
        if (flips[i] == at) {
            return 1;
        }
    }
    return 0;
}

/* @p word ('1' positive, '3' negative, as the C4FM hunt reads signs) with the sign of the symbols @p flips lists
   turned over. */
static void
append_signs(const char* word, const int* flips) {
    for (int i = 0; word[i] != '\0' && g_len < STREAM_LEN; i++) {
        const float level = word[i] == '1' ? k_word_level : -k_word_level;
        g_levels[g_len++] = flipped(flips, i) ? -level : level;
    }
}

/* The first @p count symbols a word sign errors put in, spread over it. */
static const int k_flips[][8] = {
    {-1},
    {3, -1},
    {3, 11, -1},
    {3, 11, 17, -1},
    {3, 11, 17, 6, -1},
    {3, 11, 17, 6, 20, -1},
    {3, 11, 17, 6, 20, 0, -1},
    {3, 11, 17, 6, 20, 0, 14, -1},
};

/* The window @p word with @p count sign errors, as a string. */
static void
word_with_sign_errors(const char* word, int count, char out[SYNC + 1]) {
    DSD_MEMCPY(out, word, SYNC + 1);
    for (int i = 0; k_flips[count][i] >= 0; i++) {
        const int at = k_flips[count][i];
        out[at] = out[at] == '1' ? '3' : '1';
    }
}

/* -f1 on discriminator PCM: P25 Phase 1 alone on the C4FM chain's 4800/4 hunt (opts->frame_* as the caller adds). */
static void
setup(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(init_state_buffers(state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->frame_p25p1 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_c4fm = 1;
    opts->msize = 15;
    opts->ssize = 36;
    state->rf_mod = 0;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
    state->lastsynctype = DSD_SYNC_NONE;
    dsd_frame_sync_reset_mod_state();
}

static void
teardown(dsd_state* state) {
    free_state_buffers(state);
}

/* The window @p window as the one the hunt reads when its first symbol is @p start on the symbol clock. */
static int
match_at(dsd_opts* opts, dsd_state* state, const char* window, uint64_t start) {
    state->symbol_clock = start + (uint64_t)strlen(window);
    return dsd_frame_sync_test_try_protocol_matches(opts, state, window, (int)strlen(window));
}

/* An exact word at @p sync_start whose frame of @p unit dibits validated naming NAC, its decoder having read
   @p consumed of them: the anchor waits at sync_start + unit. */
static void
anchored_at(dsd_opts* opts, dsd_state* state, uint64_t sync_start, int inverted, uint32_t unit, uint32_t consumed) {
    const int want = inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
    assert(match_at(opts, state, inverted ? INV_P25P1_SYNC : P25P1_SYNC, sync_start) == want);
    assert(dsd_frame_sync_p25p1_sync_kind(state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
    state->symbol_clock = sync_start + consumed;
    (void)dsd_frame_sync_p25p1_frame_decoded(state, NAC, unit);
    assert(state->p25_p1_sync_anchor.valid == 1U);
    assert(state->p25_p1_sync_anchor.expected_clock == sync_start + unit);
}

/* At the expected position a word 6 signs off is accepted, of either polarity, one 7 off is not; either way the anchor
   is spent. The frame behind the accepted one is an anchored candidate, its NID bound to the anchor's NAC. */
static void
test_anchored_accepts_at_t_refuses_past_it(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC + 1];
    for (int inverted = 0; inverted <= 1; inverted++) {
        const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
        const int want = inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;

        setup(&opts, &state);
        anchored_at(&opts, &state, 1000U, inverted, TSDU_1, TSDU_1 - PAD);
        word_with_sign_errors(word, DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS, window);
        assert(dsd_p25p1_sync_sign_distance(window, inverted) == DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS);
        assert(match_at(&opts, &state, window, 1000U + TSDU_1) == want);
        int nac = -1;
        unsigned int errors = 0U;
        assert(dsd_frame_sync_p25p1_sync_kind(&state, &nac, &errors) == DSD_P25P1_SYNC_ANCHORED);
        assert(nac == NAC && errors == DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS);
        assert(state.p25_p1_sync_anchor.tolerant_syncs == 1U);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        assert(state.p25_p1_sync_anchor.frame.sync_clock == 1000U + TSDU_1);
        assert(state.p25_p1_sync_candidate.valid == 1U);
        teardown(&state);

        setup(&opts, &state);
        anchored_at(&opts, &state, 1000U, inverted, TSDU_1, TSDU_1 - PAD);
        word_with_sign_errors(word, DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS + 1, window);
        assert(match_at(&opts, &state, window, 1000U + TSDU_1) == DSD_SYNC_NONE);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
        /* Spent: the same word two signs off (past the hunt-wide tolerance) a symbol later is not scored. */
        word_with_sign_errors(word, 2, window);
        assert(match_at(&opts, &state, window, 1000U + TSDU_1 + 1U) == DSD_SYNC_NONE);
        teardown(&state);
    }
    /* The other polarity's word is 24 signs off the anchor's: never within it. */
    setup(&opts, &state);
    anchored_at(&opts, &state, 1000U, 0, TSDU_1, TSDU_1 - PAD);
    assert(match_at(&opts, &state, INV_P25P1_SYNC, 1000U + TSDU_1) == DSD_SYNC_P25P1_NEG);
    assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
    teardown(&state);
    /* No anchor: two signs off are lost, as before. */
    setup(&opts, &state);
    word_with_sign_errors(P25P1_SYNC, 2, window);
    assert(match_at(&opts, &state, window, 1000U) == DSD_SYNC_NONE);
    teardown(&state);
}

/* The levels a candidate's accept is to be judged against: off the warm start's, the windows marked. */
static void
mark_levels(dsd_state* state) {
    state->max = 7.0f;
    state->min = -7.0f;
    state->center = 0.25f;
    state->umid = 4.0f;
    state->lmid = -4.0f;
    state->maxref = 5.6f;
    state->minref = -5.6f;
    for (int i = 0; i < 1024; i++) {
        state->maxbuf[i] = 7.0f + (float)i * 0.001f;
        state->minbuf[i] = -7.0f;
    }
    state->midx = 5;
}

static int
levels_marked(const dsd_state* state) {
    int ok = fabsf(state->max - 7.0f) < 1e-6f && fabsf(state->min + 7.0f) < 1e-6f
             && fabsf(state->center - 0.25f) < 1e-6f && fabsf(state->umid - 4.0f) < 1e-6f
             && fabsf(state->lmid + 4.0f) < 1e-6f && fabsf(state->maxref - 5.6f) < 1e-6f
             && fabsf(state->minref + 5.6f) < 1e-6f && state->midx == 5;
    for (int i = 0; i < 1024 && ok; i++) {
        ok = fabsf(state->maxbuf[i] - (7.0f + (float)i * 0.001f)) < 1e-6f && fabsf(state->minbuf[i] + 7.0f) < 1e-6f;
    }
    return ok;
}

/* The rest of what a candidate's accept and NID read write before the NID speaks, set apart: a DMR slot 2 burst with
   an ALG ID (a DMR carrier under -fa), the frame label, a pending crossing, a CQPSK map, a frame NAC, and no levels
   from a sync word (which under -f1 would hold the candidate's through the frames after it). */
static void
mark_slot_fields(dsd_state* state) {
    state->dmrburstR = 9U;
    state->payload_algidR = 0x84;
    state->dmr_stereo = 0;
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), "%s", "DMR");
    state->jitter = 3;
    state->p25_cqpsk_dibit_map_idx = 2U;
    state->p25_frame_nac_valid = 1U;
    state->p25_p1_c4fm_sync_levels = 0U;
}

static int
slot_fields_marked(const dsd_state* state) {
    return state->dmrburstR == 9U && state->payload_algidR == 0x84 && state->dmr_stereo == 0
           && strcmp(state->ftype, "DMR") == 0 && state->jitter == 3 && state->p25_cqpsk_dibit_map_idx == 2U
           && state->p25_frame_nac_valid == 1U && state->p25_p1_c4fm_sync_levels == 0U;
}

/* The symbols of @p window, at the word's levels, into the soft history the warm start reads. */
static void
push_window_levels(dsd_state* state, const char* window) {
    for (size_t i = 0; window[i] != '\0'; i++) {
        dsd_symbol_history_push(state, window[i] == '1' ? k_word_level : -k_word_level);
    }
}

/* An anchored candidate whose NID failed: everything its accept wrote before the NID spoke goes back. Its frame span
   no longer stands NXDN96 or M17 down: a real NXDN96 transmission, and a real M17 one, a P25p1 frame span after the
   anchor's frame (inside the candidate's) are matched. A candidate whose NID validated keeps what it wrote. */
static void
anchored_candidate(dsd_opts* opts, dsd_state* state, int extra_protocol) {
    char window[SYNC + 1];
    setup(opts, state);
    if (extra_protocol == 1) {
        opts->frame_nxdn96 = 1;
    } else if (extra_protocol == 2) {
        opts->frame_m17 = 1;
    }
    state->symbolcnt = 5000U;
    anchored_at(opts, state, 1000U, 0, TSDU_1, TSDU_1 - PAD);
    assert(state->p25_p1_c4fm_frame_valid == 1 && state->p25_p1_c4fm_frame_symbolcnt == 5000U);
    /* What stood before the candidate, set apart from what its accept writes. */
    mark_levels(state);
    state->carrier = 0;
    state->offset = 77;
    state->lastsynctype = DSD_SYNC_NONE;
    mark_slot_fields(state);
    state->symbolcnt = 5000U + TSDU_1;
    word_with_sign_errors(P25P1_SYNC, 4, window);
    push_window_levels(state, window);
    assert(match_at(opts, state, window, 1000U + TSDU_1) == DSD_SYNC_P25P1_POS);
    /* The NID is read on the word's warm start, in the word's frame span. */
    assert(fabsf(state->max - k_word_level) < 1e-4f && fabsf(state->min + k_word_level) < 1e-4f);
    assert(state->carrier == 1 && state->lastsynctype == DSD_SYNC_P25P1_POS);
    assert(state->p25_p1_c4fm_frame_valid == 1 && state->p25_p1_c4fm_frame_symbolcnt == 5000U + TSDU_1);
    /* The accept wrote its own: the P25 frame label and slot 2 state, the warm start's crossing reset and its levels'
       flag, the map. */
    assert(!slot_fields_marked(state) && state->jitter == -1 && strcmp(state->ftype, "P25 Phase 1") == 0);
    assert(state->p25_p1_c4fm_sync_levels == 1U);
    /* The dispatcher reads the NID, the frame NAC cleared first. */
    state->p25_frame_nac_valid = 0U;
    for (int i = 0; i < NID; i++) {
        dsd_symbol_history_push(state, (i & 1) ? 1.0f : -3.0f);
    }
    state->symbol_clock += NID;
    state->symbolcnt += NID;
}

static void
dispatcher_drops(dsd_state* state) {
    assert(dsd_frame_sync_p25p1_sync_kind(state, NULL, NULL) == DSD_P25P1_SYNC_ANCHORED);
    dsd_frame_sync_p25p1_candidate_rejected(state);
    (void)dsd_frame_sync_p25p1_frame_decoded(state, -1, 0U);
}

static void
test_rejected_candidate_puts_back_what_it_changed(void) {
    static dsd_opts opts;
    static dsd_state state;

    anchored_candidate(&opts, &state, 0);
    dispatcher_drops(&state);
    assert(levels_marked(&state));
    assert(state.carrier == 0 && state.offset == 77 && state.lastsynctype == DSD_SYNC_NONE);
    assert(slot_fields_marked(&state));
    assert(state.p25_p1_c4fm_frame_valid == 1 && state.p25_p1_c4fm_frame_symbolcnt == 5000U);
    assert(state.p25_p1_sync_candidate.valid == 0U);
    assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
    /* The hunt goes on from the word's and the NID's symbols. */
    assert(state.frame_sync_carry.valid == 1 && state.frame_sync_carry.count == HEAD);
    assert(state.frame_sync_carry.symbol_clock == state.symbol_clock);
    /* As the hunt's own symbols, in the pass the window's last symbol was read in. */
    assert(state.frame_sync_carry.pass_symbols == SYNC + NID);
    /* It synced nothing a scanner counts, until the next accepted sync. */
    assert(dsd_state_p25p1_frame_dropped(&state));
    assert(match_at(&opts, &state, P25P1_SYNC, 3000U) == DSD_SYNC_P25P1_POS);
    assert(!dsd_state_p25p1_frame_dropped(&state));
    teardown(&state);

    /* Validated: the frame keeps the calibration and the span its word opened. */
    anchored_candidate(&opts, &state, 0);
    (void)dsd_frame_sync_p25p1_frame_decoded(&state, NAC, TSDU_1);
    assert(fabsf(state.max - k_word_level) < 1e-4f && state.p25_p1_c4fm_sync_levels == 1U);
    assert(state.carrier == 1 && state.lastsynctype == DSD_SYNC_P25P1_POS);
    assert(state.p25_p1_c4fm_frame_symbolcnt == 5000U + TSDU_1);
    assert(state.p25_p1_sync_candidate.valid == 0U);
    assert(state.p25_p1_sync_anchor.tolerant_frames == 1U);
    teardown(&state);

    /* Off the stream the word was accepted on, nothing of the old one goes back. */
    anchored_candidate(&opts, &state, 0);
    state.carrier_seq++;
    dispatcher_drops(&state);
    assert(fabsf(state.max - k_word_level) < 1e-4f);
    assert(state.lastsynctype == DSD_SYNC_P25P1_POS);
    assert(state.frame_sync_carry.valid == 0);
    teardown(&state);
}

static void
test_rejected_candidate_lets_nxdn96_and_m17_in(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* NXDN96, two hits a frame apart, past the anchor's frame span but inside the candidate's. */
    anchored_candidate(&opts, &state, 1);
    dispatcher_drops(&state);
    state.symbolcnt = 5000U + DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
    teardown(&state);

    /* M17: a preamble run, then the link setup frame's word. */
    anchored_candidate(&opts, &state, 2);
    dispatcher_drops(&state);
    state.symbolcnt = 5000U + DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS;
    for (int i = 0; i < DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS; i++) {
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_PRE, 8) == DSD_SYNC_NONE);
    }
    assert(state.m17_pre_candidate != 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_M17_LSF_POS);
    teardown(&state);
}

/* The engine's turn at a sync, as dsd_dispatch_handle_p25p1() takes it: the NID's symbols read; when @p nid_ok, the
   frame validated, its decoder having read @p consumed of its @p unit dibits; else (behind a tolerant word) dropped.
   The verdict goes where processFrame() leaves it for the hunt's accounting. */
static void
dispatch(dsd_state* state, int nid_ok, uint32_t unit, uint32_t consumed) {
    static dsd_opts reader;
    const dsd_p25p1_sync_kind kind = dsd_frame_sync_p25p1_sync_kind(state, NULL, NULL);
    for (int i = 0; i < NID; i++) {
        (void)getSymbol(&reader, state, 1);
    }
    if (!nid_ok) {
        assert(kind != DSD_P25P1_SYNC_EXACT);
        dsd_frame_sync_p25p1_candidate_rejected(state);
        (void)dsd_frame_sync_p25p1_frame_decoded(state, -1, 0U);
        state->sps_hunt_last_frame_verdict = (int)DSD_FRAME_VERDICT_UNPRODUCTIVE;
        return;
    }
    for (uint32_t i = HEAD; i < consumed; i++) {
        (void)getSymbol(&reader, state, 1);
    }
    (void)dsd_frame_sync_p25p1_frame_decoded(state, NAC, unit);
    state->sps_hunt_last_frame_verdict = (int)DSD_FRAME_VERDICT_PROFILE_PROVEN;
}

/* getFrameSync() calls until a sync or the stream's end and @p tail symbols past it. */
static int
next_sync(dsd_opts* opts, dsd_state* state, int tail) {
    while (g_pos < g_len + tail) {
        const int sync = getFrameSync(opts, state);
        if (sync > DSD_SYNC_NONE) {
            return sync;
        }
    }
    return DSD_SYNC_NONE;
}

/* Where the frame the last sync opened starts on the stream. */
static int
sync_start(const dsd_state* state) {
    return (int)state->p25_p1_sync_anchor.frame.sync_clock;
}

/* After a validated one- or two-block TSDU (its decoder stopped before the null pad, which the hunt reads), the word
   at the expected position comes through a few signs off. The candidate is accepted there, the NID read on the
   warm start from the word's own symbols (taken as the window ends, not the NID's after it), and either kept or put
   back by what the NID says. */
static void
test_tolerant_sync_after_tsdus(void) {
    static const struct {
        uint32_t unit;
        uint32_t consumed;
    } units[] = {{TSDU_1, TSDU_1 - PAD}, {TSDU_2, TSDU_2 - PAD}, {TSDU_2, TSDU_2}};

    for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
        for (int nid_ok = 0; nid_ok <= 1; nid_ok++) {
            static dsd_opts opts;
            static dsd_state state;
            setup(&opts, &state);
            stream_reset(0x7151U + (uint32_t)u);
            append_data(300);
            append_signs(P25P1_SYNC, NULL);
            append_data((int)units[u].unit - SYNC);
            const int expected = g_len;
            append_signs(P25P1_SYNC, k_flips[5]);
            append_data(TSDU_1 - SYNC);
            append_signs(P25P1_SYNC, NULL);
            append_data(400);

            assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS && sync_start(&state) == 300);
            dispatch(&state, 1, units[u].unit, units[u].consumed);
            const float before_max = state.max;
            assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS);
            assert(sync_start(&state) == expected && g_pos == expected + SYNC);
            unsigned int errors = 0U;
            assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, &errors) == DSD_P25P1_SYNC_ANCHORED && errors == 5U);
            assert(fabsf(state.max - k_word_level) < 1e-4f && fabsf(state.min + k_word_level) < 1e-4f);
            dispatch(&state, nid_ok, TSDU_1, TSDU_1 - PAD);
            if (nid_ok) {
                assert(fabsf(state.max - k_word_level) < 1e-4f);
            } else {
                assert(fabsf(state.max - before_max) < 1e-6f);
            }
            /* The unit after it: exact, anchored behind the validated candidate or found by the hunt. */
            assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS && sync_start(&state) == expected + TSDU_1);
            assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
            teardown(&state);
        }
    }
}

/* A candidate at the expected position that is no word (its NID fails), with a real sync word starting inside the
   symbols its NID read and ending after them: a P25p1 word a slip put there, or another protocol's (YSF). The hunt goes
   on from the candidate's symbols, so the real word is matched where it ends; started from nothing, it was lost. */
static void
test_rejected_candidate_does_not_swallow_a_later_word(void) {
    for (int ysf = 0; ysf <= 1; ysf++) {
        /* From the first start whose word ends on the first symbol past the NID to the last one inside the NID. */
        const int length = ysf ? 20 : SYNC;
        const int starts[] = {HEAD - length + 1, SYNC + 20, HEAD - 1};
        for (size_t s = 0; s < sizeof(starts) / sizeof(starts[0]); s++) {
            static dsd_opts opts;
            static dsd_state state;
            setup(&opts, &state);
            opts.frame_ysf = ysf;
            stream_reset(0xA11U + (uint32_t)s);
            append_data(300);
            append_signs(P25P1_SYNC, NULL);
            append_data(TSDU_1 - SYNC);
            const int expected = g_len;
            append_signs(P25P1_SYNC, k_flips[6]);
            append_data(starts[s] - SYNC);
            const int real = g_len;
            append_signs(ysf ? FUSION_SYNC : P25P1_SYNC, NULL);
            append_data(600);

            assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS && sync_start(&state) == 300);
            dispatch(&state, 1, TSDU_1, TSDU_1 - PAD);
            assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS && sync_start(&state) == expected);
            dispatch(&state, 0, 0U, 0U);
            assert(g_pos == expected + HEAD);
            const int sync = next_sync(&opts, &state, 0);
            if (ysf) {
                assert(sync == DSD_SYNC_YSF_POS);
                assert(g_pos == real + 20);
            } else {
                assert(sync == DSD_SYNC_P25P1_POS && sync_start(&state) == real);
                assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
            }
            teardown(&state);
        }
    }
}

/* With no anchor, anywhere on the hunt: a word one sign off is an acquisition candidate, of either polarity, wherever
   the error falls but on its last symbol (which makes its tail the head of a word starting 23 symbols on); two signs off
   is not. Only on the C4FM chain's 4800/4 hunt with Phase 1 on. */
static void
test_acquisition_accepts_one_sign_refuses_two(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC + 1];
    for (int inverted = 0; inverted <= 1; inverted++) {
        const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
        const int want = inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
        for (int at = 0; at < SYNC; at++) {
            setup(&opts, &state);
            DSD_MEMCPY(window, word, sizeof(window));
            window[at] = window[at] == '1' ? '3' : '1';
            const int sync = match_at(&opts, &state, window, 2000U);
            if (at == SYNC - 1) {
                assert(sync == DSD_SYNC_NONE);
                assert(state.p25_p1_sync_anchor.acquisition_syncs == 0U);
            } else {
                assert(sync == want);
                int nac = 0;
                unsigned int errors = 0U;
                assert(dsd_frame_sync_p25p1_sync_kind(&state, &nac, &errors) == DSD_P25P1_SYNC_ACQUISITION);
                assert(nac == -1 && errors == 1U);
                assert(state.p25_p1_sync_anchor.acquisition_syncs == 1U);
                assert(state.p25_p1_sync_anchor.frame.sync_clock == 2000U);
                assert(state.p25_p1_sync_candidate.valid == 1U);
            }
            teardown(&state);
        }
        setup(&opts, &state);
        word_with_sign_errors(word, 2, window);
        assert(match_at(&opts, &state, window, 2000U) == DSD_SYNC_NONE);
        teardown(&state);
    }
    /* Not on the CQPSK or GFSK chain, not on another profile, not with Phase 1 off. */
    word_with_sign_errors(P25P1_SYNC, 1, window);
    for (int c = 0; c < 4; c++) {
        setup(&opts, &state);
        if (c == 0) {
            state.rf_mod = 1;
        } else if (c == 1) {
            state.rf_mod = 2;
        } else if (c == 2) {
            state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
        } else {
            opts.frame_p25p1 = 0;
        }
        assert(match_at(&opts, &state, window, 2000U) == DSD_SYNC_NONE);
        teardown(&state);
    }
}

/* A steal by overlap: the word's first 23 symbols, then the word. The first window is the word one sign off (its
   last symbol is the real word's first): taken, its NID read would swallow the real word, which ends inside it. It is
   refused, and the real word is matched exactly where it is, either polarity. */
static void
test_acquisition_does_not_steal_an_overlapping_word(void) {
    for (int inverted = 0; inverted <= 1; inverted++) {
        static dsd_opts opts;
        static dsd_state state;
        const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
        char head[SYNC];
        DSD_MEMCPY(head, word, SYNC - 1);
        head[SYNC - 1] = '\0';
        setup(&opts, &state);
        stream_reset(0x5EA1U);
        append_data(200);
        append_signs(head, NULL);
        const int real = g_len;
        append_signs(word, NULL);
        append_data(400);
        assert(next_sync(&opts, &state, 0) == (inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS));
        assert(sync_start(&state) == real);
        assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
        assert(state.p25_p1_sync_anchor.acquisition_syncs == 0U);
        teardown(&state);
    }
}

/* A carrier's first word comes through one sign off (cold: no anchor, no levels calibrated yet). It is an acquisition
   candidate: its NID is read on the warm start from its own symbols. A strict NID keeps that and counts the frame; a
   failed one puts the hunt's levels and lock back and the hunt goes on from the candidate's symbols, finding the next
   word exactly. */
static void
test_cold_acquisition(void) {
    for (int nid_ok = 0; nid_ok <= 1; nid_ok++) {
        for (int inverted = 0; inverted <= 1; inverted++) {
            static dsd_opts opts;
            static dsd_state state;
            const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
            const int want = inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS;
            static const int flip[] = {9, -1};
            setup(&opts, &state);
            /* Cold: the levels an acquisition reset leaves, nothing calibrated yet. */
            dsd_frame_sync_reset_acquisition(&opts, &state, 1);
            assert(fabsf(state.max - 15000.0f) < 1e-3f && fabsf(state.min + 15000.0f) < 1e-3f);
            stream_reset(0xC01DU + (uint32_t)inverted);
            append_data(500);
            const int first = g_len;
            append_signs(word, flip);
            append_data(TSDU_1 - SYNC);
            append_signs(word, NULL);
            append_data(400);
            assert(next_sync(&opts, &state, 0) == want && sync_start(&state) == first);
            unsigned int errors = 0U;
            assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, &errors) == DSD_P25P1_SYNC_ACQUISITION && errors == 1U);
            assert(fabsf(state.max - k_word_level) < 1e-4f && fabsf(state.min + k_word_level) < 1e-4f);
            assert(state.carrier == 1 && state.lastsynctype == want);
            dispatch(&state, nid_ok, TSDU_1, TSDU_1 - PAD);
            if (nid_ok) {
                assert(state.p25_p1_sync_anchor.acquisition_frames == 1U);
                assert(fabsf(state.max - k_word_level) < 1e-4f);
            } else {
                assert(state.p25_p1_sync_anchor.acquisition_frames == 0U);
                assert(state.carrier == 0 && state.lastsynctype == DSD_SYNC_NONE);
                /* Uncalibrated again, as before the candidate. */
                assert(fabsf(state.max - 15000.0f) < 1e-3f && fabsf(state.min + 15000.0f) < 1e-3f);
                assert(fabsf(state.umid - 9375.0f) < 1e-3f && fabsf(state.lmid + 9375.0f) < 1e-3f);
            }
            assert(next_sync(&opts, &state, 0) == want && sync_start(&state) == first + TSDU_1);
            assert(dsd_frame_sync_p25p1_sync_kind(&state, NULL, NULL) == DSD_P25P1_SYNC_EXACT);
            teardown(&state);
        }
    }
}

/* Where each getFrameSync() call over the rest of the stream ends without a sync (the symbol it ended on), and the
   profile's hunt budget then, after a validated TSDU whose successor's word comes through @p errors signs off. */
static void
no_sync_exits(int errors, int* out_ends, int* out_budget, int count) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    stream_reset(0xD0E11U);
    append_data(300);
    append_signs(P25P1_SYNC, NULL);
    append_data(TSDU_1 - SYNC);
    append_signs(P25P1_SYNC, k_flips[errors]);
    append_data(6000);
    assert(next_sync(&opts, &state, 0) == DSD_SYNC_P25P1_POS);
    dispatch(&state, 1, TSDU_1, TSDU_1 - PAD);
    int n = 0;
    while (n < count) {
        const int sync = getFrameSync(&opts, &state);
        if (sync > DSD_SYNC_NONE) {
            /* The candidate at the expected position: its NID fails. */
            assert(errors <= DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS);
            dispatch(&state, 0, 0U, 0U);
            continue;
        }
        out_ends[n] = g_pos;
        out_budget[n] = state.sps_hunt_counter;
        n++;
    }
    teardown(&state);
}

/* A dropped candidate leaves the hunt's timing as it was: its NID's symbols are charged to the profile's dwell as the
   hunt's own and the pass goes on past them, so every no-sync exit after it falls on the symbol it falls on when the
   word was too far off to be a candidate at all. */
static void
test_dropped_candidate_keeps_the_hunt_timing(void) {
    int with[3];
    int without[3];
    int budget_with[3];
    int budget_without[3];
    no_sync_exits(DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS, with, budget_with, 3);
    no_sync_exits(DSD_P25P1_C4FM_ANCHORED_MAX_SIGN_ERRORS + 1, without, budget_without, 3);
    for (int i = 0; i < 3; i++) {
        assert(with[i] == without[i]);
        assert(budget_with[i] == budget_without[i]);
    }
}

int
main(void) {
    test_anchored_accepts_at_t_refuses_past_it();
    test_rejected_candidate_puts_back_what_it_changed();
    test_rejected_candidate_lets_nxdn96_and_m17_in();
    test_tolerant_sync_after_tsdus();
    test_rejected_candidate_does_not_swallow_a_later_word();
    test_dropped_candidate_keeps_the_hunt_timing();
    test_acquisition_accepts_one_sign_refuses_two();
    test_acquisition_does_not_steal_an_overlapping_word();
    test_cold_acquisition();
    DSD_FPRINTF(stderr, "P25 P1 C4FM tolerance: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
