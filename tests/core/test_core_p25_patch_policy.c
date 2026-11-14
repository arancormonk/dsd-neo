// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 patch policy resolver (<dsd-neo/core/p25_patch_policy.h>): the supergroup and its current members as
 * candidates, the SG's final reasons (the `*` catch-all is not final), ADMIT and MEDIA eligibility and ranking, the
 * adjust hook, and the media consumers' keep-or-resolve.
 */

#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/p25_patch_policy.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"

#include "../test_support/radio_policy_test.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expected true\n", tag);
        return 1;
    }
    return 0;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
} fixture;

static int
fixture_open(fixture* f) {
    f->opts = calloc(1, sizeof(*f->opts));
    f->st = calloc(1, sizeof(*f->st));
    if (!f->opts || !f->st) {
        free(f->opts);
        free(f->st);
        return 1;
    }
    f->opts->trunk_tune_group_calls = 1;
    f->opts->trunk_tune_enc_calls = 1;
    return 0;
}

static void
fixture_close(fixture* f) {
    if (f->st) {
        dsd_state_ext_free_all(f->st);
    }
    free(f->st);
    free(f->opts);
}

static int
row(dsd_state* st, uint32_t id, const char* mode, int priority, int preempt, int audio) {
    dsd_tg_policy_entry e;
    if (dsd_tg_policy_make_exact_entry(id, mode, "ROW", DSD_TG_POLICY_SOURCE_IMPORTED, &e) != 0) {
        return 1;
    }
    e.priority = priority;
    e.preempt = preempt ? 1u : 0u;
    if (!audio) {
        e.audio = 0u;
        e.record = 0u;
        e.stream = 0u;
    }
    return dsd_tg_policy_append_exact(st, &e) != 0;
}

static int
catch_all(dsd_state* st, const char* mode) {
    dsd_tg_policy_entry e;
    DSD_MEMSET(&e, 0, sizeof(e));
    e.kind = DSD_TG_POLICY_ROW_PATTERN;
    DSD_SNPRINTF(e.pattern, sizeof(e.pattern), "%s", "*");
    DSD_SNPRINTF(e.mode, sizeof(e.mode), "%s", mode);
    DSD_SNPRINTF(e.name, sizeof(e.name), "%s", "EVERYONE-ELSE");
    e.source = DSD_TG_POLICY_SOURCE_IMPORTED;
    return dsd_tg_policy_add_pattern_entry(st, &e) != 0;
}

/* A live patch on @p sg with @p count members, its Add heard now on the listening clock. */
static void
seed_patch(dsd_state* st, uint16_t sg, const uint16_t* members, int count) {
    const int idx = st->p25_patch_count++;
    st->p25_patch_sgid[idx] = sg;
    st->p25_patch_is_patch[idx] = 1U;
    st->p25_patch_active[idx] = 1U;
    st->p25_patch_wgid_count[idx] = (uint8_t)count;
    for (int k = 0; k < count; k++) {
        st->p25_patch_wgid[idx][k] = members[k];
    }
    dsd_p25_patch_touch_members(st, idx);
}

static int
resolve(const fixture* f, uint32_t sg, uint32_t src, dsd_p25_patch_resolve_purpose purpose,
        dsd_p25_patch_resolution* out) {
    return dsd_p25_patch_resolve(f->opts, f->st, sg, src, 0, 0, purpose, NULL, NULL, out);
}

/* M1: a `*` catch-all with listening off is not final on the SG; an explicit SG row is. */
static int
test_catch_all_admits_listened_member(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    int rc = expect_true("m1 catch-all", catch_all(f.st, "B") == 0);
    rc |= expect_true("m1 member row", row(f.st, 1401, "A", 0, 0, 1) == 0);
    const uint16_t members[] = {1402, 1401};
    seed_patch(f.st, 1400, members, 2);
    dsd_p25_patch_resolution res;
    for (int purpose = 0; purpose < 2; purpose++) {
        rc |= expect_true("m1 resolves", resolve(&f, 1400, 2400, (dsd_p25_patch_resolve_purpose)purpose, &res) == 0);
        rc |= expect_true("m1 catch-all admits the listened member",
                          res.policy_target == 1401U && res.eligible && res.decision.target_id == 1401U
                              && res.decision.tune_allowed && res.decision.audio_allowed && res.sg_final_reasons == 0U);
    }

    // Preservation: an explicit row on the SG stays final, for ADMIT and MEDIA.
    rc |= expect_true("m1 explicit SG row", row(f.st, 1400, "B", 0, 0, 1) == 0);
    for (int purpose = 0; purpose < 2; purpose++) {
        rc |= expect_true("m1 explicit resolves",
                          resolve(&f, 1400, 2400, (dsd_p25_patch_resolve_purpose)purpose, &res) == 0);
        rc |= expect_true("m1 explicit SG B stays final",
                          res.policy_target == 1400U && !res.eligible && !res.decision.tune_allowed
                              && !res.decision.audio_allowed
                              && res.sg_final_reasons == (uint32_t)DSD_TG_POLICY_BLOCK_MODE);
    }
    fixture_close(&f);
    return rc;
}

/* ADMIT ranking: Hold, then media, then priority, then preempt; the SG wins a full tie. */
static int
test_admit_ranking(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    dsd_p25_patch_resolution res;
    // M2: an SG that may tune but plays no audio loses to a listened member of equal priority.
    int rc = expect_true("m2 SG audio off", row(f.st, 1500, "A", 0, 0, 0) == 0);
    rc |= expect_true("m2 member", row(f.st, 1501, "A", 0, 0, 1) == 0);
    const uint16_t m2[] = {1501};
    seed_patch(f.st, 1500, m2, 1);
    rc |= expect_true("m2 resolves", resolve(&f, 1500, 2500, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("m2 listened member beats an audio-off SG", res.policy_target == 1501U && res.eligible);

    // A full tie keeps the SG.
    rc |= expect_true("tie SG", row(f.st, 1510, "A", 5, 0, 1) == 0);
    rc |= expect_true("tie member", row(f.st, 1511, "A", 5, 0, 1) == 0);
    const uint16_t tie[] = {1511};
    seed_patch(f.st, 1510, tie, 1);
    rc |= expect_true("tie resolves", resolve(&f, 1510, 2510, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("the SG wins a full tie", res.policy_target == 1510U && res.eligible);

    // Priority, then preempt.
    rc |= expect_true("prio low", row(f.st, 1521, "A", 10, 0, 1) == 0);
    rc |= expect_true("prio high", row(f.st, 1522, "A", 60, 0, 1) == 0);
    rc |= expect_true("prio high preempt", row(f.st, 1523, "A", 60, 1, 1) == 0);
    const uint16_t prio[] = {1521, 1522, 1523};
    seed_patch(f.st, 1520, prio, 3);
    rc |= expect_true("prio resolves", resolve(&f, 1520, 2520, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("higher priority, then preempt, wins",
                      res.policy_target == 1523U && res.decision.priority == 60 && res.decision.preempt_requested);

    // Hold first: a held member wins over a higher-priority one.
    f.st->tg_hold = 1521;
    rc |= expect_true("hold resolves", resolve(&f, 1520, 2520, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("a held member wins", res.policy_target == 1521U && res.decision.tg_hold_match);
    f.st->tg_hold = 0;

    // Allow-list: an unlisted SG loses to a listed member, for ADMIT and MEDIA.
    f.opts->trunk_use_allow_list = 1;
    rc |= expect_true("allow member", row(f.st, 1531, "A", 0, 0, 1) == 0);
    const uint16_t allow[] = {1532, 1531};
    seed_patch(f.st, 1530, allow, 2);
    for (int purpose = 0; purpose < 2; purpose++) {
        rc |= expect_true("allow resolves", resolve(&f, 1530, 2530, (dsd_p25_patch_resolve_purpose)purpose, &res) == 0);
        rc |= expect_true("unlisted SG vs listed member: member", res.policy_target == 1531U && res.eligible);
    }
    // Nothing eligible: the SG decision stands.
    const uint16_t none[] = {1542};
    seed_patch(f.st, 1540, none, 1);
    rc |= expect_true("none resolves", resolve(&f, 1540, 2540, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("nothing eligible keeps the SG decision",
                      res.policy_target == 1540U && !res.eligible && !res.decision.tune_allowed
                          && (res.decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST));
    fixture_close(&f);
    return rc;
}

/* A blocked source applies to the winner; it never eliminates every member (the radio-only recheck keeps the call). */
static int
test_blocked_source_keeps_member(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->trunk_use_allow_list = 1;
    int rc = expect_true("src member", row(f.st, 1601, "A", 0, 0, 1) == 0);
    rc |= expect_true("src radio blocked", dsd_test_install_radio_policy(f.st, 2600, "B", 0, 0, 0) == 0);
    const uint16_t members[] = {1601};
    seed_patch(f.st, 1600, members, 1);
    dsd_p25_patch_resolution res;
    rc |= expect_true("src resolves", resolve(&f, 1600, 2600, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("blocked source still picks the member",
                      res.policy_target == 1601U && res.eligible && !res.decision.tune_allowed
                          && dsd_tg_policy_decision_only_radio_blocks(&res.decision));
    fixture_close(&f);
    return rc;
}

static void
release_lockout_on_sg(uint32_t target, dsd_tg_policy_decision* decision, const void* ctx) {
    const uint32_t* sg = (const uint32_t*)ctx;
    const uint32_t media_only = (uint32_t)DSD_TG_POLICY_BLOCK_AUDIO | (uint32_t)DSD_TG_POLICY_BLOCK_RECORD
                                | (uint32_t)DSD_TG_POLICY_BLOCK_STREAM;
    if (target == *sg) {
        decision->block_reasons &= ~(uint32_t)DSD_TG_POLICY_BLOCK_ENC_LOCKOUT;
        if ((decision->block_reasons & ~media_only) == 0U) {
            decision->tg_tune_allowed = 1;
            decision->tune_allowed = 1;
        }
    }
}

/* The encryption lockout on the SG: final for ADMIT unless the adjust hook releases it; tune-only for MEDIA. */
static int
test_enc_lockout_on_sg(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->trunk_tune_enc_calls = 0;
    f.opts->trunk_use_allow_list = 1;
    int rc = expect_true("enc member", row(f.st, 1701, "A", 0, 0, 1) == 0);
    (void)dsd_enc_lockout_note(f.st, 1700, 1, 0x84, 4);
    const uint16_t members[] = {1701};
    seed_patch(f.st, 1700, members, 1);
    dsd_p25_patch_resolution res;
    rc |= expect_true("enc admit resolves", resolve(&f, 1700, 2700, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("enc lockout ends ADMIT on the SG",
                      res.policy_target == 1700U && !res.eligible && !res.decision.tune_allowed
                          && res.sg_final_reasons == (uint32_t)DSD_TG_POLICY_BLOCK_ENC_LOCKOUT);
    const uint32_t sg = 1700U;
    rc |= expect_true("enc adjust resolves",
                      dsd_p25_patch_resolve(f.opts, f.st, 1700, 2700, 0, 0, DSD_P25_PATCH_RESOLVE_ADMIT,
                                            release_lockout_on_sg, &sg, &res)
                          == 0);
    rc |= expect_true("a released lockout lets the member admit", res.policy_target == 1701U && res.eligible
                                                                      && res.decision.tune_allowed
                                                                      && res.sg_final_reasons == 0U);

    rc |= expect_true("enc media resolves", resolve(&f, 1700, 2700, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("enc lockout is tune-only for MEDIA",
                      res.policy_target == 1701U && res.eligible && res.decision.audio_allowed
                          && res.decision.record_allowed && !res.decision.tune_allowed && !res.decision.tg_tune_allowed
                          && (res.decision.block_reasons & DSD_TG_POLICY_BLOCK_ENC_LOCKOUT));
    rc |= expect_true("enc media target", dsd_p25_patch_media_target(f.opts, f.st, 1700, 1700, 2700) == 1701U);

    // Other final reasons end MEDIA with media denied.
    rc |= expect_true("skip SG", dsd_tg_policy_call_skip_arm(f.st, 1700, 2700, 0, dsd_decode_now_mono_s()) == 0);
    rc |= expect_true("skip media resolves", resolve(&f, 1700, 2700, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("a call skip ends MEDIA on the SG",
                      res.policy_target == 1700U && !res.eligible && !res.decision.audio_allowed
                          && (res.sg_final_reasons & DSD_TG_POLICY_BLOCK_CALL_SKIP));
    rc |= expect_true("skip media target", dsd_p25_patch_media_target(f.opts, f.st, 1700, 1701, 2700) == 1700U);
    fixture_close(&f);
    return rc;
}

/* MEDIA needs no tuning: routed audio with group tuning off stays audible on a listened member. */
static int
test_media_without_group_tuning(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->trunk_tune_group_calls = 0;
    f.opts->trunk_use_allow_list = 1;
    int rc = expect_true("route member", row(f.st, 1801, "A", 0, 0, 1) == 0);
    const uint16_t members[] = {1801};
    seed_patch(f.st, 1800, members, 1);
    dsd_p25_patch_resolution res;
    rc |= expect_true("route media resolves", resolve(&f, 1800, 2800, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("group tuning off still plays the member",
                      res.policy_target == 1801U && res.eligible && res.decision.audio_allowed);
    rc |= expect_true("route admit resolves", resolve(&f, 1800, 2800, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("group tuning off admits nothing", res.policy_target == 1800U && !res.eligible);
    // MEDIA ranks a Hold, then priority.
    f.opts->trunk_tune_group_calls = 1;
    rc |= expect_true("media prio", row(f.st, 1802, "A", 40, 0, 1) == 0);
    const uint16_t two[] = {1801, 1802};
    f.st->p25_patch_wgid_count[0] = 2U;
    f.st->p25_patch_wgid[0][1] = two[1];
    rc |= expect_true("media prio resolves", resolve(&f, 1800, 2800, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("MEDIA ranks priority", res.policy_target == 1802U);
    f.st->tg_hold = 1801;
    rc |= expect_true("media hold resolves", resolve(&f, 1800, 2800, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("MEDIA ranks a Hold first", res.policy_target == 1801U);
    f.st->tg_hold = 0;
    fixture_close(&f);
    return rc;
}

/* Only members whose membership is current are candidates, judged at every read. */
static int
test_expired_members_are_not_candidates(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->trunk_use_allow_list = 1;
    int rc = expect_true("ttl member", row(f.st, 1901, "A", 0, 0, 1) == 0);
    const uint16_t members[] = {1901};
    seed_patch(f.st, 1900, members, 1);
    dsd_p25_patch_resolution res;
    rc |= expect_true("ttl current",
                      resolve(&f, 1900, 2900, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0 && res.policy_target == 1901U);
    f.st->p25_patch_listen_s += (double)DSD_P25_PATCH_TTL_SECONDS + 1.0;
    rc |= expect_true("ttl expired", resolve(&f, 1900, 2900, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0
                                         && res.policy_target == 1900U && !res.eligible);
    rc |= expect_true("ttl expired media target", dsd_p25_patch_media_target(f.opts, f.st, 1900, 1901, 2900) == 1900U);
    rc |= expect_true("ttl not a current member", dsd_p25_patch_is_current_member(f.st, 1900, 1901) == 0);
    fixture_close(&f);
    return rc;
}

/* Keep-or-resolve: the stored member is kept while current and media-eligible, else the resolver answers. */
static int
test_media_keep_or_resolve(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    f.opts->trunk_use_allow_list = 1;
    int rc = expect_true("keep low", row(f.st, 2001, "A", 10, 0, 1) == 0);
    rc |= expect_true("keep high", row(f.st, 2002, "A", 90, 0, 1) == 0);
    const uint16_t members[] = {2001, 2002};
    seed_patch(f.st, 2000, members, 2);
    rc |= expect_true("current member", dsd_p25_patch_is_current_member(f.st, 2000, 2001) == 1);
    rc |= expect_true("SG is not its own member", dsd_p25_patch_is_current_member(f.st, 2000, 2000) == 0);
    rc |= expect_true("stored eligible member is kept",
                      dsd_p25_patch_media_target(f.opts, f.st, 2000, 2001, 3000) == 2001U);
    rc |= expect_true("stored SG resolves", dsd_p25_patch_media_target(f.opts, f.st, 2000, 2000, 3000) == 2002U);
    // The stored member's row turns listening off: another listened member carries the patch.
    rc |= expect_true("keep row off", dsd_tg_policy_set_mode(f.st, 2001, 2001, "B") == 0);
    rc |= expect_true("stored member muted re-selects",
                      dsd_p25_patch_media_target(f.opts, f.st, 2000, 2001, 3000) == 2002U);
    // The stored member, listened to again, leaves the patch.
    rc |= expect_true("keep row on", dsd_tg_policy_set_mode(f.st, 2001, 2001, "A") == 0);
    rc |= expect_true("relistened member kept", dsd_p25_patch_media_target(f.opts, f.st, 2000, 2001, 3000) == 2001U);
    f.st->p25_patch_wgid[0][0] = 2002U;
    f.st->p25_patch_wgid_count[0] = 1U;
    rc |= expect_true("deleted member not current", dsd_p25_patch_is_current_member(f.st, 2000, 2001) == 0);
    rc |= expect_true("deleted member re-selects", dsd_p25_patch_media_target(f.opts, f.st, 2000, 2001, 3000) == 2002U);
    rc |= expect_true("no patch keeps the target", dsd_p25_patch_media_target(f.opts, f.st, 2100, 2101, 3000) == 2100U);
    fixture_close(&f);
    return rc;
}

/* Keep-or-resolve keeps a stored supergroup while its own decision may play, without judging members: media only needs
   some eligible candidate. A higher-priority listened member would win a resolution. */
static int
test_media_keeps_a_listened_supergroup(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    int rc = expect_true("sg keep high member", row(f.st, 2201, "A", 70, 0, 1) == 0);
    const uint16_t members[] = {2201};
    seed_patch(f.st, 2200, members, 1);
    dsd_p25_patch_resolution res;
    rc |= expect_true("sg keep resolves", resolve(&f, 2200, 3200, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("a resolution picks the higher-priority member", res.policy_target == 2201U);
    rc |= expect_true("sg keep media", dsd_p25_patch_media_resolve(f.opts, f.st, 2200, 2200, 3200, &res) == 1);
    rc |= expect_true("the stored supergroup is kept", res.policy_target == 2200U && res.eligible
                                                           && res.decision.target_id == 2200U
                                                           && dsd_p25_patch_media_eligible(&res.decision));
    rc |= expect_true("no patch: nothing evaluated",
                      dsd_p25_patch_media_resolve(f.opts, f.st, 2400, 2400, 3200, &res) == 0
                          && res.policy_target == 2400U && !res.eligible);
    fixture_close(&f);
    return rc;
}

/* MEDIA: a Hold-matched member plays through an explicit mode B/DE on the SG, as a Hold plays a plain call's mode block;
   a call skip or session avoid on the SG still ends media resolution; ADMIT keeps the SG's mode block final. */
static int
test_media_hold_member_through_sg_mode(void) {
    fixture f;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    int rc = expect_true("hold SG B", row(f.st, 2300, "B", 0, 0, 1) == 0);
    const uint16_t members[] = {2301, 2302};
    seed_patch(f.st, 2300, members, 2);
    dsd_p25_patch_resolution res;
    rc |= expect_true("no hold: nothing plays", resolve(&f, 2300, 3300, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0
                                                    && res.policy_target == 2300U && !res.eligible);
    f.st->tg_hold = 2302;
    rc |= expect_true("hold media resolves", resolve(&f, 2300, 3300, DSD_P25_PATCH_RESOLVE_MEDIA, &res) == 0);
    rc |= expect_true("the held member plays through the SG mode block",
                      res.policy_target == 2302U && res.eligible && res.decision.tg_hold_match
                          && (res.decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) && !res.decision.tune_allowed
                          && dsd_p25_patch_media_eligible(&res.decision));
    rc |= expect_true("the held member is kept", dsd_p25_patch_media_target(f.opts, f.st, 2300, 2302, 3300) == 2302U);
    rc |= expect_true("hold admit resolves", resolve(&f, 2300, 3300, DSD_P25_PATCH_RESOLVE_ADMIT, &res) == 0);
    rc |= expect_true("ADMIT keeps the SG mode block final", res.policy_target == 2300U && !res.eligible);
    rc |= expect_true("hold skip", dsd_tg_policy_call_skip_arm(f.st, 2300, 3300, 0, dsd_decode_now_mono_s()) == 0);
    rc |= expect_true("a call skip ends media despite the Hold",
                      dsd_p25_patch_media_target(f.opts, f.st, 2300, 2302, 3300) == 2300U);
    dsd_tg_policy_call_skip_clear(f.st);
    rc |= expect_true("hold avoid", dsd_tg_policy_session_avoid_add(f.st, 2300) == 0);
    rc |= expect_true("a session avoid ends media despite the Hold",
                      dsd_p25_patch_media_target(f.opts, f.st, 2300, 2302, 3300) == 2300U);
    fixture_close(&f);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_catch_all_admits_listened_member();
    rc |= test_admit_ranking();
    rc |= test_blocked_source_keeps_member();
    rc |= test_enc_lockout_on_sg();
    rc |= test_media_without_group_tuning();
    rc |= test_expired_members_are_not_candidates();
    rc |= test_media_keep_or_resolve();
    rc |= test_media_keeps_a_listened_supergroup();
    rc |= test_media_hold_member_through_sg_mode();
    if (rc == 0) {
        printf("CORE_P25_PATCH_POLICY: OK\n");
    }
    return rc;
}
