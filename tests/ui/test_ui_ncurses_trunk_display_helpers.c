// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-suspicious-include)
/*
 * Pure-helper checks for ncurses trunk channel display state.
 */

#include <assert.h>
#include <curses.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "../../src/ui/terminal/ncurses_trunk_display.c"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/synctype_ids.h"
#include "dsd-neo/ui/ncurses_p25_display.h"
#include "dsd-neo/ui/ui_prims.h"

static char g_output[8192];
static int g_header_calls;
static int g_border_calls;
static int g_attron_calls;
static int g_attroff_calls;
static int g_attr_get_calls;
static int g_attr_set_calls;
static int g_last_attron;
static int g_last_attroff;
static int g_last_attr_set_attrs;
static short g_last_attr_set_pair;
static int g_iden_match_channel = -1;
static long int g_iden_match_freq;
static int g_iden_match_iden;

static void
append_text(const char* text) {
    const size_t used = strlen(g_output);
    if (used >= sizeof(g_output) - 1U) {
        return;
    }
    DSD_SNPRINTF(g_output + used, sizeof(g_output) - used, "%s", text ? text : "");
}

static void
reset_render_capture(void) {
    g_output[0] = '\0';
    g_header_calls = 0;
    g_border_calls = 0;
    g_attron_calls = 0;
    g_attroff_calls = 0;
    g_attr_get_calls = 0;
    g_attr_set_calls = 0;
    g_last_attron = 0;
    g_last_attroff = 0;
    g_last_attr_set_attrs = 0;
    g_last_attr_set_pair = 0;
    g_iden_match_channel = -1;
    g_iden_match_freq = 0;
    g_iden_match_iden = 0;
}

static int
count_substring(const char* haystack, const char* needle) {
    int count = 0;
    const char* pos = haystack;
    while ((pos = strstr(pos, needle)) != NULL) {
        count++;
        pos += strlen(needle);
    }
    return count;
}

void
ui_print_header(const char* title) { // NOLINT(misc-use-internal-linkage)
    g_header_calls++;
    append_text("[");
    append_text(title);
    append_text("]\n");
}

void
ui_print_lborder_green(void) { // NOLINT(misc-use-internal-linkage)
    g_border_calls++;
    append_text("|");
}

short
ui_iden_color_pair(int iden) { // NOLINT(misc-use-internal-linkage)
    return (short)(21 + (iden & 7));
}

int
ui_match_iden_channel(const dsd_state* state, int ch16, long int freq,
                      int* out_iden) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    if (out_iden) {
        *out_iden = g_iden_match_iden;
    }
    return ch16 == g_iden_match_channel && freq == g_iden_match_freq;
}

int
wattr_on(WINDOW* win, attr_t attrs, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)opts;
    g_attron_calls++;
    g_last_attron = (int)attrs;
    return 0;
}

int
wattr_off(WINDOW* win, attr_t attrs, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)opts;
    g_attroff_calls++;
    g_last_attroff = (int)attrs;
    return 0;
}

int
wattr_get(WINDOW* win, attr_t* attrs, short* pair, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)opts;
    g_attr_get_calls++;
    if (attrs) {
        *attrs = 77;
    }
    if (pair) {
        *pair = 6;
    }
    return 0;
}

int
wattr_set(WINDOW* win, attr_t attrs, short pair, void* opts) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)opts;
    g_attr_set_calls++;
    g_last_attr_set_attrs = (int)attrs;
    g_last_attr_set_pair = pair;
    return 0;
}

int
waddch(WINDOW* win, const chtype ch) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    char text[2] = {(char)ch, '\0'};
    append_text(text);
    return 0;
}

int
waddnstr(WINDOW* win, const char* str, int n) { // NOLINT(misc-use-internal-linkage)
    (void)win;
    (void)n;
    append_text(str);
    return 0;
}

int
printw(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    DSD_VSNPRINTF(text, sizeof(text), fmt, ap);
    va_end(ap);
    append_text(text);
    return 0;
}

static void
add_trunk_channel(dsd_state* state, uint16_t channel, long int freq) {
    assert(channel < DSD_TRUNK_CHAN_MAP_SIZE);
    state->trunk_chan_map_used[state->trunk_chan_map_used_count++] = channel;
    state->trunk_chan_map[channel] = freq;
}

static void
test_public_guards_do_not_render(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    reset_render_capture();
    ui_print_learned_lcns(NULL, &state);
    ui_print_learned_lcns(&opts, NULL);
    ui_print_learned_lcns(&opts, &state);
    opts.trunk_enable = 1;
    ui_print_learned_lcns(&opts, &state);

    assert(g_header_calls == 0);
    assert(g_border_calls == 0);
    assert(g_attron_calls == 0);
    assert(g_output[0] == '\0');
}

static void
test_channel_map_rendering_dedupes_and_reports_extra(void) {
    static dsd_state state;
    ui_trunk_render_state render;
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(&render, 0, sizeof render);

    add_trunk_channel(&state, 0, 850000000L);
    for (int i = 0; i < UI_TRUNK_MAX_MAP_PRINT + 2; i++) {
        add_trunk_channel(&state, (uint16_t)(0x100U + (uint16_t)i), 851000000L + (long)i * 12500L);
    }
    add_trunk_channel(&state, 0x180, state.trunk_chan_map[0x100]);

    reset_render_capture();
    ui_trunk_render_chan_map(&state, state.trunk_chan_map_used_count, &render);

    assert(count_substring(g_output, "CH ") == UI_TRUNK_MAX_MAP_PRINT);
    assert(strstr(g_output, "CH 0100: 851.000000 MHz") != NULL);
    assert(strstr(g_output, "CH 011F: 851.387500 MHz") != NULL);
    assert(strstr(g_output, "CH 0120:") == NULL);
    assert(strstr(g_output, "850.000000 MHz") == NULL);
    assert(strstr(g_output, "... and 2 more learned channels") != NULL);
    assert(render.seen_count == UI_TRUNK_MAX_MAP_PRINT + 2);
    assert(render.col_in_row == 0);
}

static void
test_lcn_rendering_uses_map_channel_and_fallback(void) {
    static dsd_state state;
    ui_trunk_render_state render;
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_MEMSET(&render, 0, sizeof render);
    add_trunk_channel(&state, 0x1234, 851012500L);
    state.trunk_lcn_freq[0] = 851012500L;
    state.trunk_lcn_freq[1] = 852012500L;
    ui_trunk_mark_freq_seen(&render, 851012500L);

    reset_render_capture();
    ui_trunk_render_lcn_freqs(&state, state.trunk_chan_map_used_count, &render);

    assert(strstr(g_output, "CH 1234:") == NULL);
    assert(strstr(g_output, "CH ----: 852.012500 MHz") != NULL);
    assert(render.seen_count == 2);

    DSD_MEMSET(&render, 0, sizeof render);
    reset_render_capture();
    ui_trunk_render_lcn_freqs(&state, state.trunk_chan_map_used_count, &render);
    assert(strstr(g_output, "CH 1234: 851.012500 MHz") != NULL);
    assert(strstr(g_output, "CH ----: 852.012500 MHz") != NULL);
    assert(render.seen_count == 2);
}

static void
test_iden_rendering_restores_saved_attrs(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);
    g_iden_match_channel = 0x1234;
    g_iden_match_freq = 851012500L;
    g_iden_match_iden = 3;

    reset_render_capture();
    g_iden_match_channel = 0x1234;
    g_iden_match_freq = 851012500L;
    g_iden_match_iden = 3;
    ui_trunk_print_channel_freq(&state, 0x1234, 851012500L);

    assert(strstr(g_output, "CH 1234[I3]: 851.012500 MHz") != NULL);
    assert(g_attr_get_calls == 1);
    assert(g_attron_calls == 1);
    assert(PAIR_NUMBER(g_last_attron) == 24);
    assert(g_attr_set_calls == 1);
    assert(g_last_attr_set_attrs == 77);
    assert(g_last_attr_set_pair == 6);
}

static void
test_public_render_applies_color_and_p25_legend(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    state.carrier = 1;
    state.synctype = DSD_SYNC_P25P2_POS;
    add_trunk_channel(&state, 0x1234, 851012500L);

    reset_render_capture();
    ui_print_learned_lcns(&opts, &state);

    assert(g_header_calls == 1);
    assert(strstr(g_output, "[Channels]") != NULL);
    assert(strstr(g_output, "CH 1234: 851.012500 MHz") != NULL);
    assert(strstr(g_output, "Legend: IDEN colors I0 I1 I2 I3 I4 I5 I6 I7") != NULL);
    assert(g_attron_calls >= 10);
    assert(g_attroff_calls == 8);
    assert(PAIR_NUMBER(g_last_attroff) == 28);
    assert(PAIR_NUMBER(g_last_attron) == 3);
}

static void
test_public_render_uses_idle_color_without_p25_legend(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.trunk_enable = 1;
    state.carrier = 0;
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.trunk_lcn_freq[0] = 852012500L;

    reset_render_capture();
    ui_print_learned_lcns(&opts, &state);

    assert(strstr(g_output, "CH ----: 852.012500 MHz") != NULL);
    assert(strstr(g_output, "Legend:") == NULL);
    assert(PAIR_NUMBER(g_last_attron) == 4);
}

static void
test_lcn_presence(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);

    assert(!ui_trunk_has_lcn_freq(&state));
    state.trunk_lcn_freq[25] = 851000000;
    assert(ui_trunk_has_lcn_freq(&state));
}

static void
test_seen_frequency_tracking_is_bounded_and_dedupes(void) {
    ui_trunk_render_state render;
    DSD_MEMSET(&render, 0, sizeof render);

    assert(!ui_trunk_freq_seen(&render, 851000000));
    ui_trunk_mark_freq_seen(&render, 851000000);
    ui_trunk_mark_freq_seen(&render, 852000000);
    assert(ui_trunk_freq_seen(&render, 851000000));
    assert(ui_trunk_freq_seen(&render, 852000000));
    assert(!ui_trunk_freq_seen(&render, 853000000));

    render.seen_count = UI_TRUNK_SEEN_FREQ_CAP;
    render.seen_freqs[UI_TRUNK_SEEN_FREQ_CAP - 1] = 900000000;
    ui_trunk_mark_freq_seen(&render, 901000000);
    assert(render.seen_count == UI_TRUNK_SEEN_FREQ_CAP);
    assert(!ui_trunk_freq_seen(&render, 901000000));
}

static void
test_find_channel_for_frequency_uses_tracked_entries_only(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);

    state.trunk_chan_map_used_count = 4;
    state.trunk_chan_map_used[0] = 0x0000;
    state.trunk_chan_map_used[1] = 0x1234;
    state.trunk_chan_map_used[2] = DSD_TRUNK_CHAN_MAP_SIZE;
    state.trunk_chan_map_used[3] = 0x2345;
    state.trunk_chan_map[0x1234] = 851012500;
    state.trunk_chan_map[0x2345] = 852012500;

    assert(ui_trunk_find_channel_for_freq(&state, state.trunk_chan_map_used_count, 851012500) == 0x1234);
    assert(ui_trunk_find_channel_for_freq(&state, state.trunk_chan_map_used_count, 852012500) == 0x2345);
    assert(ui_trunk_find_channel_for_freq(&state, state.trunk_chan_map_used_count, 853012500) == -1);
    assert(ui_trunk_find_channel_for_freq(&state, 1, 851012500) == -1);
}

int
main(void) {
    test_public_guards_do_not_render();
    test_lcn_presence();
    test_seen_frequency_tracking_is_bounded_and_dedupes();
    test_find_channel_for_frequency_uses_tracked_entries_only();
    test_channel_map_rendering_dedupes_and_reports_extra();
    test_lcn_rendering_uses_map_channel_and_fallback();
    test_iden_rendering_restores_saved_attrs();
    test_public_render_applies_color_and_p25_legend();
    test_public_render_uses_idle_color_without_p25_legend();
    return 0;
}

// NOLINTEND(bugprone-suspicious-include)
