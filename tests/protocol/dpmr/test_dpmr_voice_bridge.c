// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for the active dPMR voice bridge into the NXDN scrambler path.
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dpmr/dpmr.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dpmr_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* One decoder for every case, on the heap (AGENTS.md: dsd_state and dsd_opts are heap-allocated; one dsd_state is about
   2 MiB): each case clears it before use, so none sees another's. */
static dsd_opts* g_opts;
static dsd_state* g_state;

static size_t g_mbe_calls;
static int g_ms_calls;
static int g_mbe_synctype[8];
static int g_mbe_cipher[8];
static int g_mbe_scrambled[8];
static unsigned long long g_mbe_mi[8];
static int g_dibit_calls;
/* The read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); -1 for none. */
static int g_boundary_at = -1;
/* The 24-bit channel code the first CC field carries (its twelve dibits follow the first CCH and TCH group); 0 sends
   zeros, which decode to no channel code. */
static uint32_t g_channel_code;
/* Nonzero: every Hamming block decodes to ones, so both CCH halves fail their CRC-7, as a noise frame's do. */
static int g_cch_noise;

enum { DPMR_TEST_FIRST_CC_DIBIT = 36 + 144, DPMR_TEST_CC_DIBITS = 12 };

/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for every symbol after it; -1 for none. */
static int g_interrupt_at = -1;
/* The CCH half (0 or 1) whose CRC-7 fails in every frame: its first Hamming block decodes a payload bit the zero CRC
   does not cover; -1 for none. The decoder runs six blocks per half, half 0's first. */
static int g_fail_half = -1;
static int g_hamming_calls;
/* Nonzero: the second CCH half decodes frame number 1 with its CRC-7 passing, so the frame's halves are the bound 0/1
   pair a called identity needs (TS 102 658 Tables 5.5, 5.6); the all-zero stream's halves are both numbered 0. */
static int g_second_half_frame_one;
/* Nonzero: both CCH halves fail their CRC-7 (each half's first block decodes a bit the zero CRC does not cover), their
   other fields reading voice. */
static int g_fail_both;
/* Nonzero: every CCH half reads Version 11, scrambled (TS 102 658 5.5.37), its CRC-7 computed over it. */
static int g_version3;
/* Nonzero: the halves are numbered 2 and 3 (the calling identity's pair), their CRC-7s computed over it. */
static int g_frames_two_three;
/* The Scramble steps the bridge took for frames it did not play (dsd_mbe_nxdn_scramble_skip_voice_frame()). */
static int g_skip_calls;

static void
reset_capture(void) {
    g_interrupt_at = -1;
    g_mbe_calls = 0;
    g_ms_calls = 0;
    DSD_MEMSET(g_mbe_synctype, 0, sizeof(g_mbe_synctype));
    DSD_MEMSET(g_mbe_cipher, 0, sizeof(g_mbe_cipher));
    DSD_MEMSET(g_mbe_scrambled, 0, sizeof(g_mbe_scrambled));
    DSD_MEMSET(g_mbe_mi, 0, sizeof(g_mbe_mi));
    g_dibit_calls = 0;
    g_boundary_at = -1;
    g_channel_code = 0U;
    g_fail_half = -1;
    g_hamming_calls = 0;
    g_cch_noise = 0;
    g_second_half_frame_one = 0;
    g_fail_both = 0;
    g_version3 = 0;
    g_frames_two_three = 0;
    g_skip_calls = 0;
}

/* The 49 Scramble steps the vocoder takes for an AMBE frame the NXDN decoder decodes (mbe_process_nxdn(): cipher 0x01,
   or a forced key, with a key loaded; the register loads the key when empty; LFSRN()'s 15-bit register),
   the frame decoded or skipped alike. Returns the register the frame is descrambled from, or the register unchanged
   when no Scramble applies. */
static unsigned long long
model_scramble_frame(dsd_state* state) {
    if (!((state->nxdn_cipher_type == 0x01 || state->M == 1) && dsd_key_scalar_present(state, 0))) {
        return state->nxdn_payload_mi;
    }
    if (state->nxdn_payload_mi == 0ULL) {
        state->nxdn_payload_mi = state->R;
    }
    const unsigned long long from = state->nxdn_payload_mi;
    unsigned lfsr = (unsigned)(from & 0x7FFFULL);
    for (int i = 0; i < 49; i++) {
        const unsigned bit = ((lfsr >> 1) ^ lfsr) & 1U;
        lfsr = (lfsr >> 1) | (bit << 14);
    }
    state->nxdn_payload_mi = lfsr;
    return from;
}

/* Where the Scramble register stands @p frames AMBE frames into a superframe keyed with @p key: the register the
   transmitter's frame @p frames is scrambled from. */
static unsigned long long
scramble_register_at(unsigned long long key, int frames) {
    dsd_state* scratch = (dsd_state*)calloc(1U, sizeof(*scratch));
    assert(scratch != NULL);
    scratch->R = key;
    scratch->nxdn_cipher_type = 0x01;
    unsigned long long at = 0ULL;
    for (int f = 0; f <= frames; f++) {
        at = model_scramble_frame(scratch);
    }
    free(scratch);
    return at;
}

void
dsd_mbe_nxdn_scramble_skip_voice_frame(dsd_state* state) {
    (void)model_scramble_frame(state);
    g_skip_calls++;
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)out_analog_signal;
    if (g_dibit_calls == g_boundary_at && state != NULL) {
        state->carrier_seq++;
    }
    if (g_dibit_calls == g_interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
    const int cc_dibit = g_dibit_calls - DPMR_TEST_FIRST_CC_DIBIT;
    g_dibit_calls++;
    if (cc_dibit >= 0 && cc_dibit < DPMR_TEST_CC_DIBITS) {
        return (int)((g_channel_code >> (22U - (2U * (uint32_t)cc_dibit))) & 3U);
    }
    return 0;
}

/* The 48 bits CCH half @p half decodes to under the knobs above: its 41 payload bits (frame number first, Version at
   17 and 18) and their CRC-7; a failing half's first bit flipped, which the CRC does not cover. */
static void
stub_half_bits(int half, unsigned char bits[48]) {
    DSD_MEMSET(bits, 0, 48U);
    bits[0] = (unsigned char)(g_frames_two_three ? 1U : 0U);
    bits[1] = (unsigned char)((half == 1 && (g_second_half_frame_one || g_frames_two_three)) ? 1U : 0U);
    if (g_version3) {
        bits[17] = 1U;
        bits[18] = 1U;
    }
    const uint8_t crc = dpmr_crc7(bits, 41U);
    for (int b = 0; b < 7; b++) {
        bits[41 + b] = (unsigned char)((crc >> (6 - b)) & 1U);
    }
    if ((g_fail_half >= 0 && half == g_fail_half) || g_fail_both) {
        bits[0] ^= 1U;
    }
}

bool
Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    (void)rxBits;
    if (decodedBits != NULL && nbCodewords > 0) {
        DSD_MEMSET(decodedBits, g_cch_noise ? 1 : 0, (size_t)nbCodewords * 8U);
        if (!g_cch_noise && nbCodewords == 1) {
            /* The decoder runs six blocks a half, half 0's first: block b carries bits 8b to 8b + 7. */
            unsigned char bits[48];
            stub_half_bits((g_hamming_calls % 12) / 6, bits);
            DSD_MEMCPY(decodedBits, bits + ((size_t)8 * (size_t)(g_hamming_calls % 6)), 8U);
        }
    }
    g_hamming_calls++;
    return true;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23], char ambe_fr[4][24],
                char imbe7100_fr[7][24]) {
    assert(protocol == DSD_VOICE_PROTOCOL_DPMR);
    (void)opts;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    if (g_mbe_calls < (sizeof(g_mbe_synctype) / sizeof(g_mbe_synctype[0]))) {
        g_mbe_synctype[g_mbe_calls] = state->synctype;
        g_mbe_cipher[g_mbe_calls] = (int)state->nxdn_cipher_type;
        g_mbe_scrambled[g_mbe_calls] = (state->dPMRVoiceFS2Frame.Scrambled[0] == DPMR_HALF_SCRAMBLED
                                        || state->dPMRVoiceFS2Frame.Scrambled[1] == DPMR_HALF_SCRAMBLED);
    }
    /* The register the frame is descrambled from, the Scramble stepping past it as the vocoder's does. */
    const unsigned long long from = model_scramble_frame(state);
    if (g_mbe_calls < (sizeof(g_mbe_mi) / sizeof(g_mbe_mi[0]))) {
        g_mbe_mi[g_mbe_calls] = from;
    }
    g_mbe_calls++;
}

/* As core/receive.c does: the owner generic output paths judge while dPMR plays through NXDN's decoder. */
dsd_receive_context
dsd_receive_context_begin(dsd_state* state, int protocol) {
    const dsd_receive_context saved = {state->receive_context_protocol, state->receive_context_active};
    state->receive_context_protocol = protocol;
    state->receive_context_active = 1;
    return saved;
}

void
dsd_receive_context_end(dsd_state* state, dsd_receive_context saved) {
    state->receive_context_protocol = saved.protocol;
    state->receive_context_active = saved.active;
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    /* Issue #637: playback is judged as dPMR's, scrambled halves included (they play under NXDN's synctype). */
    assert(state->receive_context_active && state->receive_context_protocol == DSD_SYNC_DPMR_FS2_POS);
    g_ms_calls++;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%llX want 0x%llX\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_text(const char* tag, const char* text, const char* needle, int want_present) {
    const int present = strstr(text, needle) != NULL;
    if (present != want_present) {
        DSD_FPRINTF(stderr, "%s: \"%s\" %s in \"%s\"\n", tag, needle, present ? "present" : "missing", text);
        return 1;
    }
    return 0;
}

/* What dpmr_print_ids() writes for @p state, or a failure. */
static int
capture_print_ids(const dsd_state* state, char* out, size_t out_size) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "dpmr_print_ids") != 0) {
        DSD_FPRINTF(stderr, "capture stderr begin failed\n");
        return 1;
    }
    dpmr_print_ids(state);
    if (dsd_test_capture_stderr_end(&cap) != 0 || dsd_test_capture_stderr_read(&cap, out, out_size) != 0) {
        DSD_FPRINTF(stderr, "capture stderr failed\n");
        return 1;
    }
    return 0;
}

static int
test_first_group_scrambler_bridge_mutes_without_key(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();

    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->nxdn_payload_mi = 0x7777ULL;
    state->dPMRVoiceFS2Frame.FrameNumbering[0] = 0;
    state->dPMRVoiceFS2Frame.CommunicationMode[0] = 0;
    state->dPMRVoiceFS2Frame.Version[0] = 3;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1;
    state->dPMRVoiceFS2Frame.FrameNumbering[1] = 1;
    state->dPMRVoiceFS2Frame.CommunicationMode[1] = 2;
    /* A checked data half: it plays nothing (a failed half would play by the transmission's checked mode). */
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1;
    state->dPMRVoiceFS2Frame.Version[1] = 3;

    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);

    int rc = 0;
    rc |= expect_int("first-group-mbe-calls", (int)g_mbe_calls, 4);
    rc |= expect_int("first-group-ms-calls", g_ms_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("first-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("first-group-cipher", g_mbe_cipher[i], 1);
        rc |= expect_u64("first-group-mi-reset", g_mbe_mi[i], 0ULL);
    }
    rc |= expect_int("first-group-synctype-restored", state->synctype, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_int("first-group-cipher-reset", (int)state->nxdn_cipher_type, 0);
    rc |= expect_int("first-group-transmission-scrambled", state->dpmr_scrambled, 2);
    return rc;
}

static int
test_second_group_scrambler_bridge_unmutes_with_key(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();

    opts->floating_point = 1;
    state->synctype = DSD_SYNC_DPMR_FS3_POS;
    state->R = 0x12345ULL;
    state->nxdn_payload_mi = 0x8888ULL;
    state->dPMRVoiceFS2Frame.FrameNumbering[0] = 0;
    state->dPMRVoiceFS2Frame.CommunicationMode[0] = 2;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1; /* a checked data half, which plays nothing */
    state->dPMRVoiceFS2Frame.FrameNumbering[1] = 1;
    state->dPMRVoiceFS2Frame.CommunicationMode[1] = 5;
    state->dPMRVoiceFS2Frame.Version[1] = 3;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1;

    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);

    int rc = 0;
    rc |= expect_int("second-group-mbe-calls", (int)g_mbe_calls, 4);
    // Float output (-y) plays through the same mono mixer (#652).
    rc |= expect_int("second-group-ms-calls", g_ms_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("second-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("second-group-cipher", g_mbe_cipher[i], 1);
        /* Frame 1 of the superframe: the key loads into the register, stepped to its place, and the frames step it. */
        rc |= expect_u64("second-group-mi-reset", g_mbe_mi[i], scramble_register_at(0x12345ULL, 4 + i));
    }
    rc |= expect_int("second-group-synctype-restored", state->synctype, DSD_SYNC_DPMR_FS3_POS);
    rc |= expect_int("second-group-cipher-reset", (int)state->nxdn_cipher_type, 0);
    return rc;
}

/* Issue #890: in a Voice and attached data call (M = 101, TS 102 658 §5.5.7, §11.1.1 Figure 11.3) a payload frame is
 * AMBE only while its CRC-checked SLD says it holds no data (DP = 00). The frames after PTT release carry the attached
 * data (DP = 11): they reach no vocoder, so they neither play nor become the scan's voice credit. A half whose CRC-7
 * failed, or with a reserved DP, is not known to be speech. @p sld1 is half 1's SLD, @p crc1 its CRC verdict; half 0
 * is speech (DP = 00) in every case. Returns the frames sent to the vocoder. */
static int
attached_data_run(unsigned sld1, unsigned crc1) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    for (int o = 0; o < 2; o++) {
        state->dPMRVoiceFS2Frame.FrameNumbering[o] = (unsigned)(2 + o);
        state->dPMRVoiceFS2Frame.CommunicationMode[o] = 5;
        state->dPMRVoiceFS2Frame.Version[o] = 0;
    }
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1;
    state->dPMRVoiceFS2Frame.SlowData[0] = 0x00080U; /* DP 00, a format and length: speech */
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = crc1;
    state->dPMRVoiceFS2Frame.SlowData[1] = sld1;
    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);
    return (int)g_mbe_calls;
}

static int
test_attached_data_frames_are_no_voice(void) {
    int rc = 0;
    /* SLD bits, first bit first: 5 reserved, DP (2), format (4), CONT (1), length (6). */
    const unsigned dp_data = 3U << 11U;
    rc |= expect_int("attached data: both halves speech", attached_data_run(0x00080U, 1), 8);
    rc |= expect_int("attached data: a data half (DP 11) reaches no vocoder",
                     attached_data_run(dp_data | (0U << 6U) | 20U, 1), 4);
    rc |= expect_int("attached data: a reserved DP (01) is not speech", attached_data_run(1U << 11U, 1), 4);
    rc |= expect_int("attached data: a damaged half is not known to be speech", attached_data_run(0x00080U, 0), 4);

    /* The rule reads the half's checked mode, not its own bits (dpmr_resolve_half_modes()): a damaged half whose bits
       read 101 in a voice call (checked 000) is voice, and one whose bits read 000 in a call checked 101 is not known
       to hold no data. */
    static const struct {
        unsigned checked_mode;
        unsigned damaged_bits;
        int want;
        const char* tag;
    } k_by_checked[] = {{0U, 5U, 8, "attached data: a damaged half of a voice call reading 101 plays"},
                        {5U, 0U, 4, "attached data: a damaged half of a 101 call reading 000 does not"}};

    for (size_t c = 0; c < sizeof(k_by_checked) / sizeof(k_by_checked[0]); c++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        reset_capture();
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->dPMRVoiceFS2Frame.FrameNumbering[0] = 2U;
        state->dPMRVoiceFS2Frame.FrameNumbering[1] = 3U;
        state->dPMRVoiceFS2Frame.CommunicationMode[0] = k_by_checked[c].checked_mode;
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
        state->dPMRVoiceFS2Frame.SlowData[0] = 0x00080U; /* DP 00 */
        state->dPMRVoiceFS2Frame.CommunicationMode[1] = k_by_checked[c].damaged_bits;
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 0U;
        dpmr_resolve_half_scrambling(state);
        dpmr_play_voice_frames(opts, state, ambe_fr);
        rc |= expect_int(k_by_checked[c].tag, (int)g_mbe_calls, k_by_checked[c].want);
    }
    return rc;
}

/* Issue #655: each half is scrambled or clear on its own, and only a half whose CCH CRC-7 passed says which. A clear
 * half after a scrambled one decodes clear (no NXDN scrambler, synctype left dPMR); a failed half reading Version 3 in
 * a clear transmission stays clear; a failed half after a verified scrambled one stays scrambled; a transmission
 * boundary forgets the verdict, and a failed half then takes this frame's verified half, or, with none, plays nothing:
 * confirmation outlives the boundary, so a scrambled call after a clear one never plays an undecodable half as clear. */
static int
bridge_run(dsd_state* state, unsigned v0, unsigned crc0, unsigned v1, unsigned crc1) {
    dsd_opts* opts = g_opts;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dPMRVoiceFS2Frame.FrameNumbering[0] = 1;
    state->dPMRVoiceFS2Frame.FrameNumbering[1] = 2;
    state->dPMRVoiceFS2Frame.CommunicationMode[0] = 0;
    state->dPMRVoiceFS2Frame.CommunicationMode[1] = 0;
    state->dPMRVoiceFS2Frame.Version[0] = v0;
    state->dPMRVoiceFS2Frame.Version[1] = v1;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = crc0;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = crc1;
    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);
    return (int)g_mbe_calls;
}

static int
expect_halves(const char* tag, int scrambled0, int scrambled1) {
    char what[96];
    int rc = 0;
    for (int i = 0; i < 8; i++) {
        const int scrambled = i < 4 ? scrambled0 : scrambled1;
        DSD_SNPRINTF(what, sizeof(what), "%s frame %d cipher", tag, i);
        rc |= expect_int(what, g_mbe_cipher[i], scrambled ? 1 : 0);
        DSD_SNPRINTF(what, sizeof(what), "%s frame %d synctype", tag, i);
        rc |= expect_int(what, g_mbe_synctype[i], scrambled ? DSD_SYNC_NXDN_POS : DSD_SYNC_DPMR_FS2_POS);
    }
    return rc;
}

static int
test_scrambling_is_per_half_and_crc_verified(void) {
    dsd_state* state = g_state;
    DSD_MEMSET(state, 0, sizeof(*state));
    int rc = 0;

    rc |= expect_int("scrambled then clear calls", bridge_run(state, 3U, 1U, 0U, 1U), 8);
    rc |= expect_halves("scrambled then clear", 1, 0);
    rc |= expect_int("scrambled then clear leaves clear", state->dpmr_scrambled, 1);
    rc |= expect_int("next clear frame", bridge_run(state, 0U, 1U, 0U, 1U), 8);
    rc |= expect_halves("next clear frame", 0, 0);

    rc |= expect_int("failed Version 3 in a clear call", bridge_run(state, 3U, 0U, 0U, 1U), 8);
    rc |= expect_halves("failed Version 3 in a clear call", 0, 0);

    rc |= expect_int("verified scrambled", bridge_run(state, 3U, 1U, 3U, 1U), 8);
    rc |= expect_halves("verified scrambled", 1, 1);
    rc |= expect_int("failed clear-looking halves in a scrambled call", bridge_run(state, 0U, 0U, 0U, 0U), 8);
    rc |= expect_halves("failed clear-looking halves in a scrambled call", 1, 1);

    dpmr_scrambling_reset(state);
    rc |= expect_int("boundary forgets", state->dpmr_scrambled, 0);
    rc |= expect_int("failed half before a verified scrambled one", bridge_run(state, 0U, 0U, 3U, 1U), 8);
    rc |= expect_halves("failed half before a verified scrambled one", 1, 1);
    // A verified clear call, its end frame, then a call whose first frame fails both CRCs: nothing of it plays.
    rc |= expect_int("verified clear call", bridge_run(state, 0U, 1U, 0U, 1U), 8);
    dpmr_scrambling_reset(state);
    rc |= expect_int("nothing verified since the boundary", bridge_run(state, 0U, 0U, 3U, 0U), 0);
    rc |= expect_int("nothing verified halves", (int)state->dPMRVoiceFS2Frame.Scrambled[0], DPMR_HALF_UNVERIFIED);
    rc |= expect_int("nothing verified cipher reset", (int)state->nxdn_cipher_type, 0);
    // Its next frame verifies one half: both play by it.
    rc |= expect_int("then a verified scrambled half", bridge_run(state, 0U, 0U, 3U, 1U), 8);
    rc |= expect_halves("then a verified scrambled half", 1, 1);
    return rc;
}

static int
test_scrambler_secret_is_not_published_as_key_id(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = 0x12345ULL;
    state->dPMRVoiceFS2Frame.Version[0] = 3U;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
    dpmr_resolve_half_scrambling(state);
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    /* Redaction, not the gate: publishing at all needs a proved transmission (#407). */
    state->dpmr_confirmed = 1;
    dpmr_publish_call(opts, state);

    dsd_call_snapshot call;
    int rc = 0;
    rc |= expect_int("scrambler-call-present", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("scrambler-call-decryptable", call.crypto, DSD_CALL_CRYPTO_DECRYPTABLE);
    rc |= expect_int("scrambler-call-audio-permitted", call.audio_permitted, 1);
    rc |= expect_int("scrambler-secret-not-key-id", call.kid, 0);
    return rc;
}

/* What the call record keeps as the call's own checked encryption indication (enc_received, which a detection's
   "Encrypted" needs) comes only from halves whose CCH CRC passed in the frame being published, never from scrambling
   the transmission learned earlier: a later call whose first frame verifies nothing says nothing, and one that verifies
   clear clears it. */
static int
test_enc_indication_is_the_frames_own(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    int rc = 0;
    dsd_call_snapshot call;

    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state->dPMRVoiceFS2Frame.Version[0] = 3U;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
    dpmr_resolve_half_scrambling(state);
    dpmr_publish_call(opts, state);
    rc |= expect_int("verified scrambled call", dsd_call_state_get(state, 0U, &call) > 0 && call.enc_received == 1U, 1);
    const uint64_t first = call.epoch;

    // The call ends; the next one's first frame verifies neither half, so the learned scrambling would play it, but a
    // frame with no checked half opens no call (issue #890) and says nothing of encryption.
    (void)dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_TERMINATOR);
    state->dPMRVoiceFS2Frame.Version[0] = 0U;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 0U;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 0U;
    dpmr_resolve_half_scrambling(state);
    rc |= expect_int("learned scrambling plays it", (int)state->dPMRVoiceFS2Frame.Scrambled[0], DPMR_HALF_SCRAMBLED);
    dpmr_publish_call(opts, state);
    rc |=
        expect_int("unchecked frame opens no call", dsd_call_state_get(state, 0U, &call) > 0 && call.epoch == first, 1);

    // Its next frame's second half verifies clear: the call opens, its first half still played by the learned
    // scrambling, and the call's own indication is the checked half's: clear.
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1U;
    state->dPMRVoiceFS2Frame.Version[1] = 0U;
    dpmr_resolve_half_scrambling(state);
    rc |= expect_int("learned scrambling still plays half 0", (int)state->dPMRVoiceFS2Frame.Scrambled[0],
                     DPMR_HALF_SCRAMBLED);
    dpmr_publish_call(opts, state);
    rc |= expect_int("next call opens", dsd_call_state_get(state, 0U, &call) > 0 && call.epoch != first, 1);
    rc |= expect_int("verified clear", call.enc_received, 0);
    return rc;
}

static int
test_deinterleave_transposes_6x12_blocks(void) {
    uint8_t input[72];
    uint8_t output[72];
    for (uint32_t i = 0; i < 72U; i++) {
        input[i] = (uint8_t)i;
        output[i] = 0U;
    }

    dpmr_deinterleave_6x12(input, output);

    int rc = 0;
    for (uint32_t j = 0; j < 6U; j++) {
        for (uint32_t i = 0; i < 12U; i++) {
            char tag[40];
            DSD_SNPRINTF(tag, sizeof(tag), "deinterleave-%u-%u", (unsigned)i, (unsigned)j);
            rc |= expect_int(tag, output[(j * 12U) + i], input[(i * 6U) + j]);
        }
    }
    return rc;
}

static int
test_crc7_and_air_interface_id_helpers(void) {
    static const uint8_t crc_bits[12] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1};
    uint8_t cch_bits[48] = {0};
    char id[8];
    int rc = 0;

    rc |= expect_int("crc7-empty", dpmr_crc7(crc_bits, 0U), 0x00);
    rc |= expect_int("crc7-pattern", dpmr_crc7(crc_bits, 12U), 0x24);
    cch_bits[41] = 1U;
    cch_bits[43] = 1U;
    cch_bits[46] = 1U;
    rc |= expect_int("cch-crc-extract", dpmr_extract_cch_crc(cch_bits), 0x52);

    dpmr_convert_air_interface_id(0U, id);
    rc |= expect_int("aiid-zero", strcmp(id, "0000000"), 0);
    dpmr_convert_air_interface_id(10U, id);
    rc |= expect_int("aiid-star-low", strcmp(id, "000000*"), 0);
    dpmr_convert_air_interface_id(1464100U, id);
    rc |= expect_int("aiid-first-digit", strcmp(id, "1000000"), 0);
    dpmr_convert_air_interface_id(1464110U, id);
    rc |= expect_int("aiid-first-digit-with-star", strcmp(id, "100000*"), 0);
    /* TS 102 658 Table A.2's examples. */
    dpmr_convert_air_interface_id(0x1B91FDU, id);
    rc |= expect_int("aiid-table-a2-1234567", strcmp(id, "1234567"), 0);
    dpmr_convert_air_interface_id(0x02C00AU, id);
    rc |= expect_int("aiid-table-a2-012345*", strcmp(id, "012345*"), 0);
    dpmr_convert_air_interface_id(0xDF6767U, id);
    rc |= expect_int("aiid-table-a2-999****", strcmp(id, "999****"), 0);
    /* Issue #890: the All Talkgroup IDs above the dialled range (A.1.2.3.3, Tables A.3 and A.4). */
    dpmr_convert_air_interface_id(0xF8339CU, id);
    rc |= expect_int("aiid-all-call-prefix-0", strcmp(id, "0******"), 0);
    dpmr_convert_air_interface_id(0xF833A5U, id);
    rc |= expect_int("aiid-all-call-prefix-9", strcmp(id, "9******"), 0);
    dpmr_convert_air_interface_id(0xF833A6U, id);
    rc |= expect_int("aiid-all-call", strcmp(id, "*******"), 0);
    /* Any other ID past "999****" is no dialled string: shown raw. */
    dpmr_convert_air_interface_id(0xDF6768U, id);
    rc |= expect_int("aiid-past-dialled-range", strcmp(id, "#DF6768"), 0);
    dpmr_convert_air_interface_id(0xF833A7U, id);
    rc |= expect_int("aiid-past-all-call", strcmp(id, "#F833A7"), 0);
    return rc;
}

/* One confirmed frame's publication: its halves' CRC-7 verdicts, frame numbers and modes, the identity the part
   carries, then dpmr_update_superframe_part() and dpmr_publish_call() as processdPMRvoice() runs them. */
static void
publish_part(dsd_opts* opts, dsd_state* state, const dpmr_superframe_part* part, unsigned mode0, unsigned mode1) {
    dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    f->ColorCode[0] = (unsigned int)(-1);
    for (int o = 0; o < 2; o++) {
        f->CCHDataCrcOk[o] = part->crc_ok[o] ? 1U : 0U;
        f->FrameNumbering[o] = part->frame_number[o];
        f->Version[o] = 0U;
        f->CommsFormat[o] = 1U;
        f->EmergencyPriority[o] = 0U;
    }
    f->CommunicationMode[0] = mode0;
    f->CommunicationMode[1] = mode1;
    dpmr_resolve_half_scrambling(state);
    dpmr_update_superframe_part(opts, state, part);
    dpmr_publish_call(opts, state);
}

/* Issue #890 (B9b): a frame names the called station only from halves 0/1 and the calling station only from halves
   2/3, both halves CRC-valid, and publishes that identity with its checked service in one observation. A half whose
   Hamming words merely correct, or a pair numbered otherwise, names nobody. */
static int
test_superframe_part_updates_called_and_calling_ids(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    dpmr_superframe_part part;
    dsd_call_snapshot call;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("called-id", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("called-with-service", call.has_service_metadata, 1);
    rc |= expect_int("called-service", call.service_options, 0x0001);
    rc |= expect_int("called-next-part", opts->dPMR_next_part_of_superframe, 2);
    rc |= expect_int("calling-id-preserved-empty", strcmp(call.source_text, ""), 0);

    /* Calling halves whose CRC-7 failed and whose Hamming words all corrected: no calling identity. */
    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 1464110U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("hamming-only-calling-id-not-published", strcmp(call.source_text, ""), 0);
    rc |= expect_int("calling-next-part", opts->dPMR_next_part_of_superframe, 1);
    rc |= expect_int("called-id-preserved", strcmp(call.target_text, "1000000"), 0);

    /* One calling half CRC-valid, the other not: no calling identity either. */
    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 1464110U,
                                  .crc_ok = {true, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("half-failed-calling-id-not-published", strcmp(call.source_text, ""), 0);

    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 1464110U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("calling-id", strcmp(call.source_text, "100000*"), 0);
    rc |= expect_int("calling-preserves-called-id", strcmp(call.target_text, "1000000"), 0);
    const uint64_t first_caller_epoch = call.epoch;

    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 2928210U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("changed-calling-id", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("changed-calling-starts-epoch", call.epoch != first_caller_epoch, 1);
    const uint64_t second_caller_epoch = call.epoch;

    /* Halves 0 and 3, both CRC-valid: the high half of one identity and the low half of another, no identity. */
    part = (dpmr_superframe_part){.frame_number = {0U, 3U},
                                  .id_value = 10U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("mismatched-pair-names-nobody", strcmp(call.target_text, "000000*") != 0, 1);
    rc |= expect_int("mismatched-pair-keeps-the-call", call.epoch == second_caller_epoch, 1);

    part = (dpmr_superframe_part){.frame_number = {1U, 2U},
                                  .id_value = 0U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{false, false}, {false, false}}};
    publish_part(opts, state, &part, 0U, 0U);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("unknown-preserves-calling", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("unknown-toggles-next-part", opts->dPMR_next_part_of_superframe, 1);

    opts->dPMR_next_part_of_superframe = 2;
    dpmr_update_superframe_part(opts, state, &part);
    rc |= expect_int("unknown-toggles-next-part-back", opts->dPMR_next_part_of_superframe, 1);

    opts->dPMR_next_part_of_superframe = 0;
    dpmr_update_superframe_part(opts, state, &part);
    rc |= expect_int("unknown-keeps-zero-part", opts->dPMR_next_part_of_superframe, 0);
    return rc;
}

/* Issue #890 (B9b, F13): the service a frame publishes is its CRC-valid half's, whatever its failed half read, and a
   checked data CCH is a data call: never voice, and never voice first and data after. */
static int
test_service_and_kind_come_from_the_checked_half(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    dpmr_superframe_part part;
    dsd_call_snapshot call;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    /* Half 0 failed, reading voice and emergency; half 1 passed: Type 2 data, no emergency. */
    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {false, true},
                                  .hamming_ok = {{true, true}, {true, true}}};
    state->dPMRVoiceFS2Frame.EmergencyPriority[0] = 1U;
    publish_part(opts, state, &part, 0U, 3U);
    state->dPMRVoiceFS2Frame.EmergencyPriority[0] = 0U;
    rc |= expect_int("checked-half-call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("checked-half-kind-data", call.kind, DSD_CALL_KIND_DATA);
    rc |= expect_int("checked-half-service", call.service_options, 0x0031);
    rc |= expect_int("checked-half-no-emergency", call.emergency, 0);
    rc |= expect_int("failed-half-names-nobody", call.target_text[0] == '\0', 1);

    /* Both halves checked data: a data call. */
    DSD_MEMSET(state, 0, sizeof(*state));
    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{true, true}, {true, true}}};
    publish_part(opts, state, &part, 2U, 2U);
    rc |= expect_int("data-cch-call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("data-cch-not-voice", call.kind, DSD_CALL_KIND_DATA);
    rc |= expect_int("data-cch-identity-with-it", strcmp(call.target_text, "1000000"), 0);

    /* A confirmed frame with no checked half opens nothing. */
    DSD_MEMSET(state, 0, sizeof(*state));
    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    publish_part(opts, state, &part, 0U, 0U);
    rc |= expect_int("unchecked-frame-opens-no-call", dsd_call_state_get(state, 0U, &call) > 0, 0);
    return rc;
}

/* Issue #890 (B19): a half's place in the scrambler comes from checked frame numbers only. A failed half that reads 0
   is frame 2, not frame 0, when the frame's checked half is numbered 3, so it does not descramble from the key; a
   failed first half is frame 0 when the checked second half is numbered 1. */
static int
test_scrambler_reloads_only_at_a_checked_frame_zero(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    int rc = 0;

    for (int second = 1; second <= 3; second += 2) {
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        reset_capture();
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->dpmr_scrambled = 2U; /* a scrambled transmission, learned from an earlier verified half */
        state->R = 0x12345ULL;
        state->nxdn_payload_mi = 0x7777ULL;
        state->dPMRVoiceFS2Frame.FrameNumbering[0] = 0U; /* failed: reads 0 */
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 0U;
        state->dPMRVoiceFS2Frame.FrameNumbering[1] = (unsigned)second;
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1U;
        state->dPMRVoiceFS2Frame.Version[1] = 3U;
        dpmr_resolve_half_scrambling(state);
        dpmr_play_voice_frames(opts, state, ambe_fr);
        rc |= expect_int("bad-frame0-plays", (int)g_mbe_calls, 8);
        const unsigned long long want = scramble_register_at(0x12345ULL, second == 1 ? 0 : 8);
        rc |= expect_u64(second == 1 ? "checked-pair-frame0-reloads" : "bad-frame0-is-frame-2", g_mbe_mi[0], want);
    }
    return rc;
}

/* Before #407 this published a talkgroup and a source: dpmr_ids_are_strong() then accepted a
   CCH whose two leading Hamming blocks merely reported correctable, which 44% of noise
   superframes do. Without a passing CRC-7 there is nothing here to publish. */
static int
test_unconfirmed_superframe_part_publishes_nothing(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    dpmr_superframe_part part;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    dpmr_update_superframe_part(opts, state, &part);
    dpmr_publish_call(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("unconfirmed-no-call-row", dsd_call_state_get(state, 0U, &call) > 0, 0);
    /* The superframe bookkeeping still advances: it tracks which half comes next, not
       whether anything decoded. */
    rc |= expect_int("unconfirmed-next-part-still-tracked", opts->dPMR_next_part_of_superframe, 2);
    return rc;
}

/* The printer only prints (issue #575). It shows what the canonical call holds, so it is no place to publish this
   carrier's colour code from: dpmr_publish_call() does that, on the confirmed path. */
static int
test_id_print_does_not_publish_the_color_code(void) {
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(state, 0, sizeof(*state));
    state->dpmr_color_code = -1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state->dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    dpmr_print_ids(state);

    dsd_call_snapshot call;
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("print-target-id", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("print-caller-id", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("print-leaves-color-code", state->dpmr_color_code, -1);
    return rc;
}

/* A confirmed transmission publishes the colour code it decoded, whether or not a caller identity decoded with it
   (issue #575): a target-only call is still a call on this carrier. */
static int
test_confirmed_target_only_call_publishes_the_color_code(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->dpmr_color_code = -1;
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state->dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    dpmr_print_ids(state);
    dpmr_publish_call(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("target-only-call-present", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("target-only-no-caller", call.source_text[0] == '\0', 1);
    rc |= expect_int("target-only-color-code", state->dpmr_color_code, 12);
    /* The carrier's received identity: the colour code of the confirmed transmission. */
    rc |= expect_int("target-only-identity-family", state->rx_id.family, DSD_RXID_FAMILY_DPMR);
    rc |= expect_int("target-only-identity-parts", (int)state->rx_id.valid, (int)DSD_RXID_ACCESS);
    rc |= expect_int("target-only-identity-colour", state->rx_id.access, 12);
    rc |= expect_int("target-only-identity-kind", state->rx_id.access_kind, DSD_ACCESS_CODE_COLOR_CODE);

    /* A later superframe whose channel code did not decode keeps the code the call already published. */
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dpmr_publish_call(opts, state);
    rc |= expect_int("undecoded-channel-code-keeps-color", state->dpmr_color_code, 12);
    rc |= expect_int("undecoded-channel-code-keeps-identity", state->rx_id.access, 12);

    /* An unconfirmed transmission names nothing. */
    dsd_rxid_reset(state);
    state->dpmr_confirmed = 0;
    state->dPMRVoiceFS2Frame.ColorCode[0] = 33U;
    dpmr_publish_call(opts, state);
    rc |= expect_int("unconfirmed-no-identity", (int)state->rx_id.valid, 0);
    return rc;
}

/* The colour code is the carrier's access code, never the call's channel: dPMR has no trunk channel number, and the
   API documents `channel` as one. A confirmed call with a decoded colour code keeps channel 0 and reads the code as its
   access code. */
static int
test_dpmr_channel_not_colour_code(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->dpmr_color_code = -1;
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = 12U;
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U; /* a frame publishes a call from a checked half (issue #890) */
    dpmr_publish_call(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("cc-call-present", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("cc-call-channel", (int)call.channel, 0);
    uint8_t kind = 0U;
    uint16_t code = 0U;
    rc |= expect_int(
        "cc-access-code-read",
        dsd_access_code_current(state, call.protocol, call.service_options, call.has_service_metadata, &kind, &code),
        1);
    rc |= expect_int("cc-access-code-kind", kind, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_int("cc-access-code", code, 12);
    return rc;
}

/* An unconfirmed frame publishes nothing, the colour code included, even with a caller identity retained from an
   earlier transmission in the canonical call (issue #575). */
static int
test_unconfirmed_frame_with_retained_caller_publishes_no_color_code(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->dpmr_color_code = -1;
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_end(state, 0U, 0.0);
    state->dpmr_confirmed = 0;
    state->dPMRVoiceFS2Frame.ColorCode[0] = 21U;

    char printed[256];
    rc |= capture_print_ids(state, printed, sizeof(printed));
    dpmr_publish_call(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("retained-caller-text", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("retained-caller-is-kept", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("unconfirmed-no-color-code", state->dpmr_color_code, -1);
    /* The retained text belongs to the earlier transmission: this unconfirmed frame decoded no identity to show. */
    rc |= expect_text("retained-target-not-printed", printed, "1000000", 0);
    rc |= expect_text("retained-caller-not-printed", printed, "200000*", 0);
    rc |= expect_text("unconfirmed-prints-target-crc-err", printed, "TG=(CRC ERR)", 1);
    rc |= expect_text("unconfirmed-prints-caller-crc-err", printed, "Src=(CRC ERR)", 1);
    return rc;
}

/* A confirmed transmission shows the identities its own frames published, and the channel code read with them. */
static int
test_confirmed_call_prints_its_identities(void) {
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state->dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    char printed[256];
    rc |= capture_print_ids(state, printed, sizeof(printed));
    rc |= expect_text("confirmed-prints-target", printed, "TG=1000000", 1);
    rc |= expect_text("confirmed-prints-caller", printed, "Src=200000*", 1);
    rc |= expect_text("confirmed-prints-channel-code", printed, "Channel Code=12", 1);
    rc |= expect_text("confirmed-no-crc-err", printed, "CRC ERR", 0);
    return rc;
}

/* A noise frame that syncs as dPMR after another protocol's call (seen on a D-STAR capture under -fa) printed that
   call's callsigns in green as the frame's TG and Src: the printer read the canonical call whoever had written it.
   Neither CCH half of this frame passes its CRC-7, so it has no identity to show, whether the other call has ended or
   is still open. */
static int
test_noise_frame_after_another_protocols_call_prints_no_identity(void) {
    int rc = 0;
    for (int still_open = 0; still_open < 2; still_open++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        reset_capture();
        dsd_call_observation observation = {
            .protocol = DSD_SYNC_DSTAR_VOICE_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
        };
        DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "CQCQCQ");
        DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "N0CALL");
        (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        if (!still_open) {
            (void)dsd_call_state_end(state, 0U, 0.0);
        }
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->dpmr_color_code = -1;
        state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
        g_cch_noise = 1;

        dsd_test_capture_stderr cap;
        char printed[512];
        if (dsd_test_capture_stderr_begin(&cap, "dpmr_noise_frame") != 0) {
            DSD_FPRINTF(stderr, "capture stderr begin failed\n");
            return 1;
        }
        const int verdict = processdPMRvoice(opts, state);
        if (dsd_test_capture_stderr_end(&cap) != 0
            || dsd_test_capture_stderr_read(&cap, printed, sizeof(printed)) != 0) {
            DSD_FPRINTF(stderr, "capture stderr failed\n");
            return 1;
        }

        rc |= expect_int("noise-verdict", verdict, 0);
        rc |= expect_int("noise-crc-0", (int)state->dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
        rc |= expect_int("noise-crc-1", (int)state->dPMRVoiceFS2Frame.CCHDataCrcOk[1], 0);
        rc |= expect_int("noise-unconfirmed", state->dpmr_confirmed, 0);
        rc |= expect_int("noise-no-voice", (int)g_mbe_calls, 0);
        rc |= expect_text("noise-no-foreign-target", printed, "CQCQCQ", 0);
        rc |= expect_text("noise-no-foreign-caller", printed, "N0CALL", 0);
        rc |= expect_text("noise-target-crc-err", printed, "TG=(CRC ERR)", 1);
        rc |= expect_text("noise-caller-crc-err", printed, "Src=(CRC ERR)", 1);

        /* The other protocol's call is left as it was. */
        dsd_call_snapshot call;
        rc |= expect_int("noise-other-call-kept", dsd_call_state_get(state, 0U, &call) > 0, 1);
        rc |= expect_int("noise-other-call-protocol", call.protocol, DSD_SYNC_DSTAR_VOICE_POS);
        rc |= expect_int("noise-other-call-phase", (int)call.phase,
                         still_open ? (int)DSD_CALL_PHASE_ACTIVE : (int)DSD_CALL_PHASE_ENDED);
    }
    return rc;
}

static int
test_id_print_side_effects_suppress_invalid_ids(void) {
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(state, 0, sizeof(*state));
    state->dpmr_color_code = 7;
    state->dPMRVoiceFS2Frame.ColorCode[0] = 4U;

    dpmr_print_ids(state);
    rc |= expect_int("invalid-called-keeps-color", state->dpmr_color_code, 7);
    return rc;
}

static int
test_process_dpmr_voice_zero_stream_updates_cch_and_dispatches_voice(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS1_POS;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();

    processdPMRvoice(opts, state);

    rc |= expect_int("process-dibit-count", g_dibit_calls, 372);
    rc |= expect_int("process-mbe-calls", (int)g_mbe_calls, 8);
    rc |= expect_int("process-ms-calls", g_ms_calls, 8);
    for (int i = 0; i < 8; i++) {
        rc |= expect_int("process-synctype", g_mbe_synctype[i], DSD_SYNC_DPMR_FS1_POS);
        rc |= expect_int("process-cipher", g_mbe_cipher[i], 0);
        rc |= expect_int("process-clear-audio", g_mbe_scrambled[i], 0);
    }

    for (uint32_t i = 0; i < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; i++) {
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "process-hamming-ok-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state->dPMRVoiceFS2Frame.CCHDataHammingOk[i], 1);
        DSD_SNPRINTF(tag, sizeof(tag), "process-crc-ok-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state->dPMRVoiceFS2Frame.CCHDataCrcOk[i], 1);
        DSD_SNPRINTF(tag, sizeof(tag), "process-frame-number-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state->dPMRVoiceFS2Frame.FrameNumbering[i], 0);
        DSD_SNPRINTF(tag, sizeof(tag), "process-comm-mode-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state->dPMRVoiceFS2Frame.CommunicationMode[i], 0);
        DSD_SNPRINTF(tag, sizeof(tag), "process-version-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state->dPMRVoiceFS2Frame.Version[i], 0);
    }

    /* Both halves read frame number 0: no 0/1 pair, so no identity (issue #890), though the frame confirms, plays and
       publishes its checked service. */
    dsd_call_snapshot call;
    rc |= expect_int("process-call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("process-unpaired-halves-name-nobody", call.target_text[0] == '\0', 1);
    rc |= expect_int("process-service", call.has_service_metadata, 1);
    rc |= expect_int("process-next-part", opts->dPMR_next_part_of_superframe, 2);
    rc |= expect_int("process-invalid-color-code", (int)state->dPMRVoiceFS2Frame.ColorCode[0], -1);
    /* The call this frame opens decoded no channel code: it has none (issue #890), not the 0 the state began with. */
    rc |= expect_int("process-new-call-no-color-code", state->dpmr_color_code, -1);
    rc |= expect_int("process-final-synctype", state->synctype, DSD_SYNC_DPMR_FS1_POS);
    return rc;
}

/* Issue #575: the carrier boundary moved the carrier count while the two frames were read (a replay read adopting a
   recorded retune). Nothing decodes before both are in, so the first frame's CCH and the channel code, read whole before
   the move, would publish the carrier left's call and colour code after the boundary ended that call and forgot the
   code: the pair is dropped, as on a sync loss. */
static int
test_process_dpmr_voice_split_by_a_carrier_boundary_publishes_nothing(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS1_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_boundary_at = 300;

    const int verdict = processdPMRvoice(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("split-moved-carrier-count", (int)state->carrier_seq, 1);
    rc |= expect_int("split-dibit-count", g_dibit_calls, 372);
    rc |= expect_int("split-verdict", verdict, 0);
    rc |= expect_int("split-mbe-calls", (int)g_mbe_calls, 0);
    rc |= expect_int("split-no-call", dsd_call_state_get(state, 0U, &call) > 0, 0);
    rc |= expect_int("split-no-crc", (int)state->dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
    rc |= expect_int("split-no-confirmation", state->dpmr_confirmed, 0);
    rc |= expect_int("split-no-color-code", state->dpmr_color_code, -1);
    return rc;
}

/* What the signal search's confirmation can read when the evidence hook fires: the app-control observer copies the
 * active call's identity and the carrier's colour code inside the hook, from state, and only on that confirmation. */
static struct {
    int calls;
    int call_active;
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    int has_service_metadata;
    int color_code;
} g_search_evidence;

static void
capture_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    assert(protocol != NULL && strcmp(protocol, "dPMR") == 0);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    g_search_evidence.calls++;
    const int active = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                       && call.protocol == state->synctype;
    g_search_evidence.call_active = active;
    DSD_SNPRINTF(g_search_evidence.target_text, sizeof(g_search_evidence.target_text), "%s",
                 active ? call.target_text : "");
    g_search_evidence.has_service_metadata = active ? call.has_service_metadata : 0;
    g_search_evidence.color_code = state->dpmr_color_code;
}

/* The frame whose CCH CRCs confirm a transmission is the frame that publishes its called ID and colour code, and the
 * search hears the confirmation only after it has: one frame followed by silence still names the call. A frame that
 * proved nothing reports nothing. */
static int
test_process_dpmr_voice_search_evidence_follows_the_published_call(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    state->synctype = DSD_SYNC_DPMR_FS1_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dsd_signal_search_evidence_set(capture_search_evidence);

    /* A pair split by a carrier boundary decodes nothing. */
    reset_capture();
    g_boundary_at = 300;
    (void)processdPMRvoice(opts, state);
    rc |= expect_int("search-split-reports-nothing", g_search_evidence.calls, 0);

    reset_capture();
    g_channel_code = 0x55FF7FU;  /* channel code 7 */
    g_second_half_frame_one = 1; /* halves 0/1: the called identity */
    (void)processdPMRvoice(opts, state);
    dsd_signal_search_evidence_set(NULL);

    rc |= expect_int("search-frame-color-code-decoded", (int)state->dPMRVoiceFS2Frame.ColorCode[0], 7);
    rc |= expect_int("search-one-confirmation", g_search_evidence.calls, 1);
    rc |= expect_int("search-sees-the-call", g_search_evidence.call_active, 1);
    rc |= expect_int("search-sees-the-called-id", strcmp(g_search_evidence.target_text, "0000000"), 0);
    rc |= expect_int("search-sees-the-service-metadata", g_search_evidence.has_service_metadata, 1);
    rc |= expect_int("search-sees-the-color-code", g_search_evidence.color_code, 7);
    return rc;
}

/* A Signal search that leaves the pair (its visit ended, or a command came), or a live read that gives up on a stalled
   input, cuts it: from the cut every symbol is an erasure getSymbol() stood in. The pair is dropped as one the carrier
   boundary splits is: no call, no colour code, no confirmation, and nothing plays. */
static int
test_process_dpmr_voice_cut_by_an_input_interruption_publishes_nothing(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS1_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_interrupt_at = 300;

    const int verdict = processdPMRvoice(opts, state);

    dsd_call_snapshot call;
    rc |= expect_int("cut-interrupted", state->input_interrupted, 1);
    rc |= expect_int("cut-no-carrier-move", (int)state->carrier_seq, 0);
    rc |= expect_int("cut-dibit-count", g_dibit_calls, 372);
    rc |= expect_int("cut-verdict", verdict, 0);
    rc |= expect_int("cut-mbe-calls", (int)g_mbe_calls, 0);
    rc |= expect_int("cut-no-call", dsd_call_state_get(state, 0U, &call) > 0, 0);
    rc |= expect_int("cut-no-crc", (int)state->dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
    rc |= expect_int("cut-no-confirmation", state->dpmr_confirmed, 0);
    rc |= expect_int("cut-no-color-code", state->dpmr_color_code, -1);
    return rc;
}

/* Two FS2 frames each with one CCH half passing its CRC-7 (one chance in 128 apiece) confirm a transmission only when
   the second runs on from the first, its FS2 one frame (384 symbols) later. Under Auto, noise hands 2400/4 hundreds of
   FS2 matches an hour between other protocols' syncs, and two weak frames seconds apart confirmed a transmission of
   noise, published a call and decoded its voice. */
static int
test_process_dpmr_voice_weak_frames_confirm_only_when_they_run(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;
    dsd_call_snapshot call;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);

    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 10000U;
    rc |= expect_int("weak-first-verdict", processdPMRvoice(opts, state), 1);
    rc |= expect_int("weak-first-unconfirmed", state->dpmr_confirmed, 0);

    /* Seconds later, other protocols' syncs between: no run. */
    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 10000U + 5000U;
    rc |= expect_int("weak-apart-verdict", processdPMRvoice(opts, state), 1);
    rc |= expect_int("weak-apart-unconfirmed", state->dpmr_confirmed, 0);
    rc |= expect_int("weak-apart-plays-nothing", (int)g_mbe_calls, 0);
    rc |= expect_int("weak-apart-no-call", dsd_call_state_get(state, 0U, &call) > 0, 0);

    /* The next frame runs on from it, a symbol late: a transmission, which publishes and plays. */
    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 10000U + 5000U + 385U;
    rc |= expect_int("weak-run-verdict", processdPMRvoice(opts, state), 1);
    rc |= expect_int("weak-run-confirmed", state->dpmr_confirmed, 1);
    rc |= expect_int("weak-run-plays", (int)g_mbe_calls, 8);
    rc |= expect_int("weak-run-call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    return rc;
}

/* Issue #890: a checked header that opens no call (an ACK, TS 102 658 5.2.4) is channel activity, not a transmission:
   it confirms nothing, so after one weak frame that learned the scrambling but confirmed no call, a frame whose CCH
   halves both fail still plays nothing and opens no row. */
static int
test_non_call_header_authorizes_no_playback(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    dsd_call_snapshot call;
    int rc = 0;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 30000U;
    (void)processdPMRvoice(opts, state);
    rc |= expect_int("weak-frame-unconfirmed", state->dpmr_confirmed, 0);
    rc |= expect_int("weak-frame-learned-scrambling", state->dpmr_scrambled != 0U, 1);

    const dpmr_header ack = {.message_type = DPMR_MT_ACK,
                             .called_id = 1464100U,
                             .calling_id = 2928210U,
                             .color_code = -1,
                             .copies = DSD_DPMR_SYNC_COPY_0};
    dpmr_process_header(opts, state, &ack, 0);
    rc |= expect_int("ack-confirms-nothing", state->dpmr_confirmed, 0);

    reset_capture();
    g_fail_both = 1;
    state->symbolcnt = 30000U + 384U;
    (void)processdPMRvoice(opts, state);
    rc |= expect_int("failed-frame-after-ack-plays-nothing", (int)g_mbe_calls, 0);
    rc |= expect_int("failed-frame-after-ack-no-row", dsd_call_state_get(state, 0U, &call) > 0, 0);
    return rc;
}

/* Issue #890: a call a frame opens takes no colour code an earlier call left when its own channel code did not decode
   (the call's access code is its own, dsd_access_code_current()); an open call keeps the one it published. */
static int
test_new_call_without_a_channel_code_has_none(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    state->dpmr_color_code = 40; /* an earlier call's, on this carrier */
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state->dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
    dpmr_resolve_half_scrambling(state);
    dpmr_publish_call(opts, state);
    dsd_call_snapshot call;
    rc |= expect_int("new-call-opens", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("new-call-no-inherited-colour", state->dpmr_color_code, -1);
    state->dPMRVoiceFS2Frame.ColorCode[0] = 12U;
    dpmr_publish_call(opts, state);
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dpmr_publish_call(opts, state);
    rc |= expect_int("open-call-keeps-its-colour", state->dpmr_color_code, 12);
    return rc;
}

/* Issue #890: a half whose CCH CRC-7 failed plays by the transmission's checked Communications Mode (TS 102 658 Table
   5.55), never by its own damaged bits: after a checked header for Type 1 data, a frame whose halves both fail while
   reading voice plays nothing; after a checked voice header the same frame plays; with no checked mode at all, nothing
   plays though the transmission is confirmed. */
static int
test_failed_halves_play_by_the_checked_mode(void) {
    int rc = 0;

    static const struct {
        int header_mode; /* -1: no header */
        size_t want_mbe;
        const char* tag;
    } k_cases[] = {
        {2, 0U, "data-header-failed-voice-halves"}, {1, 8U, "voice-header-failed-halves"}, {-1, 0U, "no-checked-mode"}};

    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->dpmr_color_code = -1;
        state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
        if (k_cases[c].header_mode >= 0) {
            const dpmr_header header = {.message_type = DPMR_MT_COMMUNICATIONS_START,
                                        .called_id = 1464100U,
                                        .calling_id = 2928210U,
                                        .mode = (uint32_t)k_cases[c].header_mode,
                                        .format = 1U,
                                        .color_code = -1,
                                        .copies = DSD_DPMR_SYNC_COPY_0};
            dpmr_process_header(opts, state, &header, 0);
        } else {
            state->dpmr_confirmed = 1;
            state->dpmr_scrambled = 1U; /* clear, learned; no mode learned */
        }
        reset_capture();
        g_fail_both = 1; /* both halves fail, their mode bits reading 000, voice */
        (void)processdPMRvoice(opts, state);
        rc |= expect_int(k_cases[c].tag, (int)g_mbe_calls, (int)k_cases[c].want_mbe);
    }
    return rc;
}

/* Issue #890: only the traffic headers whose PARMS end in MI_TYPE and MI_DET can be powersave headers (TS 102 658
   5.5.19.1); a UDT Appended_Data header (1111) carries payload in those bits (5.6.2), whose ones are no WU=15. */
static int
test_powersave_read_only_from_headers_that_carry_it(void) {
    int rc = 0;

    static const struct {
        uint32_t mt;
        int powersave;
    } k_cases[] = {{DPMR_MT_COMMUNICATIONS_START, 1}, {0xFU, 0}, {0xEU, 0}, {0xBU, 0}};

    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        state->dpmr_color_code = -1;
        const dpmr_header header = {.message_type = k_cases[c].mt,
                                    .called_id = 1464100U,
                                    .calling_id = 2928210U,
                                    .mode = 0U,
                                    .mi_type = 7U,
                                    .mi_detail = 0xFFU,
                                    .color_code = -1};
        dsd_test_capture_stderr cap;
        char printed[512];
        if (dsd_test_capture_stderr_begin(&cap, "dpmr_powersave") != 0) {
            return 1;
        }
        dpmr_process_header(opts, state, &header, 0);
        if (dsd_test_capture_stderr_end(&cap) != 0
            || dsd_test_capture_stderr_read(&cap, printed, sizeof(printed)) != 0) {
            return 1;
        }
        rc |= expect_text("powersave-print", printed, "Powersave WU=15", k_cases[c].powersave);
    }
    return rc;
}

/* Issue #890: the frame's line prints the identity the frame itself bound. Publication comes first, so the first good
   0/1 pair of a late entry prints its called ID, not the CRC ERR fields of the call snapshot before it. */
static int
test_frame_prints_the_identity_it_bound(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_second_half_frame_one = 1;
    dsd_test_capture_stderr cap;
    char printed[1024];
    if (dsd_test_capture_stderr_begin(&cap, "dpmr_frame_ids") != 0) {
        return 1;
    }
    (void)processdPMRvoice(opts, state);
    if (dsd_test_capture_stderr_end(&cap) != 0 || dsd_test_capture_stderr_read(&cap, printed, sizeof(printed)) != 0) {
        return 1;
    }
    rc |= expect_text("bound-id-printed", printed, "TG=0000000", 1);
    rc |= expect_text("no-crc-err-for-the-bound-id", printed, "TG=(CRC ERR)", 0);
    return rc;
}

/* One FS2 frame's halves as dpmr_play_voice_frames() is handed them: per half its CRC-7 verdict, frame number,
   Communications Mode, Version and SLD. */
typedef struct {
    unsigned crc[2];
    unsigned fn[2];
    unsigned mode[2];
    unsigned version[2];
    unsigned sld[2];
} bridge_frame;

static void
bridge_play(dsd_opts* opts, dsd_state* state, const bridge_frame* frame) {
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    for (int o = 0; o < 2; o++) {
        state->dPMRVoiceFS2Frame.CCHDataCrcOk[o] = frame->crc[o];
        state->dPMRVoiceFS2Frame.FrameNumbering[o] = frame->fn[o];
        state->dPMRVoiceFS2Frame.CommunicationMode[o] = frame->mode[o];
        state->dPMRVoiceFS2Frame.Version[o] = frame->version[o];
        state->dPMRVoiceFS2Frame.SlowData[o] = frame->sld[o];
    }
    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);
}

/* Issue #890: a half the bridge does not play keeps its place in the Scramble (four AMBE frames of 49 bits a half,
   frame 0 of each superframe from the key), so the next half played descrambles in phase. In a keyed Voice and
   attached data call (M = 101, TS 102 658 5.5.7, 11.1.1 Figure 11.3) consecutive payload frames carry speech; a frame
   0 whose CCH failed is not known to hold speech and stays muted, and the checked frame 1 after it (DP = 00) must play
   from frame 4 of the superframe's Scramble, not from the key. The same holds behind every half the bridge mutes: an
   unverified half, a data half, a data call's half, a clear half. */
static int
test_muted_halves_keep_the_scrambler_in_phase(void) {
    const unsigned long long key = 0x12345ULL;
    const unsigned speech = 0x00080U;        /* SLD: DP 00, a format and length */
    const unsigned data = (3U << 11U) | 20U; /* SLD: DP 11, 20 bytes */
    int rc = 0;
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;

    /* Frame 0 of a mode-101 call fails its CRC, frame 1 is checked speech: frame 1 plays from Scramble frame 4. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = key;
    const bridge_frame failed_frame0 = {
        .crc = {0U, 1U}, .fn = {0U, 1U}, .mode = {5U, 5U}, .version = {0U, 3U}, .sld = {speech, speech}};
    bridge_play(opts, state, &failed_frame0);
    rc |= expect_int("failed frame 0 is muted, frame 1 plays", (int)g_mbe_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_u64("frame 1 descrambles in phase", g_mbe_mi[i], scramble_register_at(key, 4 + i));
    }
    rc |= expect_u64("the frame leaves the register at frame 8", state->nxdn_payload_mi, scramble_register_at(key, 8));

    /* A frame whose halves both fail before anything is verified plays nothing, but the next, checked frames 2 and 3,
       play from Scramble frame 8. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = key;
    const bridge_frame unverified = {.crc = {0U, 0U}, .fn = {0U, 1U}, .mode = {0U, 0U}, .version = {3U, 3U}};
    bridge_play(opts, state, &unverified);
    rc |= expect_int("unverified halves play nothing", (int)g_mbe_calls, 0);
    reset_capture();
    const bridge_frame frames23 = {.crc = {1U, 1U}, .fn = {2U, 3U}, .mode = {0U, 0U}, .version = {3U, 3U}};
    bridge_play(opts, state, &frames23);
    rc |= expect_int("frames 2 and 3 play", (int)g_mbe_calls, 8);
    for (int i = 0; i < 8; i++) {
        rc |= expect_u64("frames 2 and 3 descramble in phase", g_mbe_mi[i], scramble_register_at(key, 8 + i));
    }

    /* Speech, then the attached data after PTT release (DP 11): the data half keeps its place, frames 2 and 3 next. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = key;
    const bridge_frame speech_then_data = {
        .crc = {1U, 1U}, .fn = {0U, 1U}, .mode = {5U, 5U}, .version = {3U, 3U}, .sld = {speech, data}};
    bridge_play(opts, state, &speech_then_data);
    rc |= expect_int("speech plays, data does not", (int)g_mbe_calls, 4);
    rc |= expect_int("the data half kept its place", (int)state->dpmr_scramble_place, 9);

    /* A scrambled data call's halves reach no vocoder, write no register, and keep their place. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = key;
    const bridge_frame data_call = {.crc = {1U, 1U}, .fn = {0U, 1U}, .mode = {2U, 2U}, .version = {3U, 3U}};
    bridge_play(opts, state, &data_call);
    rc |= expect_int("a data call plays nothing", (int)g_mbe_calls, 0);
    rc |= expect_u64("its halves wrote no register", state->nxdn_payload_mi, 0ULL);
    rc |= expect_int("its halves kept their place", (int)state->dpmr_scramble_place, 9);

    /* A clear data half is muted; the scrambled frame 3 beside it plays from its own place, frame 12. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->R = key;
    state->nxdn_payload_mi = 0x7777ULL; /* whatever the register held */
    const bridge_frame clear_data_half = {.crc = {1U, 1U}, .fn = {2U, 3U}, .mode = {2U, 0U}, .version = {0U, 3U}};
    bridge_play(opts, state, &clear_data_half);
    rc |= expect_int("the clear data half is muted", (int)g_mbe_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_u64("frame 3 plays from its place", g_mbe_mi[i], scramble_register_at(key, 12 + i));
    }

    /* Without a key nothing steps, as nothing descrambles. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    bridge_play(opts, state, &failed_frame0);
    rc |= expect_u64("no key, no step", state->nxdn_payload_mi, 0ULL);
    return rc;
}

/* Issue #890: an FS2 frame carries payload frames 0 and 1, or 2 and 3 (Figure 4.8): half 0 is even, half 1 odd. A
   checked half whose number has the wrong parity passed its CRC-7 by chance, and two checked halves that are no pair
   (0/3, or 0/2 and 1/3, where one number has the right parity) cannot both be right: neither places the scrambler,
   directly or through its neighbour, and the halves play from the place the transmission tracked. */
static int
test_impossible_half_numbers_keep_the_tracked_place(void) {
    const unsigned long long key = 0x12345ULL;

    static const struct {
        const char* tag;
        unsigned crc[2];
        unsigned fn[2];
    } k_cases[] = {
        {"half 1 checked as 2", {0U, 1U}, {1U, 2U}},       {"half 0 checked as 1", {1U, 0U}, {1U, 0U}},
        {"halves checked as 0 and 3", {1U, 1U}, {0U, 3U}}, {"halves checked as 0 and 2", {1U, 1U}, {0U, 2U}},
        {"halves checked as 1 and 3", {1U, 1U}, {1U, 3U}}, {"halves checked as 3 and 1", {1U, 1U}, {3U, 1U}},
    };

    int rc = 0;
    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->R = key;
        /* Frames 0 and 1 of a scrambled voice call: frames 2 and 3 come next. */
        const bridge_frame frames01 = {.crc = {1U, 1U}, .fn = {0U, 1U}, .mode = {0U, 0U}, .version = {3U, 3U}};
        bridge_play(opts, state, &frames01);
        reset_capture();
        const bridge_frame next = {.crc = {k_cases[c].crc[0], k_cases[c].crc[1]},
                                   .fn = {k_cases[c].fn[0], k_cases[c].fn[1]},
                                   .mode = {0U, 0U},
                                   .version = {3U, 3U}};
        bridge_play(opts, state, &next);
        rc |= expect_int(k_cases[c].tag, (int)g_mbe_calls, 8);
        for (int i = 0; i < 8; i++) {
            rc |= expect_u64(k_cases[c].tag, g_mbe_mi[i], scramble_register_at(key, 8 + i));
        }
    }
    return rc;
}

/* Issue #890: a frame played before its transmission is confirmed (one weak frame waits for the next to run on from it)
   still takes its place in the Scramble: the first frame of a keyed call, frames 0 and 1, is muted, and the second,
   frames 2 and 3, which confirms the transmission, plays from Scramble frame 8, not from the key. */
static int
test_unconfirmed_frame_keeps_the_scrambler_in_phase(void) {
    const unsigned long long key = 0x12345ULL;
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    int rc = 0;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state->R = key;

    reset_capture();
    g_version3 = 1;
    g_second_half_frame_one = 1;
    g_fail_half = 1;
    state->symbolcnt = 40000U;
    rc |= expect_int("first frame is weak", processdPMRvoice(opts, state), 1);
    rc |= expect_int("first frame unconfirmed", state->dpmr_confirmed, 0);
    rc |= expect_int("first frame plays nothing", (int)g_mbe_calls, 0);
    rc |= expect_int("first frame is scrambled", (int)state->dPMRVoiceFS2Frame.Scrambled[0], DPMR_HALF_SCRAMBLED);

    reset_capture();
    g_version3 = 1;
    g_frames_two_three = 1;
    g_fail_half = 1;
    state->symbolcnt = 40000U + 384U;
    rc |= expect_int("second frame is weak", processdPMRvoice(opts, state), 1);
    rc |= expect_int("second frame confirms", state->dpmr_confirmed, 1);
    rc |= expect_int("second frame plays", (int)g_mbe_calls, 8);
    for (int i = 0; i < 8; i++) {
        rc |= expect_u64("second frame descrambles in phase", g_mbe_mi[i], scramble_register_at(key, 8 + i));
    }

    /* An unconfirmed frame nothing says is scrambled may be noise where an NXDN call works in the same register: it
       leaves the register alone. */
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state->R = key;
    state->nxdn_payload_mi = 0x7777ULL;
    reset_capture();
    g_fail_both = 1;
    rc |= expect_int("noise frame checks nothing", processdPMRvoice(opts, state), 0);
    rc |= expect_u64("noise frame leaves the register", state->nxdn_payload_mi, 0x7777ULL);
    reset_capture();
    g_fail_half = 1; /* a checked clear frame 0, unconfirmed */
    state->symbolcnt = 50000U;
    rc |= expect_int("clear frame is weak", processdPMRvoice(opts, state), 1);
    rc |= expect_u64("clear unconfirmed frame leaves the register", state->nxdn_payload_mi, 0x7777ULL);
    return rc;
}

/* Issue #890: under Auto a false dPMR FS2 during an NXDN48 call can read one CRC-valid CCH half that says scrambled
   without confirming dPMR. Before confirmation the dPMR voice path writes no state another protocol owns: the NXDN
   call's cipher type, working register, IV flag and the synctype stay as they were, with a scalar key loaded or
   not. */
static int
test_unconfirmed_frame_leaves_nxdn_crypto_alone(void) {
    int rc = 0;
    for (int keyed = 0; keyed < 2; keyed++) {
        dsd_opts* opts = g_opts;
        dsd_state* state = g_state;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        DSD_MEMSET(state, 0, sizeof(*state));
        opts->frame_nxdn48 = 1;
        opts->frame_dpmr = 1;
        state->synctype = DSD_SYNC_DPMR_FS2_POS;
        state->dpmr_color_code = -1;
        state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
        state->R = keyed ? 0x12345ULL : 0ULL;
        /* The NXDN48 call: AES (cipher 3) with the IV its VCALL_IV carried. */
        state->nxdn_cipher_type = 0x03;
        state->nxdn_payload_mi = 0x0102030405060708ULL;
        state->nxdn_iv_on_air = 1U;
        reset_capture();
        g_version3 = 1;
        g_second_half_frame_one = 1;
        g_fail_half = 1; /* half 0 checked, frame 0, scrambled */
        state->symbolcnt = 60000U;
        rc |= expect_int("false fs2 checks one half", processdPMRvoice(opts, state), 1);
        rc |= expect_int("false fs2 unconfirmed", state->dpmr_confirmed, 0);
        rc |= expect_int("false fs2 plays nothing", (int)g_mbe_calls, 0);
        rc |= expect_int("nxdn cipher type kept", (int)state->nxdn_cipher_type, 0x03);
        rc |= expect_u64("nxdn register kept", state->nxdn_payload_mi, 0x0102030405060708ULL);
        rc |= expect_int("nxdn iv flag kept", (int)state->nxdn_iv_on_air, 1);
        rc |= expect_int("synctype kept", state->synctype, DSD_SYNC_DPMR_FS2_POS);
        rc |= expect_int("no scramble step", g_skip_calls, 0);
        /* dPMR keeps its own place (frames 2 and 3 next), which a transmission boundary forgets. */
        rc |= expect_int("dpmr place learned", (int)state->dpmr_scramble_place, 9);
        dpmr_scrambling_reset(state);
        rc |= expect_int("boundary forgets the place", (int)state->dpmr_scramble_place, 0);
    }
    return rc;
}

/* Issue #890: a checked end frame forgets the transmission's confirmation (dsd_dispatch_handle_dpmr() calls
   dpmr_confirm_reset()). A false FS2 on the carrier's tail with one CRC-valid half then opens no row and plays nothing;
   only a running second frame confirms again. */
static int
test_frame_after_a_checked_end_must_reconfirm(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    dsd_call_snapshot call;
    int rc = 0;

    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_color_code = -1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state->dpmr_confirmed = 1; /* the transmission the end frame closed */
    dpmr_confirm_reset(state);

    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 20000U;
    rc |= expect_int("tail-frame-verdict", processdPMRvoice(opts, state), 1);
    rc |= expect_int("tail-frame-unconfirmed", state->dpmr_confirmed, 0);
    rc |= expect_int("tail-frame-no-call", dsd_call_state_get(state, 0U, &call) > 0, 0);
    rc |= expect_int("tail-frame-plays-nothing", (int)g_mbe_calls, 0);

    reset_capture();
    g_fail_half = 1;
    state->symbolcnt = 20000U + 384U;
    (void)processdPMRvoice(opts, state);
    rc |= expect_int("running-frame-confirms", state->dpmr_confirmed, 1);
    rc |= expect_int("running-frame-call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    return rc;
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    if (g_opts == NULL || g_state == NULL) {
        free(g_state);
        free(g_opts);
        DSD_FPRINTF(stderr, "fixture allocation failed\n");
        return 1;
    }
    int rc = 0;
    rc |= test_first_group_scrambler_bridge_mutes_without_key();
    rc |= test_second_group_scrambler_bridge_unmutes_with_key();
    rc |= test_attached_data_frames_are_no_voice();
    rc |= test_muted_halves_keep_the_scrambler_in_phase();
    rc |= test_unconfirmed_frame_keeps_the_scrambler_in_phase();
    rc |= test_impossible_half_numbers_keep_the_tracked_place();
    rc |= test_unconfirmed_frame_leaves_nxdn_crypto_alone();
    rc |= test_scrambler_secret_is_not_published_as_key_id();
    rc |= test_scrambling_is_per_half_and_crc_verified();
    rc |= test_enc_indication_is_the_frames_own();
    rc |= test_deinterleave_transposes_6x12_blocks();
    rc |= test_crc7_and_air_interface_id_helpers();
    rc |= test_process_dpmr_voice_weak_frames_confirm_only_when_they_run();
    rc |= test_frame_after_a_checked_end_must_reconfirm();
    rc |= test_frame_prints_the_identity_it_bound();
    rc |= test_failed_halves_play_by_the_checked_mode();
    rc |= test_powersave_read_only_from_headers_that_carry_it();
    rc |= test_non_call_header_authorizes_no_playback();
    rc |= test_new_call_without_a_channel_code_has_none();
    rc |= test_superframe_part_updates_called_and_calling_ids();
    rc |= test_service_and_kind_come_from_the_checked_half();
    rc |= test_scrambler_reloads_only_at_a_checked_frame_zero();
    rc |= test_unconfirmed_superframe_part_publishes_nothing();
    rc |= test_id_print_does_not_publish_the_color_code();
    rc |= test_confirmed_target_only_call_publishes_the_color_code();
    rc |= test_dpmr_channel_not_colour_code();
    rc |= test_unconfirmed_frame_with_retained_caller_publishes_no_color_code();
    rc |= test_confirmed_call_prints_its_identities();
    rc |= test_noise_frame_after_another_protocols_call_prints_no_identity();
    rc |= test_id_print_side_effects_suppress_invalid_ids();
    rc |= test_process_dpmr_voice_zero_stream_updates_cch_and_dispatches_voice();
    rc |= test_process_dpmr_voice_split_by_a_carrier_boundary_publishes_nothing();
    rc |= test_process_dpmr_voice_search_evidence_follows_the_published_call();
    rc |= test_process_dpmr_voice_cut_by_an_input_interruption_publishes_nothing();

    free(g_state);
    free(g_opts);
    if (rc == 0) {
        printf("DPMR_VOICE_BRIDGE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
