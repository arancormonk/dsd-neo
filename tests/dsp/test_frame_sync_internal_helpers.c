// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/tetra_training.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <stdlib.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_test_support.h"

static void
reset(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
}

static void
test_tetra_soft_history_capture(void) {
    /* The history outlives the call as the state does: the state keeps its address. */
    static dsd_state state;
    static float history[16];
    float captured[5] = {0};
    DSD_MEMSET(&state, 0, sizeof(state));
    for (int i = 0; i < 16; i++) {
        history[i] = (float)(100 + i);
    }
    state.symbol_history = history;
    state.symbol_history_size = 16;
    state.symbol_history_head = 2; /* newest is history[1], after wrap */
    state.symbol_history_count = 16;

    assert(dsd_frame_sync_test_capture_tetra_soft(&state, captured, 5, 3) == 1);
    /* Eight-symbol window is history[10..15,0..1]; payload is its first five. */
    for (int i = 0; i < 5; i++) {
        assert(fabsf(captured[i] - (float)(110 + i)) < 1e-6F);
    }

    state.symbol_history_count = 7;
    assert(dsd_frame_sync_test_capture_tetra_soft(&state, captured, 5, 3) == 0);
    assert(dsd_frame_sync_test_capture_tetra_soft(NULL, captured, 5, 3) == 0);
    assert(dsd_frame_sync_test_capture_tetra_soft(&state, NULL, 5, 3) == 0);
}

static void
test_tetra_inverted_sync_patterns_flip_only_dibit_msb(void) {
    const char* normal[] = {TETRA_NDB_NTS_SYNC, TETRA_SB_SSB_SYNC};
    const char* inverted[] = {INV_TETRA_NDB_NTS_SYNC, INV_TETRA_SB_SSB_SYNC};
    for (size_t pattern = 0; pattern < sizeof(normal) / sizeof(normal[0]); pattern++) {
        assert(strlen(normal[pattern]) == strlen(inverted[pattern]));
        for (size_t i = 0; normal[pattern][i] != '\0'; i++) {
            const int expected = (normal[pattern][i] - '0') ^ 2;
            assert(inverted[pattern][i] == (char)('0' + expected));
        }
    }
}

/* Pseudo-random data dibits for the TETRA burst windows. */
static void
tetra_data_dibits(char* out, int count, uint32_t* seed) {
    for (int i = 0; i < count; i++) {
        *seed = (*seed * 1103515245u) + 12345u;
        out[i] = (char)('0' + ((*seed >> 16) & 3u));
    }
}

static int
tetra_block1_is(const dsd_state* state, const char* expected, int count) {
    for (int i = 0; i < count; i++) {
        if (state->tetra_sync.b1_dibuf[i] != (uint8_t)(expected[i] - '0')) {
            return 0;
        }
    }
    return state->tetra_sync.b1_valid == 1;
}

/* The control uplink burst's training sequence holds NTS1 two symbols before its end (TETRA_CB_SYNC[2..12], EN 300
 * 392-2 clause 9.4.4.3.2). With --tetra-uplink a control burst locks as one at the end of its training, not as a normal
 * burst two symbols earlier. An uplink normal burst locks two symbols after its training, once they show it is no
 * control burst, and hands them to the decoder as the first two dibits of block 2. The downlink normal burst still
 * locks at the end of its training. */
static void
test_tetra_uplink_control_burst_is_not_a_normal_burst(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->frame_tetra = 1;
    opts->tetra_uplink = 1;
    char stream[160];
    uint32_t seed = 7u;

    /* A control burst: data, its 42-dibit block 1 and its 15-dibit training sequence. */
    tetra_data_dibits(stream, 112, &seed);
    DSD_MEMCPY(stream + 112, TETRA_CB_SYNC, 15);
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 125) == DSD_SYNC_NONE);
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 127) == DSD_SYNC_TETRA_NDB_POS);
    assert(state->tetra_sync.cb == 1 && state->tetra_sync.b2_lead_count == 0);
    assert(tetra_block1_is(state, stream + 70, 42));

    /* Inverted, it locks with the negative synctype, which tells the decoder its polarity; the carrier's polarity is the
     * decoder's to set once the burst's SCH/HU passed its CRC. */
    DSD_MEMSET(&state->tetra_sync, 0, sizeof(state->tetra_sync));
    DSD_MEMCPY(stream + 112, INV_TETRA_CB_SYNC, 15);
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 127) == DSD_SYNC_TETRA_NDB_NEG);
    assert(state->tetra_sync.cb == 1 && state->tetra_sync.polarity == 0
           && state->lastsynctype == DSD_SYNC_TETRA_NDB_NEG);

    /* An uplink normal burst: block 1, NTS1, then two dibits of block 2 that complete no control burst. */
    DSD_MEMSET(&state->tetra_sync, 0, sizeof(state->tetra_sync));
    tetra_data_dibits(stream, 118, &seed);
    DSD_MEMCPY(stream + 118, TETRA_NDB_NTS_SYNC, 11);
    stream[129] = '1';
    stream[130] = '2';
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 129) == DSD_SYNC_NONE);
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 131) == DSD_SYNC_TETRA_NDB_POS);
    assert(state->tetra_sync.cb == 0 && state->tetra_sync.nts2 == 0);
    assert(tetra_block1_is(state, stream + 10, 108));
    assert(state->tetra_sync.b2_lead_count == 2 && state->tetra_sync.b2_lead_dibits[0] == 1
           && state->tetra_sync.b2_lead_dibits[1] == 2);

    /* The downlink normal burst: block 1, AACH half 1, NTS1; nothing of block 2 is read yet. */
    opts->tetra_uplink = 0;
    DSD_MEMSET(&state->tetra_sync, 0, sizeof(state->tetra_sync));
    tetra_data_dibits(stream, 125, &seed);
    DSD_MEMCPY(stream + 125, TETRA_NDB_NTS_SYNC, 11);
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 136) == DSD_SYNC_TETRA_NDB_POS);
    assert(state->tetra_sync.cb == 0 && state->tetra_sync.b2_lead_count == 0);
    assert(tetra_block1_is(state, stream + 10, 108));
    free(state);
    free(opts);
}

/* After a carrier change each TETRA burst waits only for what it reads back: a synchronisation burst its 79 symbols
   (block 1 and the training), a downlink normal burst block 1, AACH half 1 and the training (126). A synchronisation
   burst wholly the new carrier's is taken; a normal burst whose block 1 reaches before the change is not. */
static void
test_tetra_bursts_wait_for_their_own_look_back(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->frame_tetra = 1;
    char stream[160];
    uint32_t seed = 11u;
    state->carrier_change_seen = 1U;
    state->carrier_change_symbol_clock = 1000U;

    tetra_data_dibits(stream, 60, &seed);
    DSD_MEMCPY(stream + 60, TETRA_SB_SSB_SYNC, 19);
    state->symbol_clock = 1000U + 78U;
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 79) == DSD_SYNC_NONE);
    state->symbol_clock = 1000U + 79U;
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 79) == DSD_SYNC_TETRA_SB_POS);

    DSD_MEMSET(&state->tetra_sync, 0, sizeof(state->tetra_sync));
    tetra_data_dibits(stream, 125, &seed);
    DSD_MEMCPY(stream + 125, TETRA_NDB_NTS_SYNC, 11);
    state->symbol_clock = 1000U + 125U;
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 136) == DSD_SYNC_NONE);
    state->symbol_clock = 1000U + 126U;
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, stream, 136) == DSD_SYNC_TETRA_NDB_POS);
    free(state);
    free(opts);
}

static int g_vc_sync_hook_calls;
static int g_vc_no_sync_hook_calls;
static int g_release_hook_calls;
static int g_no_carrier_hook_calls;
static int g_vc_no_sync_hook_order;
static int g_release_hook_order;
static int g_no_carrier_hook_order;
static int g_hook_order;

static void
fake_p25_sm_vc_sync(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_vc_sync_hook_calls++;
}

static void
fake_p25_sm_vc_no_sync(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_vc_no_sync_hook_calls++;
    g_vc_no_sync_hook_order = ++g_hook_order;
}

static void
fake_p25_sm_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_release_hook_calls++;
    g_release_hook_order = ++g_hook_order;
}

static void
fake_no_carrier(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_no_carrier_hook_calls++;
    g_no_carrier_hook_order = ++g_hook_order;
}

static void
test_p25_vc_acquisition_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts.frame_p25p2 = 1;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.min = -3.0f;
    state.max = 3.0f;
    g_vc_sync_hook_calls = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_vc_sync = fake_p25_sm_vc_sync,
    });

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(g_vc_sync_hook_calls == 1);
    assert(state.last_cc_sync_time == 0);
    assert(state.last_cc_sync_time_m == 0.0);

    opts.trunk_is_tuned = 0;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(g_vc_sync_hook_calls == 1);
    assert(state.last_cc_sync_time != 0);
    assert(state.last_cc_sync_time_m > 0.0);

    reset(&opts, &state);
    g_vc_no_sync_hook_calls = 0;
    g_no_carrier_hook_calls = 0;
    g_vc_no_sync_hook_order = 0;
    g_no_carrier_hook_order = 0;
    g_hook_order = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
        .no_carrier = fake_no_carrier,
    });

    assert(dsd_frame_sync_test_handle_no_sync_timeout(&opts, &state, 1799) == 0);
    assert(g_vc_no_sync_hook_calls == 0);
    assert(g_no_carrier_hook_calls == 0);
    assert(dsd_frame_sync_test_handle_no_sync_timeout(&opts, &state, 1800) == 1);
    assert(g_vc_no_sync_hook_calls == 1);
    assert(g_no_carrier_hook_calls == 1);
    assert(g_vc_no_sync_hook_order < g_no_carrier_hook_order);

    /* An inverted P25p1 sync used to short-circuit this timeout outright, so the dwell
     * never expired while it was the last sync seen and only the hunt's budget exit ever
     * returned no-sync (#389). The dwell is the only gate now, and the timeout owes the
     * P25 SM the same accounting in the same order as the other no-sync exit. */
    reset(&opts, &state);
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.lastsynctype = DSD_SYNC_P25P1_NEG;
    g_vc_no_sync_hook_calls = 0;
    g_release_hook_calls = 0;
    g_no_carrier_hook_calls = 0;
    g_vc_no_sync_hook_order = 0;
    g_release_hook_order = 0;
    g_no_carrier_hook_order = 0;
    g_hook_order = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_release = fake_p25_sm_release,
        .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
        .no_carrier = fake_no_carrier,
    });

    assert(dsd_frame_sync_test_handle_no_sync_timeout(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 1) == 0);
    assert(g_vc_no_sync_hook_calls == 0);
    assert(g_release_hook_calls == 0);
    assert(g_no_carrier_hook_calls == 0);
    assert(dsd_frame_sync_test_handle_no_sync_timeout(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS) == 1);
    assert(g_vc_no_sync_hook_calls == 1);
    assert(g_release_hook_calls == 1);
    assert(g_no_carrier_hook_calls == 1);
    assert(g_vc_no_sync_hook_order < g_release_hook_order);
    assert(g_release_hook_order < g_no_carrier_hook_order);

    dsd_frame_sync_hooks_set(NULL);
}

static void
test_sps_hunt_skips_disabled_protocol_rates(void) {
    static dsd_opts opts;
    static dsd_state state;

    static const int expected_rates[] = {4800, 2400, 9600, 6000, 4800, 18000};
    static const int expected_levels[] = {4, 4, 2, 4, 2, 4};
    assert(dsd_frame_sync_test_sps_hunt_profile_count() == 6);
    for (int i = 0; i < 6; i++) {
        assert(dsd_frame_sync_test_sps_hunt_profile_rate(i) == expected_rates[i]);
        assert(dsd_frame_sync_test_sps_hunt_profile_levels(i) == expected_levels[i]);
    }

    reset(&opts, &state);
    opts.frame_dstar = 1;
    state.sps_hunt_idx = 3;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 4);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    state.sps_hunt_idx = 4;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 0);

    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 0;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 1);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    state.sps_hunt_idx = 1;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 2;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 3);

    reset(&opts, &state);
    state.sps_hunt_idx = 2;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == 2);

    /* TETRA's 18 ksym/s profile is the only candidate under -fT, so the hunt stays on it, and it is never a candidate
     * for any other protocol. */
    reset(&opts, &state);
    opts.frame_tetra = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_18000_4;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == DSD_FRAME_SYNC_SPS_PROFILE_18000_4);
    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    assert(frame_sync_sps_hunt_next_index(&opts, &state) == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
}

static void
test_sps_hunt_profile_updates_timing(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.wav_decimator = 48000;
    for (int profile_index = 0; profile_index < dsd_frame_sync_test_sps_hunt_profile_count(); profile_index++) {
        state.sps_hunt_idx = (profile_index + 1) % dsd_frame_sync_test_sps_hunt_profile_count();
        state.samplesPerSymbol = -1;
        state.symbolCenter = -1;
        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        int symbol_rate = dsd_frame_sync_test_sps_hunt_profile_rate(profile_index);
        int expected_sps = dsd_opts_compute_sps_rate(&opts, symbol_rate, dsd_opts_current_input_timing_rate(&opts));
        assert(state.sps_hunt_idx == profile_index);
        assert(state.samplesPerSymbol == expected_sps);
        assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));

        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        assert(state.samplesPerSymbol == expected_sps);
        assert(state.symbolCenter == dsd_opts_symbol_center(expected_sps));
    }

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    state.sps_hunt_idx = 4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == 4);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);

    /* -mg keeps the GFSK demodulator locked while alternating same-timing
     * four-level and binary gates, so D-STAR remains reachable. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_gfsk = 1;
    state.rf_mod = 2;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(state.sps_hunt_counter == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.rf_mod == 2);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    /* -mc likewise preserves the C4FM demodulator while exposing D-STAR's
     * same-rate binary matcher gate. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(state.sps_hunt_counter == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(state.rf_mod == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    /* -mq with no -f (a RadioReference simulcast import) runs the default set, Auto. Its NXDN48, dPMR and ProVoice
     * profiles are timed differently, so the locked QPSK hunt still steps only 4800/4 <-> 4800/2, as under the
     * pre-Auto default. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dmr = 1;
    opts.frame_dpmr = 1;
    opts.frame_provoice = 1;
    opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    for (int step = 0; step < 4; step++) {
        const int from = state.sps_hunt_idx;
        state.sps_hunt_counter =
            dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        assert(state.sps_hunt_idx
               == (from == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 ? DSD_FRAME_SYNC_SPS_PROFILE_4800_2
                                                             : DSD_FRAME_SYNC_SPS_PROFILE_4800_4));
        assert(state.samplesPerSymbol == 10);
        assert(state.rf_mod == 1);
    }

    /* At 16 kHz, 4800 and 6000 symbols/s both round to 3 SPS. Generic demodulator
     * locks still rotate gates, but the profile-specific -m2/M selection must not. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.min = -3.0f;
    state.max = 3.0f;
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;
    assert(dsd_opts_compute_sps_rate(&opts, 4800, 0) == state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.sps_hunt_counter == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* A known FDMA CC retune supersedes the P25p2 helper's original profile choice. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 4800, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_opts_compute_sps_rate(&opts, 6000, 0) == state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 16;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 0);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == 0);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.frame_p25p2 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 8;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 3);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == 8);
}

static int g_p25_probe_in_flight;
static int g_p25_probe_queries;

static int
fake_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_p25_probe_queries++;
    return g_p25_probe_in_flight;
}

/* One expired dwell of the unlocked hunt, starting from @p from. */
static int
step_auto_hunt_from(dsd_opts* opts, dsd_state* state, int from) {
    state->sps_hunt_idx = from;
    state->sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(opts, state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    (void)frame_sync_no_sync_sps_hunt(opts, state);
    return state->sps_hunt_idx;
}

/*
 * Auto under trunking: while a P25-driven startup or recovery probe is in flight, the hunt steps only between the P25
 * profiles (4800/4 -> 6000/4), the sequence a P25-only rotation runs, so a probe tries a Phase 2 control channel
 * within its dwell; with no probe the full Auto rotation runs (4800/4 -> 2400/4 -> ...). The engine answers the query
 * once per step; with nothing installed there is no probe.
 */
static void
test_sps_hunt_p25_probe_rotation(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.trunk_enable = 1;
    opts.frame_p25p1 = opts.frame_p25p2 = opts.frame_dmr = opts.frame_nxdn48 = opts.frame_nxdn96 = 1; // -fa
    opts.frame_dpmr = opts.frame_provoice = opts.frame_dstar = opts.frame_x2tdma = opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_frame_sync_hooks_set(NULL); // nothing installed: no probe, the full rotation
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4) == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);

    // The 4800/4 dwell of a trunked P25 session ends inside a hunt probe's two seconds, so the probe makes its one
    // step, to 6000/4, before it ends.
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS < 2 * 4800);

    g_p25_probe_queries = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){.p25_probe_in_flight = fake_p25_probe_in_flight});
    g_p25_probe_in_flight = 1;
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4) == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(g_p25_probe_queries == 1); // one query per step
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4) == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    // A profile the full rotation left the probe on (a GFSK one included) steps back to the P25 rate.
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_9600_2) == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_2400_4) == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    // Without the dwell spent nothing steps, and nothing is asked.
    g_p25_probe_queries = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = 0;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(g_p25_probe_queries == 0);

    // The probe ended (an owner named, or the hunt parked): the full Auto rotation resumes.
    g_p25_probe_in_flight = 0;
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4) == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4) == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);

    // A Phase 1 only set stays on 4800/4 while a probe is in flight.
    g_p25_probe_in_flight = 1;
    opts.frame_p25p2 = opts.frame_x2tdma = 0;
    assert(step_auto_hunt_from(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4) == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    opts.frame_p25p2 = opts.frame_x2tdma = 1;
    dsd_frame_sync_hooks_set(NULL);
}

/*
 * The hunt's dwell is a symbol budget that survives sync returns (issue #388). A sync that
 * raises carrier but yields no frame used to stop the hunt outright: it blocked the step and
 * zeroed the dwell. Only symbols a frame handler actually consumes buy the profile more time.
 */
static void
test_sps_hunt_budget_is_spent_in_symbols(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;

    /* One symbol short of the budget the profile keeps its place. */
    state.sps_hunt_counter = dwell - 1;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == dwell - 1);

    /* Carrier is not a veto. An unproductive sync raises it, and the hunt still steps. */
    state.carrier = 1;
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(state.sps_hunt_counter == 0);
}

/*
 * Issue #392: the profile on a trunked voice channel came with the grant that tuned it, so
 * an expired dwell holds it rather than rotating. The dwell still expired, though, and the
 * caller is still owed its no-sync accounting (#393) -- that accounting is what gives a dead
 * voice channel up, and suppressing it would leave nothing to clear trunk_is_tuned.
 */
static void
test_sps_hunt_holds_the_profile_on_a_tuned_voice_channel(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;

    /* The dwell expires: the profile stays, the budget restarts, and the caller is told to
     * take its no-sync exit anyway. */
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.sps_hunt_counter == 0);

    /* Restarting the budget is what keeps the caller's loop from spinning: the next call is
     * a whole dwell away from expiring again. */
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);

    /* A probe on trial when the grant arrived still has its verdict read. This is the only
     * site that clears the flag, and while it is set the modulation votes cannot take the
     * demodulator back -- leaving it latched would pin rf_mod for the whole call. */
    state.p25_p1_mod_probe_active = 1;
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.p25_p1_mod_probe_active == 0);

    /* Dropping either half of the condition hands the profile back to the hunt: a control
     * channel is not tuned in this sense, and neither is a conventional receiver. */
    opts.trunk_is_tuned = 0;
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.trunk_is_tuned = 1; /* stale flag with trunking off */
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
}

/*
 * dsd_state::symbolcnt free-runs and wraps at 2^32 (issue #395). The hunt measures what a
 * handler consumed as a modular difference against its mark, so a rollover between the mark
 * and the next getFrameSync() entry must still yield the true symbol count -- and a reset to
 * zero must still be recognised as "an unrelated subsystem zeroed it" and credit nothing.
 *
 * This is a contract test, not a regression test: it passes against the pre-#395 signed
 * fields too, because the helper already laundered the subtraction through explicit
 * (unsigned int) casts, which made the arithmetic modular whatever the field types were.
 * It pins the wrap-exactness the uint32_t fields now carry on their own -- so a later widen
 * or re-signing of either field has to keep it -- but it is not what would have caught the
 * overflow. DSP_SYMBOL_REPLAY's test_symbol_count_wraps_instead_of_overflowing() is.
 */
static void
test_sps_hunt_consumption_is_exact_across_the_symbolcnt_wrap(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* 20 symbols consumed straddling the wrap: mark at UINT32_MAX - 25, counter at
     * UINT32_MAX - 5. That is a frame's worth, so the budget is debited by exactly 20. */
    reset(&opts, &state);
    state.sps_hunt_counter = 1000;
    state.sps_hunt_symbolcnt_mark = UINT32_MAX - 25U;
    state.symbolcnt = UINT32_MAX - 5U;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 980);
    assert(state.sps_hunt_symbolcnt_mark == UINT32_MAX - 5U);

    /* Same 20 symbols, now with the wrap falling inside the interval. */
    reset(&opts, &state);
    state.sps_hunt_counter = 1000;
    state.sps_hunt_symbolcnt_mark = UINT32_MAX - 15U;
    state.symbolcnt = 4U;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 980);
    assert(state.sps_hunt_symbolcnt_mark == 4U);

    /* A reset to zero (nxdn_reset_after_cac_fail(), initState()) looks like a backwards
     * jump and buys the profile nothing; the mark re-anchors for the next call. */
    reset(&opts, &state);
    state.sps_hunt_counter = 1000;
    state.sps_hunt_symbolcnt_mark = 5000U;
    state.symbolcnt = 0U;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 1000);
    assert(state.sps_hunt_symbolcnt_mark == 0U);

    /* The floor still applies across the wrap: 6 symbols reach the rollover and the rest land
     * past it, one short of a frame in all, so nothing is credited. */
    reset(&opts, &state);
    state.sps_hunt_counter = 1000;
    state.sps_hunt_symbolcnt_mark = UINT32_MAX - 5U;
    state.symbolcnt = (uint32_t)DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS - 1U - 6U;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 1000);
}

/*
 * Issue #400: consumption credit is bounded by what a handler read, and a P25p1 control
 * channel reads 134 symbols of a ~180-symbol slot, so a decoding channel can never get ahead
 * of the failures between its frames. A handler whose own check proves the profile restarts
 * the dwell instead of being paid for the frame.
 */
static void
test_sps_hunt_proven_verdict_restarts_the_dwell(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* A proof holds however little the frame cost to read, so the size floor that keeps
     * consumption credit honest has no say in it. */
    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 1000U + (uint32_t)DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS - 1U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_PROFILE_PROVEN;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 0);
    assert(state.sps_hunt_symbolcnt_mark == state.symbolcnt);

    /* One verdict answers for one handler call: the next entry finds the field cleared and
     * falls back to measuring, so a proof cannot restart a dwell it already restarted. */
    state.sps_hunt_counter = 5000;
    state.symbolcnt += 4U;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000);

    /* Nor does the backwards-jump guard, which exists for the same reason. A subsystem that
     * zeroed symbolcnt mid-measurement makes the interval unreadable; the proof does not
     * depend on reading it. */
    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_symbolcnt_mark = 5000U;
    state.symbolcnt = 0U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_PROFILE_PROVEN;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 0);
    assert(state.sps_hunt_symbolcnt_mark == 0U);

    /* The DSP layer reads the verdict as a literal, since it includes no engine headers. A
     * value it does not know is refused credit -- toward rotating, not toward pinning. */
    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 2000U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_WITHHELD + 1;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000);

    /* An unproductive frame is still refused, and a productive one is still paid what it read
     * and no more: neither path moved. */
    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 1134U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_UNPRODUCTIVE;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000);

    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 1134U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_PRODUCTIVE;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000 - 134);
}

/*
 * Issue #392: a frame the engine declined to dispatch consumes nothing, so the credit path
 * refuses it at the size floor and the search that found it stands charged. The cycle is made
 * neutral instead -- the counter goes back to where the cycle began, and no further.
 */
static void
test_sps_hunt_withheld_verdict_refunds_only_its_own_cycle(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* The search charged 400 symbols on top of a budget already at 5000; the refund hands
     * back exactly those 400, with no consumption to read. */
    reset(&opts, &state);
    state.sps_hunt_counter_at_entry = 5000;
    state.sps_hunt_counter = 5400;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 1000U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_WITHHELD;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000);
    /* The snapshot follows the counter, so the next cycle refunds against this one's result
     * rather than against a figure two cycles old. */
    assert(state.sps_hunt_counter_at_entry == 5000);
    assert(state.sps_hunt_symbolcnt_mark == state.symbolcnt);

    /* A refund can only ever lower the counter. Something outside the hunt -- a profile
     * change, a retune, a trunk-scan target switch -- may have zeroed it since the snapshot
     * was taken, and none of those is this function's to undo. */
    reset(&opts, &state);
    state.sps_hunt_counter_at_entry = 5000;
    state.sps_hunt_counter = 12;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_WITHHELD;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 12);
    assert(state.sps_hunt_counter_at_entry == 12);

    /* One verdict answers for one handler call: the next entry finds the field cleared, so a
     * withheld frame cannot refund a cycle it did not charge. */
    state.sps_hunt_counter = 900;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 900);

    /* Withheld beats consumption: the engine skipped the handler, so whatever the interval
     * measures is not a frame this profile read and must not be credited as one. */
    reset(&opts, &state);
    state.sps_hunt_counter_at_entry = 5000;
    state.sps_hunt_counter = 5400;
    state.sps_hunt_symbolcnt_mark = 1000U;
    state.symbolcnt = 3000U;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_WITHHELD;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 5000);

    /* Every other verdict leaves the snapshot tracking the counter too, so the cycle after a
     * proof or a debit refunds against what that left behind. */
    reset(&opts, &state);
    state.sps_hunt_counter = 5000;
    state.sps_hunt_last_frame_verdict = DSD_FRAME_VERDICT_PROFILE_PROVEN;
    dsd_frame_sync_test_sps_hunt_note_handler_consumption(&state);
    assert(state.sps_hunt_counter == 0);
    assert(state.sps_hunt_counter_at_entry == 0);
}

/*
 * The whole budget, for the sites that move the decoder without going through the hunt.
 * Zeroing the counter alone leaves the anchor where it was, which is the #394 half-reset;
 * a voice-channel tune owes both, plus the refund snapshot they now share a cycle with.
 */
static void
test_sps_hunt_restart_dwell_resets_the_whole_budget(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    state.sps_hunt_counter = 4321;
    state.sps_hunt_counter_at_entry = 4000;
    state.sps_hunt_symbolcnt_mark = 11U;
    state.symbolcnt = 90210U;
    dsd_frame_sync_sps_hunt_restart_dwell(&state);
    assert(state.sps_hunt_counter == 0);
    assert(state.sps_hunt_counter_at_entry == 0);
    assert(state.sps_hunt_symbolcnt_mark == 90210U);
    /* The profile itself is not the budget's business: this grants a dwell, it does not
     * choose what spends it. */
    assert(state.sps_hunt_idx == 0);

    dsd_frame_sync_sps_hunt_restart_dwell(NULL);
}

/* Carrier no longer wipes the hunt's progress from the modulation-switch path. */
static void
test_carrier_does_not_reset_the_hunt_budget(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.carrier = 1;
    state.sps_hunt_counter = 1234;

    int lastt = 0;
    for (int i = 0; i < 48; i++) {
        frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    }
    assert(state.sps_hunt_counter == 1234);
}

static void
test_sps_hunt_reconciles_external_timing(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 3);
    assert(state.samplesPerSymbol == 8);
    assert(state.symbolCenter == 3);
    assert(state.rf_mod == 1);
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* The normal -f2 preset selects QPSK and 6000-rate timing without a CLI modulation lock. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p2 = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 1);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    /* Manual -m2 carries profile 3 explicitly because low input rates can round
     * the 4800- and 6000-symbol timings to the same samples-per-symbol value. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 11025;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 11025);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    assert(dsd_opts_compute_sps_rate(&opts, 4800, 11025) == state.samplesPerSymbol);
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 7;
    state.symbolCenter = 3;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 7);
    assert(state.symbolCenter == 3);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_c4fm = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = 3;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 10);
    assert(state.symbolCenter == 4);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
}

/* #394: a profile the reconciliation adopts starts its dwell over. Before the fix it
 * inherited whatever the outgoing profile had already spent, so an adoption landing one
 * symbol short of the dwell was stepped away from on the very next symbol. */
static void
test_adopted_profile_starts_its_dwell_over(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p2 = 1;
    opts.frame_dstar = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);

    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    assert(dwell > 1);

    /* The outgoing profile is one symbol short of stepping, and its measurement anchor sits
     * where the handler that ran under it started consuming. */
    state.symbolcnt = 4096U;
    state.sps_hunt_counter = dwell - 1;
    state.sps_hunt_symbolcnt_mark = 1024U;

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.sps_hunt_counter == 0);
    assert(state.sps_hunt_symbolcnt_mark == state.symbolcnt);

    /* frame_sync_advance_sync_window() bills one symbol per symbol, so the adopted profile
     * must be able to spend a whole dwell before the hunt may rotate off it. */
    for (int spent = 1; spent < dwell; spent++) {
        state.sps_hunt_counter++;
        assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
        assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    }
    state.sps_hunt_counter++;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);

    /* A reconciliation with nothing to do is still not allowed to refund the budget: it is
     * the same profile spending the same dwell. This one turns back at the helper's own
     * early-out, so it pins the early-out rather than the profile_changed guard. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_p25p2 = 1;
    opts.frame_dstar = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.symbolcnt = 4096U;
    state.sps_hunt_counter = dwell - 1;
    state.sps_hunt_symbolcnt_mark = 4096U;

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.sps_hunt_counter == dwell - 1);

    /* The guard itself. A two-level profile whose modulation is not GFSK gets past the
     * early-out to normalise dsd_state::rf_mod without the index moving -- the one shape that
     * reaches the reset with profile_changed clear. Still the same profile spending the same
     * dwell, so an unguarded reset would refund a live budget here, and re-anchoring the mark
     * would hand that profile credit for symbols it had already been billed for. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.frame_dstar = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 4800, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.symbolcnt = 4096U;
    state.sps_hunt_counter = dwell - 1;
    state.sps_hunt_symbolcnt_mark = 1024U;

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    /* Proof the call ran past the early-out rather than turning back at it. */
    assert(state.rf_mod == 2);
    assert(state.sps_hunt_counter == dwell - 1);
    assert(state.sps_hunt_symbolcnt_mark == 1024U);
}

static void
test_binary_profiles_override_unlocked_qpsk(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_provoice = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    frame_sync_apply_sps_hunt_profile(&opts, &state, 2, 0);
    assert(state.sps_hunt_idx == 2);
    assert(state.rf_mod == 2);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    state.sps_hunt_idx = 4;
    state.samplesPerSymbol = 10;
    state.symbolCenter = 4;
    state.rf_mod = 1;
    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 4);
    assert(state.rf_mod == 2);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    frame_sync_apply_sps_hunt_profile(&opts, &state, 4, 0);
    assert(state.sps_hunt_idx == 4);
    assert(state.rf_mod == 1);
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
}

static void
test_four_level_profiles_reset_inherited_modulation(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.rf_mod == 0);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    opts.frame_provoice = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 0);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.rf_mod == 2);
}

static void
test_nxdn_variant_follows_active_profile(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_NONE);
    assert(dsd_frame_sync_active_nxdn_variant(NULL, &state) == DSD_NXDN_VARIANT_NONE);

    opts.frame_nxdn48 = 1;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, NULL) == DSD_NXDN_VARIANT_48);
    opts.frame_nxdn48 = 0;
    opts.frame_nxdn96 = 1;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, NULL) == DSD_NXDN_VARIANT_96);

    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_96);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_48);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_NONE);
}

static void
test_bounded_symbol_history_readiness_and_wrap(void) {
    static const int window_lengths[] = {1, 8, 10, 12, 16, 20, 24, 32, 48, 79, 119, 128};
    char symbols[384];
    char out[130];
    for (int i = 0; i < (int)sizeof(symbols); i++) {
        symbols[i] = (char)(1 + ((i * 37 + i / 7) % 126));
    }
    for (size_t i = 0; i < sizeof(window_lengths) / sizeof(window_lengths[0]); i++) {
        const int length = window_lengths[i];
        DSD_MEMSET(out, 'x', sizeof(out));
        assert(dsd_frame_sync_test_history_window(symbols, length - 1, length, out, (int)sizeof(out)) == 0);
        for (int j = 0; j < (int)sizeof(out); j++) {
            assert(out[j] == 'x');
        }
        assert(dsd_frame_sync_test_history_window(symbols, length, length, out, (int)sizeof(out)) == 1);
        assert(memcmp(out, symbols, (size_t)length) == 0);
        assert(out[length] == '\0');
        /* All head positions, with the ring already wrapped twice. */
        for (int count = 256; count < 384; count++) {
            DSD_MEMSET(out, 'x', sizeof(out));
            assert(dsd_frame_sync_test_history_window(symbols, count, length, out, length) == 0);
            assert(out[0] == 'x');
            assert(dsd_frame_sync_test_history_window(symbols, count, length, out, length + 1) == 1);
            assert(memcmp(out, symbols + count - length, (size_t)length) == 0);
            assert(out[length] == '\0');
            assert(out[length + 1] == 'x');
        }
    }
    assert(dsd_frame_sync_test_history_window(symbols, 384, 0, out, (int)sizeof(out)) == 0);
    assert(dsd_frame_sync_test_history_window(symbols, 384, 129, out, (int)sizeof(out)) == 0);
}

static int
training_oracle(const char* obs, const char* pat, const char* inv, int len, int* rotation, int* inverted) {
    static const int step[] = {1, 3, 0, 2};
    for (int rot = 0; rot < 4; rot++) {
        int normal = 1, inverse = inv != NULL;
        for (int i = 0; i < len; i++) {
            int d = (unsigned char)obs[i] & 3;
            for (int r = 0; r < rot; r++) {
                d = step[d];
            }
            normal &= d == pat[i] - '0';
            if (inv) {
                inverse &= d == inv[i] - '0';
            }
        }
        if (normal || inverse) {
            *rotation = rot;
            *inverted = !normal;
            return 1;
        }
    }
    return 0;
}

static void
test_tetra_training_rotation_oracle(void) {
    static const int step[] = {1, 3, 0, 2};
    for (int symbol = -20; symbol < 270; symbol++) {
        for (int rot = -10; rot < 10; rot++) {
            int want = symbol & 3;
            for (int r = 0; r < (rot & 3); r++) {
                want = step[want];
            }
            assert(tetra_dibit_derotate(symbol, rot) == want);
        }
    }
    uint32_t random = 0x48a1f37u;
    for (int trial = 0; trial < 12000; trial++) {
        char pat[32], inv[32], obs[32];
        const int len = 1 + trial % 32;
        for (int i = 0; i < len; i++) {
            random = random * 1664525u + 1013904223u;
            int d = (int)(random >> 30);
            pat[i] = (char)('0' + d);
            inv[i] = (char)('0' + (trial % 11 == 0 ? d : d ^ 2));
            if (trial & 1) {
                d = inv[i] - '0';
            }
            for (int r = 0; r < ((trial >> 1) & 3); r++) {
                d = step[d];
            }
            obs[i] = (char)(d + (trial & 8 ? '0' : 0));
        }
        if (trial % 3 == 0) {
            obs[(trial / 3) % len] ^= 1;
        }
        if (trial % 17 == 0) {
            pat[len - 1] = '9';
        }
        const char* inverse = trial % 5 == 0 ? NULL : inv;
        int want_rot = -1, want_inv = -1, got_rot = -1, got_inv = -1;
        const int want = training_oracle(obs, pat, inverse, len, &want_rot, &want_inv);
        assert(tetra_training_derotation(obs, pat, inverse, len, &got_rot, &got_inv) == want);
        assert(got_rot == want_rot && got_inv == want_inv);
        assert(tetra_training_derotation(obs, pat, inverse, len, NULL, NULL) == want);
    }
    int rot = -1, inv = -1;
    assert(tetra_training_derotation(NULL, "0", NULL, 1, &rot, &inv) == 0);
    assert(tetra_training_derotation("0", NULL, NULL, 1, &rot, &inv) == 0);
    assert(tetra_training_derotation("0", "0", NULL, 0, &rot, &inv) == 0);
    assert(rot == -1 && inv == -1);
}

static void
test_provoice_candidate_does_not_shadow_dstar_or_nxdn(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_dstar = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    opts.frame_dstar = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = 1;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

/* dPMR's FS2 is one exact 12-symbol match against a single pattern -- 2^-12 on a sign-sliced
 * stream -- while NXDN48 shares the profile with a matcher two orders of magnitude looser. The
 * 372 dibits behind an accepted FS2 are dPMR's frame, so nothing weaker gets to match inside
 * them: an NXDN accept there consumes symbols the frame needed and warm-starts the thresholds
 * from ten symbols of another protocol's payload (#374). Same shape as the P25 suppression in
 * dsd_frame_sync_suppress_p25_alt_sync(). */
static void
test_a_dpmr_frame_is_not_reopened_by_the_nxdn_matcher(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dpmr = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 1000;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DPMR_FRAME_SYNC_2, 12) == DSD_SYNC_DPMR_FS2_POS);

    /* Inside the frame the FS2 opened, NXDN never even latches a candidate. */
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);
    state.symbolcnt = 1000 + 380;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);

    /* Past the frame the matcher is live again, on its own two-hit terms. */
    state.symbolcnt = 1000 + DSD_FRAME_SYNC_DPMR_FS2_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

/* The suppression is dPMR's frame, not a mute switch: NXDN96 lives on 4800/4 where dPMR cannot
 * match at all, and a build with dPMR disabled must behave exactly as it did before. */
static void
test_the_dpmr_frame_suppression_is_scoped_to_its_own_profile(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dpmr = 0;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.dpmr_fs2_frame_symbolcnt = 1000;
    state.dpmr_fs2_frame_valid = 1;
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);

    reset(&opts, &state);
    opts.frame_dpmr = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.dpmr_fs2_frame_symbolcnt = 1000;
    state.dpmr_fs2_frame_valid = 1;
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
}

/* The 4800/4 counterpart (#388). P25p1's sync is one of two exact 24-symbol patterns; NXDN96's
 * takes five variants per polarity over ten sign-sliced symbols. Inside the frame the stronger
 * one opened, the weaker must not take a sync -- but it must still take its level estimate,
 * which is the wideband reference the other protocols on the profile lean on. */
static void
test_a_p25p1_frame_is_not_reopened_by_the_nxdn96_matcher(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 1000;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(state.p25_p1_c4fm_frame_valid == 1);

    /* Inside the frame: no sync, and -- the half that keeps use_symbol()'s C4FM threshold
     * tracker engaged between P25p1 syncs -- no claim on lastsynctype either. */
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_P25P1_POS);
    state.symbolcnt = 1000 + 899;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_P25P1_POS);

    /* Past the frame the matcher is live again, on its own two-hit terms: the second hit a frame after the first. */
    state.symbolcnt = 1000 + DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

/* Withholding the sync must not withhold the level estimate: the CQPSK voice capture stops
 * decoding its headers altogether if this blend goes away for the span (#388). Driven through
 * the window evaluator, which is the seam that carries real symbol levels. */
static void
test_a_suppressed_nxdn96_window_still_blends_levels(void) {
    static dsd_opts opts;
    static dsd_state state;
    float fsw_levels[10];

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    opts.msize = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 10; i++) {
        fsw_levels[i] = NXDN_FSW[i] == '3' ? -3.0f : 3.0f;
    }

    /* Arm the span by hand: the point here is what the NXDN window does inside one. */
    state.p25_p1_c4fm_frame_symbolcnt = 0;
    state.p25_p1_c4fm_frame_valid = 1;
    state.symbolcnt = 100;
    state.max = 9.0f;
    state.min = -9.0f;

    assert(dsd_frame_sync_test_eval_window(&opts, &state, NXDN_FSW, fsw_levels, 10) == DSD_SYNC_NONE);
    /* Blended halfway from 9 toward the window's own outer rails, exactly as an unsuppressed
     * hit would, while the sync itself is refused. */
    assert(fabsf(state.max - 6.0f) < 0.0001f);
    assert(fabsf(state.min - (-6.0f)) < 0.0001f);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);
}

/* CQPSK closes the span rather than opening one: there the NID is sliced by the QPSK chain, and
 * the NXDN blend is the level reference that chain depends on. A GFSK vote landing mid-span is
 * the flap the span exists to survive, so it leaves an armed span standing. */
static void
test_the_p25p1_frame_guard_is_scoped_to_the_c4fm_chain(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 1000;
    state.rf_mod = 1;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(state.p25_p1_c4fm_frame_valid == 0);
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);

    /* Armed on C4FM, then flapped to GFSK: the span stands. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 1000;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    state.rf_mod = 2;
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_P25P1_POS);
}

/* The span is P25p1's frame, not a mute switch: NXDN48 lives on 2400/4, where P25p1 cannot match
 * at all, and a build with P25p1 disabled must behave exactly as it did before. */
static void
test_the_p25p1_frame_suppression_is_scoped_to_its_own_profile(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 0;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p25_p1_c4fm_frame_symbolcnt = 1000;
    state.p25_p1_c4fm_frame_valid = 1;
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p25_p1_c4fm_frame_symbolcnt = 1000;
    state.p25_p1_c4fm_frame_valid = 1;
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
}

/* The stamp wraps with symbolcnt and is compared as a modular difference. */
static void
test_the_p25p1_frame_guard_survives_symbolcnt_rollover(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p25_p1_c4fm_frame_symbolcnt = UINT32_MAX - 100u;
    state.p25_p1_c4fm_frame_valid = 1;

    /* 150 symbols past the stamp, across the wrap. */
    state.symbolcnt = 49u;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);

    /* Past the span, still across the wrap. */
    state.symbolcnt = (uint32_t)(UINT32_MAX - 100u + DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
}

static void
test_symbol_replay_bypasses_sps_profile_gating(void) {
    static const int symbol_input_types[] = {AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT};
    static dsd_opts opts;
    static dsd_state state;

    for (size_t i = 0; i < sizeof(symbol_input_types) / sizeof(symbol_input_types[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_p25p2 = 1;
        state.sps_hunt_idx = 0;
        state.min = -3.0f;
        state.max = 3.0f;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    }

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.frame_p25p2 = 1;
    state.sps_hunt_idx = 0;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_NONE);
}

static void
test_dmr_rc_sync_matches_and_respects_polarity(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* Normal polarity: the MS RC sync maps to the RC synctype. */
    reset(&opts, &state);
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_MS_RC_SYNC, 24) == DSD_SYNC_DMR_RC_DATA);
    assert(state.lastsynctype == DSD_SYNC_DMR_RC_DATA);
    assert(strcmp(state.ftype, "DMR RC") == 0);

    /* The complement pattern is ETSI-reserved: never claimed as RC at
     * normal polarity. */
    reset(&opts, &state);
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_MS_RC_SYNC_INV, 24) == DSD_SYNC_NONE);

    /* Inverted input flips the RC sync into the complement pattern. */
    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.inverted_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_MS_RC_SYNC_INV, 24) == DSD_SYNC_DMR_RC_DATA);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_MS_RC_SYNC, 24) == DSD_SYNC_NONE);

    /* No DMR decoding, no RC sync. */
    reset(&opts, &state);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_MS_RC_SYNC, 24) == DSD_SYNC_NONE);
}

static void
test_symbol_replay_requires_explicit_nxdn_variant(void) {
    static const int symbol_input_types[] = {AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT};
    static dsd_opts opts;
    static dsd_state state;

    for (size_t i = 0; i < sizeof(symbol_input_types) / sizeof(symbol_input_types[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn48 = 1;
        opts.frame_nxdn96 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);

        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn48 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
        assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_48);

        reset(&opts, &state);
        opts.audio_in_type = symbol_input_types[i];
        opts.frame_nxdn96 = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
        /* NXDN96's second hit is the next frame's, 192 symbols on. */
        state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
        assert(dsd_frame_sync_active_nxdn_variant(&opts, &state) == DSD_NXDN_VARIANT_96);
    }
}

static void
test_manual_p25p2_c4fm_bypasses_profile_gating(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 96000;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dmr = 1;
    opts.frame_ysf = 1;
    opts.mod_c4fm = 1;
    opts.mod_cli_lock = 1;
    state.sps_hunt_idx = 0;
    state.samplesPerSymbol = 20;
    state.symbolCenter = 8;
    state.min = -3.0f;
    state.max = 3.0f;

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == 0);
    assert(state.samplesPerSymbol == 20);
    assert(state.symbolCenter == 8);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_NONE);

    opts.mod_p25p2_c4fm = 1;
    assert(dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_P25) == 0);
    assert(opts.mod_p25p2_c4fm == 1);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    dsd_scan_mode_leave(&opts, &state);
}

static void
test_locked_p25p2_c4fm_survives_sync(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    opts.mod_c4fm = 1;
    opts.mod_cli_lock = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.min = -3.0f;
    state.max = 3.0f;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(state.rf_mod == 0);
}

/* A preamble is only a candidate once it has run for DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS windows,
 * so a single marker proves nothing to the matcher. Feed it the run a real one supplies. */
static void
feed_m17_preamble_run(dsd_opts* opts, dsd_state* state, const char* marker) {
    for (int i = 0; i < DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS; i++) {
        assert(dsd_frame_sync_test_try_protocol_matches(opts, state, marker, 8) == DSD_SYNC_NONE);
    }
}

/* M17's preamble is an alternating run, which P25 payload supplies readily, so inside a P25p1
 * frame the run is dropped rather than latched -- and a candidate already in hand is aged out
 * rather than left to spend the moment the span lapses. */
static void
test_a_p25p1_frame_suppresses_m17_candidates(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_m17 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 1000;

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);

    /* Inside the frame a full preamble run latches nothing and the sync word behind it is
     * refused. */
    state.symbolcnt = 1000 + 100;
    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(state.m17_pre_candidate == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_NONE);

    /* Past the frame the run re-earns its candidate and the word spends it. */
    state.symbolcnt = 1000 + DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS;
    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(state.m17_pre_candidate != 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_M17_LSF_POS);
}

/* The windows an alternating run of @p windows symbols presents, one per symbol: the marker and its inversion in
 * turn, as a real preamble reads one symbol over. */
static void
feed_m17_alternating_windows(dsd_opts* opts, dsd_state* state, int windows) {
    for (int i = 0; i < windows; i++) {
        state->symbolcnt++;
        (void)dsd_frame_sync_test_try_protocol_matches(opts, state, (i & 1) ? M17_PIV : M17_PRE, 8);
    }
}

static void
m17_join_setup(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    opts->frame_m17 = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->min = -3.0f;
    state->max = 3.0f;
    state->symbolcnt = 7000;
}

/*
 * M17's way in used to be any alternating run of eight windows followed by a sync word within one error, and receiver
 * noise supplies that a few times a second (the noise_floor fixture opened 27 M17 frames in 10 s under -fz). The same
 * weakness was how a transmission joined mid-stream got in: no preamble, so a run in the payload and a lucky word.
 * Every M17 frame is 192 symbols, a sync burst and 184 symbols of payload (M17 spec, Frame), and stream and BERT frames
 * follow each other back to back, so a transmission in progress offers its frame's sync word every 192 symbols --
 * something noise does not. The same exact sync word three frames running now joins a transmission, for that one
 * frame, and the preamble, which a transmission sends for 192 symbols, must run for DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS
 * windows.
 */
static int
m17_word_three_frames(dsd_opts* opts, dsd_state* state, const char* first, const char* second, const char* third) {
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, first, 8) == DSD_SYNC_NONE);
    state->symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(opts, state, second, 8) == DSD_SYNC_NONE);
    state->symbolcnt += 192;
    return dsd_frame_sync_test_try_protocol_matches(opts, state, third, 8);
}

static void
test_m17_joins_on_a_sync_word_three_frames_running(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* A stream joined mid-transmission: the first two words only mark the place, the third joins. */
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_STR, M17_STR, M17_STR) == DSD_SYNC_M17_STR_POS);
    /* ... and the frames after it chain as ever, once the joined frame proved itself. */
    state.m17_confirm_weak_streak = 1;
    state.symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_M17_STR_POS);

    /* A join opens one frame. A joined frame that proved nothing hands the next none of its permission: the words
     * behind it are forgotten, and the stream waits for three fresh ones. */
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_STR, M17_STR, M17_STR) == DSD_SYNC_M17_STR_POS);
    state.symbolcnt += 192;
    assert(m17_word_three_frames(&opts, &state, M17_STR, M17_STR, M17_STR) == DSD_SYNC_M17_STR_POS);

    /* Every exact word is remembered, whichever path takes it: a BERT run that loses its PRBS lock rejoins on the
     * frames it was read on, the one its preamble opened and the one the lock continued. */
    m17_join_setup(&opts, &state);
    feed_m17_alternating_windows(&opts, &state, DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS);
    state.symbolcnt++;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_BRT, 8) == DSD_SYNC_M17_BRT_POS);
    state.m17_bert_locked = 1;
    state.symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_BRT, 8) == DSD_SYNC_M17_BRT_POS);
    state.m17_bert_locked = 0;
    state.symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_BRT, 8) == DSD_SYNC_M17_BRT_POS);

    /* Two words a frame apart are what dead channels offer by chance, so they are not enough on their own. */
    m17_join_setup(&opts, &state);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);

    /* Not a frame apart, no join: one symbol either way, or a frame and a half. */
    m17_join_setup(&opts, &state);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 191;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 2;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 96;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);

    /* Before a lock every payload window is read too, and a payload matches some sync word a few times a frame. Those
     * hits are remembered beside the real ones; they must not hide them. */
    m17_join_setup(&opts, &state);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 50;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_PKT, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 50;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 92;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 30;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_NONE);
    state.symbolcnt += 162;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_M17_STR_POS);

    /* Different words a frame apart are not one transmission's frames. */
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_STR, M17_STR, M17_LSF) == DSD_SYNC_NONE);
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_STR, M17_LSF, M17_STR) == DSD_SYNC_NONE);

    /* The LSF word repeats only as an inverted stream (an LSF is sent once), and BERT joins on its own word -- or on
     * the packet word, its inversion, when the session's polarity says the transmission is inverted. */
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_LSF, M17_LSF, M17_LSF) == DSD_SYNC_M17_STR_NEG);

    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_BRT, M17_BRT, M17_BRT) == DSD_SYNC_M17_BRT_POS);

    m17_join_setup(&opts, &state);
    opts.inverted_m17 = 1;
    assert(m17_word_three_frames(&opts, &state, M17_PKT, M17_PKT, M17_PKT) == DSD_SYNC_M17_BRT_NEG);

    /* A packet is never joined: its one CRC covers every packet frame, so a packet joined after its first cannot be
     * read, and the packet word turns up a frame apart in NXDN96 data frames' LICH. Nor is a BERT word under an
     * inverted session, which is an inverted packet. */
    m17_join_setup(&opts, &state);
    assert(m17_word_three_frames(&opts, &state, M17_PKT, M17_PKT, M17_PKT) == DSD_SYNC_NONE);
    m17_join_setup(&opts, &state);
    opts.inverted_m17 = 1;
    assert(m17_word_three_frames(&opts, &state, M17_BRT, M17_BRT, M17_BRT) == DSD_SYNC_NONE);

    /* A short alternating run -- what noise supplies -- latches nothing, so the word behind it opens nothing. */
    m17_join_setup(&opts, &state);
    feed_m17_alternating_windows(&opts, &state, 8);
    assert(state.m17_pre_candidate == 0);
    state.symbolcnt++;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_NONE);

    /* A preamble that runs as long as the rule asks still opens the LSF behind it. */
    m17_join_setup(&opts, &state);
    feed_m17_alternating_windows(&opts, &state, DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS);
    assert(state.m17_pre_candidate != 0);
    state.symbolcnt++;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_M17_LSF_POS);
}

/*
 * NXDN96's frame sync word is ten symbols in five variants per polarity, sliced on sign, so about one arbitrary window in
 * a hundred matches, and the matcher's two hits could be any distance apart: receiver noise opened 20 NXDN96 frames in
 * the noise_floor fixture's 10 s under -fn. An NXDN frame is 384 bits, 192 symbols at 9600 bps (NXDN TS 1-A 4.4: one
 * frame per 40 ms), and every frame opens with the FSW. The receiver rules (TS 1-A 5.3, Table 5.9-3) acquire on two
 * FSWs a frame apart and hold the lock through five missed FSWs. The 2400/4 matcher (NXDN48) keeps its two hits at
 * any distance.
 */
static void
nxdn96_sync_setup(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    opts->frame_nxdn96 = 1;
    opts->msize = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->min = -3.0f;
    state->max = 3.0f;
    state->symbolcnt = 5000;
}

/* Four-level readings of the NXDN FSW (TS 1-A 4.4.4): clean, and with every magnitude swapped -- the signs right and
 * nothing else, which is what a window of noise matching on signs reads as. */
static const float k_nxdn_fsw_clean[10] = {-3.0f, 1.0f, -3.0f, 3.0f, -3.0f, -3.0f, 3.0f, 3.0f, -1.0f, 3.0f};
static const float k_nxdn_fsw_swapped[10] = {-1.0f, 3.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -3.0f, 1.0f};

/* One NXDN96 FSW at the current count, read on four levels (@p swapped: the noise-like reading) at @p polarity, and,
 * when the matcher takes it, its frame's LICH reported as nxdn_frame() reports it, eight symbols on. */
static int
nxdn96_fsw(dsd_opts* opts, dsd_state* state, int polarity, int swapped, dsd_nxdn96_lich_result lich) {
    float levels[10];
    for (int i = 0; i < 10; i++) {
        levels[i] = (float)polarity * (swapped ? k_nxdn_fsw_swapped[i] : k_nxdn_fsw_clean[i]);
    }
    const int sync = dsd_frame_sync_test_eval_window(opts, state, polarity > 0 ? NXDN_FSW : INV_NXDN_FSW, levels, 10);
    if (DSD_SYNC_IS_NXDN(sync)) {
        dsd_nxdn96_sync_note_lich(&state->nxdn96_sync, (uint32_t)state->symbolcnt + 8U, lich);
    }
    return sync;
}

static void
test_nxdn96_follows_the_receiver_sync_rules(void) {
    static dsd_opts opts;
    static dsd_state state;
    const dsd_nxdn96_lich_result good = DSD_NXDN96_LICH_GOOD;

    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    /* A second hit that is not a frame later only marks a new place (N1 = 2: two FSWs a frame apart). */
    state.symbolcnt += 100;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    /* A transmission's frames follow on. */
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    /* A locked transmission rides out missed FSWs: five in a row (N7), each next one a symbol off the frame grid. */
    state.symbolcnt += 6U * 192U + 1U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    state.symbolcnt += 2U * 192U - 1U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    /* The next frame's FSW is held to its signs, as ever; one after a missed frame to N7's two bit errors as well. */
    state.symbolcnt += 192U;
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NXDN_POS);
    state.symbolcnt += 2U * 192U;
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NONE);
    /* Six missed lose the lock, and so does a hit two symbols off, or one of the other polarity. */
    state.symbolcnt += 5U * 192U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192U + 2U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192U;
    assert(nxdn96_fsw(&opts, &state, -1, 0, good) == DSD_SYNC_NONE);

    /* Acquisition takes the second FSW within a symbol of a frame on, either way, at either polarity. */
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 193;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, -1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 191;
    assert(nxdn96_fsw(&opts, &state, -1, 0, good) == DSD_SYNC_NXDN_NEG);

    /* Payload hits in between, of either polarity, do not hide the FSW a frame back. */
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 40;
    assert(nxdn96_fsw(&opts, &state, -1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 60;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 92;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    /* A hit a frame after one of the other polarity is not that frame's FSW. */
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, -1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);

    /* One FSW behind the preamble's tail (N6) does not acquire alone: see frame_sync_nxdn96_accepts(). */
    nxdn96_sync_setup(&opts, &state);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, "11111313113331" NXDN_FSW, 24) == DSD_SYNC_NONE);
}

/*
 * The lock follows the LICH as TS 1-A 5.3 has it: the LICH conditions apply only when the LICH's parity holds, and
 * frame sync is lost after S1 = 5 good-parity LICHs in a row that name no frame of the channel (5.3.2, Table 5.9-3).
 * A real call's frame whose LICH fails its parity does not drop the lock; an accept on noise, whose LICH fails, does
 * not refresh it either, so a lock still lapses N7 frames after the call.
 */
static void
test_nxdn96_lock_follows_the_lich(void) {
    static dsd_opts opts;
    static dsd_state state;
    const dsd_nxdn96_lich_result good = DSD_NXDN96_LICH_GOOD;
    const dsd_nxdn96_lich_result parity = DSD_NXDN96_LICH_PARITY_ERROR;
    const dsd_nxdn96_lich_result wrong = DSD_NXDN96_LICH_WRONG;

    /* A call: acquired, then a frame whose LICH parity fails. The next frame is still taken. */
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, parity) == DSD_SYNC_NXDN_POS);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);

    /* After the call: an accept whose LICH fails is not the call, and the lock keeps the call's last good frame. A hit
     * seven frames after that frame is past N7, though it is only four after the failed accept. */
    const uint32_t last_good = state.symbolcnt;
    state.symbolcnt = last_good + 3U * 192U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, parity) == DSD_SYNC_NXDN_POS);
    state.symbolcnt = last_good + 7U * 192U;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);

    /* Four good-parity LICHs naming something else are borne; the fifth in a row loses frame sync (S1). */
    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    for (int i = 0; i < 4; i++) {
        state.symbolcnt += 192;
        assert(nxdn96_fsw(&opts, &state, 1, 0, wrong) == DSD_SYNC_NXDN_POS);
    }
    assert(state.nxdn96_sync.lock_type == DSD_SYNC_NXDN_POS);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, wrong) == DSD_SYNC_NXDN_POS);
    assert(state.nxdn96_sync.lock_type == 0U);
    /* ... and the hits with it: the next FSW acquires afresh. */
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NONE);
}

/*
 * The FSW the specification detects is 20 bits, 10 four-level symbols, within two bit errors; the sign-sliced window
 * here is ten bits, which one arbitrary window in a hundred matches. Acquisition reads the window's levels too: hits
 * whose levels name the wrong magnitudes -- what receiver noise that happens to have the signs offers -- do not
 * acquire, while a clean FSW does.
 */
static void
test_nxdn96_acquires_on_the_four_level_fsw(void) {
    static dsd_opts opts;
    static dsd_state state;
    const dsd_nxdn96_lich_result good = DSD_NXDN96_LICH_GOOD;

    nxdn96_sync_setup(&opts, &state);
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NONE);
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 0, good) == DSD_SYNC_NXDN_POS);
    /* Once locked, the next frame is timed by the lock, as the receiver's flywheel does. */
    state.symbolcnt += 192;
    assert(nxdn96_fsw(&opts, &state, 1, 1, good) == DSD_SYNC_NXDN_POS);
}

/* One NXDN96 frame: the FSW, the LICH of a trunked outbound data frame (0101111, scrambled), and payload. Its LICH puts
 * the exact M17 packet word in the window ending at the frame's 15th symbol (review of the M17/NXDN96 joins). */
static void
nxdn96_udch_frame(char* frame, int fsw_lost, int inverted) {
    static const char head[] = NXDN_FSW "13333113";
    DSD_MEMCPY(frame, head, 18);
    if (fsw_lost) {
        frame[0] = '1';
        frame[1] = '1';
    }
    for (int i = 18; i < 192; i++) {
        frame[i] = "3311"[(i - 18) % 4];
    }
    if (inverted) {
        for (int i = 0; i < 192; i++) {
            frame[i] = frame[i] == '1' ? '3' : '1';
        }
    }
}

/* Read @p count symbols the way getFrameSync() does, a window of up to 24 behind each; an accepted sync hands its frame
 * to the handler, which reads the rest of it, and the search starts again with an empty window. */
static void
read_symbol_stream(dsd_opts* opts, dsd_state* state, const char* symbols, int count, int* nxdn, int* m17) {
    char window[25];
    int filled = 0;
    for (int i = 0; i < count; i++) {
        state->symbolcnt++;
        if (filled == 24) {
            DSD_MEMMOVE(window, window + 1, 23);
            filled = 23;
        }
        window[filled++] = symbols[i];
        window[filled] = '\0';
        const int sync = dsd_frame_sync_test_try_protocol_matches(opts, state, window, filled);
        int consumed = 0;
        if (DSD_SYNC_IS_NXDN(sync)) {
            (*nxdn)++;
            consumed = 182;
            dsd_nxdn96_sync_note_lich(&state->nxdn96_sync, (uint32_t)state->symbolcnt + 8U, DSD_NXDN96_LICH_GOOD);
        } else if (sync != DSD_SYNC_NONE) {
            (*m17)++;
            consumed = 184;
        }
        if (consumed > 0) {
            const int left = count - 1 - i;
            const int skip = consumed < left ? consumed : left;
            state->symbolcnt += (uint32_t)skip;
            i += skip;
            filled = 0;
        }
    }
}

/*
 * A trunked NXDN96 data call carries an exact M17 packet word in every frame's LICH, 192 symbols apart. Under Auto, one
 * lost FSW used to give that word three frames running while NXDN96 waited out its own acquisition, and the M17 join
 * took the frame -- reading on into the next FSW, so NXDN96 never got its window back, while the join re-took every
 * frame from its own hits. The call stays NXDN96: the lock rides out the lost FSW, and M17 joins no packet.
 */
static void
test_an_nxdn96_data_call_stays_nxdn96_under_auto(void) {
    static dsd_opts opts;
    static dsd_state state;
    static char stream[12 * 192];
    for (int f = 0; f < 12; f++) {
        nxdn96_udch_frame(stream + f * 192, f == 5, 0);
    }

    reset(&opts, &state);
    opts.frame_nxdn96 = 1;
    opts.frame_m17 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 9000;
    int nxdn = 0;
    int m17 = 0;
    read_symbol_stream(&opts, &state, stream, (int)sizeof(stream), &nxdn, &m17);
    assert(m17 == 0);
    assert(nxdn == 10); /* every frame but the first, which only marks, and the one whose FSW was lost */

    /* With NXDN96 off the frames are M17's to judge alone, and their packet word joins nothing. */
    reset(&opts, &state);
    opts.frame_m17 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 9000;
    nxdn = 0;
    m17 = 0;
    read_symbol_stream(&opts, &state, stream, (int)sizeof(stream), &nxdn, &m17);
    assert(m17 == 0 && nxdn == 0);

    /* Inverted, the same LICH holds the exact BERT word instead, which M17 does join. The call stays NXDN96 under Auto
     * all the same: the lock rides out the lost FSW, so M17 never sees the word three frames running. */
    for (int f = 0; f < 12; f++) {
        nxdn96_udch_frame(stream + f * 192, f == 5, 1);
    }
    reset(&opts, &state);
    opts.frame_nxdn96 = 1;
    opts.frame_m17 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 9000;
    nxdn = 0;
    m17 = 0;
    read_symbol_stream(&opts, &state, stream, (int)sizeof(stream), &nxdn, &m17);
    assert(m17 == 0);
    assert(nxdn == 10);
}

static void
test_nxdn48_keeps_two_hits_at_any_distance(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* NXDN48 on 2400/4 is unchanged. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.symbolcnt = 5000;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    state.symbolcnt += 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

/* Issue #445. The spans above cover a frame; this covers the gap a hunt step opens. A proof of
 * 2400/4 says a transmission is running there, and a step onto 4800/4 offers that same signal,
 * read at twice its rate, to two matchers weak enough to take it. Armed by hand here: what the
 * arming site records is pinned by test_a_proven_verdict_stamps_the_profile_it_was_read_on(). */
static void
arm_2400_4_proof(dsd_state* state, uint32_t at_symbolcnt) {
    const int hunt_idx = state->sps_hunt_idx;
    const uint32_t symbolcnt = state->symbolcnt;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state->symbolcnt = at_symbolcnt;
    dsd_frame_sync_note_profile_proof(state);
    state->sps_hunt_idx = hunt_idx;
    state->symbolcnt = symbolcnt;
}

static void
test_a_proven_2400_4_transmission_stands_down_the_4800_4_matchers(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* NXDN96: refused, and -- as inside a P25p1 frame -- with no claim on lastsynctype. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    arm_2400_4_proof(&state, 1000);

    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);
    state.symbolcnt = 1000 + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS - 1u;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);

    /* Once the transmission can no longer be running, the matcher is live again on its own
     * two-hit terms: the second hit a frame after the first. */
    state.symbolcnt = 1000 + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);

    /* M17: the run latches nothing and the word behind it is refused, so no candidate is left
     * to spend the moment the span lapses. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_m17 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    arm_2400_4_proof(&state, 1000);

    state.symbolcnt = 1000 + 100;
    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(state.m17_pre_candidate == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_NONE);

    state.symbolcnt = 1000 + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS;
    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(state.m17_pre_candidate != 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_M17_LSF_POS);
}

/* Withholding the sync must not withhold the level estimate, for the same reason as inside a
 * P25p1 frame: the blend is the wideband reference the other protocols on 4800/4 lean on. */
static void
test_the_2400_4_proof_suppression_still_blends_levels(void) {
    static dsd_opts opts;
    static dsd_state state;
    float fsw_levels[10];

    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.msize = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 10; i++) {
        fsw_levels[i] = NXDN_FSW[i] == '3' ? -3.0f : 3.0f;
    }
    arm_2400_4_proof(&state, 0);
    state.symbolcnt = 100;
    state.max = 9.0f;
    state.min = -9.0f;

    assert(dsd_frame_sync_test_eval_window(&opts, &state, NXDN_FSW, fsw_levels, 10) == DSD_SYNC_NONE);
    assert(fabsf(state.max - 6.0f) < 0.0001f);
    assert(fabsf(state.min - (-6.0f)) < 0.0001f);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);
}

/* The guard is scoped three ways: to the profile it protects against, to the profile it was
 * proved on, and to builds carrying a protocol that can prove 2400/4 at all. */
static void
test_the_2400_4_proof_suppression_is_scoped_to_its_arming_protocols(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* Neither 2400/4 protocol enabled: nothing could have proved that profile, so a stamp on
     * it says nothing and NXDN96 syncs as before. */
    reset(&opts, &state);
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    arm_2400_4_proof(&state, 1000);
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS; /* NXDN96's second hit is a frame on */
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);

    /* A proof read on 4800/4 -- P25p1's, say -- is inert here: what this guard acts on is a
     * transmission proved somewhere the hunt has since stepped away from. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.symbolcnt = 1000;
    dsd_frame_sync_note_profile_proof(&state);
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    state.symbolcnt += DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS; /* NXDN96's second hit is a frame on */
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);

    /* And it never mutes the protocol whose transmission armed it: on 2400/4 itself, NXDN48
     * syncs throughout on its own terms. */
    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.min = -3.0f;
    state.max = 3.0f;
    arm_2400_4_proof(&state, 1000);
    state.symbolcnt = 1000 + 100;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NXDN_POS);
}

/* The stamp wraps with symbolcnt and is compared as a modular difference. */
static void
test_the_2400_4_proof_suppression_survives_symbolcnt_rollover(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    arm_2400_4_proof(&state, UINT32_MAX - 100u);

    state.symbolcnt = 49u;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype != DSD_SYNC_NXDN_POS);

    state.symbolcnt = (uint32_t)(UINT32_MAX - 100u + DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, NXDN_FSW, 10) == DSD_SYNC_NONE);
    assert(state.lastsynctype == DSD_SYNC_NXDN_POS);
}

/* A preamble is an alternating symbol run, which any 4800-baud signal can present, so it is
 * latched as a candidate rather than returned as a sync; the sync word that must follow it is
 * what the decoder acts on (#399). */
static void
test_m17_candidate_chains_to_lsf_under_auto(void) {
    static dsd_opts opts;
    static dsd_state state;
    char one_error_preamble[9];
    DSD_MEMCPY(one_error_preamble, M17_PRE, sizeof(one_error_preamble));
    one_error_preamble[0] = one_error_preamble[0] == '1' ? '3' : '1';

    /* The full AUTO candidate set -- the configuration that used to reject every real M17
     * preamble on the air while accepting D-STAR's bit sync. */
    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_ysf = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;

    feed_m17_preamble_run(&opts, &state, one_error_preamble);
    assert(state.m17_pre_candidate == 1);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_M17_LSF_POS);
    assert(state.m17_pre_candidate == 0);
    assert(state.m17_polarity == 1);

    /* An inverted run carries its LSF as the stream pattern. */
    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_ysf = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    feed_m17_preamble_run(&opts, &state, M17_PIV);
    assert(state.m17_pre_candidate == 2);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_STR, 8) == DSD_SYNC_M17_LSF_NEG);
    assert(state.m17_polarity == 2);

    /* A BERT frame confirms a candidate too. */
    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;
    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_BRT, 8) == DSD_SYNC_M17_BRT_POS);
}

/* The candidate is only good for the sync word that should arrive right behind the preamble
 * run; left standing it would let an unrelated window minutes later open an M17 frame. */
static void
test_m17_candidate_expires_without_a_following_sync(void) {
    static const char neutral_window[] = "00000000";
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.min = -3.0f;
    state.max = 3.0f;

    feed_m17_preamble_run(&opts, &state, M17_PRE);
    assert(state.m17_pre_candidate == 1);
    for (int i = 0; i < DSD_FRAME_SYNC_M17_CANDIDATE_TTL; i++) {
        assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, neutral_window, 8) == DSD_SYNC_NONE);
    }
    assert(state.m17_pre_candidate == 0);
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, M17_LSF, 8) == DSD_SYNC_NONE);
}

static void
test_short_m17_window_estimates_levels_without_warm_start_history(void) {
    static dsd_opts opts;
    static dsd_state state;
    float preamble_levels[8];
    float lsf_levels[8];

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.msize = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 8; i++) {
        preamble_levels[i] = M17_PRE[i] == '3' ? -3.0f : 3.0f;
        lsf_levels[i] = M17_LSF[i] == '3' ? -3.0f : 3.0f;
    }

    assert(state.symbol_history == NULL);
    /* The run is where M17's levels come from: an alternating pair of outer rails is the best
     * reference the protocol offers, and the sync word behind it cannot be sliced without it. */
    for (int i = 0; i < DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS; i++) {
        assert(dsd_frame_sync_test_eval_window(&opts, &state, M17_PRE, preamble_levels, 8) == DSD_SYNC_NONE);
    }
    assert(fabsf(state.min - (-1.5f)) < 0.0001f);
    assert(fabsf(state.max - 1.5f) < 0.0001f);
    assert(dsd_frame_sync_test_eval_window(&opts, &state, M17_LSF, lsf_levels, 8) == DSD_SYNC_M17_LSF_POS);
}

/* D-STAR's bit sync opens with an exact M17 marker and presents doubled alternating runs, which
 * is what used to be accepted as an M17 preamble on captures containing no M17 at all. */
static void
test_m17_alternating_runs_alone_are_never_a_sync(void) {
    static const char* const dstar_patterns[] = {DSTAR_SYNC, INV_DSTAR_SYNC, DSTAR_HD, INV_DSTAR_HD};
    static const char repeated_pre[] = M17_PRE M17_PRE;
    static const char repeated_piv[] = M17_PIV M17_PIV;
    static dsd_opts opts;
    static dsd_state state;

    for (size_t pattern_index = 0; pattern_index < sizeof(dstar_patterns) / sizeof(dstar_patterns[0]);
         pattern_index++) {
        for (int symbol_count = 8; symbol_count <= 24; symbol_count++) {
            reset(&opts, &state);
            opts.frame_m17 = 1;
            opts.frame_dstar = 1;
            state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
            assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, dstar_patterns[pattern_index], symbol_count)
                   == DSD_SYNC_NONE);
        }
    }

    /* Doubling the marker proves nothing about it: a repeated alternating run is still an
     * alternating run, so it stays a candidate and produces no sync of its own. */
    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, repeated_pre, 16) == DSD_SYNC_NONE);
    assert(state.m17_pre_candidate == 0);

    reset(&opts, &state);
    opts.frame_m17 = 1;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, repeated_piv, 16) == DSD_SYNC_NONE);
    assert(state.m17_pre_candidate == 0);
}

static void
test_elapsed_seconds_prefers_monotonic_then_wall_time(void) {
    assert(fabs(frame_sync_elapsed_seconds(12.5, (time_t)20, 10.0, (time_t)3) - 2.5) < 0.000001);
    assert(fabs(frame_sync_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)3) - 17.0) < 0.000001);
    assert(frame_sync_elapsed_seconds(12.5, (time_t)20, 0.0, (time_t)0) > 1.0e8);
}

/* A slot is active while its MAC signalling is fresh, or while its audio gate is open until the voice sync is a
   hangtime old. What the output still holds counts for nothing (issue #651: the P25 Phase 2 playout drains it). */
static void
test_p25_slot_activity_honors_gate_mac_and_hangtime(void) {
    static dsd_opts opts;
    static dsd_state state;
    int left_active = 0;
    int right_active = 0;

    reset(&opts, &state);
    opts.trunk_hangtime = 2.0f;
    state.p25_p2_last_mac_active_m[0] = 99.8;
    state.p25_p2_last_mac_active_m[1] = 95.0;
    state.p25_p2_audio_allowed[1] = 1;
    frame_sync_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 1.0, &left_active, &right_active);
    assert(left_active == 1);
    assert(right_active == 1);

    left_active = 0;
    right_active = 0;
    frame_sync_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 2.0, &left_active, &right_active);
    assert(left_active == 1);
    assert(right_active == 0);

    state.p25_p2_last_mac_active_m[0] = 90.0;
    left_active = 1;
    frame_sync_p25_slot_activity(&opts, &state, (time_t)100, 100.0, 0.75, 1.0, &left_active, &right_active);
    assert(left_active == 0);
}

static void
test_hamming_helpers_find_best_patterns(void) {
    const char* patterns[] = {"012301", "333333", "111111"};

    assert(frame_sync_hamming_distance_pattern("012301", "012301", 6) == 0);
    assert(frame_sync_hamming_distance_pattern("012301", "012300", 6) == 1);
    assert(frame_sync_best_ham_for_patterns("111101", patterns, 3, 6, 6) == 1);
    assert(frame_sync_best_ham_for_patterns("222222", patterns, 3, 6, 3) == 3);
    assert(frame_sync_best_nxdn_scaled_ham("3131331131", 24) == 0);
    assert(frame_sync_best_nxdn_scaled_ham("1313113300", 24) == 5);
}

#ifdef USE_RADIO
static double g_snr_c4fm = -100.0;
static double g_snr_c4fm_eye = -100.0;
static double g_snr_cqpsk = -100.0;
static double g_snr_gfsk = -100.0;
static double g_snr_qpsk_const = -100.0;
static int g_frame_sync_tick_calls = 0;
static int g_profile_set_calls = 0;
static int g_profile_rate = 0;
static int g_profile_levels = 0;
static int g_profile_channel = 0;
static int g_profile_cqpsk = 0;
static int g_profile_ted_sps = 0;

static double
fake_snr_c4fm_db(void) {
    return g_snr_c4fm;
}

static double
fake_snr_c4fm_eye_db(void) {
    return g_snr_c4fm_eye;
}

static double
fake_snr_cqpsk_db(void) {
    return g_snr_cqpsk;
}

static double
fake_snr_gfsk_db(void) {
    return g_snr_gfsk;
}

static double
fake_snr_qpsk_const_db(void) {
    return g_snr_qpsk_const;
}

static unsigned int
fake_output_rate_hz(void) {
    return 48000U;
}

static int
fake_apply_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile, int ted_sps) {
    g_profile_set_calls++;
    g_profile_cqpsk = cqpsk_enable;
    g_profile_rate = symbol_rate_hz;
    g_profile_levels = levels;
    g_profile_channel = channel_profile;
    g_profile_ted_sps = ted_sps;
    return 0;
}

static void
reset_fake_profile_capture(void) {
    g_profile_set_calls = 0;
    g_profile_cqpsk = -1;
    g_profile_rate = 0;
    g_profile_levels = 0;
    g_profile_channel = 0;
    g_profile_ted_sps = 0;
}

static void
fake_p25_sm_try_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_frame_sync_tick_calls++;
}

static void
set_fake_snr(double c4fm, double c4fm_eye, double cqpsk, double qpsk_const) {
    g_snr_c4fm = c4fm;
    g_snr_c4fm_eye = c4fm_eye;
    g_snr_cqpsk = cqpsk;
    g_snr_gfsk = -100.0;
    g_snr_qpsk_const = qpsk_const;
}

static void
install_fake_snr_hooks(void) {
    dsd_rtl_stream_metrics_hooks hooks = {
        .snr_c4fm_db = fake_snr_c4fm_db,
        .snr_c4fm_eye_db = fake_snr_c4fm_eye_db,
        .snr_cqpsk_db = fake_snr_cqpsk_db,
        .snr_gfsk_db = fake_snr_gfsk_db,
        .snr_qpsk_const_db = fake_snr_qpsk_const_db,
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

static void
test_rtl_symbol_profile_selection(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.frame_dstar = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 4) == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);

    reset(&opts, &state);
    opts.frame_provoice = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 2) == DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE);

    reset(&opts, &state);
    opts.frame_dstar = 1;
    opts.frame_dmr = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 3) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    reset(&opts, &state);
    opts.frame_x2tdma = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 3) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_dmr = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.rf_mod = 0;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    state.rf_mod = 1;
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 0) == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    reset(&opts, &state);
    assert(dsd_frame_sync_test_rtl_profile_for_sps_index(&opts, &state, 1) == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);
}

/*
 * The decode-mode layer and the SPS hunt each name a symbol rate and a level
 * count, and the two have to be the same numbers: the mode picks the profile the
 * hunt then searches from, so a mode claiming 2400/4 while its chosen index means
 * 4800/4 puts the decoder on one symbol clock and the hunt on another.
 *
 * The index also has to be one that can match the mode at all —
 * frame_sync_sps_profile_has_candidate() offers each profile only to the modes it
 * carries, so a mode pointed at a profile that never accepts it hunts forever.
 */
static void
test_decode_mode_profiles_agree_with_the_sps_hunt_table(void) {
    static const dsdneoUserDecodeMode modes[] = {
        DSDCFG_MODE_AUTO, DSDCFG_MODE_DSTAR,    DSDCFG_MODE_X2TDMA,   DSDCFG_MODE_P25P1,  DSDCFG_MODE_P25P2,
        DSDCFG_MODE_DMR,  DSDCFG_MODE_DMR_MONO, DSDCFG_MODE_NXDN48,   DSDCFG_MODE_NXDN96, DSDCFG_MODE_DPMR,
        DSDCFG_MODE_YSF,  DSDCFG_MODE_M17,      DSDCFG_MODE_EDACS_PV, DSDCFG_MODE_TDMA,   DSDCFG_MODE_ANALOG,
    };

    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        const dsd_decode_mode_profile profile = dsd_decode_mode_profile_for(modes[i]);
        const int index = (int)profile.sps_profile_index;
        assert(index >= 0 && index < dsd_frame_sync_test_sps_hunt_profile_count());
        assert(profile.symbol_rate_hz == dsd_frame_sync_test_sps_hunt_profile_rate(index));
        assert(profile.levels == dsd_frame_sync_test_sps_hunt_profile_levels(index));

        /* Checked against what the preset actually writes, because that is what
         * the hunt and the UI both read back. */
        static dsd_opts opts;
        static dsd_state state;
        reset(&opts, &state);
        assert(dsd_apply_decode_mode_preset(modes[i], DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) == 0);

        /* The half the rate/levels comparison cannot see: an index whose row
         * happens to carry the right numbers is still useless if the hunt never
         * offers it to this mode's frame set. Analog is the one mode with no
         * frame set at all and so no candidate anywhere. */
        if (modes[i] != DSDCFG_MODE_ANALOG) {
            assert(dsd_frame_sync_test_sps_hunt_profile_has_candidate(&opts, index) == 1);
        }

        /* The modulation half of the same agreement. A two-level profile runs
         * GFSK and nothing else, so a preset naming C4FM or QPSK on one is
         * already a pass behind the demodulator: frame_sync_apply_sps_hunt_profile()
         * normalises rf_mod to GFSK the moment the hunt lands there and never
         * touches opts->mod_*, which is what the UI's modulation control binds
         * to -- and nothing afterwards reconciles the pair. Both readings are
         * asserted because they are separately reachable and it is exactly their
         * disagreement that is the defect. */
        assert(state.rf_mod == dsd_frame_sync_profile_modulation(profile.levels, state.rf_mod));
        assert(dsd_opts_modulation(&opts) == state.rf_mod);
    }

    /* The two NXDN variants are the pair this is most likely to be got wrong on:
     * they share a preset symbol timing but not a symbol rate. */
    assert(dsd_decode_mode_profile_for(DSDCFG_MODE_NXDN48).symbol_rate_hz == 2400);
    assert(dsd_decode_mode_profile_for(DSDCFG_MODE_NXDN96).symbol_rate_hz == 4800);
    const dsd_decode_mode_profile tetra = dsd_decode_mode_profile_for(DSDCFG_MODE_TETRA);
    assert(tetra.symbol_rate_hz == 18000);
    assert(tetra.levels == 4);
    assert(tetra.sps_profile_index == DSD_FRAME_SYNC_SPS_PROFILE_18000_4);
    assert(dsd_rtl_channel_profile_for(NULL, tetra.symbol_rate_hz, tetra.levels, 1)
           == DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE);

    /* AUTO's whole promise is that the hunt may visit every profile, and a config file selects
     * the same AUTO the command line does. The profile argument decides audio layout, never the
     * decoder set, so every candidate must be reachable from the config path too. TETRA is the
     * exception: it runs on its own 72 kHz baseband, so only -fT selects its profile. */
    for (int profile_index = 0; profile_index < dsd_frame_sync_test_sps_hunt_profile_count(); profile_index++) {
        static dsd_opts opts;
        static dsd_state state;
        reset(&opts, &state);
        assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CONFIG, &opts, &state) == 0);
        const int want = profile_index == DSD_FRAME_SYNC_SPS_PROFILE_18000_4 ? 0 : 1;
        assert(dsd_frame_sync_test_sps_hunt_profile_has_candidate(&opts, profile_index) == want);
    }
}

static void
test_rtl_p25p2_timing_reconciliation_preserves_cqpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p2 = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.samplesPerSymbol = dsd_opts_compute_sps_rate(&opts, 6000, 48000);
    state.symbolCenter = dsd_opts_symbol_center(state.samplesPerSymbol);
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    frame_sync_ensure_enabled_sps_profile(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_rate == 6000);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_ted_sps == 8);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * Issue #423: a P25p1 FDMA signal that has decoded through the CQPSK chain must keep it across
 * a hunt rotation. Without the learned modulation the hunt normalises the 4800/4-level profile
 * back to C4FM every time it comes back round, so an LSM control channel loses the chain it
 * just decoded on.
 */
static void
test_sps_hunt_restores_learned_p25p1_cqpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    /* Rotating back into the P25p1 profile restores the learned CQPSK instead of C4FM. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.rf_mod == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    /* The restored modulation gets its dwell back, or the votes evict it before the first
     * sync of the new dwell can arrive. */
    assert(dsd_frame_sync_test_qpsk_dwell_armed());
    dsd_rtl_stream_metrics_hooks_set(NULL);

    /* Never validated, and validated through C4FM, both keep the C4FM default. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    state.p25_p1_validated_rf_mod = -1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.rf_mod == 0);

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    state.p25_p1_validated_rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.rf_mod == 0);

    /* A learned CQPSK never overrides a binary profile: those force GFSK. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_2, 0);
    assert(state.rf_mod == 2);

    /* The learned value is not consulted for a profile P25p1 does not own. */
    reset(&opts, &state);
    opts.frame_p25p2 = 1;
    opts.frame_provoice = 1;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_6000_4, 0);
    assert(state.rf_mod == 0);

    /* An explicit CLI modulation lock keeps its demodulator regardless. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    opts.mod_cli_lock = 1;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    frame_sync_apply_sps_hunt_profile(&opts, &state, DSD_FRAME_SYNC_SPS_PROFILE_4800_4, 0);
    assert(state.rf_mod == 2);
}

/*
 * Issue #423: P25p1 cannot reach CQPSK by measurement -- while the chain is off the QPSK SNR
 * estimate comes from raw un-derotated IQ and the sync-hamming candidate can only tie -- so the
 * hunt tries it on alternate visits to the profile instead.
 */
/* Pin the acquisition schedule in symbol time: a default target visit must
 * include a CQPSK trial even though the row enables both P25 phases. */
static void
test_trunk_scan_p25_probe_fits_visit(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    const dsd_rtl_stream_metrics_hooks hooks = {.output_rate_hz = fake_output_rate_hz,
                                                .apply_demod_profile = fake_apply_demod_profile};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_scan_enabled = 1;
    opts->trunk_enable = 1;
    state->rtl_ctx = (struct RtlSdrContext*)state;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    state->p25_p1_validated_rf_mod = -1;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(opts, state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    const double first_trial_ms = 1000.0 * dwell / 4800;
    assert(first_trial_ms < DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS);
    assert(2.0 * first_trial_ms < DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS);
    state->sps_hunt_counter = dwell - 1;
    assert(!frame_sync_no_sync_sps_hunt(opts, state));
    assert(state->rf_mod == 0);
    state->sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(opts, state));
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state->rf_mod == 1 && state->p25_p1_mod_probe_active);
    assert(g_profile_cqpsk == 1 && g_profile_rate == 4800);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    /* An unproductive CQPSK trial must still give Phase 2 its turn. */
    state->sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(opts, state));
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    dsd_scan_mode_leave(opts, state);
    dsd_state_ext_free_all(state);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    free(state);
    free(opts);
}

static void
test_p25p1_auto_hunt_alternates_cqpsk_probe(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.p25_p1_validated_rf_mod = -1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    /* P25p1 alone is the only candidate, so every expired dwell wraps to the same profile.
     * The first stays on C4FM, the next watches CQPSK, and it alternates from there. */
    state.sps_hunt_counter = dwell;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.rf_mod == 0);

    reset_fake_profile_capture();
    state.sps_hunt_counter = dwell;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.rf_mod == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(dsd_frame_sync_test_qpsk_dwell_armed());

    /* A probe that decoded nothing hands the profile back to C4FM. */
    state.sps_hunt_counter = dwell;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.rf_mod == 0);

    state.sps_hunt_counter = dwell;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.rf_mod == 1);
    dsd_rtl_stream_metrics_hooks_set(NULL);

    /* An explicit modulation lock is never probed against. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.mod_cli_lock = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 4; i++) {
        state.sps_hunt_counter = dwell;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        assert(state.rf_mod == 0);
    }

    /* Only the RTL front end can be retuned into the CQPSK chain. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 4; i++) {
        state.sps_hunt_counter = dwell;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        assert(state.rf_mod == 0);
    }

    /* Without P25p1 enabled there is nothing on this profile that wants CQPSK. */
    reset(&opts, &state);
    opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    for (int i = 0; i < 4; i++) {
        state.sps_hunt_counter = dwell;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        assert(state.rf_mod != 1);
    }

    /* A dwell that expired decoded nothing, so a GFSK profile-mate has no claim on the
     * demodulator either: the probe takes its turn from there too. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    state.p25_p1_mod_probe_next_qpsk = 1;
    state.sps_hunt_counter = dwell;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.rf_mod == 1);
    assert(state.p25_p1_mod_probe_active == 1);
}

/* Start a CQPSK probe on 4800/4 the way an expired dwell does, with @p configure choosing the decoders. */
static int
start_cqpsk_probe(dsd_opts* opts, dsd_state* state, void (*configure)(dsd_opts* opts)) {
    static int fake_rtl_context;
    reset(opts, state);
    opts->frame_p25p1 = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    configure(opts);
    state->rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->rf_mod = 0;
    state->p25_p1_validated_rf_mod = -1;
    state->p25_p1_mod_probe_next_qpsk = 1;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(opts, state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    state->sps_hunt_counter = dwell;
    (void)frame_sync_no_sync_sps_hunt(opts, state);
    assert(state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state->rf_mod == 1 && state->p25_p1_mod_probe_active == 1);
    return dwell;
}

static void
configure_p25p1_only(dsd_opts* opts) {
    (void)opts;
}

static void
configure_with_dmr(dsd_opts* opts) {
    opts->frame_dmr = 1;
}

static void
configure_with_nxdn96(dsd_opts* opts) {
    opts->frame_nxdn96 = 1;
}

static void
configure_with_ysf(dsd_opts* opts) {
    opts->frame_ysf = 1;
}

static void
configure_with_m17(dsd_opts* opts) {
    opts->frame_m17 = 1;
}

static void
configure_trunk_scan_target_with_dmr(dsd_opts* opts) {
    opts->frame_p25p2 = 1;
    opts->frame_dmr = 1;
    opts->trunk_scan_enabled = 1;
    opts->trunk_enable = 1;
}

/*
 * The CQPSK trial cannot hear DMR, NXDN96, YSF or M17, and it held the votes off for its whole dwell, so on a session
 * that decodes those too every other 4800/4 visit was deaf to them: a DMR carrier that arrived in one lost the dwell
 * and a whole rotation (faDMRNXDNScan, 121 DMR frames). A CQPSK carrier decodes on the chain within a frame, so a
 * trial that has validated nothing by half its dwell gives the rest of it back to C4FM and the votes.
 */
static void
test_p25p1_cqpsk_trial_leaves_half_the_dwell_to_fsk_modes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    void (*const with_fsk[])(dsd_opts*) = {configure_with_dmr, configure_with_nxdn96, configure_with_ysf,
                                           configure_with_m17};
    for (size_t i = 0; i < sizeof(with_fsk) / sizeof(with_fsk[0]); i++) {
        const int dwell = start_cqpsk_probe(&opts, &state, with_fsk[i]);
        state.sps_hunt_counter = dwell / 2 - 1;
        assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
        assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 1);
        reset_fake_profile_capture();
        state.sps_hunt_counter = dwell / 2;
        /* A step: the demodulator changed under the caller's sync window, which getFrameSync() leaves to rebuild it --
         * but not a no-sync exit. The dwell goes on, so the P25 SM's accounting and noCarrier() are not owed: running
         * them would end calls and reset the decoder's frame state halfway through a dwell that has not expired. */
        g_vc_no_sync_hook_calls = 0;
        g_no_carrier_hook_calls = 0;
        dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
            .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
            .no_carrier = fake_no_carrier,
        });
        assert(dsd_frame_sync_test_hunt_budget_exit(&opts, &state) == FRAME_SYNC_HUNT_CHAIN_HANDBACK);
        dsd_frame_sync_hooks_set(NULL);
        assert(g_no_carrier_hook_calls == 0);
        assert(g_vc_no_sync_hook_calls == 0);
        assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
        assert(state.rf_mod == 0 && state.p25_p1_mod_probe_active == 0);
        assert(g_profile_cqpsk == 0);
        /* The rest of the dwell is the profile's: the counter runs on, and the votes are free again. */
        assert(state.sps_hunt_counter == dwell / 2);
        if (opts.frame_dmr == 1 || opts.frame_nxdn96 == 1) {
            dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
            lastt = 24;
            frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
            assert(state.rf_mod == 2);
        }
        /* Handed back once: further calls before the dwell ends change nothing. */
        const int mod = state.rf_mod;
        state.sps_hunt_counter = dwell - 1;
        assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
        assert(state.rf_mod == mod);
    }

    /* P25p1 alone keeps the whole dwell on trial, as before. */
    int dwell = start_cqpsk_probe(&opts, &state, configure_p25p1_only);
    state.sps_hunt_counter = dwell - 1;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 1);

    /* So does a trunk-scan P25 target's trial, whose schedule fits the CQPSK trial and Phase 2 in one visit. */
    dwell = start_cqpsk_probe(&opts, &state, configure_trunk_scan_target_with_dmr);
    state.sps_hunt_counter = dwell - 1;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 1);

    /* And a CQPSK dwell that is measuring a chain comparison rather than probing. */
    dwell = start_cqpsk_probe(&opts, &state, configure_with_dmr);
    state.p25_p1_mod_compare.phase = 2;
    state.sps_hunt_counter = dwell - 1;
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 0);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * A trial the votes can revoke before it decodes anything is not a trial: entering GFSK takes
 * a single vote. While a probe is on trial the modulation heuristics do not get to answer.
 */
static void
test_p25p1_probe_survives_its_dwell(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    int lastt = 24;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    state.p25_p1_mod_probe_active = 1;

    /* A GFSK answer would normally take the demodulator on its first vote. */
    set_fake_snr(-100.0, 20.0, -100.0, -100.0);
    install_fake_snr_hooks();
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    /* Once the trial is over the heuristics are free again. */
    state.p25_p1_mod_probe_active = 0;
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 2);
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * A validated P25p1 frame is direct evidence of what the signal is; the SNR and hamming
 * candidates are guesses. While such frames keep arriving the guesses do not get to move the
 * demodulator -- and once they stop, they do.
 */
static void
test_validated_p25p1_modulation_outranks_the_heuristics(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    int lastt = 24;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    state.symbolcnt = 100000;
    state.p25_p1_validated_rf_mod = 1;
    state.p25_p1_validated_symbolcnt = state.symbolcnt;

    set_fake_snr(-100.0, 20.0, -100.0, -100.0);
    install_fake_snr_hooks();
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    /* Frames stopped long enough ago that the hold has lapsed. */
    state.symbolcnt = state.p25_p1_validated_symbolcnt + DSD_FRAME_SYNC_P25P1_VALIDATED_HOLD_SYMBOLS;
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 2);

    /* The hold speaks only for a chain that actually validated a frame: a C4FM claim says
     * nothing about the CQPSK chain the demodulator happens to be on. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    state.symbolcnt = 100000;
    state.p25_p1_validated_rf_mod = 0;
    state.p25_p1_validated_symbolcnt = state.symbolcnt;
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 2);

    /* And a zeroed state claims nothing at all. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    dsd_frame_sync_test_set_recent_hamming(24, 24, 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 2);
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * A claim that CQPSK works survives the hunt's tour of the other protocols' profiles, so a
 * control channel does not have to re-earn its demodulator every time the hunt looks
 * elsewhere. But it is a claim about a signal, not a permanent setting: a dwell that expires
 * on the CQPSK chain having decoded nothing withdraws it.
 */
static void
test_validated_cqpsk_survives_rotation_but_not_disproof(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    const int dwell = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;

    /* The first expired dwell ran on the CQPSK chain and decoded nothing, so the claim goes. */
    state.sps_hunt_counter = dwell;
    frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.p25_p1_validated_rf_mod != 1);

    /* A claim made while the hunt is elsewhere is not disproved by that profile's dwell, and
     * the profile comes back up on CQPSK when the rotation returns to it. */
    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.frame_dstar = 1;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.p25_p1_validated_rf_mod = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    for (int i = 0; i < 6; i++) {
        state.sps_hunt_counter = dwell;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        if (state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4) {
            assert(state.rf_mod == 1);
            assert(state.p25_p1_validated_rf_mod == 1);
            break;
        }
        assert(state.p25_p1_validated_rf_mod == 1);
    }
}

/* A P25p1 frame whose NID decoded on the chain the demodulator is on: getFrameSync() searched @p search symbols and
 * returned on its sync, the handler read @p read, and getFrameSync() is entered again. */
static void
compare_frame(dsd_opts* opts, dsd_state* state, uint32_t search, uint32_t read) {
    state->symbolcnt += search;
    state->sps_hunt_symbolcnt_mark = state->symbolcnt;
    state->symbolcnt += read;
    state->p25_p1_validated_rf_mod = state->rf_mod;
    state->p25_p1_validated_symbolcnt = state->symbolcnt;
    frame_sync_p25p1_mod_compare_note(opts, state);
}

/* A no-sync return after @p symbols of search, and the entry after it. */
static void
compare_idle(dsd_opts* opts, dsd_state* state, uint32_t symbols) {
    state->symbolcnt += symbols;
    state->sps_hunt_symbolcnt_mark = state->symbolcnt;
    frame_sync_p25p1_mod_compare_note(opts, state);
}

static void
compare_setup(dsd_opts* opts, dsd_state* state, int incumbent_rf_mod) {
    static int fake_rtl_context;
    reset(opts, state);
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_dmr = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->rf_mod = incumbent_rf_mod;
    state->p25_p1_validated_rf_mod = -1;
    state->p25_p2_active_slot = -1;
    state->symbolcnt = 50000U;
    reset_fake_profile_capture();
}

/* One search pass of a control channel's one-block TSDUs on the current chain, from the first frame's sync: every
 * slot (134 symbols read, 46 searched) when @p every is 1, one slot in @p every otherwise. */
static void
compare_pass(dsd_opts* opts, dsd_state* state, int every) {
    const uint32_t slot = 180U;
    uint32_t used = 0;
    int frames = 0;
    while (used + slot * (uint32_t)every <= (uint32_t)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS) {
        const uint32_t search = frames == 0 ? 0U : slot * (uint32_t)every - 134U;
        compare_frame(opts, state, search, 134U);
        used += search + 134U;
        frames++;
    }
    compare_idle(opts, state, (uint32_t)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - used);
}

/*
 * A chain that decodes part of an LSM simulcast control channel decodes, so the binary trial took it and its own
 * proofs kept it for as long as the channel stayed up: the 769 MHz capture's control channel spent 164 s on C4FM at
 * half the TSBKs the CQPSK chain reads. The first validated frame of an acquisition now measures what that chain's
 * validated frames read over a search pass; one that let a frame go by unread in it has the other chain measured,
 * and the chain that read more keeps the profile.
 */
static void
test_p25p1_partial_chain_loses_the_comparison(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    /* C4FM reads one TSBK in three; its first opens the comparison and the votes are kept off the chain. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 134);
    assert(state.p25_p1_mod_probe_active == 1);
    assert(state.rf_mod == 0);
    for (int i = 0; i < 2; i++) {
        compare_frame(&opts, &state, 3 * 180 - 134, 134);
        assert(state.rf_mod == 0);
    }
    /* Its pass let frames go by unread: the pass ends on the other chain. */
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 134 - 2 * 3 * 180);
    assert(state.rf_mod == 1);
    assert(g_profile_cqpsk == 1 && g_profile_rate == 4800);
    assert(state.p25_p1_mod_probe_active == 1);
    /* CQPSK reads every TSBK: it wins, keeps the profile and becomes the learned modulation. */
    state.sps_hunt_counter = 1500;
    compare_pass(&opts, &state, 1);
    assert(state.rf_mod == 1);
    assert(state.p25_p1_validated_rf_mod == 1);
    assert(state.p25_p1_mod_compare.verdict_valid == 1);
    assert(state.p25_p1_mod_probe_active == 0);
    assert(state.sps_hunt_counter == 0);
    /* One comparison per acquisition: the chain it chose opens no other. */
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_mod_probe_active == 0);
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 1);

    /* A chain that reads the carrier back to back is never tried against: CQPSK first and whole stays, untouched. */
    compare_setup(&opts, &state, 1);
    compare_pass(&opts, &state, 1);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 0);
    assert(state.p25_p1_validated_rf_mod == 1);
    assert(state.p25_p1_mod_compare.verdict_valid == 1);
    assert(state.p25_p1_mod_probe_active == 0);

    /* The other way round: CQPSK came first reading one TSBK in three, C4FM reads them all. */
    compare_setup(&opts, &state, 1);
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 0);
    assert(g_profile_cqpsk == 0);
    compare_pass(&opts, &state, 1);
    assert(state.rf_mod == 0);
    assert(state.p25_p1_validated_rf_mod == 0);
    assert(state.p25_p1_mod_probe_active == 0);

    /* One TSBK missed in a pass the chain otherwise reads whole is a frame gone by: the other chain is measured. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 134);
    for (int i = 0; i < 4; i++) {
        compare_frame(&opts, &state, 46, 134);
    }
    compare_frame(&opts, &state, 46 + 180, 134);
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 6 * 134 - 5 * 46 - 180);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 1);

    /* A call's LDUs follow each other with almost no search: no trial. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 864);
    compare_frame(&opts, &state, 2, 864);
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 2 * 864 - 2);
    assert(state.rf_mod == 0 && state.p25_p1_mod_probe_active == 0);
    assert(state.p25_p1_mod_compare.verdict_valid == 1 && state.p25_p1_validated_rf_mod == 0);

    /* A tie keeps the incumbent, put back on its chain. */
    compare_setup(&opts, &state, 0);
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 1);
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 0 && g_profile_cqpsk == 0);
    assert(state.p25_p1_validated_rf_mod == 0);
    assert(state.p25_p1_mod_probe_active == 0);
    /* ... and the verdict holds against the next frame on it. */
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_mod_probe_active == 0 && state.rf_mod == 0);

    /* A carrier boundary ends a comparison without a verdict; the next acquisition compares afresh. */
    compare_setup(&opts, &state, 0);
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 1);
    state.carrier_seq++;
    compare_idle(&opts, &state, 100);
    assert(state.p25_p1_mod_probe_active == 0);
    assert(state.p25_p1_mod_compare.verdict_valid == 0);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_mod_probe_active == 1);

    /* Nothing is compared under a modulation lock, off the RTL front end, on a trunked voice channel, or on a profile
     * P25p1 does not own. */
    compare_setup(&opts, &state, 1);
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 0);

    compare_setup(&opts, &state, 0);
    opts.audio_in_type = AUDIO_IN_WAV;
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 0 && state.p25_p1_mod_probe_active == 0);

    compare_setup(&opts, &state, 0);
    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 0 && state.p25_p1_mod_probe_active == 0);

    compare_setup(&opts, &state, 1);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    compare_pass(&opts, &state, 3);
    assert(state.rf_mod == 1 && state.p25_p1_mod_probe_active == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/* Run a comparison into the other chain's pass: the incumbent reads one TSBK in three (it misses frames). */
static void
compare_into_trial(dsd_opts* opts, dsd_state* state, int incumbent_rf_mod) {
    compare_setup(opts, state, incumbent_rf_mod);
    compare_pass(opts, state, 3);
    assert(state->p25_p1_mod_compare.phase == 2);
    assert(state->rf_mod == !incumbent_rf_mod);
}

/*
 * How a comparison ends when its passes are cut short or read nothing, and what it leaves behind.
 */
static void
test_p25p1_comparison_ends_cleanly(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);

    /* A transmission that ends inside the incumbent's pass: its opening frame is all either pass validated, so there is
     * nothing to choose between and no verdict. The incumbent's chain is put back, nothing is stamped as learned, and
     * the carrier's next frame compares afresh. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 134);
    const uint32_t stamped = state.p25_p1_validated_symbolcnt;
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS);
    assert(state.p25_p1_mod_compare.phase == 2);
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS);
    assert(state.p25_p1_mod_compare.phase == 0);
    assert(state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.rf_mod == 0 && g_profile_cqpsk == 0);
    assert(state.p25_p1_mod_probe_active == 0);
    assert(state.p25_p1_validated_symbolcnt == stamped);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_mod_probe_active == 1);

    /* An opening frame long enough to fill most of the incumbent's pass (a data PDU) is not a pass that read the
     * carrier whole: with no frame after it, the pass has measured nothing, and the other chain gets its pass. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 1000);
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 1000);
    assert(state.p25_p1_mod_compare.phase == 2 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.rf_mod == 1);

    /* The other chain's pass is the decoder's experiment: the dwell gets back what that pass searched, no more. */
    compare_setup(&opts, &state, 0);
    compare_frame(&opts, &state, 0, 134);
    for (int i = 0; i < 2; i++) {
        compare_frame(&opts, &state, 3 * 180 - 134, 134);
    }
    state.sps_hunt_counter = 300;
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS - 134 - 2 * 3 * 180);
    assert(state.p25_p1_mod_compare.phase == 2);
    state.sps_hunt_counter = 2100;
    compare_idle(&opts, &state, DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS);
    assert(state.p25_p1_mod_compare.verdict_valid == 1 && state.rf_mod == 0);
    assert(state.sps_hunt_counter == 300);

    /* A verdict's own stamp is not a frame: with the verdict gone, the next entry opens nothing until a frame
     * validates. */
    state.p25_p1_mod_compare.verdict_valid = 0;
    compare_idle(&opts, &state, 10);
    assert(state.p25_p1_mod_probe_active == 0 && state.p25_p1_mod_compare.phase == 0);

    /* A C4FM verdict is withdrawn the way a learned CQPSK is: by a dwell on that chain that decodes nothing. */
    compare_setup(&opts, &state, 1);
    compare_pass(&opts, &state, 3);
    compare_pass(&opts, &state, 1);
    assert(state.p25_p1_mod_compare.verdict_valid == 1 && state.p25_p1_mod_compare.verdict_rf_mod == 0);
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    (void)frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.p25_p1_mod_compare.verdict_valid == 0);

    /* ... and a CQPSK verdict with the learned CQPSK it set. */
    compare_setup(&opts, &state, 0);
    compare_pass(&opts, &state, 3);
    compare_pass(&opts, &state, 1);
    assert(state.p25_p1_mod_compare.verdict_valid == 1 && state.p25_p1_mod_compare.verdict_rf_mod == 1);
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    (void)frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.p25_p1_mod_compare.verdict_valid == 0 && state.p25_p1_validated_rf_mod == -1);

    /* An acquisition reset in the other chain's pass ends the comparison without a verdict. Its caller owns the
     * demodulator and the learned modulation from there (a trunk-scan switch has restored the target's), so neither
     * is touched. */
    compare_into_trial(&opts, &state, 1);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_validated_rf_mod == 0);
    state.p25_p1_validated_rf_mod = -1; /* as a restored snapshot of a target that learned nothing would leave it */
    dsd_frame_sync_reset_acquisition(&opts, &state, 0);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.p25_p1_mod_probe_active == 0 && state.p25_p1_validated_rf_mod == -1);

    /* A dwell expiry that overtakes the other chain's pass ends it on the incumbent's chain, before the hunt decides
     * where to go: a P25p1-only session re-enters 4800/4 without normalising it. */
    compare_into_trial(&opts, &state, 0);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_validated_rf_mod == 1);
    opts.frame_p25p2 = 0;
    opts.frame_dmr = 0;
    state.p25_p1_mod_probe_next_qpsk = 0;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    (void)frame_sync_no_sync_sps_hunt(&opts, &state);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(state.rf_mod == 0 && state.p25_p1_validated_rf_mod == 0);

    /* ... and judges the dwell on the chain it ran on: a C4FM trial pass overtaken on a CQPSK incumbent decoded nothing
     * on CQPSK, so the learned CQPSK the abort puts back is not withdrawn. */
    compare_into_trial(&opts, &state, 1);
    compare_frame(&opts, &state, 46, 134);
    assert(state.rf_mod == 0 && state.p25_p1_validated_rf_mod == 0);
    opts.frame_p25p2 = 0;
    opts.frame_dmr = 0;
    state.p25_p1_mod_probe_next_qpsk = 1;
    state.sps_hunt_counter = dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
    /* A step: the abort put the other chain under the caller's sync window. */
    assert(frame_sync_no_sync_sps_hunt(&opts, &state) == 1);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_validated_rf_mod == 1);
    assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && state.rf_mod == 1);

    /* A lock taken mid-pass ends it without a verdict and leaves the demodulator to the lock. The other chain's frame
     * had overwritten the learned modulation; it goes back to the leading chain. */
    compare_into_trial(&opts, &state, 0);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_validated_rf_mod == 1);
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    compare_idle(&opts, &state, 10);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.p25_p1_mod_probe_active == 0 && state.p25_p1_validated_rf_mod == 0);

    /* So does a profile change: the demodulator belongs to the new profile. */
    compare_into_trial(&opts, &state, 0);
    compare_frame(&opts, &state, 46, 134);
    assert(state.p25_p1_validated_rf_mod == 1);
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.rf_mod = 2;
    compare_idle(&opts, &state, 10);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.rf_mod == 2 && state.p25_p1_validated_rf_mod == 0);

    /* A Phase 2 grant has claimed the modulation for its TDMA slot before the tune: the learned P25p1 modulation goes
     * back to the leading chain, the decoder's stays CQPSK. */
    compare_into_trial(&opts, &state, 0);
    compare_frame(&opts, &state, 46, 134);
    state.p25_p2_active_slot = 1;
    state.rf_mod = 1;
    dsd_frame_sync_p25p1_mod_compare_end_for_tune(&state);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_probe_active == 0);
    assert(state.rf_mod == 1 && state.p25_p1_validated_rf_mod == 0);
    state.p25_p2_active_slot = -1;

    /* A tune ends a comparison on the chain leading so far: the incumbent unless the other chain has read more. */
    compare_into_trial(&opts, &state, 1);
    compare_frame(&opts, &state, 46, 134);
    dsd_frame_sync_p25p1_mod_compare_end_for_tune(&state);
    assert(state.p25_p1_mod_compare.phase == 0 && state.p25_p1_mod_compare.verdict_valid == 0);
    assert(state.rf_mod == 1 && state.p25_p1_validated_rf_mod == 1 && state.p25_p1_mod_probe_active == 0);
    compare_into_trial(&opts, &state, 0);
    for (int i = 0; i < 9; i++) {
        compare_frame(&opts, &state, 46, 134);
    }
    assert(state.p25_p1_mod_compare.phase == 2);
    dsd_frame_sync_p25p1_mod_compare_end_for_tune(&state);
    assert(state.rf_mod == 1 && state.p25_p1_validated_rf_mod == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/*
 * Issue #423: a P25p1 sync cannot claim CQPSK -- the sync word is identical on both
 * modulations -- but while the chain is already on it must re-assert the front-end profile the
 * way P25p2 does, so an engine retune cannot leave the two out of step.
 */
static void
test_p25p1_sync_reasserts_cqpsk_demod_profile(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    state.min = -3.0f;
    state.max = 3.0f;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);

    /* On C4FM the same sync claims nothing and pushes nothing. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 0;
    state.min = -3.0f;
    state.max = 3.0f;
    reset_fake_profile_capture();
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(state.rf_mod == 0);
    assert(g_profile_set_calls == 0);

    /* A CLI modulation lock owns the front end; the accept leaves it alone. */
    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 1;
    state.min = -3.0f;
    state.max = 3.0f;
    reset_fake_profile_capture();
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P1_SYNC, 24) == DSD_SYNC_P25P1_POS);
    assert(g_profile_set_calls == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_unlocked_rtl_p25p2_sync_switches_demod_family(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p2 = 1;
    state.rf_mod = 0;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;
    state.p2_wacn = 1;
    state.p2_cc = 1;
    state.p2_sysid = 1;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, P25P2_SYNC, 20) == DSD_SYNC_P25P2_POS);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 6000);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_ted_sps == 8);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_rtl_sps_profiles_apply_and_lock_on_sync(void) {
    static const int expected_rates[] = {4800, 2400, 9600, 6000, 4800};
    static const int expected_levels[] = {4, 4, 2, 4, 2};
    static const int expected_channels[] = {
        DSD_RTL_STREAM_CHANNEL_PROFILE_12K5,     DSD_RTL_STREAM_CHANNEL_PROFILE_6K25,
        DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE, DSD_RTL_STREAM_CHANNEL_PROFILE_12K5,
        DSD_RTL_STREAM_CHANNEL_PROFILE_6K25,
    };
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dstar = 1;
    opts.frame_x2tdma = 1;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dmr = 1;
    opts.frame_dpmr = 1;
    opts.frame_provoice = 1;
    opts.frame_ysf = 1;
    opts.frame_m17 = 1;
    state.rf_mod = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    for (int profile_index = 0; profile_index < 5; profile_index++) {
        state.sps_hunt_idx = (profile_index + 1) % 5;
        frame_sync_apply_sps_hunt_profile(&opts, &state, profile_index, 0);
        assert(state.sps_hunt_idx == profile_index);
        assert(g_profile_rate == expected_rates[profile_index]);
        assert(g_profile_levels == expected_levels[profile_index]);
        assert(g_profile_channel == expected_channels[profile_index]);
        assert(state.samplesPerSymbol == dsd_opts_compute_sps_rate(&opts, expected_rates[profile_index], 48000));
    }
    assert(g_profile_set_calls == 5);

    const int locked_sps = state.samplesPerSymbol;
    const int locked_center = state.symbolCenter;
    const int calls_before_sync = g_profile_set_calls;
    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DSTAR_SYNC, 24) == DSD_SYNC_DSTAR_VOICE_POS);
    assert(state.sps_hunt_idx == 4);
    assert(state.samplesPerSymbol == locked_sps);
    assert(state.symbolCenter == locked_center);
    assert(g_profile_set_calls == calls_before_sync);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 2);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_6K25);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_dmr_sync_applies_gfsk_rtl_profile(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = 0;
    state.rf_mod = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state.min = -3.0f;
    state.max = 3.0f;

    dsd_rtl_stream_metrics_hooks hooks = {
        .output_rate_hz = fake_output_rate_hz,
        .apply_demod_profile = fake_apply_demod_profile,
    };
    dsd_rtl_stream_metrics_hooks_set(&hooks);
    reset_fake_profile_capture();

    assert(dsd_frame_sync_test_try_protocol_matches(&opts, &state, DMR_BS_DATA_SYNC, 24) == DSD_SYNC_DMR_BS_DATA_POS);
    assert(state.rf_mod == 2);
    assert(g_profile_set_calls == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_12K5);
    assert(g_profile_cqpsk == 0);
    assert(g_profile_ted_sps == 10);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_active_profile_metrics_power_gate_and_votes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_pwr = 0.25;
    opts.rtl_squelch_level = 0.5;
    opts.frame_p25p1 = 1;
    opts.frame_p25p2 = 1;
    opts.frame_dstar = 1;
    opts.frame_dmr = 1;
    opts.frame_nxdn48 = 1;
    opts.frame_nxdn96 = 1;
    opts.frame_dpmr = 1;
    g_snr_c4fm = 101.0;
    g_snr_cqpsk = 102.0;
    g_snr_gfsk = 103.0;
    install_fake_snr_hooks();

    state.sps_hunt_idx = 0;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 0);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 101.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    state.rf_mod = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 102.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    state.sps_hunt_idx = 4;
    state.rf_mod = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 103.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);
    /* The auto squelch is off on a digital channel (issue #518 follow-up): it never skips a GFSK sync, whatever the
       level left behind from a level setting. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;

    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 1);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 102.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);
    opts.mod_cli_lock = 0;
    opts.mod_qpsk = 0;

    opts.audio_in_type = AUDIO_IN_WAV;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_mixed_profile_snr_recovers_from_gfsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    set_fake_snr(0.0, -100.0, 20.0, -100.0);
    install_fake_snr_hooks();
    dsd_frame_sync_reset_mod_state();
    reset_fake_profile_capture();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 1);
    assert(gfsk_votes == 0);
    assert(g_profile_set_calls == 0);

    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);
    assert(g_profile_rate == 4800);
    assert(g_profile_levels == 4);
    assert(g_profile_channel == DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    assert(g_profile_ted_sps == 10);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    lastt = 24;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dstar = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state.rf_mod = 2;
    lastt = 24;
    dsd_frame_sync_reset_mod_state();
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 2);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_snr_squelch_only_applies_to_rtl_input(void) {
    static const int non_radio_inputs[] = {
        AUDIO_IN_WAV, AUDIO_IN_TCP, AUDIO_IN_UDP, AUDIO_IN_SYMBOL_BIN, AUDIO_IN_SYMBOL_FLT,
    };
    static dsd_opts opts;
    static dsd_state state;

    set_fake_snr(-100.0, -100.0, -100.0, -100.0);
    install_fake_snr_hooks();
    (void)dsd_setenv("DSD_NEO_SNR_SQL_DB", "10", 1);
    dsd_neo_config_init();

    for (size_t i = 0; i < sizeof(non_radio_inputs) / sizeof(non_radio_inputs[0]); i++) {
        reset(&opts, &state);
        opts.audio_in_type = non_radio_inputs[i];
        opts.frame_dmr = 1;
        opts.mod_cli_lock = 1;
        opts.mod_gfsk = 1;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
        state.rf_mod = 2;
        assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);
    }

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_dmr = 1;
    opts.mod_cli_lock = 1;
    opts.mod_gfsk = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 2;
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);

    (void)dsd_unsetenv("DSD_NEO_SNR_SQL_DB");
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_nxdn_only_profiles_use_gfsk_snr_gate(void) {
    static dsd_opts opts;
    static dsd_state state;

    install_fake_snr_hooks();
    (void)dsd_setenv("DSD_NEO_SNR_SQL_DB", "10", 1);
    dsd_neo_config_init();

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 0;
    g_snr_c4fm = 0.0;
    g_snr_gfsk = 20.0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_nxdn48 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 2);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    g_snr_c4fm = 20.0;
    g_snr_gfsk = 0.0;
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 1);

    reset(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.frame_p25p1 = 1;
    opts.frame_nxdn96 = 1;
    state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state.rf_mod = 0;
    assert(frame_sync_active_profile_modulation(&opts, &state) == 0);
    assert(frame_sync_active_profile_snr_db(&opts, &state) == 20.0);
    assert(frame_sync_should_skip_snr_or_power_gate(&opts, &state) == 0);

    (void)dsd_unsetenv("DSD_NEO_SNR_SQL_DB");
    dsd_neo_config_init();
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

static void
test_modulation_snr_fallback_votes_and_dwell(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.carrier = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    set_fake_snr(-100.0, 4.0, -100.0, 12.0);
    install_fake_snr_hooks();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 0);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 1);
    assert(gfsk_votes == 0);

    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    lastt = 24;
    set_fake_snr(25.0, -100.0, 5.0, -100.0);
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/* One auto-switch pass, with the recent sync hamming set first when @p c4fm_hamming: a near-exact C4FM sync pattern,
   which outvotes the SNR bias. */
static void
dwell_auto_switch_pass(dsd_opts* opts, dsd_state* state, int c4fm_hamming) {
    int lastt = 24;
    if (c4fm_hamming) {
        dsd_frame_sync_test_set_recent_hamming(0, 24, 24);
    }
    frame_sync_maybe_auto_switch_modulation(opts, state, 24, &lastt);
}

/* Arm the CQPSK dwell at the decode time @p armed_ns: two CQPSK-leaning passes enter CQPSK from C4FM. */
static void
dwell_arm_at(dsd_opts* opts, dsd_state* state, uint64_t armed_ns) {
    reset(opts, state);
    opts->frame_p25p1 = 1;
    state->carrier = 1;
    state->rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    dsd_decode_clock_use_test(armed_ns);
    set_fake_snr(-100.0, 4.0, -100.0, 12.0);
    install_fake_snr_hooks();
    dwell_auto_switch_pass(opts, state, 0);
    dwell_auto_switch_pass(opts, state, 0);
    assert(state->rf_mod == 1);
    assert(dsd_frame_sync_test_qpsk_dwell_armed());
}

/*
 * The CQPSK dwell is a decode decision, so it runs on the decode clock: a replay settles it by capture time, however
 * fast the capture is read. Driven through the TEST decode clock, with no real time passing, both of its reads hold at
 * decode +1.9 s and let go at +2.1 s:
 * - the SNR bias: inside the dwell a C4FM SNR edge casts no vote, past it three votes take the chain back;
 * - the switch threshold: inside the dwell C4FM needs five votes (here from the sync hamming), past it three, so
 *   three votes hold and a fourth past the dwell switches.
 */
static void
check_cqpsk_dwell_on_the_decode_clock(uint64_t armed_ns) {
    static dsd_opts opts;
    static dsd_state state;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    dwell_arm_at(&opts, &state, armed_ns);
    set_fake_snr(25.0, -100.0, 5.0, -100.0);
    dsd_decode_clock_test_set_ns(armed_ns + 1900000000ULL);
    for (int pass = 0; pass < 3; pass++) {
        dwell_auto_switch_pass(&opts, &state, 0);
    }
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 1);
    assert(c4fm_votes == 0);
    dsd_decode_clock_test_set_ns(armed_ns + 2100000000ULL);
    dwell_auto_switch_pass(&opts, &state, 0);
    dwell_auto_switch_pass(&opts, &state, 0);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 1);
    assert(c4fm_votes == 2);
    dwell_auto_switch_pass(&opts, &state, 0);
    assert(state.rf_mod == 0);

    dwell_arm_at(&opts, &state, armed_ns);
    dsd_decode_clock_test_set_ns(armed_ns + 1900000000ULL);
    for (int pass = 0; pass < 3; pass++) {
        dwell_auto_switch_pass(&opts, &state, 1);
    }
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 1);
    assert(c4fm_votes == 3);
    dsd_decode_clock_test_set_ns(armed_ns + 2100000000ULL);
    dwell_auto_switch_pass(&opts, &state, 1);
    assert(state.rf_mod == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_decode_clock_use_system();
}

/* The dwell holds and lets go the same way at a short decode time and at a replay's capture time, whose ms are far past
 * INT_MAX, including one whose ms are a whole multiple of 2^32: the stamp is kept full width, so it is armed there
 * too, not mistaken for the 0 that means disarmed. */
static void
test_cqpsk_dwell_runs_on_the_decode_clock(void) {
    check_cqpsk_dwell_on_the_decode_clock(7000ULL * 1000000000ULL);
    check_cqpsk_dwell_on_the_decode_clock(1788245497ULL * 1000000000ULL + 123000000ULL); /* 2026-09-01T06:51:37.123Z */
    check_cqpsk_dwell_on_the_decode_clock(416ULL * 4294967296ULL * 1000000ULL);          /* 416 x 2^32 ms */
}

static void
test_modulation_cli_lock_prevents_votes(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    set_fake_snr(-100.0, -100.0, 20.0, -100.0);
    install_fake_snr_hooks();

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 0);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 0);

    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/* The analog preset (-fA) leaves the modulation unlocked, but an analog channel has no digital modulation to pick.
 * A CQPSK vote there applied the P25 CQPSK demod profile to the RTL front end, which then delivered symbols instead
 * of monitor audio (a carrier near 0 Hz votes CQPSK). The analog family stands the auto-switch down; the same
 * unlocked options and metrics outside it still switch on the second vote. */
static void
test_analog_family_never_auto_switches_modulation(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int fake_rtl_context;
    int c4fm_votes = -1;
    int qpsk_votes = -1;
    int gfsk_votes = -1;

    reset(&opts, &state);
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_ANALOG, DSD_DECODE_PRESET_PROFILE_CLI, &opts, &state) == 0);
    assert(dsd_opts_is_analog_family(&opts) == 1);
    assert(opts.mod_cli_lock == 0);
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    /* CQPSK 14 dB above C4FM after its 6 dB offset: every vote goes to CQPSK. */
    set_fake_snr(0.0, -100.0, 20.0, -100.0);
    install_fake_snr_hooks();
    dsd_frame_sync_reset_mod_state();
    reset_fake_profile_capture();

    for (int i = 0; i < 8; i++) {
        int lastt = 24;
        frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    }
    dsd_frame_sync_test_get_mod_votes(&c4fm_votes, &qpsk_votes, &gfsk_votes);
    assert(state.rf_mod == 0);
    assert(c4fm_votes == 0);
    assert(qpsk_votes == 0);
    assert(gfsk_votes == 0);
    assert(g_profile_set_calls == 0);

    /* Control: only the analog family differs. */
    opts.analog_only = 0;
    assert(dsd_opts_is_analog_family(&opts) == 0);
    for (int i = 0; i < 2; i++) {
        int lastt = 24;
        frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    }
    assert(state.rf_mod == 1);
    assert(g_profile_set_calls == 1);
    assert(g_profile_cqpsk == 1);

    dsd_frame_sync_reset_mod_state();
    dsd_rtl_stream_metrics_hooks_set(NULL);
}

/* An RTL session on the analog monitor (-fA) or, as the control, on a digital preset. */
static void
init_rtl_profile_session(dsd_opts* opts, dsd_state* state, int analog, int digital_frame_is_dstar) {
    static int fake_rtl_context;
    reset(opts, state);
    if (analog) {
        opts->analog_only = 1;
        opts->monitor_input_audio = 1;
    } else if (digital_frame_is_dstar) {
        opts->frame_dstar = 1;
    } else {
        opts->frame_p25p1 = 1;
    }
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    install_fake_snr_hooks();
    reset_fake_profile_capture();
}

/*
 * The analog monitor has no symbol clock: nothing the sync hunt decides while it runs may hand
 * the RTL front end a symbol profile. A CQPSK one turns the stream's monitor audio into symbols,
 * which silenced the monitor and received-tone detection with it (issue #522: a -fA replay that
 * opened on quiet audio decoded nothing at all), and a digital channel profile narrows it. Every
 * profile request the hunt makes goes through one helper, and two paths reach it: the
 * modulation vote, and the dwell running out on a two-level profile, which re-normalises its
 * modulation (a live switch to -fA from D-STAR leaves the hunt on 4800/2). The same calls on a
 * digital session are the control: there they do reach the front end.
 */
static void
test_analog_monitor_never_requests_rtl_symbol_profiles(void) {
    static dsd_opts opts;
    static dsd_state state;

    for (int analog = 1; analog >= 0; analog--) {
        int lastt = 24;
        init_rtl_profile_session(&opts, &state, analog, 0);
        state.carrier = 1;
        dsd_frame_sync_reset_mod_state();
        set_fake_snr(-100.0, 4.0, -100.0, 12.0);

        /* CQPSK-favouring metrics: whether or not the vote runs under -fA, only the digital
           session's switch reaches the front end. */
        frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
        lastt = 24;
        frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
        if (analog) {
            assert(g_profile_set_calls == 0);
        } else {
            assert(state.rf_mod == 1);
            assert(g_profile_set_calls == 1 && g_profile_cqpsk == 1);
        }
        dsd_rtl_stream_metrics_hooks_set(NULL);

        init_rtl_profile_session(&opts, &state, analog, 1);
        state.rf_mod = 0;
        state.sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
        state.sps_hunt_counter =
            dsd_frame_sync_sps_hunt_dwell_passes(&opts, &state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
        state.samplesPerSymbol = 10;
        state.symbolCenter = 4;
        state.min = -3.0f;
        state.max = 3.0f;
        frame_sync_no_sync_sps_hunt(&opts, &state);
        /* The hunt re-normalised the binary profile to GFSK, so the request was made. */
        assert(state.sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2 && state.rf_mod == 2);
        if (analog) {
            assert(g_profile_set_calls == 0);
        } else {
            assert(g_profile_set_calls == 1 && g_profile_cqpsk == 0 && g_profile_levels == 2);
        }
        dsd_rtl_stream_metrics_hooks_set(NULL);
    }
}

static void
test_hamming_override_can_select_qpsk(void) {
    static dsd_opts opts;
    static dsd_state state;
    int lastt = 24;

    reset(&opts, &state);
    opts.frame_p25p1 = 1;
    state.rf_mod = 0;
    dsd_frame_sync_reset_mod_state();
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_frame_sync_test_set_recent_hamming(10, 1, 24);

    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 0);
    lastt = 24;
    frame_sync_maybe_auto_switch_modulation(&opts, &state, 24, &lastt);
    assert(state.rf_mod == 1);
}

static void
test_p25_trunk_tick_recency(void) {
    static dsd_opts opts;
    static dsd_state state;

    reset(&opts, &state);
    opts.trunk_enable = 1;
    state.p25_p2_active_slot = -1;
    state.lastsynctype = DSD_SYNC_P25P1_POS;
    g_frame_sync_tick_calls = 0;
    dsd_frame_sync_test_reset_p25_trunk_tick_state();
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_try_tick = fake_p25_sm_try_tick,
    });

    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)100);
    assert(g_frame_sync_tick_calls == 1);
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)100);
    assert(g_frame_sync_tick_calls == 1);

    state.lastsynctype = DSD_SYNC_NONE;
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)101);
    assert(g_frame_sync_tick_calls == 2);
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)105);
    assert(g_frame_sync_tick_calls == 2);

    state.p25_p2_active_slot = 0;
    frame_sync_maybe_tick_p25_trunk_sm(&opts, &state, (time_t)106);
    assert(g_frame_sync_tick_calls == 3);

    dsd_frame_sync_hooks_set(NULL);
}
#endif

static void
test_scan_class_matchers_and_no_sync(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->ssize = 36;
    opts->msize = 15;
    for (int m = DSD_SCAN_MODE_P25; m <= DSD_SCAN_MODE_M17; m++) {
        assert(dsd_scan_mode_enter(opts, state, (dsd_scan_mode)m) == 0);
        const int expected = (int)dsd_scan_mode_profile((dsd_scan_mode)m).sps_profile_index;
        dsd_frame_sync_reset_acquisition(opts, state, 1);
        for (int p = 0; p < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; p++) {
            const int allowed = p == expected || (m == DSD_SCAN_MODE_P25 && p == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
            assert(dsd_frame_sync_test_sps_hunt_profile_has_candidate(opts, p) == allowed);
        }
        for (int n = 0; n < 120; n++) {
            state->sps_hunt_counter =
                dsd_frame_sync_sps_hunt_dwell_passes(opts, state) * DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS;
            (void)frame_sync_no_sync_sps_hunt(opts, state);
            assert(state->sps_hunt_idx == expected
                   || (m == DSD_SCAN_MODE_P25 && state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4));
        }
        state->sps_hunt_idx = expected;
        if (m != DSD_SCAN_MODE_P25) {
            assert(dsd_frame_sync_test_try_protocol_matches(opts, state, P25P1_SYNC, 24) == DSD_SYNC_NONE);
            assert(dsd_frame_sync_test_try_protocol_matches(opts, state, P25P2_SYNC, 20) == DSD_SYNC_NONE);
        }
        if (m != DSD_SCAN_MODE_DMR) {
            assert(dsd_frame_sync_test_try_protocol_matches(opts, state, DMR_MS_RC_SYNC, 24) == DSD_SYNC_NONE);
        }
        state->profile_proof_valid = 1;
        state->symbol_history_count = 45;
        state->sps_hunt_counter = 300;
        state->p25_p1_validated_rf_mod = 1;
        state->sbuf[0] = 123.0f;
        /* Nor the decoder's verdict that the carrier is two-slot TDMA: a decoded frame of the next acquisition proves
           it again, whatever the snapshots published before the reset still say. */
        state->tdma_carrier = 1U;
        state->x2tdma_superframe_valid = 1U;
        /* A received tone never outlives the boundary this reset marks (issue #522): a new
           row, target or mode starts with nothing heard. */
        state->analog_rx.carrier_open = 1;
        state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
        state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
        state->analog_rx.ctcss_tenths_hz = 1000;
        const uint32_t tone_generation = state->analog_rx.generation;
        dsd_frame_sync_reset_acquisition(opts, state, 1);
        assert(!state->profile_proof_valid && !state->symbol_history_count && !state->sps_hunt_counter);
        assert(state->p25_p1_validated_rf_mod == -1 && state->sidx == 0 && state->midx == 0);
        assert(state->tdma_carrier == 0U && state->x2tdma_superframe_valid == 0U);
        assert(fabsf(state->sbuf[0] - state->min) < 0.01f);
        assert(state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
        assert(state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.ctcss_tenths_hz == 0);
        assert(state->analog_rx.carrier_open == 0 && state->analog_rx.generation != tone_generation);
        /* Nor does a received DCS code (issue #523), through the reset a scan target switch makes
           (forget 0), which the engine tests' stubs only mirror. */
        state->analog_rx.carrier_open = 1;
        state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
        state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
        state->analog_rx.dcs_code = 023;
        state->analog_rx.dcs_inverted = 1;
        const uint32_t code_generation = state->analog_rx.generation;
        dsd_frame_sync_reset_acquisition(opts, state, 0);
        assert(state->analog_rx.tone_state == DSD_ANALOG_TONE_STATE_INACTIVE);
        assert(state->analog_rx.tone_kind == DSD_ANALOG_TONE_KIND_NONE && state->analog_rx.dcs_code == 0);
        assert(state->analog_rx.dcs_inverted == 0 && state->analog_rx.carrier_open == 0);
        assert(state->analog_rx.generation != code_generation);
    }
    dsd_scan_mode_leave(opts, state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

int
main(void) {
    test_tetra_soft_history_capture();
    test_tetra_inverted_sync_patterns_flip_only_dibit_msb();
    test_tetra_uplink_control_burst_is_not_a_normal_burst();
    test_tetra_bursts_wait_for_their_own_look_back();
    test_scan_class_matchers_and_no_sync();
    test_p25_vc_acquisition_hooks();
    test_sps_hunt_skips_disabled_protocol_rates();
    test_sps_hunt_profile_updates_timing();
    test_sps_hunt_reconciles_external_timing();
    test_adopted_profile_starts_its_dwell_over();
    test_sps_hunt_budget_is_spent_in_symbols();
    test_sps_hunt_consumption_is_exact_across_the_symbolcnt_wrap();
    test_sps_hunt_proven_verdict_restarts_the_dwell();
    test_sps_hunt_withheld_verdict_refunds_only_its_own_cycle();
    test_sps_hunt_restart_dwell_resets_the_whole_budget();
    test_sps_hunt_holds_the_profile_on_a_tuned_voice_channel();
    test_sps_hunt_p25_probe_rotation();
    test_carrier_does_not_reset_the_hunt_budget();
    test_binary_profiles_override_unlocked_qpsk();
    test_four_level_profiles_reset_inherited_modulation();
    test_nxdn_variant_follows_active_profile();
    test_bounded_symbol_history_readiness_and_wrap();
    test_tetra_training_rotation_oracle();
    test_provoice_candidate_does_not_shadow_dstar_or_nxdn();
    test_symbol_replay_bypasses_sps_profile_gating();
    test_dmr_rc_sync_matches_and_respects_polarity();
    test_symbol_replay_requires_explicit_nxdn_variant();
    test_manual_p25p2_c4fm_bypasses_profile_gating();
    test_locked_p25p2_c4fm_survives_sync();
    test_m17_candidate_chains_to_lsf_under_auto();
    test_m17_candidate_expires_without_a_following_sync();
    test_short_m17_window_estimates_levels_without_warm_start_history();
    test_m17_alternating_runs_alone_are_never_a_sync();
    test_a_dpmr_frame_is_not_reopened_by_the_nxdn_matcher();
    test_the_dpmr_frame_suppression_is_scoped_to_its_own_profile();
    test_a_p25p1_frame_is_not_reopened_by_the_nxdn96_matcher();
    test_a_suppressed_nxdn96_window_still_blends_levels();
    test_the_p25p1_frame_guard_is_scoped_to_the_c4fm_chain();
    test_the_p25p1_frame_suppression_is_scoped_to_its_own_profile();
    test_the_p25p1_frame_guard_survives_symbolcnt_rollover();
    test_a_p25p1_frame_suppresses_m17_candidates();
    test_m17_joins_on_a_sync_word_three_frames_running();
    test_nxdn96_follows_the_receiver_sync_rules();
    test_nxdn96_lock_follows_the_lich();
    test_nxdn96_acquires_on_the_four_level_fsw();
    test_an_nxdn96_data_call_stays_nxdn96_under_auto();
    test_nxdn48_keeps_two_hits_at_any_distance();
    test_a_proven_2400_4_transmission_stands_down_the_4800_4_matchers();
    test_the_2400_4_proof_suppression_still_blends_levels();
    test_the_2400_4_proof_suppression_is_scoped_to_its_arming_protocols();
    test_the_2400_4_proof_suppression_survives_symbolcnt_rollover();
    test_elapsed_seconds_prefers_monotonic_then_wall_time();
    test_p25_slot_activity_honors_gate_mac_and_hangtime();
    test_hamming_helpers_find_best_patterns();
#ifdef USE_RADIO
    test_sps_hunt_restores_learned_p25p1_cqpsk();
    test_trunk_scan_p25_probe_fits_visit();
    test_p25p1_auto_hunt_alternates_cqpsk_probe();
    test_p25p1_probe_survives_its_dwell();
    test_p25p1_cqpsk_trial_leaves_half_the_dwell_to_fsk_modes();
    test_validated_p25p1_modulation_outranks_the_heuristics();
    test_validated_cqpsk_survives_rotation_but_not_disproof();
    test_p25p1_partial_chain_loses_the_comparison();
    test_p25p1_comparison_ends_cleanly();
    test_p25p1_sync_reasserts_cqpsk_demod_profile();
    test_rtl_symbol_profile_selection();
    test_decode_mode_profiles_agree_with_the_sps_hunt_table();
    test_rtl_p25p2_timing_reconciliation_preserves_cqpsk();
    test_unlocked_rtl_p25p2_sync_switches_demod_family();
    test_rtl_sps_profiles_apply_and_lock_on_sync();
    test_dmr_sync_applies_gfsk_rtl_profile();
    test_active_profile_metrics_power_gate_and_votes();
    test_mixed_profile_snr_recovers_from_gfsk();
    test_snr_squelch_only_applies_to_rtl_input();
    test_nxdn_only_profiles_use_gfsk_snr_gate();
    test_modulation_snr_fallback_votes_and_dwell();
    test_cqpsk_dwell_runs_on_the_decode_clock();
    test_modulation_cli_lock_prevents_votes();
    test_analog_family_never_auto_switches_modulation();
    test_analog_monitor_never_requests_rtl_symbol_profiles();
    test_hamming_override_can_select_qpsk();
    test_p25_trunk_tick_recency();
#endif
    return 0;
}
