// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>

#include <stdint.h>
#include <stdio.h>

static int g_hard_calls;
static int g_soft_calls;
static int g_play_ms_calls;
static int g_play_fm_calls;
static char g_last_hard[4][24];
static dsd_vocoder_soft_bit g_last_soft[4][24];
static dsd_opts g_opts;
static dsd_state g_state;

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float(const char* label, float got, float want) {
    float delta = got - want;
    if (delta < 0.0f) {
        delta = -delta;
    }
    if (delta > 1e-6f) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", label, got, want);
        return 1;
    }
    return 0;
}

static void
reset_spies(void) {
    g_hard_calls = 0;
    g_soft_calls = 0;
    g_play_ms_calls = 0;
    g_play_fm_calls = 0;
    DSD_MEMSET(g_last_hard, 0, sizeof(g_last_hard));
    DSD_MEMSET(g_last_soft, 0, sizeof(g_last_soft));
}

static void
reset_decoder_objects(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
}

static void
fill_dibits(uint8_t dbuf[182], uint8_t reliab[182]) {
    for (int i = 0; i < 182; i++) {
        dbuf[i] = (uint8_t)(((i & 1) << 1) | ((i >> 1) & 1));
        reliab[i] = (uint8_t)(200 - i);
    }
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)imbe_fr;
    (void)imbe7100_fr;
    g_hard_calls++;
    DSD_MEMCPY(g_last_hard, ambe_fr, sizeof(g_last_hard));
    for (int i = 0; i < 160; i++) {
        state->audio_out_temp_buf[i] = (float)(g_hard_calls * 1000 + i);
    }
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)opts;
    (void)imbe_fr;
    (void)imbe7100_fr;
    g_soft_calls++;
    DSD_MEMCPY(g_last_soft, ambe_fr, sizeof(g_last_soft));
    for (int i = 0; i < 160; i++) {
        state->audio_out_temp_buf[i] = (float)(g_soft_calls * 2000 + i);
    }
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_ms_calls++;
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_fm_calls++;
}

static int
test_hard_voice_modes(void) {
    int err = 0;
    uint8_t dbuf[182];
    uint8_t reliab[182];

    reset_decoder_objects();
    fill_dibits(dbuf, reliab);

    reset_spies();
    nxdn_voice(&g_opts, &g_state, 1, dbuf, NULL);
    err |= expect_int("voice 1 hard frames", g_hard_calls, 2);
    err |= expect_int("voice 1 short playback", g_play_ms_calls, 2);
    err |= expect_int("voice 1 float playback", g_play_fm_calls, 0);
    err |= expect_int("voice 1 final high bit", g_last_hard[0][23], (dbuf[74] >> 1) & 1);
    err |= expect_int("voice 1 final low bit", g_last_hard[0][5], dbuf[74] & 1);
    err |= expect_float("voice 1 copies final audio", g_state.f_l[7], 2007.0f);

    reset_spies();
    reset_decoder_objects();
    nxdn_voice(&g_opts, &g_state, 2, dbuf, NULL);
    err |= expect_int("voice 2 hard frames", g_hard_calls, 2);
    err |= expect_int("voice 2 starts at second half high bit", g_last_hard[0][23], (dbuf[146] >> 1) & 1);
    err |= expect_int("voice 2 starts at second half low bit", g_last_hard[0][5], dbuf[146] & 1);

    reset_spies();
    reset_decoder_objects();
    g_opts.floating_point = 1;
    nxdn_voice(&g_opts, &g_state, 3, dbuf, NULL);
    err |= expect_int("voice 3 hard frames", g_hard_calls, 4);
    err |= expect_int("voice 3 short playback", g_play_ms_calls, 0);
    err |= expect_int("voice 3 float playback", g_play_fm_calls, 4);
    err |= expect_float("voice 3 copies final audio", g_state.f_l[159], 4159.0f);

    reset_spies();
    nxdn_voice(&g_opts, &g_state, 0, dbuf, NULL);
    err |= expect_int("voice 0 does not decode", g_hard_calls, 0);
    err |= expect_int("voice 0 does not play", g_play_fm_calls, 0);

    return err;
}

static int
test_soft_reliability_mapping(void) {
    int err = 0;
    uint8_t dbuf[182];
    uint8_t reliab[182];

    reset_decoder_objects();
    fill_dibits(dbuf, reliab);

    reset_spies();
    nxdn_voice(&g_opts, &g_state, 1, dbuf, reliab);

    err |= expect_int("soft voice calls", g_soft_calls, 2);
    err |= expect_int("soft path bypasses hard decoder", g_hard_calls, 0);
    err |= expect_int("soft high bit", g_last_soft[0][23].bit, (dbuf[74] >> 1) & 1);
    err |= expect_int("soft high reliability", g_last_soft[0][23].reliability, reliab[74]);
    err |= expect_int("soft low bit", g_last_soft[0][5].bit, dbuf[74] & 1);
    err |= expect_int("soft low reliability", g_last_soft[0][5].reliability, reliab[74]);
    err |= expect_float("soft copies final audio", g_state.f_l[3], 4003.0f);

    return err;
}

int
main(void) {
    int err = 0;

    err |= test_hard_voice_modes();
    err |= test_soft_reliability_mapping();

    if (err == 0) {
        DSD_FPRINTF(stdout, "NXDN_VOICE_MAPPING: OK\n");
    }
    return err;
}
