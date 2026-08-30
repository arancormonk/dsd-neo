// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI menu service helpers.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "services.h"

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "dsd-neo/platform/sockets.h"

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

FILE*
dsd_fopen_existing_regular_file(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
}

int
dsd_fileno(FILE* fp) {
    (void)fp;
    return -1;
}

int
dsd_fstat(int fd, dsd_stat_t* st) {
    (void)fd;
    (void)st;
    return -1;
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    (void)path;
    (void)st;
    return -1;
}

int
dsd_stat_is_regular(const dsd_stat_t* st) {
    (void)st;
    return 0;
}

int
dsd_mkdir(const char* path, int mode) {
    (void)path;
    (void)mode;
    return -1;
}

int
dsd_setenv(const char* name, const char* value, int overwrite) {
    (void)name;
    (void)value;
    (void)overwrite;
    return 0;
}

void
parse_audio_output_string(dsd_opts* opts, char* input) {
    (void)opts;
    (void)input;
}

void
parse_audio_input_string(dsd_opts* opts, char* input) {
    (void)opts;
    (void)input;
}

SNDFILE*
open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    (void)dir;
    (void)temp_filename;
    (void)temp_filename_size;
    (void)sample_rate;
    (void)ext;
    return NULL;
}

void
openSymbolOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
closeSymbolOutFile(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->symbol_out_f = NULL;
    }
    (void)state;
}

void
openWavOutFileLR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openWavOutFileRaw(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

/*
 * Controllable importer stub: fills whichever state it is handed, the way the
 * real importer does, so the service's replace-vs-append behavior is observable.
 */
static int g_chan_import_result = -1;
static int g_chan_import_count = 0;
static uint32_t g_chan_import_chan[4];
static long int g_chan_import_freq[4];

int
csvChanImport(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (g_chan_import_result != 0 || !state) {
        return g_chan_import_result;
    }
    for (int i = 0; i < g_chan_import_count; i++) {
        dsd_state_set_trunk_chan_freq(state, g_chan_import_chan[i], g_chan_import_freq[i]);
        state->trunk_lcn_freq[state->lcn_freq_count++] = g_chan_import_freq[i];
    }
    return 0;
}

static int g_key_import_result = -1;

int
csvKeyImportDec(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return g_key_import_result;
}

int
csvKeyImportHex(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return g_key_import_result;
}

int
dsd_tg_policy_reload_group_file(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

/* Counts calls rather than returning a canned value: what svc_clear_group_list()
 * owes the caller is that the policy was asked to empty itself, and the emptying
 * itself belongs to CORE_TALKGROUP_POLICY. */
static int g_tg_policy_clear_calls = 0;

int
dsd_tg_policy_clear(dsd_state* state) {
    (void)state;
    g_tg_policy_clear_calls++;
    return 0;
}

/* The throwaway import state may carry module extensions; nothing under test
 * allocates one, so releasing it is a no-op here. */
void
dsd_state_ext_free_all(dsd_state* state) {
    (void)state;
}

int
dsd_config_expand_path(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }
    DSD_SNPRINTF(output, output_size, "%s", input);
    return 0;
}

void
init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop) {
    if (!event_struct) {
        return;
    }
    for (uint8_t i = start; i < stop; i++) {
        DSD_MEMSET(&event_struct->Event_History_Items[i], 0, sizeof(event_struct->Event_History_Items[i]));
        event_struct->Event_History_Items[i].color_pair = 4;
        event_struct->Event_History_Items[i].systype = -1;
        event_struct->Event_History_Items[i].subtype = -1;
    }
}

// Stands in for the core implementation, which also clears the per-slot commit bookkeeping
// (covered by CORE_CALL_ALERT_HISTORY). This test only asserts the service delegates the reset.
void
dsd_event_history_reset(dsd_state* state) {
    if (!state || !state->event_history_s) {
        return;
    }
    for (uint8_t slot = 0; slot < 2U; slot++) {
        init_event_history(&state->event_history_s[slot], 0, 255);
    }
}

dsd_socket_t
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return DSD_INVALID_SOCKET;
}

int
udp_socket_connect(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

int
udp_socket_connectA(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

int
io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    (void)freq;
    return -1;
}

static int g_p25_tick_guard_depth = 0;
static int g_p25_tick_guard_enter_calls = 0;
static int g_p25_tick_guard_leave_calls = 0;
static int g_p25_tick_guard_errors = 0;
static int g_rtl_lifecycle_outside_guard = 0;
static int g_rtl_stop_calls = 0;
static int g_rtl_destroy_calls = 0;
static int g_rtl_create_calls = 0;
static int g_rtl_start_calls = 0;
static int g_rtl_create_result = -1;
static int g_rtl_start_result = -1;
static int g_rtltcp_autotune_result = 0;

void
p25_sm_tick_guard_enter(void) {
    g_p25_tick_guard_enter_calls++;
    if (g_p25_tick_guard_depth != 0) {
        g_p25_tick_guard_errors++;
    }
    g_p25_tick_guard_depth++;
}

void
p25_sm_tick_guard_leave(void) {
    g_p25_tick_guard_leave_calls++;
    if (g_p25_tick_guard_depth != 1) {
        g_p25_tick_guard_errors++;
    }
    g_p25_tick_guard_depth--;
}

static void
note_rtl_lifecycle_call(void) {
    if (g_p25_tick_guard_depth != 1) {
        g_rtl_lifecycle_outside_guard++;
    }
}

int
rtl_stream_stop(RtlSdrContext* ctx) {
    (void)ctx;
    note_rtl_lifecycle_call();
    g_rtl_stop_calls++;
    return 0;
}

int
rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    note_rtl_lifecycle_call();
    g_rtl_destroy_calls++;
    return 0;
}

int
rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    (void)opts;
    note_rtl_lifecycle_call();
    g_rtl_create_calls++;
    if (out_ctx) {
        *out_ctx = g_rtl_create_result == 0 ? (RtlSdrContext*)out_ctx : NULL;
    }
    return g_rtl_create_result;
}

int
rtl_stream_start(RtlSdrContext* ctx) {
    (void)ctx;
    note_rtl_lifecycle_call();
    g_rtl_start_calls++;
    return g_rtl_start_result;
}

int
rtl_stream_request_ppm(dsd_opts* opts, int ppm) {
    if (!opts) {
        return -1;
    }
    if (ppm < -200) {
        ppm = -200;
    }
    if (ppm > 200) {
        ppm = 200;
    }
    opts->rtlsdr_ppm_error = ppm;
    return 0;
}

void
rtl_stream_set_channel_squelch(float level) {
    (void)level;
}

int
rtl_stream_set_bias_tee(int on) {
    (void)on;
    return 0;
}

int
rtl_stream_set_rtltcp_autotune(int onoff) {
    (void)onoff;
    return g_rtltcp_autotune_result;
}

void
rtl_stream_set_auto_ppm(int onoff) {
    (void)onoff;
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
expect_uint(const char* tag, unsigned got, unsigned want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_ull(const char* tag, unsigned long long got, unsigned long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %llu want %llu\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_double(const char* tag, double got, double want) {
    if (fabs(got - want) > 1.0e-9) {
        DSD_FPRINTF(stderr, "%s: got %.3f want %.3f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_mute_and_protocol_inversion_toggles(void) {
    int rc = 0;
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    rc |= expect_int("all mutes null guard", svc_toggle_all_mutes(NULL), -1);
    opts.unmute_encrypted_p25 = 0;
    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 0;
    rc |= expect_int("all mutes toggle success", svc_toggle_all_mutes(&opts), 0);
    rc |= expect_int("p25 unmute toggled", opts.unmute_encrypted_p25, 1);
    rc |= expect_int("dmr left mute toggled", opts.dmr_mute_encL, 0);
    rc |= expect_int("dmr right mute toggled", opts.dmr_mute_encR, 1);

    opts.inverted_dmr = 0;
    opts.inverted_dpmr = 1;
    opts.inverted_x2tdma = 1;
    opts.inverted_ysf = 0;
    opts.inverted_m17 = 1;
    svc_toggle_inv_dmr(&opts);
    svc_toggle_inv_dpmr(&opts);
    svc_toggle_inv_x2(&opts);
    svc_toggle_inv_m17(&opts);
    rc |= expect_int("dmr protocol inversion toggled independently", opts.inverted_dmr, 1);
    rc |= expect_int("dpmr protocol inversion toggled independently", opts.inverted_dpmr, 0);
    rc |= expect_int("x2 protocol inversion toggled independently", opts.inverted_x2tdma, 0);
    rc |= expect_int("m17 protocol inversion toggled independently", opts.inverted_m17, 0);
    rc |= expect_int("ysf inversion unchanged by specific toggles", opts.inverted_ysf, 0);

    return rc;
}

static int
test_lrrp_event_log_and_history_state(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I histories[2];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(histories, 0, sizeof(histories));

    rc |= expect_int("lrrp home null opts", svc_lrrp_set_home(NULL), -1);
    rc |= expect_int("lrrp home success", svc_lrrp_set_home(&opts), 0);
    rc |= expect_int("lrrp home enables output", opts.lrrp_file_output, 1);
    rc |= expect_str("lrrp home path expanded", opts.lrrp_out_file, "~/lrrp.txt");
    rc |= expect_int("lrrp custom null opts", svc_lrrp_set_custom(NULL, "x.lrrp"), -1);
    rc |= expect_int("lrrp custom empty path", svc_lrrp_set_custom(&opts, ""), -1);
    rc |= expect_int("lrrp dsdp success", svc_lrrp_set_dsdp(&opts), 0);
    rc |= expect_int("lrrp dsdp enables output", opts.lrrp_file_output, 1);
    rc |= expect_str("lrrp dsdp path", opts.lrrp_out_file, "DSDPlus.LRRP");
    rc |= expect_int("lrrp custom success", svc_lrrp_set_custom(&opts, "custom.lrrp"), 0);
    rc |= expect_str("lrrp custom path", opts.lrrp_out_file, "custom.lrrp");
    svc_lrrp_disable(&opts);
    rc |= expect_int("lrrp disable clears flag", opts.lrrp_file_output, 0);
    rc |= expect_int("lrrp disable clears path", opts.lrrp_out_file[0], '\0');

    rc |= expect_int("event log null opts", svc_set_event_log(NULL, "events.log"), -1);
    rc |= expect_int("event log empty path", svc_set_event_log(&opts, ""), -1);
    rc |= expect_int("event log valid path", svc_set_event_log(&opts, "events.log"), 0);
    rc |= expect_str("event log stores path", opts.event_out_file, "events.log");
    svc_disable_event_log(&opts);
    rc |= expect_int("event log disable clears path", opts.event_out_file[0], '\0');

    DSD_SNPRINTF(histories[0].Event_History_Items[0].event_string,
                 sizeof(histories[0].Event_History_Items[0].event_string), "%s", "slot0");
    histories[0].Event_History_Items[0].source_id = 1234U;
    DSD_SNPRINTF(histories[1].Event_History_Items[254].event_string,
                 sizeof(histories[1].Event_History_Items[254].event_string), "%s", "slot1");
    histories[1].Event_History_Items[254].source_id = 5678U;
    state.event_history_s = histories;
    svc_reset_event_history(&state);
    rc |= expect_int("history slot0 event reset", histories[0].Event_History_Items[0].event_string[0], '\0');
    rc |= expect_uint("history slot0 source reset", histories[0].Event_History_Items[0].source_id, 0U);
    rc |= expect_int("history slot1 event reset", histories[1].Event_History_Items[254].event_string[0], '\0');
    rc |= expect_uint("history slot1 source reset", histories[1].Event_History_Items[254].source_id, 0U);

    return rc;
}

static int
test_p2_trunking_and_slot_controls(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    svc_set_p2_params(&state, 0xFFFFFFULL, 0xFFFFULL, 0xFFFFULL);
    rc |= expect_ull("p2 wacn clamps to 20 bits", state.p2_wacn, 0xFFFFFULL);
    rc |= expect_ull("p2 sysid clamps to 12 bits", state.p2_sysid, 0xFFFULL);
    rc |= expect_ull("p2 cc clamps to 12 bits", state.p2_cc, 0xFFFULL);
    rc |= expect_int("p2 hardset enabled when all fields nonzero", state.p2_hardset, 1);
    svc_set_p2_params(&state, 0, 1, 1);
    rc |= expect_int("p2 hardset disabled when any field zero", state.p2_hardset, 0);

    svc_set_tg_hold(&state, 65535U);
    rc |= expect_uint("tg hold stored", state.tg_hold, 65535U);
    svc_set_hangtime(&opts, -2.5);
    rc |= expect_double("negative hangtime clamps to zero", opts.trunk_hangtime, 0.0);
    svc_set_hangtime(&opts, 3.25);
    rc |= expect_double("positive hangtime stored", opts.trunk_hangtime, 3.25);

    svc_set_rigctl_setmod_bw(&opts, -10);
    rc |= expect_int("negative setmod bandwidth clamps low", opts.setmod_bw, 0);
    svc_set_rigctl_setmod_bw(&opts, 99999);
    rc |= expect_int("setmod bandwidth clamps high", opts.setmod_bw, 25000);
    svc_set_rigctl_setmod_bw(&opts, 12500);
    rc |= expect_int("setmod bandwidth stores in range", opts.setmod_bw, 12500);

    svc_toggle_reverse_mute(&opts);
    svc_toggle_lcw_retune(&opts);
    svc_toggle_dmr_le(&opts);
    rc |= expect_int("reverse mute toggled", opts.reverse_mute, 1);
    rc |= expect_int("lcw retune toggled", opts.p25_lcw_retune, 1);
    rc |= expect_int("dmr little-endian toggled", opts.dmr_le, 1);

    svc_set_slot_pref(&opts, -1);
    rc |= expect_int("slot preference clamps low", opts.slot_preference, 0);
    svc_set_slot_pref(&opts, 2);
    rc |= expect_int("slot preference stores auto", opts.slot_preference, 2);
    svc_set_slot_pref(&opts, 42);
    rc |= expect_int("slot preference clamps high", opts.slot_preference, 2);
    svc_set_slots_onoff(&opts, 1);
    rc |= expect_int("slot mask enables slot1", opts.slot1_on, 1);
    rc |= expect_int("slot mask disables slot2", opts.slot2_on, 0);
    svc_set_slots_onoff(&opts, 2);
    rc |= expect_int("slot mask disables slot1", opts.slot1_on, 0);
    rc |= expect_int("slot mask enables slot2", opts.slot2_on, 1);
    svc_set_slots_onoff(&opts, 3);
    rc |= expect_int("slot mask enables both slot1", opts.slot1_on, 1);
    rc |= expect_int("slot mask enables both slot2", opts.slot2_on, 1);

    return rc;
}

static int
test_payload_symbol_and_pulse_state(void) {
    int rc = 0;
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    rc |= expect_int("pulse output null opts", svc_set_pulse_output(NULL, "1"), -1);
    rc |= expect_int("pulse input null opts", svc_set_pulse_input(NULL, "1"), -1);
    rc |= expect_int("pulse output valid index", svc_set_pulse_output(&opts, "2"), 0);
    rc |= expect_str("pulse output device set", opts.audio_out_dev, "pulse");
    rc |= expect_int("pulse output type set", opts.audio_out_type, 0);
    rc |= expect_int("pulse input valid index", svc_set_pulse_input(&opts, "3"), 0);
    rc |= expect_str("pulse input device set", opts.audio_in_dev, "pulse");
    rc |= expect_int("pulse input type set", opts.audio_in_type, AUDIO_IN_PULSE);

    return rc;
}

#ifdef USE_RADIO
static void
reset_rtl_restart_stubs(void) {
    g_p25_tick_guard_depth = 0;
    g_p25_tick_guard_enter_calls = 0;
    g_p25_tick_guard_leave_calls = 0;
    g_p25_tick_guard_errors = 0;
    g_rtl_lifecycle_outside_guard = 0;
    g_rtl_stop_calls = 0;
    g_rtl_destroy_calls = 0;
    g_rtl_create_calls = 0;
    g_rtl_start_calls = 0;
    g_rtl_create_result = -1;
    g_rtl_start_result = -1;
}

static int
test_rtl_restart_quiesces_p25_retunes(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_rtl_restart_stubs();

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_started = 1;
    opts.rtl_needs_restart = 1;
    state.rtl_ctx = (RtlSdrContext*)&state;

    rc |= expect_int("guarded rtl restart reports create failure", svc_rtl_restart(&opts, &state), -1);
    rc |= expect_int("rtl restart enters P25 tune guard", g_p25_tick_guard_enter_calls, 1);
    rc |= expect_int("rtl restart leaves P25 tune guard", g_p25_tick_guard_leave_calls, 1);
    rc |= expect_int("rtl restart balances P25 tune guard", g_p25_tick_guard_depth, 0);
    rc |= expect_int("rtl restart uses P25 tune guard correctly", g_p25_tick_guard_errors, 0);
    rc |= expect_int("rtl lifecycle stays inside P25 tune guard", g_rtl_lifecycle_outside_guard, 0);
    rc |= expect_int("guarded rtl restart stops old stream", g_rtl_stop_calls, 1);
    rc |= expect_int("guarded rtl restart destroys old stream", g_rtl_destroy_calls, 1);
    rc |= expect_int("guarded rtl restart attempts replacement", g_rtl_create_calls, 1);
    rc |= expect_int("guarded rtl restart does not start failed replacement", g_rtl_start_calls, 0);
    rc |= expect_int("failed guarded rtl restart clears context", state.rtl_ctx == NULL, 1);

    reset_rtl_restart_stubs();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    g_rtl_create_result = 0;
    g_rtl_start_result = -1;

    rc |= expect_int("guarded rtl restart reports start failure", svc_rtl_restart(&opts, &state), -1);
    rc |= expect_int("start failure leaves P25 tune guard", g_p25_tick_guard_leave_calls, 1);
    rc |= expect_int("start failure balances P25 tune guard", g_p25_tick_guard_depth, 0);
    rc |= expect_int("start failure lifecycle stays guarded", g_rtl_lifecycle_outside_guard, 0);
    rc |= expect_int("start failure destroys replacement", g_rtl_destroy_calls, 1);
    rc |= expect_int("start failure clears replacement context", state.rtl_ctx == NULL, 1);

    reset_rtl_restart_stubs();
    return rc;
}

static int
test_rtl_service_option_contracts(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    rc |= expect_int("rtl enable null opts", svc_rtl_enable_input(NULL, &state), -1);
    rc |= expect_int("rtl enable null state", svc_rtl_enable_input(&opts, NULL), -1);
    rc |= expect_int("rtl enable restart failure", svc_rtl_enable_input(&opts, &state), -1);
    rc |= expect_int("rtl enable selects rtl input before restart", opts.audio_in_type, AUDIO_IN_RTL);
    rc |= expect_int("rtl enable leaves stream stopped after create failure", opts.rtl_started, 0);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=rtlsdr");
    rc |= expect_int("soapy dev index is unsupported", svc_rtl_set_dev_index(&opts, &state, 2), DSD_ERR_NOT_SUPPORTED);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl");
    rc |= expect_int("rtl dev index clamps low", svc_rtl_set_dev_index(&opts, &state, -3), 0);
    rc |= expect_int("rtl dev index stored", opts.rtl_dev_index, 0);
    rc |= expect_int("rtl dev index marks restart", opts.rtl_needs_restart, 1);

    opts.audio_in_type = AUDIO_IN_RTL;
    rc |= expect_int("active rtl dev index returns restart failure", svc_rtl_set_dev_index(&opts, &state, 4), -1);
    rc |= expect_int("active rtl dev index stores requested index", opts.rtl_dev_index, 4);
    rc |= expect_int("active rtl restart clears pending flag before create failure", opts.rtl_needs_restart, 0);

    opts.audio_in_type = 0;
    rc |= expect_int("rtl gain clamps high", svc_rtl_set_gain(&opts, &state, 99), 0);
    rc |= expect_int("rtl gain stored", opts.rtl_gain_value, 49);
    rc |= expect_int("rtl bandwidth invalid defaults", svc_rtl_set_bandwidth(&opts, &state, 7), 0);
    rc |= expect_int("rtl bandwidth default stored", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_int("rtl bandwidth valid stored", svc_rtl_set_bandwidth(&opts, &state, 12), 0);
    rc |= expect_int("rtl bandwidth exact stored", opts.rtl_dsp_bw_khz, 12);

    rc |= expect_int("rtl squelch stores converted threshold", svc_rtl_set_sql_db(&opts, -12.5), 0);
    rc |= expect_double("rtl squelch level stored", opts.rtl_squelch_level, pow(10.0, -1.25));

    /* 0 dB is full scale: as a threshold it closes the gate forever, so it is the
     * natural spelling of "off" and matches what 0 means in the CLI and config.
     * Without it neither UI could switch the squelch off at all. */
    rc |= expect_int("rtl squelch accepts zero", svc_rtl_set_sql_db(&opts, 0.0), 0);
    rc |= expect_double("rtl squelch zero switches off", opts.rtl_squelch_level, 0.0);
    rc |= expect_int("rtl squelch accepts positive", svc_rtl_set_sql_db(&opts, 3.0), 0);
    rc |= expect_double("rtl squelch positive switches off", opts.rtl_squelch_level, 0.0);
    rc |= expect_int("rtl volume invalid defaults", svc_rtl_set_volume_mult(&opts, -1), 0);
    rc |= expect_int("rtl volume default stored", opts.rtl_volume_multiplier, 1);
    rc |= expect_int("rtl volume valid stored", svc_rtl_set_volume_mult(&opts, 3), 0);
    rc |= expect_int("rtl volume exact stored", opts.rtl_volume_multiplier, 3);

    state.rtl_ctx = (RtlSdrContext*)&state;
    rc |= expect_int("rtl bias tee live apply", svc_rtl_set_bias_tee(&opts, &state, 7), 0);
    rc |= expect_int("rtl bias tee boolean stored", opts.rtl_bias_tee, 1);
    g_rtltcp_autotune_result = -1;
    rc |= expect_int("rtltcp autotune live failure", svc_rtltcp_set_autotune(&opts, &state, 1), -1);
    rc |= expect_int("rtltcp autotune failure preserves state", opts.rtltcp_autotune, 0);
    g_rtltcp_autotune_result = 0;
    rc |= expect_int("rtltcp autotune live apply", svc_rtltcp_set_autotune(&opts, &state, 1), 0);
    rc |= expect_int("rtltcp autotune stored", opts.rtltcp_autotune, 1);
    rc |= expect_int("rtl auto ppm live apply", svc_rtl_set_auto_ppm(&opts, &state, 0), 0);
    rc |= expect_int("rtl auto ppm stored", opts.rtl_auto_ppm, 0);

    return rc;
}
#endif

static int
test_file_network_and_import_failure_contracts(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    rc |= expect_int("symbol out empty path", svc_open_symbol_out(&opts, &state, ""), -1);
    rc |= expect_int("symbol out failed open", svc_open_symbol_out(&opts, &state, "capture.sym"), -1);
    rc |= expect_str("symbol out path still stored", opts.symbol_out_file, "capture.sym");

    rc |= expect_int("symbol in missing file", svc_open_symbol_in(&opts, &state, "missing.sym"), -1);
    rc |= expect_int("symbol in missing leaves type unchanged", opts.audio_in_type, 0);
    rc |= expect_int("static wav invalid path", svc_open_static_wav(&opts, &state, ""), -1);
    rc |= expect_int("static wav failed open", svc_open_static_wav(&opts, &state, "static.wav"), -1);
    rc |= expect_str("static wav path stored before open", opts.wav_out_file, "static.wav");
    rc |= expect_int("static wav mode marked", opts.static_wav_file, 1);
    rc |= expect_int("raw wav failed open", svc_open_raw_wav(&opts, &state, "raw.wav"), -1);
    rc |= expect_str("raw wav path stored before open", opts.wav_out_file_raw, "raw.wav");

    rc |= expect_int("dsp output empty name", svc_set_dsp_output_file(&opts, ""), -1);
    rc |= expect_int("dsp output valid name", svc_set_dsp_output_file(&opts, "symbols.dsp"), 0);
    rc |= expect_str("dsp output path uses DSP directory", opts.dsp_out_file, "./DSP/symbols.dsp");
    rc |= expect_int("dsp output flag set", opts.use_dsp_output, 1);

    rc |= expect_int("udp output invalid port", svc_udp_output_config(&opts, &state, "127.0.0.1", 0), -1);
    rc |= expect_int("udp output socket failure", svc_udp_output_config(&opts, &state, "239.0.0.1", 23456), -1);
    rc |= expect_str("udp output host stored before connect", opts.udp_hostname, "239.0.0.1");
    rc |= expect_int("udp output port stored before connect", opts.udp_portno, 23456);
    rc |= expect_int("udp output type not enabled on connect failure", opts.audio_out_type, 0);

    rc |= expect_int("rigctl invalid port", svc_rigctl_connect(&opts, "localhost", 0), -1);
    rc |= expect_int("rigctl connect failure", svc_rigctl_connect(&opts, "rig.local", 4532), -1);
    rc |= expect_str("rigctl host stored before connect", opts.rigctlhostname, "rig.local");
    rc |= expect_int("rigctl port stored before connect", opts.rigctlportno, 4532);
    rc |= expect_int("rigctl socket invalid after connect failure", opts.rigctl_sockfd, DSD_INVALID_SOCKET);
    rc |= expect_int("rigctl disabled after connect failure", opts.use_rigctl, 0);

    g_chan_import_result = -1;
    rc |= expect_int("channel import failure", svc_import_channel_map(&opts, &state, "channels.csv"), -1);
    rc |= expect_str("channel import path stored", opts.chan_in_file, "channels.csv");
    rc |= expect_int("group import failure", svc_import_group_list(&opts, &state, "groups.csv"), -1);
    rc |= expect_str("group import path stored", opts.group_in_file, "groups.csv");
    rc |= expect_int("keys dec import failure", svc_import_keys_dec(&opts, &state, "keys.csv"), -1);
    rc |= expect_str("keys dec import path stored", opts.key_in_file, "keys.csv");
    rc |= expect_int("keys hex import failure", svc_import_keys_hex(&opts, &state, "keys.hex"), -1);
    rc |= expect_str("keys hex import path stored", opts.key_in_file, "keys.hex");

    return rc;
}

static int
test_channel_map_reimport_replaces_previous_map(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    g_chan_import_result = 0;
    g_chan_import_count = 3;
    g_chan_import_chan[0] = 101;
    g_chan_import_freq[0] = 851000000L;
    g_chan_import_chan[1] = 102;
    g_chan_import_freq[1] = 852000000L;
    g_chan_import_chan[2] = 103;
    g_chan_import_freq[2] = 853000000L;
    rc |= expect_int("chan map first import ok", svc_import_channel_map(&opts, &state, "a.csv"), 0);
    rc |= expect_int("first import lcn count", state.lcn_freq_count, 3);
    rc |= expect_int("first import chan applied", (int)state.trunk_chan_map[101], 851000000);
    rc |= expect_int("first import used count", (int)state.trunk_chan_map_used_count, 3);

    g_chan_import_count = 2;
    g_chan_import_chan[0] = 201;
    g_chan_import_freq[0] = 860000000L;
    g_chan_import_chan[1] = 202;
    g_chan_import_freq[1] = 861000000L;
    rc |= expect_int("chan map reimport ok", svc_import_channel_map(&opts, &state, "b.csv"), 0);
    rc |= expect_int("reimport replaces lcn count", state.lcn_freq_count, 2);
    rc |= expect_int("reimport clears stale chan", (int)state.trunk_chan_map[101], 0);
    rc |= expect_int("reimport applies new chan", (int)state.trunk_chan_map[201], 860000000);
    rc |= expect_int("reimport replaces used count", (int)state.trunk_chan_map_used_count, 2);
    rc |= expect_int("reimport replaces lcn list", (int)state.trunk_lcn_freq[0], 860000000);

    g_chan_import_result = -1;
    rc |= expect_int("failed reimport rc", svc_import_channel_map(&opts, &state, "bad.csv"), -1);
    rc |= expect_int("failed reimport preserves lcn count", state.lcn_freq_count, 2);
    rc |= expect_int("failed reimport preserves chan", (int)state.trunk_chan_map[201], 860000000);
    rc |= expect_str("failed reimport still stores path", opts.chan_in_file, "bad.csv");

    // The importer reports success for any file it can open, so a mispicked CSV
    // parses to an empty map. Adopting that would replace the live map with
    // zeros; the service has to refuse instead.
    g_chan_import_result = 0;
    g_chan_import_count = 0;
    rc |= expect_int("empty import refused", svc_import_channel_map(&opts, &state, "talkgroups.csv"), -1);
    rc |= expect_int("empty import preserves chan", (int)state.trunk_chan_map[201], 860000000);
    rc |= expect_int("empty import preserves lcn count", state.lcn_freq_count, 2);

    // Trunk scan owns per-target maps; a global runtime import would wipe them.
    opts.trunk_scan_enabled = 1;
    g_chan_import_count = 1;
    g_chan_import_chan[0] = 301;
    g_chan_import_freq[0] = 870000000L;
    rc |= expect_int("trunk-scan import refused", svc_import_channel_map(&opts, &state, "scan.csv"), -1);
    rc |= expect_int("trunk-scan import preserves chan", (int)state.trunk_chan_map[201], 860000000);
    opts.trunk_scan_enabled = 0;

    // dmr_lcn_trust is provenance for the map, not a separate table: a stale
    // "learned on the CC" byte would authorize an off-CC tune to a frequency
    // only the new CSV asserts. lcn_freq_roll indexes the replaced LCN list.
    state.dmr_lcn_trust[201] = 2;
    state.lcn_freq_roll = 1;
    g_chan_import_count = 1;
    g_chan_import_chan[0] = 401;
    g_chan_import_freq[0] = 880000000L;
    rc |= expect_int("adopt ok", svc_import_channel_map(&opts, &state, "c.csv"), 0);
    rc |= expect_int("adopt clears stale lcn trust", (int)state.dmr_lcn_trust[201], 0);
    rc |= expect_int("adopt restarts the lcn roll", state.lcn_freq_roll, 0);

    return rc;
}

/*
 * Runtime key import has to arm the keyring the way -k/-K does: every consumer
 * of rkey_array gates on keyloader, so without this the rows load and nothing
 * ever uses them while the UI reports success.
 */
static int
test_key_import_arms_keyloader(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    g_key_import_result = -1;
    rc |= expect_int("dec key import failure rc", svc_import_keys_dec(&opts, &state, "keys.csv"), -1);
    rc |= expect_int("failed dec key import leaves keyloader off", state.keyloader, 0);

    g_key_import_result = 0;
    rc |= expect_int("dec key import ok", svc_import_keys_dec(&opts, &state, "keys.csv"), 0);
    rc |= expect_int("dec key import arms keyloader", state.keyloader, 1);

    state.keyloader = 0;
    rc |= expect_int("hex key import ok", svc_import_keys_hex(&opts, &state, "keys.hex"), 0);
    rc |= expect_int("hex key import arms keyloader", state.keyloader, 1);

    g_key_import_result = -1;
    return rc;
}

/*
 * Unloading. A frontend whose system can deselect a CSV needs a way to say
 * "none": every importer takes a path and rejects an empty one, so without
 * these the deselected file stays live for the rest of the session.
 */
static int
test_clear_services_unload_what_the_importers_loaded(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    g_chan_import_result = 0;
    g_chan_import_count = 2;
    g_chan_import_chan[0] = 101;
    g_chan_import_freq[0] = 851000000L;
    g_chan_import_chan[1] = 102;
    g_chan_import_freq[1] = 852000000L;
    rc |= expect_int("clear: seed import ok", svc_import_channel_map(&opts, &state, "a.csv"), 0);
    state.dmr_lcn_trust[101] = 2;
    state.lcn_freq_roll = 1;
    const unsigned int seq_before = state.trunk_chan_map_seq;

    rc |= expect_int("chan map clear ok", svc_clear_channel_map(&opts, &state), 0);
    rc |= expect_str("chan map clear forgets the path", opts.chan_in_file, "");
    rc |= expect_int("chan map clear empties the map", (int)state.trunk_chan_map[101], 0);
    rc |= expect_int("chan map clear empties used count", (int)state.trunk_chan_map_used_count, 0);
    rc |= expect_int("chan map clear empties lcn list", state.lcn_freq_count, 0);
    rc |= expect_int("chan map clear restarts the lcn roll", state.lcn_freq_roll, 0);
    // Provenance goes with the map for the same reason it does on an adopt: a
    // surviving trust byte authorizes an off-CC tune to a frequency now gone.
    rc |= expect_int("chan map clear drops lcn trust", (int)state.dmr_lcn_trust[101], 0);
    rc |= expect_int("chan map clear advances the map seq", (int)(state.trunk_chan_map_seq != seq_before), 1);

    // Trunk scan owns per-target maps; emptying a global one is not this
    // frontend's call, exactly as on the import side.
    opts.trunk_scan_enabled = 1;
    rc |= expect_int("trunk-scan chan map clear refused", svc_clear_channel_map(&opts, &state), -1);
    opts.trunk_scan_enabled = 0;

    DSD_SNPRINTF(opts.group_in_file, sizeof opts.group_in_file, "%s", "groups.csv");
    const int clears_before = g_tg_policy_clear_calls;
    rc |= expect_int("group list clear ok", svc_clear_group_list(&opts, &state), 0);
    rc |= expect_str("group list clear forgets the path", opts.group_in_file, "");
    rc |= expect_int("group list clear empties the policy", g_tg_policy_clear_calls - clears_before, 1);

    g_key_import_result = 0;
    rc |= expect_int("clear: seed key import ok", svc_import_keys_dec(&opts, &state, "keys.csv"), 0);
    state.rkey_array[7] = 0x1234ULL;
    state.rkey_array_loaded[7] = 1U;
    state.dmr_tg_key_map_tg[0] = 123U;
    state.dmr_tg_key_map_kid[0] = 0x7B;
    state.dmr_tg_key_map_count = 1;
    state.dmr_tg_key_note_epoch[0] = 42U;
    state.dmr_tg_key_skip_epoch[0] = 43U;
    rc |= expect_int("keys clear ok", svc_clear_keys(&opts, &state), 0);
    rc |= expect_str("keys clear forgets the path", opts.key_in_file, "");
    rc |= expect_int("keys clear empties the keyring", (int)state.rkey_array[7], 0);
    rc |= expect_int("keys clear marks the slot unloaded", (int)state.rkey_array_loaded[7], 0);
    // Disarmed, or every consumer keeps treating the zeroed array as loaded keys.
    rc |= expect_int("keys clear disarms the keyloader", state.keyloader, 0);
    // The TG->key ID override map indexes into the keyring, so it goes with it.
    rc |= expect_int("keys clear empties the tg key map", state.dmr_tg_key_map_count, 0);
    rc |= expect_int("keys clear drops the tg key notice latch", (int)state.dmr_tg_key_note_epoch[0], 0);
    rc |= expect_int("keys clear drops the tg key skip latch", (int)state.dmr_tg_key_skip_epoch[0], 0);

    g_chan_import_result = -1;
    g_key_import_result = -1;
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_mute_and_protocol_inversion_toggles();
    rc |= test_lrrp_event_log_and_history_state();
    rc |= test_p2_trunking_and_slot_controls();
    rc |= test_payload_symbol_and_pulse_state();
#ifdef USE_RADIO
    rc |= test_rtl_restart_quiesces_p25_retunes();
    rc |= test_rtl_service_option_contracts();
#endif
    rc |= test_file_network_and_import_failure_contracts();
    rc |= test_channel_map_reimport_replaces_previous_map();
    rc |= test_key_import_arms_keyloader();
    rc |= test_clear_services_unload_what_the_importers_loaded();
    return rc ? 1 : 0;
}
