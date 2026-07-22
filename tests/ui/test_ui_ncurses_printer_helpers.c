// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Pure-helper checks for the ncurses dashboard printer.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/app_control/history.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/ui/menu_core.h>
#include <dsd-neo/ui/ncurses.h>
#include <dsd-neo/ui/ncurses_dsp_display.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ncurses_p25_display.h>
#include <dsd-neo/ui/ncurses_trunk_display.h>
#include <dsd-neo/ui/panels.h>
#include <dsd-neo/ui/ui_async.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/call_state.h"
#include "dsd-neo/core/dsd_time.h"
#include "dsd-neo/platform/platform.h"

int ncurses_last_synctype;
WINDOW* stdscr;
unsigned long long int edacs_channel_tree[33][6];

static char g_printw_capture[4096];
static size_t g_printw_capture_len;

static void
reset_printw_capture(void) {
    DSD_MEMSET(g_printw_capture, 0, sizeof(g_printw_capture));
    g_printw_capture_len = 0U;
}

static void append_printw_capture(const char* fmt, va_list ap) DSD_ATTR_FORMAT(printf, 1, 0);

static void
append_printw_capture(const char* fmt, va_list ap) {
    if (!fmt || g_printw_capture_len >= sizeof(g_printw_capture) - 1U) {
        return;
    }
    size_t remaining = sizeof(g_printw_capture) - g_printw_capture_len;
    int wrote = DSD_VSNPRINTF(g_printw_capture + g_printw_capture_len, remaining, fmt, ap);
    if (wrote <= 0) {
        return;
    }
    if ((size_t)wrote >= remaining) {
        g_printw_capture_len = sizeof(g_printw_capture) - 1U;
    } else {
        g_printw_capture_len += (size_t)wrote;
    }
}

static void
assert_capture_contains(const char* needle) {
    assert(needle != NULL);
    assert(strstr(g_printw_capture, needle) != NULL);
}

int
printw(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    va_list ap;
    va_start(ap, fmt);
    append_printw_capture(fmt, ap);
    va_end(ap);
    return 0;
}

int
wprintw(WINDOW* win, const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)fmt;
    return 0;
}

int
waddch(WINDOW* win, const chtype ch) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)ch;
    return 0;
}

int
waddnstr(WINDOW* win, const char* str, int n) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)str;
    (void)n;
    return 0;
}

int
wattr_on(WINDOW* win, attr_t attrs, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)attrs;
    (void)opts;
    return 0;
}

int
wattr_off(WINDOW* win, attr_t attrs, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)attrs;
    (void)opts;
    return 0;
}

int
werase(WINDOW* win) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    return 0;
}

int
wmove(WINDOW* win, int y, int x) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)y;
    (void)x;
    return 0;
}

int
whline(WINDOW* win, chtype ch, int n) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)ch;
    (void)n;
    return 0;
}

int
getmaxx(const WINDOW* win) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    return 80;
}

int
getmaxy(const WINDOW* win) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    return 24;
}

int
getcurx(const WINDOW* win) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    return 0;
}

int
getcury(const WINDOW* win) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    return 0;
}

int
ui_is_thread_context(void) { // NOLINT(misc-use-internal-linkage)
    return 1;
}

int
ui_is_locked_from_label(const dsd_state* state, const char* label) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    return label != NULL && strstr(label, "Locked") != NULL;
}

int
ui_is_transient_enc_locked_from_label(const dsd_state* state, const char* label) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    return label != NULL && strstr(label, "TransientEnc") != NULL;
}

int
dsd_tg_policy_lookup_label(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                           size_t name_sz) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    if (id != 1234U) {
        return 0;
    }
    if (mode && mode_sz > 0U) {
        DSD_SNPRINTF(mode, mode_sz, "A");
    }
    if (name && name_sz > 0U) {
        DSD_SNPRINTF(name, name_sz, "Dispatch");
    }
    return 1;
}

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    return mean_power;
}

const char*
dsd_input_level_status_label(dsd_input_level_status status) { // NOLINT(misc-use-internal-linkage)
    (void)status;
    return "ok";
}

const char*
dsd_input_level_display_label(dsd_input_level_source source) { // NOLINT(misc-use-internal-linkage)
    (void)source;
    return "Input";
}

int
dsd_input_level_source_is_rf(dsd_input_level_source source) { // NOLINT(misc-use-internal-linkage)
    return source != DSD_INPUT_LEVEL_SOURCE_PCM;
}

int
getAfsString(const dsd_state* state, char* buffer, int a, int f, int s) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)a;
    (void)f;
    (void)s;
    DSD_SNPRINTF(buffer, 8, "00-00");
    return 0;
}

int
getAfsStringLength(const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    return 6;
}

int
compute_p25p1_voice_avg_err(const dsd_state* s, double* out_avg) { // NOLINT(misc-use-internal-linkage)
    (void)s;
    if (out_avg) {
        *out_avg = 0.0;
    }
    return 0;
}

size_t
dsd_app_frontend_history_compact_event_text(char* out, size_t out_size, const char* event_text,
                                            int mode) { // NOLINT(misc-use-internal-linkage)
    (void)mode;
    if (out_size > 0U) {
        DSD_SNPRINTF(out, out_size, "%s", event_text ? event_text : "");
    }
    return event_text ? strlen(event_text) : 0U;
}

time_t
dsd_app_frontend_history_event_sort_time(const char* event_text,
                                         time_t fallback_time) { // NOLINT(misc-use-internal-linkage)
    (void)event_text;
    return fallback_time;
}

int
dsd_app_frontend_history_get_mode(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

const char*
dsd_synctype_to_string(int synctype) { // NOLINT(misc-use-internal-linkage)
    (void)synctype;
    return "SYNC";
}

uint8_t
m17_address_classify(unsigned long long address) { // NOLINT(misc-use-internal-linkage)
    (void)address;
    return M17_ADDRESS_STANDARD;
}

int
p25_patch_compose_details(const dsd_state* state, char* out, size_t cap) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    if (cap > 0U) {
        out[0] = '\0';
    }
    return 0;
}

void
p25_wacn_sysid_to_callsign(uint32_t wacn, uint16_t sysid, char out[7]) { // NOLINT(misc-use-internal-linkage)
    (void)wacn;
    (void)sysid;
    if (out) {
        out[0] = '\0';
    }
}

long int
ui_guess_active_vc_freq(const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    return 0;
}

int
ui_menu_is_open(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

void
ui_menu_tick(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_print_header(const char* title) {
    (void)title;
} // NOLINT(misc-use-internal-linkage)

void
ui_print_hr(void) {} // NOLINT(misc-use-internal-linkage)

void
ui_print_lborder_green(void) {} // NOLINT(misc-use-internal-linkage)

void
ui_panel_header_render(const dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_panel_footer_status_render(const dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_print_learned_lcns(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

int
ui_print_p25_metrics(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    return 0;
}

void
ui_print_p25_neighbors(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_print_p25_iden_plan(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_print_p25_cc_candidates(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
ui_print_p25_secondary_ccs(const dsd_opts* opts, const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
print_dsp_status(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

#include "../../src/ui/terminal/dsd_ncurses_printer.c"
#include "dsd-neo/app_control/frontend.h"
#include "dsd-neo/core/input_level.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

static int g_requested_ppm;

int
dsd_app_frontend_get_metrics(dsd_frontend_metrics* out) { // NOLINT(misc-use-internal-linkage)
    DSD_MEMSET(out, 0, sizeof(*out));
    out->requested_ppm = g_requested_ppm;
    return 0;
}

static void
test_input_source_helpers(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    assert(ui_audio_in_is_soapy(NULL) == 0);
    assert(ui_audio_in_is_soapy(&opts) == 0);

    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "soapy");
    assert(ui_audio_in_is_soapy(&opts) == 1);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "soapy:driver=rtlsdr");
    assert(ui_audio_in_is_soapy(&opts) == 1);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl");
    assert(ui_audio_in_is_soapy(&opts) == 0);
}

static void
test_basic_input_source_rendering(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));

    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.pulse_digi_rate_in = 48000;
    opts.pulse_digi_in_channels = 2;
    opts.input_volume_multiplier = 3;
    opts.use_rigctl = 1;
    opts.rigctlportno = 4532;
    DSD_SNPRINTF(opts.pa_input_idx, sizeof(opts.pa_input_idx), "pulse-device");
    DSD_SNPRINTF(opts.tcp_hostname, sizeof(opts.tcp_hostname), "radio.local");
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| Pulse Signal Input:  48 kHz; 2 Ch;");
    assert_capture_contains(" D: pulse-device;");
    assert_capture_contains("RIG: radio.local:4532;");
    assert_capture_contains(" IV: 3X;");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.wav_sample_rate = 48000;
    opts.tcp_portno = 7355;
    opts.input_volume_multiplier = 4;
    DSD_SNPRINTF(opts.tcp_hostname, sizeof(opts.tcp_hostname), "10.0.0.5");
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| TCP Signal Input: 10.0.0.5:7355; 48 kHz; 1 Ch;");
    assert_capture_contains(" IV: 4X;");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.wav_sample_rate = 96000;
    opts.udp_in_portno = 23456;
    opts.input_volume_multiplier = 5;
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| UDP Signal Input: 127.0.0.1:23456; 96 kHz; 1 Ch;");
    assert_capture_contains("[Waiting]");

    opts.udp_in_packets = 42ULL;
    opts.udp_in_drops = 3ULL;
    DSD_SNPRINTF(opts.udp_in_bindaddr, sizeof(opts.udp_in_bindaddr), "0.0.0.0");
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| UDP Signal Input: 0.0.0.0:23456; 96 kHz; 1 Ch;");
    assert_capture_contains("Pkts:42 Drops:3");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    opts.input_volume_multiplier = 2;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "capture.wav");
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| WAV Audio Input: capture.wav; 48000 kHz;");
    assert_capture_contains(" IV: 2X;");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_STDIN;
    reset_printw_capture();
    ui_render_basic_input_sources(&opts);
    assert_capture_contains("| STDIN Standard Input: - Menu Disabled when using STDIN!");
}

static void
test_rtl_and_soapy_input_source_rendering(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dev_index = 2;
    opts.rtl_gain_value = 21;
    opts.rtl_volume_multiplier = 3;
    opts.rtlsdr_ppm_error = -7;
    g_requested_ppm = opts.rtlsdr_ppm_error;
    opts.rtl_squelch_level = -37.5f;
    opts.rtl_dsp_bw_khz = 24;
    opts.rtlsdr_center_freq = 851012500;
    opts.rtl_udp_port = 5555;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "rtl");
    DSD_SNPRINTF(opts.rtl_udp_bindaddr, sizeof(opts.rtl_udp_bindaddr), "127.0.0.1");
    reset_printw_capture();
    ui_render_rtl_input_source(&opts, &state);
    assert_capture_contains("| RTL: 2;");
    assert_capture_contains(" G: 21dB;");
    assert_capture_contains(" Mon: 3X;");
    assert_capture_contains(" PPM: -7;");
    assert_capture_contains(" SQL: -37.5 dB;");
    assert_capture_contains(" DSP-BW: 24 kHz;");
    assert_capture_contains(" FRQ: 851012500;");
    assert_capture_contains("| Auto PPM: Off");
    assert_capture_contains("| External RTL Tuning on UDP: 127.0.0.1:5555");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_gain_value = 0;
    opts.rtl_dsp_bw_khz = 12;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "soapy:driver=rtlsdr");
    reset_printw_capture();
    ui_render_rtl_input_source(&opts, &state);
    assert_capture_contains("| SoapySDR: driver=rtlsdr;");
    assert_capture_contains(" G: AGC;");
    assert_capture_contains(" DSP-BW: 12 kHz;");

    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "soapy");
    reset_printw_capture();
    ui_render_rtl_input_source(&opts, &state);
    assert_capture_contains("| SoapySDR;");
}

static void
test_rtl_auto_ppm_status_rendering(void) {
    reset_printw_capture();
    ui_print_rtl_auto_ppm_status_values(0, 0, 0, -100.0, 0.0, 0);
    assert_capture_contains("| Auto PPM: Off");

    reset_printw_capture();
    ui_print_rtl_auto_ppm_status_values(1, 0, 0, 18.2, -122.5, 1);
    assert_capture_contains("| Auto PPM: On; SNR: 18.2 dB; df: -122.5 Hz; step: +1;");

    reset_printw_capture();
    ui_print_rtl_auto_ppm_status_values(1, 0, 0, 20.0, 0.0, 0);
    assert_capture_contains("| Auto PPM: On; SNR: 20.0 dB; df: 0.0 Hz; step: hold;");

    reset_printw_capture();
    ui_print_rtl_auto_ppm_status_values(1, 1, -12, 25.0, 10.0, -1);
    assert_capture_contains("| Auto PPM: Locked (PPM: -12)");
}

static void
test_demod_symbol_rate_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(ui_demod_symbol_rate_hz(NULL, NULL) == 48000);

    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    state.samplesPerSymbol = 10;
    assert(ui_demod_symbol_rate_hz(&opts, &state) == 4800);

    opts.wav_sample_rate = 44100;
    state.samplesPerSymbol = 9;
    assert(ui_demod_symbol_rate_hz(&opts, &state) == 4900);

    state.samplesPerSymbol = 0;
    assert(ui_demod_symbol_rate_hz(&opts, &state) == 44100);

    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.pulse_digi_rate_in = 0;
    state.samplesPerSymbol = 12;
    assert(ui_demod_symbol_rate_hz(&opts, &state) == 4000);
}

static void
test_input_level_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(ui_compute_input_level_and_color(NULL, &state) == 0);
    assert(ui_compute_input_level_and_color(&opts, NULL) == 0);

    state.carrier = 0;
    state.max = 10000.0f;
    assert(ui_compute_input_level_and_color(&opts, &state) == 0);

    state.carrier = 1;
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.mod_qpsk = 0;
    state.rf_mod = 0;
    state.max = 8200.0f;
    assert(ui_compute_input_level_and_color(&opts, &state) == 50);

    opts.mod_qpsk = 1;
    assert(ui_compute_input_level_and_color(&opts, &state) == 100);

    opts.mod_qpsk = 0;
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    assert(ui_compute_input_level_and_color(&opts, &state) == 50);

    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 48;
    state.max = 0.075f;
    assert(ui_compute_input_level_and_color(&opts, &state) == 50);
}

static void
test_history_and_sort_helpers(void) {
    Event_History item;
    DSD_MEMSET(&item, 0, sizeof(item));
    assert(ui_eh_item_has_content(NULL) == 0);
    assert(ui_eh_item_has_content(&item) == 0);

    DSD_SNPRINTF(item.alias, sizeof(item.alias), "Unit 12");
    assert(ui_eh_item_has_content(&item) == 1);

    time_t last_seen[4] = {(time_t)10, (time_t)40, (time_t)20, (time_t)30};
    int idxs[4] = {0, 1, 2, 3};
    ui_sort_indices_by_last_seen(last_seen, idxs, 4);
    assert(idxs[0] == 1);
    assert(idxs[1] == 3);
    assert(idxs[2] == 2);
    assert(idxs[3] == 0);

    ui_history_item_ref newer = {.slot = 1, .idx = 7, .sort_time = (time_t)200};
    ui_history_item_ref older = {.slot = 0, .idx = 1, .sort_time = (time_t)100};
    assert(ui_history_item_ref_compare(&newer, &older) < 0);
    assert(ui_history_item_ref_compare(&older, &newer) > 0);

    ui_history_item_ref same_time_low_idx = {.slot = 1, .idx = 2, .sort_time = (time_t)200};
    ui_history_item_ref same_time_high_idx = {.slot = 0, .idx = 3, .sort_time = (time_t)200};
    assert(ui_history_item_ref_compare(&same_time_low_idx, &same_time_high_idx) < 0);

    static dsd_state state;
    static Event_History_I history[2];
    ui_history_item_ref refs[4];
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(history, 0, sizeof(history));
    DSD_MEMSET(refs, 0, sizeof(refs));
    state.event_history_s = history;

    DSD_SNPRINTF(history[0].Event_History_Items[1].event_string, sizeof(history[0].Event_History_Items[1].event_string),
                 "slot0 old");
    history[0].Event_History_Items[1].event_time = (time_t)100;
    DSD_SNPRINTF(history[1].Event_History_Items[3].text_message, sizeof(history[1].Event_History_Items[3].text_message),
                 "slot1 newest");
    history[1].Event_History_Items[3].event_time = (time_t)300;
    DSD_SNPRINTF(history[1].Event_History_Items[4].alias, sizeof(history[1].Event_History_Items[4].alias),
                 "slot1 middle");
    history[1].Event_History_Items[4].event_time = (time_t)200;

    assert(ui_history_collect_slot_items(NULL, 0, refs, 0U, 4U) == 0U);
    assert(ui_history_collect_slot_items(&state, 2, refs, 0U, 4U) == 0U);
    assert(ui_history_collect_slot_items(&state, 0, NULL, 0U, 4U) == 0U);
    assert(ui_history_collect_slot_items(&state, 0, refs, 4U, 4U) == 4U);

    DSD_MEMSET(refs, 0, sizeof(refs));
    assert(ui_history_collect_slot_items(&state, 0, refs, 0U, 4U) == 1U);
    assert(refs[0].slot == 0);
    assert(refs[0].idx == 1);
    assert(refs[0].sort_time == (time_t)100);

    DSD_MEMSET(refs, 0, sizeof(refs));
    assert(ui_history_collect_sorted_items(&state, 2, refs, 4U) == 3U);
    assert(refs[0].slot == 1);
    assert(refs[0].idx == 3);
    assert(refs[1].slot == 1);
    assert(refs[1].idx == 4);
    assert(refs[2].slot == 0);
    assert(refs[2].idx == 1);
}

static void
test_history_viewport_helpers(void) {
    int draw_footer = 1;
    ui_history_render_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));

    ui_history_setup_render_ctx(1, &draw_footer, &ctx);
    assert(draw_footer == 1);
    assert(ctx.history_mode == 1);
    assert(ctx.rows == 24);
    assert(ctx.cols == 80);
    assert(ctx.history_stop_y == 22);
    assert(ctx.events_to_show == 22);
    assert(ctx.string_size == 71);

    assert(ui_history_has_room_for_line(0) == 0);
    assert(ui_history_has_room_for_line(1) == 1);
    assert(ui_history_clamp_line_size(&ctx, 2) == 71U);
    assert(ui_history_clamp_line_size(&ctx, 90) == 0U);

    draw_footer = 1;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ui_history_setup_render_ctx(2, &draw_footer, &ctx);
    assert(draw_footer == 1);
    assert(ctx.history_mode == 2);
    assert(ctx.events_to_show == 22);
    assert(ctx.history_stop_y == 22);
    assert(ctx.string_size == 1999U);
    assert(ui_history_clamp_line_size(&ctx, 90) == 1999U);

    Event_History item;
    DSD_MEMSET(&item, 0, sizeof(item));
    assert(ui_history_print_detail_line(1, UINT8_MAX, "Alias: ", "") == 1);
    assert(ui_history_print_detail_line(1, UINT8_MAX, "Alias: ", NULL) == 1);
    assert(ui_history_print_detail_line(0, UINT8_MAX, "Alias: ", "Unit") == 0);
    assert(ui_history_print_detail_line(1, 0, "Alias: ", "Unit") == 1);
}

static void
test_hytera_key_format_helper(void) {
    static dsd_state state;
    char key_text[96];
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(key_text, 0, sizeof(key_text));

    state.K1 = 0x123456789AULL;
    assert(strcmp(ui_format_hytera_key(key_text, sizeof(key_text), &state, 1, 1U), "123456789A") == 0);
    assert(strcmp(ui_format_hytera_key(key_text, sizeof(key_text), &state, 0, 1U), DSD_SECRET_REDACTED) == 0);

    state.K1 = 0x1111222233334444ULL;
    state.K2 = 0x5555666677778888ULL;
    assert(strcmp(ui_format_hytera_key(key_text, sizeof(key_text), &state, 1, 2U), "1111222233334444 5555666677778888")
           == 0);

    state.K3 = 0x9999AAAABBBBCCCCULL;
    state.K4 = 0xDDDDEEEEFFFF0001ULL;
    assert(strcmp(ui_format_hytera_key(key_text, sizeof(key_text), &state, 1, 4U),
                  "1111222233334444 5555666677778888 9999AAAABBBBCCCC DDDDEEEEFFFF0001")
           == 0);

    state.K1 = 0xABCDEF123456ULL;
    assert(strcmp(ui_format_hytera_key(key_text, sizeof(key_text), &state, 1, 3U), "CDEF123456") == 0);
}

static void
test_edacs_tree_update_helpers(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(edacs_channel_tree, 0, sizeof(edacs_channel_tree));
    ncurses_last_synctype = DSD_SYNC_NONE;

    ui_update_sync_and_edacs_tree(NULL);
    assert(ncurses_last_synctype == DSD_SYNC_NONE);

    state.synctype = DSD_SYNC_EDACS_POS;
    state.carrier = 0;
    state.edacs_vc_lcn = 5;
    ui_update_sync_and_edacs_tree(&state);
    assert(ncurses_last_synctype == DSD_SYNC_EDACS_POS);
    assert(edacs_channel_tree[5][1] == 0ULL);

    state.carrier = 1;
    state.lasttg = 1234;
    state.lastsrc = 5678;
    state.edacs_vc_call_type = EDACS_IS_GROUP;
    ui_update_sync_and_edacs_tree(&state);
    assert(edacs_channel_tree[5][0] == (unsigned long long)DSD_SYNC_EDACS_POS);
    assert(edacs_channel_tree[5][1] == 5ULL);
    assert(edacs_channel_tree[5][2] == 1234ULL);
    assert(edacs_channel_tree[5][3] == 5678ULL);
    assert(edacs_channel_tree[5][4] == (unsigned long long)EDACS_IS_GROUP);
    assert(edacs_channel_tree[5][5] != 0ULL);

    edacs_channel_tree[5][3] = 77ULL;
    state.synctype = DSD_SYNC_NONE;
    state.lastsrc = 0;
    state.lasttg = 4321;
    ui_update_sync_and_edacs_tree(&state);
    assert(ncurses_last_synctype == DSD_SYNC_EDACS_POS);
    assert(edacs_channel_tree[5][2] == 4321ULL);
    assert(edacs_channel_tree[5][3] == 77ULL);

    state.lastsrc = 0x800;
    ui_update_sync_and_edacs_tree(&state);
    assert(edacs_channel_tree[5][3] == 0ULL);

    state.ea_mode = 1;
    state.lastsrc = 0;
    edacs_channel_tree[5][3] = 99ULL;
    ui_update_sync_and_edacs_tree(&state);
    assert(edacs_channel_tree[5][3] == 0ULL);
}

static void
test_patch_and_slot_helpers(void) {
    char tokens[48][64];
    int count = 0;
    DSD_MEMSET(tokens, 0, sizeof(tokens));
    ui_parse_patch_tokens(" TG 100 ; ; TG 200 ; very-long-token-that-keeps-its-text ", tokens, &count);
    assert(count == 3);
    assert(strcmp(tokens[0], "TG 100") == 0);
    assert(strcmp(tokens[1], "TG 200") == 0);
    assert(strcmp(tokens[2], "very-long-token-that-keeps-its-text") == 0);
    assert(ui_patch_tokens_col_width(tokens, count) == 28);

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dmrburstL = 16;
    state.dmrburstR = 21;
    state.lasttg = 101;
    state.lasttgR = 202;
    state.lastsrc = 303;
    state.lastsrcR = 404;
    state.payload_algid = 0x24;
    state.payload_algidR = 0x21;
    state.payload_keyid = 11;
    state.payload_keyidR = 22;
    state.payload_mi = 0x12345678ULL;
    state.payload_miR = 0x87654321ULL;
    state.payload_miP = 0x1111222233334444ULL;
    state.payload_miN = 0x5555666677778888ULL;
    state.R = 0x12345ULL;
    state.RR = 0x67890ULL;
    state.aes_key_loaded[0] = 1;
    state.A2[0] = 0x1111ULL;
    state.A4[0] = 0x2222ULL;
    DSD_SNPRINTF(state.call_string[1], sizeof(state.call_string[1]), "slot-two-call");
    DSD_SNPRINTF(state.dmr_embedded_gps[1], sizeof(state.dmr_embedded_gps[1]), "gps");
    DSD_SNPRINTF(state.dmr_lrrp_gps[1], sizeof(state.dmr_lrrp_gps[1]), "lrrp");
    DSD_SNPRINTF(state.generic_talker_alias[1], sizeof(state.generic_talker_alias[1]), "alias");

    ui_slot_view right = ui_build_slot_view(&state, 1);
    assert(right.slot_no == 2);
    assert(right.burst == 21);
    assert(right.lasttg == 202);
    assert(right.lastsrc == 404);
    assert(right.payload_algid == 0x21);
    assert(right.payload_keyid == 22);
    assert(right.payload_mi_dmr == 0x87654321ULL);
    assert(right.payload_mi_p25 == 0x5555666677778888ULL);
    assert(right.rc4_key == 0x67890ULL);
    assert(strcmp(right.call_banner, "slot-two-call") == 0);
    assert(strcmp(right.embedded_gps, "gps") == 0);
    assert(strcmp(right.lrrp_gps, "lrrp") == 0);
    assert(strcmp(right.talker_alias, "alias") == 0);

    assert(ui_slot_has_dxtra_embedded(1, 26) == 1);
    assert(ui_slot_has_dxtra_embedded(2, 26) == 0);
    assert(ui_slot_has_dxtra_embedded(2, 21) == 1);
}

static void
test_lock_and_protocol_helpers(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(ui_channel_label_is_locked(&opts, &state, "Already Locked") == 1);
    assert(ui_channel_label_is_locked(NULL, &state, "TG: 123") == 0);

    opts.trunk_tune_data_calls = 0;
    assert(ui_channel_label_is_locked(&opts, &state, "Active Data Ch: 7") == 1);
    opts.trunk_tune_data_calls = 1;
    opts.trunk_tune_group_calls = 0;
    assert(ui_channel_label_is_locked(&opts, &state, "TG: 123") == 1);
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 0;
    assert(ui_channel_label_is_locked(&opts, &state, "TGT: 456") == 1);

    DSD_SNPRINTF(state.nxdn_location_category, sizeof(state.nxdn_location_category), "Type-D");
    assert(ui_nxdn_is_idas(&state) == 1);
    DSD_SNPRINTF(state.nxdn_location_category, sizeof(state.nxdn_location_category), "Type-C");
    assert(ui_nxdn_is_idas(&state) == 0);
}

static void
test_canonical_p25_slot_and_recent_activity(void) {
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    assert(state != NULL);
    state->synctype = DSD_SYNC_P25P2_POS;
    state->dmrburstL = 0;

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 1201U;
    observation.policy_target_id = 1201U;
    observation.group_id = 1201U;
    observation.frequency_hz = 851012500;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);

    dsd_call_crypto_update crypto = {0};
    crypto.classification = DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
    crypto.algid = 0x84U;
    crypto.kid = 0x2468U;
    crypto.mi = 0x1122334455667788ULL;
    crypto.audio_permitted = 0U;
    crypto.observed_m = 1.1;
    assert(dsd_call_state_update_crypto(state, 0U, &crypto) == 1);

    ui_slot_view slot = ui_build_slot_view(state, 0);
    assert(slot.canonical_p25 == 1);
    assert(slot.burst == 21);
    assert(slot.lasttg == 1201);
    assert(slot.lastsrc == 0);
    reset_printw_capture();
    ui_slot_render_flags flags = {0};
    ui_render_slot_header_line(state, &slot, &flags);
    ui_render_slot_vxtra_line(&(dsd_opts){0}, state, &slot, &flags);
    assert_capture_contains("TGT: [    1201]");
    assert_capture_contains("SRC: [       0]");
    assert_capture_contains("ALG: 0x84 KEY ID: 0x2468 MI: 0x1122334455667788");
    assert(strstr(g_printw_capture, "UNKNOWN") == NULL);
    assert(strstr(g_printw_capture, "ENC?") == NULL);
    assert(strstr(g_printw_capture, "FREQ:") == NULL);
    assert(strstr(g_printw_capture, "[GROUP]") == NULL);
    assert(strstr(g_printw_capture, "P25 VOICE") == NULL);
    assert_capture_contains(" | VOICE");

    state->dmrburstR = 21;
    state->lasttgR = 9999;
    state->lastsrcR = 8888;
    ui_slot_view idle_companion = ui_build_slot_view(state, 1);
    assert(idle_companion.canonical_p25 == 0);
    assert(idle_companion.call.phase == DSD_CALL_PHASE_IDLE);
    reset_printw_capture();
    ui_render_p25_dmr_slot_block(&(dsd_opts){0}, state, &idle_companion);
    assert_capture_contains("TGT: [        ] SRC: [        ]");
    assert(strstr(g_printw_capture, "9999") == NULL);
    assert(strstr(g_printw_capture, "8888") == NULL);

    const uint64_t now_ms = (uint64_t)(dsd_time_now_monotonic_s() * 1000.0);
    assert(dsd_recent_activity_set_at(state, 0U, "old TG: 100; ", now_ms - 4000U) == 1);
    assert(dsd_recent_activity_set_at(state, 1U, "fresh TG: 200; ", now_ms - 1000U) == 1);
    reset_printw_capture();
    ui_render_active_channel_list(&(dsd_opts){0}, state, 31U);
    assert(strstr(g_printw_capture, "old") == NULL);
    assert_capture_contains("fresh TG: 200");
    reset_printw_capture();
    ui_render_p25_dmr_active_channels_line(&(dsd_opts){0}, state);
    assert_capture_contains("|        | fresh TG: 200");
    assert(strstr(g_printw_capture, "RECENT") == NULL);

    assert(dsd_call_state_end(state, 0U, 2.0) == 1);
    slot = ui_build_slot_view(state, 0);
    assert(slot.call.phase == DSD_CALL_PHASE_ENDED);
    reset_printw_capture();
    ui_render_p25_dmr_slot_block(&(dsd_opts){0}, state, &slot);
    assert_capture_contains("TGT: [        ] SRC: [        ]");
    assert(strstr(g_printw_capture, "1201") == NULL);

    state->dmrburstL = 25;
    state->payload_algid = 0x84;
    state->payload_keyid = 0x2468;
    state->payload_miP = 0x1122334455667788ULL;
    slot = ui_build_slot_view(state, 0);
    assert(slot.canonical_p25 == 0);
    assert(slot.call.phase == DSD_CALL_PHASE_ENDED);
    reset_printw_capture();
    ui_render_p25_dmr_slot_block(&(dsd_opts){0}, state, &slot);
    assert_capture_contains("HDU");
    assert_capture_contains("ALG: 0x84 KEY ID: 0x2468 MI: 0x1122334455667788");

    static dsd_opts tuned_opts;
    DSD_MEMSET(&tuned_opts, 0, sizeof(tuned_opts));
    tuned_opts.trunk_enable = 1;
    tuned_opts.trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = 851025000L;
    reset_printw_capture();
    ui_render_p25_dmr_tuned_freq_line(&tuned_opts, state);
    assert_capture_contains("Frequency: 851.025000 MHz");

    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmrburstL = 21;
    state->lasttg = 4321;
    state->lastsrc = 8765;
    slot = ui_build_slot_view(state, 0);
    assert(slot.canonical_p25 == 0);
    assert(slot.lasttg == 4321);
    assert(slot.lastsrc == 8765);
    reset_printw_capture();
    ui_render_p25_dmr_slot_block(&(dsd_opts){0}, state, &slot);
    assert_capture_contains("TGT: [    4321]");
    assert_capture_contains("SRC: [    8765]");
    dsd_state_ext_free_all(state);
    free(state);
}

int
main(void) {
    test_input_source_helpers();
    test_basic_input_source_rendering();
    test_rtl_and_soapy_input_source_rendering();
    test_rtl_auto_ppm_status_rendering();
    test_demod_symbol_rate_helpers();
    test_input_level_policy();
    test_history_and_sort_helpers();
    test_history_viewport_helpers();
    test_hytera_key_format_helper();
    test_edacs_tree_update_helpers();
    test_patch_and_slot_helpers();
    test_lock_and_protocol_helpers();
    test_canonical_p25_slot_and_recent_activity();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
