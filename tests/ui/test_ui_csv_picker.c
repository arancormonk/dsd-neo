// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The CSV import picker's collect step: which imports-directory files it offers
 * for a given kind, and in what order.
 */

#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include "dsd-neo/platform/platform.h"

#include "csv_picker.h"
#include "menu_prompts.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define TEST_RMDIR rmdir
#endif

/* csv_picker.c references these menu_prompts symbols from its open() path, which
   this test never exercises; the terminal library is not linked, so stub them. */

/* cppcheck miscounts the parameters across the callback typedefs and reports
   arg 5 as unnamed in the declaration; csv_picker.c and menu_prompts.c suppress
   the same false positive on the same signatures. */
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
ui_prompt_open_string_async(const char* title, const char* prefill, size_t cap, ui_prompt_string_done_fn on_done,
                            void* user_ctx) {
    (void)title;
    (void)prefill;
    (void)cap;
    (void)on_done;
    (void)user_ctx;
}

void
ui_chooser_start(const char* title, const char* const* items, int count, ui_chooser_done_fn on_done, void* user_ctx) {
    (void)title;
    (void)items;
    (void)count;
    (void)on_done;
    (void)user_ctx;
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

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

int
main(void) {
    test_collect_matches_kind();
    test_collect_empty();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_CSV_PICKER: OK\n");
    return 0;
}
