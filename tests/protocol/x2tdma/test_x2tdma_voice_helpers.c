// SPDX-License-Identifier: ISC
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-signed-char-misuse,cert-str34-c,bugprone-suspicious-include)
/*
 * Focused coverage for X2-TDMA voice slot state, signaling, and AMBE dispatch.
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "x2tdma_frame.h"

static int g_get_queue[2048];
static int g_get_len;
static int g_get_pos;
static int g_skip_calls;
static int g_skip_total;
static int g_mbe_calls;
static int g_mbe_first_bit[32];
static int g_play_calls[4];
/* The read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); -1 for none. */
static int g_boundary_at = -1;

static void
reset_stubs(void) {
    DSD_MEMSET(g_get_queue, 0, sizeof(g_get_queue));
    DSD_MEMSET(g_mbe_first_bit, 0, sizeof(g_mbe_first_bit));
    DSD_MEMSET(g_play_calls, 0, sizeof(g_play_calls));
    g_get_len = 0;
    g_get_pos = 0;
    g_skip_calls = 0;
    g_skip_total = 0;
    g_mbe_calls = 0;
    g_boundary_at = -1;
}

static int
dibit_for_sync_char(char ch, int inverted) {
    const int decoded = (ch == '3') ? 2 : 0;
    return inverted ? (decoded ^ 2) : decoded;
}

static void
append_dibit(int dibit) {
    assert(g_get_len < (int)(sizeof(g_get_queue) / sizeof(g_get_queue[0])));
    g_get_queue[g_get_len++] = dibit;
}

static void
append_zeros(int count) {
    for (int i = 0; i < count; i++) {
        append_dibit(0);
    }
}

static void
append_sync(const char sync[25], int inverted) {
    for (int i = 0; i < 24; i++) {
        append_dibit(dibit_for_sync_char(sync[i], inverted));
    }
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)out_analog_signal;
    if (g_boundary_at >= 0 && g_get_pos == g_boundary_at && state != NULL) {
        state->carrier_seq++;
    }
    assert(g_get_pos >= 0 && g_get_pos < g_get_len);
    return g_get_queue[g_get_pos++];
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_calls++;
    g_skip_total += count;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)imbe7100_fr;
    assert(ambe_fr != NULL);
    assert(g_mbe_calls < (int)(sizeof(g_mbe_first_bit) / sizeof(g_mbe_first_bit[0])));
    g_mbe_first_bit[g_mbe_calls++] = ambe_fr[0][0];
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_calls[0]++;
}

void
playSynthesizedVoiceSS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_calls[1]++;
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_calls[2]++;
}

void
playSynthesizedVoiceFS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_calls[3]++;
}

#include "../../../src/protocol/x2tdma/x2tdma_voice.c"

static void
set_syncdata_from_sync(x2tdma_voice_ctx* ctx, const char sync[25]) {
    for (int i = 0; i < 24; i++) {
        ctx->syncdata[i] = dibit_for_sync_char(sync[i], 0);
        ctx->sync[i] = sync[i];
    }
    ctx->sync[24] = '\0';
    ctx->syncdata[24] = '\0';
}

static void
set_slot_from_cach(int dibits[144], int slot, int inverted) {
    int decoded = (slot == 0) ? 0 : 2;
    dibits[54 + 2] = inverted ? (decoded ^ 2) : decoded;
}

static void
set_slot_sync(int dibits[144], const char sync[25], int inverted) {
    for (int i = 0; i < 24; i++) {
        dibits[54 + 12 + 36 + 18 + i] = dibit_for_sync_char(sync[i], inverted);
    }
}

static void
set_slot_ambe_seed(int dibits[144]) {
    for (int i = 0; i < 36; i++) {
        dibits[54 + 12 + i] = (i & 1) ? 1 : 2;
    }
    for (int i = 0; i < 18; i++) {
        dibits[54 + 12 + 36 + i] = (i & 1) ? 3 : 0;
    }
}

static void
prepare_stream_after_slot(const char next_sync[25], int inverted) {
    append_zeros(18 + 36);
    append_zeros(12);
    append_sync(next_sync, inverted);
}

static void
test_slot_light_and_mute_helpers(void) {
    static dsd_state state;
    x2tdma_voice_ctx ctx;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));

    x2tdma_update_currentslot_from_cach(&state, 0);
    assert(state.currentslot == 0);
    assert(state.slot0light[0] == '[');
    assert(state.slot0light[6] == ']');
    assert(state.slot1light[0] == ' ');
    assert(state.slot1light[6] == ' ');

    x2tdma_update_currentslot_from_cach(&state, 2);
    assert(state.currentslot == 1);
    assert(state.slot1light[0] == '[');
    assert(state.slot1light[6] == ']');
    assert(state.slot0light[0] == ' ');
    assert(state.slot0light[6] == ' ');

    set_syncdata_from_sync(&ctx, X2TDMA_BS_DATA_SYNC);
    x2tdma_update_mute_and_lights(&ctx, &state);
    assert(ctx.mutecurrentslot == 1);
    assert(strcmp(state.slot1light, "[slot1]") == 0);

    set_syncdata_from_sync(&ctx, X2TDMA_BS_VOICE_SYNC);
    x2tdma_update_mute_and_lights(&ctx, &state);
    assert(ctx.mutecurrentslot == 0);
    assert(strcmp(state.slot1light, "[SLOT1]") == 0);

    ctx.msMode = 0;
    set_syncdata_from_sync(&ctx, X2TDMA_MS_VOICE_SYNC);
    x2tdma_update_ms_mode(&ctx);
    assert(ctx.msMode == 1);
}

static void
test_signaling_extracts_lc_mi_and_encryption_fields(void) {
    static dsd_state state;
    x2tdma_voice_ctx ctx;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(ctx.lcformat, '_', 8);
    DSD_MEMSET(ctx.mfid, '_', 8);
    DSD_MEMSET(ctx.lcinfo, '_', 56);
    dsd_x2tdma_init_mi_placeholder(ctx.mi);
    set_syncdata_from_sync(&ctx, X2TDMA_BS_VOICE_SYNC);
    ctx.syncdata[1] = 0;
    ctx.syncdata[2] = 0;

    ctx.eeei = 0;
    ctx.aiei = 0;
    x2tdma_decode_signal_j1(&ctx);
    x2tdma_decode_signal_j2(&ctx);
    x2tdma_decode_signal_j4(&ctx);
    assert(ctx.lcformat[0] == '1');
    assert(ctx.lcformat[7] == '1');
    assert(ctx.mfid[0] == '1');
    assert(ctx.lcinfo[55] == '0');
    assert(ctx.mi[0] == '_');

    dsd_x2tdma_init_mi_placeholder(ctx.mi);
    ctx.syncdata[1] = 1;
    ctx.syncdata[2] = 0;
    x2tdma_decode_signal_j1(&ctx);
    x2tdma_decode_signal_j2(&ctx);
    x2tdma_decode_signal_j4(&ctx);
    assert(ctx.mi[0] == '1');
    assert(ctx.mi[71] == '0');

    ctx.syncdata[1] = 0;
    x2tdma_decode_signal_j3(&ctx, &state);
    assert(strcmp(state.algid, "10001010") == 0);
    assert(strcmp(state.keyid, "1000100010101000") == 0);

    ctx.syncdata[1] = 1;
    x2tdma_decode_signal_j3(&ctx, &state);
    assert(strcmp(state.algid, "________") == 0);
    assert(strcmp(state.keyid, "________________") == 0);
}

static void
test_voice_frame_dispatch_gates_first_frame_and_mute(void) {
    static dsd_opts opts;
    static dsd_state state;
    x2tdma_voice_ctx ctx;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 1;
    reset_stubs();
    append_zeros(18 + 36);

    state.firstframe = 1;
    ctx.mutecurrentslot = 0;
    x2tdma_process_voice_frames(&opts, &state, &ctx);
    assert(state.firstframe == 0);
    assert(g_mbe_calls == 1);
    assert(g_play_calls[2] == 1);
    assert(g_get_pos == 54);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_stubs();
    append_zeros(18 + 36);
    state.firstframe = 0;
    ctx.mutecurrentslot = 0;
    x2tdma_process_voice_frames(&opts, &state, &ctx);
    assert(g_mbe_calls == 3);
    assert(g_play_calls[2] == 3);
    assert(g_get_pos == 54);

    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    reset_stubs();
    append_zeros(18 + 36);
    ctx.mutecurrentslot = 1;
    x2tdma_process_voice_frames(&opts, &state, &ctx);
    assert(g_mbe_calls == 0);
    assert(g_play_calls[2] == 1);
    assert(g_get_pos == 54);
}

static void
test_slot_iteration_voice_and_data_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    x2tdma_voice_ctx ctx;
    int dibits[144];
    int* dibit_p;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 2;
    DSD_MEMSET(dibits, 0, sizeof(dibits));
    reset_stubs();
    set_slot_from_cach(dibits, 0, opts.inverted_x2tdma);
    set_slot_ambe_seed(dibits);
    set_slot_sync(dibits, X2TDMA_BS_VOICE_SYNC, opts.inverted_x2tdma);
    prepare_stream_after_slot(X2TDMA_BS_DATA_SYNC, opts.inverted_x2tdma);
    dibit_p = dibits;

    x2tdma_process_slot_iteration(&opts, &state, &ctx, 0, &dibit_p);
    assert(dibit_p == dibits + 144);
    assert(state.currentslot == 0);
    assert(strcmp(state.slot0light, "[SLOT0]") == 0);
    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(ctx.mutecurrentslot == 0);
    assert(g_mbe_calls == 3);
    assert(g_play_calls[1] == 3);
    assert(g_skip_calls == 1);
    assert(g_skip_total == 54);
    assert(g_get_pos == g_get_len);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    DSD_MEMSET(dibits, 0, sizeof(dibits));
    reset_stubs();
    set_slot_from_cach(dibits, 1, opts.inverted_x2tdma);
    set_slot_sync(dibits, X2TDMA_MS_DATA_SYNC, opts.inverted_x2tdma);
    prepare_stream_after_slot(X2TDMA_BS_VOICE_SYNC, opts.inverted_x2tdma);
    dibit_p = dibits;

    x2tdma_process_slot_iteration(&opts, &state, &ctx, 0, &dibit_p);
    assert(state.currentslot == 1);
    assert(strcmp(state.slot1light, "[slot1]") == 0);
    assert(strcmp(state.slot0light, " slot0 ") == 0);
    assert(ctx.mutecurrentslot == 1);
    assert(ctx.msMode == 1);
    assert(g_mbe_calls == 0);
    assert(g_play_calls[1] == 1);
    assert(g_skip_calls == 1);
    assert(g_skip_total == 54);
}

/* A whole voice superframe: six slots on slot 0's voice sync, the first from the frame sync's buffer. */
static void
prepare_voice_superframe(int dibits[144]) {
    DSD_MEMSET(dibits, 0, sizeof(int) * 144U);
    set_slot_sync(dibits, X2TDMA_BS_VOICE_SYNC, 0);
    prepare_stream_after_slot(X2TDMA_BS_VOICE_SYNC, 0);
    for (int j = 1; j < 6; j++) {
        append_zeros(54 + 12 + 36 + 18);
        append_sync(X2TDMA_BS_VOICE_SYNC, 0);
        prepare_stream_after_slot(X2TDMA_BS_VOICE_SYNC, 0);
    }
}

/* Issue #575: the carrier boundary moved the carrier count while the superframe was read (a replay read adopting a
   recorded retune). The call's encryption, assembled from the slots' signalling, is published only after the sixth slot,
   so signalling read whole before the move would label the carrier left's call after the boundary ended it; the slot
   read across the move would decode AMBE frames read before it. From that slot on, nothing decodes or publishes, as on
   a sync loss. */
static void
test_voice_superframe_split_by_a_carrier_boundary_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int dibits[144];
    dsd_call_snapshot call;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 2;
    state.synctype = DSD_SYNC_X2TDMA_VOICE_POS;
    reset_stubs();
    prepare_voice_superframe(dibits);
    state.dibit_buf_p = dibits + 144;
    processX2TDMAvoice(&opts, &state);
    assert(g_mbe_calls == 18);
    assert(dsd_call_state_get(&state, 0, &call) > 0);
    assert(call.crypto != DSD_CALL_CRYPTO_UNKNOWN);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_X2TDMA_VOICE_POS;
    reset_stubs();
    prepare_voice_superframe(dibits);
    state.dibit_buf_p = dibits + 144;
    /* Inside the sixth slot's second AMBE half: its first frame and its signalling were read before the move. */
    g_boundary_at = 90 + (4 * 234) + 144 + 10;
    processX2TDMAvoice(&opts, &state);
    assert(state.carrier_seq == 1U);
    assert(g_mbe_calls == 15);
    assert(dsd_call_state_get(&state, 0, &call) > 0);
    assert(call.crypto == DSD_CALL_CRYPTO_UNKNOWN);
}

int
main(void) {
    test_slot_light_and_mute_helpers();
    test_signaling_extracts_lc_mi_and_encryption_fields();
    test_voice_frame_dispatch_gates_first_frame_and_mute();
    test_slot_iteration_voice_and_data_state();
    test_voice_superframe_split_by_a_carrier_boundary_publishes_nothing();
    printf("X2TDMA_VOICE_HELPERS: OK\n");
    return 0;
}

// NOLINTEND(bugprone-signed-char-misuse,cert-str34-c,bugprone-suspicious-include)
