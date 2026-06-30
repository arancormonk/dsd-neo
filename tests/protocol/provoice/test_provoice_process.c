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
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
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
static int play_fm_calls;
static int convert_calls;
static uintptr_t convert_base_addr;
static uint32_t convert_lengths[8];
static size_t convert_offsets[8];
static char captured_first_frame[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];

static void
reset_counters(void) {
    dibit_calls = 0;
    mbe_calls = 0;
    play_ms_calls = 0;
    play_fm_calls = 0;
    convert_calls = 0;
    convert_base_addr = 0U;
    DSD_MEMSET(convert_lengths, 0, sizeof(convert_lengths));
    DSD_MEMSET(convert_offsets, 0, sizeof(convert_offsets));
    DSD_MEMSET(captured_first_frame, 0, sizeof(captured_first_frame));
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    int value = dibit_calls & 1;
    dibit_calls++;
    return value;
}

uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    assert(BufferIn != NULL);
    assert(convert_calls < (int)(sizeof(convert_lengths) / sizeof(convert_lengths[0])));
    uintptr_t buffer_addr = (uintptr_t)BufferIn;
    if (convert_base_addr == 0U) {
        convert_base_addr = buffer_addr;
    }
    convert_offsets[convert_calls] = (size_t)(buffer_addr - convert_base_addr);
    convert_lengths[convert_calls] = BitLength;
    convert_calls++;

    uint64_t value = 0;
    for (uint32_t i = 0; i < BitLength && i < 64; i++) {
        value = (value << 1) | (uint64_t)(BufferIn[i] & 1U);
    }
    return value;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
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

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    play_fm_calls++;
}

static void
assert_convert_offsets_and_lengths(void) {
    assert(convert_calls == 4);
    assert(convert_offsets[0] == 0U);
    assert(convert_lengths[0] == 64U);
    assert(convert_offsets[1] == 64U);
    assert(convert_lengths[1] == 16U);
    assert(convert_offsets[2] == 80U);
    assert(convert_lengths[2] == 64U);
    assert(convert_offsets[3] == (size_t)54U * 8U);
    assert(convert_lengths[3] == 16U);
}

static void
assert_first_imbe_frame_uses_interleave_schedule(void) {
    int first_pair_start = PROVOICE_EXPECTED_HEADER_BITS;
    assert(captured_first_frame[provoice_interleave_w[0]][provoice_interleave_x[0]] == (char)(first_pair_start & 1));
    assert(captured_first_frame[provoice_interleave_w[1]][provoice_interleave_x[1]]
           == (char)((first_pair_start + 1) & 1));
    assert(captured_first_frame[provoice_interleave_w[5]][provoice_interleave_x[5]]
           == (char)((first_pair_start + 5) & 1));
}

static void
test_process_voice_integer_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.payload = 1;
    opts.floating_point = 0;
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    state.ea_mode = 1;
    state.lasttg = 100344;
    state.lastsrc = 321;
    state.edacs_site_id = 7;
    state.edacs_tuned_lcn = 4;
    reset_counters();

    processProVoice(&opts, &state);

    assert(dibit_calls == PROVOICE_EXPECTED_TOTAL_DIBITS);
    assert(mbe_calls == 4);
    assert(play_ms_calls == 4);
    assert(play_fm_calls == 0);
    assert_convert_offsets_and_lengths();
    assert_first_imbe_frame_uses_interleave_schedule();
}

static void
test_process_voice_float_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    opts.p25_trunk = 1;
    opts.p25_is_tuned = 1;
    state.ea_mode = 0;
    state.lastsrc = 0x123;
    state.edacs_site_id = 9;
    state.edacs_tuned_lcn = 6;
    reset_counters();

    processProVoice(&opts, &state);

    assert(dibit_calls == PROVOICE_EXPECTED_TOTAL_DIBITS);
    assert(mbe_calls == 4);
    assert(play_ms_calls == 0);
    assert(play_fm_calls == 4);
    assert_convert_offsets_and_lengths();
}

int
main(void) {
    test_process_voice_integer_playback();
    test_process_voice_float_playback();
    printf("PROVOICE_PROCESS: OK\n");
    return 0;
}
