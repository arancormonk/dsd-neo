// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for the active dPMR voice bridge into the NXDN scrambler path.
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
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
#include <string.h>
#include "dpmr_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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

enum { DPMR_TEST_FIRST_CC_DIBIT = 36 + 144, DPMR_TEST_CC_DIBITS = 12 };

/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for every symbol after it; -1 for none. */
static int g_interrupt_at = -1;

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

bool
Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    (void)rxBits;
    if (decodedBits != NULL && nbCodewords > 0) {
        DSD_MEMSET(decodedBits, 0, (size_t)nbCodewords * 8U);
    }
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
        g_mbe_mi[g_mbe_calls] = state->payload_miN;
    }
    g_mbe_calls++;
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
test_first_group_scrambler_bridge_mutes_without_key(void) {
    static dsd_opts opts;
    static dsd_state state;
    char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
    reset_capture();

    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.payload_miN = 0x7777ULL;
    state.dPMRVoiceFS2Frame.FrameNumbering[0] = 0;
    state.dPMRVoiceFS2Frame.CommunicationMode[0] = 0;
    state.dPMRVoiceFS2Frame.Version[0] = 3;
    state.dPMRVoiceFS2Frame.CCHDataCrcOk[0] = 1;
    state.dPMRVoiceFS2Frame.FrameNumbering[1] = 2;
    state.dPMRVoiceFS2Frame.CommunicationMode[1] = 2;

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
    state.payload_miN = 0x8888ULL;
    state.dPMRVoiceFS2Frame.FrameNumbering[0] = 1;
    state.dPMRVoiceFS2Frame.CommunicationMode[0] = 2;
    state.dPMRVoiceFS2Frame.FrameNumbering[1] = 0;
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
                                  .crc_ok = {false, false},
                                  .hamming_ok = {{true, true}, {true, true}}};
    dpmr_update_superframe_part(&opts, &state, &part);
    (void)dsd_call_state_get(&state, 0U, &call);
    rc |= expect_int("calling-id", strcmp(call.source_text, "100000*"), 0);
    rc |= expect_int("calling-next-part", opts.dPMR_next_part_of_superframe, 1);
    rc |= expect_int("called-id-preserved", strcmp(call.target_text, "1000000"), 0);
    const uint64_t first_caller_epoch = call.epoch;

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

/* Before #407 this published a talkgroup and a source: dpmr_ids_are_strong() accepts a
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

/* The printer only prints (issue #575). It reads the canonical call, whose caller text outlives the transmission
   that decoded it, and never checks confirmation, so it is no place to publish this carrier's colour code from:
   dpmr_publish_call() does that, on the confirmed path. */
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

    /* A later superframe whose channel code did not decode keeps the code the call already published. */
    state.dPMRVoiceFS2Frame.ColorCode[0] = (unsigned int)(-1);
    dpmr_publish_call(&opts, &state);
    rc |= expect_int("undecoded-channel-code-keeps-color", state.dpmr_color_code, 12);
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

    dpmr_print_ids(&state);
    dpmr_publish_call(&opts, &state);

    dsd_call_snapshot call;
    rc |= expect_int("retained-caller-text", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    rc |= expect_int("retained-caller-is-printed", strcmp(call.source_text, "200000*"), 0);
    rc |= expect_int("unconfirmed-no-color-code", state.dpmr_color_code, -1);
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
        rc |= expect_int(tag, (int)state.dPMRVoiceFS2Frame.FrameNumbering[i], 0);
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
capture_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                        const char* access) {
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

int
main(void) {
    int rc = 0;
    rc |= test_first_group_scrambler_bridge_mutes_without_key();
    rc |= test_second_group_scrambler_bridge_unmutes_with_key();
    rc |= test_scrambler_secret_is_not_published_as_key_id();
    rc |= test_scrambling_is_per_half_and_crc_verified();
    rc |= test_deinterleave_transposes_6x12_blocks();
    rc |= test_crc7_and_air_interface_id_helpers();
    rc |= test_superframe_part_updates_called_and_calling_ids();
    rc |= test_unconfirmed_superframe_part_publishes_nothing();
    rc |= test_id_print_does_not_publish_the_color_code();
    rc |= test_confirmed_target_only_call_publishes_the_color_code();
    rc |= test_unconfirmed_frame_with_retained_caller_publishes_no_color_code();
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
