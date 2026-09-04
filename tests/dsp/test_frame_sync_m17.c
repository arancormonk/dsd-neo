// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_state_buffers.h"

static size_t g_sample_index;
static const char* g_sync_pattern = M17_PRE;
static char g_fill_symbol = '1';

static float
symbol_level_for_m17(char dibit) {
    return (dibit == '3') ? -3.0f : 3.0f;
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (!out || !out_got || count == 0U) {
        return -1;
    }

    const size_t pattern_len = strlen(g_sync_pattern);
    const size_t samples_per_symbol = 10U;
    for (size_t i = 0; i < count; i++) {
        size_t symbol_index = g_sample_index / samples_per_symbol;
        char dibit = (symbol_index < pattern_len) ? g_sync_pattern[symbol_index] : g_fill_symbol;
        out[i] = symbol_level_for_m17(dibit);
        g_sample_index++;
    }
    *out_got = (int)count;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 1.0;
}

static int
fake_output_kind(void) {
    return RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return 1U;
}

static int
fake_stream_active(void) {
    return 1;
}

static int
fake_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 0;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 0;
    }
    return 0;
}

static void
install_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics_hooks = {
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .cqpsk_status = fake_cqpsk_status,
        .stream_active = fake_stream_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics_hooks);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static void
clear_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static int
init_m17_sync_case(dsd_opts* opts, dsd_state* state, int* fake_rtl_context) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    if (!init_state_buffers(state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        return 0;
    }

    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 1;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 1;
    opts->frame_provoice = 1;
    opts->frame_ysf = 1;
    opts->frame_m17 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_gfsk = 1;
    opts->msize = 1;
    opts->ssize = 128;

    state->rf_mod = 2;
    state->p25_p2_active_slot = -1;
    state->rtl_ctx = (struct RtlSdrContext*)fake_rtl_context;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
    return 1;
}

static int
run_one_on_state(dsd_opts* opts, dsd_state* state, const char* pattern, int expected_sync, const char* label) {
    g_sample_index = 0U;
    g_sync_pattern = pattern;
    g_fill_symbol = (expected_sync < 0) ? '3' : '1';
    state->rtl_symbol_cache_pos = 0;
    state->rtl_symbol_cache_len = 0;
    state->rtl_symbol_cache_published_pending = 0;
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();

    int sync = getFrameSync(opts, state);
    if (sync != expected_sync) {
        DSD_FPRINTF(stderr, "%s returned %d, expected %d\n", label, sync, expected_sync);
        return 1;
    }
    return 0;
}

/* Frames after the first one in a chain are earned: a stream or packet that follows another
 * needs the transmission to have produced a clean LICH or a CRC, and a BERT chain needs its
 * PRBS9 lock (#399). A case that starts mid-chain has to stand where a real one would. */
static int
run_m17_chain_case(int initial_last, uint8_t initial_polarity, const char* pattern, int expected_sync,
                   const char* label, int evidence, int bert_locked) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    dsd_frame_sync_reset_mod_state();
    if (!init_m17_sync_case(&opts, &state, &fake_rtl_context)) {
        return 1;
    }
    state.lastsynctype = initial_last;
    state.m17_polarity = initial_polarity;
    state.m17_confirm_weak_streak = (uint8_t)(evidence ? 1 : 0);
    state.m17_bert_locked = (uint8_t)(bert_locked ? 1 : 0);

    install_hooks();
    const int rc = run_one_on_state(&opts, &state, pattern, expected_sync, label);
    clear_hooks();
    free_state_buffers(&state);
    return rc;
}

static int
run_m17_sync_case(int initial_last, uint8_t initial_polarity, const char* pattern, int expected_sync,
                  const char* label) {
    return run_m17_chain_case(initial_last, initial_polarity, pattern, expected_sync, label, 1, 1);
}

int
main(void) {
    int rc = 0;
    /* The preamble candidate itself is covered in tests/dsp/test_frame_sync_internal_helpers.c,
     * which drives one window at a time: this harness pads a pattern with a single repeated
     * symbol, and a uniform run is within one error of both M17_BRT and M17_PKT, so a run-length
     * pattern here would be measuring the padding. What it can pin is the chain behind the
     * candidate. */
    rc |= run_m17_sync_case(DSD_SYNC_M17_LSF_POS, 1U, M17_STR, DSD_SYNC_M17_STR_POS, "M17 LSF to stream");
    rc |= run_m17_sync_case(DSD_SYNC_M17_LSF_POS, 1U, M17_PKT, DSD_SYNC_M17_PKT_POS, "M17 LSF to packet");
    rc |= run_m17_sync_case(DSD_SYNC_M17_BRT_POS, 1U, M17_BRT, DSD_SYNC_M17_BRT_POS, "M17 BERT to BERT");
    rc |= run_m17_sync_case(DSD_SYNC_M17_STR_POS, 1U, M17_STR, DSD_SYNC_M17_STR_POS, "M17 stream to stream");
    rc |= run_m17_sync_case(DSD_SYNC_M17_PKT_POS, 1U, M17_PKT, DSD_SYNC_M17_PKT_POS, "M17 packet to packet");
    rc |= run_m17_sync_case(DSD_SYNC_M17_STR_POS, 1U, M17_EOT, DSD_SYNC_M17_EOT_POS, "M17 stream to EOT");
    rc |= run_m17_sync_case(DSD_SYNC_NONE, 0U, M17_EOT, -1, "M17 rejects cold EOT");

    /* Frames after the first in a chain are earned: a stream or packet following another needs
     * the transmission to have produced a clean LICH or a CRC, a BERT chain needs its PRBS9
     * lock, and a terminator needs a transmission to terminate (#399). */
    rc |= run_m17_chain_case(DSD_SYNC_M17_STR_POS, 1U, M17_STR, -1, "M17 stream chain needs evidence", 0, 1);
    rc |= run_m17_chain_case(DSD_SYNC_M17_PKT_POS, 1U, M17_PKT, -1, "M17 packet chain needs evidence", 0, 1);
    rc |= run_m17_chain_case(DSD_SYNC_M17_BRT_POS, 1U, M17_BRT, -1, "M17 BERT chain needs a PRBS9 lock", 1, 0);
    rc |= run_m17_chain_case(DSD_SYNC_M17_STR_POS, 1U, M17_EOT, -1, "M17 EOT needs a transmission to end", 0, 1);
    return rc;
}
