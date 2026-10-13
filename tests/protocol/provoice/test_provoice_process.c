// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for ProVoice voice processing loop boundaries.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "provoice_frame.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/provoice/provoice.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <stdint.h>
#include <stdio.h>

enum {
    PROVOICE_EXPECTED_HEADER_BITS = 64 + 16 + 64,
    PROVOICE_EXPECTED_BF_PREFIX_BITS = 2 + 16,
    PROVOICE_EXPECTED_TRAILER_BITS = 2,
    PROVOICE_EXPECTED_TOTAL_DIBITS = PROVOICE_EXPECTED_HEADER_BITS + (2 * DSD_PROVOICE_FRAME_PAIR_DIBITS)
        + PROVOICE_EXPECTED_BF_PREFIX_BITS + PROVOICE_EXPECTED_TRAILER_BITS,
};

static int dibit_calls;
static int mbe_calls;
static int play_ms_calls;
static uint8_t expected_bits[PROVOICE_EXPECTED_TOTAL_DIBITS];
static dsd_provoice_imbe_frame captured_first_frame;
/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for that symbol and every one after it; -1 for none. */
static int interrupt_at = -1;

/* The stub reader hands out a symbol per bit whose value is the reliability the stub helper below reports for it,
 * so each frame cell shows which symbol's confidence it received. */
static uint8_t
expected_reliability(int stream_idx) {
    return (uint8_t)(1 + ((stream_idx * 37) % 254));
}

static void
fill_expected_bits(void) {
    uint16_t state = 0xACE1U;
    for (size_t i = 0; i < sizeof(expected_bits); i++) {
        uint8_t bit = (uint8_t)(state & 1U);
        expected_bits[i] = bit;
        state >>= 1U;
        if (bit != 0U) {
            state ^= 0xB400U;
        }
    }
}

static void
reset_counters(void) {
    dibit_calls = 0;
    mbe_calls = 0;
    play_ms_calls = 0;
    interrupt_at = -1;
    fill_expected_bits();
    DSD_MEMSET(captured_first_frame, 0, sizeof(captured_first_frame));
}

int
getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    assert(dibit_calls < PROVOICE_EXPECTED_TOTAL_DIBITS);
    assert(out_soft_symbol != NULL);
    if (dibit_calls == interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
    *out_soft_symbol = (float)expected_reliability(dibit_calls);
    int value = expected_bits[dibit_calls];
    dibit_calls++;
    return value;
}

uint8_t
dsd_two_level_symbol_reliability(const dsd_opts* opts, const dsd_state* state, float symbol) {
    assert(opts != NULL);
    assert(state != NULL);
    return (uint8_t)(int)symbol;
}

/* The soft path is ProVoice's only way into the vocoder: no processMbeFrame stub exists, so a hard call fails to
 * link. */
void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    assert(protocol == DSD_VOICE_PROTOCOL_PROVOICE);
    (void)opts;
    (void)state;
    assert(imbe_fr == NULL);
    assert(ambe_fr == NULL);
    assert(imbe7100_fr != NULL);
    if (mbe_calls == 0) {
        DSD_MEMCPY(captured_first_frame, imbe7100_fr, sizeof(captured_first_frame));
    }
    mbe_calls++;
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    play_ms_calls++;
}

/* The first frame's first segment takes the six symbols after the header, bit and reliability both. */
static void
assert_first_imbe_frame_uses_interleave_schedule(void) {
    int first_pair_start = PROVOICE_EXPECTED_HEADER_BITS;
    for (int i = 0; i < 6; i++) {
        const dsd_vocoder_soft_bit* cell = &captured_first_frame[provoice_interleave_w[i]][provoice_interleave_x[i]];
        assert(cell->bit == expected_bits[first_pair_start + i]);
        assert(cell->reliability == expected_reliability(first_pair_start + i));
    }
}

static void
test_process_voice_integer_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.payload = 1;
    opts.floating_point = 0;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.ea_mode = 1;
    state.edacs_site_id = 7;
    state.edacs_tuned_lcn = 4;
    reset_counters();

    /* Unconfirmed on its own: one frame proves only that its own sync word matched (#421). */
    assert(processProVoice(&opts, &state) == 0);

    assert(dibit_calls == PROVOICE_EXPECTED_TOTAL_DIBITS);
    assert(mbe_calls == 4);
    assert(play_ms_calls == 4);
    assert_first_imbe_frame_uses_interleave_schedule();
}

/* Float output (-y) plays through the same mono mixer: it only changes how the output writes the samples (#652). */
static void
test_process_voice_float_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.ea_mode = 0;
    state.edacs_site_id = 9;
    state.edacs_tuned_lcn = 6;
    reset_counters();

    assert(processProVoice(&opts, &state) == 0);

    assert(dibit_calls == PROVOICE_EXPECTED_TOTAL_DIBITS);
    assert(mbe_calls == 4);
    assert(play_ms_calls == 4);
}

/* #421: nothing inside the 736 dibits can fail a check, so a lone frame buys the 9600/2
 * profile nothing. Two in a row -- each behind its own exact 32-symbol sync word, which noise
 * does not supply twice before the carrier drops -- confirm the transmission. */
static void
test_voice_process_confirms_on_the_second_frame(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;

    reset_counters();
    assert(processProVoice(&opts, &state) == 0);
    assert(state.provoice_confirm_weak_streak == 1);

    reset_counters();
    assert(processProVoice(&opts, &state) == 1);
    assert(state.provoice_confirmed == 1);

    /* Sticky: a confirmed transmission stays productive. */
    reset_counters();
    assert(processProVoice(&opts, &state) == 1);

    /* And it clears with the carrier. */
    provoice_confirm_reset(&state);
    assert(state.provoice_confirmed == 0);
    assert(state.provoice_confirm_weak_streak == 0);
}

/* Each IMBE pair plays as soon as it is read. A Signal search that leaves the frame (its visit ended, or a command came),
   or a live read that gives up on a stalled input, cuts it: getSymbol() stands 0 in for every symbol from there, which
   against thresholds that do not centre on 0 reads as confident bits. No pair read across or after the cut reaches the
   vocoder or the speaker; the frame ends there, as when a pair does not load. */
static void
test_voice_frame_cut_by_an_input_interruption_plays_nothing_after_the_cut(void) {
    enum {
        FIRST_PAIR = PROVOICE_EXPECTED_HEADER_BITS,
        BF_PREFIX = FIRST_PAIR + DSD_PROVOICE_FRAME_PAIR_DIBITS,
        SECOND_PAIR = BF_PREFIX + PROVOICE_EXPECTED_BF_PREFIX_BITS,
        TRAILER = SECOND_PAIR + DSD_PROVOICE_FRAME_PAIR_DIBITS,
    };

    static const int cuts[6] = {
        0, 40, FIRST_PAIR + 100, BF_PREFIX + 3, SECOND_PAIR + DSD_PROVOICE_FRAME_PAIR_DIBITS - 1, TRAILER};
    static const int pairs_played[6] = {0, 0, 0, 1, 1, 2};
    static const int dibits_read[6] = {BF_PREFIX, BF_PREFIX, BF_PREFIX,
                                       TRAILER,   TRAILER,   PROVOICE_EXPECTED_TOTAL_DIBITS};
    for (int c = 0; c < 6; c++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        reset_counters();
        interrupt_at = cuts[c];

        (void)processProVoice(&opts, &state);

        assert(state.input_interrupted == 1);
        assert(mbe_calls == 2 * pairs_played[c]);
        assert(play_ms_calls == 2 * pairs_played[c]);
        assert(dibit_calls == dibits_read[c]);
    }
}

int
main(void) {
    test_process_voice_integer_playback();
    test_process_voice_float_playback();
    test_voice_process_confirms_on_the_second_frame();
    test_voice_frame_cut_by_an_input_interruption_plays_nothing_after_the_cut();
    printf("PROVOICE_PROCESS: OK\n");
    return 0;
}
