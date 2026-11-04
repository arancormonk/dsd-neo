// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #672 (B11/B18): the config file services the terminal menu calls instead of reading and writing the file
 * itself. Each reads or writes a real file and hands the decoder what it read as a command, captured here.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/config_files.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_env.h"
#include "test_support.h"

static int g_submit_result = DSD_APP_COMMAND_SUBMIT_QUEUED;

static struct {
    int load_calls;
    dsd_app_config_load_payload load;
    int metadata_calls;
    dsd_app_config_metadata_payload metadata;
} g_sent;

int
dsd_app_command_load_config(const dsd_app_config_load_payload* payload) {
    g_sent.load_calls++;
    if (payload) {
        DSD_MEMCPY(&g_sent.load, payload, sizeof g_sent.load);
    }
    return g_submit_result;
}

int
dsd_app_command_set_config_metadata(const dsd_app_config_metadata_payload* payload) {
    g_sent.metadata_calls++;
    if (payload) {
        g_sent.metadata = *payload;
    }
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

static void
reset_sent(void) {
    DSD_MEMSET(&g_sent, 0, sizeof g_sent);
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
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
write_temp_config(const char* contents, char* out_path, size_t out_sz) {
    int fd = dsd_test_mkstemp(out_path, out_sz, "dsdneo_config_files");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    const size_t len = strlen(contents);
    const ssize_t wr = dsd_write(fd, contents, len);
    (void)dsd_close(fd);
    if (wr < 0 || (size_t)wr != len) {
        DSD_FPRINTF(stderr, "write failed: %s\n", strerror(errno));
        (void)remove(out_path);
        return 1;
    }
    return 0;
}

static const char* const k_ini = "[mode]\n"
                                 "decode = \"p25p1\"\n"
                                 "\n"
                                 "[radioreference]\n"
                                 "username = \"alice\"\n"
                                 "\n"
                                 "[profile.fire]\n"
                                 "mode.decode = \"dmr\"\n";

/* A file load submits the whole file with its own autosave (on, the file), which the decoder adopts once it stays
   applied; a file that does not read submits nothing. */
static int
test_load_submits_the_file_and_its_autosave(const char* path) {
    int rc = 0;
    reset_sent();
    rc |= expect_int("load: read", dsd_app_config_load_submit(path), 0);
    rc |= expect_int("load: one apply", g_sent.load_calls, 1);
    rc |= expect_int("load: no metadata of its own", g_sent.metadata_calls, 0);
    rc |= expect_int("load: the file's mode",
                     g_sent.load.config.has_mode && g_sent.load.config.decode_mode == DSDCFG_MODE_P25P1, 1);
    rc |= expect_str("load: the file's keys", g_sent.load.config.rr_username, "alice");
    rc |= expect_int("load: autosave on", g_sent.load.autosave.autosave_enabled, 1);
    rc |= expect_str("load: autosave to the file", g_sent.load.autosave.path, path);
    rc |= expect_str("load: the whole file", g_sent.load.profile, "");

    char missing[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(missing, sizeof missing, "%s.missing", path);
    reset_sent();
    rc |= expect_int("load: missing file", dsd_app_config_load_submit(missing), -1);
    rc |= expect_int("load: missing submits nothing", g_sent.load_calls, 0);
    rc |= expect_int("load: no path", dsd_app_config_load_submit(""), -1);
    rc |= expect_int("load: no path submits nothing", g_sent.load_calls, 0);

    /* A queue that refuses the apply (no session) is a failed load, for the menu to say so. */
    reset_sent();
    g_submit_result = DSD_APP_COMMAND_SUBMIT_REJECTED;
    rc |= expect_int("load: refused by the queue", dsd_app_config_load_submit(path), -1);
    rc |= expect_int("load: refused after one try", g_sent.load_calls, 1);
    g_submit_result = DSD_APP_COMMAND_SUBMIT_QUEUED;
    return rc;
}

/* A profile load submits the base config with the profile on top and leaves autosave off, naming the file and the
   profile; an unknown profile submits nothing. */
static int
test_profile_load_overlays_and_leaves_autosave_off(const char* path) {
    int rc = 0;
    reset_sent();
    rc |= expect_int("profile: read", dsd_app_config_load_profile_submit(path, "fire"), 0);
    rc |= expect_int("profile: one apply", g_sent.load_calls, 1);
    rc |= expect_int("profile: no metadata of its own", g_sent.metadata_calls, 0);
    rc |= expect_int("profile: the overlay's mode", g_sent.load.config.decode_mode == DSDCFG_MODE_DMR, 1);
    rc |= expect_str("profile: the base keys", g_sent.load.config.rr_username, "alice");
    rc |= expect_int("profile: autosave off", g_sent.load.autosave.autosave_enabled, 0);
    rc |= expect_str("profile: the file", g_sent.load.autosave.path, path);
    rc |= expect_str("profile: the profile", g_sent.load.profile, "fire");

    reset_sent();
    rc |= expect_int("profile: unknown", dsd_app_config_load_profile_submit(path, "nosuch"), -1);
    rc |= expect_int("profile: unknown submits nothing", g_sent.load_calls, 0);

    reset_sent();
    g_submit_result = DSD_APP_COMMAND_SUBMIT_REJECTED;
    rc |= expect_int("profile: refused by the queue", dsd_app_config_load_profile_submit(path, "fire"), -1);
    g_submit_result = DSD_APP_COMMAND_SUBMIT_QUEUED;
    return rc;
}

static int
test_profiles_are_listed(const char* path) {
    int rc = 0;
    const char* names[8];
    char names_buf[256];
    const int n = dsd_app_config_list_profiles(path, names, names_buf, sizeof names_buf, 8);
    rc |= expect_int("list: one profile", n, 1);
    if (n == 1) {
        rc |= expect_str("list: its name", names[0], "fire");
    }
    char missing[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(missing, sizeof missing, "%s.missing", path);
    rc |= expect_int("list: missing file", dsd_app_config_list_profiles(missing, names, names_buf, sizeof names_buf, 8),
                     -1);
    return rc;
}

/* A save writes what the snapshot pair holds; only an adopting save makes the file the autosave, and only once it is
   written. */
static int
test_save_writes_the_snapshot_and_adopts_on_request(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.rr_username, sizeof opts.rr_username, "%s", "bob");

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config("", path, sizeof path) != 0) {
        return 1;
    }

    reset_sent();
    rc |= expect_int("save: written", dsd_app_config_save(path, &opts, &state, 0), 0);
    rc |= expect_int("save: no autosave change", g_sent.metadata_calls, 0);
    static dsdneoUserConfig loaded;
    rc |= expect_int("save: reads back", dsd_user_config_load(path, &loaded), 0);
    rc |= expect_str("save: the snapshot's keys", loaded.rr_username, "bob");

    reset_sent();
    rc |= expect_int("save as: written", dsd_app_config_save(path, &opts, &state, 1), 0);
    rc |= expect_int("save as: one metadata", g_sent.metadata_calls, 1);
    rc |= expect_int("save as: autosave on", g_sent.metadata.autosave_enabled, 1);
    rc |= expect_str("save as: autosave to the file", g_sent.metadata.path, path);
    rc |= expect_int("save as: applies nothing", g_sent.load_calls, 0);

    /* Under a regular file, where no directory can be made. */
    char unwritable[DSD_TEST_PATH_MAX + 32];
    DSD_SNPRINTF(unwritable, sizeof unwritable, "%s/config.ini", path);
    reset_sent();
    rc |= expect_int("save: unwritable", dsd_app_config_save(unwritable, &opts, &state, 1), -1);
    rc |= expect_int("save: unwritable adopts nothing", g_sent.metadata_calls, 0);
    reset_sent();
    rc |= expect_int("save: no snapshot", dsd_app_config_save(path, NULL, &state, 1), -1);
    rc |= expect_int("save: no snapshot adopts nothing", g_sent.metadata_calls, 0);

    (void)remove(path);
    return rc;
}

/* The menu-open hint's question (issue #672): the default file is missing until something writes it. The default path
   is resolved once per process, from XDG_CONFIG_HOME (APPDATA on Windows), here a scratch directory. */
static int
test_default_missing_until_written(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_config_default") == NULL) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    (void)dsd_test_setenv("XDG_CONFIG_HOME", dir, 1);
    (void)dsd_test_setenv("APPDATA", dir, 1);
    const char* def = dsd_user_config_default_path();
    if (def == NULL || strncmp(def, dir, strlen(dir)) != 0) {
        DSD_FPRINTF(stderr, "default path not under the scratch directory: %s\n", def ? def : "(none)");
        return 1;
    }
    char def_path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(def_path, sizeof def_path, "%s", def);

    char got[DSD_TEST_PATH_MAX];
    rc |= expect_int("default: missing before a save", dsd_app_config_default_missing(got, sizeof got), 1);
    rc |= expect_str("default: names the path", got, def_path);
    char tiny[4];
    rc |= expect_int("default: a path too long for the buffer says nothing",
                     dsd_app_config_default_missing(tiny, sizeof tiny), 0);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    reset_sent();
    rc |= expect_int("default: written", dsd_app_config_save(def_path, &opts, &state, 0), 0);
    rc |= expect_int("default: present after a save", dsd_app_config_default_missing(got, sizeof got), 0);

    (void)remove(def_path);
    char sub_dir[DSD_TEST_PATH_MAX + 16];
    DSD_SNPRINTF(sub_dir, sizeof sub_dir, "%s", def_path);
    char* sep = strrchr(sub_dir, '/');
    char* bsep = strrchr(sub_dir, '\\');
    if (bsep != NULL && (sep == NULL || bsep > sep)) {
        sep = bsep;
    }
    if (sep != NULL) {
        *sep = '\0';
        (void)dsd_test_rmdir(sub_dir);
    }
    (void)dsd_test_rmdir(dir);
    return rc;
}

int
main(void) {
    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(k_ini, path, sizeof path) != 0) {
        return 1;
    }
    int rc = test_load_submits_the_file_and_its_autosave(path);
    rc |= test_profile_load_overlays_and_leaves_autosave_off(path);
    rc |= test_profiles_are_listed(path);
    rc |= test_save_writes_the_snapshot_and_adopts_on_request();
    rc |= test_default_missing_until_written();
    (void)remove(path);
    if (rc == 0) {
        printf("APP_CONTROL_CONFIG_FILES: OK\n");
    }
    return rc;
}
