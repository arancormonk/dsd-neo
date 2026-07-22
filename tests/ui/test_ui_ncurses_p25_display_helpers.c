// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Pure-helper checks for ncurses P25 display state.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/call_state.h"
#include "dsd-neo/platform/platform.h"

int ncurses_last_synctype;

static dsd_trunk_cc_candidates g_cc_candidates;
static dsdneoRuntimeConfig g_runtime_config;
static char g_printw_capture[4096];
static size_t g_printw_capture_len;
static int g_test_rows = 24;
static int g_test_cols = 80;
static uint64_t g_monotonic_ns;

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
append_raw_capture(const char* text, int len) {
    if (!text || g_printw_capture_len >= sizeof(g_printw_capture) - 1U) {
        return;
    }
    if (len < 0) {
        len = (int)strlen(text);
    }
    size_t remaining = sizeof(g_printw_capture) - g_printw_capture_len;
    size_t copy_len = (size_t)len;
    if (copy_len >= remaining) {
        copy_len = remaining - 1U;
    }
    if (copy_len == 0U) {
        return;
    }
    DSD_MEMCPY(g_printw_capture + g_printw_capture_len, text, copy_len);
    g_printw_capture_len += copy_len;
    g_printw_capture[g_printw_capture_len] = '\0';
}

static void
assert_capture_contains(const char* needle) {
    assert(needle != NULL);
    assert(strstr(g_printw_capture, needle) != NULL);
}

static void
assert_capture_lines_fit(int max_cols) {
    int col = 0;
    for (const char* p = g_printw_capture; *p; p++) {
        if (*p == '\n') {
            assert(col <= max_cols);
            col = 0;
            continue;
        }
        col++;
    }
    assert(col <= max_cols);
}

uint64_t
dsd_time_monotonic_ns(void) { // NOLINT(misc-use-internal-linkage)
    return g_monotonic_ns;
}

uint64_t
dsd_time_monotonic_ms(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) { // NOLINT(misc-use-internal-linkage)
    return &g_runtime_config;
}

int
p25_sm_in_tick(void) { // NOLINT(misc-use-internal-linkage)
    return 0;
}

const dsd_trunk_cc_candidates*
dsd_trunk_cc_candidates_peek(const dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    return &g_cc_candidates;
}

int
p25_iden_vu_bandwidth_hz(uint8_t bw_vu) { // NOLINT(misc-use-internal-linkage)
    if ((bw_vu & 0x0FU) == 0x4U) {
        return 6250;
    }
    if ((bw_vu & 0x0FU) == 0x5U) {
        return 12500;
    }
    return 0;
}

size_t
p25_format_adjacent_cfva(uint8_t cfva, char* out, size_t out_len) { // NOLINT(misc-use-internal-linkage)
    const char* text = (cfva & 0x2U) ? "current" : "last known";
    if (out && out_len > 0) {
        DSD_SNPRINTF(out, out_len, "%s", text);
    }
    return 1U;
}

size_t
p25_format_system_service_names(uint32_t service_mask, char* out, size_t out_len) { // NOLINT(misc-use-internal-linkage)
    const char* text = "group voice";
    size_t count = 1U;
    if (service_mask == 0x22F7FEU) {
        text = "network active, group voice, individual voice, group data, individual data, unit registration, "
               "group affiliation, group affiliation query, authentication, user status, user message, unit status, "
               "user status query, unit status query, unit page";
        count = 15U;
    } else if (service_mask == 0U) {
        text = "";
        count = 0U;
    }
    if (out && out_len > 0) {
        DSD_SNPRINTF(out, out_len, "%s", text);
    }
    return count;
}

const char*
dsd_synctype_to_string(int synctype) { // NOLINT(misc-use-internal-linkage)
    (void)synctype;
    return "SYNC";
}

int
compute_percentiles_u8(const uint8_t* src, int len, double* p50, double* p95) { // NOLINT(misc-use-internal-linkage)
    (void)src;
    (void)len;
    if (p50) {
        *p50 = 0.0;
    }
    if (p95) {
        *p95 = 0.0;
    }
    return 0;
}

void
ui_print_lborder_green(void) { // NOLINT(misc-use-internal-linkage)
    append_raw_capture("|", 1);
}

short
ui_iden_color_pair(int iden) { // NOLINT(misc-use-internal-linkage)
    return (short)(20 + iden);
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
    char c = (char)(ch & A_CHARTEXT);
    append_raw_capture(&c, 1);
    return 0;
}

int
waddnstr(WINDOW* win, const char* str, int n) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    append_raw_capture(str, n);
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
wattr_get(WINDOW* win, attr_t* attrs, short* pair, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)opts;
    if (attrs) {
        *attrs = 0;
    }
    if (pair) {
        *pair = 0;
    }
    return 0;
}

int
wattr_set(WINDOW* win, attr_t attrs, short pair, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)attrs;
    (void)pair;
    (void)opts;
    return 0;
}

WINDOW* stdscr;

#undef getmaxyx
#define getmaxyx(win, y, x)                                                                                            \
    do {                                                                                                               \
        (void)(win);                                                                                                   \
        (y) = g_test_rows;                                                                                             \
        (x) = g_test_cols;                                                                                             \
    } while (0)

#include "../../src/ui/terminal/ncurses_p25_display.c"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/protocol/p25/p25_trunk_sm.h"

static void
set_fdma_iden(dsd_state* state, int iden, int base_freq, int spacing, int explicit_tdma) {
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_iden_fdma[iden].base_freq = base_freq;
    state->p25_iden_fdma[iden].chan_spac = spacing;
    state->p25_chan_tdma_explicit[iden] = (uint8_t)explicit_tdma;
}

static void
set_tdma_iden(dsd_state* state, int iden, int base_freq, int spacing, int chan_type) {
    state->p25_iden_tdma[iden].populated = 1;
    state->p25_iden_tdma[iden].base_freq = base_freq;
    state->p25_iden_tdma[iden].chan_spac = spacing;
    state->p25_iden_tdma[iden].chan_type = chan_type;
}

static int
run_iden_match_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_P25P1_POS;

    set_fdma_iden(&state, 2, 154000000, 100, 1);
    const int fdma_ch = (2 << 12) | 7;
    const long fdma_freq = (154000000L * 5L) + (7L * 100L * 125L);
    assert(ui_is_iden_channel(&state, fdma_ch, fdma_freq) == 1);

    int iden = -1;
    assert(ui_match_iden_channel(&state, fdma_ch, fdma_freq, &iden) == 1);
    assert(iden == 2);
    assert(ui_match_iden_channel(&state, fdma_ch, fdma_freq + 125L, &iden) == 0);
    assert(iden == 2);

    state.p25_cc_is_tdma = 1;
    set_fdma_iden(&state, 3, 160000000, 50, 0);
    const int implicit_tdma_ch = (3 << 12) | 8;
    const long implicit_tdma_freq = (160000000L * 5L) + (4L * 50L * 125L);
    assert(ui_is_iden_channel(&state, implicit_tdma_ch, implicit_tdma_freq) == 1);

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_P25P2_POS;
    set_tdma_iden(&state, 4, 170000000, 25, 4);
    const int tdma_ch = (4 << 12) | 9;
    const long tdma_freq = (170000000L * 5L) + (2L * 25L * 125L);
    assert(ui_is_iden_channel(&state, tdma_ch, tdma_freq) == 1);
    assert(ui_is_iden_channel(&state, tdma_ch, tdma_freq + 1L) == 0);
    assert(ui_is_iden_channel(&state, 0, tdma_freq) == 0);
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    assert(ui_is_iden_channel(&state, tdma_ch, tdma_freq) == 0);
    assert(ui_is_iden_channel(NULL, tdma_ch, tdma_freq) == 0);

    return 0;
}

static int
run_active_vc_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(ui_guess_active_vc_freq(NULL) == 0);
    assert(ui_guess_active_vc_freq(&state) == 0);

    state.p25_vc_freq[0] = 851012500L;
    assert(ui_guess_active_vc_freq(&state) == 851012500L);

    state.trunk_vc_freq[0] = 852012500L;
    assert(ui_guess_active_vc_freq(&state) == 852012500L);

    state.trunk_vc_freq[0] = 0;
    state.p25_vc_freq[0] = 0;
    dsd_call_observation recent = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 123U,
        .channel = 0x123AU,
        .frequency_hz = 853012500L,
    };
    g_monotonic_ns = UINT64_C(10000000000);
    const uint64_t now_ms = dsd_time_monotonic_ns() / UINT64_C(1000000);
    assert(dsd_recent_activity_publish(&state, 2U, &recent, "TG 123 Ch: 123A slot 1", now_ms) == 1);
    assert(ui_guess_active_vc_freq(&state) == 853012500L);
    assert(dsd_recent_activity_publish(&state, 2U, &recent, "TG 123 Ch: 123A slot 1",
                                       now_ms - DSD_RECENT_ACTIVITY_TTL_MS - 1U)
           == 1);
    assert(ui_guess_active_vc_freq(&state) == 0);
    dsd_state_ext_free_all(&state);
    g_monotonic_ns = 0U;

    dsd_state* canonical = (dsd_state*)calloc(1U, sizeof(*canonical));
    assert(canonical != NULL);
    canonical->synctype = DSD_SYNC_P25P2_POS;
    canonical->p25_vc_freq[0] = 855012500L;
    canonical->trunk_chan_map[1234] = 856012500L;
    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 1U;
    observation.frequency_hz = 857012500L;
    observation.observed_m = 1.0;
    assert(dsd_call_state_observe(canonical, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(ui_guess_active_vc_freq(canonical) == 857012500L);
    assert(dsd_call_state_end(canonical, 0U, 2.0) == 1);
    assert(ui_guess_active_vc_freq(canonical) == 855012500L);
    canonical->trunk_vc_freq[0] = 858012500L;
    assert(ui_guess_active_vc_freq(canonical) == 858012500L);
    observation.observed_m = 3.0;
    assert(dsd_call_state_observe(canonical, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(ui_guess_active_vc_freq(canonical) == 857012500L);
    dsd_state_ext_free_all(canonical);
    free(canonical);

    return 0;
}

static int
run_voice_average_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    double avg = -1.0;
    assert(compute_p25p1_voice_avg_err(&state, &avg) == 0);
    assert(avg == -1.0);
    state.p25_p1_voice_err_hist_len = 4;
    state.p25_p1_voice_err_hist_sum = 22;
    assert(compute_p25p1_voice_avg_err(&state, &avg) == 1);
    assert(avg == 5.5);
    assert(compute_p25p1_voice_avg_err(&state, NULL) == 1);

    assert(compute_p25p2_voice_avg_err(&state, -1, &avg) == 0);
    assert(compute_p25p2_voice_avg_err(&state, 2, &avg) == 0);
    state.p25_p2_voice_err_hist_len = 3;
    state.p25_p2_voice_err_hist_sum[0] = 9;
    state.p25_p2_voice_err_hist_sum[1] = 12;
    assert(compute_p25p2_voice_avg_err(&state, 0, &avg) == 1);
    assert(avg == 3.0);
    assert(compute_p25p2_voice_avg_err(&state, 1, &avg) == 1);
    assert(avg == 4.0);
    assert(compute_p25p2_voice_avg_err(&state, 1, NULL) == 1);

    return 0;
}

static int
run_neighbor_helper_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_cc_candidates, 0, sizeof(g_cc_candidates));

    state.p25_nb_count = 5;
    state.p25_cc_freq = 852012500L;
    state.p25_nb_entries[0].freq = 851012500L;
    state.p25_nb_entries[0].last_seen = 100;
    state.p25_nb_entries[1].freq = 0;
    state.p25_nb_entries[1].last_seen = 300;
    state.p25_nb_entries[2].freq = 852012500L;
    state.p25_nb_entries[2].last_seen = 125;
    state.p25_nb_entries[3].freq = 853012500L;
    state.p25_nb_entries[3].sysid = 0x123;
    state.p25_nb_entries[3].rfss = 4;
    state.p25_nb_entries[3].site = 5;
    state.p25_nb_entries[3].cfva = 2;
    state.p25_nb_entries[3].cfva_valid = 1;
    state.p25_nb_entries[3].lra = 0x44;
    state.p25_nb_entries[3].lra_valid = 1;
    state.p25_nb_entries[3].last_seen = 175;
    state.p25_nb_entries[4].freq = 854012500L;
    state.p25_nb_entries[4].last_seen = 50;

    int idxs[4] = {-1, -1, -1, -1};
    assert(ui_collect_neighbor_indices(&state, idxs, 3) == 3);
    assert(idxs[0] == 0);
    assert(idxs[1] == 2);
    assert(idxs[2] == 3);

    ui_sort_neighbors_by_last_seen(&state, idxs, 3);
    assert(idxs[0] == 3);
    assert(idxs[1] == 2);
    assert(idxs[2] == 0);

    g_cc_candidates.count = 2;
    g_cc_candidates.candidates[0] = 853012500L;
    g_cc_candidates.candidates[1] = 855012500L;
    assert(ui_freq_in_cc_candidates(&state, 853012500L) == 1);
    assert(ui_freq_in_cc_candidates(&state, 851012500L) == 0);

    char line[96];
    assert(ui_format_neighbor_line(&state, 3, (time_t)200, line, sizeof(line)) > 0);
    assert(strstr(line, "853.012500 MHz") != NULL);
    assert(strstr(line, " [C]") != NULL);
    assert(strstr(line, "SYS:123 R:004 S:005") != NULL);
    assert(strstr(line, "LRA:44 CFVA:current") != NULL);
    assert(strstr(line, "age:25s") != NULL);

    char short_line[32];
    int short_len = ui_format_neighbor_line(&state, 3, (time_t)200, short_line, sizeof(short_line));
    assert(short_len == (int)strlen(short_line));
    assert(short_len < (int)sizeof(short_line));

    assert(ui_format_neighbor_line(&state, 2, (time_t)100, line, sizeof(line)) > 0);
    assert(strstr(line, "852.012500 MHz [CC]") != NULL);
    assert(strstr(line, "age:0s") != NULL);

    state.p25_secondary_cc_count = 1;
    state.p25_secondary_cc_entries[0].freq = 853012500L;
    state.p25_secondary_cc_entries[0].channel = 0x1234;
    state.p25_secondary_cc_entries[0].rfss = 4;
    state.p25_secondary_cc_entries[0].site = 5;
    state.p25_secondary_cc_entries[0].ssc = 0xA0;
    state.p25_secondary_cc_entries[0].last_seen = 180;
    assert(ui_format_secondary_cc_line(&state, 0, (time_t)200, line, sizeof(line)) > 0);
    assert(strstr(line, "853.012500 MHz [C] CH:1234 R:004 S:005 SSC:A0 age:20s") != NULL);

    return 0;
}

static int
run_trunk_sm_helper_cases(void) {
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_ON_CC), "CC") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_ON_VC), "VC") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_HANG), "HANG") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_HUNTING), "HUNT") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_ARMED), "ARM") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_FOLLOW), "FOL") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(DSD_P25_SM_MODE_RETURNING), "RET") == 0);
    assert(strcmp(ui_p25_sm_mode_to_str(-1), "?") == 0);

    assert(ui_sm_tag_symbol("after-tune") == 'V');
    assert(ui_sm_tag_symbol("after-release") == 'R');
    assert(ui_sm_tag_symbol("release-deferred") == 'H');
    assert(ui_sm_tag_symbol("after-neigh") == 'N');
    assert(ui_sm_tag_symbol("tick") == 'T');
    assert(ui_sm_tag_symbol("other") == '?');

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_sm_tag_head = 2;
    assert(ui_p25_sm_tag_idx(&state, 0) == 1);
    assert(ui_p25_sm_tag_idx(&state, 1) == 0);
    assert(ui_p25_sm_tag_idx(&state, 2) == 7);

    char path[16] = {0};
    int wrote = ui_append_sm_path_symbol(path, sizeof(path), 0, 'V');
    assert(wrote == 1);
    assert(strcmp(path, "V") == 0);
    wrote = ui_append_sm_path_symbol(path, sizeof(path), wrote, 'R');
    assert(wrote == 2);
    assert(strcmp(path, "V\xE2\x86\x92R") == 0);

    char full[4] = "ABC";
    assert(ui_append_sm_path_symbol(full, sizeof(full), 1, 'Z') == 1);
    assert(strcmp(full, "ABC") == 0);

    return 0;
}

static int
run_iden_summary_helper_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    int total = -1;
    int confirmed = -1;
    assert(ui_p25_iden_confirmed_count(&state, &total, &confirmed) == 0);
    assert(total == 0);
    assert(confirmed == 0);

    state.p25_iden_fdma[1].populated = 1;
    state.p25_iden_fdma[1].trust = 1;
    state.p25_iden_tdma[1].populated = 1;
    state.p25_iden_tdma[1].trust = 2;
    state.p25_iden_fdma[2].populated = 1;
    state.p25_iden_fdma[2].trust = 3;
    assert(ui_p25_iden_confirmed_count(&state, &total, &confirmed) == 1);
    assert(total == 3);
    assert(confirmed == 2);

    assert(ui_p25_iden_entry_has_data(&state.p25_iden_fdma[1]) == 0);
    state.p25_iden_fdma[1].base_freq = 851000000;
    assert(ui_p25_iden_entry_has_data(&state.p25_iden_fdma[1]) == 1);
    state.p25_iden_fdma[1].base_freq = 0;
    state.p25_iden_fdma[1].chan_spac = 50;
    assert(ui_p25_iden_entry_has_data(&state.p25_iden_fdma[1]) == 1);

    assert(strcmp(ui_p25_iden_trust_str(2), "ok") == 0);
    assert(strcmp(ui_p25_iden_trust_str(1), "prov") == 0);
    assert(strcmp(ui_p25_iden_trust_str(0), "-") == 0);

    return 0;
}

static int
run_iden_render_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    state.p25_iden_fdma[5].populated = 1;
    state.p25_iden_fdma[5].base_freq = 170200000;
    state.p25_iden_fdma[5].chan_spac = 100;
    state.p25_iden_fdma[5].chan_type = 1;
    state.p25_iden_fdma[5].trans_off = -45000000;
    state.p25_iden_fdma[5].trust = 2;
    state.p25_iden_fdma[5].bw_vu = 4;
    state.p25_iden_fdma[5].wacn = 0xABCDEULL;
    state.p25_iden_fdma[5].sysid = 0x123ULL;
    state.p25_iden_fdma[5].rfss = 2ULL;
    state.p25_iden_fdma[5].site = 7ULL;
    state.p25_iden_tdma[5].populated = 1;
    state.p25_iden_tdma[5].base_freq = 170400000;
    state.p25_iden_tdma[5].chan_spac = 50;
    state.p25_iden_tdma[5].chan_type = 4;
    state.p25_iden_tdma[5].trust = 1;

    reset_printw_capture();
    ui_print_p25_iden_plan(NULL, &state);
    assert_capture_contains(
        "IDEN 5: FDMA[F/T] type:1 base:851.000000MHz spac:0.012500MHz off:-45000000 trust:ok bw:6250Hz");
    assert_capture_contains("W:ABCDE S:123");
    assert_capture_contains("R:2 I:7");
    assert_capture_contains("IDEN 5: TDMA[F/T] type:4 base:852.000000MHz spac:0.006250MHz off:0 trust:prov");

    return 0;
}

static int
run_p25_frequency_display_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    state.trunk_cc_freq = 851012500L;
    state.trunk_vc_freq[0] = 852012500L;
    reset_printw_capture();
    assert(ui_print_p25_cc_vc_metric(&state) == 1);
    assert_capture_contains("| CC/VC: CC:851.012500 MHz VC:852.012500 MHz");

    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_cc_freq = 853012500L;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .channel = 0x123AU,
        .frequency_hz = 854012500L,
    };
    assert(dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    reset_printw_capture();
    assert(ui_print_p25_cc_vc_metric(&state) == 1);
    assert_capture_contains("| CC/VC: CC:853.012500 MHz VC:854.012500 MHz");
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_printw_capture();
    assert(ui_print_p25_cc_vc_metric(&state) == 1);
    assert_capture_contains("| CC/VC: CC:- VC:-");

    return 0;
}

static int
run_service_metric_display_cases(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.p25_sys_services_valid = 1;
    state.p25_sys_services_available = 0x22F7FEU;
    state.p25_sys_services_supported = 0x22F7FEU;
    state.p25_sys_services_request_priority = 1;

    g_test_cols = 72;
    reset_printw_capture();
    int lines = ui_print_p25_service_metric(&state);
    assert(lines > 2);
    assert_capture_contains("| Services: Avail:22F7FE(15) Supp:22F7FE(15) RPL:1");
    assert_capture_contains("Avail/Supp: network active, group voice");
    assert(strstr(g_printw_capture, "\n| Supp: network active") == NULL);
    assert_capture_lines_fit(g_test_cols);

    state.p25_sys_services_supported = 0U;
    reset_printw_capture();
    lines = ui_print_p25_service_metric(&state);
    assert(lines > 3);
    assert_capture_contains("| Services: Avail:22F7FE(15) Supp:000000(0) RPL:1");
    assert_capture_contains("Avail: network active");
    assert_capture_contains("Supp: -");
    assert_capture_lines_fit(g_test_cols);

    state.p25_sys_services_valid = 0;
    reset_printw_capture();
    assert(ui_print_p25_service_metric(&state) == 0);
    assert(g_printw_capture[0] == '\0');
    g_test_cols = 80;

    return 0;
}

static int
run_p2_gate_helper_cases(void) {
    DSD_MEMSET(&g_runtime_config, 0, sizeof(g_runtime_config));
    g_runtime_config.p25_ring_hold_s = 1.25;
    g_runtime_config.p25_mac_hold_s = 4.5;

    double ring_hold = 0.0;
    double mac_hold = 0.0;
    ui_get_p2_hold_windows(&ring_hold, &mac_hold);
    assert(ring_hold == 1.25);
    assert(mac_hold == 4.5);

    assert(ui_clamp_p2_ring_fill(-2) == 0);
    assert(ui_clamp_p2_ring_fill(2) == 2);
    assert(ui_clamp_p2_ring_fill(99) == DSD_P25_P2_AUDIO_RING_DEPTH);

    assert(ui_p2_ring_recent(0, 0.2, 0.75) == 0);
    assert(ui_p2_ring_recent(1, -0.1, 0.75) == 0);
    assert(ui_p2_ring_recent(1, 0.8, 0.75) == 0);
    assert(ui_p2_ring_recent(1, 0.75, 0.75) == 1);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.trunk_hangtime = 2.0;

    assert(ui_p2_slot_active(&opts, &state, 0, 5.0, 0.0, 0.75, 3.0) == 0);
    state.p25_p2_audio_allowed[0] = 1;
    assert(ui_p2_slot_active(&opts, &state, 0, 5.0, 0.0, 0.75, 3.0) == 1);
    assert(ui_p2_slot_active(&opts, &state, 0, 5.0, 3.0, 0.75, 3.0) == 0);
    state.p25_p2_audio_ring_count[0] = 1;
    assert(ui_p2_slot_active(&opts, &state, 0, 0.5, 3.0, 0.75, 3.0) == 1);
    state.p25_p2_audio_allowed[0] = 0;
    state.p25_p2_audio_ring_count[0] = 0;
    assert(ui_p2_slot_active(&opts, &state, 0, 2.5, 9.0, 0.75, 3.0) == 1);

    return 0;
}

int
main(void) {
    run_iden_match_cases();
    run_active_vc_cases();
    run_voice_average_cases();
    run_neighbor_helper_cases();
    run_trunk_sm_helper_cases();
    run_iden_summary_helper_cases();
    run_iden_render_cases();
    run_p25_frequency_display_cases();
    run_service_metric_display_cases();
    run_p2_gate_helper_cases();
    printf("UI_NCURSES_P25_DISPLAY_HELPERS: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
