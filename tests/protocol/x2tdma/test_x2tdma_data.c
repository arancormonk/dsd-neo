// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for the cached X2-TDMA data slot decoder.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <stdio.h>
#include <string.h>

static int g_skip_calls;
static int g_skip_total;

static void
reset_fixture(dsd_opts* opts, dsd_state* state, int dibits[90]) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(dibits, 0, 90U * sizeof(dibits[0]));
    state->dibit_buf_p = dibits + 90;
    g_skip_calls = 0;
    g_skip_total = 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_calls++;
    g_skip_total += count;
}

static int
dibit_for_sync_char(char ch, int inverted) {
    const int decoded = (ch == '3') ? 2 : 0;
    return inverted ? (decoded ^ 2) : decoded;
}

static void
set_sync(int dibits[90], const char sync[25], int inverted) {
    for (int i = 0; i < 24; i++) {
        dibits[66 + i] = dibit_for_sync_char(sync[i], inverted);
    }
}

static void
set_slot_from_cach(int dibits[90], int slot, int inverted) {
    int decoded = (slot == 0) ? 0 : 2;
    dibits[2] = inverted ? (decoded ^ 2) : decoded;
}

static void
set_bursttype(int dibits[90], const char bursttype[5], int inverted) {
    int first = ((bursttype[0] - '0') << 1) | (bursttype[1] - '0');
    int second = ((bursttype[2] - '0') << 1) | (bursttype[3] - '0');
    dibits[63] = inverted ? (first ^ 2) : first;
    dibits[64] = inverted ? (second ^ 2) : second;
}

static void
assert_common_skip(void) {
    assert(g_skip_calls == 1);
    assert(g_skip_total == 120);
}

static void
test_data_sync_marks_slot_and_maps_bursttype(void) {
    static dsd_opts opts;
    static dsd_state state;
    int dibits[90];

    reset_fixture(&opts, &state, dibits);
    opts.errorbars = 0;
    set_slot_from_cach(dibits, 0, opts.inverted_x2tdma);
    set_bursttype(dibits, "0110", opts.inverted_x2tdma);
    set_sync(dibits, X2TDMA_BS_DATA_SYNC, opts.inverted_x2tdma);

    processX2TDMAdata(&opts, &state);

    assert(state.currentslot == 0);
    assert(strcmp(state.slot0light, "[slot0]") == 0);
    assert(strcmp(state.fsubtype, " DATA Header  ") == 0);
    assert_common_skip();
}

static void
test_inverted_mobile_data_marks_slot_one(void) {
    static dsd_opts opts;
    static dsd_state state;
    int dibits[90];

    reset_fixture(&opts, &state, dibits);
    opts.inverted_x2tdma = 1;
    set_slot_from_cach(dibits, 1, opts.inverted_x2tdma);
    set_bursttype(dibits, "1001", opts.inverted_x2tdma);
    set_sync(dibits, X2TDMA_MS_DATA_SYNC, opts.inverted_x2tdma);

    processX2TDMAdata(&opts, &state);

    assert(state.currentslot == 1);
    assert(strcmp(state.slot1light, "[slot1]") == 0);
    assert(strcmp(state.fsubtype, " Slot idle    ") == 0);
    assert_common_skip();
}

static void
test_unknown_burst_without_data_sync_uses_blank_subtype(void) {
    static dsd_opts opts;
    static dsd_state state;
    int dibits[90];

    reset_fixture(&opts, &state, dibits);
    set_slot_from_cach(dibits, 0, opts.inverted_x2tdma);
    set_bursttype(dibits, "1111", opts.inverted_x2tdma);
    set_sync(dibits, X2TDMA_BS_VOICE_SYNC, opts.inverted_x2tdma);

    processX2TDMAdata(&opts, &state);

    assert(state.currentslot == 0);
    assert(state.slot0light[0] == '[');
    assert(state.slot0light[6] == ']');
    assert(strcmp(state.fsubtype, "              ") == 0);
    assert_common_skip();
}

int
main(void) {
    test_data_sync_marks_slot_and_maps_bursttype();
    test_inverted_mobile_data_marks_slot_one();
    test_unknown_burst_without_data_sync_uses_blank_subtype();
    printf("X2TDMA_DATA: OK\n");
    return 0;
}
