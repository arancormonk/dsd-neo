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
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-variable-declarations"
#endif

static size_t g_mbe_calls;
static int g_ms_calls;
static int g_fm_calls;
static int g_mbe_synctype[8];
static int g_mbe_cipher[8];
static int g_mbe_enc[8];
static unsigned long long g_mbe_mi[8];

static void
reset_capture(void) {
    g_mbe_calls = 0;
    g_ms_calls = 0;
    g_fm_calls = 0;
    DSD_MEMSET(g_mbe_synctype, 0, sizeof(g_mbe_synctype));
    DSD_MEMSET(g_mbe_cipher, 0, sizeof(g_mbe_cipher));
    DSD_MEMSET(g_mbe_enc, 0, sizeof(g_mbe_enc));
    DSD_MEMSET(g_mbe_mi, 0, sizeof(g_mbe_mi));
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
    dsd_opts opts;
    dsd_state state;
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
    dsd_opts opts;
    dsd_state state;
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

int
main(void) {
    int rc = 0;
    rc |= test_first_group_scrambler_bridge_mutes_without_key();
    rc |= test_second_group_scrambler_bridge_unmutes_with_key();

    if (rc == 0) {
        printf("DPMR_VOICE_BRIDGE: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
