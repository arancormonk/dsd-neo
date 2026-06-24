// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused checks for the active dPMR voice bridge into the NXDN scrambler path.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static size_t g_mbe_calls;
static int g_ms_calls;
static int g_fm_calls;
static int g_mbe_synctype[8];
static int g_mbe_cipher[8];
static int g_mbe_enc[8];
static unsigned long long g_mbe_mi[8];
static int g_dibit_calls;

static void
reset_capture(void) {
    g_mbe_calls = 0;
    g_ms_calls = 0;
    g_fm_calls = 0;
    DSD_MEMSET(g_mbe_synctype, 0, sizeof(g_mbe_synctype));
    DSD_MEMSET(g_mbe_cipher, 0, sizeof(g_mbe_cipher));
    DSD_MEMSET(g_mbe_enc, 0, sizeof(g_mbe_enc));
    DSD_MEMSET(g_mbe_mi, 0, sizeof(g_mbe_mi));
    g_dibit_calls = 0;
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_dibit_calls++;
    return 0;
}

uint64_t
ConvertBitIntoBytes(const uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t value = 0ULL;
    if (BufferIn == NULL) {
        return 0ULL;
    }
    for (uint32_t i = 0U; i < BitLength; i++) {
        value = (value << 1U) | (uint64_t)(BufferIn[i] & 1U);
    }
    return value;
}

bool
Hamming_12_8_decode(unsigned char* rxBits, unsigned char* decodedBits, int nbCodewords) {
    (void)rxBits;
    if (decodedBits != NULL && nbCodewords > 0) {
        DSD_MEMSET(decodedBits, 0, (size_t)nbCodewords * 8U);
    }
    return true;
}

void dsd_test_dpmr_play_voice_frames(dsd_opts* opts, dsd_state* state,
                                     char ambe_fr[NB_OF_DPMR_VOICE_FRAME_TO_DECODE * 4][4][24]);
void dsd_test_dpmr_deinterleave_6x12(const uint8_t* input, uint8_t* output);
uint8_t dsd_test_dpmr_crc7(const uint8_t* input, uint32_t bit_length);
void dsd_test_dpmr_convert_air_interface_id(uint32_t ai_id, char id[8]);
uint8_t dsd_test_dpmr_extract_cch_crc(const uint8_t cch_bits[48]);
void dsd_test_dpmr_update_superframe_part(dsd_opts* opts, dsd_state* state, uint32_t frame0, uint32_t frame1,
                                          uint32_t id_value, bool crc0_ok, bool crc1_ok, bool hamming0_ok,
                                          bool hamming1_ok);
void dsd_test_dpmr_print_ids(dsd_state* state, const char called_id[8], const char calling_id[8]);
void processdPMRvoice(dsd_opts* opts, dsd_state* state);

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    if (g_mbe_calls < (sizeof(g_mbe_synctype) / sizeof(g_mbe_synctype[0]))) {
        g_mbe_synctype[g_mbe_calls] = state->synctype;
        g_mbe_cipher[g_mbe_calls] = (int)state->nxdn_cipher_type;
        g_mbe_enc[g_mbe_calls] = state->dmr_encL;
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

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_fm_calls++;
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
    state.dPMRVoiceFS2Frame.FrameNumbering[1] = 2;
    state.dPMRVoiceFS2Frame.CommunicationMode[1] = 2;

    dsd_test_dpmr_play_voice_frames(&opts, &state, ambe_fr);

    int rc = 0;
    rc |= expect_int("first-group-mbe-calls", (int)g_mbe_calls, 4);
    rc |= expect_int("first-group-ms-calls", g_ms_calls, 4);
    rc |= expect_int("first-group-fm-calls", g_fm_calls, 0);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("first-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("first-group-cipher", g_mbe_cipher[i], 1);
        rc |= expect_int("first-group-muted", g_mbe_enc[i], 1);
        rc |= expect_u64("first-group-mi-reset", g_mbe_mi[i], 0ULL);
    }
    rc |= expect_int("first-group-synctype-restored", state.synctype, DSD_SYNC_DPMR_FS2_POS);
    rc |= expect_int("first-group-cipher-reset", (int)state.nxdn_cipher_type, 0);
    rc |= expect_int("first-group-final-muted", state.dmr_encL, 1);
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

    dsd_test_dpmr_play_voice_frames(&opts, &state, ambe_fr);

    int rc = 0;
    rc |= expect_int("second-group-mbe-calls", (int)g_mbe_calls, 4);
    rc |= expect_int("second-group-ms-calls", g_ms_calls, 0);
    rc |= expect_int("second-group-fm-calls", g_fm_calls, 4);
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("second-group-synctype", g_mbe_synctype[i], DSD_SYNC_NXDN_POS);
        rc |= expect_int("second-group-cipher", g_mbe_cipher[i], 1);
        rc |= expect_int("second-group-unmuted", g_mbe_enc[i], 0);
        rc |= expect_u64("second-group-mi-reset", g_mbe_mi[i], 0ULL);
    }
    rc |= expect_int("second-group-synctype-restored", state.synctype, DSD_SYNC_DPMR_FS3_POS);
    rc |= expect_int("second-group-cipher-reset", (int)state.nxdn_cipher_type, 0);
    rc |= expect_int("second-group-final-unmuted", state.dmr_encL, 0);
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

    dsd_test_dpmr_deinterleave_6x12(input, output);

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

    rc |= expect_int("crc7-empty", dsd_test_dpmr_crc7(crc_bits, 0U), 0x00);
    rc |= expect_int("crc7-pattern", dsd_test_dpmr_crc7(crc_bits, 12U), 0x24);
    cch_bits[41] = 1U;
    cch_bits[43] = 1U;
    cch_bits[46] = 1U;
    rc |= expect_int("cch-crc-extract", dsd_test_dpmr_extract_cch_crc(cch_bits), 0x52);

    dsd_test_dpmr_convert_air_interface_id(0U, id);
    rc |= expect_int("aiid-zero", strcmp(id, "0000000"), 0);
    dsd_test_dpmr_convert_air_interface_id(10U, id);
    rc |= expect_int("aiid-star-low", strcmp(id, "000000*"), 0);
    dsd_test_dpmr_convert_air_interface_id(1464100U, id);
    rc |= expect_int("aiid-first-digit", strcmp(id, "1000000"), 0);
    dsd_test_dpmr_convert_air_interface_id(1464110U, id);
    rc |= expect_int("aiid-first-digit-with-star", strcmp(id, "100000*"), 0);
    return rc;
}

static int
test_superframe_part_updates_called_and_calling_ids(void) {
    static dsd_opts opts;
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    dsd_test_dpmr_update_superframe_part(&opts, &state, 0U, 1U, 1464100U, true, true, false, false);
    rc |= expect_int("called-id", strcmp((const char*)state.dPMRVoiceFS2Frame.CalledID, "1000000"), 0);
    rc |= expect_int("called-id-ok", (int)state.dPMRVoiceFS2Frame.CalledIDOk, 1);
    rc |= expect_int("called-next-part", opts.dPMR_next_part_of_superframe, 2);
    rc |= expect_int("calling-id-preserved-empty", strcmp((const char*)state.dPMRVoiceFS2Frame.CallingID, ""), 0);

    dsd_test_dpmr_update_superframe_part(&opts, &state, 2U, 3U, 1464110U, false, false, true, true);
    rc |= expect_int("calling-id", strcmp((const char*)state.dPMRVoiceFS2Frame.CallingID, "100000*"), 0);
    rc |= expect_int("calling-id-ok-from-hamming", (int)state.dPMRVoiceFS2Frame.CallingIDOk, 1);
    rc |= expect_int("calling-next-part", opts.dPMR_next_part_of_superframe, 1);
    rc |= expect_int("called-id-preserved", strcmp((const char*)state.dPMRVoiceFS2Frame.CalledID, "1000000"), 0);

    dsd_test_dpmr_update_superframe_part(&opts, &state, 0U, 1U, 10U, false, false, true, false);
    rc |= expect_int("weak-called-id", strcmp((const char*)state.dPMRVoiceFS2Frame.CalledID, "000000*"), 0);
    rc |= expect_int("weak-called-id-not-ok", (int)state.dPMRVoiceFS2Frame.CalledIDOk, 0);
    rc |= expect_int("weak-called-next-part", opts.dPMR_next_part_of_superframe, 2);

    state.dPMRVoiceFS2Frame.CalledIDOk = 1U;
    state.dPMRVoiceFS2Frame.CallingIDOk = 1U;
    dsd_test_dpmr_update_superframe_part(&opts, &state, 1U, 2U, 0U, false, false, false, false);
    rc |= expect_int("unknown-clears-called-ok", (int)state.dPMRVoiceFS2Frame.CalledIDOk, 0);
    rc |= expect_int("unknown-clears-calling-ok", (int)state.dPMRVoiceFS2Frame.CallingIDOk, 0);
    rc |= expect_int("unknown-toggles-next-part", opts.dPMR_next_part_of_superframe, 1);

    opts.dPMR_next_part_of_superframe = 1;
    dsd_test_dpmr_update_superframe_part(&opts, &state, 1U, 2U, 0U, false, false, false, false);
    rc |= expect_int("unknown-toggles-next-part-back", opts.dPMR_next_part_of_superframe, 2);

    opts.dPMR_next_part_of_superframe = 0;
    dsd_test_dpmr_update_superframe_part(&opts, &state, 1U, 2U, 0U, false, false, false, false);
    rc |= expect_int("unknown-keeps-zero-part", opts.dPMR_next_part_of_superframe, 0);
    return rc;
}

static int
test_id_print_side_effects_track_valid_target_caller_and_color(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.dPMRVoiceFS2Frame.CalledIDOk = 1U;
    state.dPMRVoiceFS2Frame.CallingIDOk = 1U;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 12U;

    dsd_test_dpmr_print_ids(&state, "1000000", "200000*");

    rc |= expect_int("print-target-id", strcmp(state.dpmr_target_id, "1000000"), 0);
    rc |= expect_int("print-caller-id", strcmp(state.dpmr_caller_id, "200000*"), 0);
    rc |= expect_int("print-color-code", state.dpmr_color_code, 12);
    return rc;
}

static int
test_id_print_side_effects_suppress_invalid_ids(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_SNPRINTF(state.dpmr_target_id, sizeof(state.dpmr_target_id), "%s", "prior-tg");
    DSD_SNPRINTF(state.dpmr_caller_id, sizeof(state.dpmr_caller_id), "%s", "prior-src");
    state.dpmr_color_code = 7;
    state.dPMRVoiceFS2Frame.CalledIDOk = 0U;
    state.dPMRVoiceFS2Frame.CallingIDOk = 1U;
    state.dPMRVoiceFS2Frame.ColorCode[0] = 4U;

    dsd_test_dpmr_print_ids(&state, "3000000", "4000000");

    rc |= expect_int("invalid-called-keeps-target", strcmp(state.dpmr_target_id, "prior-tg"), 0);
    rc |= expect_int("invalid-called-keeps-caller", strcmp(state.dpmr_caller_id, "prior-src"), 0);
    rc |= expect_int("invalid-called-keeps-color", state.dpmr_color_code, 7);

    state.dPMRVoiceFS2Frame.CalledIDOk = 1U;
    state.dPMRVoiceFS2Frame.CallingIDOk = 0U;
    dsd_test_dpmr_print_ids(&state, "5000000", "6000000");
    rc |= expect_int("invalid-calling-updates-target", strcmp(state.dpmr_target_id, "5000000"), 0);
    rc |= expect_int("invalid-calling-keeps-caller", strcmp(state.dpmr_caller_id, "prior-src"), 0);
    rc |= expect_int("invalid-calling-keeps-color", state.dpmr_color_code, 7);
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
    rc |= expect_int("process-fm-calls", g_fm_calls, 0);
    for (int i = 0; i < 8; i++) {
        rc |= expect_int("process-synctype", g_mbe_synctype[i], DSD_SYNC_DPMR_FS1_POS);
        rc |= expect_int("process-cipher", g_mbe_cipher[i], 0);
        rc |= expect_int("process-clear-audio", g_mbe_enc[i], 0);
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

    rc |= expect_int("process-called-id", strcmp((const char*)state.dPMRVoiceFS2Frame.CalledID, "0000000"), 0);
    rc |= expect_int("process-called-id-ok", (int)state.dPMRVoiceFS2Frame.CalledIDOk, 1);
    rc |= expect_int("process-next-part", opts.dPMR_next_part_of_superframe, 2);
    rc |= expect_int("process-invalid-color-code", (int)state.dPMRVoiceFS2Frame.ColorCode[0], -1);
    rc |= expect_int("process-no-color-side-effect", state.dpmr_color_code, 0);
    rc |= expect_int("process-final-synctype", state.synctype, DSD_SYNC_DPMR_FS1_POS);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_first_group_scrambler_bridge_mutes_without_key();
    rc |= test_second_group_scrambler_bridge_unmutes_with_key();
    rc |= test_deinterleave_transposes_6x12_blocks();
    rc |= test_crc7_and_air_interface_id_helpers();
    rc |= test_superframe_part_updates_called_and_calling_ids();
    rc |= test_id_print_side_effects_track_valid_target_caller_and_color();
    rc |= test_id_print_side_effects_suppress_invalid_ids();
    rc |= test_process_dpmr_voice_zero_stream_updates_cch_and_dispatches_voice();

    if (rc == 0) {
        printf("DPMR_VOICE_BRIDGE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
