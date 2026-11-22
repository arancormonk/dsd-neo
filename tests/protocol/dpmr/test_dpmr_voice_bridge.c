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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dpmr_confirm.h"
#include "dpmr_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

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

/* The CRC-7 of a clean frame's second CCH half: all zero but its frame number, 01 (frame 2), so the pair reads as the
   called-ID pair (frames 1 and 2 behind one FS2, TS 102 658 Figure 5.4). */
static uint8_t
second_half_crc(void) {
    uint8_t bits[41] = {0};
    bits[1] = 1U;
    return dpmr_crc7(bits, 41U);
}

bool
Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    (void)rxBits;
    if (decodedBits != NULL && nbCodewords > 0) {
        const int block = g_hamming_calls % 12;
        DSD_MEMSET(decodedBits, g_cch_noise ? 1 : 0, (size_t)nbCodewords * 8U);
        if (!g_cch_noise && block == 6) {
            decodedBits[1] = 1; /* second half's FN = 01 */
        }
        if (!g_cch_noise && block == 11) {
            const uint8_t crc = second_half_crc();
            for (int k = 0; k < 7; k++) {
                decodedBits[1 + k] = (unsigned char)((crc >> (6 - k)) & 1U); /* CCH bits 41..47 */
            }
        }
        if (g_fail_half >= 0 && block == g_fail_half * 6) {
            decodedBits[0] = 1;
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
        g_mbe_mi[g_mbe_calls] = state->nxdn_payload_mi;
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
    static dsd_opts opts;
    static dsd_state state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();

    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.nxdn_payload_mi = 0x7777ULL;
    state.dPMRVoiceFS2Frame.FrameNumbering[0] = 0;
    state.dPMRVoiceFS2Frame.CommunicationMode[0] = 0;
    state.dPMRVoiceFS2Frame.Version[0] = 3;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1;
    state.dPMRVoiceFS2Frame.FrameNumbering[1] = 1;
    /* A verified data half plays nothing (a failed one's mode is not trusted, issue #903). */
    state.dPMRVoiceFS2Frame.CommunicationMode[1] = 2;
    state.dPMRVoiceFS2Frame.Version[1] = 3;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1;

    dpmr_resolve_half_scrambling(&state);
    dpmr_play_voice_frames(&opts, &state, ambe_fr);

    int rc = 0;
    rc |= expect_int("first-group-mbe-calls", (int)g_mbe_calls, 4);
    rc |= expect_int("first-group-ms-calls", g_ms_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("first-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("first-group-cipher", g_mbe_cipher[i], 1);
        rc |= expect_u64("first-group-mi-reset", g_mbe_mi[i], 0ULL);
    }
    rc |= expect_int("first-group-synctype-restored", state.synctype, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_int("first-group-cipher-reset", (int)state.nxdn_cipher_type, 0);
    rc |= expect_int("first-group-transmission-scrambled", state.dpmr_scrambled, 2);
    return rc;
}

static int
test_second_group_scrambler_bridge_unmutes_with_key(void) {
    static dsd_opts opts;
    static dsd_state state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();

    opts.floating_point = 1;
    state.synctype = DSD_SYNC_DPMR_FS3_POS;
    state.R = 0x12345ULL;
    state.nxdn_payload_mi = 0x8888ULL;
    state.dPMRVoiceFS2Frame.FrameNumbering[0] = 0;
    /* A verified data half plays nothing (a failed one's mode is not trusted, issue #903). */
    state.dPMRVoiceFS2Frame.CommunicationMode[0] = 2;
    state.dPMRVoiceFS2Frame.Version[0] = 3;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1;
    state.dPMRVoiceFS2Frame.FrameNumbering[1] = 1;
    state.dPMRVoiceFS2Frame.CommunicationMode[1] = 5;
    state.dPMRVoiceFS2Frame.Version[1] = 3;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1;

    dpmr_resolve_half_scrambling(&state);
    dpmr_play_voice_frames(&opts, &state, ambe_fr);

    int rc = 0;
    rc |= expect_int("second-group-mbe-calls", (int)g_mbe_calls, 4);
    // Float output (-y) plays through the same mono mixer (#652).
    rc |= expect_int("second-group-ms-calls", g_ms_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("second-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("second-group-cipher", g_mbe_cipher[i], 1);
        rc |= expect_u64("second-group-mi-reset", g_mbe_mi[i], 0ULL);
    }
    rc |= expect_int("second-group-synctype-restored", state.synctype, DSD_SYNC_DPMR_FS3_POS);
    rc |= expect_int("second-group-cipher-reset", (int)state.nxdn_cipher_type, 0);
    return rc;
}

/* Issue #655: each half is scrambled or clear on its own, and only a half whose CCH CRC-7 passed says which. A clear
 * half after a scrambled one decodes clear (no NXDN scrambler, synctype left dPMR); a failed half reading Version 3 in
 * a clear transmission stays clear; a failed half after a verified scrambled one stays scrambled; a transmission
 * boundary forgets the verdict, and a failed half then takes this frame's verified half, or, with none, plays nothing:
 * confirmation outlives the boundary, so a scrambled call after a clear one never plays an undecodable half as clear. */
static int
bridge_run(dsd_state* state, unsigned v0, unsigned crc0, unsigned v1, unsigned crc1) {
    static dsd_opts opts;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(&opts, 0, sizeof(opts));
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
    dpmr_play_voice_frames(&opts, state, ambe_fr);
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
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    rc |= expect_int("scrambled then clear calls", bridge_run(&state, 3U, 1U, 0U, 1U), 8);
    rc |= expect_halves("scrambled then clear", 1, 0);
    rc |= expect_int("scrambled then clear leaves clear", state.dpmr_scrambled, 1);
    rc |= expect_int("next clear frame", bridge_run(&state, 0U, 1U, 0U, 1U), 8);
    rc |= expect_halves("next clear frame", 0, 0);

    rc |= expect_int("failed Version 3 in a clear call", bridge_run(&state, 3U, 0U, 0U, 1U), 8);
    rc |= expect_halves("failed Version 3 in a clear call", 0, 0);

    rc |= expect_int("verified scrambled", bridge_run(&state, 3U, 1U, 3U, 1U), 8);
    rc |= expect_halves("verified scrambled", 1, 1);
    rc |= expect_int("failed clear-looking halves in a scrambled call", bridge_run(&state, 0U, 0U, 0U, 0U), 8);
    rc |= expect_halves("failed clear-looking halves in a scrambled call", 1, 1);

    dpmr_scrambling_reset(&state);
    rc |= expect_int("boundary forgets", state.dpmr_scrambled, 0);
    rc |= expect_int("failed half before a verified scrambled one", bridge_run(&state, 0U, 0U, 3U, 1U), 8);
    rc |= expect_halves("failed half before a verified scrambled one", 1, 1);
    // A verified clear call, its end frame, then a call whose first frame fails both CRCs: nothing of it plays.
    rc |= expect_int("verified clear call", bridge_run(&state, 0U, 1U, 0U, 1U), 8);
    dpmr_scrambling_reset(&state);
    rc |= expect_int("nothing verified since the boundary", bridge_run(&state, 0U, 0U, 3U, 0U), 0);
    rc |= expect_int("nothing verified halves", (int)state.dPMRVoiceFS2Frame.Scrambled[0], DPMR_HALF_UNVERIFIED);
    rc |= expect_int("nothing verified cipher reset", (int)state.nxdn_cipher_type, 0);
    // Its next frame verifies one half: both play by it.
    rc |= expect_int("then a verified scrambled half", bridge_run(&state, 0U, 0U, 3U, 1U), 8);
    rc |= expect_halves("then a verified scrambled half", 1, 1);
    return rc;
}

/* One CCH half as dpmr_decode_cch_frames() leaves it: its CRC-7 verdict and the fields it decoded. */
static void
set_half(dsd_state* state, int o, unsigned crc_ok, unsigned fn, unsigned mode, unsigned version, unsigned ep) {
    dPMRVoiceFS2Frame_t* f = &state->dPMRVoiceFS2Frame;
    f->CCHDataCrcOk[o] = crc_ok;
    f->FrameNumbering[o] = fn;
    f->CommunicationMode[o] = mode;
    f->Version[o] = version;
    f->CommsFormat[o] = 0U;
    f->EmergencyPriority[o] = ep;
}

/* A zeroed decoder fixture on the heap, as dsd_state and dsd_opts are everywhere (AGENTS.md); NULL on failure. The
   call-state stub's extension is static storage with no cleanup, so release_fixture() frees the state alone. */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} bridge_fixture;

static void
release_fixture(bridge_fixture* fx) {
    free(fx->opts);
    free(fx->state);
    fx->opts = NULL;
    fx->state = NULL;
}

static int
new_fixture(bridge_fixture* fx) {
    fx->opts = calloc(1, sizeof(*fx->opts));
    fx->state = calloc(1, sizeof(*fx->state));
    if (fx->opts == NULL || fx->state == NULL) {
        DSD_FPRINTF(stderr, "fixture allocation failed\n");
        release_fixture(fx);
        return 1;
    }
    return 0;
}

/* Play the frame set_half() built, with the scrambler's working register at @p mi; how many vocoder frames ran. */
static int
play_frame(dsd_opts* opts, dsd_state* state, unsigned long long mi) {
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();
    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->nxdn_payload_mi = mi;
    dpmr_resolve_half_scrambling(state);
    dpmr_play_voice_frames(opts, state, ambe_fr);
    return (int)g_mbe_calls;
}

/* Issue #903: the frame number of a CCH half whose CRC-7 failed is not trusted, so it never reloads the scrambler
   (FN 0, frame 1). A faded middle half reading 0 left the running register for the rest of the superframe reloaded
   from the key. A failed half's position still follows from its pair partner when that one passed: frames 1/2 and 3/4
   share one FS2 (TS 102 658 Figure 5.4), so a passing frame 2 makes the half before it frame 1. With neither half
   checked, the transmission's cadence places the frame: a verified FN 2/3 pair means the next on-time FS2 carries
   FN 0/1, so the scrambler reloads there rather than descrambling a whole superframe with the old register. A gap in
   the cadence drops that prediction. */
static int
test_failed_cch_frame_number_does_not_reset_the_scrambler(void) {
    bridge_fixture fx;
    if (new_fixture(&fx) != 0) {
        return 1;
    }
    dsd_state* state = fx.state;
    int rc = 0;

    /* A faded frame 3 reading FN 0, its frame 4 verified. */
    set_half(state, 0, 0U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 3U, 0U, 3U, 0U);
    rc |= expect_int("faded middle half plays", play_frame(fx.opts, state, 0x1234ULL), 8);
    for (int i = 0; i < 8; i++) {
        rc |= expect_u64("faded middle half keeps the register", g_mbe_mi[i], 0x1234ULL);
    }

    /* On time, the next pair fails both checks, its first half reading FN 2: the cadence makes it frames 1/2. */
    set_half(state, 0, 0U, 2U, 0U, 3U, 0U);
    set_half(state, 1, 0U, 3U, 0U, 3U, 0U);
    rc |= expect_int("predicted frames 1/2 play", play_frame(fx.opts, state, 0x5678ULL), 8);
    rc |= expect_u64("predicted frame 1 reloads", g_mbe_mi[0], 0ULL);
    /* And the pair after it, failed again with its first half reading FN 0, is frames 3/4. */
    set_half(state, 0, 0U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 0U, 1U, 0U, 3U, 0U);
    rc |= expect_int("predicted frames 3/4 play", play_frame(fx.opts, state, 0x6789ULL), 8);
    rc |= expect_u64("predicted frame 3 keeps the register", g_mbe_mi[0], 0x6789ULL);

    /* The prediction is for frames 1/2 now, but this frame's verified half reads FN 1, which a first half cannot
       carry: a passing half outranks the cadence, so nothing places the frame and nothing reloads. */
    set_half(state, 0, 1U, 1U, 0U, 3U, 0U);
    set_half(state, 1, 0U, 0U, 0U, 3U, 0U);
    rc |= expect_int("misplaced verified half plays", play_frame(fx.opts, state, 0x7777ULL), 8);
    rc |= expect_u64("misplaced verified half keeps the register", g_mbe_mi[0], 0x7777ULL);
    rc |= expect_int("misplaced verified half drops the prediction", state->dpmr_expected_pair, 0);

    /* A verified frames 3/4 predicts frames 1/2, but a gap in the cadence (here the first FS2 timed; equally one off
       the frame spacing) drops that: nothing places the failed pair after it. */
    set_half(state, 0, 1U, 2U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 3U, 0U, 3U, 0U);
    rc |= expect_int("verified frames 3/4 play", play_frame(fx.opts, state, 0x6789ULL), 8);
    rc |= expect_int("verified frames 3/4 predict frames 1/2", state->dpmr_expected_pair, 1);
    dpmr_confirm_note_frame_start(state, 100U);
    rc |= expect_int("gap drops the prediction", state->dpmr_expected_pair, 0);
    set_half(state, 0, 0U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 0U, 1U, 0U, 3U, 0U);
    rc |= expect_int("unplaced pair plays", play_frame(fx.opts, state, 0x789AULL), 8);
    rc |= expect_u64("unplaced pair keeps the register", g_mbe_mi[0], 0x789AULL);

    /* A failed frame 1 that misread its number, behind a verified frame 2: the pair places it, and it reloads. */
    set_half(state, 0, 0U, 2U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 1U, 0U, 3U, 0U);
    rc |= expect_int("placed frame 1 plays", play_frame(fx.opts, state, 0x9ABCULL), 8);
    rc |= expect_u64("placed frame 1 reloads", g_mbe_mi[0], 0ULL);

    /* Frames that run on keep the prediction: frames 3/4 (no reload), then 1/2 (reload), both failed. */
    dpmr_confirm_note_frame_start(state, 100U + DPMR_CONFIRM_FRAME_SYMBOLS);
    set_half(state, 0, 0U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 0U, 1U, 0U, 3U, 0U);
    rc |= expect_int("on-time frames 3/4 play", play_frame(fx.opts, state, 0xABCDULL), 8);
    rc |= expect_u64("on-time frame 3 keeps the register", g_mbe_mi[0], 0xABCDULL);
    dpmr_confirm_note_frame_start(state, 100U + (2U * DPMR_CONFIRM_FRAME_SYMBOLS) + DPMR_CONFIRM_FRAME_SYMBOLS_SLIP);
    rc |= expect_int("on-time frames 1/2 play", play_frame(fx.opts, state, 0xBCDEULL), 8);
    rc |= expect_u64("on-time frame 1 reloads", g_mbe_mi[0], 0ULL);

    /* A transmission boundary drops it too. */
    dpmr_scrambling_reset(state);
    rc |= expect_int("boundary drops the prediction", state->dpmr_expected_pair, 0);

    /* A verified frame 1 reloads. */
    set_half(state, 0, 1U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 1U, 0U, 3U, 0U);
    rc |= expect_int("verified frame 1 plays", play_frame(fx.opts, state, 0xDEF0ULL), 8);
    rc |= expect_u64("verified frame 1 reloads", g_mbe_mi[0], 0ULL);

    /* Halves that both pass but name different pairs (FN 0 with FN 3) place nothing: no reload, and no prediction
       for the next frame. */
    set_half(state, 0, 1U, 0U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 3U, 0U, 3U, 0U);
    rc |= expect_int("disagreeing halves play", play_frame(fx.opts, state, 0xEF01ULL), 8);
    rc |= expect_u64("disagreeing halves keep the register", g_mbe_mi[0], 0xEF01ULL);
    rc |= expect_int("disagreeing halves drop the prediction", state->dpmr_expected_pair, 0);

    /* A passing second half reading FN 0 cannot be frame 2, so it never reloads the second half. */
    set_half(state, 0, 0U, 1U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 0U, 0U, 3U, 0U);
    rc |= expect_int("misplaced second half plays", play_frame(fx.opts, state, 0xF012ULL), 8);
    rc |= expect_u64("misplaced second half keeps the register", g_mbe_mi[4], 0xF012ULL);

    /* Both halves pass, the first reading FN 1 which a first half cannot carry, the second FN 1: the second places
       pair 0, the same pair the cadence follows, so the first half reloads. */
    set_half(state, 0, 1U, 1U, 0U, 3U, 0U);
    set_half(state, 1, 1U, 1U, 0U, 3U, 0U);
    rc |= expect_int("mixed-validity pair plays", play_frame(fx.opts, state, 0x1357ULL), 8);
    rc |= expect_u64("mixed-validity pair reloads the first half", g_mbe_mi[0], 0ULL);
    rc |= expect_int("mixed-validity pair predicts frames 3/4", state->dpmr_expected_pair, 2);
    release_fixture(&fx);
    return rc;
}

/* Issue #903: nor is a failed half's Communication Mode trusted. Once a verified half has said what the transmission
   carries, a failed half reading a data mode does not cut its voice, and one reading a voice mode does not play a data
   transmission's payload as speech. */
static int
test_failed_cch_mode_does_not_switch_voice(void) {
    bridge_fixture fx;
    if (new_fixture(&fx) != 0) {
        return 1;
    }
    dsd_state* state = fx.state;
    int rc = 0;

    set_half(state, 0, 0U, 0U, 2U, 0U, 0U);
    set_half(state, 1, 1U, 1U, 0U, 0U, 0U);
    rc |= expect_int("failed data-mode half in a voice call plays", play_frame(fx.opts, state, 0ULL), 8);
    set_half(state, 0, 0U, 2U, 3U, 0U, 0U);
    set_half(state, 1, 0U, 3U, 4U, 0U, 0U);
    rc |= expect_int("failed data-mode pair in a voice call plays", play_frame(fx.opts, state, 0ULL), 8);

    /* The next transmission carries data, which a verified pair says. */
    dpmr_scrambling_reset(state);
    set_half(state, 0, 1U, 0U, 2U, 0U, 0U);
    set_half(state, 1, 1U, 1U, 2U, 0U, 0U);
    rc |= expect_int("verified data plays nothing", play_frame(fx.opts, state, 0ULL), 0);
    set_half(state, 0, 0U, 2U, 0U, 0U, 0U);
    set_half(state, 1, 0U, 3U, 1U, 0U, 0U);
    rc |=
        expect_int("failed voice-mode pair in a data transmission plays nothing", play_frame(fx.opts, state, 0ULL), 0);
    release_fixture(&fx);
    return rc;
}

/* Issue #903: emergency and the service options come only from a CCH half whose CRC-7 passed. A failed half in a
   confirmed transmission set or cleared emergency and replaced the service options. */
static int
test_failed_cch_changes_no_service_metadata(void) {
    bridge_fixture fx;
    if (new_fixture(&fx) != 0) {
        return 1;
    }
    dsd_opts* opts = fx.opts;
    dsd_state* state = fx.state;
    int rc = 0;
    dsd_call_snapshot call;

    state->synctype = DSD_SYNC_DPMR_FS2_POS;
    state->dpmr_confirmed = 1;
    state->dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);

    /* Verified: voice + slow data, clear, no emergency. */
    set_half(state, 0, 1U, 0U, 1U, 0U, 0U);
    set_half(state, 1, 1U, 1U, 1U, 0U, 0U);
    dpmr_resolve_half_scrambling(state);
    dpmr_publish_call(opts, state);
    rc |= expect_int("verified call", dsd_call_state_get(state, 0U, &call) > 0, 1);
    rc |= expect_int("verified no emergency", call.emergency, 0);
    rc |= expect_int("verified service options", call.service_options, 0x10);
    rc |= expect_int("verified has metadata", call.has_service_metadata, 1);
    const uint64_t epoch = call.epoch;

    /* Both halves failed, reading emergency, data mode, another version and format. */
    set_half(state, 0, 0U, 2U, 2U, 2U, 1U);
    set_half(state, 1, 0U, 3U, 2U, 2U, 1U);
    state->dPMRVoiceFS2Frame.CommsFormat[0] = 3U;
    dpmr_resolve_half_scrambling(state);
    dpmr_publish_call(opts, state);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("failed pair sets no emergency", call.emergency, 0);
    rc |= expect_int("failed pair keeps service options", call.service_options, 0x10);
    rc |= expect_int("failed pair keeps the call", call.epoch == epoch, 1);

    /* The first half failed reading emergency; the second verified none, in voice mode. */
    set_half(state, 0, 0U, 0U, 2U, 2U, 1U);
    set_half(state, 1, 1U, 1U, 0U, 0U, 0U);
    dpmr_resolve_half_scrambling(state);
    dpmr_publish_call(opts, state);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("verified second half: no emergency", call.emergency, 0);
    rc |= expect_int("verified second half: its service options", call.service_options, 0x00);

    /* A verified emergency sets it, and a failed half reading none does not clear it. */
    set_half(state, 1, 1U, 1U, 0U, 0U, 1U);
    dpmr_publish_call(opts, state);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("verified emergency", call.emergency, 1);
    set_half(state, 0, 0U, 2U, 0U, 0U, 0U);
    set_half(state, 1, 0U, 3U, 0U, 0U, 0U);
    dpmr_publish_call(opts, state);
    (void)dsd_call_state_get(state, 0U, &call);
    rc |= expect_int("failed halves keep the emergency", call.emergency, 1);
    release_fixture(&fx);
    return rc;
}

static int
test_scrambler_secret_is_not_published_as_key_id(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.R = 0x12345ULL;
    state.dPMRVoiceFS2Frame.Version[0] = 3U;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
    dpmr_resolve_half_scrambling(&state);
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    /* Redaction, not the gate: publishing at all needs a proved transmission (#407). */
    state.dpmr_confirmed = 1;
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    int rc = 0;
    rc |= expect_int("scrambler-call-present", dsd_call_state_get(&state, 0U, &call) > 0, 1);
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
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;
    dsd_call_snapshot call;

    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_confirmed = 1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    state.dPMRVoiceFS2Frame.Version[0] = 3U;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1U;
    dpmr_resolve_half_scrambling(&state);
    dpmr_publish_call(&opts, &state);
    rc |=
        expect_int("verified scrambled call", dsd_call_state_get(&state, 0U, &call) > 0 && call.enc_received == 1U, 1);
    const uint64_t first = call.epoch;

    // The call ends; the next one's first frame verifies neither half, so the learned scrambling still plays it.
    (void)dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_TERMINATOR);
    state.dPMRVoiceFS2Frame.Version[0] = 0U;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 0U;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 0U;
    dpmr_resolve_half_scrambling(&state);
    rc |= expect_int("learned scrambling plays it", (int)state.dPMRVoiceFS2Frame.Scrambled[0], DPMR_HALF_SCRAMBLED);
    dpmr_publish_call(&opts, &state);
    rc |= expect_int("next call opens", dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch != first, 1);
    rc |= expect_int("nothing of its own says encrypted", call.enc_received, 0);

    // A verified clear half: clear.
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[1] = 1U;
    state.dPMRVoiceFS2Frame.Version[1] = 0U;
    dpmr_resolve_half_scrambling(&state);
    dpmr_publish_call(&opts, &state);
    rc |= expect_int("verified clear", dsd_call_state_get(&state, 0U, &call) > 0 && call.enc_received == 0U, 1);
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
    char id[DPMR_AIR_INTERFACE_ID_TEXT_SIZE];
    int rc = 0;

    rc |= expect_int("crc7-empty", dpmr_crc7(crc_bits, 0U), 0x00);
    rc |= expect_int("crc7-pattern", dpmr_crc7(crc_bits, 12U), 0x24);
    cch_bits[41] = 1U;
    cch_bits[43] = 1U;
    cch_bits[46] = 1U;
    rc |= expect_int("cch-crc-extract", dpmr_extract_cch_crc(cch_bits), 0x52);

    dpmr_convert_air_interface_id(0U, id, sizeof(id));
    rc |= expect_int("aiid-zero", strcmp(id, "0000000"), 0);
    dpmr_convert_air_interface_id(10U, id, sizeof(id));
    rc |= expect_int("aiid-star-low", strcmp(id, "000000*"), 0);
    dpmr_convert_air_interface_id(1464100U, id, sizeof(id));
    rc |= expect_int("aiid-first-digit", strcmp(id, "1000000"), 0);
    dpmr_convert_air_interface_id(1464110U, id, sizeof(id));
    rc |= expect_int("aiid-first-digit-with-star", strcmp(id, "100000*"), 0);
    dpmr_convert_air_interface_id(0xDF6767U, id, sizeof(id));
    rc |= expect_int("aiid-top-of-dial-space", strcmp(id, "999****"), 0);
    return rc;
}

/* Issue #903: B2 maps only the dial strings "0000000".."999****" (TS 102 658 A.1.2.1.1.6, Table A.1), the IDs up to
   0xDF6767. Above that the first "digit" ran past 9 and printed ';' (0xF8339C read ";10***0"), or 0xDF6768 read as the
   plausible "*000000". An ID there has no dial string: it renders as its decimal number, 8 digits, which no 7-character
   dial string can be; a buffer too short for that gets hex, and one too short for anything gets nothing. */
static int
test_air_interface_ids_above_the_dial_space_render_as_numbers(void) {
    char id[DPMR_AIR_INTERFACE_ID_TEXT_SIZE];
    char want[16];
    int rc = 0;
    int bad = 0;
    for (uint32_t ai = 0xDF6768U; ai <= 0xFFFFFFU && bad < 4; ai++) {
        dpmr_convert_air_interface_id(ai, id, sizeof(id));
        DSD_SNPRINTF(want, sizeof(want), "%u", (unsigned)ai);
        if (strcmp(id, want) != 0) {
            DSD_FPRINTF(stderr, "aiid 0x%06X rendered \"%s\" want \"%s\"\n", (unsigned)ai, id, want);
            bad++;
        }
    }
    rc |= expect_int("aiid-above-dial-space-decimal", bad, 0);

    dpmr_convert_air_interface_id(0xDF6768U, id, sizeof(id));
    rc |= expect_int("aiid-first-reserved", strcmp(id, "14641000"), 0);
    dpmr_convert_air_interface_id(0xF8339CU, id, sizeof(id));
    rc |= expect_int("aiid-alltalk0", strcmp(id, "16266140"), 0);
    dpmr_convert_air_interface_id(0xFFFFFFU, id, sizeof(id));
    rc |= expect_int("aiid-all-ones", strcmp(id, "16777215"), 0);

    /* The old eight-byte buffer: hex, never a cut-off number or dial string. */
    char narrow[8];
    dpmr_convert_air_interface_id(0xF8339CU, narrow, sizeof(narrow));
    rc |= expect_int("aiid-narrow-hex", strcmp(narrow, "F8339C"), 0);
    dpmr_convert_air_interface_id(1464100U, narrow, sizeof(narrow));
    rc |= expect_int("aiid-narrow-dial-string", strcmp(narrow, "1000000"), 0);
    char tiny[6];
    dpmr_convert_air_interface_id(0xF8339CU, tiny, sizeof(tiny));
    rc |= expect_int("aiid-tiny-number", strcmp(tiny, ""), 0);
    dpmr_convert_air_interface_id(1464100U, tiny, sizeof(tiny));
    rc |= expect_int("aiid-tiny-dial-string", strcmp(tiny, ""), 0);
    return rc;
}

/* Issue #903: an ID half is used only once its own CCH passed CRC-7 (TS 102 658 Table 7.1; the Hamming(12,8) flags
   cannot see a miscorrection), and only from the frame pair that carries that ID (section 11.1.0: FN 0/1 the called
   ID, FN 2/3 the calling one). Neither failed halves, nor one failed half, nor two passing halves from different
   superframe positions change the call (called "1000000", calling "100000*") or start a caller epoch. */
static int
expect_unchecked_pairs_change_nothing(dsd_opts* opts, dsd_state* state, uint64_t caller_epoch) {
    static const struct {
        const char* tag;
        uint32_t fn0, fn1;
        bool crc0, crc1;
    } k_rejected[] = {
        {"calling-both-crc-failed", 2U, 3U, false, false},
        {"calling-first-half-failed", 2U, 3U, false, true},
        {"calling-second-half-failed", 2U, 3U, true, false},
        {"called-second-half-failed", 0U, 1U, true, false},
        {"mixed-pair-0-3", 0U, 3U, true, true},
        {"mixed-pair-2-1", 2U, 1U, true, true},
        {"swapped-pair-1-0", 1U, 0U, true, true},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(k_rejected) / sizeof(k_rejected[0]); i++) {
        char tag[96];
        dsd_call_snapshot call;
        const dpmr_superframe_part part = {.frame_number = {k_rejected[i].fn0, k_rejected[i].fn1},
                                           .id_value = 2928210U,
                                           .crc_ok = {k_rejected[i].crc0, k_rejected[i].crc1},
                                           .hamming_ok = {{true, true}, {true, true}}};
        dpmr_update_superframe_part(opts, state, &part);
        (void)dsd_call_state_get(state, 0U, &call);
        DSD_SNPRINTF(tag, sizeof(tag), "%s keeps the calling id", k_rejected[i].tag);
        rc |= expect_int(tag, strcmp(call.source_text, "100000*"), 0);
        DSD_SNPRINTF(tag, sizeof(tag), "%s keeps the called id", k_rejected[i].tag);
        rc |= expect_int(tag, strcmp(call.target_text, "1000000"), 0);
        DSD_SNPRINTF(tag, sizeof(tag), "%s keeps the caller epoch", k_rejected[i].tag);
        rc |= expect_int(tag, call.epoch == caller_epoch, 1);
    }
    return rc;
}

static int
test_superframe_part_updates_called_and_calling_ids(void) {
    static dsd_opts opts;
    static dsd_state state;
    dpmr_superframe_part part;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    /* Routing, not the gate: this case is about which identity each frame-number pair
       carries, so give it a transmission that has already proved itself. The gate is
       covered by test_unconfirmed_superframe_part_publishes_nothing below. */
    state.dpmr_confirmed = 1;
    const dsd_call_observation begin = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(&state, &begin, DSD_CALL_BOUNDARY_BEGIN);
    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    dsd_call_snapshot call;
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("called-id", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("called-next-part", opts.dPMR_next_part_of_superframe, 2);
    rc |= expect_int("calling-id-preserved-empty", strcmp(call.source_text, ""), 0);

    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 1464110U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("calling-id", strcmp(call.source_text, "100000*"), 0);
    rc |= expect_int("calling-next-part", opts.dPMR_next_part_of_superframe, 1);
    rc |= expect_int("called-id-preserved", strcmp(call.target_text, "1000000"), 0);
    const uint64_t first_caller_epoch = call.epoch;

    rc |= expect_unchecked_pairs_change_nothing(&opts, &state, first_caller_epoch);

    part = (dpmr_superframe_part){.frame_number = {2U, 3U},
                                  .id_value = 2928210U,
                                  .crc_ok = {true, true},
                                  .hamming_ok = {{false, false}, {false, false}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("changed-calling-id", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("changed-calling-preserves-called-id", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("changed-calling-starts-epoch", call.epoch != first_caller_epoch, 1);

    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 10U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {false, false}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("weak-called-id-does-not-overwrite", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("weak-called-next-part", opts.dPMR_next_part_of_superframe, 2);

    part = (dpmr_superframe_part){.frame_number = {1U, 2U},
                                  .id_value = 0U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{false, false}, {false, false}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("unknown-preserves-called", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("unknown-preserves-calling", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("unknown-toggles-next-part", opts.dPMR_next_part_of_superframe, 1);

    opts.dPMR_next_part_of_superframe = 1;
    dpmr_update_superframe_part(&opts, &state, &part);
    rc |= expect_int("unknown-toggles-next-part-back", opts.dPMR_next_part_of_superframe, 2);

    opts.dPMR_next_part_of_superframe = 0;
    dpmr_update_superframe_part(&opts, &state, &part);
    rc |= expect_int("unknown-keeps-zero-part", opts.dPMR_next_part_of_superframe, 0);
    return rc;
}

/* Before #407 this published a talkgroup and a source: the identity gate accepted a
   CCH whose two leading Hamming blocks merely report correctable, which 44% of noise
   superframes do. Without a passing CRC-7 there is nothing here to publish. */
static int
test_unconfirmed_superframe_part_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    dpmr_superframe_part part;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    part = (dpmr_superframe_part){.frame_number = {0U, 1U},
                                  .id_value = 1464100U,
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("unconfirmed-no-call-row", dsd_call_state_get(&state, 0U, &call) > 0, 0);
    /* The superframe bookkeeping still advances: it tracks which half comes next, not
       whether anything decoded. */
    rc |= expect_int("unconfirmed-next-part-still-tracked", opts.dPMR_next_part_of_superframe, 2);
    return rc;
}

/* The printer only prints (issue #575). It shows what the canonical call holds, so it is no place to publish this
   carrier's colour code from: dpmr_publish_call() does that, on the confirmed path. */
static int
test_id_print_does_not_publish_the_color_code(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = -1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state.dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    dpmr_print_ids(&state);

    dsd_call_snapshot call;
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("print-target-id", strcmp(call.target_text, "1000000"), 0);
    rc |= expect_int("print-caller-id", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("print-leaves-color-code", state.dpmr_color_code, -1);
    return rc;
}

/* A confirmed transmission publishes the colour code it decoded, whether or not a caller identity decoded with it
   (issue #575): a target-only call is still a call on this carrier. */
static int
test_confirmed_target_only_call_publishes_the_color_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = -1;
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_confirmed = 1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state.dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    dpmr_print_ids(&state);
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("target-only-call-present", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("target-only-no-caller", call.source_text[0] == '\0', 1);
    rc |= expect_int("target-only-color-code", state.dpmr_color_code, 12);
    /* The carrier's received identity: the colour code of the confirmed transmission. */
    rc |= expect_int("target-only-identity-family", state.rx_id.family, DSD_RXID_FAMILY_DPMR);
    rc |= expect_int("target-only-identity-parts", (int)state.rx_id.valid, (int)DSD_RXID_ACCESS);
    rc |= expect_int("target-only-identity-colour", state.rx_id.access, 12);
    rc |= expect_int("target-only-identity-kind", state.rx_id.access_kind, DSD_ACCESS_CODE_COLOR_CODE);

    /* A later superframe whose channel code did not decode keeps the code the call already published. */
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dpmr_publish_call(&opts, &state);
    rc |= expect_int("undecoded-channel-code-keeps-color", state.dpmr_color_code, 12);
    rc |= expect_int("undecoded-channel-code-keeps-identity", state.rx_id.access, 12);

    /* An unconfirmed transmission names nothing. */
    dsd_rxid_reset(&state);
    state.dpmr_confirmed = 0;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 33U;
    dpmr_publish_call(&opts, &state);
    rc |= expect_int("unconfirmed-no-identity", (int)state.rx_id.valid, 0);
    return rc;
}

/* The colour code is the carrier's access code, never the call's channel: dPMR has no trunk channel number, and the
   API documents `channel` as one. A confirmed call with a decoded colour code keeps channel 0 and reads the code as its
   access code. */
static int
test_dpmr_channel_not_colour_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = -1;
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_confirmed = 1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 12U;
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("cc-call-present", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("cc-call-channel", (int)call.channel, 0);
    uint8_t kind = 0U;
    uint16_t code = 0U;
    rc |= expect_int(
        "cc-access-code-read",
        dsd_access_code_current(&state, call.protocol, call.service_options, call.has_service_metadata, &kind, &code),
        1);
    rc |= expect_int("cc-access-code-kind", kind, DSD_ACCESS_CODE_COLOR_CODE);
    rc |= expect_int("cc-access-code", code, 12);
    return rc;
}

/* An unconfirmed frame publishes nothing, the colour code included, even with a caller identity retained from an
   earlier transmission in the canonical call (issue #575). */
static int
test_unconfirmed_frame_with_retained_caller_publishes_no_color_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = -1;
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_end(&state, 0U, 0.0);
    state.dpmr_confirmed = 0;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 21U;

    char printed[256];
    rc |= capture_print_ids(&state, printed, sizeof(printed));
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("retained-caller-text", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("retained-caller-is-kept", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("unconfirmed-no-color-code", state.dpmr_color_code, -1);
    /* The retained text belongs to the earlier transmission: this unconfirmed frame decoded no identity to show. */
    rc |= expect_text("retained-target-not-printed", printed, "1000000", 0);
    rc |= expect_text("retained-caller-not-printed", printed, "200000*", 0);
    rc |= expect_text("unconfirmed-prints-target-crc-err", printed, "Called=(CRC ERR)", 1);
    rc |= expect_text("unconfirmed-prints-caller-crc-err", printed, "Src=(CRC ERR)", 1);
    return rc;
}

/* A confirmed transmission shows the identities its own frames published, and the channel code read with them. */
static int
test_confirmed_call_prints_its_identities(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_confirmed = 1;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "1000000");
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "200000*");
    (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    state.dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    char printed[256];
    rc |= capture_print_ids(&state, printed, sizeof(printed));
    /* The called ID may be a radio's as well as a group's (issue #903): the line names neither. */
    rc |= expect_text("confirmed-prints-target", printed, "Called=1000000", 1);
    rc |= expect_text("confirmed-prints-no-tg-label", printed, "TG=", 0);
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
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_capture();
        dsd_call_observation observation = {
            .protocol = DSD_SYNC_DSTAR_VOICE_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_VOICE,
        };
        DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "CQCQCQ");
        DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", "N0CALL");
        (void)dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        if (!still_open) {
            (void)dsd_call_state_end(&state, 0U, 0.0);
        }
        state.synctype = DSD_SYNC_DPMR_FS2_POS;
        state.dpmr_color_code = -1;
        state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
        g_cch_noise = 1;

        dsd_test_capture_stderr cap;
        char printed[512];
        if (dsd_test_capture_stderr_begin(&cap, "dpmr_noise_frame") != 0) {
            DSD_FPRINTF(stderr, "capture stderr begin failed\n");
            return 1;
        }
        const int verdict = processdPMRvoice(&opts, &state);
        if (dsd_test_capture_stderr_end(&cap) != 0
            || dsd_test_capture_stderr_read(&cap, printed, sizeof(printed)) != 0) {
            DSD_FPRINTF(stderr, "capture stderr failed\n");
            return 1;
        }

        rc |= expect_int("noise-verdict", verdict, 0);
        rc |= expect_int("noise-crc-0", (int)state.dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
        rc |= expect_int("noise-crc-1", (int)state.dPMRVoiceFS2Frame.CCHDataCrcOk[1], 0);
        rc |= expect_int("noise-unconfirmed", state.dpmr_confirmed, 0);
        rc |= expect_int("noise-no-voice", (int)g_mbe_calls, 0);
        rc |= expect_text("noise-no-foreign-target", printed, "CQCQCQ", 0);
        rc |= expect_text("noise-no-foreign-caller", printed, "N0CALL", 0);
        rc |= expect_text("noise-target-crc-err", printed, "Called=(CRC ERR)", 1);
        rc |= expect_text("noise-caller-crc-err", printed, "Src=(CRC ERR)", 1);

        /* The other protocol's call is left as it was. */
        dsd_call_snapshot call;
        rc |= expect_int("noise-other-call-kept", dsd_call_state_get(&state, 0U, &call) > 0, 1);
        rc |= expect_int("noise-other-call-protocol", call.protocol, DSD_SYNC_DSTAR_VOICE_POS);
        rc |= expect_int("noise-other-call-phase", (int)call.phase,
                         still_open ? (int)DSD_CALL_PHASE_ACTIVE : (int)DSD_CALL_PHASE_ENDED);
    }
    return rc;
}

static int
test_id_print_side_effects_suppress_invalid_ids(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = 7;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 4U;

    dpmr_print_ids(&state);
    rc |= expect_int("invalid-called-keeps-color", state.dpmr_color_code, 7);
    return rc;
}

static int
test_process_dpmr_voice_zero_stream_updates_cch_and_dispatches_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();

    processdPMRvoice(&opts, &state);

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
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.CCHDataHammingOk[i], 1);
        DSD_SNPRINTF(tag, sizeof(tag), "process-crc-ok-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.CCHDataCrcOk[i], 1);
        DSD_SNPRINTF(tag, sizeof(tag), "process-frame-number-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.FrameNumbering[i], (int)i);
        DSD_SNPRINTF(tag, sizeof(tag), "process-comm-mode-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.CommunicationMode[i], 0);
        DSD_SNPRINTF(tag, sizeof(tag), "process-version-%u", (unsigned)i);
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.Version[i], 0);
    }

    dsd_call_snapshot call;
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("process-called-id", strcmp(call.target_text, "0000000"), 0);
    rc |= expect_int("process-next-part", opts.dPMR_next_part_of_superframe, 2);
    rc |= expect_int("process-invalid-color-code", (int)state.dPMRVoiceFS2Frame.ColorCode[0], -1);
    rc |= expect_int("process-no-color-side-effect", state.dpmr_color_code, 0);
    rc |= expect_int("process-final-synctype", state.synctype, DSD_SYNC_DPMR_FS1_POS);
    return rc;
}

/* Issue #575: the carrier boundary moved the carrier count while the two frames were read (a replay read adopting a
   recorded retune). Nothing decodes before both are in, so the first frame's CCH and the channel code, read whole before
   the move, would publish the carrier left's call and colour code after the boundary ended that call and forgot the
   code: the pair is dropped, as on a sync loss. */
static int
test_process_dpmr_voice_split_by_a_carrier_boundary_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    state.dpmr_color_code = -1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_boundary_at = 300;

    const int verdict = processdPMRvoice(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("split-moved-carrier-count", (int)state.carrier_seq, 1);
    rc |= expect_int("split-dibit-count", g_dibit_calls, 372);
    rc |= expect_int("split-verdict", verdict, 0);
    rc |= expect_int("split-mbe-calls", (int)g_mbe_calls, 0);
    rc |= expect_int("split-no-call", dsd_call_state_get(&state, 0U, &call) > 0, 0);
    rc |= expect_int("split-no-crc", (int)state.dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
    rc |= expect_int("split-no-confirmation", state.dpmr_confirmed, 0);
    rc |= expect_int("split-no-color-code", state.dpmr_color_code, -1);
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
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    state.dpmr_color_code = -1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dsd_signal_search_evidence_set(capture_search_evidence);

    /* A pair split by a carrier boundary decodes nothing. */
    reset_capture();
    g_boundary_at = 300;
    (void)processdPMRvoice(&opts, &state);
    rc |= expect_int("search-split-reports-nothing", g_search_evidence.calls, 0);

    reset_capture();
    g_channel_code = 0x55FF7FU; /* channel code 7 */
    (void)processdPMRvoice(&opts, &state);
    dsd_signal_search_evidence_set(NULL);

    rc |= expect_int("search-frame-color-code-decoded", (int)state.dPMRVoiceFS2Frame.ColorCode[0], 7);
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
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    state.dpmr_color_code = -1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    reset_capture();
    g_interrupt_at = 300;

    const int verdict = processdPMRvoice(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("cut-interrupted", state.input_interrupted, 1);
    rc |= expect_int("cut-no-carrier-move", (int)state.carrier_seq, 0);
    rc |= expect_int("cut-dibit-count", g_dibit_calls, 372);
    rc |= expect_int("cut-verdict", verdict, 0);
    rc |= expect_int("cut-mbe-calls", (int)g_mbe_calls, 0);
    rc |= expect_int("cut-no-call", dsd_call_state_get(&state, 0U, &call) > 0, 0);
    rc |= expect_int("cut-no-crc", (int)state.dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
    rc |= expect_int("cut-no-confirmation", state.dpmr_confirmed, 0);
    rc |= expect_int("cut-no-color-code", state.dpmr_color_code, -1);
    return rc;
}

/* Two FS2 frames each with one CCH half passing its CRC-7 (one chance in 128 apiece) confirm a transmission only when
   the second runs on from the first, its FS2 one frame (384 symbols) later. Under Auto, noise hands 2400/4 hundreds of
   FS2 matches an hour between other protocols' syncs, and two weak frames seconds apart confirmed a transmission of
   noise, published a call and decoded its voice. */
static int
test_process_dpmr_voice_weak_frames_confirm_only_when_they_run(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;
    dsd_call_snapshot call;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.dpmr_color_code = -1;
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);

    reset_capture();
    g_fail_half = 1;
    state.symbolcnt = 10000U;
    rc |= expect_int("weak-first-verdict", processdPMRvoice(&opts, &state), 1);
    rc |= expect_int("weak-first-unconfirmed", state.dpmr_confirmed, 0);

    /* Seconds later, other protocols' syncs between: no run. */
    reset_capture();
    g_fail_half = 1;
    state.symbolcnt = 10000U + 5000U;
    rc |= expect_int("weak-apart-verdict", processdPMRvoice(&opts, &state), 1);
    rc |= expect_int("weak-apart-unconfirmed", state.dpmr_confirmed, 0);
    rc |= expect_int("weak-apart-plays-nothing", (int)g_mbe_calls, 0);
    rc |= expect_int("weak-apart-no-call", dsd_call_state_get(&state, 0U, &call) > 0, 0);

    /* The next frame runs on from it, a symbol late: a transmission, which publishes and plays. */
    reset_capture();
    g_fail_half = 1;
    state.symbolcnt = 10000U + 5000U + 385U;
    rc |= expect_int("weak-run-verdict", processdPMRvoice(&opts, &state), 1);
    rc |= expect_int("weak-run-confirmed", state.dpmr_confirmed, 1);
    rc |= expect_int("weak-run-plays", (int)g_mbe_calls, 8);
    rc |= expect_int("weak-run-call", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_first_group_scrambler_bridge_mutes_without_key();
    rc |= test_second_group_scrambler_bridge_unmutes_with_key();
    rc |= test_scrambler_secret_is_not_published_as_key_id();
    rc |= test_scrambling_is_per_half_and_crc_verified();
    rc |= test_enc_indication_is_the_frames_own();
    rc |= test_failed_cch_frame_number_does_not_reset_the_scrambler();
    rc |= test_failed_cch_mode_does_not_switch_voice();
    rc |= test_failed_cch_changes_no_service_metadata();
    rc |= test_deinterleave_transposes_6x12_blocks();
    rc |= test_crc7_and_air_interface_id_helpers();
    rc |= test_air_interface_ids_above_the_dial_space_render_as_numbers();
    rc |= test_process_dpmr_voice_weak_frames_confirm_only_when_they_run();
    rc |= test_superframe_part_updates_called_and_calling_ids();
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

    if (rc == 0) {
        printf("DPMR_VOICE_BRIDGE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
