// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #672 (B18): the imports directory services the terminal's CSV picker, Imported Systems browser and RadioReference
 * import wizard call instead of doing the file I/O themselves, on a real scratch directory.
 */

#include <dsd-neo/app_control/import_files.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdio.h>
#include <string.h>
#include "test_env.h"
#include "test_support.h"

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
write_file(const char* path, const char* body) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    int rc = fputs(body, fp) < 0 ? -1 : 0;
    rc |= fclose(fp) != 0 ? -1 : 0;
    return rc;
}

/* Whether @p path holds exactly @p want. */
static int
file_is(const char* path, const char* want) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        return 0;
    }
    char buf[256];
    const size_t n = fread(buf, 1U, sizeof buf, fp);
    (void)fclose(fp);
    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

static int
exists(const char* path) {
    dsd_stat_t st;
    return dsd_stat_path(path, &st) == 0;
}

static void
fill_prov(dsd_rr_provenance* p, const char* kind, int sid, const char* site_ids) {
    DSD_MEMSET(p, 0, sizeof(*p));
    DSD_SNPRINTF(p->kind, sizeof p->kind, "%s", kind);
    p->sid = sid;
    DSD_SNPRINTF(p->site_ids, sizeof p->site_ids, "%s", site_ids);
    DSD_SNPRINTF(p->system_name, sizeof p->system_name, "%s", "SARA");
    p->imported_at = 1700000000LL;
}

typedef struct {
    int visits;
    int stop_after;
    char names[4][64];
    char kinds[4][16];
    char paths[4][DSD_TEST_PATH_MAX];
} list_seen;

static int
record_visit(const char* path, const char* name, const dsd_rr_provenance* prov, void* user) {
    list_seen* seen = (list_seen*)user;
    if (seen->visits < 4) {
        DSD_SNPRINTF(seen->names[seen->visits], sizeof seen->names[0], "%s", name);
        DSD_SNPRINTF(seen->kinds[seen->visits], sizeof seen->kinds[0], "%s", prov->kind);
        DSD_SNPRINTF(seen->paths[seen->visits], sizeof seen->paths[0], "%s", path);
    }
    seen->visits++;
    return (seen->stop_after > 0 && seen->visits >= seen->stop_after) ? 1 : 0;
}

/* The listing hands over each ".csv" with a readable sidecar, with its path, name and provenance, and skips the rest;
   a visitor can stop it; a missing directory is the first-run state, told apart from a bad call. */
static int
test_list(const char* dir) {
    int rc = 0;
    char group_csv[DSD_TEST_PATH_MAX];
    char chan_csv[DSD_TEST_PATH_MAX];
    char handmade[DSD_TEST_PATH_MAX];
    char notes[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(group_csv, sizeof group_csv, dir, "SARA group.csv");
    (void)dsd_test_path_join(chan_csv, sizeof chan_csv, dir, "SARA chan.csv");
    (void)dsd_test_path_join(handmade, sizeof handmade, dir, "handmade.csv");
    (void)dsd_test_path_join(notes, sizeof notes, dir, "notes.txt");
    rc |= expect_int("list: files written",
                     write_file(group_csv, "1,A,One,T\n") | write_file(chan_csv, "1,770000000\n")
                         | write_file(handmade, "1,771000000\n") | write_file(notes, "x\n"),
                     0);
    dsd_rr_provenance prov;
    fill_prov(&prov, "group", 6673, "11");
    rc |= expect_int("list: group sidecar", dsd_app_import_provenance_write(group_csv, &prov), 0);
    fill_prov(&prov, "chan", 6673, "11");
    rc |= expect_int("list: chan sidecar", dsd_app_import_provenance_write(chan_csv, &prov), 0);

    list_seen seen;
    DSD_MEMSET(&seen, 0, sizeof seen);
    rc |= expect_int("list: walked", dsd_app_imports_list(dir, record_visit, &seen), 0);
    rc |= expect_int("list: the two sidecar'd CSVs alone", seen.visits, 2);
    const int group_first = strcmp(seen.names[0], "SARA group.csv") == 0;
    const int g = group_first ? 0 : 1;
    rc |= expect_str("list: group name", seen.names[g], "SARA group.csv");
    rc |= expect_str("list: group kind", seen.kinds[g], "group");
    rc |= expect_str("list: group path", seen.paths[g], group_csv);
    rc |= expect_str("list: chan name", seen.names[1 - g], "SARA chan.csv");
    rc |= expect_str("list: chan kind", seen.kinds[1 - g], "chan");
    rc |= expect_str("list: chan path", seen.paths[1 - g], chan_csv);

    DSD_MEMSET(&seen, 0, sizeof seen);
    seen.stop_after = 1;
    rc |= expect_int("list: a stopped walk is no failure", dsd_app_imports_list(dir, record_visit, &seen), 0);
    rc |= expect_int("list: stopped after one", seen.visits, 1);

    char missing[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(missing, sizeof missing, dir, "not-yet");
    DSD_MEMSET(&seen, 0, sizeof seen);
    rc |= expect_int("list: missing directory", dsd_app_imports_list(missing, record_visit, &seen), 1);
    rc |= expect_int("list: missing directory visits nothing", seen.visits, 0);
    rc |= expect_int("list: a file is not a directory", dsd_app_imports_list(notes, record_visit, &seen), -1);
    rc |= expect_int("list: no directory", dsd_app_imports_list("", record_visit, &seen), -1);
    rc |= expect_int("list: no visitor", dsd_app_imports_list(dir, NULL, NULL), -1);

    (void)remove(handmade);
    (void)remove(notes);
    return rc;
}

/* Delete takes the CSV and its sidecar, and says whether the CSV went. */
static int
test_remove(const char* dir) {
    int rc = 0;
    char group_csv[DSD_TEST_PATH_MAX];
    char side[DSD_TEST_PATH_MAX + 4];
    (void)dsd_test_path_join(group_csv, sizeof group_csv, dir, "SARA group.csv");
    DSD_SNPRINTF(side, sizeof side, "%s.rr", group_csv);
    rc |= expect_int("remove: sidecar there", exists(side), 1);
    rc |= expect_int("remove: the CSV went", dsd_app_import_remove(group_csv), 1);
    rc |= expect_int("remove: CSV gone", exists(group_csv), 0);
    rc |= expect_int("remove: sidecar gone", exists(side), 0);
    rc |= expect_int("remove: again removes nothing", dsd_app_import_remove(group_csv), 0);
    rc |= expect_int("remove: no path", dsd_app_import_remove(""), 0);
    rc |= expect_int("remove: NULL path", dsd_app_import_remove(NULL), 0);
    char chan_csv[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(chan_csv, sizeof chan_csv, dir, "SARA chan.csv");
    rc |= expect_int("remove: the other half", dsd_app_import_remove(chan_csv), 1);
    return rc;
}

/* A write lands the exact bytes, replaces a file there and leaves no temp; a checked replace keeps the stored file when
   the new text is not a valid list, and swaps it in when it is. */
static int
test_write_and_replace(const char* dir) {
    int rc = 0;
    char path[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(path, sizeof path, dir, "Fire group.csv");
    static const char k_group[] = "DEC,Mode,Name,Tag\n1449,A,Fire Dispatch,Fire\n";
    static const char k_group2[] = "DEC,Mode,Name,Tag\n1450,A,Fire Ops,Fire\n";
    rc |= expect_int("write: written", dsd_app_import_write(path, k_group, strlen(k_group)), 0);
    rc |= expect_int("write: the bytes", file_is(path, k_group), 1);
    rc |= expect_int("write: replaces", dsd_app_import_write(path, k_group2, strlen(k_group2)), 0);
    rc |= expect_int("write: the new bytes", file_is(path, k_group2), 1);
    rc |= expect_int("write: no path", dsd_app_import_write("", k_group, strlen(k_group)), -1);

    static const char k_header_only[] = "DEC,Mode,Name,Tag\n";
    rc |= expect_int("replace: a list with no rows is refused",
                     dsd_app_import_replace_checked(path, k_header_only, strlen(k_header_only), 0), -2);
    rc |= expect_int("replace: refused keeps the stored file", file_is(path, k_group2), 1);
    rc |= expect_int("replace: a real list", dsd_app_import_replace_checked(path, k_group, strlen(k_group), 0), 0);
    rc |= expect_int("replace: swapped in", file_is(path, k_group), 1);
    rc |= expect_int("replace: no text", dsd_app_import_replace_checked(path, NULL, 0U, 0), -1);

    char chan[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(chan, sizeof chan, dir, "Fire chan.csv");
    static const char k_chan[] = "channel(dec), freq(Hz)\n141,423862500\n";
    static const char k_chan_header[] = "channel(dec), freq(Hz)\n";
    rc |= expect_int("replace: chan written", dsd_app_import_write(chan, k_chan, strlen(k_chan)), 0);
    rc |= expect_int("replace: a map with no rows is refused",
                     dsd_app_import_replace_checked(chan, k_chan_header, strlen(k_chan_header), 1), -2);
    rc |= expect_int("replace: the map kept", file_is(chan, k_chan), 1);
    (void)remove(path);
    (void)remove(chan);
    return rc;
}

/* The sidecar reads back what was written; a path is spoken for unless it is free or holds this very import. */
static int
test_provenance_and_conflicts(const char* dir) {
    int rc = 0;
    char path[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(path, sizeof path, dir, "Iowa - Ames group.csv");
    rc |= expect_int("conflict: a free path", dsd_app_import_path_conflicts(path, 7, "1,2"), 0);
    rc |= expect_int("conflict: CSV written", write_file(path, "1,A,One,T\n"), 0);
    rc |= expect_int("conflict: a hand-made file", dsd_app_import_path_conflicts(path, 7, "1,2"), 1);

    dsd_rr_provenance prov;
    fill_prov(&prov, "group", 7, "1,2");
    rc |= expect_int("provenance: written", dsd_app_import_provenance_write(path, &prov), 0);
    dsd_rr_provenance back;
    DSD_MEMSET(&back, 0, sizeof back);
    rc |= expect_int("provenance: read", dsd_app_import_provenance_read(path, &back), 0);
    rc |= expect_int("provenance: sid", back.sid, 7);
    rc |= expect_str("provenance: kind", back.kind, "group");
    rc |= expect_str("provenance: sites", back.site_ids, "1,2");
    rc |= expect_int("provenance: no path", dsd_app_import_provenance_read(NULL, &back), -1);
    rc |= expect_int("provenance: no out", dsd_app_import_provenance_read(path, NULL), -1);
    rc |= expect_int("provenance: nothing to write", dsd_app_import_provenance_write(path, NULL), -1);

    rc |= expect_int("conflict: this same import", dsd_app_import_path_conflicts(path, 7, "1,2"), 0);
    rc |= expect_int("conflict: another system", dsd_app_import_path_conflicts(path, 8, "1,2"), 1);
    rc |= expect_int("conflict: other sites of this system", dsd_app_import_path_conflicts(path, 7, "3"), 1);
    rc |= expect_int("delete the import", dsd_app_import_remove(path), 1);
    return rc;
}

/* The imports directory is made where the config lives, and then lists as an empty directory. */
static int
test_dir_create(const char* dir) {
    int rc = 0;
    const char* imports = dsd_user_imports_dir();
    char want_prefix[DSD_TEST_PATH_MAX];
    (void)dsd_test_path_join(want_prefix, sizeof want_prefix, dir, "dsd-neo");
    rc |= expect_int("dir: resolves under the scratch config",
                     imports != NULL && strncmp(imports, want_prefix, strlen(want_prefix)) == 0, 1);
    char path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(path, sizeof path, "%s", imports ? imports : "");
    list_seen seen;
    DSD_MEMSET(&seen, 0, sizeof seen);
    rc |= expect_int("dir: missing before", dsd_app_imports_list(path, record_visit, &seen), 1);
    rc |= expect_int("dir: created", dsd_app_imports_dir_create(), 0);
    rc |= expect_int("dir: lists once made", dsd_app_imports_list(path, record_visit, &seen), 0);
    rc |= expect_int("dir: empty", seen.visits, 0);
    rc |= expect_int("dir: created twice", dsd_app_imports_dir_create(), 0);
    (void)dsd_test_rmdir(path);
    (void)dsd_test_rmdir(want_prefix);
    return rc;
}

int
main(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_import_files") == NULL) {
        DSD_FPRINTF(stderr, "scratch dir not created\n");
        return 1;
    }
    /* The config, and so the imports directory, under the scratch directory: set before anything resolves it. */
    (void)dsd_test_setenv("XDG_CONFIG_HOME", dir, 1);
    (void)dsd_test_setenv("APPDATA", dir, 1);

    int rc = 0;
    rc |= test_list(dir);
    rc |= test_remove(dir);
    rc |= test_write_and_replace(dir);
    rc |= test_provenance_and_conflicts(dir);
    rc |= test_dir_create(dir);
    /* Removing the directory fails when anything (a stray temp file included) is left in it. */
    rc |= expect_int("scratch dir left empty", dsd_test_rmdir(dir), 0);
    if (rc != 0) {
        return 1;
    }
    printf("APP_CONTROL_IMPORT_FILES: OK\n");
    return 0;
}
