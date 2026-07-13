// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(readability-suspicious-call-argument)
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Control-channel cache loading tests.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/posix_compat.h"
#include "test_support.h"

#define setenv dsd_test_setenv

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_long(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
write_cache_fixture(const char* path) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        DSD_FPRINTF(stderr, "failed to write cache fixture: %s\n", strerror(errno));
        return 0;
    }
    DSD_FPRINTF(fp, "851111111\n");
    DSD_FPRINTF(fp, "cc 851222222\n");
    DSD_FPRINTF(fp, "  cc\t851333333\n");
    DSD_FPRINTF(fp, "notcc 851444444\n");
    fclose(fp);
    return 1;
}

int
main(void) {
    int rc = 0;
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_cc_path")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 100;
    }
    char cache_root[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(cache_root, sizeof cache_root, dir, "cache") != 0) {
        DSD_FPRINTF(stderr, "failed to build cache root path: %s\n", strerror(errno));
        return 100;
    }
    setenv("DSD_NEO_CACHE_DIR", cache_root, 1);
    dsd_neo_config_init();

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);

    static dsd_state missing_root;
    DSD_MEMSET(&missing_root, 0, sizeof missing_root);
    missing_root.p2_wacn = 0xABCDEULL;
    missing_root.p2_sysid = 0x122ULL;
    const long no_neighbor = 0;
    p25_cc_record_neighbor_frequencies(&opts, &missing_root, &no_neighbor, 1);
    dsd_stat_t cache_root_stat;
    rc |= expect_eq_int("read-only loader does not create cache directory", dsd_stat_path(cache_root, &cache_root_stat),
                        -1);
    if (dsd_mkdir(cache_root, 0700) != 0) {
        DSD_FPRINTF(stderr, "failed to create cache fixture directory: %s\n", strerror(errno));
        return 100;
    }

    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);
    st.p2_wacn = 0xABCDEULL;
    st.p2_sysid = 0x123ULL;
    st.p2_rfssid = 7ULL;
    st.p2_siteid = 11ULL;
    char cache_path[DSD_TEST_PATH_MAX];
    int n = DSD_SNPRINTF(cache_path, sizeof cache_path, "%s/p25_cc_%05llX_%03llX_R%03llu_S%03llu.txt", cache_root,
                         st.p2_wacn, st.p2_sysid, st.p2_rfssid, st.p2_siteid);
    if (n <= 0 || (size_t)n >= sizeof cache_path || !write_cache_fixture(cache_path)) {
        return 101;
    }

    p25_cc_record_neighbor_frequencies(&opts, NULL, &no_neighbor, 1);
    p25_cc_record_neighbor_frequencies(&opts, &st, &no_neighbor, 1);

    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(&st);
    rc |= expect_eq_int("marked cache loaded", st.p25_cc_cache_loaded, 1);
    rc |= expect_eq_int("loaded marked candidates only", cc ? cc->count : 0, 2);
    if (cc && cc->count == 2) {
        rc |= expect_eq_long("loaded cache candidate 0", cc->candidates[0], 851222222L);
        rc |= expect_eq_long("loaded cache candidate 1", cc->candidates[1], 851333333L);
        rc |= expect_eq_int("loaded cache candidate 0 flag", (cc->flags[0] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0,
                            1);
        rc |= expect_eq_int("loaded cache candidate 1 flag", (cc->flags[1] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0,
                            1);
    }

    p25_cc_record_neighbor_frequencies(&opts, &st, &no_neighbor, 1);
    rc |= expect_eq_int("already loaded preserves count", cc ? cc->count : 0, 2);

    static dsd_state missing;
    DSD_MEMSET(&missing, 0, sizeof missing);
    missing.p2_wacn = 0xABCDEULL;
    missing.p2_sysid = 0x124ULL;
    missing.p2_rfssid = 8ULL;
    missing.p2_siteid = 12ULL;
    p25_cc_record_neighbor_frequencies(&opts, &missing, &no_neighbor, 1);
    rc |= expect_eq_int("missing cache marked loaded", missing.p25_cc_cache_loaded, 1);
    rc |= expect_eq_int("missing cache count", dsd_trunk_cc_candidates_peek(&missing) != NULL, 0);

    rc |= expect_eq_int("add null candidate", p25_cc_add_candidate(NULL, 852111111L, 1), 0);
    rc |= expect_eq_int("add zero candidate", p25_cc_add_candidate(&st, 0, 1), 0);
    st.p25_cc_freq = 852222222L;
    rc |= expect_eq_int("add current cc candidate", p25_cc_add_candidate(&st, 852222222L, 1), 0);
    rc |= expect_eq_int("add current-site candidate", p25_cc_add_candidate(&st, 852555555L, 1), 1);
    rc |= expect_eq_int("add generic candidate", dsd_trunk_cc_candidates_add(&st, 852666666L, 1, 0), 1);

    return rc;
}

// NOLINTEND(readability-suspicious-call-argument)
