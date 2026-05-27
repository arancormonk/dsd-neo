// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>

#include "fuzz_support.h"

#include <stdlib.h>

static int
set_path(char* dst, size_t dst_size, const char* path) {
    int n = DSD_SNPRINTF(dst, dst_size, "%s", path);
    return (n < 0 || (size_t)n >= dst_size) ? -1 : 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL || size == 0U) {
        return 0;
    }

    char path[DSD_TEST_PATH_MAX];
    uint8_t selector = data[0];
    data++;
    size = dsd_fuzz_bounded_size(size - 1U);

    if (dsd_fuzz_make_temp_path(path, sizeof(path), "dsdneo_fuzz_csv") != 0) {
        return 0;
    }
    if (dsd_fuzz_write_file(path, data, size) != 0) {
        (void)remove(path);
        return 0;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1U, sizeof(*state));
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        (void)remove(path);
        return 0;
    }

    switch (selector % 6U) {
        case 0: (void)csvGroupImportPath(path, state); break;
        case 1:
            if (set_path(opts->lcn_in_file, sizeof(opts->lcn_in_file), path) == 0) {
                (void)csvLCNImport(opts, state);
            }
            break;
        case 2:
            if (set_path(opts->chan_in_file, sizeof(opts->chan_in_file), path) == 0) {
                (void)csvChanImport(opts, state);
            }
            break;
        case 3:
            if (set_path(opts->key_in_file, sizeof(opts->key_in_file), path) == 0) {
                (void)csvKeyImportDec(opts, state);
            }
            break;
        case 4:
            if (set_path(opts->key_in_file, sizeof(opts->key_in_file), path) == 0) {
                (void)csvKeyImportHex(opts, state);
            }
            break;
        default: (void)csvVertexKsImport(state, path); break;
    }

    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    (void)remove(path);
    return 0;
}
