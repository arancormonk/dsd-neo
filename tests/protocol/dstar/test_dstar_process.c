// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for D-STAR voice/header processing loop boundaries.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>

enum {
    DSTAR_VOICE_FRAMES = 21,
    DSTAR_VOICE_DIBITS_PER_FRAME = 72,
    DSTAR_SLOW_DATA_FRAMES = 20,
    DSTAR_SLOW_DATA_DIBITS_PER_FRAME = 24,
    DSTAR_FRAME_DIBITS = DSTAR_VOICE_DIBITS_PER_FRAME + DSTAR_SLOW_DATA_DIBITS_PER_FRAME,
    DSTAR_EXPECTED_VOICE_DIBITS = DSTAR_VOICE_FRAMES * DSTAR_VOICE_DIBITS_PER_FRAME,
    DSTAR_EXPECTED_SLOW_DIBITS = DSTAR_SLOW_DATA_FRAMES * DSTAR_SLOW_DATA_DIBITS_PER_FRAME,
};

/* Both readers share one symbol stream, so the position of each read shows its order. A read at position `pos`
 * returns dibit `pos & 3`, and the soft reader also hands out the symbol (pos + 1) / 4, from which the reliability
 * stub below recovers `pos` and reports expected_reliability(pos). */
static int stream_pos;
static int dibit_calls;
static int soft_symbol_calls;
static int mbe_frame_calls;
static int voice_play_calls;
static int slow_data_calls;
static int ui_calls;
/* Publishing now follows the installed telemetry hooks, not the frontend kind. */
static int telemetry_active;
static int watchdog_history_calls;
static int watchdog_current_calls;
static int header_decode_soft_calls;
/* What the stubbed header CRC reports; processDSTAR_HD() must hand it straight back. */
static int header_decode_soft_result = 1;
static uint8_t captured_slow_data[DSTAR_EXPECTED_SLOW_DIBITS];
static dsd_vocoder_soft_bit captured_ambe_frames[DSTAR_VOICE_FRAMES][4][24];
static float captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS];

/* Never 0, so a cell the reader left untouched ({0, 0}) cannot pass for one it filled. */
static uint8_t
expected_reliability(int pos) {
    return (uint8_t)(1 + ((pos * 37) % 254));
}

static void
reset_counters(void) {
    stream_pos = 0;
    dibit_calls = 0;
    soft_symbol_calls = 0;
    mbe_frame_calls = 0;
    voice_play_calls = 0;
    slow_data_calls = 0;
    ui_calls = 0;
    telemetry_active = 0;
    watchdog_history_calls = 0;
    watchdog_current_calls = 0;
    header_decode_soft_calls = 0;
    DSD_MEMSET(captured_slow_data, 0, sizeof(captured_slow_data));
    DSD_MEMSET(captured_ambe_frames, 0, sizeof(captured_ambe_frames));
    DSD_MEMSET(captured_soft_symbols, 0, sizeof(captured_soft_symbols));
}

/* Only the slow data is read hard (issue #599). */
int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    (void)out_analog_signal;
    int value = stream_pos & 3;
    stream_pos++;
    dibit_calls++;
    return value;
}

int
getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    (void)state;
    assert(out_soft_symbol != NULL);
    *out_soft_symbol = (float)(stream_pos + 1) * 0.25F;
    int value = stream_pos & 3;
    stream_pos++;
    soft_symbol_calls++;
    return value;
}

uint8_t
dsd_two_level_symbol_reliability(const dsd_opts* opts, const dsd_state* state, float symbol) {
    assert(opts != NULL);
    assert(state != NULL);
    return expected_reliability((int)(symbol * 4.0F) - 1);
}

/* The soft path is D-STAR voice's only way into the vocoder: no processMbeFrame stub exists, so a hard call fails to
 * link. */
void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    assert(imbe_fr == NULL);
    assert(ambe_fr != NULL);
    assert(imbe7100_fr == NULL);
    assert(mbe_frame_calls < DSTAR_VOICE_FRAMES);
    DSD_MEMCPY(captured_ambe_frames[mbe_frame_calls], ambe_fr, sizeof(captured_ambe_frames[0]));
    mbe_frame_calls++;
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_play_calls++;
}

void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_play_calls++;
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_play_calls++;
}

void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_play_calls++;
}

void
processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd) {
    (void)opts;
    (void)state;
    assert(sd != NULL);
    DSD_MEMCPY(captured_slow_data, sd, sizeof(captured_slow_data));
    slow_data_calls++;
}

int
dsd_telemetry_is_active(void) {
    return telemetry_active;
}

void
dsd_telemetry_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    ui_calls++;
}

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    assert(slot == 0);
    watchdog_history_calls++;
    watchdog_current_calls++;
}

int
dstar_header_decode_soft(struct dsd_state* state, const float soft_symbols[DSD_DSTAR_HEADER_CODED_BITS]) {
    (void)state;
    assert(soft_symbols != NULL);
    DSD_MEMCPY(captured_soft_symbols, soft_symbols, sizeof(captured_soft_symbols));
    header_decode_soft_calls++;
    return header_decode_soft_result;
}

static void
assert_voice_loop_counts(int header_symbols, int expected_ui_calls) {
    assert(soft_symbol_calls == header_symbols + DSTAR_EXPECTED_VOICE_DIBITS);
    assert(dibit_calls == DSTAR_EXPECTED_SLOW_DIBITS);
    assert(mbe_frame_calls == DSTAR_VOICE_FRAMES);
    assert(voice_play_calls == DSTAR_VOICE_FRAMES);
    assert(slow_data_calls == 1);
    assert(ui_calls == expected_ui_calls);
    assert(watchdog_history_calls == DSTAR_VOICE_FRAMES);
    assert(watchdog_current_calls == DSTAR_VOICE_FRAMES);
}

/* Every voice frame's 72 symbols land on the interleave schedule, each cell with its symbol's dibit folded to one bit
 * (dibits 2 and 3 exercise the fold) and the reliability reported for that symbol. The 24 cells the schedule never
 * reaches stay {0, 0}. `start` is the stream position of the first voice symbol: 0, or the header's length. */
static void
assert_ambe_frames_follow_interleave(int start) {
    for (int frame = 0; frame < DSTAR_VOICE_FRAMES; frame++) {
        dsd_vocoder_soft_bit expected[4][24];
        DSD_MEMSET(expected, 0, sizeof(expected));
        for (int i = 0; i < DSTAR_VOICE_DIBITS_PER_FRAME; i++) {
            const int pos = start + (frame * DSTAR_FRAME_DIBITS) + i;
            expected[dstar_interleave_w[i]][dstar_interleave_x[i]].bit = (uint8_t)(pos & 1);
            expected[dstar_interleave_w[i]][dstar_interleave_x[i]].reliability = expected_reliability(pos);
        }
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 24; col++) {
                assert(captured_ambe_frames[frame][row][col].bit == expected[row][col].bit);
                assert(captured_ambe_frames[frame][row][col].reliability == expected[row][col].reliability);
            }
        }
    }
}

/* Each voice frame but the last is followed by 24 hard slow-data symbols. */
static void
assert_slow_data_follows_each_voice_frame(int start) {
    for (int frame = 0; frame < DSTAR_SLOW_DATA_FRAMES; frame++) {
        for (int i = 0; i < DSTAR_SLOW_DATA_DIBITS_PER_FRAME; i++) {
            const int pos = start + (frame * DSTAR_FRAME_DIBITS) + DSTAR_VOICE_DIBITS_PER_FRAME + i;
            assert(captured_slow_data[(frame * DSTAR_SLOW_DATA_DIBITS_PER_FRAME) + i] == (uint8_t)(pos & 3));
        }
    }
}

static void
test_voice_process_without_telemetry(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();

    /* Unconfirmed on its own: one superframe whose slow data checked nothing is weak
     * evidence, and weak evidence has to repeat (#421). */
    assert(processDSTAR(&opts, &state) == 0);

    assert_voice_loop_counts(0, 0);
    assert(header_decode_soft_calls == 0);
    assert_ambe_frames_follow_interleave(0);
    assert_slow_data_follows_each_voice_frame(0);
}

static void
test_voice_process_with_telemetry_attached(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 2;
    reset_counters();
    telemetry_active = 1;

    processDSTAR(&opts, &state);

    assert_voice_loop_counts(0, DSTAR_VOICE_FRAMES);
    assert(header_decode_soft_calls == 0);
}

static void
test_header_process_captures_header_then_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();

    header_decode_soft_result = 1;
    assert(processDSTAR_HD(&opts, &state) == 1);

    assert(header_decode_soft_calls == 1);
    assert(captured_soft_symbols[0] == 0.25F);
    assert(captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS - 1] == 165.0F);
    assert_voice_loop_counts(DSD_DSTAR_HEADER_CODED_BITS, 0);
    assert_ambe_frames_follow_interleave(DSD_DSTAR_HEADER_CODED_BITS);
    assert_slow_data_follows_each_voice_frame(DSD_DSTAR_HEADER_CODED_BITS);
}

/* #391: the header CRC is the only verdict the D-STAR data path has, and processDSTAR_HD()
 * must report it unchanged -- it decodes the voice frame behind a failed header either way,
 * so the caller's only way to know those 1992 symbols validated nothing is this return. */
static void
test_header_process_reports_the_header_crc_verdict(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();

    header_decode_soft_result = 0;
    assert(processDSTAR_HD(&opts, &state) == 0);

    assert(header_decode_soft_calls == 1);
    /* The voice frame is consumed regardless: that is the point of reporting the failure. */
    assert_voice_loop_counts(DSD_DSTAR_HEADER_CODED_BITS, 0);
    header_decode_soft_result = 1;
}

/* #421: the 1992 symbols a superframe takes are the largest block any handler consumes, on
 * the profile where D-STAR is the only candidate. One is weak evidence -- an exact 24-symbol
 * sync word was matched, nothing more -- so the verdict only turns productive when a second
 * arrives before the carrier drops. */
static void
test_voice_process_confirms_on_the_second_superframe(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;

    reset_counters();
    assert(processDSTAR(&opts, &state) == 0);
    assert(state.dstar_confirm_weak_streak == 1);

    reset_counters();
    assert(processDSTAR(&opts, &state) == 1);
    assert(state.dstar_confirmed == 1);

    /* Sticky: a third superframe that proves nothing of its own stays productive, because a
     * confirmed transmission that fades is still decoding (#391). */
    reset_counters();
    assert(processDSTAR(&opts, &state) == 1);

    /* And it clears with the carrier. */
    dstar_confirm_reset(&state);
    assert(state.dstar_confirmed == 0);
    assert(state.dstar_confirm_weak_streak == 0);
}

/* A passing header CRC-16/X.25 is proof on its own, so the voice superframe behind it is
 * productive on the first call rather than the second (#421). */
static void
test_passing_header_confirms_the_voice_frame_behind_it(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();

    header_decode_soft_result = 1;
    assert(processDSTAR_HD(&opts, &state) == 1);
    assert(state.dstar_confirmed == 1);
}

/* A failed header leaves the transmission unproved: the superframe it consumed anyway is
 * weak evidence only, so it reports unconfirmed and the header's own verdict stands. */
static void
test_failed_header_leaves_the_transmission_pending(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();

    header_decode_soft_result = 0;
    assert(processDSTAR_HD(&opts, &state) == 0);
    assert(state.dstar_confirmed == 0);
    assert(state.dstar_confirm_weak_streak == 1);
    header_decode_soft_result = 1;
}

int
main(void) {
    test_voice_process_without_telemetry();
    test_voice_process_with_telemetry_attached();
    test_header_process_captures_header_then_voice();
    test_header_process_reports_the_header_crc_verdict();
    test_voice_process_confirms_on_the_second_superframe();
    test_passing_header_confirms_the_voice_frame_behind_it();
    test_failed_header_leaves_the_transmission_pending();
    printf("DSTAR_PROCESS: OK\n");
    return 0;
}
