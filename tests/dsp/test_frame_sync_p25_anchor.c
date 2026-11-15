// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 Phase 1 sync anchor (dsd_p25p1_sync_anchor): after a validated frame the hunt accepts a sync word within
 * DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS bit errors at the position where that frame ends, and only there, on the RTL
 * CQPSK 4800/4 path, on the stream the frame was read on. The windows go through the real matchers
 * (dsd_frame_sync_test_try_protocol_matches()), each read as the window ending on dsd_state::symbol_clock, as the hunt
 * reads them; the anchor is installed through the dispatcher's entry point (dsd_frame_sync_p25p1_frame_decoded()).
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"
#include "frame_sync_test_support.h"
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif

enum {
    SYNC_DIBITS = 24,
    NAC = 0x1A6,
    ANCHOR_SYNC = 1000, /* the validated frame's sync starts here on the symbol clock */
    TSDU_1 = 180,
    EXPECTED = ANCHOR_SYNC + TSDU_1,
};

/* The P25p1 sync word with the low bit of @p bits dibits flipped (one level off, one bit each). */
static void
word_with_bit_errors(const char* word, int bits, char out[SYNC_DIBITS + 1]) {
    static const int positions[] = {0, 4, 8, 12, 16, 20, 2, 6, 10};
    DSD_MEMCPY(out, word, SYNC_DIBITS + 1);
    for (int i = 0; i < bits; i++) {
        out[positions[i]] = (char)('0' + ((out[positions[i]] - '0') ^ 1));
    }
}

#ifdef USE_RADIO
/* The NAC an anchored frame's NID must name, -1 behind any other sync. */
static int
anchored_nac(const dsd_state* state, unsigned int* out_errors) {
    int nac = -1;
    return dsd_frame_sync_p25p1_sync_kind(state, &nac, out_errors) == DSD_P25P1_SYNC_ANCHORED ? nac : -1;
}

/* The window @p window (24 dibits) as the one the hunt reads when its first dibit is @p start on the symbol clock. */
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

/* The RTL CQPSK hunt on 4800/4, Phase 1 only, the modulation locked so no vote moves it. */
static void
setup(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    assert(init_state_buffers(state));
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_p25p1 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_qpsk = 1;
    opts->msize = 1;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->min = -3.0f;
    state->max = 3.0f;
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

/* A frame of @p unit dibits whose sync started at @p sync_start validated naming @p nac, its decoder having read
   @p consumed dibits of it. */
static void
validated_frame(dsd_opts* opts, dsd_state* state, uint64_t sync_start, uint32_t unit, uint32_t consumed, int nac) {
    assert(match_at(opts, state, P25P1_SYNC, sync_start) == DSD_SYNC_P25P1_POS);
    unsigned int bit_errors = 99U;
    assert(anchored_nac(state, &bit_errors) == -1);
    assert(bit_errors == 0U);
    state->symbol_clock = sync_start + consumed;
    dsd_frame_sync_p25p1_frame_decoded(state, nac, unit);
}

/* The anchor a validated one-block TSDU at ANCHOR_SYNC leaves: the next sync starts at EXPECTED. */
static void
anchored(dsd_opts* opts, dsd_state* state) {
    validated_frame(opts, state, ANCHOR_SYNC, TSDU_1, 158, NAC);
    assert(state->p25_p1_sync_anchor.valid == 1U);
    assert(state->p25_p1_sync_anchor.expected_clock == EXPECTED);
    assert(state->p25_p1_sync_anchor.nac == NAC);
}
#endif

/* The distance counts bits, under the map and polarity asked for; the shifted word's distance to itself is the bound
   the threshold relies on. */
static void
test_bit_distance(void) {
    char window[SYNC_DIBITS + 1];
    assert(dsd_p25p1_sync_bit_distance(P25P1_SYNC, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 0);
    assert(dsd_p25p1_sync_bit_distance(INV_P25P1_SYNC, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 1) == 0);
    /* Every dibit a sign error away, the low bit kept: 24 bits. */
    assert(dsd_p25p1_sync_bit_distance(INV_P25P1_SYNC, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 24);
    for (int bits = 0; bits <= 7; bits++) {
        word_with_bit_errors(P25P1_SYNC, bits, window);
        assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == bits);
    }
    /* A '1' read as '2' (+3 as -1) is two bits off. */
    DSD_MEMCPY(window, P25P1_SYNC, sizeof(window));
    window[0] = '2';
    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 2);
    /* The word as an N1200-rotated receiver slices it is the word under that map. */
    for (int i = 0; i < SYNC_DIBITS; i++) {
        window[i] = (char)('0'
                           + dsd_p25_cqpsk_raw_dibit_for_corrected(DSD_P25_CQPSK_DIBIT_MAP_N1200,
                                                                   (uint8_t)(INV_P25P1_SYNC[i] - '0')));
    }
    window[SYNC_DIBITS] = '\0';
    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_N1200, 1) == 0);
    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 1) > 0);
    /* A byte that is no dibit counts two, a short window everything. */
    window[3] = 'x';
    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_N1200, 1) == 2);
    assert(dsd_p25p1_sync_bit_distance("1111", DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 48);
    assert(dsd_p25p1_sync_bit_distance(NULL, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 48);

    /* The word shifted by 1..5 symbols, the symbols shifted in matching the word itself (the best case for a false
       match), late and early: 9, 10, 9, 8, 9 bits, all past the threshold. */
    static const int shifted[] = {9, 10, 9, 8, 9};
    for (int k = 1; k <= 5; k++) {
        char late[SYNC_DIBITS + 1];
        char early[SYNC_DIBITS + 1];
        for (int i = 0; i < SYNC_DIBITS; i++) {
            late[i] = i + k < SYNC_DIBITS ? P25P1_SYNC[i + k] : P25P1_SYNC[i];
            early[i] = i < k ? P25P1_SYNC[i] : P25P1_SYNC[i - k];
        }
        late[SYNC_DIBITS] = '\0';
        early[SYNC_DIBITS] = '\0';
        assert(dsd_p25p1_sync_bit_distance(late, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == shifted[k - 1]);
        assert(dsd_p25p1_sync_bit_distance(early, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == shifted[k - 1]);
        assert(shifted[k - 1] > DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS);
    }
}

#ifdef USE_RADIO
/* At the expected position a word T bits off is accepted, one T+1 bits off is not; either way the anchor is spent. */
static void
test_expected_position_threshold(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];

    setup(&opts, &state);
    anchored(&opts, &state);
    word_with_bit_errors(P25P1_SYNC, DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS, window);
    /* Without the anchor main's exact matchers refuse it. */
    assert(strcmp(window, P25P1_SYNC) != 0);
    state.p25_p1_c4fm_frame_valid = 1;
    state.ftype[0] = '\0';
    state.dmrburstR = 0;
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P1_POS);
    unsigned int bit_errors = 0U;
    assert(anchored_nac(&state, &bit_errors) == NAC);
    assert(bit_errors == DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS);
    assert(state.p25_p1_sync_anchor.tolerant_syncs == 1U);
    assert(state.p25_p1_sync_anchor.valid == 0U);
    assert(state.p25_p1_sync_anchor.frame_valid == 1U);
    assert(state.p25_p1_sync_anchor.frame.sync_clock == EXPECTED);
    /* What an accept sets that is no credit is set as for an exact word: the lock, the frame's span flag, its labels. */
    assert(state.lastsynctype == DSD_SYNC_P25P1_POS);
    assert(state.carrier == 1);
    assert(state.p25_p1_c4fm_frame_valid == 0);
    assert(strcmp(state.ftype, "P25 Phase 1") == 0);
    assert(state.dmrburstR == 17);
    teardown(&state);

    setup(&opts, &state);
    anchored(&opts, &state);
    word_with_bit_errors(P25P1_SYNC, DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS + 1, window);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    assert(state.p25_p1_sync_anchor.valid == 0U);
    assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
    /* Spent: the same word at T a symbol later is not scored. */
    word_with_bit_errors(P25P1_SYNC, 1, window);
    assert(match_at(&opts, &state, window, EXPECTED + 1) == DSD_SYNC_NONE);
    teardown(&state);
}

/* Every other position is exact-only: one or two symbols either side, and the next unit's slot (a missed frame).
   Exact words are still matched wherever they are. */
static void
test_other_positions_exact_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, 1, window);
    static const int offsets[] = {-2, -1, 1, 2, TSDU_1};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        setup(&opts, &state);
        anchored(&opts, &state);
        assert(match_at(&opts, &state, window, (uint64_t)(EXPECTED + offsets[i])) == DSD_SYNC_NONE);
        /* Before the expected position the anchor waits for it; at or past it, it is spent. */
        assert(state.p25_p1_sync_anchor.valid == (offsets[i] < 0 ? 1U : 0U));
        assert(match_at(&opts, &state, P25P1_SYNC, (uint64_t)(EXPECTED + offsets[i])) == DSD_SYNC_P25P1_POS);
        assert(anchored_nac(&state, NULL) == -1);
        /* Any accepted sync spends it. */
        assert(state.p25_p1_sync_anchor.valid == 0U);
        teardown(&state);
    }
    /* No anchor at all (a fresh hunt): nothing but exact words. */
    setup(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);
}

/* Symbols around a real sync word: pseudo-random data dibits. */
static void
data_dibits(char* out, int count, uint32_t* seed) {
    for (int i = 0; i < count; i++) {
        *seed = (*seed * 1103515245U) + 12345U;
        out[i] = (char)('0' + ((*seed >> 16) & 3U));
    }
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

/* A slip of one or two symbols (a symbol inserted or lost) after a validated anchor moves the real word off the expected
   position, with the real symbols around it. An exact word wins where it is: the expected-position window is then a
   shifted word, which no corruption below brings within the threshold. A corrupted word the slip moved is lost, as on an
   exact-only hunt. */
static void
test_slips_with_real_neighbours(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const int slips[] = {-2, -1, 1, 2};

    enum { LEAD = 40, LEN = LEAD + 2 + SYNC_DIBITS + 33 };

    for (size_t s = 0; s < sizeof(slips) / sizeof(slips[0]); s++) {
        for (int corrupted = 0; corrupted <= 1; corrupted++) {
            char stream[LEN + 1];
            uint32_t seed = 0x5A5AU + (uint32_t)s;
            data_dibits(stream, LEN, &seed);
            stream[LEN] = '\0';
            /* The predecessor's padding up to the expected position, then the word where the slip put it. */
            const int at = LEAD + slips[s];
            char word[SYNC_DIBITS + 1];
            word_with_bit_errors(P25P1_SYNC, corrupted ? 2 : 0, word);
            DSD_MEMCPY(stream + at, word, SYNC_DIBITS);
            setup(&opts, &state);
            anchored(&opts, &state);
            int sync = DSD_SYNC_NONE;
            const long found = hunt(&opts, &state, stream, LEN, (uint64_t)(EXPECTED - LEAD), &sync);
            if (corrupted) {
                assert(found == -1);
                assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
            } else {
                assert(found == EXPECTED + slips[s]);
                assert(sync == DSD_SYNC_P25P1_POS);
                assert(anchored_nac(&state, NULL) == -1);
            }
            teardown(&state);
        }
    }
}

/* An exact word 1 to 13 symbols late, under every map and polarity the exact matchers take, the symbols before it the
   anchor's word's own (the best case for a false match at the expected position). Under the anchor's map the shifted
   word stays at least 7 bits off; inverted, or under the P1200 map, shifted 11 to 13 symbols it comes within the
   tolerance. Such a window is not taken; the exact word is matched where it is (the N1200 and P1200 matchers also fit
   the window's levels, which this hunt over symbols alone has none of, so those words are only seen not pre-empted). */
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

    enum { LEAD = 40 };

    int within_tolerance = 0;
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        for (int inverted = 0; inverted <= 1; inverted++) {
            const char* word = inverted ? INV_P25P1_SYNC : P25P1_SYNC;
            for (int k = 1; k <= DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS; k++) {
                char stream[LEAD + DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS + SYNC_DIBITS + 1];
                uint32_t seed = 0x6C6CU + (uint32_t)k;
                data_dibits(stream, LEAD, &seed);
                for (int i = 0; i < k; i++) {
                    stream[LEAD + i] = P25P1_SYNC[i];
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
                within_tolerance += dsd_p25p1_sync_bit_distance(expected_window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0)
                                            <= DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS
                                        ? 1
                                        : 0;
                setup(&opts, &state);
                anchored(&opts, &state);
                int sync = DSD_SYNC_NONE;
                const long found = hunt(&opts, &state, stream, len, (uint64_t)(EXPECTED - LEAD), &sync);
                assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
                assert(anchored_nac(&state, NULL) == -1);
                if (maps[m] == DSD_P25_CQPSK_DIBIT_MAP_IDENTITY || maps[m] == DSD_P25_CQPSK_DIBIT_MAP_X2400) {
                    assert(found == EXPECTED + k);
                    assert(sync == (inverted ? DSD_SYNC_P25P1_NEG : DSD_SYNC_P25P1_POS));
                } else {
                    assert(found != EXPECTED);
                }
                teardown(&state);
            }
        }
    }
    /* The case is real: windows within the tolerance at the expected position (inverted at 11, 12 and 13 symbols, P1200
       at 12 and 13). */
    assert(within_tolerance == 5);
}

/* Another protocol's exact word a slip put after the expected position, under -fa -mq: with X2-TDMA on, the five
   symbols "11111" and an intact X2-TDMA base voice word give a window at the expected position six bits off the
   P25p1 word. Taken, the NID read behind it went through the X2-TDMA word, and its burst was lost though the strict
   NID dropped the frame. It is not taken; the X2-TDMA matcher takes its word where it is. */
static void
test_late_exact_word_of_another_protocol_wins(void) {
    static dsd_opts opts;
    static dsd_state state;

    enum { LEAD = 40, SLIP = 5 };

    char stream[LEAD + SLIP + SYNC_DIBITS + 1];
    uint32_t seed = 0x7D7DU;
    data_dibits(stream, LEAD, &seed);
    DSD_MEMCPY(stream + LEAD, "11111", SLIP);
    DSD_MEMCPY(stream + LEAD + SLIP, X2TDMA_BS_VOICE_SYNC, SYNC_DIBITS);
    const int len = LEAD + SLIP + SYNC_DIBITS;
    stream[len] = '\0';
    char expected_window[SYNC_DIBITS + 1];
    DSD_MEMCPY(expected_window, stream + LEAD, SYNC_DIBITS);
    expected_window[SYNC_DIBITS] = '\0';
    assert(dsd_p25p1_sync_bit_distance(expected_window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0)
           == DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS);

    for (int anchor = 0; anchor <= 1; anchor++) {
        setup(&opts, &state);
        opts.frame_x2tdma = 1;
        if (anchor) {
            anchored(&opts, &state);
        }
        int sync = DSD_SYNC_NONE;
        const long found = hunt(&opts, &state, stream, len, (uint64_t)(EXPECTED - LEAD), &sync);
        assert(found == EXPECTED + SLIP);
        assert(sync == DSD_SYNC_X2TDMA_VOICE_POS);
        assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
        teardown(&state);
    }
}

/* Every matcher the hunt tries declares its exact words and the enable predicate they are read under: a protocol added
   to the try table without them would leave its words open to a tolerant P25 word. */
static void
test_every_matcher_declares_its_words(void) {
    const size_t count = dsd_frame_sync_test_try_count();
    assert(count >= 12U);
    for (size_t i = 0; i < count; i++) {
        int has_enabled = 0;
        int reading = -1;
        const char* const* words = dsd_frame_sync_test_try_words(i, &has_enabled, &reading);
        assert(words != NULL && words[0] != NULL);
        assert(has_enabled == 1);
        assert(reading >= 0 && reading <= 2);
        for (const char* const* w = words; *w != NULL; w++) {
            assert(strlen(*w) >= 8U);
            for (const char* c = *w; *c != '\0'; c++) {
                assert((*c >= '0' && *c <= '3') || *c == '.');
            }
        }
    }
    assert(dsd_frame_sync_test_try_words(count, NULL, NULL) == NULL);

    /* What X2-TDMA, YSF and DMR declare is what their matchers take, each word on its own (the others need a frame's
       context first: NXDN96 a hit a frame back, M17 a preamble or a frame chain). */
    static const char* const first_words[] = {X2TDMA_BS_DATA_SYNC, FUSION_SYNC, DMR_MS_DATA_SYNC};
    int checked = 0;
    for (size_t i = 0; i < count; i++) {
        const char* const* words = dsd_frame_sync_test_try_words(i, NULL, NULL);
        int plain = 0;
        for (size_t f = 0; f < sizeof(first_words) / sizeof(first_words[0]); f++) {
            plain |= strcmp(words[0], first_words[f]) == 0;
        }
        if (!plain) {
            continue;
        }
        for (const char* const* w = words; *w != NULL; w++) {
            static dsd_opts opts;
            static dsd_state state;
            char stream[40 + 32];
            uint32_t seed = 0xA1A1U;
            data_dibits(stream, 40, &seed);
            const size_t length = strlen(*w);
            DSD_MEMCPY(stream + 40, *w, length);
            stream[40 + length] = '\0';
            /* Under either polarity option: the reverse-channel word a matcher takes under one alone is declared. */
            long found = -1;
            int sync = DSD_SYNC_NONE;
            for (int inverted = 0; inverted <= 1 && found < 0; inverted++) {
                setup(&opts, &state);
                opts.frame_x2tdma = 1;
                opts.frame_ysf = 1;
                opts.frame_dmr = 1;
                opts.inverted_dmr = inverted;
                found = hunt(&opts, &state, stream, (int)(40 + length), 5000U, &sync);
                teardown(&state);
            }
            assert(found == 5000 + 40 + (long)length - (long)SYNC_DIBITS);
            assert(sync != DSD_SYNC_NONE && !DSD_SYNC_IS_P25(sync));
            checked++;
        }
    }
    assert(checked == 4 + 2 + 10);
}

/* The exact words of every matcher that runs on the 4800/4 CQPSK hunt with every protocol of that profile on (the
   -fa set: P25p1, X2-TDMA, YSF, M17, DMR, NXDN96), each starting 1 to the span's symbols after the expected position
   with the anchor's own word before it (the best case for a false match): wherever that window is within the tolerance,
   it is not taken. Every word comes from the matchers' declarations, so a protocol added there is covered here. */
static void
test_no_exact_word_of_any_protocol_is_pre_empted(void) {
    static dsd_opts opts;
    static dsd_state state;

    enum { LEAD = 40 };

    int within_tolerance = 0;
    int protocols = 0;
    for (size_t t = 0; t < dsd_frame_sync_test_try_count(); t++) {
        setup(&opts, &state);
        opts.frame_x2tdma = 1;
        opts.frame_ysf = 1;
        opts.frame_m17 = 1;
        opts.frame_dmr = 1;
        opts.frame_nxdn96 = 1;
        const int enabled = dsd_frame_sync_test_try_enabled(&opts, &state, t);
        teardown(&state);
        if (!enabled) {
            continue;
        }
        protocols++;
        for (const char* const* w = dsd_frame_sync_test_try_words(t, NULL, NULL); *w != NULL; w++) {
            const int length = (int)strlen(*w);
            /* No word that runs here is longer than the window, so its heads cover every start. */
            assert(length <= SYNC_DIBITS);
            for (int start = 1; start <= DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS; start++) {
                char stream[LEAD + DSD_P25P1_TOLERANT_SYNC_NO_STEAL_SHIFTS + 64];
                uint32_t seed = 0x8E8EU + (uint32_t)start;
                data_dibits(stream, LEAD, &seed);
                DSD_MEMCPY(stream + LEAD, P25P1_SYNC, (size_t)start);
                for (int i = 0; i < length; i++) {
                    stream[LEAD + start + i] = (*w)[i] == '.' ? '1' : (*w)[i];
                }
                const int len = LEAD + start + length;
                stream[len] = '\0';
                if (start + length <= SYNC_DIBITS) {
                    continue; /* the whole word inside the expected-position window: no later word */
                }
                char expected_window[SYNC_DIBITS + 1];
                DSD_MEMCPY(expected_window, stream + LEAD, SYNC_DIBITS);
                expected_window[SYNC_DIBITS] = '\0';
                if (dsd_p25p1_sync_bit_distance(expected_window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0)
                    > DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS) {
                    continue;
                }
                within_tolerance++;
                setup(&opts, &state);
                opts.frame_x2tdma = 1;
                opts.frame_ysf = 1;
                opts.frame_m17 = 1;
                opts.frame_dmr = 1;
                opts.frame_nxdn96 = 1;
                anchored(&opts, &state);
                int sync = DSD_SYNC_NONE;
                (void)hunt(&opts, &state, stream, len, (uint64_t)(EXPECTED - LEAD), &sync);
                assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
                teardown(&state);
            }
        }
    }
    /* P25p1, X2-TDMA, YSF, M17, DMR and NXDN run there; the case is real for several of their words. */
    assert(protocols == 6);
    assert(within_tolerance > 20);
}

/* r1 review #3: a word with three bit errors one symbol late, behind a '1': the window one symbol early, which is the
   expected position, is 6 bits off and is accepted there. The NID it reads is then one symbol off; the dispatcher's
   strict check is what drops it (P25_P1_TOLERANT_SYNC, the NAC 555 case). */
static void
test_slipped_corrupted_word_one_symbol_early(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char received[] = "111131133311333313133333";
    char window[SYNC_DIBITS + 1];
    assert(dsd_p25p1_sync_bit_distance(received, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 3);
    window[0] = '1';
    DSD_MEMCPY(window + 1, received, SYNC_DIBITS - 1);
    window[SYNC_DIBITS] = '\0';
    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 6);
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P1_POS);
    assert(anchored_nac(&state, NULL) == NAC);
    teardown(&state);
}

/* Final review: the tolerant word is tried only after every exact matcher of every enabled protocol failed. A window at
   the expected position 3 bits off the P25p1 word that ends in an inverted NXDN FSW is NXDN's under a live NXDN96 lock
   one frame back (-fa runs both on 4800/4): the NXDN matcher takes it, and the anchor is spent unscored. Without the
   lock the NXDN matcher refuses it and the same window is the tolerant P25p1 word. */
static void
test_exact_matcher_of_another_protocol_wins(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char window[] = "111113113311331313113313";

    enum { OTHER_NAC = 0x2D1, NOW = 5000 };

    assert(dsd_p25p1_sync_bit_distance(window, DSD_P25_CQPSK_DIBIT_MAP_IDENTITY, 0) == 3);
    assert(strcmp(window + SYNC_DIBITS - 10, INV_NXDN_FSW) == 0);
    for (int locked = 0; locked <= 1; locked++) {
        setup(&opts, &state);
        opts.frame_nxdn96 = 1;
        validated_frame(&opts, &state, ANCHOR_SYNC, TSDU_1, 158, OTHER_NAC);
        assert(state.p25_p1_sync_anchor.valid == 1U && state.p25_p1_sync_anchor.expected_clock == EXPECTED);
        state.symbolcnt = NOW;
        if (locked) {
            state.nxdn96_sync.lock_type = (uint8_t)DSD_SYNC_NXDN_NEG;
            state.nxdn96_sync.lock_symbolcnt = NOW - DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
        }
        const int sync = match_at(&opts, &state, window, EXPECTED);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        if (locked) {
            assert(sync == DSD_SYNC_NXDN_NEG);
            assert(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
            assert(anchored_nac(&state, NULL) == -1);
            assert(state.lastsynctype == DSD_SYNC_NXDN_NEG);
        } else {
            assert(sync == DSD_SYNC_P25P1_POS);
            assert(state.p25_p1_sync_anchor.tolerant_syncs == 1U);
            assert(anchored_nac(&state, NULL) == OTHER_NAC);
            assert(state.lastsynctype == DSD_SYNC_P25P1_POS);
        }
        teardown(&state);
    }
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
    /* The SPS hunt stepped (a step back to 4800/4 moves the clock epoch too, as every RTL profile change does). */
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
}

static void
modulation_moves(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->rf_mod = 0;
}

static void
chain_step(dsd_opts* opts, dsd_state* state) {
    /* The P25p1 chain comparison's step, to the other chain and back. */
    frame_sync_p25p1_set_chain(opts, state, 0);
    frame_sync_p25p1_set_chain(opts, state, 1);
}

static void
acquisition_reset(dsd_opts* opts, dsd_state* state) {
    /* A scan row or target, an input boundary. */
    dsd_frame_sync_reset_acquisition(opts, state, 0);
    state->rf_mod = 1;
}

static void
anchor_forgotten(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    /* The no-carrier pass (engine.c), a trunk scan target's restore. */
    dsd_state_forget_p25p1_sync_anchor(state);
}

static void
other_sync_accepted(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    frame_sync_p25p1_anchor_note_accept(state, DSD_SYNC_X2TDMA_DATA_POS);
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
        {chain_step, "chain step"},
        {acquisition_reset, "acquisition reset"},
        {anchor_forgotten, "forget (no carrier, scan restore)"},
        {other_sync_accepted, "another protocol's sync"},
        {off_thread_tune_noted, "pending off-thread tune"},
    };

    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, 1, window);
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        setup(&opts, &state);
        anchored(&opts, &state);
        changes[i].change(&opts, &state);
        const int sync = match_at(&opts, &state, window, EXPECTED);
        if (sync != DSD_SYNC_NONE) {
            DSD_FPRINTF(stderr, "anchor survived: %s\n", changes[i].name);
        }
        assert(sync == DSD_SYNC_NONE);
        if (g_pending_tune) {
            (void)dsd_frame_sync_apply_off_thread_tunes(&opts, &state);
            g_pending_tune = 0;
        }
        teardown(&state);
    }
    /* Control: nothing changed, the word is taken. */
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P1_POS);
    teardown(&state);
}

/* The sync-time context is captured when the sync is accepted and checked again when the frame reports it validated:
   a change in between, a decoder that read past the unit, an unknown length or no usable NAC install nothing. */
static void
test_installation(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const stream_change_fn changes[] = {
        epoch_moves,       input_moves,   carrier_moves,    tune_completes,
        histories_dropped, profile_moves, modulation_moves, off_thread_tune_noted,
    };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        setup(&opts, &state);
        assert(match_at(&opts, &state, P25P1_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P1_POS);
        assert(state.p25_p1_sync_anchor.frame_valid == 1U);
        changes[i](&opts, &state);
        state.symbol_clock = ANCHOR_SYNC + 158;
        dsd_frame_sync_p25p1_frame_decoded(&state, NAC, TSDU_1);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        assert(state.p25_p1_sync_anchor.frame_valid == 0U);
        if (g_pending_tune) {
            (void)dsd_frame_sync_apply_off_thread_tunes(&opts, &state);
            g_pending_tune = 0;
        }
        teardown(&state);
    }

    /* A sync accepted while an off-thread tune is pending captures nothing. */
    setup(&opts, &state);
    off_thread_tune_noted(&opts, &state);
    assert(match_at(&opts, &state, P25P1_SYNC, ANCHOR_SYNC) == DSD_SYNC_P25P1_POS);
    assert(state.p25_p1_sync_anchor.frame_valid == 0U);
    (void)dsd_frame_sync_apply_off_thread_tunes(&opts, &state);
    g_pending_tune = 0;
    state.symbol_clock = ANCHOR_SYNC + 158;
    dsd_frame_sync_p25p1_frame_decoded(&state, NAC, TSDU_1);
    assert(state.p25_p1_sync_anchor.valid == 0U);
    teardown(&state);

    /* The decoder read past the unit's end: the hunt starts beyond the expected position. Up to it is fine. */
    setup(&opts, &state);
    validated_frame(&opts, &state, ANCHOR_SYNC, TSDU_1, TSDU_1 + 1, NAC);
    assert(state.p25_p1_sync_anchor.valid == 0U);
    validated_frame(&opts, &state, ANCHOR_SYNC, TSDU_1, TSDU_1, NAC);
    assert(state.p25_p1_sync_anchor.valid == 1U);
    teardown(&state);

    /* No known length, no usable NAC (0x000, 0xFFF, none), or no frame captured (reported twice): nothing. */
    static const struct {
        int nac;
        uint32_t unit;
    } refused[] = {{NAC, 0U}, {0x000, TSDU_1}, {0xFFF, TSDU_1}, {-1, TSDU_1}};

    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        setup(&opts, &state);
        validated_frame(&opts, &state, ANCHOR_SYNC, refused[i].unit, 100, refused[i].nac);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        assert(state.p25_p1_sync_anchor.frame_valid == 0U);
        dsd_frame_sync_p25p1_frame_decoded(&state, NAC, TSDU_1);
        assert(state.p25_p1_sync_anchor.valid == 0U);
        teardown(&state);
    }

    /* A validated frame behind a tolerant word anchors the next one in turn; a frame on another NAC replaces it. */
    setup(&opts, &state);
    anchored(&opts, &state);
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, 3, window);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P1_POS);
    state.symbol_clock = EXPECTED + 864;
    dsd_frame_sync_p25p1_frame_decoded(&state, NAC, 864);
    assert(anchored_nac(&state, NULL) == -1);
    assert(state.p25_p1_sync_anchor.valid == 1U && state.p25_p1_sync_anchor.expected_clock == EXPECTED + 864);
    validated_frame(&opts, &state, EXPECTED + 864, 396, 396, 0x2A1);
    assert(state.p25_p1_sync_anchor.valid == 1U && state.p25_p1_sync_anchor.nac == 0x2A1);
    assert(state.p25_p1_sync_anchor.expected_clock == EXPECTED + 864 + 396);
    teardown(&state);
}

/* The anchor's dibit map and polarity read the window and are what the frame is decoded under: an inverted N1200
   receiver's word, three bits off. */
static void
test_map_and_polarity_follow_the_anchor(void) {
    static dsd_opts opts;
    static dsd_state state;
    setup(&opts, &state);
    /* The validated frame's sync was the inverted word under N1200 (the rotated matcher's accept). */
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_N1200;
    state.symbol_clock = ANCHOR_SYNC + SYNC_DIBITS;
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_NEG, DSD_P25P1_SYNC_EXACT, -1, 0);
    state.symbol_clock = ANCHOR_SYNC + 158;
    dsd_frame_sync_p25p1_frame_decoded(&state, NAC, TSDU_1);
    assert(state.p25_p1_sync_anchor.valid == 1U);
    assert(state.p25_p1_sync_anchor.anchor.inverted == 1U);
    state.p25_cqpsk_dibit_map_idx = DSD_P25_CQPSK_DIBIT_MAP_IDENTITY;

    char corrected[SYNC_DIBITS + 1];
    char raw[SYNC_DIBITS + 1];
    word_with_bit_errors(INV_P25P1_SYNC, 3, corrected);
    for (int i = 0; i < SYNC_DIBITS; i++) {
        raw[i] = (char)('0'
                        + dsd_p25_cqpsk_raw_dibit_for_corrected(DSD_P25_CQPSK_DIBIT_MAP_N1200,
                                                                (uint8_t)(corrected[i] - '0')));
    }
    raw[SYNC_DIBITS] = '\0';
    assert(match_at(&opts, &state, raw, EXPECTED) == DSD_SYNC_P25P1_NEG);
    assert(state.p25_cqpsk_dibit_map_idx == DSD_P25_CQPSK_DIBIT_MAP_N1200);
    assert(state.lastsynctype == DSD_SYNC_P25P1_NEG);
    unsigned int bit_errors = 0U;
    assert(anchored_nac(&state, &bit_errors) == NAC);
    assert(bit_errors == 3U);
    assert(state.p25_p1_sync_anchor.frame.map_idx == DSD_P25_CQPSK_DIBIT_MAP_N1200);
    assert(state.p25_p1_sync_anchor.frame.inverted == 1U);
    teardown(&state);

    /* The same window under the identity map and positive polarity is no word at all. */
    setup(&opts, &state);
    anchored(&opts, &state);
    assert(match_at(&opts, &state, raw, EXPECTED) == DSD_SYNC_NONE);
    teardown(&state);
}

/* The levels the window's symbols left: a biased centre an exact accept refits from. */
static void
push_sync_levels(dsd_state* state, float bias) {
    for (int i = 0; i < SYNC_DIBITS; i++) {
        dsd_symbol_history_push(state, (P25P1_SYNC[i] == '1' ? 3.0f : -3.0f) + bias);
    }
}

/* A tolerant accept writes no centre at acceptance (CENTER_SKIP) and keeps no control channel alive
   (last_cc_sync_time); an exact accept does both. Only the acceptance is pinned: on the RTL CQPSK path the next symbol
   read replaces the centre either way. */
static void
test_tolerant_accept_withholds_sync_time_effects(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, 2, window);

    setup(&opts, &state);
    anchored(&opts, &state);
    push_sync_levels(&state, 0.5f);
    state.center = 0.0f;
    state.last_cc_sync_time = 0;
    state.last_cc_sync_time_m = 0.0;
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_P25P1_POS);
    assert(fabsf(state.center) < 1e-6f);
    assert(state.last_cc_sync_time == 0);
    assert(fabs(state.last_cc_sync_time_m) < 1e-9);
    teardown(&state);

    /* The exact word at the same place, with the same levels behind it. */
    setup(&opts, &state);
    anchored(&opts, &state);
    push_sync_levels(&state, 0.5f);
    state.center = 0.0f;
    state.last_cc_sync_time = 0;
    state.last_cc_sync_time_m = -1.0;
    assert(match_at(&opts, &state, P25P1_SYNC, EXPECTED) == DSD_SYNC_P25P1_POS);
    assert(fabsf(state.center - 0.5f) < 0.05f);
    assert(fabs(state.last_cc_sync_time_m + 1.0) > 1e-9);
    teardown(&state);
}

static int g_profile_pushes;
static int g_profile_cqpsk;
static int g_profile_rate;
static int g_profile_levels;

static int
capture_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    (void)channel_profile;
    (void)ted_sps;
    g_profile_pushes++;
    g_profile_cqpsk = cqpsk_enable;
    g_profile_rate = symbol_rate_hz;
    g_profile_levels = levels;
    return 0;
}

/* The tolerant accept @p bits bits off at the expected position, the modulation unlocked on an RTL stream: how many
   demodulator profiles it pushed. */
static int
tolerant_accept_profile_pushes(int bits) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, bits, window);
    setup(&opts, &state);
    anchored(&opts, &state);
    opts.mod_cli_lock = 0;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    dsd_rtl_stream_metrics_hooks hooks = {
        .cqpsk_status = cqpsk_status,
        .apply_demod_profile = capture_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    g_profile_pushes = 0;
    const int synctype = match_at(&opts, &state, window, EXPECTED);
    assert(synctype == (bits <= DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS ? DSD_SYNC_P25P1_POS : DSD_SYNC_NONE));
    const int pushes = g_profile_pushes;
    state.rtl_ctx = NULL;
    teardown(&state);
    return pushes;
}

/* On CQPSK with the modulation unlocked a P25p1 sync re-pushes the demodulator profile already decided, which re-asserts
   the chain and decides nothing (a P25p1 word is the same on both modulations): a tolerant accept does the same, once,
   for the 4800/4 CQPSK profile it is on. A word past the tolerance pushes nothing. */
static void
test_tolerant_accept_repushes_demod_profile(void) {
    assert(tolerant_accept_profile_pushes(3) == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(tolerant_accept_profile_pushes(DSD_P25P1_TOLERANT_SYNC_MAX_BIT_ERRORS + 1) == 0);
}

/* Off the RTL CQPSK 4-level path (C4FM, a symbol file) nothing is tolerated: the anchor is spent unscored. */
static void
test_cqpsk_path_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    char window[SYNC_DIBITS + 1];
    word_with_bit_errors(P25P1_SYNC, 1, window);
    setup(&opts, &state);
    anchored(&opts, &state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    assert(match_at(&opts, &state, window, EXPECTED) == DSD_SYNC_NONE);
    assert(state.p25_p1_sync_anchor.valid == 0U);
    teardown(&state);
}
#endif

int
main(void) {
    test_bit_distance();
#ifdef USE_RADIO
    test_expected_position_threshold();
    test_other_positions_exact_only();
    test_slips_with_real_neighbours();
    test_late_exact_word_of_any_map_wins();
    test_late_exact_word_of_another_protocol_wins();
    test_every_matcher_declares_its_words();
    test_no_exact_word_of_any_protocol_is_pre_empted();
    test_slipped_corrupted_word_one_symbol_early();
    test_exact_matcher_of_another_protocol_wins();
    test_invalidations();
    test_installation();
    test_map_and_polarity_follow_the_anchor();
    test_tolerant_accept_withholds_sync_time_effects();
    test_tolerant_accept_repushes_demod_profile();
    test_cqpsk_path_only();
#endif
    DSD_FPRINTF(stderr, "FRAME_SYNC_P25_ANCHOR: OK\n");
    return 0;
}
