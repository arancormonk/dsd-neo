// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CSV import picker's collect step: which imports-directory files it offers
 * for a given kind, and in what order; and its flow through the real chooser and
 * prompt (issue #672, B8): the owned context is disposed exactly once on every
 * path - pick, cancel, "Enter a path..." then submit or cancel, the render
 * path's cancel, a prompt that cannot allocate, a replacing widget, and the
 * close at UI exit.
 */

#include <curses.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include "dsd-neo/platform/platform.h"

#include "csv_picker.h"
#include "menu_prompts.h"
#include "test_env.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define TEST_RMDIR rmdir
#endif

/* menu_prompts.c's seams outside the widgets: no window (the render path then cancels), no toast. */
WINDOW* ui_make_window(int h, int w, int y, int x);  // NOLINT(misc-use-internal-linkage)
void ui_statusf(const char* fmt, ...);               // NOLINT(misc-use-internal-linkage)
int ui_status_peek(char* buf, size_t n, time_t now); // NOLINT(misc-use-internal-linkage)
void ui_status_clear_if_expired(time_t now);         // NOLINT(misc-use-internal-linkage)

WINDOW*
ui_make_window(int h, int w, int y, int x) { // NOLINT(misc-use-internal-linkage)
    (void)h;
    (void)w;
    (void)y;
    (void)x;
    return NULL;
}

void
ui_statusf(const char* fmt, ...) { // NOLINT(misc-use-internal-linkage)
    (void)fmt;
}

int
ui_status_peek(char* buf, size_t n, time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)buf;
    (void)n;
    (void)now;
    return 0;
}

void
ui_status_clear_if_expired(time_t now) { // NOLINT(misc-use-internal-linkage)
    (void)now;
}

static int g_failures = 0;

static void
expect(const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static int
write_file(const char* path, const char* body) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        return -1;
    }
    int rc = fputs(body, fp) < 0 ? -1 : 0;
    rc |= fclose(fp) != 0 ? -1 : 0;
    return rc;
}

static int
write_csv_with_sidecar(const char* dir, const char* leaf, const char* kind, int sid, const char* name, char* out_path,
                       size_t out_sz) {
    if (dsd_test_path_join(out_path, out_sz, dir, leaf) != 0) {
        return -1;
    }
    if (write_file(out_path, "1,x\n") != 0) {
        return -1;
    }
    dsd_rr_provenance p;
    DSD_MEMSET(&p, 0, sizeof p);
    DSD_STRNCPY(p.kind, kind, sizeof p.kind - 1);
    p.sid = sid;
    DSD_STRNCPY(p.system_name, name, sizeof p.system_name - 1);
    p.imported_at = 1755500000LL;
    return dsd_rr_provenance_write(out_path, &p);
}

static void
rm_with_sidecar(const char* path) {
    char side[DSD_TEST_PATH_MAX + 8];
    (void)DSD_SNPRINTF(side, sizeof side, "%s.rr", path);
    (void)remove(side);
    (void)remove(path);
}

/* A collect returns only files whose sidecar kind matches, sorted by label. */
static void
test_collect_matches_kind(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_csv_pick") == NULL) {
        expect("scratch dir", 0);
        return;
    }

    char chan_z[DSD_TEST_PATH_MAX];
    char chan_a[DSD_TEST_PATH_MAX];
    char group[DSD_TEST_PATH_MAX];
    char orphan[DSD_TEST_PATH_MAX];
    (void)write_csv_with_sidecar(dir, "Zulu chan.csv", "chan", 2, "Zulu", chan_z, sizeof chan_z);
    (void)write_csv_with_sidecar(dir, "Alpha chan.csv", "chan", 1, "Alpha", chan_a, sizeof chan_a);
    (void)write_csv_with_sidecar(dir, "Alpha group.csv", "group", 1, "Alpha", group, sizeof group);
    (void)dsd_test_path_join(orphan, sizeof orphan, dir, "handmade.csv");
    (void)write_file(orphan, "1,x\n"); /* no sidecar -> not offered */

    char paths[CSV_PICKER_MAX][CSV_PICKER_PATH_MAX];
    char labels[CSV_PICKER_MAX][CSV_PICKER_LABEL_MAX];

    int n = ui_csv_picker_collect(dir, "chan", paths, labels, CSV_PICKER_MAX);
    expect("two chan files offered", n == 2);
    /* Sorted by label: "Alpha chan.csv" before "Zulu chan.csv". */
    expect("first is Alpha", strstr(labels[0], "Alpha") != NULL);
    expect("second is Zulu", strstr(labels[1], "Zulu") != NULL);
    expect("path pairs with label", strcmp(paths[0], chan_a) == 0);

    n = ui_csv_picker_collect(dir, "group", paths, labels, CSV_PICKER_MAX);
    expect("one group file offered", n == 1);
    expect("group path", strcmp(paths[0], group) == 0);

    rm_with_sidecar(chan_z);
    rm_with_sidecar(chan_a);
    rm_with_sidecar(group);
    (void)remove(orphan);
    (void)TEST_RMDIR(dir);
}

/* An empty or absent directory collects nothing (the caller then just prompts). */
static void
test_collect_empty(void) {
    char paths[CSV_PICKER_MAX][CSV_PICKER_PATH_MAX];
    char labels[CSV_PICKER_MAX][CSV_PICKER_LABEL_MAX];
    expect("NULL dir collects nothing", ui_csv_picker_collect(NULL, "chan", paths, labels, CSV_PICKER_MAX) == 0);
    expect("empty dir collects nothing", ui_csv_picker_collect("", "chan", paths, labels, CSV_PICKER_MAX) == 0);

    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_csv_empty") == NULL) {
        expect("scratch dir", 0);
        return;
    }
    expect("no matching files collects nothing",
           ui_csv_picker_collect(dir, "chan", paths, labels, CSV_PICKER_MAX) == 0);
    (void)TEST_RMDIR(dir);
}

/* ---- The flow through the real chooser and prompt (issue #672, B8) ---- */

/* What the caller's callback was answered with. */
static int g_done_calls;
static int g_done_null;
static char g_done_text[DSD_TEST_PATH_MAX];
static int g_caller_token;

static void
caller_done(void* user, const char* text) {
    expect("the caller's own context comes back", user == &g_caller_token);
    g_done_calls++;
    g_done_null = (text == NULL);
    (void)DSD_SNPRINTF(g_done_text, sizeof g_done_text, "%s", text ? text : "");
}

static void
reset_done(void) {
    g_done_calls = 0;
    g_done_null = 0;
    g_done_text[0] = '\0';
}

static void
other_chooser_done(void* user, int sel) {
    (void)user;
    (void)sel;
}

static void
other_prompt_done(void* user, const char* text) {
    (void)user;
    (void)text;
}

/* The chooser over the two "chan" imports plus "Enter a path...". */
static void
open_chan_picker(void) {
    reset_done();
    ui_csv_import_picker_open("chan", "Channel map CSV", 64, caller_done, &g_caller_token);
    expect("chooser open", ui_chooser_active() == 1);
    expect("chooser lists the files and the path row", ui_chooser_test_snapshot().count == 3);
    expect("one live context", ui_csv_picker_live_contexts_for_test() == 1);
}

/* The chooser's last row, "Enter a path...": the prompt takes the context over. */
static void
open_path_prompt(void) {
    open_chan_picker();
    expect("End key", ui_chooser_handle_key(KEY_END) == 1);
    expect("Enter on the path row", ui_chooser_handle_key('\n') == 1);
    expect("chooser closed", ui_chooser_active() == 0);
    expect("prompt open", ui_prompt_active() == 1);
    expect("the prompt owns the one context", ui_csv_picker_live_contexts_for_test() == 1);
    expect("no answer yet", g_done_calls == 0);
}

static void
expect_answered(const char* what, int null, const char* text) {
    if (g_done_calls != 1 || g_done_null != null || strcmp(g_done_text, text) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d call(s), null %d, text \"%s\"\n", what, g_done_calls, g_done_null,
                    g_done_text);
        g_failures++;
    }
    expect("context disposed once", ui_csv_picker_live_contexts_for_test() == 0);
    expect("nothing left open", ui_chooser_active() == 0 && ui_prompt_active() == 0);
}

static void
test_flow(const char* chan_b) {
    /* Pick: the second file. */
    open_chan_picker();
    expect("Down", ui_chooser_handle_key(KEY_DOWN) == 1);
    expect("Enter", ui_chooser_handle_key('\n') == 1);
    expect_answered("pick", 0, chan_b);

    /* Esc on the list. */
    open_chan_picker();
    expect("Esc", ui_chooser_handle_key(27) == 1);
    expect_answered("chooser cancel", 1, "");

    /* "Enter a path..." then a typed path. */
    open_path_prompt();
    expect("type", ui_prompt_handle_key('a') == 1 && ui_prompt_handle_key('.') == 1 && ui_prompt_handle_key('c') == 1);
    expect("submit", ui_prompt_handle_key('\n') == 1);
    expect_answered("typed path", 0, "a.c");

    /* "Enter a path..." then Esc. */
    open_path_prompt();
    expect("Esc in the prompt", ui_prompt_handle_key(27) == 1);
    expect_answered("prompt cancel", 1, "");

    /* The render path cancels a list it cannot draw (no screen here). */
    open_chan_picker();
    ui_chooser_render();
    expect_answered("render-path cancel", 1, "");

    /* "Enter a path..." whose prompt cannot allocate its buffer: the cancel arrives at once. */
    open_chan_picker();
    expect("End key", ui_chooser_handle_key(KEY_END) == 1);
    ui_prompt_test_fail_next_alloc();
    expect("Enter on the path row", ui_chooser_handle_key('\n') == 1);
    expect_answered("hand-off allocation failure", 1, "");

    /* No imports of a kind: the prompt opens directly, and its allocation failure answers at once too. */
    reset_done();
    ui_prompt_test_fail_next_alloc();
    ui_csv_import_picker_open("src", "Source ID list CSV", 64, caller_done, &g_caller_token);
    expect_answered("direct prompt allocation failure", 1, "");

    /* Replaced by another chooser: disposed, no answer (a chooser closed without one says nothing). */
    static const char* const other_items[] = {"Other"};
    open_chan_picker();
    ui_chooser_start("Other", other_items, 1, other_chooser_done, NULL);
    expect("replaced chooser says nothing", g_done_calls == 0);
    expect("replaced chooser disposed", ui_csv_picker_live_contexts_for_test() == 0);
    ui_chooser_close();

    /* The path prompt replaced by another prompt: the replaced prompt is cancelled, then disposed. */
    open_path_prompt();
    ui_prompt_open_string_async("Other", NULL, 16, other_prompt_done, NULL);
    expect("replaced prompt cancelled", g_done_calls == 1 && g_done_null == 1);
    expect("replaced prompt disposed", ui_csv_picker_live_contexts_for_test() == 0);
    ui_prompt_close_all();

    /* UI exit with the list open (ui_overlays_shutdown() closes the chooser): disposed, no answer. */
    open_chan_picker();
    ui_chooser_close();
    expect("closed chooser says nothing", g_done_calls == 0);
    expect("closed chooser disposed", ui_csv_picker_live_contexts_for_test() == 0);

    /* UI exit with the path prompt open: cancelled, then disposed. */
    open_path_prompt();
    ui_prompt_close_all();
    expect_answered("prompt closed at exit", 1, "");
}

/* The imports directory is resolved from XDG_CONFIG_HOME (APPDATA on Windows), here a scratch directory. */
static void
test_flow_through_the_widgets(void) {
    char cfg[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(cfg, sizeof cfg, "dsdneo_csv_flow") == NULL) {
        expect("scratch dir", 0);
        return;
    }
    (void)dsd_test_setenv("XDG_CONFIG_HOME", cfg, 1);
    (void)dsd_test_setenv("APPDATA", cfg, 1);
    if (dsd_user_imports_dir_create() != 0) {
        expect("imports dir", 0);
        return;
    }
    char dir[DSD_TEST_PATH_MAX];
    (void)DSD_SNPRINTF(dir, sizeof dir, "%s", dsd_user_imports_dir());
    char chan_a[DSD_TEST_PATH_MAX];
    char chan_b[DSD_TEST_PATH_MAX];
    (void)write_csv_with_sidecar(dir, "A chan.csv", "chan", 1, "A", chan_a, sizeof chan_a);
    (void)write_csv_with_sidecar(dir, "B chan.csv", "chan", 2, "B", chan_b, sizeof chan_b);

    test_flow(chan_b);

    rm_with_sidecar(chan_a);
    rm_with_sidecar(chan_b);
    (void)TEST_RMDIR(dir);
    char parent[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(parent, sizeof parent, cfg, "dsd-neo") == 0) {
        (void)TEST_RMDIR(parent);
    }
    (void)TEST_RMDIR(cfg);
}

int
main(void) {
    test_collect_matches_kind();
    test_collect_empty();
    test_flow_through_the_widgets();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_CSV_PICKER: OK\n");
    return 0;
}
