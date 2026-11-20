// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The -Y voice-only scan's voice check and the talkgroup Hold check judge a P25 group call on a live patch's
 * supergroup (SG) through the patch resolver, on the real talkgroup policy: a member deleted from the patch, or a final
 * block on the SG, counts, and so does a listened member still carrying the patch. A call on no live patch keeps its
 * stored policy target (preservation). */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static int
listed_priority(dsd_state* st, uint32_t id, int priority) {
    dsd_tg_policy_entry e;
    if (dsd_tg_policy_make_exact_entry(id, "A", "LISTED", DSD_TG_POLICY_SOURCE_IMPORTED, &e) != 0) {
        return 1;
    }
    e.priority = priority;
    return dsd_tg_policy_append_exact(st, &e) != 0;
}

static int
listed(dsd_state* st, uint32_t id) {
    return listed_priority(st, id, 0);
}

#if defined(DSD_TEST_WRAP_POLICY_EVALUATION)
/* Group-call policy evaluations made through the library's public entry point (GNU ld --wrap). */
static int g_group_evaluations = 0;

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __real_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                             int encrypted, int data_call, dsd_tg_policy_decision* out);
int __wrap_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                             int encrypted, int data_call, dsd_tg_policy_decision* out);

int
__wrap_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                         int encrypted, int data_call, dsd_tg_policy_decision* out) {
    g_group_evaluations++;
    return __real_dsd_tg_policy_evaluate_group_call(opts, state, tg, src, encrypted, data_call, out);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
#endif

/* A live patch on @p sg carrying @p count members, heard now. */
static void
seed_patch(dsd_state* st, uint16_t sg, const uint16_t* members, int count) {
    int idx = -1;
    for (int i = 0; i < st->p25_patch_count; i++) {
        if (st->p25_patch_sgid[i] == sg) {
            idx = i;
        }
    }
    if (idx < 0) {
        idx = st->p25_patch_count++;
    }
    st->p25_patch_sgid[idx] = sg;
    st->p25_patch_is_patch[idx] = 1U;
    st->p25_patch_active[idx] = 1U;
    st->p25_patch_wgid_count[idx] = (uint8_t)count;
    for (int k = 0; k < count; k++) {
        st->p25_patch_wgid[idx][k] = members[k];
    }
    dsd_p25_patch_touch_members(st, idx);
}

/* A frame its decoder output as speech (issue #890): the call's media mark and its checked media, as a decoder makes
   both for a good frame; the voice-only scan credits only the checked media. */
static int
decoded_media(dsd_state* state, uint8_t slot, int media_active, double t) {
    const int rc = dsd_call_state_update_media(state, slot, media_active, t);
    dsd_call_snapshot call;
    if (rc == 1 && media_active && dsd_call_state_get(state, slot, &call) > 0) {
        (void)dsd_call_state_note_checked_media(state, slot, call.protocol, t);
    }
    return rc;
}

/* A P25 Phase 1 group voice call on slot 0 to @p ota, judged on @p policy, with 0.5 s of voice from @p t. */
static void
open_voice(dsd_state* st, uint64_t ota, uint64_t policy, double t) {
    dsd_call_observation obs;
    DSD_MEMSET(&obs, 0, sizeof(obs));
    obs.protocol = DSD_SYNC_P25P1_POS;
    obs.slot = 0U;
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.ota_source_id = 11U;
    obs.ota_target_id = ota;
    obs.policy_target_id = policy;
    obs.observed_m = t;
    (void)dsd_call_state_observe(st, &obs, DSD_CALL_BOUNDARY_BEGIN);
    (void)decoded_media(st, 0U, 1, t);
    (void)decoded_media(st, 0U, 1, t + 0.5);
}

static int
voice_counts(const dsd_opts* opts, const dsd_state* st) {
    dsd_scan_voice_probe_result result;
    return dsd_scan_voice_probe(opts, st, &result) == 1;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
} fixture;

/* Allow-list mode: member 22 listed, the SG 500 and member 24 not. */
static int
fixture_open(fixture* f) {
    f->opts = calloc(1, sizeof(*f->opts));
    f->st = calloc(1, sizeof(*f->st));
    if (!f->opts || !f->st || dsd_call_state_ensure(f->st) <= 0) {
        return 1;
    }
    f->opts->trunk_tune_group_calls = 1;
    f->opts->trunk_tune_enc_calls = 1;
    f->opts->trunk_use_allow_list = 1;
    f->opts->scanner_mode = 1;
    f->opts->scan_voice_only = 1;
    return listed(f->st, 22);
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
test_voice_check(void) {
    fixture f;
    int rc = expect_true("voice: fixture", fixture_open(&f) == 0);
    const uint16_t both[] = {22, 24};
    const uint16_t unlisted_only[] = {24};
    seed_patch(f.st, 500, both, 2);
    open_voice(f.st, 500, 22, 100.0);
    rc |= expect_true("voice: a listened member carries the patch", voice_counts(f.opts, f.st));
    seed_patch(f.st, 500, unlisted_only, 1);
    rc |= expect_true("voice: the member deleted from the patch counts no more", !voice_counts(f.opts, f.st));
    seed_patch(f.st, 500, both, 2);
    rc |= expect_true("voice: avoid", dsd_tg_policy_session_avoid_add(f.st, 500) == 0);
    rc |= expect_true("voice: an avoided supergroup counts no more", !voice_counts(f.opts, f.st));
    dsd_tg_policy_session_avoid_clear(f.st);
    rc |= expect_true("voice: the avoid lifted", voice_counts(f.opts, f.st));
    // A listened member re-selected for a call stored on the unlisted SG.
    open_voice(f.st, 500, 500, 101.0);
    rc |= expect_true("voice: a call stored on the SG counts through its listened member", voice_counts(f.opts, f.st));
    // Preservation: a call on no live patch keeps its stored target.
    open_voice(f.st, 600, 22, 102.0);
    rc |= expect_true("voice: no live patch keeps the stored member", voice_counts(f.opts, f.st));
    fixture_close(&f);
    return rc;
}

/* The voice check is judged on the call the audio follows (the patch's keep-or-resolve media target), with that call's
   own tune verdict: the -Y gate stays for traffic it plays and would follow, as for a plain call. A member that plays
   but may not be followed (here an encryption lockout of its own, with encrypted calls not tuned) does not hold the
   channel even when a lower-priority member could be followed; an encrypted call with encrypted tuning off never
   does. */
static int
test_voice_tune_verdict(void) {
    fixture f;
    int rc = expect_true("tune: fixture", fixture_open(&f) == 0);
    f.opts->trunk_tune_enc_calls = 0;
    rc |= expect_true("tune: members", listed_priority(f.st, 30, 50) == 0 && listed_priority(f.st, 31, 10) == 0);
    const uint16_t members[] = {30, 31};
    seed_patch(f.st, 520, members, 2);
    open_voice(f.st, 520, 520, 100.0);
    rc |= expect_true("tune: the playing member followed", voice_counts(f.opts, f.st));
    rc |= expect_true("tune: lockout armed", dsd_enc_lockout_note(f.st, 30, 1, 0x84, 0x1234) == 1);
    rc |= expect_true("tune: the playing member may not be followed", !voice_counts(f.opts, f.st));
    dsd_call_crypto_update update;
    DSD_MEMSET(&update, 0, sizeof(update));
    update.classification = DSD_CALL_CRYPTO_ENCRYPTED;
    update.observed_m = 100.6;
    open_voice(f.st, 520, 31, 101.0);
    rc |= expect_true("tune: a clear call on the other member", voice_counts(f.opts, f.st));
    (void)dsd_call_state_update_crypto(f.st, 0U, DSD_SYNC_P25P1_POS, &update);
    rc |= expect_true("tune: encrypted with encrypted tuning off", !voice_counts(f.opts, f.st));
    f.opts->trunk_tune_enc_calls = 1;
    rc |= expect_true("tune: encrypted with encrypted tuning on", voice_counts(f.opts, f.st));
    fixture_close(&f);
    return rc;
}

/* The checks run every frame: a kept call costs what it cost before the patch resolver (one evaluation for a kept
   supergroup or a plain call, two for a kept member: the supergroup's final blocks and the member), and a Hold the
   patch does not carry costs none. */
static int
test_evaluation_counts(void) {
    int rc = 0;
#if defined(DSD_TEST_WRAP_POLICY_EVALUATION)
    fixture f;
    rc |= expect_true("count: fixture", fixture_open(&f) == 0 && listed(f.st, 540) == 0);
    const uint16_t members[] = {22, 24};
    seed_patch(f.st, 500, members, 2);
    seed_patch(f.st, 540, members, 2);
    open_voice(f.st, 600, 600, 100.0);
    g_group_evaluations = 0;
    (void)voice_counts(f.opts, f.st);
    rc |= expect_true("count: a plain call, one evaluation", g_group_evaluations == 1);
    open_voice(f.st, 540, 540, 101.0);
    g_group_evaluations = 0;
    rc |= expect_true("count: kept supergroup counts", voice_counts(f.opts, f.st));
    rc |= expect_true("count: a kept supergroup, one evaluation", g_group_evaluations == 1);
    open_voice(f.st, 500, 22, 102.0);
    g_group_evaluations = 0;
    rc |= expect_true("count: kept member counts", voice_counts(f.opts, f.st));
    rc |= expect_true("count: a kept member, two evaluations", g_group_evaluations == 2);
    f.st->tg_hold = 99;
    g_group_evaluations = 0;
    rc |= expect_true("count: a Hold the patch does not carry", dsd_scan_tg_hold_call_active(f.opts, f.st) == 0);
    rc |= expect_true("count: ... costs no evaluation", g_group_evaluations == 0);
    f.st->tg_hold = 22;
    g_group_evaluations = 0;
    rc |= expect_true("count: a Hold on a member the patch carries", dsd_scan_tg_hold_call_active(f.opts, f.st) == 1);
    rc |= expect_true("count: ... costs the supergroup's and the member's evaluations", g_group_evaluations == 2);
    open_voice(f.st, 540, 540, 102.5);
    f.st->tg_hold = 540;
    g_group_evaluations = 0;
    rc |= expect_true("count: a Hold on the supergroup", dsd_scan_tg_hold_call_active(f.opts, f.st) == 1);
    rc |= expect_true("count: ... costs the supergroup's evaluation", g_group_evaluations == 1);

    // However many members the patch carries: a 32-member patch costs no more.
    uint16_t many[DSD_P25_PATCH_GROUP_MAX];
    for (int k = 0; k < DSD_P25_PATCH_GROUP_MAX; k++) {
        many[k] = (uint16_t)(700 + k);
    }
    seed_patch(f.st, 560, many, DSD_P25_PATCH_GROUP_MAX);
    open_voice(f.st, 560, 560, 103.0);
    f.st->tg_hold = 731;
    rc |= expect_true("count: listed held member", listed(f.st, 731) == 0);
    g_group_evaluations = 0;
    rc |= expect_true("count: a Hold on the last of 32 members", dsd_scan_tg_hold_call_active(f.opts, f.st) == 1);
    rc |= expect_true("count: ... still costs two evaluations", g_group_evaluations == 2);
    fixture_close(&f);
#endif
    return rc;
}

static int
test_hold_check(void) {
    fixture f;
    int rc = expect_true("hold: fixture", fixture_open(&f) == 0);
    const uint16_t both[] = {22, 24};
    const uint16_t unlisted_only[] = {24};
    seed_patch(f.st, 500, both, 2);
    open_voice(f.st, 500, 24, 100.0);
    f.st->tg_hold = 22;
    rc |= expect_true("hold: a held member the patch carries", dsd_scan_tg_hold_call_active(f.opts, f.st) == 1);
    seed_patch(f.st, 500, unlisted_only, 1);
    open_voice(f.st, 500, 22, 101.0);
    rc |= expect_true("hold: a held member deleted from the patch", dsd_scan_tg_hold_call_active(f.opts, f.st) == 0);
    seed_patch(f.st, 500, both, 2);
    rc |= expect_true("hold: avoid", dsd_tg_policy_session_avoid_add(f.st, 500) == 0);
    rc |= expect_true("hold: a held member of an avoided supergroup", dsd_scan_tg_hold_call_active(f.opts, f.st) == 0);
    dsd_tg_policy_session_avoid_clear(f.st);
    // Preservation: a call on no live patch is held by its stored target.
    open_voice(f.st, 600, 22, 102.0);
    rc |= expect_true("hold: no live patch keeps the stored member", dsd_scan_tg_hold_call_active(f.opts, f.st) == 1);
    fixture_close(&f);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_voice_check();
    rc |= test_hold_check();
    rc |= test_voice_tune_verdict();
    rc |= test_evaluation_counts();
    if (rc == 0) {
        printf("ENGINE_P25_PATCH_SCAN: OK\n");
    }
    return rc;
}
