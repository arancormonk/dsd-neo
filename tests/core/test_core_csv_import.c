// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int
pick_missing_dir(char* out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }
    for (int i = 0; i < 1000; ++i) {
        (void)snprintf(out, out_sz, "dsd-neo-test-missing-dir-%d", i);
        struct stat st;
        if (stat(out, &st) != 0) {
            return 0;
        }
    }
    return -1;
}

static int
test_group_import_missing_file(void) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        return 1;
    }

    state.group_tally = 123;
    (void)snprintf(opts.group_in_file, sizeof opts.group_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvGroupImport(&opts, &state);
    if (rc == 0) {
        return 1;
    }
    if (state.group_tally != 123) {
        return 1;
    }
    return 0;
}

static int
test_channel_import_missing_file(void) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof opts);
    memset(&state, 0, sizeof state);

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        return 1;
    }

    state.lcn_freq_count = 456;
    (void)snprintf(opts.chan_in_file, sizeof opts.chan_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvChanImport(&opts, &state);
    if (rc == 0) {
        return 1;
    }
    if (state.lcn_freq_count != 456) {
        return 1;
    }
    return 0;
}

int
main(void) {
    if (test_group_import_missing_file() != 0) {
        return 1;
    }
    if (test_channel_import_missing_file() != 0) {
        return 1;
    }
    return 0;
}
