// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-no-int-to-ptr)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_confidence.h"
#include "dsd-neo/app_control/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

volatile uint8_t exitflag = 0;

static uint8_t g_dibits[512];
static int g_bootstrap_payload[90];
static size_t g_dibit_index = 0;
static unsigned int g_open_left_calls;
static unsigned int g_open_right_calls;
static unsigned int g_close_left_calls;
static unsigned int g_close_right_calls;
static unsigned int g_process_mbe_calls;
static unsigned int g_play_fs3_calls;
static unsigned int g_play_ss3_calls;
static unsigned int g_ui_redraw_calls;
static unsigned int g_history_calls[2];
static unsigned int g_current_calls[2];
static unsigned int g_data_sync_calls;
static unsigned int g_data_burst_calls;
static uint8_t g_data_burst_last;
static unsigned int g_debug_dump_calls;
static uint8_t g_debug_dump_last_slot;
static uint8_t g_debug_dump_last_type;
static unsigned int g_cach_calls;
static unsigned int g_reset_blocks_calls;
static unsigned int g_alg_refresh_calls;
static unsigned int g_alg_reset_calls;
static unsigned int g_refresh_error_calls;
static unsigned int g_hytera_refresh_calls;
static unsigned int g_late_entry_calls;
static uint8_t g_late_entry_last_vc;
static unsigned int g_sbrc_calls;
static uint8_t g_sbrc_last_power;
static unsigned int g_sm_voice_sync_calls;
static int g_sm_voice_sync_last_slot;
static unsigned int g_sm_tick_calls;
static unsigned int g_confidence_reset_calls;
static unsigned int g_confidence_reset_slot_calls;
static unsigned int g_confidence_reset_last_slot;
static unsigned int g_confidence_voice_sync_calls;
static unsigned int g_confidence_voice_burst_calls;
static dmr_confidence_result g_confidence_result = DMR_CONFIDENCE_LOCKED;
static int g_voice_slot_open = 1;
static int g_any_voice_open = 0;

static void
reset_spies(void) {
    g_dibit_index = 0;
    g_open_left_calls = 0;
    g_open_right_calls = 0;
    g_close_left_calls = 0;
    g_close_right_calls = 0;
    g_process_mbe_calls = 0;
    g_play_fs3_calls = 0;
    g_play_ss3_calls = 0;
    g_ui_redraw_calls = 0;
    g_history_calls[0] = 0;
    g_history_calls[1] = 0;
    g_current_calls[0] = 0;
    g_current_calls[1] = 0;
    g_data_sync_calls = 0;
    g_data_burst_calls = 0;
    g_data_burst_last = 0;
    g_debug_dump_calls = 0;
    g_debug_dump_last_slot = 0xFFU;
    g_debug_dump_last_type = 0;
    g_cach_calls = 0;
    g_reset_blocks_calls = 0;
    g_alg_refresh_calls = 0;
    g_alg_reset_calls = 0;
    g_refresh_error_calls = 0;
    g_hytera_refresh_calls = 0;
    g_late_entry_calls = 0;
    g_late_entry_last_vc = 0;
    g_sbrc_calls = 0;
    g_sbrc_last_power = 0;
    g_sm_voice_sync_calls = 0;
    g_sm_voice_sync_last_slot = -1;
    g_sm_tick_calls = 0;
    g_confidence_reset_calls = 0;
    g_confidence_reset_slot_calls = 0;
    g_confidence_reset_last_slot = 0xFFU;
    g_confidence_voice_sync_calls = 0;
    g_confidence_voice_burst_calls = 0;
    g_confidence_result = DMR_CONFIDENCE_LOCKED;
    g_voice_slot_open = 1;
    g_any_voice_open = 0;
    exitflag = 0;
}

static void
set_cach_tact_bit(size_t burst_start, uint8_t tact_index, uint8_t value) {
    static const int cach_interleave[24] = {
        0, 7, 8, 9, 1, 10, 11, 12, 2, 13, 14, 15, 3, 16, 4, 17, 18, 19, 5, 20, 21, 22, 6, 23,
    };
    for (size_t packed = 0; packed < 24U; packed++) {
        if (cach_interleave[packed] != tact_index) {
            continue;
        }
        uint8_t mask = (uint8_t)(packed % 2U == 0U ? 2U : 1U);
        if (value != 0U) {
            g_dibits[burst_start + (packed / 2U)] |= mask;
        } else {
            g_dibits[burst_start + (packed / 2U)] &= (uint8_t)~mask;
        }
        return;
    }
}

static void
set_sync_dibits(size_t burst_start, const char* sync) {
    const size_t sync_offset = burst_start + 12U + 36U + 18U;
    for (size_t i = 0; sync[i] != '\0'; i++) {
        g_dibits[sync_offset + i] = (sync[i] == '3') ? 2U : 0U;
    }
}

static void
load_single_burst_stream(uint8_t slot, const char* sync) {
    DSD_MEMSET(g_dibits, 0, sizeof(g_dibits));
    reset_spies();

    g_dibits[12U + 16U] = 1U;
    g_dibits[144U + 12U + 16U] = 1U;
    set_cach_tact_bit(0, 1, slot);
    set_sync_dibits(0, sync);
}

static void
load_voice_burst_stream(void) {
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
}

static void
load_bootstrap_voice_stream(uint8_t slot) {
    load_single_burst_stream(slot, DMR_BS_VOICE_SYNC);
    for (size_t i = 0; i < 90U; i++) {
        g_bootstrap_payload[i] = g_dibits[i];
    }
    DSD_MEMSET(g_dibits + 144U, 0, 144U);
    g_dibit_index = 90U;
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (g_dibit_index >= sizeof(g_dibits)) {
        return 0;
    }
    return g_dibits[g_dibit_index++] & 0x3U;
}

bool
Hamming_7_4_decode(unsigned char* rxBits) {
    (void)rxBits;
    return true;
}

bool
QR_16_7_6_decode(unsigned char* rxBits) {
    (void)rxBits;
    return true;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 1234567890ULL;
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    exitflag = 1;
}

void
getTimeC_buf(char out[9]) {
    DSD_SNPRINTF(out, 9, "%s", "00:00:00");
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_left_calls++;
    opts->mbe_out_f = (FILE*)(uintptr_t)1;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_right_calls++;
    opts->mbe_out_fR = (FILE*)(uintptr_t)1;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_left_calls++;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_right_calls++;
    opts->mbe_out_fR = NULL;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_process_mbe_calls++;
}

void
playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_fs3_calls++;
}

void
playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_ss3_calls++;
}

void
dsd_telemetry_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_ui_redraw_calls++;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    g_history_calls[slot & 1U]++;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    g_current_calls[slot & 1U]++;
}

void
tyt16_ambe2_codeword_keystream(const dsd_state* state, char ambe_fr[4][24], int fnum) {
    (void)state;
    (void)ambe_fr;
    (void)fnum;
}

void
csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]) {
    (void)state;
    (void)ambe_fr;
}

void
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_data_sync_calls++;
}

void
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst) {
    (void)opts;
    (void)state;
    (void)info;
    g_data_burst_calls++;
    g_data_burst_last = databurst;
}

void
dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    (void)opts;
    (void)state;
    g_debug_dump_calls++;
    g_debug_dump_last_slot = slot_index;
    g_debug_dump_last_type = burst_type;
}

uint8_t
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    (void)opts;
    (void)state;
    (void)cach_bits;
    g_cach_calls++;
    return 0;
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_reset_blocks_calls++;
}

void
dmr_alg_refresh(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_alg_refresh_calls++;
}

void
dmr_alg_reset(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_alg_reset_calls++;
}

void
dmr_refresh_algids_on_error(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_refresh_error_calls++;
}

void
hytera_enhanced_alg_refresh(dsd_state* state) {
    (void)state;
    g_hytera_refresh_calls++;
}

void
dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                           uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]) {
    (void)opts;
    (void)state;
    (void)vc;
    (void)ambe_fr;
    (void)ambe_fr2;
    (void)ambe_fr3;
    g_late_entry_calls++;
    g_late_entry_last_vc = vc;
}

void
dmr_sbrc(const dsd_opts* opts, dsd_state* state, uint8_t power) {
    (void)opts;
    (void)state;
    g_sbrc_calls++;
    g_sbrc_last_power = power;
}

void
dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    g_sm_voice_sync_calls++;
    g_sm_voice_sync_last_slot = slot;
}

void
dmr_sm_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_sm_tick_calls++;
}

void
dmr_confidence_reset(dsd_state* state) {
    (void)state;
    g_confidence_reset_calls++;
}

void
dmr_confidence_reset_slot(dsd_state* state, unsigned int slot) {
    (void)state;
    g_confidence_reset_slot_calls++;
    g_confidence_reset_last_slot = slot;
}

void
dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
    g_confidence_voice_sync_calls++;
}

dmr_confidence_result
dmr_confidence_note_voice_burst(dsd_state* state, unsigned int slot, unsigned int color_code) {
    (void)state;
    (void)slot;
    (void)color_code;
    g_confidence_voice_burst_calls++;
    return g_confidence_result;
}

int
dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
    return g_voice_slot_open;
}

int
dmr_confidence_any_voice_open(const dsd_state* state) {
    (void)state;
    return g_any_voice_open;
}

static void
test_bs_voice_sync_refreshes_when_trunk_alias_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    state.currentslot = 0;
    state.dmr_color_code = 16;
    load_voice_burst_stream();

    dmrBS(&opts, &state);

    assert(state.last_vc_sync_time > 0);
    assert(state.last_cc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
    assert(state.last_cc_sync_time_m > 0.0);
}

static void
test_bs_slot2_voice_routes_right_channel_and_post_skip_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    state.currentslot = 1;
    state.dmr_color_code = 16;
    load_single_burst_stream(1, DMR_BS_VOICE_SYNC);
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(state.dmrburstR == 16);
    assert(g_open_right_calls == 1U);
    assert(g_open_left_calls == 0U);
    assert(g_close_right_calls == 1U);
    assert(g_process_mbe_calls == 3U);
    assert(g_play_fs3_calls == 1U);
    assert(g_play_ss3_calls == 0U);
    assert(g_ui_redraw_calls == 1U);
    assert(g_history_calls[0] == 1U);
    assert(g_history_calls[1] == 1U);
    assert(g_current_calls[0] == 1U);
    assert(g_current_calls[1] == 1U);
    assert(g_debug_dump_calls == 1U);
    assert(g_debug_dump_last_slot == 1U);
    assert(g_debug_dump_last_type == 0x10U);
    assert(g_cach_calls == 1U);
    assert(g_late_entry_calls == 1U);
    assert(g_late_entry_last_vc == 1U);
    assert(g_sm_voice_sync_calls == 1U);
    assert(g_sm_voice_sync_last_slot == 1);
    assert(g_sm_tick_calls == 1U);
    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
}

static void
test_bs_slot2_voice_integer_output_uses_ss3_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1;
    state.dmr_color_code = 16;
    load_single_burst_stream(1, DMR_BS_VOICE_SYNC);
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(state.dmrburstR == 16);
    assert(g_play_fs3_calls == 0U);
    assert(g_play_ss3_calls == 1U);
    assert(g_process_mbe_calls == 3U);
    assert(g_sm_voice_sync_last_slot == 1);
}

static void
test_bs_data_sync_closes_slot_file_and_resets_error_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.mbe_out_fR = (FILE*)(uintptr_t)1;
    state.currentslot = 1;
    load_single_burst_stream(1, DMR_BS_DATA_SYNC);

    dmrBS(&opts, &state);

    assert(g_data_sync_calls == 1U);
    assert(g_close_right_calls == 1U);
    assert(opts.mbe_out_fR == NULL);
    assert(g_process_mbe_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_refresh_error_calls == 1U);
    assert(g_confidence_reset_calls == 1U);
}

static void
test_bs_confidence_reject_resets_slot_without_voice_decode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0;
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_confidence_result = DMR_CONFIDENCE_REJECT;

    dmrBS(&opts, &state);

    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
    assert(g_confidence_reset_slot_calls == 1U);
    assert(g_confidence_reset_last_slot == 0U);
    assert(g_debug_dump_calls == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_refresh_error_calls == 1U);
    assert(g_confidence_reset_calls == 1U);
}

static void
test_bs_voice_gate_closed_skips_decode_but_keeps_loop_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    state.currentslot = 0;
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_voice_slot_open = 0;

    dmrBS(&opts, &state);

    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
    assert(g_debug_dump_calls == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_ui_redraw_calls == 1U);
    assert(g_history_calls[0] == 1U);
    assert(g_history_calls[1] == 1U);
    assert(g_current_calls[0] == 1U);
    assert(g_current_calls[1] == 1U);
    assert(g_sm_tick_calls == 1U);
}

static void
test_bs_bootstrap_prefetched_voice_runs_first_frame_path(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    state.currentslot = 1;
    state.dmr_color_code = 16;
    state.dmr_payload_p = g_bootstrap_payload + 90U;
    load_bootstrap_voice_stream(1);
    g_any_voice_open = 1;

    dmrBSBootstrap(&opts, &state);

    assert(g_debug_dump_calls >= 1U);
    assert(g_alg_reset_calls == 1U);
    assert(g_process_mbe_calls >= 3U);
    assert(g_cach_calls >= 1U);
    assert(g_late_entry_calls >= 1U);
    assert(g_open_right_calls == 1U);
    assert(g_close_right_calls >= 1U);
    assert(g_play_fs3_calls >= 1U);
    assert(state.last_vc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
}

int
main(void) {
    test_bs_voice_sync_refreshes_when_trunk_alias_tuned();
    test_bs_slot2_voice_routes_right_channel_and_post_skip_hooks();
    test_bs_slot2_voice_integer_output_uses_ss3_playback();
    test_bs_data_sync_closes_slot_file_and_resets_error_state();
    test_bs_confidence_reject_resets_slot_without_voice_decode();
    test_bs_voice_gate_closed_skips_decode_but_keeps_loop_hooks();
    test_bs_bootstrap_prefetched_voice_runs_first_frame_path();
    printf("DMR BS sync times: OK\n");
    return 0;
}

// NOLINTEND(performance-no-int-to-ptr)
