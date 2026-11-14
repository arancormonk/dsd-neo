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

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
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
    for (int i = 0; i < st->p25_patch_count && i < DSD_P25_PATCH_MAX; i++) {
        if (st->p25_patch_sgid[i] == sgid) {
            return i;
        }
    }
    return -1;
}

/* Hear the control channel for @p seconds: CRC-good PDUs every 100 ms advance the listening clock. */
static void
listen_for(dsd_state* st, double seconds) {
    double now = st->p25_patch_listen_last_m;
    if (now <= 0.0) {
        now = 1000.0;
        dsd_p25_patch_listen_note(st, now);
    }
    const int steps = (int)(seconds * 10.0 + 0.5);
    for (int i = 0; i < steps; i++) {
        now += 0.1;
        dsd_p25_patch_listen_note(st, now);
    }
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

    // A full table of live entries replaces the least recently refreshed one (lowest sequence), whatever the
    // decode-time stamps say: entry 0 has the newest last_update but the lowest sequence.
    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = DSD_P25_PATCH_MAX;
    time_t base = time(NULL);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(100 + i);
        st.p25_patch_last_update[i] = base - i;
        st.p25_patch_seq[i] = (uint32_t)(i + 1);
        st.p25_patch_active[i] = 1;
        st.p25_patch_wgid_count[i] = 1;
        st.p25_patch_key_valid[i] = 1;
    }
    st.p25_patch_seq_counter = DSD_P25_PATCH_MAX;
    p25_patch_update(&st, 999, 0, 1);
    rc |= expect_eq_int("full table count", st.p25_patch_count, DSD_P25_PATCH_MAX);
    rc |= expect_eq_int("full table replaces stalest", st.p25_patch_sgid[0], 999);
    rc |= expect_eq_int("replacement clears wgid count", st.p25_patch_wgid_count[0], 0);
    rc |= expect_eq_int("replacement clears key valid", st.p25_patch_key_valid[0], 0);
    rc |= expect_eq_int("replacement stores simulselect", st.p25_patch_is_patch[0], 0);

    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = DSD_P25_PATCH_MAX;
    base = time(NULL);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(200 + i);
        st.p25_patch_active[i] = 1;
        st.p25_patch_last_update[i] = base - (8 - i);
        st.p25_patch_wgid_count[i] = 1;
        st.p25_patch_wgid[i][0] = (uint16_t)(300 + i);
    }
    p25_patch_clear_sg(&st, 205);
    p25_patch_update(&st, 299, 1, 1);
    rc |= expect_eq_int("inactive slot reused count", st.p25_patch_count, DSD_P25_PATCH_MAX);
    rc |= expect_true("inactive slot reused new sg", find_idx(&st, 299) >= 0);
    rc |= expect_true("inactive slot reuse preserves active oldest", find_idx(&st, 200) >= 0);
    int reused = find_idx(&st, 299);
    if (reused >= 0) {
        rc |= expect_eq_int("inactive slot reused clears members", st.p25_patch_wgid_count[reused], 0);
        rc |= expect_eq_int("inactive slot reused active", st.p25_patch_active[reused], 1);
    }

    DSD_MEMSET(&st, 0, sizeof st);
    st.p25_patch_count = DSD_P25_PATCH_MAX;
    base = time(NULL);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        st.p25_patch_sgid[i] = (uint16_t)(400 + i);
        st.p25_patch_active[i] = 1;
        st.p25_patch_last_update[i] = base;
    }
    p25_patch_update(&st, 499, 1, 0);
    rc |= expect_eq_int("inactive full table does not evict count", st.p25_patch_count, DSD_P25_PATCH_MAX);
    rc |= expect_true("inactive full table preserves active oldest", find_idx(&st, 400) >= 0);
    rc |= expect_true("inactive full table skips new inactive", find_idx(&st, 499) < 0);

    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_update(&st, 10, 1, 1);
    p25_patch_add_wgid(&st, 10, 111);
    p25_patch_set_kas(&st, 10, 0x2222, 0x84, 3);
    p25_patch_update(&st, 20, 1, 1);
    p25_patch_add_wgid(&st, 20, 222);
    // 21 s of listening later only SG 20 is refreshed: SG 10 drops out of the details, which never mutate the table.
    listen_for(&st, 21.0);
    p25_patch_add_wgid(&st, 20, 222);
    (void)p25_patch_compose_details(&st, out, sizeof out);
    rc |= expect_true("stale entry left out of details", strstr(out, "SG010[") == NULL);
    rc |= expect_true("current entry kept in details", strstr(out, "SG020[P] WG:0222") != NULL);
    rc |= expect_eq_int("details leave the table in place", st.p25_patch_count, 2);
    rc |= expect_true("details leave the stale entry", find_idx(&st, 10) >= 0);

    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_add_wgid(&st, 69, 0x2345);
    p25_patch_set_kas(&st, 69, 0, 0x84, 17);
    rc |= expect_eq_int("clear tg policy active", p25_patch_tg_key_is_clear(&st, 0x2345), 1);
    rc |= expect_eq_int("clear sg policy active", p25_patch_sg_key_is_clear(&st, 69), 1);
    listen_for(&st, 21.0);
    rc |= expect_eq_int("clear tg policy stale", p25_patch_tg_key_is_clear(&st, 0x2345), 0);
    rc |= expect_eq_int("clear sg policy stale", p25_patch_sg_key_is_clear(&st, 69), 0);
    p25_patch_add_wgid(&st, 69, 0x2345);
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
        uint16_t members[DSD_P25_PATCH_GROUP_MAX] = {0};
        rc |= expect_eq_int("prepare changed ssn clears old", st.p25_patch_wgid_count[idx500], 1);
        rc |= expect_eq_int("prepare changed ssn new member", st.p25_patch_wgid[idx500][0], 102);
        rc |= expect_eq_int("collect active count",
                            p25_patch_collect_active_wgids(&st, 500, members, DSD_P25_PATCH_GROUP_MAX), 1);
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

/* B4: 16 live patches fit without eviction; 32 groups and 16 radios per patch are stored, one more is dropped. */
static int
test_caps(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        p25_patch_add_wgid(&st, 1400 + i * 2, 1401 + i * 2);
    }
    rc |= expect_eq_int("caps 16 patches count", st.p25_patch_count, DSD_P25_PATCH_MAX);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        int idx = find_idx(&st, (uint16_t)(1400 + i * 2));
        rc |= expect_true("caps patch kept", idx >= 0);
        if (idx >= 0) {
            rc |= expect_eq_int("caps patch member kept", st.p25_patch_wgid[idx][0], 1401 + i * 2);
            rc |= expect_eq_int("caps patch active", st.p25_patch_active[idx], 1);
        }
    }

    DSD_MEMSET(&st, 0, sizeof st);
    for (int k = 0; k < DSD_P25_PATCH_GROUP_MAX + 1; k++) {
        p25_patch_add_wgid(&st, 1400, 2001 + k);
    }
    rc |= expect_eq_int("caps 32 groups stored", st.p25_patch_wgid_count[0], DSD_P25_PATCH_GROUP_MAX);
    for (int k = 0; k < DSD_P25_PATCH_GROUP_MAX; k++) {
        rc |= expect_eq_int("caps group order", st.p25_patch_wgid[0][k], 2001 + k);
    }
    for (int k = 0; k < DSD_P25_PATCH_RADIO_MAX + 1; k++) {
        p25_patch_add_wuid(&st, 1400, (uint32_t)(70001 + k));
    }
    rc |= expect_eq_int("caps 16 radios stored", st.p25_patch_wuid_count[0], DSD_P25_PATCH_RADIO_MAX);
    rc |= expect_eq_int("caps last radio", (int)st.p25_patch_wuid[0][DSD_P25_PATCH_RADIO_MAX - 1],
                        70000 + DSD_P25_PATCH_RADIO_MAX);

    uint16_t members[DSD_P25_PATCH_GROUP_MAX + 1];
    for (int k = 0; k < DSD_P25_PATCH_GROUP_MAX + 1; k++) {
        members[k] = 0xFFFF;
    }
    rc |= expect_eq_int("collect bounded by cap", p25_patch_collect_active_wgids(&st, 1400, members, 8), 8);
    rc |= expect_eq_int("collect leaves the rest", members[8], 0xFFFF);
    rc |=
        expect_eq_int("collect all 32", p25_patch_collect_active_wgids(&st, 1400, members, DSD_P25_PATCH_GROUP_MAX + 1),
                      DSD_P25_PATCH_GROUP_MAX);
    rc |= expect_eq_int("collect 32nd member", members[DSD_P25_PATCH_GROUP_MAX - 1], 2000 + DSD_P25_PATCH_GROUP_MAX);
    rc |= expect_eq_int("collect null out", p25_patch_collect_active_wgids(&st, 1400, NULL, 8), 0);
    return rc;
}

/* B4: eviction follows the refresh sequence, not second-resolution decode time: equal stamps and a wall clock that
   steps back never evict the newest entry. */
static int
test_seq_eviction(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);
    const uint64_t t0 = 1700000000ULL * 1000000000ULL;
    dsd_decode_clock_use_test(t0);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        p25_patch_update(&st, 1400 + i, 1, 1);
    }
    p25_patch_update(&st, 1400, 1, 1); // 1400 refreshed: 1401 is now the oldest
    dsd_decode_clock_test_set_ns(t0 - 100ULL * 1000000000ULL);
    p25_patch_update(&st, 1500, 1, 1);
    rc |= expect_true("seq evicts the least recently refreshed", find_idx(&st, 1401) < 0);
    rc |= expect_true("seq keeps the refreshed entry", find_idx(&st, 1400) >= 0);
    rc |= expect_true("seq stores the newcomer", find_idx(&st, 1500) >= 0);
    p25_patch_update(&st, 1501, 1, 1);
    rc |= expect_true("seq keeps the newest entry after a backward step", find_idx(&st, 1500) >= 0);
    rc |= expect_true("seq evicts the next oldest", find_idx(&st, 1402) < 0);
    rc |= expect_eq_int("seq table stays full", st.p25_patch_count, DSD_P25_PATCH_MAX);
    dsd_decode_clock_use_system();
    return rc;
}

/* A message that only names the SG keeps the row; members and KEY context need a membership message. Readers ignore
   stale members without mutating; the next change to the entry expires them at once. */
static int
test_two_stamps_and_expiry(void) {
    int rc = 0;
    static dsd_state st;
    char out[256];
    uint16_t members[DSD_P25_PATCH_GROUP_MAX];
    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_add_wgid(&st, 1400, 1401);
    p25_patch_set_kas(&st, 1400, 0, 0x84, 3);
    rc |= expect_eq_int("expiry clear before", p25_patch_tg_key_is_clear(&st, 1401), 1);

    listen_for(&st, 15.0);
    p25_patch_update(&st, 1400, 1, 1); // a grant naming the SG
    listen_for(&st, 10.0);
    rc |= expect_eq_int("sg-only keeps no members", p25_patch_collect_active_wgids(&st, 1400, members, 32), 0);
    rc |= expect_eq_int("stale KEY not clear (tg)", p25_patch_tg_key_is_clear(&st, 1401), 0);
    rc |= expect_eq_int("stale KEY not clear (sg)", p25_patch_sg_key_is_clear(&st, 1400), 0);
    (void)p25_patch_compose_details(&st, out, sizeof out);
    rc |= expect_true("row kept by the sg-only refresh", strstr(out, "SG1400[P]") != NULL);
    rc |= expect_true("stale members not shown", strstr(out, "WG:") == NULL && strstr(out, "A:84") == NULL);
    rc |= expect_eq_int("reader leaves stale members", st.p25_patch_wgid_count[0], 1);
    rc |= expect_eq_int("reader leaves stale KEY", st.p25_patch_key_valid[0], 1);

    p25_patch_update(&st, 1400, 1, 1); // SG-only refresh after members expired: no formatter run needed
    rc |= expect_eq_int("mutation expires members", st.p25_patch_wgid_count[0], 0);
    rc |= expect_eq_int("mutation expires KEY", st.p25_patch_key_valid[0], 0);
    rc |= expect_eq_int("mutation expires ALG", st.p25_patch_alg[0], 0);
    rc |= expect_eq_int("mutation keeps the row", st.p25_patch_active[0], 1);
    return rc;
}

/* B2: every accepted Add refreshes both stamps and the sequence and reactivates; the last-member delete takes the KEY
   context with it. */
static int
test_add_refresh_and_last_delete(void) {
    int rc = 0;
    static dsd_state st;
    uint16_t members[DSD_P25_PATCH_GROUP_MAX];
    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_add_wgid(&st, 1400, 1401);
    const uint32_t seq_before = st.p25_patch_seq[0];
    listen_for(&st, 15.0);
    p25_patch_add_wgid(&st, 1400, 1401); // duplicate Add
    rc |= expect_true("duplicate add takes a new sequence", st.p25_patch_seq[0] != seq_before);
    listen_for(&st, 10.0);
    rc |=
        expect_eq_int("duplicate add keeps members current", p25_patch_collect_active_wgids(&st, 1400, members, 32), 1);

    p25_patch_clear_sg(&st, 1400);
    rc |= expect_eq_int("cancel deactivates", st.p25_patch_active[0], 0);
    p25_patch_add_wgid(&st, 1400, 1402);
    rc |= expect_eq_int("add reactivates an inactive entry", st.p25_patch_active[0], 1);
    rc |= expect_eq_int("reactivated member", p25_patch_collect_active_wgids(&st, 1400, members, 32), 1);
    rc |= expect_eq_int("reactivated member id", members[0], 1402);

    p25_patch_set_kas(&st, 1400, 0, 0x84, 3);
    rc |= expect_eq_int("KEY clear before the last delete", p25_patch_sg_key_is_clear(&st, 1400), 1);
    p25_patch_remove_wgid(&st, 1400, 1402);
    rc |= expect_eq_int("last delete deactivates", st.p25_patch_active[0], 0);
    rc |= expect_eq_int("last delete expires KEY", st.p25_patch_key_valid[0], 0);
    rc |= expect_eq_int("last delete expires ALG/SSN", st.p25_patch_alg[0] | st.p25_patch_ssn[0], 0);
    rc |= expect_eq_int("last delete expires members", dsd_p25_patch_members_current(&st, 0), 0);
    p25_patch_update(&st, 1400, 1, 1); // a later grant brings the row back without the old KEY=0
    rc |= expect_eq_int("no KEY=0 after the last delete", p25_patch_sg_key_is_clear(&st, 1400), 0);
    return rc;
}

/* B3: the SG is never its own member; a Delete listing the SG ends the whole patch. */
static int
test_sg_as_member(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof st);
    p25_patch_add_wgid(&st, 1400, 1400);
    rc |= expect_eq_int("sg-as-member add creates nothing", st.p25_patch_count, 0);
    p25_patch_add_wgid(&st, 1400, 1401);
    p25_patch_add_wgid(&st, 1400, 1400);
    p25_patch_add_wgid(&st, 1400, 1402);
    rc |= expect_eq_int("sg-as-member add stores real members only", st.p25_patch_wgid_count[0], 2);
    p25_patch_set_kas(&st, 1400, 0, -1, -1);
    p25_patch_remove_wgid(&st, 1400, 1400);
    rc |= expect_eq_int("sg-as-member delete ends the patch", st.p25_patch_active[0], 0);
    rc |= expect_eq_int("sg-as-member delete clears members", st.p25_patch_wgid_count[0], 0);
    rc |= expect_eq_int("sg-as-member delete clears KEY", st.p25_patch_key_valid[0], 0);
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

    // TTL: after 21 s of listening in which SG142 is not named, it disappears from details
    // (op25 uses a 20 s PATCH_EXPIRY_TIME; here on the listening clock)
    listen_for(&st, 21.0);
    p25_patch_update(&st, 69, /*is_patch*/ 1, /*active*/ 1);
    p25_patch_add_wuid(&st, 77, 1001);
    p25_patch_add_wuid(&st, 77, 1002);
    p25_patch_add_wuid(&st, 77, 1003);
    (void)p25_patch_compose_details(&st, det, sizeof det);
    rc |= expect_true("details dropped SG142", strstr(det, "SG142[") == NULL);
    rc |= expect_true("details keep refreshed SG069", strstr(det, "SG069[P]") != NULL);

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
    rc |= test_caps();
    rc |= test_seq_eviction();
    rc |= test_two_stamps_and_expiry();
    rc |= test_add_refresh_and_last_delete();
    rc |= test_sg_as_member();

    (void)errno;
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(bugprone-misplaced-widening-cast)
