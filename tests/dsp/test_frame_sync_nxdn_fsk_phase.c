// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN48 payload decoding must not depend on the sub-symbol phase the decoder's
 * symbol grid happened to start on.
 *
 * Symbol timing on the RTL FSK discriminator path is decoder-side and open loop:
 * getSymbol() consumes samplesPerSymbol samples per symbol, so the symbol
 * boundary is nothing but "samples consumed since the grid was last reset, mod
 * sps". A profile switch or a stream flush leaves that boundary wherever the
 * consumed-sample count happened to land, and the sync search then accepts the
 * first phase whose dibits match rather than the best one, so the phase a frame
 * is read at varies with history the signal knows nothing about.
 *
 * These cases drive the real getFrameSync()/getSymbol()/getDibitSoft() chain --
 * nothing else covers that chain on the RTL sample path -- over a synthetic
 * 2400-baud four-level stream, once per starting sample offset and cache read
 * size, and require the payload to decode to the transmitted dibits for two
 * frames running whatever phase the search settled on.
 *
 * Symbol transitions are shaped over most of a symbol, as a filtered 2400-baud
 * stream's are, so an off-centre window reads a blend of two symbols rather than
 * a clean one and a phase that drifted shows up as wrong dibits.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_state_buffers.h"

#define NXDN48_SPS       20
#define NXDN_FSW         "3131331131"
#define NXDN_FSW_LEN     10
#define NXDN_FRAME       192
#define NXDN_PAYLOAD_LEN (NXDN_FRAME - NXDN_FSW_LEN)
#define FRAME_COUNT      16
#define PREAMBLE_DIBITS  32

/* Mixed inner/outer payload: outer runs stay short so no window of it can look
   like an all-outer frame sync word. */
static const char kPayloadCycle[] = "0132230110322301";

static size_t g_ramp_samples = 8U;
static float* g_waveform;
static size_t g_waveform_len;
static size_t g_waveform_pos;
static size_t g_read_chunk = 512U;
static int g_waveform_exhausted;

static char g_dibits[PREAMBLE_DIBITS + (FRAME_COUNT * NXDN_FRAME) + 1];
static size_t g_dibit_count;

static float
level_for_dibit(char dibit) {
    switch (dibit) {
        case '0': return 8000.0f;
        case '1': return 24000.0f;
        case '2': return -8000.0f;
        case '3': return -24000.0f;
        default: break;
    }
    return 0.0f;
}

static void
build_dibit_stream(void) {
    size_t n = 0;
    for (size_t i = 0; i < PREAMBLE_DIBITS; i++) {
        g_dibits[n++] = (i % 2U) ? '3' : '1';
    }
    const size_t cycle_len = sizeof(kPayloadCycle) - 1U;
    for (size_t f = 0; f < FRAME_COUNT; f++) {
        for (size_t i = 0; i < NXDN_FSW_LEN; i++) {
            g_dibits[n++] = NXDN_FSW[i];
        }
        for (size_t i = 0; i < NXDN_PAYLOAD_LEN; i++) {
            g_dibits[n++] = kPayloadCycle[(f + i) % cycle_len];
        }
    }
    g_dibits[n] = '\0';
    g_dibit_count = n;
}

/* Ideal NRZ smoothed by a boxcar, so every symbol boundary becomes a ramp
   RAMP_SAMPLES wide. A square wave would decode the same at any phase. */
static int
build_waveform(size_t ramp_samples) {
    g_ramp_samples = ramp_samples;
    free(g_waveform);
    g_waveform = NULL;
    const size_t n = g_dibit_count * NXDN48_SPS;
    float* nrz = (float*)calloc(n, sizeof(float));
    g_waveform = (float*)calloc(n, sizeof(float));
    if (!nrz || !g_waveform) {
        free(nrz);
        free(g_waveform);
        g_waveform = NULL;
        return 0;
    }
    for (size_t d = 0; d < g_dibit_count; d++) {
        const float level = level_for_dibit(g_dibits[d]);
        for (size_t s = 0; s < NXDN48_SPS; s++) {
            nrz[(d * NXDN48_SPS) + s] = level;
        }
    }
    for (size_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (size_t k = 0; k < g_ramp_samples; k++) {
            long idx = (long)i + (long)k - (long)(g_ramp_samples / 2U);
            if (idx < 0) {
                idx = 0;
            }
            if (idx >= (long)n) {
                idx = (long)n - 1;
            }
            sum += nrz[idx];
        }
        g_waveform[i] = sum / (float)g_ramp_samples;
    }
    g_waveform_len = n;
    free(nrz);
    return 1;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U) {
        return -1;
    }
    if (g_waveform_pos >= g_waveform_len) {
        g_waveform_exhausted = 1;
        *out_got = 0;
        return -1;
    }
    size_t want = (count < g_read_chunk) ? count : g_read_chunk;
    if (want > (g_waveform_len - g_waveform_pos)) {
        want = g_waveform_len - g_waveform_pos;
    }
    for (size_t i = 0; i < want; i++) {
        out[i] = g_waveform[g_waveform_pos++];
    }
    *out_got = (int)want;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 0.0;
}

static int
fake_output_kind(void) {
    return RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}

static unsigned int
fake_output_rate_hz(void) {
    return 48000U;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 2400;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return 1U;
}

/* Decode one frame's payload and report how many dibits missed. The frame the
   decoder locked onto is identified by its first payload dibit, so a phase that
   costs a whole symbol is reported as a mismatch rather than silently aligned. */
static int
check_payload(dsd_opts* opts, dsd_state* state, size_t frame_index, int* out_first_bad) {
    const size_t cycle_len = sizeof(kPayloadCycle) - 1U;
    int bad = 0;
    if (out_first_bad) {
        *out_first_bad = -1;
    }
    for (size_t i = 0; i < NXDN_PAYLOAD_LEN; i++) {
        dsd_dibit_soft_t soft;
        int dibit = getDibitSoft(opts, state, &soft);
        int expect = kPayloadCycle[(frame_index + i) % cycle_len] - '0';
        if (dibit != expect) {
            if (bad == 0 && out_first_bad) {
                *out_first_bad = (int)i;
            }
            bad++;
        }
    }
    return bad;
}

static int
run_phase_case(int sample_offset, size_t read_chunk, size_t* out_frame_index) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    g_waveform_pos = (size_t)sample_offset;
    g_waveform_exhausted = 0;
    g_read_chunk = read_chunk;
    dsd_frame_sync_reset_mod_state();

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_nxdn48 = 1;
    opts.msize = 1;
    opts.ssize = 128;

    state.rf_mod = 2;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .output_rate_hz = fake_output_rate_hz,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int rc = 0;
    int sync = DSD_SYNC_NONE;
    for (int attempt = 0; attempt < 4 && !g_waveform_exhausted; attempt++) {
        sync = getFrameSync(&opts, &state);
        if (sync == DSD_SYNC_NXDN_POS) {
            break;
        }
    }
    if (sync != DSD_SYNC_NXDN_POS) {
        DSD_FPRINTF(stderr, "offset %d ramp %zu chunk %zu: no NXDN48 sync (got %d, exhausted=%d)\n", sample_offset,
                    g_ramp_samples, read_chunk, sync, g_waveform_exhausted);
        rc = 1;
    } else {
        state.synctype = sync;
        /* Which transmitted frame the FSW belongs to: the samples consumed so
           far locate it, and the payload cycle is rotated per frame. */
        size_t consumed_dibits = (g_waveform_pos - (size_t)sample_offset) / NXDN48_SPS;
        size_t frame_index = 0;
        if (consumed_dibits > PREAMBLE_DIBITS) {
            frame_index = (consumed_dibits - PREAMBLE_DIBITS) / NXDN_FRAME;
        }
        if (out_frame_index) {
            *out_frame_index = frame_index;
        }
        int first_bad = -1;
        int bad = check_payload(&opts, &state, frame_index, &first_bad);
        if (bad != 0) {
            DSD_FPRINTF(stderr, "offset %d ramp %zu chunk %zu: %d/%d payload dibits wrong (first at %d)\n",
                        sample_offset, g_ramp_samples, read_chunk, bad, NXDN_PAYLOAD_LEN, first_bad);
            rc = 1;
        }

        /* The next frame must re-accept and decode just as cleanly, on the same
           grid: a phase held only for the frame it was acquired on is not held. */
        sync = getFrameSync(&opts, &state);
        if (sync != DSD_SYNC_NXDN_POS) {
            DSD_FPRINTF(stderr, "offset %d ramp %zu chunk %zu: second frame did not sync (got %d)\n", sample_offset,
                        g_ramp_samples, read_chunk, sync);
            rc = 1;
        } else {
            state.synctype = sync;
            first_bad = -1;
            bad = check_payload(&opts, &state, frame_index + 1U, &first_bad);
            if (bad != 0) {
                DSD_FPRINTF(stderr, "offset %d ramp %zu chunk %zu: frame 2 %d/%d payload dibits wrong (first at %d)\n",
                            sample_offset, g_ramp_samples, read_chunk, bad, NXDN_PAYLOAD_LEN, first_bad);
                rc = 1;
            }
        }
    }

    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    build_dibit_stream();

    int failures = 0;
    /* Two transition shapes: at 8 samples the search-time nudge settles wherever
       the starting offset left it, at 14 it settles on a fixed bias. Neither is
       the transmitted clock. Two read chunk sizes vary how much sits in the
       decoder's cache when a symbol is composed. */
    static const size_t kRamps[] = {8U, 14U};
    static const size_t kChunks[] = {512U, 37U};
    for (size_t r = 0; r < sizeof(kRamps) / sizeof(kRamps[0]); r++) {
        if (!build_waveform(kRamps[r])) {
            DSD_FPRINTF(stderr, "failed to build NXDN48 phase fixture\n");
            return 1;
        }
        for (size_t c = 0; c < sizeof(kChunks) / sizeof(kChunks[0]); c++) {
            for (int off = 0; off < NXDN48_SPS; off++) {
                size_t frame_index = 0;
                if (run_phase_case(off, kChunks[c], &frame_index) != 0) {
                    failures++;
                }
            }
        }
    }

    free(g_waveform);
    g_waveform = NULL;
    if (failures != 0) {
        DSD_FPRINTF(stderr, "NXDN48 symbol path: %d cases decoded wrongly\n", failures);
        return 1;
    }
    return 0;
}
