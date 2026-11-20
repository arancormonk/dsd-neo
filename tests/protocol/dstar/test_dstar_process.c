// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for D-STAR voice/header processing loop boundaries.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dstar_sd_state.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <dsd-neo/runtime/telemetry.h>
#include <math.h>
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
    /* The superframe's symbols up to the data frame after its last voice frame (the next resynchronization word). */
    DSTAR_SUPERFRAME_DIBITS = DSTAR_EXPECTED_VOICE_DIBITS + DSTAR_EXPECTED_SLOW_DIBITS,
    /* What an RF header takes before voice frame 0. */
    DSTAR_HEADER_DIBITS = 660,
    /* Behind an RF header: voice frame 0, then the word, then the superframe. */
    DSTAR_HEADER_VOICE_FRAMES = DSTAR_VOICE_FRAMES + 1,
    /* The default stream's bit after a superframe's last voice frame is 0 (dibit pos & 3 on an even position): neither
       place an end pattern could take there starts with it, so the decoder reads that one symbol and hands it back to
       the hunt. */
    DSTAR_DEFAULT_RESYNC_PROBE = 1,
    STREAM_SCRIPT_MAX = 8192,
};

/* JARL STD 6.0 section 4.1.2: the resynchronization word, and the end pattern's two 24-bit halves (0xAAAAAAAA135E). */
#define RESYNC_WORD   0xAAB468U
#define END_FIRST_24  0xAAAAAAU
#define END_SECOND_24 0xAA135EU

/* Both readers share one symbol stream, so the position of each read shows its order. A read at position `pos`
 * returns dibit `pos & 3`, and the soft reader also hands out the symbol (pos + 1) / 4, from which the reliability
 * stub below recovers `pos` and reports expected_reliability(pos). */
static int stream_pos;
/* What the stream holds at a position, when a test scripted it (a bit, 0 or 1); -1: the default, dibit pos & 3. */
static int8_t stream_script[STREAM_SCRIPT_MAX];
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
static dsd_vocoder_soft_bit captured_ambe_frames[DSTAR_HEADER_VOICE_FRAMES][4][24];
static float captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS];
/* The stream position whose read adopts a retune its capture recorded, where the carrier boundary moves the carrier
   count (dsd_engine_carrier_boundary(), issue #575); -1 for none. */
static int boundary_at = -1;
/* The stream position whose read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the
   interruption there and stands erasures in for that symbol and every one after it; -1 for none. */
static int interrupt_at = -1;
/* What the decoder handed back to the hunt (dsd_frame_sync_hand_back_symbols()), and where the stream then stood. */
static int hand_back_calls;
static int handed_back;
static int hand_back_pos;
/* The calls the end pattern ended (dsd_call_state_end_ex()), and the call slot 0 holds (dsd_call_state_get()). */
static int call_end_calls;
static int call_end_reason = -1;
static int call_present = 1;
static int call_protocol = DSD_SYNC_DSTAR_VOICE_POS;

static int
stream_value(int pos) {
    if (pos >= 0 && pos < STREAM_SCRIPT_MAX && stream_script[pos] >= 0) {
        return stream_script[pos];
    }
    return pos & 3;
}

/* @p count bits of @p bits, most significant first, from stream position @p at. */
static void
script_bits(int at, uint32_t bits, int count) {
    for (int i = 0; i < count; i++) {
        assert(at + i >= 0 && at + i < STREAM_SCRIPT_MAX);
        stream_script[at + i] = (int8_t)((bits >> (unsigned)(count - 1 - i)) & 1U);
    }
}

/* The 48-bit end pattern from @p at, with the bit @p flip of it (0-47) inverted, or none for -1. */
static void
script_end_pattern(int at, int flip) {
    script_bits(at, END_FIRST_24, 24);
    script_bits(at + 24, END_SECOND_24, 24);
    if (flip >= 0) {
        stream_script[at + flip] = (int8_t)(stream_script[at + flip] ^ 1);
    }
}

static int
float_near(float a, float b) {
    return fabsf(a - b) < 1e-4F;
}

static void
note_read(dsd_state* state) {
    if (stream_pos == boundary_at && state != NULL) {
        state->carrier_seq++;
    }
    if (stream_pos == interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
}

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
    boundary_at = -1;
    interrupt_at = -1;
    DSD_MEMSET(stream_script, 0xFF, sizeof(stream_script));
    hand_back_calls = 0;
    handed_back = 0;
    hand_back_pos = -1;
    call_end_calls = 0;
    call_end_reason = -1;
    call_present = 1;
    call_protocol = DSD_SYNC_DSTAR_VOICE_POS;
    DSD_MEMSET(captured_slow_data, 0, sizeof(captured_slow_data));
    DSD_MEMSET(captured_ambe_frames, 0, sizeof(captured_ambe_frames));
    DSD_MEMSET(captured_soft_symbols, 0, sizeof(captured_soft_symbols));
}

/* Only the slow data is read hard (issue #599). */
int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)out_analog_signal;
    note_read(state);
    int value = stream_value(stream_pos);
    stream_pos++;
    dibit_calls++;
    return value;
}

int
getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    note_read(state);
    assert(out_soft_symbol != NULL);
    *out_soft_symbol = (float)(stream_pos + 1) * 0.25F;
    int value = stream_value(stream_pos);
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
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    assert(protocol == DSD_VOICE_PROTOCOL_DSTAR);
    (void)opts;
    (void)state;
    assert(imbe_fr == NULL);
    assert(ambe_fr != NULL);
    assert(imbe7100_fr == NULL);
    assert(mbe_frame_calls < DSTAR_HEADER_VOICE_FRAMES);
    DSD_MEMCPY(captured_ambe_frames[mbe_frame_calls], ambe_fr, sizeof(captured_ambe_frames[0]));
    mbe_frame_calls++;
}

/* A verified header ends the capture of the transmission before it (issue #661). */
static int capture_close_calls;
static int capture_close_voice_frames_before = -1;
static int capture_close_synctype = -1;

void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    (void)opts;
    (void)state;
    assert(slot_mask == DSD_MBE_OUT_SLOT1);
    capture_close_calls++;
    capture_close_voice_frames_before = mbe_frame_calls;
    capture_close_synctype = synctype;
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

void
dsd_frame_sync_hand_back_symbols(dsd_state* state, int symbols) {
    assert(state != NULL);
    hand_back_calls++;
    handed_back = symbols;
    hand_back_pos = stream_pos;
}

int
dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    assert(state != NULL);
    assert(slot == 0U);
    DSD_MEMSET(out, 0, sizeof(*out));
    out->protocol = call_protocol;
    out->phase = DSD_CALL_PHASE_ACTIVE;
    return call_present;
}

int
dsd_call_state_end_ex(dsd_state* state, uint8_t slot, double observed_m, dsd_call_end_reason reason) {
    assert(state != NULL);
    assert(slot == 0U);
    (void)observed_m;
    call_end_calls++;
    call_end_reason = (int)reason;
    return 1;
}

int
dstar_header_decode_soft(struct dsd_state* state, const float soft_symbols[DSD_DSTAR_HEADER_CODED_BITS]) {
    (void)state;
    assert(soft_symbols != NULL);
    DSD_MEMCPY(captured_soft_symbols, soft_symbols, sizeof(captured_soft_symbols));
    header_decode_soft_calls++;
    return header_decode_soft_result;
}

/* A superframe read whole (@p header: behind an RF header, so voice frame 0 and the resynchronization word first):
 * @p voice_frames of its voice frames reached the vocoder and the speaker (all of a confirmed transmission's, none of
 * another), every symbol was read and the slow data decoded either way, and the start of the next word went back to
 * the hunt. */
static void
assert_voice_loop_counts(int header, int expected_ui_calls, int voice_frames) {
    const int frames = header ? DSTAR_HEADER_VOICE_FRAMES : DSTAR_VOICE_FRAMES;
    assert(soft_symbol_calls == (header ? DSTAR_HEADER_DIBITS : 0) + (frames * DSTAR_VOICE_DIBITS_PER_FRAME));
    assert(dibit_calls
           == (header ? DSTAR_SLOW_DATA_DIBITS_PER_FRAME : 0) + DSTAR_EXPECTED_SLOW_DIBITS
                  + DSTAR_DEFAULT_RESYNC_PROBE);
    assert(mbe_frame_calls == voice_frames);
    assert(voice_play_calls == voice_frames);
    assert(slow_data_calls == 1);
    assert(ui_calls == expected_ui_calls);
    assert(watchdog_history_calls == frames);
    assert(watchdog_current_calls == frames);
    assert(hand_back_calls == 1);
    assert(handed_back == DSTAR_DEFAULT_RESYNC_PROBE);
    assert(call_end_calls == 0);
}

/* Every voice frame's 72 symbols land on the interleave schedule, each cell with its symbol's dibit folded to one bit
 * (dibits 2 and 3 exercise the fold) and the reliability reported for that symbol. The 24 cells the schedule never
 * reaches stay {0, 0}. `start` is the stream position of the first voice symbol: 0, or the header's length. */
static void
assert_ambe_frames_follow_interleave(int start, int frames) {
    for (int frame = 0; frame < frames; frame++) {
        dsd_vocoder_soft_bit expected[4][24];
        DSD_MEMSET(expected, 0, sizeof(expected));
        for (int i = 0; i < DSTAR_VOICE_DIBITS_PER_FRAME; i++) {
            const int pos = start + (frame * DSTAR_FRAME_DIBITS) + i;
            expected[dstar_interleave_w[i]][dstar_interleave_x[i]].bit = (uint8_t)(stream_value(pos) & 1);
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
            assert(captured_slow_data[(frame * DSTAR_SLOW_DATA_DIBITS_PER_FRAME) + i] == (uint8_t)stream_value(pos));
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
     * evidence, and weak evidence has to repeat (#421). Its voice plays nothing and opens no call: one sync word on
     * receiver noise is no call. */
    assert(processDSTAR(&opts, &state) == 0);

    assert_voice_loop_counts(0, 0, 0);
    assert(header_decode_soft_calls == 0);
    assert_slow_data_follows_each_voice_frame(0);
    assert(hand_back_pos == DSTAR_SUPERFRAME_DIBITS + DSTAR_DEFAULT_RESYNC_PROBE);

    /* The next superframe confirms the transmission as it starts, and plays. */
    reset_counters();
    assert(processDSTAR(&opts, &state) == 1);
    assert_voice_loop_counts(0, 0, DSTAR_VOICE_FRAMES);
    assert_ambe_frames_follow_interleave(0, DSTAR_VOICE_FRAMES);
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
    state.dstar_confirmed = 1; // a transmission its header confirmed

    processDSTAR(&opts, &state);

    assert_voice_loop_counts(0, DSTAR_VOICE_FRAMES, DSTAR_VOICE_FRAMES);
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
    assert(float_near(captured_soft_symbols[0], 0.25F));
    assert(float_near(captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS - 1], 165.0F));
    /* Voice frame 0 and the 21 behind its word: no 20 ms lost. */
    assert_voice_loop_counts(1, 0, DSTAR_HEADER_VOICE_FRAMES);
    assert_ambe_frames_follow_interleave(DSTAR_HEADER_DIBITS, DSTAR_HEADER_VOICE_FRAMES);
    /* The slow data starts behind the word, in voice frame 1's data frame. */
    assert_slow_data_follows_each_voice_frame(DSTAR_HEADER_DIBITS + DSTAR_FRAME_DIBITS);
    assert(hand_back_pos == DSTAR_HEADER_DIBITS + DSTAR_FRAME_DIBITS + DSTAR_SUPERFRAME_DIBITS + 1);
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
    /* The voice frame is consumed regardless: that is the point of reporting the failure. Nothing confirmed it, so
     * none of it plays. */
    assert_voice_loop_counts(1, 0, 0);
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

    capture_close_calls = 0;
    capture_close_voice_frames_before = -1;
    state.synctype = DSD_SYNC_DSTAR_HD_NEG;
    header_decode_soft_result = 1;
    assert(processDSTAR_HD(&opts, &state) == 1);
    assert(state.dstar_confirmed == 1);
    /* It also ends the capture before any voice frame of its own superframe is written (issue #661). */
    assert(capture_close_calls == 1);
    assert(capture_close_voice_frames_before == 0);
    assert(DSD_SYNC_IS_DSTAR(capture_close_synctype));
}

/* A failed header leaves the transmission unproved: the superframe it consumed anyway is weak evidence only, and only
 * when the resynchronization word in voice frame 0's data frame was there (the header's own sync proves the header was
 * seen, not the word), so it reports unconfirmed and the header's own verdict stands. */
static void
test_failed_header_leaves_the_transmission_pending(void) {
    for (int word = 0; word <= 1; word++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        if (word) {
            script_bits(DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME, RESYNC_WORD, 24);
        }

        capture_close_calls = 0;
        header_decode_soft_result = 0;
        assert(processDSTAR_HD(&opts, &state) == 0);
        assert(state.dstar_confirmed == 0);
        assert(state.dstar_confirm_weak_streak == (uint8_t)word);
        /* A header that fails its CRC bounds nothing: the capture goes on. */
        assert(capture_close_calls == 0);
        header_decode_soft_result = 1;
    }
}

/* Issue #575: the carrier boundary moved the carrier count while the superframe was read (a replay read adopting a
   recorded retune). Its slow data decodes only after the twenty-first voice frame, so a header, text or position
   message read whole before the move would publish the carrier left's station after the boundary ended its call: the
   superframe is dropped from the voice frame read across or after the move, slow data and all, as on a sync loss. */
static void
test_voice_superframe_split_by_a_carrier_boundary_is_dropped(void) {
    static const int boundaries[3] = {
        (10 * DSTAR_FRAME_DIBITS) + 30,                                /* inside the eleventh voice frame */
        (19 * DSTAR_FRAME_DIBITS) + DSTAR_VOICE_DIBITS_PER_FRAME + 10, /* inside the last slow data */
        (20 * DSTAR_FRAME_DIBITS) + 5,                                 /* inside the last voice frame */
    };
    static const int voice_frames_before[3] = {10, 20, 20};
    for (int b = 0; b < 3; b++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        state.dstar_confirmed = 1; // a confirmed transmission's superframe, whose voice plays up to the cut
        boundary_at = boundaries[b];

        (void)processDSTAR(&opts, &state);

        assert(state.carrier_seq == 1U);
        assert(slow_data_calls == 0);
        assert(mbe_frame_calls == voice_frames_before[b]);
        assert(voice_play_calls == voice_frames_before[b]);
        assert(soft_symbol_calls == (voice_frames_before[b] + 1) * DSTAR_VOICE_DIBITS_PER_FRAME);
    }
}

/* A Signal search left the superframe (its visit ended, or a command came) or a live read gave up on a stalled input:
   getSymbol() stands 0 in for every symbol from there, and against thresholds that do not centre on 0 those read as
   confident bits. Nothing from the voice frame read across or after the cut reaches the vocoder or the speaker, and the
   slow data, which decodes only after the last voice frame, is dropped with it, as on a carrier boundary. */
static void
test_voice_superframe_cut_by_an_input_interruption_is_dropped(void) {
    static const int cuts[4] = {
        0,                                                             /* the first voice symbol */
        (10 * DSTAR_FRAME_DIBITS) + 30,                                /* inside the eleventh voice frame */
        (19 * DSTAR_FRAME_DIBITS) + DSTAR_VOICE_DIBITS_PER_FRAME + 10, /* inside the last slow data */
        (20 * DSTAR_FRAME_DIBITS) + 71,                                /* the last voice symbol */
    };
    static const int voice_frames_before[4] = {0, 10, 20, 20};
    for (int c = 0; c < 4; c++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        state.dstar_confirmed = 1; // a confirmed transmission's superframe, whose voice plays up to the cut
        interrupt_at = cuts[c];

        (void)processDSTAR(&opts, &state);

        assert(state.input_interrupted == 1);
        assert(state.carrier_seq == 0U);
        assert(slow_data_calls == 0);
        assert(mbe_frame_calls == voice_frames_before[c]);
        assert(voice_play_calls == voice_frames_before[c]);
        assert(soft_symbol_calls == (voice_frames_before[c] + 1) * DSTAR_VOICE_DIBITS_PER_FRAME);
    }
}

/* The header's CRC-16 is checked after a soft Viterbi decode that reads the stood-in zeros as confident symbols. A
   header an interruption cut decodes nothing: no header, no proof of the transmission, no capture boundary, and the
   superframe behind it, which would read only erasures, is not read. */
static void
test_header_cut_by_an_input_interruption_decodes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();
    capture_close_calls = 0;
    header_decode_soft_result = 1;
    state.synctype = DSD_SYNC_DSTAR_HD_POS;
    interrupt_at = 400;

    assert(processDSTAR_HD(&opts, &state) == 0);

    assert(header_decode_soft_calls == 0);
    assert(capture_close_calls == 0);
    assert(state.dstar_confirmed == 0);
    assert(state.dstar_confirm_weak_streak == 0);
    assert(soft_symbol_calls == DSD_DSTAR_HEADER_CODED_BITS);
    assert(mbe_frame_calls == 0);
    assert(voice_play_calls == 0);
    assert(slow_data_calls == 0);
}

/* A carrier boundary inside the header (issue #575) drops it the same way: the rest of it is another carrier's. */
static void
test_header_split_by_a_carrier_boundary_decodes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();
    capture_close_calls = 0;
    header_decode_soft_result = 1;
    state.synctype = DSD_SYNC_DSTAR_HD_POS;
    interrupt_at = -1;
    boundary_at = 400;

    assert(processDSTAR_HD(&opts, &state) == 0);

    assert(state.carrier_seq == 1U);
    assert(state.input_interrupted == 0);
    assert(header_decode_soft_calls == 0);
    assert(capture_close_calls == 0);
    assert(state.dstar_confirmed == 0);
    assert(mbe_frame_calls == 0);
    assert(voice_play_calls == 0);
    assert(slow_data_calls == 0);
}

/* Issue #800 2.10: every RF header opens a transmission (dsd_state::dstar_tx_seq), whether its CRC passed or not, so
 * what slow data the one before it left half assembled -- a D-PRS record (one missing only its CR would otherwise be
 * finished by this transmission and published as its position) or a text message -- is forgotten, and no slow data
 * reaches a call until a checked header names the transmission's MY callsign (dsd_state::dstar_tx_call_epoch; the
 * real header decode names it: DSTAR_DPRS). */
static void
test_every_header_forgets_a_half_collected_dprs_record(void) {
    static dsd_opts opts;
    static dsd_state state;
    for (int verified = 0; verified <= 1; verified++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        header_decode_soft_result = verified;
        dstar_sd_state* sd = dstar_sd_st(&state);
        assert(sd != NULL);
        sd->dprs.collecting = 1U;
        sd->dprs.len = 12U;
        sd->dprs.bind.call_epoch = 7U;
        DSD_MEMCPY(sd->dprs.text, "$$CRC9E3E,N0", 12U);
        sd->text.places = 1U;
        state.dstar_tx_seq = 3U;
        state.dstar_tx_call_epoch = 7U;

        assert(processDSTAR_HD(&opts, &state) == verified);
        /* The stub decode names no callsign, so neither outcome names the transmission. */
        assert(state.dstar_tx_seq == 4U);
        assert(state.dstar_tx_call_epoch == 0U);
        assert(sd->dprs.collecting == 0U);
        assert(sd->dprs.len == 0U);
        assert(sd->dprs.bind.call_epoch == 0U);
        assert(sd->text.places == 0U);
        dsd_state_ext_free_all(&state);
    }
    header_decode_soft_result = 1;
}

/* Issue #890 (B11): behind an RF header the stream is voice frame 0 and then the resynchronization word in its data
 * frame (JARL STD 6.0 section 4.1.2 (c)). The header path reads both, so the superframe behind them starts at voice
 * frame 1 with every data frame where it belongs: the first 24 slow-data bits are voice frame 1's data frame, not the
 * word, and a header rebroadcast in the first superframe is read whole (DSTAR_HEADER_UTILS decodes it from these
 * bits). The 20th data frame and the voice frame after it, which the read used to leave to the hunt, are read too. */
static void
test_header_entry_reads_voice_zero_and_the_word_first(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_counters();
    const int word_at = DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME;
    script_bits(word_at, RESYNC_WORD, 24);
    header_decode_soft_result = 0;

    assert(processDSTAR_HD(&opts, &state) == 0);

    /* The word was there: the superframe behind it is the weak evidence it always was. */
    assert(state.dstar_confirm_weak_streak == 1);
    assert_voice_loop_counts(1, 0, 0);
    assert_slow_data_follows_each_voice_frame(word_at + DSTAR_SLOW_DATA_DIBITS_PER_FRAME);
    for (int i = 0; i < DSTAR_SLOW_DATA_DIBITS_PER_FRAME; i++) {
        /* None of the word reached the slow data. */
        assert(captured_slow_data[i] == (uint8_t)stream_value(word_at + DSTAR_FRAME_DIBITS + i));
    }
    header_decode_soft_result = 1;
}

/* The end of a transmission as the decoder saw it: one call end, with the terminator's reason, its capture closed, a
 * new transmission begun, nothing read past @p pattern_end, nothing handed to the hunt. */
static void
assert_ended_at(const dsd_state* state, int pattern_end, uint32_t tx_before) {
    assert(stream_pos == pattern_end);
    assert(call_end_calls == 1);
    assert(call_end_reason == (int)DSD_CALL_END_TERMINATOR);
    assert(capture_close_calls >= 1);
    assert(state->dstar_tx_seq == tx_before + 1U);
    assert(hand_back_calls == 0);
}

/* Issue #890 (B11): the end pattern (JARL STD 6.0 section 4.1.2 (g)-(h), 0xAAAAAAAA135E) ends the call as a terminator,
 * on the 48 bits ending on any step of the frames' 24-symbol grid: starting on a data frame (JARL's placement, the
 * DECODE_IQ_DSTAR capture's), on a voice frame (MMDVM's), 24 symbols into one, or ending with a data frame (MMDVMHost's end
 * frame). Nothing after it is read, so the next transmission's header is the hunt's. No CRC covers it: it is no
 * evidence, so an unconfirmed transmission stays unconfirmed. */
static void
test_end_pattern_ends_the_call_at_an_ordinary_position(void) {
    /* Where the pattern starts, how many voice frames played before it. Data frame 4 starts at 4 * 96 + 72. */
    static const int starts[4] = {
        (4 * DSTAR_FRAME_DIBITS) + DSTAR_VOICE_DIBITS_PER_FRAME, /* on data frame 4 */
        5 * DSTAR_FRAME_DIBITS,                                  /* on voice frame 5 */
        (5 * DSTAR_FRAME_DIBITS) + 24,                           /* 24 symbols into it */
        (5 * DSTAR_FRAME_DIBITS) + 48,                           /* ending with data frame 5 */
    };
    static const int played[4] = {5, 5, 5, 6};
    for (int c = 0; c < 4; c++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        capture_close_calls = 0;
        state.dstar_confirmed = 1; // a confirmed transmission: its voice plays up to the end
        state.dstar_tx_seq = 5U;
        script_end_pattern(starts[c], -1);

        assert(processDSTAR(&opts, &state) == 1);

        assert_ended_at(&state, starts[c] + 48, 5U);
        assert(mbe_frame_calls == played[c]);
        /* The superframe was cut: its slow data is dropped, as on a sync loss. */
        assert(slow_data_calls == 0);
    }

    /* Unconfirmed: the end proves nothing, so the superframe's own resynchronization word is all it has. */
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_counters();
    script_end_pattern(starts[0], -1);
    assert(processDSTAR(&opts, &state) == 0);
    assert(state.dstar_confirmed == 0);
    assert(state.dstar_confirm_weak_streak == 1);
    assert(mbe_frame_calls == 0);
    assert(call_end_calls == 1);

    /* Another protocol's call on the slot (a DMR call left open) is not D-STAR's to end. */
    reset_counters();
    DSD_MEMSET(&state, 0, sizeof(state));
    call_protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    script_end_pattern(starts[0], -1);
    (void)processDSTAR(&opts, &state);
    assert(stream_pos == starts[0] + 48);
    assert(call_end_calls == 0);
    assert(state.dstar_tx_seq == 1U);
}

/* A near miss is no end: one bit of the 48 off, in either half, and the superframe reads on whole. The 24 symbols read
 * after a data frame that held the pattern's first half are the next voice frame's: they decode as its first 24
 * symbols (lookahead kept). The next transmission is not touched either. */
static void
test_end_pattern_near_miss_reads_on(void) {
    static const int flips[3] = {0, 30, 47};
    for (int f = 0; f < 3; f++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        state.dstar_confirmed = 1;
        const int at = (4 * DSTAR_FRAME_DIBITS) + DSTAR_VOICE_DIBITS_PER_FRAME;
        script_end_pattern(at, flips[f]);

        assert(processDSTAR(&opts, &state) == 1);

        assert_voice_loop_counts(0, 0, DSTAR_VOICE_FRAMES);
        assert_ambe_frames_follow_interleave(0, DSTAR_VOICE_FRAMES);
        assert_slow_data_follows_each_voice_frame(0);
    }
}

/* The first data position behind an RF header is the word's: an end there (a header and one voice frame, a key-up
 * released at once) ends the call the header opened, as do the two placements around voice frame 0. Nothing after
 * the pattern is read: the next transmission's header is the hunt's, and a header read next starts right there. */
static void
test_end_pattern_ends_the_call_behind_a_header(void) {
    static const int starts[3] = {
        DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME,      /* on the word's data frame */
        DSTAR_HEADER_DIBITS,                                     /* on voice frame 0 */
        DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME - 24, /* ending with the word's data frame */
    };
    static const int played[3] = {1, 0, 1};
    for (int c = 0; c < 3; c++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        capture_close_calls = 0;
        header_decode_soft_result = 1;
        script_end_pattern(starts[c], -1);

        assert(processDSTAR_HD(&opts, &state) == 1);

        /* The header began transmission 1, its end begins 2. */
        assert_ended_at(&state, starts[c] + 48, 1U);
        assert(mbe_frame_calls == played[c]);
        assert(slow_data_calls == 0);

        /* The next transmission's header is read from the symbol after the pattern. */
        const int next = stream_pos;
        mbe_frame_calls = 0;
        assert(processDSTAR_HD(&opts, &state) == 1);
        assert(float_near(captured_soft_symbols[0], (float)(next + 1) * 0.25F));
    }
}

/* The data frame after the superframe's last voice frame is the next resynchronization word, which the hunt matches,
 * or the end pattern, which only the decoder reads: it reads that frame one symbol at a time while an end can still
 * be there, and hands what it read back to the hunt once none can. A word goes back after its 12th symbol, the first
 * that is no end pattern's; an end ending on the frame (behind a voice frame whose last 24 bits begin it) or 24
 * symbols after it ends the call there; after a 24-bit first half that the rest does not complete, every symbol read
 * goes back, the mismatch included. */
static void
test_end_pattern_at_the_hand_back_position(void) {
    static dsd_opts opts;
    static dsd_state state;
    const int slot = DSTAR_SUPERFRAME_DIBITS;

    /* The word: matched by the hunt where it ends. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_counters();
    state.dstar_confirmed = 1;
    script_bits(slot, RESYNC_WORD, 24);
    assert(processDSTAR(&opts, &state) == 1);
    assert(slow_data_calls == 1);
    assert(hand_back_calls == 1 && handed_back == 12 && hand_back_pos == slot + 12);
    assert(call_end_calls == 0);

    /* Ending on the frame: the last voice frame's last 24 bits and the frame. The superframe was whole: its slow data
       decodes before the call ends. */
    for (int after = 0; after <= 1; after++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_counters();
        capture_close_calls = 0;
        state.dstar_confirmed = 1;
        script_end_pattern(after ? slot : slot - 24, -1);
        assert(processDSTAR(&opts, &state) == 1);
        assert(slow_data_calls == 1);
        assert(mbe_frame_calls == DSTAR_VOICE_FRAMES);
        assert_ended_at(&state, after ? slot + 48 : slot + 24, 0U);
    }

    /* The first half, then a second half off by one bit: all 48 read go back. Off earlier, at its 6th bit: 30. */
    static const int flips[2] = {47, 29};
    static const int back[2] = {48, 30};
    for (int f = 0; f < 2; f++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_counters();
        state.dstar_confirmed = 1;
        script_end_pattern(slot, flips[f]);
        assert(processDSTAR(&opts, &state) == 1);
        assert(call_end_calls == 0);
        assert(hand_back_calls == 1 && handed_back == back[f] && hand_back_pos == slot + back[f]);
    }
}

/* Issue #575 behind a header: one carrier count for the whole read the header leads. A carrier boundary inside voice
 * frame 0 drops it; one inside the word's data frame (its first, middle and last symbol) drops everything after voice
 * frame 0, which was read whole on the header's carrier and plays: none of the superframe is read, played or
 * published, and nothing goes back to the hunt. */
static void
test_header_led_read_split_by_a_carrier_boundary_drops_what_follows(void) {
    static const int boundaries[4] = {
        DSTAR_HEADER_DIBITS + 30,
        DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME,
        DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME + 10,
        DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME + 23,
    };
    static const int played[4] = {0, 1, 1, 1};
    for (int b = 0; b < 4; b++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        header_decode_soft_result = 1;
        script_bits(DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME, RESYNC_WORD, 24);
        boundary_at = boundaries[b];

        assert(processDSTAR_HD(&opts, &state) == 1);

        assert(state.carrier_seq == 1U);
        assert(mbe_frame_calls == played[b]);
        assert(voice_play_calls == played[b]);
        assert(soft_symbol_calls == DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME);
        assert(slow_data_calls == 0);
        assert(hand_back_calls == 0);
        assert(call_end_calls == 0);
        /* No superframe was accounted: no weak evidence from a word read across the boundary. */
        assert(state.dstar_confirm_weak_streak == 0);
    }
}

/* A near miss at the first data position behind a header is no end either: the read goes on whole, and the 24 symbols
 * after the pattern's first half decode as voice frame 1's first 24. */
static void
test_end_pattern_near_miss_behind_a_header_reads_on(void) {
    static const int flips[3] = {0, 30, 47};
    for (int f = 0; f < 3; f++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.floating_point = 1;
        opts.pulse_digi_out_channels = 1;
        reset_counters();
        header_decode_soft_result = 1;
        script_end_pattern(DSTAR_HEADER_DIBITS + DSTAR_VOICE_DIBITS_PER_FRAME, flips[f]);

        assert(processDSTAR_HD(&opts, &state) == 1);

        assert_voice_loop_counts(1, 0, DSTAR_HEADER_VOICE_FRAMES);
        assert_ambe_frames_follow_interleave(DSTAR_HEADER_DIBITS, DSTAR_HEADER_VOICE_FRAMES);
        assert_slow_data_follows_each_voice_frame(DSTAR_HEADER_DIBITS + DSTAR_FRAME_DIBITS);
    }
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
    test_voice_superframe_split_by_a_carrier_boundary_is_dropped();
    test_voice_superframe_cut_by_an_input_interruption_is_dropped();
    test_header_cut_by_an_input_interruption_decodes_nothing();
    test_header_split_by_a_carrier_boundary_decodes_nothing();
    test_every_header_forgets_a_half_collected_dprs_record();
    test_header_entry_reads_voice_zero_and_the_word_first();
    test_end_pattern_ends_the_call_at_an_ordinary_position();
    test_end_pattern_near_miss_reads_on();
    test_end_pattern_ends_the_call_behind_a_header();
    test_end_pattern_at_the_hand_back_position();
    test_header_led_read_split_by_a_carrier_boundary_drops_what_follows();
    test_end_pattern_near_miss_behind_a_header_reads_on();
    printf("DSTAR_PROCESS: OK\n");
    return 0;
}
