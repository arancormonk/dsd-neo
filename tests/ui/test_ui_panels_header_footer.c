// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Focused checks for terminal header/footer panel contracts without a real
 * curses screen.
 */

#include <assert.h>
#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/ui/panels.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

const char GIT_TAG[] = "test-tag";
const char GIT_HASH[] = "test-hash";

static int g_attron_calls;
static int g_attroff_calls;
static int g_hr_calls;
static int g_printw_calls;
static int g_last_attron;
static int g_last_attroff;
static char g_last_printw[256];

static void
reset_calls(void) {
    g_attron_calls = 0;
    g_attroff_calls = 0;
    g_hr_calls = 0;
    g_printw_calls = 0;
    g_last_attron = 0;
    g_last_attroff = 0;
    g_last_printw[0] = '\0';
}

int
attron(int attrs) { // NOLINT(misc-use-internal-linkage)
    g_attron_calls++;
    g_last_attron = attrs;
    return 0;
}

int
attroff(int attrs) { // NOLINT(misc-use-internal-linkage)
    g_attroff_calls++;
    g_last_attroff = attrs;
    return 0;
}

int
printw(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    va_list ap;
    va_start(ap, fmt);
    DSD_VSNPRINTF(g_last_printw, sizeof g_last_printw, fmt, ap);
    va_end(ap);
    g_printw_calls++;
    return 0;
}

void
ui_print_hr(void) { // NOLINT(misc-use-internal-linkage)
    g_hr_calls++;
}

static void
test_header_null_opts_is_noop(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);

    reset_calls();
    ui_panel_header_render(NULL, &state);

    assert(g_attron_calls == 0);
    assert(g_attroff_calls == 0);
    assert(g_hr_calls == 0);
    assert(g_printw_calls == 0);
}

static void
test_header_compact_renders_without_banner_color(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.terminal_compact = 1;

    reset_calls();
    ui_panel_header_render(&opts, &state);

    assert(g_attron_calls == 0);
    assert(g_attroff_calls == 0);
    assert(g_hr_calls == 2);
    assert(g_printw_calls == 1);
    assert(strstr(g_last_printw, "test-tag") != NULL);
    assert(strstr(g_last_printw, "test-hash") != NULL);
}

static void
test_header_compact_trunk_restores_trunk_color(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    opts.terminal_compact = 1;
    opts.p25_trunk = 1;

    reset_calls();
    ui_panel_header_render(&opts, &state);

    assert(g_attron_calls == 1);
    assert(g_last_attron == COLOR_PAIR(4));
    assert(g_attroff_calls == 0);
    assert(g_hr_calls == 2);
    assert(g_printw_calls == 1);
}

static void
test_header_full_renders_banner_and_body_colors(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    reset_calls();
    ui_panel_header_render(&opts, &state);

    assert(g_attron_calls == 2);
    assert(g_attroff_calls == 1);
    assert(g_last_attroff == COLOR_PAIR(6));
    assert(g_last_attron == COLOR_PAIR(4));
    assert(g_hr_calls == 2);
    assert(g_printw_calls == 1);
}

static void
test_footer_null_inputs_are_noop(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);

    reset_calls();
    ui_panel_footer_status_render(NULL, &state);
    ui_panel_footer_status_render(&opts, NULL);

    assert(g_attron_calls == 0);
    assert(g_attroff_calls == 0);
    assert(g_hr_calls == 0);
    assert(g_printw_calls == 0);
}

static void
test_footer_active_toast_renders_without_clearing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(state.ui_msg, sizeof state.ui_msg, "muted");
    state.ui_msg_expire = time(NULL) + 60;

    reset_calls();
    ui_panel_footer_status_render(&opts, &state);

    assert(g_attron_calls == 1);
    assert(g_last_attron == COLOR_PAIR(2));
    assert(g_attroff_calls == 1);
    assert(g_last_attroff == COLOR_PAIR(2));
    assert(g_hr_calls == 1);
    assert(g_printw_calls == 1);
    assert(strstr(g_last_printw, "muted") != NULL);
    assert(strcmp(state.ui_msg, "muted") == 0);
}

static void
test_footer_expired_toast_clears_snapshot_only(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(state.ui_msg, sizeof state.ui_msg, "expired");
    state.ui_msg_expire = time(NULL) - 1;

    reset_calls();
    ui_panel_footer_status_render(&opts, &state);

    assert(g_attron_calls == 0);
    assert(g_attroff_calls == 0);
    assert(g_hr_calls == 0);
    assert(g_printw_calls == 0);
    assert(state.ui_msg[0] == '\0');
    assert(state.ui_msg_expire == 0);
}

int
main(void) {
    test_header_null_opts_is_noop();
    test_header_compact_renders_without_banner_color();
    test_header_compact_trunk_restores_trunk_color();
    test_header_full_renders_banner_and_body_colors();
    test_footer_null_inputs_are_noop();
    test_footer_active_toast_renders_without_clearing();
    test_footer_expired_toast_clears_snapshot_only();
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
