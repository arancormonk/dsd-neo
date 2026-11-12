// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 Phase 2 sync anchor (dsd_p25p2_sync_anchor): after a window whose own I-ISCH placed it, the hunt accepts a
 * sync word within DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS bit errors where the next window's sync must be, and only
 * there, on the RTL CQPSK 6000/4 path, on the stream the window was read on. The windows go through the real matchers
 * (dsd_frame_sync_test_try_protocol_matches()), each read as the window ending on dsd_state::symbol_clock, as the hunt
 * reads them; the anchor is installed through processP2()'s entry point (dsd_frame_sync_p25p2_window_decoded()).
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p2_sync_anchor.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"
#include "frame_sync_test_support.h"
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif

enum {
    SYNC_DIBITS = 20,
    WINDOW_DIBITS = 700,
    ANCHOR_SYNC = 1000, /* the placed window's sync starts here on the symbol clock */
    ANCHOR_START = 2,   /* and the window at superframe timeslot 2 */
    NEXT_START = 6,     /* so the next window starts at 6 */
    EXPECTED = ANCHOR_SYNC + SYNC_DIBITS + WINDOW_DIBITS,
    TIMESLOT_DIBITS = 180,
};

/* The P25p2 sync word with the low bit of @p bits dibits flipped (one level off, one bit each). */
static void
word_with_bit_errors(const char* word, int bits, char out[SYNC_DIBITS + 1]) {
    static const int positions[] = {0, 4, 8, 12, 16, 2, 6, 10, 14};
    DSD_MEMCPY(out, word, SYNC_DIBITS + 1);
    for (int i = 0; i < bits; i++) {
        out[positions[i]] = (char)('0' + ((out[positions[i]] - '0') ^ 1));
    }
}

/* The word shifted @p k dibits later (@p late) or earlier, the dibits shifted in matching the word itself: the best a
   misplaced word can do against the expected-position window. */
static void
shifted_word(int k, int late, char out[SYNC_DIBITS + 1]) {
    k = k < 0 ? 0 : (k > SYNC_DIBITS ? SYNC_DIBITS : k);
    for (int i = 0; i < SYNC_DIBITS; i++) {
        if (late) {
            out[i] = i < k ? P25P2_SYNC[i] : P25P2_SYNC[i - k];
        } else {
            out[i] = i + k < SYNC_DIBITS ? P25P2_SYNC[i + k] : P25P2_SYNC[i];
        }
    }
    out[SYNC_DIBITS] = '\0';
}

/* The distance counts bits, under the map and polarity asked for; the shifted word's distance to itself is the bound the
   threshold relies on (design §0: 7, 8, 7, 7, 6 for 1 to 5 symbols, at least 5 up to 12). */
static void
test_bit_distance(void) {
    char window[SYNC_DIBITS + 1];
    assert(dsd_p25p2_sync_bit_distance(P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 0);
    assert(dsd_p25p2_sync_bit_distance(INV_P25P2_SYNC, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 1) == 0);
    for (int bits = 0; bits <= 8; bits++) {
        word_with_bit_errors(P25P2_SYNC, bits, window);
        assert(dsd_p25p2_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == bits);
    }
    static const uint8_t maps[] = {DSD_P25_CQPSK_DIBIT_MAP_X2400, DSD_P25_CQPSK_DIBIT_MAP_N1200,
                                   DSD_P25_CQPSK_DIBIT_MAP_P1200};
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int inverted = 0; inverted <= 1; inverted++) {
            char corrected[SYNC_DIBITS + 1];
            word_with_bit_errors(inverted ? INV_P25P2_SYNC : P25P2_SYNC, DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS,
                                 corrected);
            for (int i = 0; i < SYNC_DIBITS; i++) {
                window[i] = (char)('0' + dsd_p25_cqpsk_raw_dibit_for_corrected(maps[m], (uint8_t)(corrected[i] - '0')));
            }
            window[SYNC_DIBITS] = '\0';
            assert(dsd_p25p2_sync_bit_distance(window, maps[m], inverted) == DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS);
        }
    }
    assert(dsd_p25p2_sync_bit_distance("1111", DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 40);
    assert(dsd_p25p2_sync_bit_distance(NULL, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 40);

    static const int late_bound[5] = {7, 8, 7, 7, 6};
    for (int k = 1; k <= 12; k++) {
        for (int late = 0; late <= 1; late++) {
            shifted_word(k, late, window);
            const int d = dsd_p25p2_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0);
            assert(d > DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS);
            if (k <= 5) {
                assert(d == late_bound[k - 1]);
            }
        }
    }
}

#ifdef USE_RADIO
static int
match_at(dsd_opts* opts, dsd_state* state, const char* window, uint64_t start) {
    state->symbol_clock = start + SYNC_DIBITS;
    return dsd_frame_sync_test_try_protocol_matches(opts, state, window, SYNC_DIBITS);
}

static int
cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    *out_cqpsk_enable = 1;
    *out_cqpsk_timing_active = 1;
    return 0;
}

/* The RTL CQPSK hunt on 6000/4, Phase 2 only, the modulation locked so no vote moves it. */
static void
setup(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(init_state_buffers(state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_p25p2 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_qpsk = 1;
    opts->msize = 1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state->min = -3.0f;
    state->max = 3.0f;
    /* The site's scrambling parameters, which an accepted sync reports. */
    state->p2_wacn = 0xBEE00;
    state->p2_sysid = 0x164;
    state->p2_cc = 0x161;
    dsd_rtl_stream_metrics_hooks hooks = {
        .cqpsk_status = cqpsk_status,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

static void
teardown(dsd_state* state) {
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free_state_buffers(state);
}

/* A window whose sync started at @p sync_start, read whole, reported placed at @p start. */
static void
placed_window(dsd_opts* opts, dsd_state* state, uint64_t sync_start, int start) {
    assert(match_at(opts, state, P25P2_SYNC, sync_start) == DSD_SYNC_P25P2_POS);
    unsigned int bit_errors = 99U;
    assert(dsd_frame_sync_p25p2_tolerant_start(state, &bit_errors) == -1);
    assert(bit_errors == 0U);
    assert(dsd_frame_sync_p25p2_window_context_holds(state));
    state->symbol_clock = sync_start + SYNC_DIBITS + WINDOW_DIBITS;
    dsd_frame_sync_p25p2_window_decoded(state, 1, start);
}

static void
anchored(dsd_opts* opts, dsd_state* state) {
    placed_window(opts, state, ANCHOR_SYNC, ANCHOR_START);
    assert(state->p25_p2_sync_anchor.valid == 1U);
    assert(state->p25_p2_sync_anchor.expected_clock == EXPECTED);
    assert(state->p25_p2_sync_anchor.start == ANCHOR_START);
}

/* At the expected position a word T bits off is accepted, predicting the next window's start; one T+1 bits off is not.
   Either way the anchor is spent. */
static void
test_expected_position_threshold(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];

    setup(&opts, &state);
    anchored(&opts, &state);
    word_with_bit_errors(P25P2_SYNC, DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS, window);
    state.ftype[0] = '\0';
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P2_POS);
    unsigned int bit_errors = 0U;
    assert(dsd_frame_sync_p25p2_tolerant_start(&state, &bit_errors) == NEXT_START);
    assert(bit_errors == DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS);
    assert(state.p25_p2_sync_anchor.tolerant_syncs == 1U);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    assert(state.p25_p2_sync_anchor.frame_valid == 1U);
    assert(state.p25_p2_sync_anchor.frame.sync_clock == EXPECTED);
    /* What an accept sets that is no credit is set as for an exact word: the lock and the sync type. */
    assert(state.lastsynctype == DSD_SYNC_P25P2_POS);
    assert(state.carrier == 1);
    assert(opts.inverted_p2 == 0);
    /* The window behind it passes its verdict and is read whole: the next one is anchored in turn. */
    state.symbol_clock = EXPECTED + SYNC_DIBITS + WINDOW_DIBITS;
    dsd_frame_sync_p25p2_window_decoded(&state, 1, NEXT_START);
    assert(state.p25_p2_sync_anchor.tolerant_windows == 1U);
    assert(state.p25_p2_sync_anchor.valid == 1U);
    assert(state.p25_p2_sync_anchor.start == NEXT_START);
    assert(state.p25_p2_sync_anchor.expected_clock == EXPECTED + SYNC_DIBITS + WINDOW_DIBITS);
    teardown(&state);

    setup(&opts, &state);
    anchored(&opts, &state);
    word_with_bit_errors(P25P2_SYNC, DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS + 1, window);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    assert(state.p25_p2_sync_anchor.tolerant_syncs == 0U);
    /* Spent: a word one bit off a symbol later is not scored. */
    word_with_bit_errors(P25P2_SYNC, 1, window);
    assert(match_at(&opts, &state, window, EXPECTED + 1) == DSD_SYNC_NONE);
    teardown(&state);

    /* A window whose verdict failed counts nothing and leaves no anchor. */
    setup(&opts, &state);
    anchored(&opts, &state);
    word_with_bit_errors(P25P2_SYNC, 2, window);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P2_POS);
    dsd_frame_sync_p25p2_window_decoded(&state, 0, -1);
    assert(state.p25_p2_sync_anchor.tolerant_windows == 0U);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    assert(state.p25_p2_sync_anchor.frame_valid == 0U);
    assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
    teardown(&state);
}

/* Every other position is exact-only: a symbol either side, and the later S-ISCH positions (+180 when the window
   starts on slot 0's timeslot, +540 a missed window on). No anchor at all: exact words only. */
static void
test_other_positions_exact_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P2_SYNC, 1, window);
    static const int offsets[] = {-1, 1, TIMESLOT_DIBITS, 3 * TIMESLOT_DIBITS};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        setup(&opts, &state);
        anchored(&opts, &state);
        assert(match_at(&opts, &state, window, (uint64_t)(EXPECTED + offsets[i])) == DSD_SYNC_NONE);
        /* Before the expected position the anchor waits for it; at or past it, it is spent. */
        assert(state.p25_p2_sync_anchor.valid == (offsets[i] < 0 ? 1U : 0U));
        assert(match_at(&opts, &state, P25P2_SYNC, (uint64_t)(EXPECTED + offsets[i])) == DSD_SYNC_P25P2_POS);
        assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
        assert(state.p25_p2_sync_anchor.valid == 0U);
        teardown(&state);
    }
    setup(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);
}

/* The hunt over @p stream (@p len dibits, the first at @p first on the symbol clock): every window from the first full
   one on, until a sync is accepted. Returns the accepted window's start, or -1. */
static long
hunt(dsd_opts* opts, dsd_state* state, const char* stream, int len, uint64_t first, int* out_sync) {
    for (int n = SYNC_DIBITS; n <= len; n++) {
        state->symbol_clock = first + (uint64_t)n;
        const int sync = dsd_frame_sync_test_try_protocol_matches(opts, state, stream, n);
        if (sync != DSD_SYNC_NONE) {
            *out_sync = sync;
            return (long)(first + (uint64_t)n - SYNC_DIBITS);
        }
    }
    *out_sync = DSD_SYNC_NONE;
    return -1;
}

/* An exact word 1 to 12 symbols late, the symbols before it the word's own (the best case for a false match at the
   expected position): it is matched exactly where it is, never pre-empted at the expected position. */
static void
test_late_exact_word_wins(void) {
    static dsd_opts opts;
    static dsd_state state;

    enum { LEAD = 30 };

    for (int k = 1; k <= 12; k++) {
        char stream[LEAD + 12 + SYNC_DIBITS + 1];
        uint32_t seed = 0x2A2AU + (uint32_t)k;
        for (int i = 0; i < LEAD; i++) {
            seed = (seed * 1103515245U) + 12345U;
            stream[i] = (char)('0' + ((seed >> 16) & 3U));
        }
        /* Up to the expected position (LEAD dibits in) the predecessor's symbols; then the word's own first k. */
        for (int i = 0; i < k; i++) {
            stream[LEAD + i] = P25P2_SYNC[i];
        }
        DSD_MEMCPY(stream + LEAD + k, P25P2_SYNC, SYNC_DIBITS);
        const int len = LEAD + k + SYNC_DIBITS;
        stream[len] = '\0';
        setup(&opts, &state);
        anchored(&opts, &state);
        int sync = DSD_SYNC_NONE;
        const long found = hunt(&opts, &state, stream, len, (uint64_t)(EXPECTED - LEAD), &sync);
        assert(found == EXPECTED + k);
        assert(sync == DSD_SYNC_P25P2_POS);
        assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
        assert(state.p25_p2_sync_anchor.tolerant_syncs == 0U);
        teardown(&state);
    }
}

/* The same under every other map and polarity the exact matchers take: the word's head under the P1200 map or inverted,
   shifted 10 to 12 symbols, lies within the tolerance of the word under the anchor's map when the symbols before it
   match. Such a window is not taken; the exact word is matched where it is (the N1200 and P1200 matchers also fit the
   window's levels, which this hunt over symbols alone has none of, so those words are only seen not pre-empted). */
static void
test_late_exact_word_of_any_map_wins(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const uint8_t maps[] = {
        DSD_P25_CQPSK_DIBIT_MAP_IDENTITY,
        DSD_P25_CQPSK_DIBIT_MAP_X2400,
        DSD_P25_CQPSK_DIBIT_MAP_N1200,
        DSD_P25_CQPSK_DIBIT_MAP_P1200,
    };

    enum { LEAD = 30 };

    int within_tolerance = 0;
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int inverted = 0; inverted <= 1; inverted++) {
            const char* word = inverted ? INV_P25P2_SYNC : P25P2_SYNC;
            for (int k = 1; k <= DSD_P25P2_TOLERANT_SYNC_NO_STEAL_SHIFTS; k++) {
                char stream[LEAD + DSD_P25P2_TOLERANT_SYNC_NO_STEAL_SHIFTS + SYNC_DIBITS + 1];
                uint32_t seed = 0x3B3BU + (uint32_t)k;
                for (int i = 0; i < LEAD; i++) {
                    seed = (seed * 1103515245U) + 12345U;
                    stream[i] = (char)('0' + ((seed >> 16) & 3U));
                }
                /* The anchor's word's own first k symbols, then the word under this map and polarity. */
                for (int i = 0; i < k; i++) {
                    stream[LEAD + i] = P25P2_SYNC[i];
                }
                for (int i = 0; i < SYNC_DIBITS; i++) {
                    stream[LEAD + k + i] =
                        (char)('0' + dsd_p25_cqpsk_raw_dibit_for_corrected(maps[m], (uint8_t)(word[i] - '0')));
                }
                const int len = LEAD + k + SYNC_DIBITS;
                stream[len] = '\0';
                char expected_window[SYNC_DIBITS + 1];
                DSD_MEMCPY(expected_window, stream + LEAD, SYNC_DIBITS);
                expected_window[SYNC_DIBITS] = '\0';
                within_tolerance += dsd_p25p2_sync_bit_distance(expected_window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0)
                                            <= DSD_P25P2_TOLERANT_SYNC_MAX_BIT_ERRORS
                                        ? 1
                                        : 0;
                setup(&opts, &state);
                anchored(&opts, &state);
                int sync = DSD_SYNC_NONE;
                const long found = hunt(&opts, &state, stream, len, (uint64_t)(EXPECTED - LEAD), &sync);
                assert(state.p25_p2_sync_anchor.tolerant_syncs == 0U);
                assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
                if (maps[m] == DSD_P25_CQPSK_DIBIT_MAP_IDENTITY || maps[m] == DSD_P25_CQPSK_DIBIT_MAP_X2400) {
                    assert(found == EXPECTED + k);
                    assert(sync == (inverted ? DSD_SYNC_P25P2_NEG : DSD_SYNC_P25P2_POS));
                } else {
                    assert(found != EXPECTED);
                }
                teardown(&state);
            }
        }
    }
    /* The case is real: windows within the tolerance at the expected position (inverted at 10, 11 and 12 symbols, P1200
       at 10, 11 and 12). */
    assert(within_tolerance == 6);
}

/* An exact word at the expected position, inverted or under a rotated map: the exact matchers take it. */
static void
test_exact_words_take_the_exact_path(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, INV_P25P2_SYNC, EXPECTED) == DSD_SYNC_P25P2_NEG);
    assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
    teardown(&state);

    char rotated[SYNC_DIBITS + 1];
    for (int i = 0; i < SYNC_DIBITS; i++) {
        rotated[i] = (char)('0'
                            + dsd_p25_cqpsk_raw_dibit_for_corrected(DSD_P25_CQPSK_DIBIT_MAP_X2400,
                                                                    (uint8_t)(P25P2_SYNC[i] - '0')));
    }
    rotated[SYNC_DIBITS] = '\0';
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, rotated, EXPECTED) == DSD_SYNC_P25P2_POS);
    assert(dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1);
    assert(state.p25_cqpsk_dibit_map_idx == DSD_P25_CQPSK_DIBIT_MAP_X2400);
    teardown(&state);
}

/* The anchor's dibit map and polarity read the window and are what the window is decoded under: an inverted X2400
   receiver's word, three bits off. The same window under the identity map and positive polarity is no word. */
static void
test_map_and_polarity_follow_the_anchor(void) {
    static dsd_opts opts;
    static dsd_state state;
    char corrected[SYNC_DIBITS + 1];
    char raw[SYNC_DIBITS + 1];
    word_with_bit_errors(INV_P25P2_SYNC, 3, corrected);
    for (int i = 0; i < SYNC_DIBITS; i++) {
        raw[i] = (char)('0'
                        + dsd_p25_cqpsk_raw_dibit_for_corrected(DSD_P25_CQPSK_DIBIT_MAP_X2400,
                                                                (uint8_t)(corrected[i] - '0')));
    }
    raw[SYNC_DIBITS] = '\0';

    setup(&opts, &state);
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_X2400;
    state.symbol_clock = ANCHOR_SYNC + SYNC_DIBITS;
    frame_sync_p25p2_anchor_note_sync(&state, DSD_SYNC_P25P2_NEG, -1, 0, 0);
    state.symbol_clock = EXPECTED;
    dsd_frame_sync_p25p2_window_decoded(&state, 1, ANCHOR_START);
    assert(state.p25_p2_sync_anchor.valid == 1U && state.p25_p2_sync_anchor.anchor.inverted == 1U);
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_IDENTITY;
    opts.inverted_p2 = 0;
    assert(match_at(&opts, &state, raw, EXPECTED) == DSD_SYNC_P25P2_NEG);
    assert(state.p25_cqpsk_dibit_map_idx == DSD_P25_CQPSK_DIBIT_MAP_X2400);
    assert(opts.inverted_p2 == 1);
    assert(state.lastsynctype == DSD_SYNC_P25P2_NEG);
    assert(state.p25_p2_sync_anchor.frame.inverted == 1U);
    teardown(&state);

    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, raw, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);
}

typedef void (*stream_change_fn)(dsd_opts* opts, dsd_state* state);

static void
epoch_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->symbol_clock_epoch++;
}

static void
input_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->symbol_clock_input++;
}

static void
carrier_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->carrier_seq++;
}

static void
tune_completes(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    dsd_trunk_tuning_generation_advance();
}

static void
histories_dropped(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    dsd_state_forget_sync_join_histories(state);
}

static void
profile_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
}

static void
modulation_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->rf_mod = 0;
}

static void
acquisition_reset(dsd_opts* opts, dsd_state* state) {
    dsd_frame_sync_reset_acquisition(opts, state, 0);
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
}

static void
anchor_forgotten(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    /* The no-carrier pass (engine.c), a trunk scan target's restore. */
    dsd_state_forget_p25p2_sync_anchor(state);
}

static void
other_sync_accepted(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    frame_sync_p25p2_anchor_note_accept(state, DSD_SYNC_X2TDMA_DATA_POS);
}

static int g_pending_tune;

static void
off_thread_tune_noted(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    dsd_p25p1_decision read;
    dsd_frame_sync_p25p1_read_decision(&read);
    dsd_frame_sync_note_off_thread_tune(&read);
    g_pending_tune = 1;
}

static void
settle_pending_tune(dsd_opts* opts, dsd_state* state) {
    if (g_pending_tune) {
        (void)dsd_frame_sync_apply_off_thread_tunes(opts, state);
        g_pending_tune = 0;
    }
}

/* Each change of the stream between the anchor and the expected position: the tolerant word is refused there. */
static void
test_invalidations(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const struct {
        stream_change_fn change;
        const char* name;
    } changes[] = {
        {epoch_moves, "clock epoch"},
        {input_moves, "input generation"},
        {carrier_moves, "carrier"},
        {tune_completes, "tuning generation"},
        {histories_dropped, "history drops"},
        {profile_moves, "SPS profile"},
        {modulation_moves, "modulation"},
        {acquisition_reset, "acquisition reset"},
        {anchor_forgotten, "forget (no carrier, scan restore)"},
        {other_sync_accepted, "another protocol's sync"},
        {off_thread_tune_noted, "pending off-thread tune"},
    };

    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P2_SYNC, 1, window);
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        setup(&opts, &state);
        anchored(&opts, &state);
        changes[i].change(&opts, &state);
        const int sync = match_at(&opts, &state, window, EXPECTED);
        if (sync != DSD_SYNC_NONE) {
            DSD_FPRINTF(stderr, "anchor survived: %s\n", changes[i].name);
        }
        assert(sync == DSD_SYNC_NONE);
        settle_pending_tune(&opts, &state);
        teardown(&state);
    }
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P2_POS);
    teardown(&state);
}

/* The sync-time context is captured when the sync is accepted and checked again when processP2() reports the window:
   a change in between, a decoder that read past the window, a start no S-ISCH precedes or no start installs nothing. */
static void
test_installation(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const stream_change_fn changes[] = {
        epoch_moves, input_moves, carrier_moves, tune_completes, histories_dropped, profile_moves, modulation_moves,
    };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        setup(&opts, &state);
        assert(match_at(&opts, &state, P25P2_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P2_POS);
        assert(state.p25_p2_sync_anchor.frame_valid == 1U);
        changes[i](&opts, &state);
        assert(!dsd_frame_sync_p25p2_window_context_holds(&state));
        state.symbol_clock = EXPECTED;
        dsd_frame_sync_p25p2_window_decoded(&state, 1, ANCHOR_START);
        assert(state.p25_p2_sync_anchor.valid == 0U);
        assert(state.p25_p2_sync_anchor.frame_valid == 0U);
        teardown(&state);
    }

    /* A sync accepted while an off-thread tune is pending captures nothing. */
    setup(&opts, &state);
    off_thread_tune_noted(&opts, &state);
    assert(match_at(&opts, &state, P25P2_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P2_POS);
    assert(state.p25_p2_sync_anchor.frame_valid == 0U);
    assert(!dsd_frame_sync_p25p2_window_context_holds(&state));
    settle_pending_tune(&opts, &state);
    state.symbol_clock = EXPECTED;
    dsd_frame_sync_p25p2_window_decoded(&state, 1, ANCHOR_START);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    teardown(&state);

    /* Read past the window's end: the hunt starts beyond the expected position. */
    setup(&opts, &state);
    assert(match_at(&opts, &state, P25P2_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P2_POS);
    state.symbol_clock = EXPECTED + 1;
    dsd_frame_sync_p25p2_window_decoded(&state, 1, ANCHOR_START);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    teardown(&state);

    /* No start (not placed by an I-ISCH, contradicted, retuned), a start whose next sync is an I-ISCH, a start out of
       range, a window cut, and a report with no sync captured (reported twice): nothing. */
    static const struct {
        int dispatched;
        int start;
    } refused[] = {{1, -1}, {1, 0}, {1, 1}, {1, 4}, {1, 5}, {1, 8}, {1, 9}, {1, 12}, {0, ANCHOR_START}};

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        setup(&opts, &state);
        assert(match_at(&opts, &state, P25P2_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P2_POS);
        state.symbol_clock = EXPECTED;
        dsd_frame_sync_p25p2_window_decoded(&state, refused[i].dispatched, refused[i].start);
        assert(state.p25_p2_sync_anchor.valid == 0U);
        assert(state.p25_p2_sync_anchor.frame_valid == 0U);
        teardown(&state);
    }
    setup(&opts, &state);
    anchored(&opts, &state);
    state.p25_p2_sync_anchor.valid = 0U;
    dsd_frame_sync_p25p2_window_decoded(&state, 1, ANCHOR_START);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    teardown(&state);

    /* Every start an S-ISCH precedes installs. */
    static const int starts[] = {2, 3, 6, 7, 10, 11};
    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        setup(&opts, &state);
        placed_window(&opts, &state, ANCHOR_SYNC, starts[i]);
        assert(state.p25_p2_sync_anchor.valid == 1U && state.p25_p2_sync_anchor.start == starts[i]);
        teardown(&state);
    }
}

/* The levels the window's symbols left: a biased centre an exact accept refits from. */
static void
push_sync_levels(dsd_state* state, float bias) {
    static const float levels[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    for (int i = 0; i < SYNC_DIBITS; i++) {
        dsd_symbol_history_push(state, levels[P25P2_SYNC[i] - '0'] + bias);
    }
}

static int g_vc_sync_hooks;

static void
count_vc_sync(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_vc_sync_hooks++;
}

static int g_profile_pushes;

static int
count_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    (void)levels;
    (void)channel_profile;
    (void)ted_sps;
    g_profile_pushes++;
    return 0;
}

/* A tolerant accept writes no centre at acceptance (CENTER_SKIP), notes no channel activity (neither the voice channel
   hook nor the control channel's sync time) and pushes no demodulator profile; an exact accept at the same place does
   all three. processP2() notes the activity once the window's verdict passes. */
static void
test_tolerant_accept_withholds_sync_time_effects(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P2_SYNC, 2, window);
    dsd_frame_sync_hooks saved;
    dsd_frame_sync_hooks_get(&saved);
    dsd_frame_sync_hooks hooks = saved;
    hooks.p25_sm_vc_sync = count_vc_sync;
    dsd_frame_sync_hooks_set(&hooks);

    for (int exact = 0; exact <= 1; exact++) {
        for (int tuned = 0; tuned <= 1; tuned++) {
            setup(&opts, &state);
            anchored(&opts, &state);
            opts.trunk_enable = tuned;
            opts.trunk_is_tuned = tuned;
            opts.mod_cli_lock = 0;
            state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
            dsd_rtl_stream_metrics_hooks metrics = {
                .cqpsk_status = cqpsk_status,
                .apply_demod_profile = count_demod_profile,
            };
            dsd_rtl_stream_metrics_hooks_set(&metrics);
            push_sync_levels(&state, 0.5f);
            state.center = 0.0f;
            state.last_cc_sync_time = 0;
            state.last_cc_sync_time_m = -1.0;
            g_vc_sync_hooks = 0;
            g_profile_pushes = 0;
            assert(match_at(&opts, &state, exact ? P25P2_SYNC : window, EXPECTED) == DSD_SYNC_P25P2_POS);
            assert((dsd_frame_sync_p25p2_tolerant_start(&state, NULL) == -1) == exact);
            if (exact) {
                assert(fabsf(state.center - 0.5f) < 0.05f);
                assert(g_profile_pushes == 1);
                assert(tuned ? g_vc_sync_hooks == 1 : fabs(state.last_cc_sync_time_m + 1.0) > 1e-9);
            } else {
                assert(fabsf(state.center) < 1e-6f);
                assert(g_profile_pushes == 0);
                assert(g_vc_sync_hooks == 0);
                assert(fabs(state.last_cc_sync_time_m + 1.0) < 1e-9);
                /* processP2()'s note after the verdict passed. */
                dsd_frame_sync_p25p2_note_sync_activity(&opts, &state);
                assert(tuned ? g_vc_sync_hooks == 1 : fabs(state.last_cc_sync_time_m + 1.0) > 1e-9);
            }
            state.rtl_ctx = NULL;
            teardown(&state);
        }
    }
    dsd_frame_sync_hooks_set(&saved);
}

/* Off the RTL CQPSK 4-level path nothing is tolerated: no CQPSK chain status (C4FM under -m3, a symbol file), or the
   modulation on C4FM. The anchor is spent unscored. */
static void
test_cqpsk_path_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P2_SYNC, 1, window);

    setup(&opts, &state);
    anchored(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    assert(state.p25_p2_sync_anchor.valid == 0U);
    teardown(&state);

    setup(&opts, &state);
    anchored(&opts, &state);
    opts.audio_in_type = AUDIO_IN_SYMBOL_FLT;
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);

    /* The -m3 C4FM Phase 2 path: 10-SPS C4FM timing, rf_mod 0. An anchor taken on it holds rf_mod 0. */
    setup(&opts, &state);
    opts.mod_p25p2_c4fm = 1;
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    anchored(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);
}
#endif

int
main(void) {
    test_bit_distance();
#ifdef USE_RADIO
    test_expected_position_threshold();
    test_other_positions_exact_only();
    test_late_exact_word_wins();
    test_late_exact_word_of_any_map_wins();
    test_exact_words_take_the_exact_path();
    test_map_and_polarity_follow_the_anchor();
    test_invalidations();
    test_installation();
    test_tolerant_accept_withholds_sync_time_effects();
    test_cqpsk_path_only();
#endif
    DSD_FPRINTF(stderr, "FRAME_SYNC_P25P2_ANCHOR: OK\n");
    return 0;
}
