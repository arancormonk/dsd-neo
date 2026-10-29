// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include "fuzz_support.h"

#include <stdlib.h>

static int
set_path(char* dst, size_t dst_size, const char* path) {
    int n = DSD_SNPRINTF(dst, dst_size, "%s", path);
    return (n < 0 || (size_t)n >= dst_size) ? -1 : 0;
}

/* Run the group-list lookups and evaluation over the imported rows on IDs taken from the input, plus the edges, so
 * pattern, range and exact rows are matched against IDs the input itself chose. */
static void
fuzz_group_lookups(const dsd_opts* opts, const dsd_state* state, const uint8_t* data, size_t size) {
    uint32_t ids[10] = {0U, UINT32_MAX};
    size_t count = 2U;
    for (size_t off = 0; off + 4U <= size && count < sizeof(ids) / sizeof(ids[0]); off += 4U) {
        ids[count++] = (uint32_t)data[off] | ((uint32_t)data[off + 1U] << 8) | ((uint32_t)data[off + 2U] << 16)
                       | ((uint32_t)data[off + 3U] << 24);
    }
    for (size_t i = 0; i < count; i++) {
        dsd_tg_policy_lookup lookup;
        dsd_tg_policy_decision decision;
        (void)dsd_tg_policy_lookup_id(state, ids[i], &lookup);
        (void)dsd_tg_policy_lookup_group(state, ids[i], &lookup);
        (void)dsd_tg_policy_evaluate_group_call(opts, state, ids[i], 1U, 0, 0, &decision);
    }
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

    switch (selector % 8U) {
        case 0:
            (void)csvGroupImportPath(path, state);
            fuzz_group_lookups(opts, state, data, size);
            break;
        case 1:
            if (set_path(opts->chan_in_file, sizeof(opts->chan_in_file), path) == 0) {
                (void)csvChanImport(opts, state);
            }
            break;
        case 2:
            if (set_path(opts->key_in_file, sizeof(opts->key_in_file), path) == 0) {
                (void)csvKeyImportDec(opts, state);
            }
            break;
        case 3:
            if (set_path(opts->key_in_file, sizeof(opts->key_in_file), path) == 0) {
                (void)csvKeyImportHex(opts, state);
            }
            break;
        case 4: (void)csvVertexKsImport(state, path); break;
        case 5: (void)csvDmrTgKeyImport(state, path); break;
        case 6: (void)csvP25BandplanImportPath(path, state); break;
        default: (void)csvSrcImportPath(path, state); break;
    }

    dsd_state_ext_free_all(state);
    dsd_state_trunk_lcn_free(state);
    free(state);
    free(opts);
    (void)remove(path);
    return 0;
}
