// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-misplaced-widening-cast)
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 regroup/patch tracking tests.
 * Covers: add/update, dedup, membership counts, details formatting,
 * TTL sweep of stale entries, and clear/remove deactivation.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
find_idx(const dsd_state* st, uint16_t sgid) {
    for (int i = 0; i < st->p25_patch_count && i < 8; i++) {
        if (st->p25_patch_sgid[i] == sgid) {
            return i;
        }
    }
    return -1;
}

static int
test_guard_replacement_and_policy_edges(void) {
    int rc = 0;
    static dsd_state st;
    char out[128];

    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_update(NULL, 1, 1, 1);
    p25_patch_add_wgid(NULL, 1, 1);
    p25_patch_add_wuid(NULL, 1, 1);
    p25_patch_remove_wgid(NULL, 1, 1);
    p25_patch_clear_sg(NULL, 1);
    p25_patch_set_kas(NULL, 1, 0, 0, 0);
    rc |= expect_eq_int("details null out", p25_patch_compose_details(&st, NULL, sizeof out), 0);
    rc |= expect_eq_int("details zero cap", p25_patch_compose_details(&st, out, 0), 0);
    rc |= expect_eq_int("details null state", p25_patch_compose_details(NULL, out, sizeof out), 0);
    rc |= expect_eq_int("tg clear null", p25_patch_tg_key_is_clear(NULL, 123), 0);
    rc |= expect_eq_int("sg clear null", p25_patch_sg_key_is_clear(NULL, 123), 0);

    st.p25_patch_count = -3;
    p25_patch_update(&st, 300, 1, 1);
    rc |= expect_eq_int("negative count recovers", st.p25_patch_count, 1);
    rc |= expect_eq_int("negative count stores sgid", st.p25_patch_sgid[0], 300);

    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = 8;
    time_t base = time(NULL);
    for (int i = 0; i < 8; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(100 + i);
        st.p25_patch_last_update[i] = base - (8 - i);
        st.p25_patch_active[i] = 1;
        st.p25_patch_wgid_count[i] = 1;
        st.p25_patch_key_valid[i] = 1;
    }
    p25_patch_update(&st, 999, 0, 1);
    rc |= expect_eq_int("full table count", st.p25_patch_count, 8);
    rc |= expect_eq_int("full table replaces stalest", st.p25_patch_sgid[0], 999);
    rc |= expect_eq_int("replacement clears wgid count", st.p25_patch_wgid_count[0], 0);
    rc |= expect_eq_int("replacement clears key valid", st.p25_patch_key_valid[0], 0);
    rc |= expect_eq_int("replacement stores simulselect", st.p25_patch_is_patch[0], 0);

    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = 8;
    base = time(NULL);
    for (int i = 0; i < 8; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(200 + i);
        st.p25_patch_active[i] = 1;
        st.p25_patch_last_update[i] = base - (8 - i);
        st.p25_patch_wgid_count[i] = 1;
        st.p25_patch_wgid[i][0] = (uint16_t)(300 + i);
    }
    p25_patch_clear_sg(&st, 205);
    p25_patch_update(&st, 299, 1, 1);
    rc |= expect_eq_int("inactive slot reused count", st.p25_patch_count, 8);
    rc |= expect_true("inactive slot reused new sg", find_idx(&st, 299) >= 0);
    rc |= expect_true("inactive slot reuse preserves active oldest", find_idx(&st, 200) >= 0);
    int reused = find_idx(&st, 299);
    if (reused >= 0) {
        rc |= expect_eq_int("inactive slot reused clears members", st.p25_patch_wgid_count[reused], 0);
        rc |= expect_eq_int("inactive slot reused active", st.p25_patch_active[reused], 1);
    }

    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = 8;
    base = time(NULL);
    for (int i = 0; i < 8; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(400 + i);
        st.p25_patch_active[i] = 1;
        st.p25_patch_last_update[i] = base;
    }
    p25_patch_update(&st, 499, 1, 0);
    rc |= expect_eq_int("inactive full table does not evict count", st.p25_patch_count, 8);
    rc |= expect_true("inactive full table preserves active oldest", find_idx(&st, 400) >= 0);
    rc |= expect_true("inactive full table skips new inactive", find_idx(&st, 499) < 0);

    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_update(&st, 10, 1, 1);
    p25_patch_add_wgid(&st, 10, 111);
    p25_patch_set_kas(&st, 10, 0x2222, 0x84, 3);
    p25_patch_update(&st, 20, 1, 1);
    p25_patch_add_wgid(&st, 20, 222);
    int stale = find_idx(&st, 10);
    if (stale >= 0) {
        st.p25_patch_last_update[stale] = time(NULL) - 21;
    }
    int active = find_idx(&st, 20);
    if (active >= 0) {
        st.p25_patch_last_update[active] = time(NULL);
    }
    (void)p25_patch_compose_details(&st, out, sizeof out);
    rc |= expect_eq_int("sweep compacts count", st.p25_patch_count, 1);
    rc |= expect_eq_int("sweep preserves active sgid", st.p25_patch_sgid[0], 20);
    rc |= expect_eq_int("sweep preserves active member", st.p25_patch_wgid[0][0], 222);

    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_add_wgid(&st, 69, 0x2345);
    p25_patch_set_kas(&st, 69, 0, 0x84, 17);
    rc |= expect_eq_int("clear tg policy active", p25_patch_tg_key_is_clear(&st, 0x2345), 1);
    rc |= expect_eq_int("clear sg policy active", p25_patch_sg_key_is_clear(&st, 69), 1);
    st.p25_patch_last_update[0] = time(NULL) - 21;
    rc |= expect_eq_int("clear tg policy stale", p25_patch_tg_key_is_clear(&st, 0x2345), 0);
    rc |= expect_eq_int("clear sg policy stale", p25_patch_sg_key_is_clear(&st, 69), 0);
    st.p25_patch_last_update[0] = time(NULL);
    p25_patch_set_kas(&st, 69, 0x1234, -1, -1);
    rc |= expect_eq_int("nonclear tg policy", p25_patch_tg_key_is_clear(&st, 0x2345), 0);
    rc |= expect_eq_int("nonclear sg policy", p25_patch_sg_key_is_clear(&st, 69), 0);

    DSD_MEMSET(&st, 0, sizeof st);
    rc |= expect_eq_int("prepare active returns process", p25_patch_prepare_grg_update(&st, 500, 1, 1, 3), 1);
    p25_patch_add_wgid(&st, 500, 100);
    p25_patch_set_kas(&st, 500, 0x2222, 0x84, 3);
    rc |= expect_eq_int("prepare same ssn returns process", p25_patch_prepare_grg_update(&st, 500, 1, 1, 3), 1);
    p25_patch_add_wgid(&st, 500, 101);
    int idx500 = find_idx(&st, 500);
    rc |= expect_true("prepare same ssn keeps sg", idx500 >= 0);
    if (idx500 >= 0) {
        rc |= expect_eq_int("prepare same ssn accumulates", st.p25_patch_wgid_count[idx500], 2);
    }
    rc |= expect_eq_int("prepare changed ssn returns process", p25_patch_prepare_grg_update(&st, 500, 1, 1, 4), 1);
    p25_patch_add_wgid(&st, 500, 102);
    p25_patch_set_kas(&st, 500, 0x3333, 0x89, 4);
    idx500 = find_idx(&st, 500);
    rc |= expect_true("prepare changed ssn keeps sg", idx500 >= 0);
    if (idx500 >= 0) {
        uint16_t members[8] = {0};
        rc |= expect_eq_int("prepare changed ssn clears old", st.p25_patch_wgid_count[idx500], 1);
        rc |= expect_eq_int("prepare changed ssn new member", st.p25_patch_wgid[idx500][0], 102);
        rc |= expect_eq_int("collect active count", p25_patch_collect_active_wgids(&st, 500, members, 8), 1);
        rc |= expect_eq_int("collect active member", members[0], 102);
    }
    rc |= expect_eq_int("prepare inactive suppresses members", p25_patch_prepare_grg_update(&st, 500, 1, 0, 4), 0);
    idx500 = find_idx(&st, 500);
    rc |= expect_true("prepare inactive keeps record", idx500 >= 0);
    if (idx500 >= 0) {
        rc |= expect_eq_int("prepare inactive clears active", st.p25_patch_active[idx500], 0);
        rc |= expect_eq_int("prepare inactive clears count", st.p25_patch_wgid_count[idx500], 0);
        rc |= expect_eq_int("prepare inactive clears key valid", st.p25_patch_key_valid[idx500], 0);
    }

    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);

    // Create a patch SG=069 with WG membership and crypt context
    p25_patch_update(&st, 69, /*is_patch*/ 1, /*active*/ 1);
    p25_patch_add_wgid(&st, 69, 0x0345);
    p25_patch_add_wgid(&st, 69, 0x0789);
    p25_patch_add_wgid(&st, 69, 0x0ABC);
    // Dedup
    p25_patch_add_wgid(&st, 69, 0x0345);
    // Crypt
    p25_patch_set_kas(&st, 69, 0x1234, 0x84, 17);

    // Create a simulselect SG=077 with 3 unit members
    p25_patch_update(&st, 77, /*is_patch*/ 0, /*active*/ 1);
    p25_patch_add_wuid(&st, 77, 1001);
    p25_patch_add_wuid(&st, 77, 1002);
    p25_patch_add_wuid(&st, 77, 1003);

    // Create another patch SG=142 with no membership
    p25_patch_update(&st, 142, /*is_patch*/ 1, /*active*/ 1);

    char det[256];
    int dl = p25_patch_compose_details(&st, det, sizeof det);
    rc |= expect_true("details len>0", dl > 0);
    // SG069 shows WG list up to 3 and crypt context
    rc |= expect_true("details includes SG069[P]", strstr(det, "SG069[P]") != NULL);
    rc |= expect_true("details includes WG list", strstr(det, "WG:0837,1929,2748") != NULL);
    rc |= expect_true("details includes crypt", strstr(det, "K:1234 A:84 S:17") != NULL);
    // SG077 simulselect appears with U:3
    rc |= expect_true("details includes SG077[S]", strstr(det, "SG077[S]") != NULL);
    rc |= expect_true("details includes U:3", strstr(det, " U:3") != NULL);
    // SG142 shows as patch with no WG/U context
    rc |= expect_true("details includes SG142[P]", strstr(det, "SG142[P]") != NULL);

    // Add a 4th WGID to SG069 to trigger compact details form WG:4(a,b+)
    p25_patch_add_wgid(&st, 69, 0x0DEF);
    (void)p25_patch_compose_details(&st, det, sizeof det);
    rc |= expect_true("details compact WG", strstr(det, "WG:4(0837,1929+") != NULL);

    // TTL sweep: mark SG142 stale, ensure it disappears from details
    // (op25 uses 20s PATCH_EXPIRY_TIME)
    int idx142 = find_idx(&st, 142);
    if (idx142 >= 0) {
        st.p25_patch_last_update[idx142] = time(NULL) - 21; // >20s ago (op25 aligned)
    }
    (void)p25_patch_compose_details(&st, det, sizeof det);
    rc |= expect_true("details dropped SG142", strstr(det, "SG142[") == NULL);

    // Clear SG069; expect SG069 inactive
    p25_patch_clear_sg(&st, 69);

    // Removal makes entry inactive when last member removed
    // Re-add as patch and remove members one-by-one
    p25_patch_update(&st, 69, 1, 1);
    p25_patch_add_wgid(&st, 69, 0x1111);
    p25_patch_add_wgid(&st, 69, 0x2222);
    p25_patch_remove_wgid(&st, 69, 0x1111);
    p25_patch_remove_wgid(&st, 69, 0x2222);
    // Compose details should not include SG069 anymore (inactive)
    (void)p25_patch_compose_details(&st, det, sizeof det);
    rc |= expect_true("SG069 inactive after removals", strstr(det, "SG069[") == NULL);

    // SG077[S] still present (simulselect) with U:3
    rc |= expect_true("SG077 remains", strstr(det, "SG077[S]") != NULL);
    rc |= test_guard_replacement_and_policy_edges();

    (void)errno;
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(bugprone-misplaced-widening-cast)
