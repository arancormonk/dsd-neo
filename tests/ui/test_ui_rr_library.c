// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The Imported Systems browser's curses-free model: grouping file halves into
 * systems, formatting an aligned row, telling which system a session is using,
 * and scanning a real directory of sidecars.
 */

#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include "dsd-neo/platform/platform.h"

#include "rr_library.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#define TEST_RMDIR _rmdir
#else
#include <unistd.h>
#define TEST_RMDIR rmdir
#endif

static int g_failures = 0;

static void
expect(const char* what, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void
expect_str(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
        g_failures++;
    }
}

static void
prov_group(dsd_rr_provenance* p, int sid, const char* name, int present, dsd_rr_protocol proto, long long hz,
           int trunking, int scan_list) {
    DSD_MEMSET(p, 0, sizeof *p);
    DSD_STRNCPY(p->kind, "group", sizeof p->kind - 1);
    p->sid = sid;
    DSD_STRNCPY(p->system_name, name, sizeof p->system_name - 1);
    DSD_STRNCPY(p->site_label, "Site 1", sizeof p->site_label - 1);
    p->partial_enc_as_de = 1;
    p->imported_at = 1755500000LL;
    p->recipe.present = present;
    p->recipe.protocol = proto;
    p->recipe.tune_hz = hz;
    p->recipe.trunking = trunking;
    p->recipe.scan_list = scan_list;
}

/* Two halves of one import fold into a single system with both paths. */
static void
test_group_and_chan_fold_into_one_system(void) {
    RrLibrary lib;
    rr_library_init(&lib);

    dsd_rr_provenance g;
    prov_group(&g, 6673, "SARA Network", 1, DSD_RR_PROTO_P25, 769768750LL, 1, 0);
    expect("group folds", rr_library_add(&lib, "/imports/SARA Network group.csv", &g) == 0);

    dsd_rr_provenance c = g;
    DSD_STRNCPY(c.kind, "chan", sizeof c.kind - 1);
    expect("chan folds", rr_library_add(&lib, "/imports/SARA Network chan.csv", &c) == 0);

    expect("one system", lib.count == 1);
    expect("system has both halves", lib.systems[0].has_group == 1 && lib.systems[0].has_chan == 1);
    expect_str("group path", lib.systems[0].group_path, "/imports/SARA Network group.csv");
    expect_str("chan path", lib.systems[0].chan_path, "/imports/SARA Network chan.csv");
    expect("recipe carried", lib.systems[0].recipe.present == 1);
    expect("recipe protocol", lib.systems[0].recipe.protocol == DSD_RR_PROTO_P25);
}

/* Different system ids stay separate systems. */
static void
test_distinct_sids_are_distinct_systems(void) {
    RrLibrary lib;
    rr_library_init(&lib);

    dsd_rr_provenance a;
    prov_group(&a, 100, "Alpha", 1, DSD_RR_PROTO_DMR_CAPPLUS, 851012500LL, 1, 0);
    dsd_rr_provenance b;
    prov_group(&b, 200, "Bravo", 1, DSD_RR_PROTO_DMR_TIER3, 852000000LL, 1, 0);
    expect("a folds", rr_library_add(&lib, "/imports/Alpha group.csv", &a) == 0);
    expect("b folds", rr_library_add(&lib, "/imports/Bravo group.csv", &b) == 0);
    expect("two systems", lib.count == 2);
}

/** @brief Widths for @p lib rendered into @p cols columns. */
static RrLibraryLayout
layout_for(const RrLibrary* lib, int cols) {
    RrLibraryLayout layout;
    rr_library_layout(lib, cols, &layout);
    return layout;
}

/*
 * The whole row, spelled out. Four columns after a two-column gutter: system,
 * site, protocol, and the detail cell that closes the line. The site is there
 * because several rows can now name the same system.
 */
static void
test_row_format_trunked(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 6673, "SARA Network", 1, DSD_RR_PROTO_P25, 769768750LL, 1, 0);
    DSD_STRNCPY(g.site_label, "Sioux City", sizeof g.site_label - 1);
    (void)rr_library_add(&lib, "/imports/SARA Network - Sioux City group.csv", &g);
    const RrLibraryLayout layout = layout_for(&lib, 80);

    char row[256];
    (void)rr_library_row_format(&lib.systems[0], &layout, 0, row, sizeof row);
    expect_str("the whole row", row, "  SARA Network  Sioux City  P25          769.76875");

    /* In use is a gutter mark, not a suffix: it costs two columns instead of
       nine, and it reads down the left edge instead of hiding at the end of
       lines of differing length. */
    (void)rr_library_row_format(&lib.systems[0], &layout, 1, row, sizeof row);
    expect_str("the in-use row", row, "* SARA Network  Sioux City  P25          769.76875");
}

/*
 * Nothing imported per-site yet - every sidecar predates the label - so the
 * site column would be a column of nothing. Drop it and give the width back to
 * the system name.
 */
static void
test_layout_drops_an_empty_site_column(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 6673, "SARA Network", 1, DSD_RR_PROTO_P25, 769768750LL, 1, 0);
    g.site_label[0] = '\0';
    (void)rr_library_add(&lib, "/imports/SARA Network group.csv", &g);

    const RrLibraryLayout layout = layout_for(&lib, 80);
    expect("no site column", layout.site == 0);

    char row[256];
    (void)rr_library_row_format(&lib.systems[0], &layout, 0, row, sizeof row);
    expect_str("the unlabelled row", row, "  SARA Network  P25          769.76875");
}

/*
 * Columns are measured against the terminal, not fixed: 80 columns is the width
 * that has to keep working, and a wide terminal should not waste the space it
 * has. Neither column is allowed to starve the other.
 */
static void
test_layout_shares_the_width_it_has(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 9, "A Very Long System Name That Goes On And On And On", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    DSD_STRNCPY(g.site_label, "A Very Long Site Description Indeed", sizeof g.site_label - 1);
    (void)rr_library_add(&lib, "/imports/Long - Site group.csv", &g);

    const RrLibraryLayout wide = layout_for(&lib, 200);
    expect("a wide terminal shows the whole name", wide.name == 40);
    expect("and the whole site", wide.site == 28);

    const RrLibraryLayout narrow = layout_for(&lib, 80);
    expect("80 columns splits what is left evenly", narrow.name == 23 && narrow.site == 23);

    /* A terminal narrower than the minimum still renders; the chooser clips it. */
    const RrLibraryLayout tiny = layout_for(&lib, 40);
    expect("neither column starves", tiny.name >= 12 && tiny.site >= 10);
}

/* A files-only system (no recipe) shows dashes rather than a frequency. */
static void
test_row_format_files_only(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 42, "Legacy", 0, DSD_RR_PROTO_UNSUPPORTED, 0, 0, 0);
    (void)rr_library_add(&lib, "/imports/Legacy group.csv", &g);
    const RrLibraryLayout layout = layout_for(&lib, 80);

    char row[256];
    (void)rr_library_row_format(&lib.systems[0], &layout, 0, row, sizeof row);
    expect("files-only names the system", strstr(row, "Legacy") != NULL);
    expect("files-only shows no frequency", strstr(row, ".") == NULL || strstr(row, "MHz") == NULL);
    expect("files-only uses a dash for the detail", strstr(row, "-") != NULL);
}

/* A conventional scan list is flagged in the detail cell. */
static void
test_row_format_scan_list(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 7, "County Fire", 1, DSD_RR_PROTO_DMR_CONV, 462562500LL, 0, 1);
    DSD_STRNCPY(g.site_label, "9 repeaters", sizeof g.site_label - 1);
    /* Conventional emits a chan (scan list) file, and the kind has to match the
       name on disk: the two halves are paired by the stem the filename carries. */
    DSD_STRNCPY(g.kind, "chan", sizeof g.kind - 1);
    expect("the scan list folds", rr_library_add(&lib, "/imports/County Fire - 9 repeaters chan.csv", &g) == 0);
    const RrLibraryLayout layout = layout_for(&lib, 80);

    char row[256];
    (void)rr_library_row_format(&lib.systems[0], &layout, 0, row, sizeof row);
    expect("scan list is flagged", strstr(row, "scan") != NULL);
    expect("a repeater count stands in for a site", strstr(row, "9 repeaters") != NULL);
}

/* Long text is truncated so the columns stay aligned - in both of them. */
static void
test_row_format_truncates_long_text(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 9, "A Very Long System Name That Exceeds The Column Width By A Lot", 1, DSD_RR_PROTO_P25,
               770000000LL, 1, 0);
    DSD_STRNCPY(g.site_label, "A Very Long Site Description Indeed", sizeof g.site_label - 1);
    (void)rr_library_add(&lib, "/imports/Long group.csv", &g);
    const RrLibraryLayout layout = layout_for(&lib, 80);

    char row[256];
    (void)rr_library_row_format(&lib.systems[0], &layout, 0, row, sizeof row);
    expect("truncation is marked", strstr(row, "..") != NULL);
    expect("the name is cut to its column", strstr(row, "A Very Long System Name") == NULL);
    expect("the site is cut to its column", strstr(row, "A Very Long Site Description") == NULL);
    expect("protocol still present after truncation", strstr(row, "P25") != NULL);
    expect("frequency still present after truncation", strstr(row, "770") != NULL);
}

/*
 * Every message that names a stored import - the action list's title, the
 * delete confirmation, the apply status - has to name the SITE too. Several
 * rows carry the same system name now, and "Delete Iowa Statewide from disk?"
 * would not say which county it is about to delete.
 */
static void
test_display_name_names_the_site(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 8734, "Iowa Statewide", 1, DSD_RR_PROTO_P25, 770418750LL, 1, 0);
    DSD_STRNCPY(g.site_label, "Polk (Des Moines)", sizeof g.site_label - 1);
    (void)rr_library_add(&lib, "/imports/Iowa Statewide - Polk group.csv", &g);

    char text[256];
    rr_library_display_name(&lib.systems[0], text, sizeof text);
    expect_str("named with its site", text, "Iowa Statewide - Polk (Des Moines)");

    /* A file imported before labels existed has only the one name to give. */
    rr_library_init(&lib);
    prov_group(&g, 42, "Legacy", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    g.site_label[0] = '\0';
    (void)rr_library_add(&lib, "/imports/Legacy group.csv", &g);
    rr_library_display_name(&lib.systems[0], text, sizeof text);
    expect_str("unlabelled names the system alone", text, "Legacy");

    /* Never NULL and never unterminated: these go straight into a prompt. */
    char tiny[8];
    rr_library_display_name(&lib.systems[0], tiny, sizeof tiny);
    expect_str("a short buffer still yields a usable name", tiny, "Legacy");
    rr_library_display_name(NULL, text, sizeof text);
    expect_str("a missing row yields no name", text, "");
}

/* in_use compares stored paths against the session's in-use paths. */
static void
test_in_use_predicate(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 6673, "SARA", 1, DSD_RR_PROTO_P25, 769768750LL, 1, 0);
    (void)rr_library_add(&lib, "/imports/SARA group.csv", &g);
    dsd_rr_provenance c = g;
    DSD_STRNCPY(c.kind, "chan", sizeof c.kind - 1);
    (void)rr_library_add(&lib, "/imports/SARA chan.csv", &c);

    expect("chan in use matches", rr_library_system_in_use(&lib.systems[0], "/imports/SARA chan.csv", "") == 1);
    expect("group in use matches", rr_library_system_in_use(&lib.systems[0], "", "/imports/SARA group.csv") == 1);
    expect("neither in use", rr_library_system_in_use(&lib.systems[0], "/other/x.csv", "/other/y.csv") == 0);
    expect("NULL in-use paths are safe", rr_library_system_in_use(&lib.systems[0], NULL, NULL) == 0);
}

/* The library caps at RR_LIBRARY_MAX and flags overflow rather than corrupting. */
static void
test_overflow_is_flagged(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    for (int i = 0; i < RR_LIBRARY_MAX + 5; i++) {
        dsd_rr_provenance g;
        char path[64];
        prov_group(&g, 1000 + i, "Sys", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
        (void)DSD_SNPRINTF(path, sizeof path, "/imports/s%d group.csv", i);
        (void)rr_library_add(&lib, path, &g);
    }
    expect("count is capped", lib.count == RR_LIBRARY_MAX);
    expect("overflow is flagged", lib.overflow == 1);
}

/* A sidecar with no usable sid, or a kind that is neither half, is not a
   managed import: folding those in would merge every one of them into a single
   bogus row that no action can complete. */
static void
test_unusable_sidecars_are_skipped(void) {
    RrLibrary lib;
    rr_library_init(&lib);

    dsd_rr_provenance g;
    prov_group(&g, 0, "No Sid", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    expect("sid 0 is refused", rr_library_add(&lib, "/imports/a group.csv", &g) == -1);
    prov_group(&g, -3, "Negative", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    expect("negative sid is refused", rr_library_add(&lib, "/imports/b group.csv", &g) == -1);
    prov_group(&g, 5, "Odd Kind", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    DSD_STRNCPY(g.kind, "keys", sizeof g.kind - 1);
    expect("an unknown kind is refused", rr_library_add(&lib, "/imports/c keys.csv", &g) == -1);
    expect("nothing was folded", lib.count == 0);
}

/*
 * A statewide system is imported once per site, so several stored imports
 * legitimately share a sid. They are told apart by their file stem - the two
 * halves one import wrote - and each is its own row with its own site.
 */
static void
test_two_sites_of_one_system_are_two_rows(void) {
    RrLibrary lib;
    rr_library_init(&lib);

    dsd_rr_provenance polk;
    prov_group(&polk, 8734, "Iowa Statewide", 1, DSD_RR_PROTO_P25, 770418750LL, 1, 0);
    DSD_STRNCPY(polk.site_label, "Polk (Des Moines)", sizeof polk.site_label - 1);
    DSD_STRNCPY(polk.site_ids, "16863", sizeof polk.site_ids - 1);
    dsd_rr_provenance linn;
    prov_group(&linn, 8734, "Iowa Statewide", 1, DSD_RR_PROTO_P25, 770668750LL, 1, 0);
    DSD_STRNCPY(linn.site_label, "Linn (Cedar Rapids)", sizeof linn.site_label - 1);
    DSD_STRNCPY(linn.site_ids, "23581", sizeof linn.site_ids - 1);

    expect("polk folds", rr_library_add(&lib, "/imports/Iowa Statewide - Polk group.csv", &polk) == 0);
    expect("linn folds", rr_library_add(&lib, "/imports/Iowa Statewide - Linn group.csv", &linn) == 0);
    expect("one row per site", lib.count == 2);
    expect_str("first row's site", lib.systems[0].site_label, "Polk (Des Moines)");
    expect_str("second row's site", lib.systems[1].site_label, "Linn (Cedar Rapids)");
    expect("both rows keep the system id", lib.systems[0].sid == 8734 && lib.systems[1].sid == 8734);

    /* Each site's two halves still pair up - by stem, which is what one import
       wrote both of its files under. */
    dsd_rr_provenance polk_chan = polk;
    DSD_STRNCPY(polk_chan.kind, "chan", sizeof polk_chan.kind - 1);
    expect("polk chan folds", rr_library_add(&lib, "/imports/Iowa Statewide - Polk chan.csv", &polk_chan) == 0);
    expect("still two rows", lib.count == 2);
    expect("polk has both halves", lib.systems[0].has_group == 1 && lib.systems[0].has_chan == 1);
    expect("linn still has only its group half", lib.systems[1].has_chan == 0);
}

/*
 * RadioReference renames a system from time to time. The re-import lands under
 * a new stem and the old pair stays on disk, so it gets a row of its own: it is
 * a real stored import, and a row is the only way to use, refresh or - most of
 * the time - delete it.
 */
static void
test_a_renamed_system_leaves_both_pairs_listed(void) {
    for (int order = 0; order < 2; order++) {
        RrLibrary lib;
        rr_library_init(&lib);

        dsd_rr_provenance older;
        prov_group(&older, 111, "Metro Radio", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
        older.imported_at = 1000;
        dsd_rr_provenance newer;
        prov_group(&newer, 111, "Metro Regional", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
        newer.imported_at = 2000;

        if (order == 0) {
            (void)rr_library_add(&lib, "/imports/Metro Radio - Site 1 group.csv", &older);
            (void)rr_library_add(&lib, "/imports/Metro Regional - Site 1 group.csv", &newer);
        } else {
            (void)rr_library_add(&lib, "/imports/Metro Regional - Site 1 group.csv", &newer);
            (void)rr_library_add(&lib, "/imports/Metro Radio - Site 1 group.csv", &older);
        }
        expect("the stale pair is not hidden", lib.count == 2);
        rr_library_sort(&lib);
        expect_str("both names are listed", lib.systems[0].name, "Metro Radio");
        expect_str("both names are listed", lib.systems[1].name, "Metro Regional");
    }
}

/* sort orders by name, then site, then sid: sibling sites of one system land
   together and in a stable order. */
static void
test_sort_orders_by_name(void) {
    RrLibrary lib;
    rr_library_init(&lib);
    dsd_rr_provenance g;
    prov_group(&g, 3, "Charlie", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    (void)rr_library_add(&lib, "/imports/Charlie group.csv", &g);
    prov_group(&g, 1, "Alpha", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    (void)rr_library_add(&lib, "/imports/Alpha group.csv", &g);
    prov_group(&g, 2, "Bravo", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    (void)rr_library_add(&lib, "/imports/Bravo group.csv", &g);
    prov_group(&g, 2, "Bravo", 1, DSD_RR_PROTO_P25, 770000000LL, 1, 0);
    DSD_STRNCPY(g.site_label, "Ames", sizeof g.site_label - 1);
    (void)rr_library_add(&lib, "/imports/Bravo - Ames group.csv", &g);
    rr_library_sort(&lib);
    expect_str("first", lib.systems[0].name, "Alpha");
    expect_str("second", lib.systems[1].name, "Bravo");
    expect_str("sites of one system sort together", lib.systems[1].site_label, "Ames");
    expect_str("and in order", lib.systems[2].site_label, "Site 1");
    expect_str("third", lib.systems[3].name, "Charlie");
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

/* Scanning a real directory folds sidecar'd files and skips the rest. */
static void
test_scan_directory(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_rr_lib") == NULL) {
        expect("scratch dir created", 0);
        return;
    }

    char group_csv[DSD_TEST_PATH_MAX];
    char chan_csv[DSD_TEST_PATH_MAX];
    char orphan_csv[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(group_csv, sizeof group_csv, dir, "SARA group.csv");
    (void)dsd_test_path_join(chan_csv, sizeof chan_csv, dir, "SARA chan.csv");
    (void)dsd_test_path_join(orphan_csv, sizeof orphan_csv, dir, "handmade.csv");
    (void)write_file(group_csv, "1,ONE,D\n");
    (void)write_file(chan_csv, "1,770.0\n");
    (void)write_file(orphan_csv, "1,771.0\n"); /* no sidecar: skipped */

    dsd_rr_provenance g;
    prov_group(&g, 6673, "SARA", 1, DSD_RR_PROTO_P25, 769768750LL, 1, 0);
    expect("group sidecar written", dsd_rr_provenance_write(group_csv, &g) == 0);
    DSD_STRNCPY(g.kind, "chan", sizeof g.kind - 1);
    expect("chan sidecar written", dsd_rr_provenance_write(chan_csv, &g) == 0);

    RrLibrary lib;
    int n = rr_library_scan(&lib, dir);
    expect("scan finds one system", n == 1);
    expect("scan count", lib.count == 1);
    expect("scan folds both halves", lib.systems[0].has_group == 1 && lib.systems[0].has_chan == 1);
    expect_str("scan group path", lib.systems[0].group_path, group_csv);

    /* Cleanup: CSVs, sidecars, orphan, dir. */
    char side[DSD_TEST_PATH_MAX + 8];
    (void)DSD_SNPRINTF(side, sizeof side, "%s.rr", group_csv);
    (void)remove(side);
    (void)DSD_SNPRINTF(side, sizeof side, "%s.rr", chan_csv);
    (void)remove(side);
    (void)remove(group_csv);
    (void)remove(chan_csv);
    (void)remove(orphan_csv);
    (void)TEST_RMDIR(dir);
}

int
main(void) {
    test_group_and_chan_fold_into_one_system();
    test_distinct_sids_are_distinct_systems();
    test_row_format_trunked();
    test_row_format_files_only();
    test_row_format_scan_list();
    test_row_format_truncates_long_text();
    test_layout_drops_an_empty_site_column();
    test_layout_shares_the_width_it_has();
    test_display_name_names_the_site();
    test_in_use_predicate();
    test_overflow_is_flagged();
    test_unusable_sidecars_are_skipped();
    test_two_sites_of_one_system_are_two_rows();
    test_a_renamed_system_leaves_both_pairs_listed();
    test_sort_orders_by_name();
    test_scan_directory();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("UI_RR_LIBRARY: OK\n");
    return 0;
}
