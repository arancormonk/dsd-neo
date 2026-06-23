// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for D-STAR voice/header processing loop boundaries.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/dstar/dstar.h>
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
    DSTAR_EXPECTED_VOICE_DIBITS = DSTAR_VOICE_FRAMES * DSTAR_VOICE_DIBITS_PER_FRAME,
    DSTAR_EXPECTED_SLOW_DIBITS = DSTAR_SLOW_DATA_FRAMES * DSTAR_SLOW_DATA_DIBITS_PER_FRAME,
};

static int dibit_calls;
static int soft_symbol_calls;
static int soft_mbe_calls;
static int slow_data_calls;
static int ui_calls;
static int watchdog_history_calls;
static int watchdog_current_calls;
static int header_decode_soft_calls;
static uint8_t captured_slow_data[DSTAR_EXPECTED_SLOW_DIBITS];
static char captured_ambe_frame[4][24];
static float captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS];

static void
reset_counters(void) {
    dibit_calls = 0;
    soft_symbol_calls = 0;
    soft_mbe_calls = 0;
    slow_data_calls = 0;
    ui_calls = 0;
    watchdog_history_calls = 0;
    watchdog_current_calls = 0;
    header_decode_soft_calls = 0;
    DSD_MEMSET(captured_slow_data, 0, sizeof(captured_slow_data));
    DSD_MEMSET(captured_ambe_frame, 0, sizeof(captured_ambe_frame));
    DSD_MEMSET(captured_soft_symbols, 0, sizeof(captured_soft_symbols));
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    int value = dibit_calls & 3;
    dibit_calls++;
    return value;
}

int
getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    (void)state;
    assert(out_soft_symbol != NULL);
    *out_soft_symbol = (float)(soft_symbol_calls + 1) * 0.25F;
    soft_symbol_calls++;
    return soft_symbol_calls & 3;
}

void
soft_mbe(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    assert(imbe_fr == NULL);
    assert(ambe_fr != NULL);
    assert(imbe7100_fr == NULL);
    if (soft_mbe_calls == 0) {
        DSD_MEMCPY(captured_ambe_frame, ambe_fr, sizeof(captured_ambe_frame));
    }
    soft_mbe_calls++;
}

void
processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd) {
    (void)opts;
    (void)state;
    assert(sd != NULL);
    DSD_MEMCPY(captured_slow_data, sd, sizeof(captured_slow_data));
    slow_data_calls++;
}

void
ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    ui_calls++;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    assert(slot == 0);
    watchdog_history_calls++;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    assert(slot == 0);
    watchdog_current_calls++;
}

void
dstar_header_decode_soft(struct dsd_state* state, const float soft_symbols[DSD_DSTAR_HEADER_CODED_BITS]) {
    (void)state;
    assert(soft_symbols != NULL);
    DSD_MEMCPY(captured_soft_symbols, soft_symbols, sizeof(captured_soft_symbols));
    header_decode_soft_calls++;
}

static void
assert_voice_loop_counts(int expected_ui_calls) {
    assert(dibit_calls == DSTAR_EXPECTED_VOICE_DIBITS + DSTAR_EXPECTED_SLOW_DIBITS);
    assert(soft_mbe_calls == DSTAR_VOICE_FRAMES);
    assert(slow_data_calls == 1);
    assert(ui_calls == expected_ui_calls);
    assert(watchdog_history_calls == DSTAR_VOICE_FRAMES);
    assert(watchdog_current_calls == DSTAR_VOICE_FRAMES);
}

static void
assert_first_ambe_frame_is_interleaved_lsb_stream(void) {
    assert(captured_ambe_frame[0][10] == 0);
    assert(captured_ambe_frame[0][22] == 1);
    assert(captured_ambe_frame[3][11] == 0);
    assert(captured_ambe_frame[2][9] == 1);
}

static void
assert_slow_data_starts_after_first_voice_frame(void) {
    for (int i = 0; i < DSTAR_SLOW_DATA_DIBITS_PER_FRAME; i++) {
        int expected = (DSTAR_VOICE_DIBITS_PER_FRAME + i) & 3;
        assert(captured_slow_data[i] == (uint8_t)expected);
    }
}

static void
test_voice_process_without_ncurses(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_counters();

    processDSTAR(&opts, &state);

    assert_voice_loop_counts(0);
    assert(soft_symbol_calls == 0);
    assert(header_decode_soft_calls == 0);
    assert_first_ambe_frame_is_interleaved_lsb_stream();
    assert_slow_data_starts_after_first_voice_frame();
}

static void
test_voice_process_with_ncurses_refresh(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.use_ncurses_terminal = 1;
    reset_counters();

    processDSTAR(&opts, &state);

    assert_voice_loop_counts(DSTAR_VOICE_FRAMES);
    assert(soft_symbol_calls == 0);
    assert(header_decode_soft_calls == 0);
}

static void
test_header_process_captures_header_then_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_counters();

    processDSTAR_HD(&opts, &state);

    assert(soft_symbol_calls == DSD_DSTAR_HEADER_CODED_BITS);
    assert(header_decode_soft_calls == 1);
    assert(captured_soft_symbols[0] == 0.25F);
    assert(captured_soft_symbols[DSD_DSTAR_HEADER_CODED_BITS - 1] == 165.0F);
    assert_voice_loop_counts(0);
}

int
main(void) {
    test_voice_process_without_ncurses();
    test_voice_process_with_ncurses_refresh();
    test_header_process_captures_header_then_voice();
    printf("DSTAR_PROCESS: OK\n");
    return 0;
}
