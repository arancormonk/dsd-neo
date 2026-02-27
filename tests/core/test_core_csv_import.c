// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free(opts);
        free(state);
        return 1;
    }

    state->group_tally = 123;
    (void)snprintf(opts->group_in_file, sizeof opts->group_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvGroupImport(opts, state);
    if (rc == 0) {
        free(opts);
        free(state);
        return 1;
    }
    if (state->group_tally != 123) {
        free(opts);
        free(state);
        return 1;
    }

    free(opts);
    free(state);
    return 0;
}

static int
test_channel_import_missing_file(void) {
    // dsd_state is a multi-megabyte struct; avoid Windows' default ~1MB stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free(opts);
        free(state);
        return 1;
    }

    state->lcn_freq_count = 456;
    (void)snprintf(opts->chan_in_file, sizeof opts->chan_in_file, "%s/%s", dir, "missing.csv");
    int rc = csvChanImport(opts, state);
    if (rc == 0) {
        free(opts);
        free(state);
        return 1;
    }
    if (state->lcn_freq_count != 456) {
        free(opts);
        free(state);
        return 1;
    }

    free(opts);
    free(state);
    return 0;
}

static int
test_group_import_capacity_cap(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }

    char tmpl[] = "dsd-neo-test-group-overflow-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(opts);
        free(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = fopen(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free(opts);
        free(state);
        return 1;
    }

    fprintf(fp, "group,mode,name\n");
    const size_t cap = sizeof(state->group_array) / sizeof(state->group_array[0]);
    const size_t rows = cap + 25;
    for (size_t i = 0; i < rows; i++) {
        fprintf(fp, "%zu,D,Alias %zu\n", i + 1, i + 1);
    }
    fclose(fp);

    (void)snprintf(opts->group_in_file, sizeof opts->group_in_file, "%s", tmpl);
    int rc = csvGroupImport(opts, state);

    int failed = 0;
    if (rc != 0) {
        failed = 1;
    }
    if (state->group_tally != cap) {
        failed = 1;
    }
    if (state->group_array[cap - 1].groupNumber != (unsigned long)cap) {
        failed = 1;
    }

    (void)remove(tmpl);
    free(opts);
    free(state);
    return failed;
}

static unsigned
bits_to_u8(const char* bits, int start) {
    unsigned v = 0U;
    for (int i = 0; i < 8; i++) {
        v = (v << 1) | (unsigned)(bits[start + i] & 1);
    }
    return v;
}

static int
test_vertex_import_missing_file(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    char dir[128];
    if (pick_missing_dir(dir, sizeof dir) != 0) {
        free(state);
        return 1;
    }

    state->vertex_ks_count = 7;
    int rc = csvVertexKsImport(state, dir);
    if (rc == 0) {
        free(state);
        return 1;
    }
    if (state->vertex_ks_count != 7) {
        free(state);
        return 1;
    }

    free(state);
    return 0;
}

static int
test_vertex_import_and_apply(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        return 1;
    }

    char tmpl[] = "dsd-neo-test-vertex-ks-XXXXXX";
    int fd = dsd_mkstemp(tmpl);
    if (fd < 0) {
        free(state);
        return 1;
    }
    (void)dsd_close(fd);

    FILE* fp = fopen(tmpl, "w");
    if (!fp) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    fprintf(fp, "key_hex,keystream_spec\n");
    fprintf(fp, "1234567891,8:F0:2:3\n");
    fprintf(fp, "ABCDEF,8:0F\n");
    fprintf(fp, "0,8:AA\n");
    fclose(fp);

    int rc = csvVertexKsImport(state, tmpl);
    if (rc != 0 || state->vertex_ks_count != 3) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (state->vertex_ks_key[0] != 0x1234567891ULL || state->vertex_ks_mod[0] != 8
        || state->vertex_ks_frame_mode[0] != 1 || state->vertex_ks_frame_off[0] != 2
        || state->vertex_ks_frame_step[0] != 3) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }

    char frame0[49];
    char frame1[49];
    char frame_slot1[49];
    char frame2[49];
    char frame_zero_key[49];
    memset(frame0, 0, sizeof(frame0));
    memset(frame1, 0, sizeof(frame1));
    memset(frame_slot1, 0, sizeof(frame_slot1));
    memset(frame2, 0, sizeof(frame2));
    memset(frame_zero_key, 0, sizeof(frame_zero_key));

    if (vertex_key_map_apply_frame49(state, 0, 0x1234567891ULL, frame0) != 1) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (vertex_key_map_apply_frame49(state, 0, 0x1234567891ULL, frame1) != 1) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (vertex_key_map_apply_frame49(state, 1, 0x1234567891ULL, frame_slot1) != 1) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (bits_to_u8(frame0, 0) != 0xC3U || bits_to_u8(frame1, 0) != 0x1EU || bits_to_u8(frame_slot1, 0) != 0xC3U) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }

    if (vertex_key_map_apply_frame49(state, 0, 0xABCDEFULL, frame2) != 1) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (bits_to_u8(frame2, 0) != 0x0FU) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }

    if (vertex_key_map_apply_frame49(state, 0, 0ULL, frame_zero_key) != 1) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (bits_to_u8(frame_zero_key, 0) != 0xAAU) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }

    char unknown[49];
    memset(unknown, 0, sizeof(unknown));
    if (vertex_key_map_apply_frame49(state, 0, 0x999999ULL, unknown) != 0) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }
    if (bits_to_u8(unknown, 0) != 0x00U) {
        (void)remove(tmpl);
        free(state);
        return 1;
    }

    (void)remove(tmpl);
    free(state);
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
    if (test_group_import_capacity_cap() != 0) {
        return 1;
    }
    if (test_vertex_import_missing_file() != 0) {
        return 1;
    }
    if (test_vertex_import_and_apply() != 0) {
        return 1;
    }
    return 0;
}
