// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * X2-TDMA's symbol rate (discussion #717). Motorola's X2-TDMA reuses the P25 Phase 1 modulation and its 9600 bit/s:
 * 4800 symbols/s over four levels. dsd-neo hunted it, and matched its sync words only, on P25 Phase 2's 6000/4
 * profile, so neither -fx nor Auto could sync a real X2-TDMA signal. These cases synthesize X2-TDMA bursts (144 symbols:
 * 66 of payload, the 24-symbol BS sync, 54 of payload; a data burst every sixth, voice otherwise), write them as 48 kHz
 * discriminator audio, and run the real WAV reader, symbol slicer and frame sync over them: at 4800 symbols/s -fx and
 * Auto sync X2-TDMA; at 6000 symbols/s neither finds an X2-TDMA sync.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/exitflag.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_dsp_stubs.h"
#include "frame_sync_state_buffers.h"
#include "test_support.h"

enum {
    X2_RATE_HZ = 48000,
    X2_BURST_SYMBOLS = 144,
    X2_SYNC_OFFSET = 66, /* CACH 12 + the first 54 payload symbols */
    X2_BURSTS = 400,
    X2_HANDLER_SYMBOLS = 100, /* what a burst's decoder takes after its sync, roughly */
};

/* A deterministic payload: four levels from a 32-bit LCG. */
static uint32_t g_lcg = 1U;

static int
x2_payload_level(void) {
    g_lcg = g_lcg * 1664525U + 1013904223U;
    static const int levels[4] = {-3, -1, 1, 3};
    return levels[(g_lcg >> 30) & 3U];
}

/* The DSD sync convention: '1' is the positive outer level, '3' the negative one. */
static int
x2_sync_level(char c) {
    return c == '1' ? 3 : -3;
}

/* Write X2_BURSTS bursts at @p symbol_rate as 16-bit 48 kHz audio; returns the symbol count, or -1. */
static long
write_x2_wav(const char* path, int symbol_rate) {
    const int sps = X2_RATE_HZ / symbol_rate;
    const long symbols = (long)X2_BURSTS * X2_BURST_SYMBOLS;
    short* samples = (short*)calloc((size_t)(symbols * sps), sizeof(short));
    if (!samples) {
        return -1;
    }
    g_lcg = 1U;
    long n = 0;
    for (int burst = 0; burst < X2_BURSTS; burst++) {
        const char* sync = (burst % 6) == 0 ? X2TDMA_BS_DATA_SYNC : X2TDMA_BS_VOICE_SYNC;
        for (int i = 0; i < X2_BURST_SYMBOLS; i++) {
            const int in_sync = i >= X2_SYNC_OFFSET && i < X2_SYNC_OFFSET + 24;
            const int level = in_sync ? x2_sync_level(sync[i - X2_SYNC_OFFSET]) : x2_payload_level();
            for (int k = 0; k < sps; k++) {
                samples[n++] = (short)(level * 3000);
            }
        }
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = X2_RATE_HZ;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(path, SFM_WRITE, &info);
    long rc = -1;
    if (wav) {
        rc = (sf_write_short(wav, samples, n) == n) ? symbols : -1;
        sf_close(wav);
    }
    free(samples);
    return rc;
}

/* X2-TDMA syncs the real reader and frame sync find in @p path under decode preset @p mode. */
static int
count_x2_syncs(const char* path, dsdneoUserDecodeMode mode) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    assert(init_state_buffers(&state));
    assert(dsd_apply_decode_mode_preset(mode, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) == 0);
    opts.audio_in_file_info = (SF_INFO*)calloc(1, sizeof(*opts.audio_in_file_info));
    assert(opts.audio_in_file_info != NULL);
    opts.audio_in_file = sf_open(path, SFM_READ, opts.audio_in_file_info);
    assert(opts.audio_in_file != NULL);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = X2_RATE_HZ;
    opts.input_volume_multiplier = 1;
    opts.msize = 1;
    opts.ssize = 128;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", path);
    const dsd_decode_mode_profile profile = dsd_decode_mode_profile_for(mode);
    state.sps_hunt_idx = profile.sps_profile_index;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, profile.symbol_rate_hz, X2_RATE_HZ);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.p25_p2_active_slot = -1;
    state.min = -9000.0f;
    state.max = 9000.0f;
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    state.jitter = -1;
    exitflag = 0;
    dsd_frame_sync_reset_mod_state();

    /* The reader asks for a shutdown at the file's end. */
    const int shutdowns = g_frame_sync_dsp_stub_shutdown_requests;
    int x2_syncs = 0;
    while (!exitflag && g_frame_sync_dsp_stub_shutdown_requests == shutdowns && !state.input_interrupted) {
        const int sync = getFrameSync(&opts, &state);
        if (DSD_SYNC_IS_X2TDMA(sync)) {
            x2_syncs++;
        }
        if (sync != DSD_SYNC_NONE) {
            for (int i = 0; i < X2_HANDLER_SYMBOLS; i++) {
                (void)getSymbol(&opts, &state, 1);
            }
        }
    }
    sf_close(opts.audio_in_file);
    opts.audio_in_file = NULL;
    free(opts.audio_in_file_info);
    opts.audio_in_file_info = NULL;
    free_state_buffers(&state);
    exitflag = 0;
    return x2_syncs;
}

static int
run_rate(int symbol_rate, int want_syncs) {
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_x2_rate");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "no temp file\n");
        return 1;
    }
    (void)dsd_close(fd);
    const long symbols = write_x2_wav(path, symbol_rate);
    if (symbols < 0) {
        DSD_FPRINTF(stderr, "could not write the %d sym/s X2-TDMA WAV\n", symbol_rate);
        (void)remove(path);
        return 1;
    }

    static const struct {
        dsdneoUserDecodeMode mode;
        const char* name;
    } modes[] = {{DSDCFG_MODE_X2TDMA, "-fx"}, {DSDCFG_MODE_AUTO, "-fa"}};

    int rc = 0;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        const int syncs = count_x2_syncs(path, modes[i].mode);
        /* The bursts it could find: one sync each, less what the hunt spends elsewhere under Auto. */
        const int ok = want_syncs ? syncs >= X2_BURSTS / 4 : syncs == 0;
        if (!ok) {
            DSD_FPRINTF(stderr, "FAIL #717: %s on %d bursts at %d sym/s found %d X2-TDMA syncs (want %s)\n",
                        modes[i].name, X2_BURSTS, symbol_rate, syncs, want_syncs ? ">= a quarter of them" : "none");
            rc = 1;
        }
    }
    (void)remove(path);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= run_rate(4800, 1);
    rc |= run_rate(6000, 0);
    if (rc == 0) {
        printf("FRAME_SYNC_X2TDMA_RATE: OK\n");
    }
    return rc;
}
