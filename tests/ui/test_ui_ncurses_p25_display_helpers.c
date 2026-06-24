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
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int ncurses_last_synctype;

static dsd_trunk_cc_candidates g_cc_candidates;
static dsdneoRuntimeConfig g_runtime_config;
static char g_printw_capture[4096];
static size_t g_printw_capture_len;

static void
reset_printw_capture(void) {
    DSD_MEMSET(g_printw_capture, 0, sizeof(g_printw_capture));
    g_printw_capture_len = 0U;
}

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

uint64_t
dsd_time_monotonic_ns(void) { // NOLINT(misc-use-internal-linkage)
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
    DSD_SNPRINTF(state.active_channel[2], sizeof(state.active_channel[2]), "TG 123 Ch: 123A slot 1");
    state.trunk_chan_map[0x123A] = 853012500L;
    assert(ui_guess_active_vc_freq(&state) == 853012500L);

    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_SNPRINTF(state.active_channel[0], sizeof(state.active_channel[0]), "TG 456 Ch: 1234");
    state.trunk_chan_map[1234] = 854012500L;
    assert(ui_guess_active_vc_freq(&state) == 854012500L);

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
    assert(strstr(line, "age:25s") != NULL);

    assert(ui_format_neighbor_line(&state, 2, (time_t)100, line, sizeof(line)) > 0);
    assert(strstr(line, "852.012500 MHz [CC]") != NULL);
    assert(strstr(line, "age:0s") != NULL);

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
    assert_capture_contains("IDEN 5: FDMA[F/T] type:1 base:851.000000MHz spac:0.012500MHz off:-45000000 trust:ok");
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
    DSD_SNPRINTF(state.active_channel[3], sizeof(state.active_channel[3]), "Active Group Ch: 123A TG: 100;");
    state.trunk_chan_map[0x123A] = 854012500L;
    reset_printw_capture();
    assert(ui_print_p25_cc_vc_metric(&state) == 1);
    assert_capture_contains("| CC/VC: CC:853.012500 MHz VC:854.012500 MHz");

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_printw_capture();
    assert(ui_print_p25_cc_vc_metric(&state) == 1);
    assert_capture_contains("| CC/VC: CC:- VC:-");

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
    run_p2_gate_helper_cases();
    printf("UI_NCURSES_P25_DISPLAY_HELPERS: OK\n");
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
