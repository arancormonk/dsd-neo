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
#include <dsd-neo/core/power.h>
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

int
csvChanImport(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

int
csvKeyImportDec(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

int
csvKeyImportHex(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

int
dsd_tg_policy_reload_group_file(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return -1;
}

double
dB_to_pwr(double dB) {
    return dB;
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
    rc |= expect_double("rtl squelch level stored", opts.rtl_squelch_level, -12.5);
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
    return rc ? 1 : 0;
}
