// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 patch table state helpers (<dsd-neo/core/p25_patch.h>): the listening clock and its 0.25 s gap
 * rule, the seen/members stamps, member expiry, wrap-safe recency and the whole-table clear.
 */

#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

static int g_failures = 0;

static void
check(int cond, const char* what) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static int
near(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static dsd_state*
fresh_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!st) {
        DSD_FPRINTF(stderr, "out of memory\n");
        exit(1);
    }
    return st;
}

static void
test_clock_gap_rule(void) {
    dsd_state* st = fresh_state();

    // The first reading after a fresh state (previous reading 0 = broken) only stores itself, even a small one.
    dsd_p25_patch_listen_note(st, 0.1);
    check(near(st->p25_patch_listen_s, 0.0), "first note after a fresh state adds nothing");
    check(near(st->p25_patch_listen_last_m, 0.1), "first note stores its reading");

    dsd_p25_patch_listen_note(st, 0.2);
    check(near(st->p25_patch_listen_s, 0.1), "a 0.1 s gap counts");

    // The 0.25 s edge counts; anything past it does not.
    dsd_p25_patch_listen_note(st, 0.45);
    check(near(st->p25_patch_listen_s, 0.35), "a gap of exactly 0.25 s counts");
    dsd_p25_patch_listen_note(st, 0.7001);
    check(near(st->p25_patch_listen_s, 0.35), "a gap past 0.25 s does not count");
    check(near(st->p25_patch_listen_last_m, 0.7001), "a long gap still stores the reading");

    // Equal readings add nothing; a reading that goes backwards adds nothing and becomes the new previous one.
    dsd_p25_patch_listen_note(st, 0.7001);
    check(near(st->p25_patch_listen_s, 0.35), "equal readings add nothing");
    dsd_p25_patch_listen_note(st, 0.6);
    check(near(st->p25_patch_listen_s, 0.35), "a backward reading adds nothing");
    dsd_p25_patch_listen_note(st, 0.7);
    check(near(st->p25_patch_listen_s, 0.45), "the clock runs on from a backward reading");

    // A break makes the next reading only store itself, however close it is.
    dsd_p25_patch_listen_break(st);
    check(near(st->p25_patch_listen_last_m, 0.0), "break clears the previous reading");
    dsd_p25_patch_listen_note(st, 0.75);
    check(near(st->p25_patch_listen_s, 0.45), "the note after a break adds nothing");
    dsd_p25_patch_listen_note(st, 0.8);
    check(near(st->p25_patch_listen_s, 0.5), "the clock runs again after a break");

    dsd_p25_patch_listen_note(NULL, 1.0);
    dsd_p25_patch_listen_break(NULL);
    free(st);
}

static void
test_clock_fractional_accumulation(void) {
    dsd_state* st = fresh_state();
    // 30 ms PDUs for 25 s of continuous reception, from a large monotonic origin.
    const double origin = 86400.0;
    dsd_p25_patch_listen_note(st, origin);
    for (int i = 1; i <= 25000 / 30; i++) {
        dsd_p25_patch_listen_note(st, origin + (double)i * 0.030);
    }
    const double want = (double)(25000 / 30) * 0.030;
    check(fabs(st->p25_patch_listen_s - want) < 1e-6, "fractional gaps accumulate without drift");
    free(st);
}

static void
test_two_stamps(void) {
    dsd_state* st = fresh_state();
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = 1400;
    st->p25_patch_active[0] = 1;
    st->p25_patch_listen_s = 100.0;

    dsd_p25_patch_touch_members(st, 0);
    check(near(st->p25_patch_seen_listen_s[0], 100.0), "touch_members stamps seen");
    check(near(st->p25_patch_members_listen_s[0], 100.0), "touch_members stamps members");
    const uint32_t first_seq = st->p25_patch_seq[0];
    check(first_seq == st->p25_patch_seq_counter && first_seq != 0U, "touch_members takes a new sequence");

    st->p25_patch_listen_s = 110.0;
    dsd_p25_patch_touch_seen(st, 0);
    check(near(st->p25_patch_seen_listen_s[0], 110.0), "touch_seen stamps seen");
    check(near(st->p25_patch_members_listen_s[0], 100.0), "touch_seen leaves the members stamp");
    check(st->p25_patch_seq[0] == first_seq + 1U, "touch_seen takes a new sequence");

    // Members stay current through 20 s of listening and no further; the row follows its own stamp.
    st->p25_patch_listen_s = 120.0;
    check(dsd_p25_patch_members_current(st, 0) == 1, "members current at exactly the TTL");
    st->p25_patch_listen_s = 120.0 + 5e-7; // a clock summed from many gaps lands a hair past the TTL
    check(dsd_p25_patch_members_current(st, 0) == 1, "members current within the TTL's rounding slack");
    st->p25_patch_listen_s = 120.001;
    check(dsd_p25_patch_members_current(st, 0) == 0, "members stale a millisecond past the TTL");
    check(dsd_p25_patch_entry_seen_current(st, 0) == 1, "seen current inside the TTL");
    st->p25_patch_listen_s = 120.5;
    check(dsd_p25_patch_members_current(st, 0) == 0, "members stale past the TTL");
    check(dsd_p25_patch_entry_seen_current(st, 0) == 1, "seen still current on its later stamp");
    st->p25_patch_listen_s = 130.5;
    check(dsd_p25_patch_entry_seen_current(st, 0) == 0, "seen stale past its TTL");

    check(dsd_p25_patch_entry_seen_current(NULL, 0) == 0, "seen null state");
    check(dsd_p25_patch_entry_seen_current(st, -1) == 0, "seen negative index");
    check(dsd_p25_patch_entry_seen_current(st, DSD_P25_PATCH_MAX) == 0, "seen index past cap");
    check(dsd_p25_patch_members_current(st, DSD_P25_PATCH_MAX) == 0, "members index past cap");
    free(st);
}

static void
test_expire_members(void) {
    dsd_state* st = fresh_state();
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = 1400;
    st->p25_patch_active[0] = 1;
    st->p25_patch_wgid_count[0] = 2;
    st->p25_patch_wgid[0][0] = 1401;
    st->p25_patch_wgid[0][1] = 1402;
    st->p25_patch_wuid_count[0] = 1;
    st->p25_patch_wuid[0][0] = 70001;
    st->p25_patch_key[0] = 0;
    st->p25_patch_key_valid[0] = 1;
    st->p25_patch_alg[0] = 0x84;
    st->p25_patch_ssn[0] = 3;
    st->p25_patch_listen_s = 5.0;
    dsd_p25_patch_touch_members(st, 0);

    check(dsd_p25_patch_has_current_wgid(st, 0, 1402) == 1, "current member found");
    check(dsd_p25_patch_has_current_wgid(st, 0, 1403) == 0, "unknown member not found");
    check(dsd_p25_patch_expire_stale_members(st, 0) == 0, "current members are not expired");
    check(st->p25_patch_wgid_count[0] == 2, "current members kept");

    // A reader never mutates: stale members stay in the table but are not reported.
    st->p25_patch_listen_s = 30.0;
    check(dsd_p25_patch_has_current_wgid(st, 0, 1402) == 0, "stale member not reported");
    check(st->p25_patch_wgid_count[0] == 2, "reader leaves stale members in place");

    check(dsd_p25_patch_expire_stale_members(st, 0) == 1, "stale members expire");
    check(st->p25_patch_wgid_count[0] == 0 && st->p25_patch_wgid[0][0] == 0, "expire clears groups");
    check(st->p25_patch_wuid_count[0] == 0 && st->p25_patch_wuid[0][0] == 0, "expire clears radios");
    check(st->p25_patch_key_valid[0] == 0 && st->p25_patch_alg[0] == 0 && st->p25_patch_ssn[0] == 0,
          "expire clears KEY/ALG/SSN");
    check(st->p25_patch_sgid[0] == 1400 && st->p25_patch_active[0] == 1, "expire keeps the entry");

    // An unconditional expire also makes the members stamp never current, even at once.
    st->p25_patch_wgid_count[0] = 1;
    st->p25_patch_wgid[0][0] = 1401;
    dsd_p25_patch_touch_members(st, 0);
    dsd_p25_patch_expire_members(st, 0);
    check(dsd_p25_patch_members_current(st, 0) == 0, "expired members are never current");
    check(dsd_p25_patch_entry_seen_current(st, 0) == 1, "expire keeps the seen stamp");

    dsd_p25_patch_expire_members(NULL, 0);
    dsd_p25_patch_expire_members(st, DSD_P25_PATCH_MAX);
    check(dsd_p25_patch_expire_stale_members(NULL, 0) == 0, "expire stale null state");
    free(st);
}

static void
test_seq_age_wraps(void) {
    dsd_state* st = fresh_state();
    st->p25_patch_seq_counter = UINT32_MAX - 1U;
    dsd_p25_patch_touch_seen(st, 0); // UINT32_MAX
    dsd_p25_patch_touch_seen(st, 1); // 0 after the wrap
    dsd_p25_patch_touch_seen(st, 2); // 1
    check(st->p25_patch_seq[1] == 0U, "the counter wraps");
    check(dsd_p25_patch_seq_age(st, 2) == 0U, "the newest entry has age 0");
    check(dsd_p25_patch_seq_age(st, 1) == 1U, "the age crosses the wrap");
    check(dsd_p25_patch_seq_age(st, 0) == 2U, "the oldest entry is oldest across the wrap");
    check(dsd_p25_patch_seq_age(NULL, 0) == UINT32_MAX, "age null state");
    check(dsd_p25_patch_seq_age(st, DSD_P25_PATCH_MAX) == UINT32_MAX, "age index past cap");
    free(st);
}

static void
test_clear_all(void) {
    dsd_state* st = fresh_state();
    st->p25_patch_count = DSD_P25_PATCH_MAX;
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        st->p25_patch_sgid[i] = (uint16_t)(1400 + i);
        st->p25_patch_active[i] = 1;
        st->p25_patch_wgid_count[i] = DSD_P25_PATCH_GROUP_MAX;
        st->p25_patch_wgid[i][DSD_P25_PATCH_GROUP_MAX - 1] = 1;
        st->p25_patch_wuid_count[i] = DSD_P25_PATCH_RADIO_MAX;
        st->p25_patch_wuid[i][DSD_P25_PATCH_RADIO_MAX - 1] = 1;
        st->p25_patch_key_valid[i] = 1;
        st->p25_patch_last_update[i] = 1000;
    }
    st->p25_patch_listen_s = 50.0;
    st->p25_patch_listen_last_m = 7.0;
    st->p25_patch_seq_counter = 99U;
    st->p25_patch_owner_known = 1;
    st->p25_patch_owner_wacn = 0xABCDEU;
    st->p25_patch_owner_sysid = 0x123U;

    dsd_p25_patch_clear_all(st);
    int clean = st->p25_patch_count == 0 && st->p25_patch_seq_counter == 0U && st->p25_patch_owner_known == 0
                && st->p25_patch_owner_wacn == 0U && st->p25_patch_owner_sysid == 0U
                && near(st->p25_patch_listen_s, 0.0) && near(st->p25_patch_listen_last_m, 0.0);
    for (int i = 0; i < DSD_P25_PATCH_MAX; i++) {
        clean = clean && st->p25_patch_sgid[i] == 0 && st->p25_patch_active[i] == 0 && st->p25_patch_wgid_count[i] == 0
                && st->p25_patch_wgid[i][DSD_P25_PATCH_GROUP_MAX - 1] == 0 && st->p25_patch_wuid_count[i] == 0
                && st->p25_patch_wuid[i][DSD_P25_PATCH_RADIO_MAX - 1] == 0 && st->p25_patch_key_valid[i] == 0
                && st->p25_patch_last_update[i] == 0 && st->p25_patch_seq[i] == 0U;
    }
    check(clean, "clear_all empties the table, owner, counter and clock");
    dsd_p25_patch_clear_all(NULL);
    free(st);
}

/* One live entry (SG 1400, WG 1401) on a clock that has run 30 s. */
static void
seed_one_patch(dsd_state* st) {
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = 1400;
    st->p25_patch_active[0] = 1;
    st->p25_patch_wgid_count[0] = 1;
    st->p25_patch_wgid[0][0] = 1401;
    st->p25_patch_listen_s = 30.0;
    st->p25_patch_seen_listen_s[0] = 29.0;
    st->p25_patch_members_listen_s[0] = 29.0;
}

static void
test_note_system(void) {
    dsd_state* st = fresh_state();
    seed_one_patch(st);

    // An unknown owner adopts the first identity and keeps what the table holds.
    check(dsd_p25_patch_note_system(st, 0xA1001U, 0x101U) == 0, "first identity clears nothing");
    check(st->p25_patch_owner_known == 1 && st->p25_patch_owner_wacn == 0xA1001U && st->p25_patch_owner_sysid == 0x101U,
          "first identity becomes the owner");
    check(st->p25_patch_count == 1 && st->p25_patch_wgid_count[0] == 1, "first identity keeps the table");

    // The same identity again keeps everything.
    check(dsd_p25_patch_note_system(st, 0xA1001U, 0x101U) == 0, "same identity clears nothing");
    check(st->p25_patch_count == 1 && near(st->p25_patch_listen_s, 30.0), "same identity keeps the table and clock");

    // A zero identity names no system.
    check(dsd_p25_patch_note_system(st, 0U, 0U) == 0, "zero identity clears nothing");
    check(st->p25_patch_owner_wacn == 0xA1001U && st->p25_patch_count == 1, "zero identity changes nothing");

    // Another system (WACN or SYSID differs) empties the table and becomes the owner.
    check(dsd_p25_patch_note_system(st, 0xA1001U, 0x102U) == 1, "another sysid clears");
    check(st->p25_patch_count == 0 && st->p25_patch_active[0] == 0 && st->p25_patch_wgid_count[0] == 0,
          "another system empties the table");
    check(near(st->p25_patch_listen_s, 0.0), "another system starts its own clock");
    check(st->p25_patch_owner_known == 1 && st->p25_patch_owner_wacn == 0xA1001U && st->p25_patch_owner_sysid == 0x102U,
          "another system becomes the owner");
    seed_one_patch(st);
    check(dsd_p25_patch_note_system(st, 0xB2002U, 0x102U) == 1 && st->p25_patch_count == 0, "another wacn clears");

    check(dsd_p25_patch_note_system(NULL, 1U, 1U) == 0, "null state");
    free(st);
}

static void
test_snapshot_round_trip(void) {
    dsd_state* st = fresh_state();
    seed_one_patch(st);
    st->p25_patch_wuid_count[0] = 1;
    st->p25_patch_wuid[0][0] = 7001;
    st->p25_patch_key[0] = 0x0004;
    st->p25_patch_alg[0] = 0x84;
    st->p25_patch_ssn[0] = 3;
    st->p25_patch_key_valid[0] = 1;
    st->p25_patch_is_patch[0] = 1;
    st->p25_patch_last_update[0] = 1234;
    st->p25_patch_seq[0] = 5U;
    st->p25_patch_seq_counter = 6U;
    st->p25_patch_owner_known = 1;
    st->p25_patch_owner_wacn = 0xA1001U;
    st->p25_patch_owner_sysid = 0x101U;
    st->p25_patch_listen_last_m = 77.0;

    dsd_p25_patch_snapshot* snap = (dsd_p25_patch_snapshot*)calloc(1, sizeof(*snap));
    check(snap != NULL, "snapshot allocation");
    if (!snap) {
        free(st);
        return;
    }
    dsd_p25_patch_snapshot_save(st, snap);

    // Another system's table replaces it in the state, then the snapshot comes back whole.
    dsd_p25_patch_clear_all(st);
    st->p25_patch_count = 1;
    st->p25_patch_sgid[0] = 1400;
    st->p25_patch_active[0] = 1;
    st->p25_patch_owner_known = 1;
    st->p25_patch_owner_wacn = 0xB2002U;
    st->p25_patch_listen_s = 3.0;
    st->p25_patch_listen_last_m = 88.0;
    dsd_p25_patch_snapshot_restore(st, snap);

    check(st->p25_patch_count == 1 && st->p25_patch_sgid[0] == 1400 && st->p25_patch_active[0] == 1,
          "restore brings the entry back");
    check(st->p25_patch_wgid_count[0] == 1 && st->p25_patch_wgid[0][0] == 1401 && st->p25_patch_wuid_count[0] == 1
              && st->p25_patch_wuid[0][0] == 7001,
          "restore brings the members back");
    check(st->p25_patch_key[0] == 0x0004 && st->p25_patch_alg[0] == 0x84 && st->p25_patch_ssn[0] == 3
              && st->p25_patch_key_valid[0] == 1 && st->p25_patch_is_patch[0] == 1,
          "restore brings the key context back");
    check(st->p25_patch_last_update[0] == 1234 && st->p25_patch_seq[0] == 5U && st->p25_patch_seq_counter == 6U,
          "restore brings the recency back");
    check(near(st->p25_patch_listen_s, 30.0) && near(st->p25_patch_seen_listen_s[0], 29.0)
              && near(st->p25_patch_members_listen_s[0], 29.0),
          "restore brings the clock and stamps back");
    check(st->p25_patch_owner_known == 1 && st->p25_patch_owner_wacn == 0xA1001U && st->p25_patch_owner_sysid == 0x101U,
          "restore brings the owner back");
    // The previous reading belonged to the carrier the receiver was on, so the restore breaks continuity.
    check(near(st->p25_patch_listen_last_m, 0.0), "restore breaks continuity");

    dsd_p25_patch_snapshot_save(NULL, snap);
    dsd_p25_patch_snapshot_restore(NULL, snap);
    dsd_p25_patch_snapshot_save(st, NULL);
    dsd_p25_patch_snapshot_restore(st, NULL);
    check(st->p25_patch_count == 1, "null arguments change nothing");
    free(snap);
    free(st);
}

int
main(void) {
    test_clock_gap_rule();
    test_clock_fractional_accumulation();
    test_two_stamps();
    test_expire_members();
    test_seq_age_wraps();
    test_clear_all();
    test_note_system();
    test_snapshot_round_trip();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
