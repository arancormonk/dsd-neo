// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for the DMR MS/direct-mode data collector.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_stream[4096];
static size_t g_stream_index;
static int g_skip_calls;
static int g_skip_total;
static int g_data_sync_calls;
static int g_data_sync_stereo;
static int g_data_sync_ms_mode;
static int g_data_sync_directmode;
static int g_data_sync_payload[144];
static int g_mark_vc_sync_calls;
static int g_fopen_private_calls;
static int g_process_mbe_calls;
static int g_play_fs3_calls;
static int g_play_ss3_calls;
static int g_ui_calls;
static int g_watchdog_history_calls;
static int g_watchdog_current_calls;
static int g_tyt_keystream_calls;
static int g_csi_keystream_calls;
static int g_data_burst_calls;
static int g_debug_dump_calls;
static int g_debug_format_calls;
static int g_alg_refresh_calls;
static int g_hytera_refresh_calls;
static int g_late_entry_calls;
static int g_sbrc_calls;
static int g_voice_sync_calls;
static int g_sm_tick_calls;

static void
reset_fixture(void) {
    g_stream_index = 0U;
    g_skip_calls = 0;
    g_skip_total = 0;
    g_data_sync_calls = 0;
    g_data_sync_stereo = 0;
    g_data_sync_ms_mode = 0;
    g_data_sync_directmode = 0;
    g_mark_vc_sync_calls = 0;
    g_fopen_private_calls = 0;
    g_process_mbe_calls = 0;
    g_play_fs3_calls = 0;
    g_play_ss3_calls = 0;
    g_ui_calls = 0;
    g_watchdog_history_calls = 0;
    g_watchdog_current_calls = 0;
    g_tyt_keystream_calls = 0;
    g_csi_keystream_calls = 0;
    g_data_burst_calls = 0;
    g_debug_dump_calls = 0;
    g_debug_format_calls = 0;
    g_alg_refresh_calls = 0;
    g_hytera_refresh_calls = 0;
    g_late_entry_calls = 0;
    g_sbrc_calls = 0;
    g_voice_sync_calls = 0;
    g_sm_tick_calls = 0;
    DSD_MEMSET(g_stream, 0, sizeof(g_stream));
    DSD_MEMSET(g_data_sync_payload, 0, sizeof(g_data_sync_payload));
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    assert(g_stream_index < (sizeof(g_stream) / sizeof(g_stream[0])));
    return g_stream[g_stream_index++] & 3;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_calls++;
    g_skip_total += count;
    g_stream_index += (size_t)count;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
getTimeC_buf(char out[9]) {
    DSD_SNPRINTF(out, 9, "%s", "00:00:00");
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_data_sync_calls++;
    g_data_sync_stereo = state->dmr_stereo;
    g_data_sync_ms_mode = state->dmr_ms_mode;
    g_data_sync_directmode = state->directmode;
    assert(strcmp(state->slot1light, "") == 0);
    assert(strcmp(state->slot2light, "") == 0);
    DSD_MEMCPY(g_data_sync_payload, state->dmr_stereo_payload, sizeof(g_data_sync_payload));
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
Hamming_7_4_decode(unsigned char* rx_bits) {
    (void)rx_bits;
    return true;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
QR_16_7_6_decode(unsigned char* rx_bits) {
    (void)rx_bits;
    return true;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_mark_vc_sync(dsd_state* state) {
    (void)state;
    g_mark_vc_sync_calls++;
}

FILE*
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    g_fopen_private_calls++;
    return NULL;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_process_mbe_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_fs3_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_ss3_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_telemetry_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_ui_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_watchdog_history_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_watchdog_current_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tyt16_ambe2_codeword_keystream(const dsd_state* state, char ambe_fr[4][24], int fnum) {
    (void)state;
    (void)ambe_fr;
    (void)fnum;
    g_tyt_keystream_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]) {
    (void)state;
    (void)ambe_fr;
    g_csi_keystream_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst) {
    (void)opts;
    (void)state;
    (void)info;
    (void)databurst;
    g_data_burst_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    (void)opts;
    (void)state;
    (void)slot_index;
    (void)burst_type;
    g_debug_dump_calls++;
}

size_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_debug_format_burst_payload(char* out, size_t out_size, const int payload[144], uint8_t slot_index,
                               uint8_t burst_type) {
    (void)payload;
    (void)slot_index;
    (void)burst_type;
    if (out_size == 0U) {
        return 0U;
    }
    g_debug_format_calls++;
    out[0] = '\0';
    return 0U;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_alg_refresh(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_alg_refresh_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
hytera_enhanced_alg_refresh(dsd_state* state) {
    (void)state;
    g_hytera_refresh_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                           uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]) {
    (void)opts;
    (void)state;
    (void)vc;
    (void)ambe_fr;
    (void)ambe_fr2;
    (void)ambe_fr3;
    g_late_entry_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sbrc(const dsd_opts* opts, dsd_state* state, uint8_t power) {
    (void)opts;
    (void)state;
    (void)power;
    g_sbrc_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    (void)slot;
    g_voice_sync_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_sm_tick_calls++;
}

static void
load_voice_stream(void) {
    for (size_t i = 0; i < (sizeof(g_stream) / sizeof(g_stream[0])); i++) {
        g_stream[i] = (int)(i & 3U);
    }
}

static void
prepare_state(dsd_state* state, int payload[90]) {
    DSD_MEMSET(state, 0, sizeof(*state));
    for (size_t i = 0; i < 90U; i++) {
        payload[i] = (int)(i & 3U);
    }
    state->dmr_payload_p = payload + 90;
    state->dmr_color_code = 16;
    state->dmr_stereo = 7;
    state->dmr_ms_mode = 8;
    state->directmode = 1;
    DSD_SNPRINTF(state->slot1light, sizeof(state->slot1light), "%s", "stale1");
    DSD_SNPRINTF(state->slot2light, sizeof(state->slot2light), "%s", "stale2");
}

static void
load_live_half(void) {
    for (size_t i = 0; i < 54U; i++) {
        g_stream[i] = (int)((i + 1U) & 3U);
    }
    for (size_t i = 54U; i < 264U; i++) {
        g_stream[i] = 3;
    }
}

static void
test_ms_data_collects_payload_and_cleans_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    reset_fixture();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload);
    load_live_half();

    dmrMSData(&opts, &state);

    assert(g_data_sync_calls == 1);
    assert(g_data_sync_stereo == 1);
    assert(g_data_sync_ms_mode == 1);
    assert(g_data_sync_directmode == 1);
    assert(g_stream_index == 264U);
    assert(state.dmr_stereo == 0);
    assert(state.dmr_ms_mode == 0);
    assert(state.directmode == 0);
    assert(g_data_sync_payload[0] == payload[0]);
    assert(g_data_sync_payload[89] == payload[89]);
    assert(g_data_sync_payload[90] == g_stream[0]);
    assert(g_data_sync_payload[143] == g_stream[53]);
    for (size_t i = 0; i < 144U; i++) {
        assert(state.dmr_stereo_payload[i] == 1);
    }
}

static void
test_ms_data_applies_inversion_to_cached_and_live_halves(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    reset_fixture();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload);
    load_live_half();
    opts.inverted_dmr = 1;

    dmrMSData(&opts, &state);

    assert(g_data_sync_calls == 1);
    assert(g_data_sync_payload[0] == ((payload[0] ^ 2) & 3));
    assert(g_data_sync_payload[1] == ((payload[1] ^ 2) & 3));
    assert(g_data_sync_payload[89] == ((payload[89] ^ 2) & 3));
    assert(g_data_sync_payload[90] == ((g_stream[0] ^ 2) & 3));
    assert(g_data_sync_payload[143] == ((g_stream[53] ^ 2) & 3));
    assert(state.dmr_stereo == 0);
    assert(state.dmr_ms_mode == 0);
    assert(state.directmode == 0);
}

static void
test_ms_voice_cycle_processes_frames_and_cleans_mode_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_fixture();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    load_voice_stream();

    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 2;
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    state.payload_algid = 0x02;
    state.static_ks_counter[0] = 7;
    state.vertex_ks_counter[0] = 8;
    state.vertex_ks_active_idx[0] = 2;
    state.vertex_ks_warned[0] = 1;
    state.tyt_bp = 1;
    state.csi_ee = 1;

    dmrMS(&opts, &state);

    assert(g_voice_sync_calls == 5);
    assert(g_process_mbe_calls == 15);
    assert(g_play_fs3_calls == 5);
    assert(g_play_ss3_calls == 0);
    assert(g_late_entry_calls == 5);
    assert(g_tyt_keystream_calls == 15);
    assert(g_csi_keystream_calls == 15);
    assert(g_data_burst_calls == 1);
    assert(g_sbrc_calls == 1);
    assert(g_alg_refresh_calls == 1);
    assert(g_hytera_refresh_calls == 1);
    assert(g_ui_calls == 4);
    assert(g_watchdog_history_calls == 4);
    assert(g_watchdog_current_calls == 4);
    assert(g_sm_tick_calls == 4);
    assert(g_mark_vc_sync_calls == 5);
    assert(g_debug_dump_calls == 5);
    assert(g_skip_calls == 5);
    assert(g_skip_total == 720);
    assert(g_stream_index == 1506U);
    assert(state.dmr_stereo == 0);
    assert(state.dmr_ms_mode == 0);
    assert(state.directmode == 0);
    assert(state.static_ks_counter[0] == 0);
    assert(state.vertex_ks_counter[0] == 0);
    assert(state.vertex_ks_active_idx[0] == -1);
    assert(state.vertex_ks_warned[0] == 0);
}

static void
test_ms_bootstrap_uses_cached_payload_then_enters_voice_cycle(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    reset_fixture();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload);
    load_voice_stream();

    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 2;
    opts.use_dsp_output = 1;
    opts.dmr_debug_burst = 1;
    opts.dmr_le = 2;
    state.dmr_color_code = 5;

    dmrMSBootstrap(&opts, &state);

    assert(g_voice_sync_calls == 6);
    assert(g_process_mbe_calls == 18);
    assert(g_play_fs3_calls == 0);
    assert(g_play_ss3_calls == 6);
    assert(g_late_entry_calls == 0);
    assert(g_debug_format_calls == 1);
    assert(g_fopen_private_calls == 6);
    assert(g_skip_calls == 6);
    assert(g_skip_total == 864);
    assert(g_stream_index == 1704U);
    assert(state.dmr_stereo == 0);
    assert(state.dmr_ms_mode == 0);
    assert(state.directmode == 0);
}

int
main(void) {
    test_ms_data_collects_payload_and_cleans_state();
    test_ms_data_applies_inversion_to_cached_and_live_halves();
    test_ms_voice_cycle_processes_frames_and_cleans_mode_state();
    test_ms_bootstrap_uses_cached_payload_then_enters_voice_cycle();
    DSD_FPRINTF(stdout, "DMR_MS_DATA: OK\n");
    return 0;
}
