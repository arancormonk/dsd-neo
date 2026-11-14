// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A followed patched call (a supergroup, SG, carrying member work groups) is one call while the patch resolver
 * re-selects the member it is judged on: the trunk SM keys follow, dwell, duplicates and moved slots on the
 * over-the-air identity, re-resolves on voice identity events and ticks, and describes the held call to preemption by
 * the current resolution (no protection once nothing in the patch may be followed). */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../test_support/radio_policy_test.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_env.h"
#include "test_support.h"

#define CH_HELD   0x100A /* slot 0 on a TDMA plan */
#define CH_HELD_1 0x100B /* slot 1 of the same TDMA carrier */
#define CH_PRIO   0x100C
#define CH_THIRD  0x100D
#define FREQ_HELD 851125000L
#define FREQ_PRIO 851150000L

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)ted_sps;
    (void)request_id;
    return freq > 0 ? DSD_TRUNK_TUNE_RESULT_OK : DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static void
set_clock_s(double s) {
    dsd_decode_clock_test_set_ns((uint64_t)(s * 1e9));
}

static void
set_dwell_cooldown_ms(const char* dwell, const char* cooldown) {
    (void)dsd_test_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", dwell, 1);
    (void)dsd_test_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", cooldown, 1);
}

static int
seed_row(dsd_state* st, uint32_t id, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, "A", "row", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return 1;
    }
    row.priority = priority;
    row.preempt = preempt ? 1u : 0u;
    return dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static void
seed_fdma_iden(dsd_state* st) {
    st->p25_chan_iden = 1;
    st->p25_iden_fdma[1].base_freq = 851000000 / 5;
    st->p25_iden_fdma[1].chan_type = 1;
    st->p25_iden_fdma[1].chan_spac = 100;
    st->p25_iden_fdma[1].trust = 2;
    st->p25_iden_fdma[1].populated = 1;
    st->p25_chan_tdma_explicit[1] = 1;
}

static void
seed_tdma_iden(dsd_state* st) {
    st->p25_chan_iden = 1;
    st->p25_iden_tdma[1].base_freq = 851000000 / 5;
    st->p25_iden_tdma[1].chan_type = 3;
    st->p25_iden_tdma[1].chan_spac = 100;
    st->p25_iden_tdma[1].trust = 2;
    st->p25_iden_tdma[1].populated = 1;
    st->p25_chan_tdma_explicit[1] = 2;
    /* The descrambler seed a TDMA grant waits for (made-up values). */
    st->p2_wacn = 0xBEE00;
    st->p2_sysid = 0x1A2;
    st->p2_cc = 0x293;
}

/* A fresh Phase 1 site (or a Phase 2 one with @p tdma): the control channel known and the preempting talkgroups 250
 * (priority 60) and 200 (priority 100) listed. */
static void
fresh_site(dsd_opts* opts, dsd_state* st, int tdma) {
    dsd_state_ext_free_all(st);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(st, 0, sizeof(*st));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_hangtime = 2.0f;
    st->p25_cc_freq = 851000000;
    st->synctype = tdma ? DSD_SYNC_P25P2_POS : DSD_SYNC_P25P1_POS;
    if (tdma) {
        seed_tdma_iden(st);
    } else {
        seed_fdma_iden(st);
    }
    (void)seed_row(st, 250, 60, 1);
    (void)seed_row(st, 200, 100, 1);
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, st);
}

/* Follow group @p tg on @p channel from the control channel (source 8000 + tg) and hear its voice. */
static void
follow(dsd_opts* opts, dsd_state* st, int tg, int channel, int svc_bits) {
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, tg, 8000 + tg, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant);
    const int slot = p25_sm_get_ctx()->vc_is_tdma ? (channel & 1) : 0;
    p25_sm_event_t voice = p25_sm_ev_active(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice);
}

static void
offer_update(dsd_opts* opts, dsd_state* st, int tg, int channel) {
    p25_sm_event_t ev = p25_sm_ev_group_grant_update(channel, 0, tg, 0, P25_SM_SVC_UNKNOWN);
    ev.preempt_only = 1;
    p25_sm_event(p25_sm_get_ctx(), opts, st, &ev);
}

static int
following_freq(long freq) {
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    return ctx->state == P25_SM_TUNED && ctx->vc_freq_hz == freq;
}

static int
call_on(const dsd_state* st, uint8_t slot, dsd_call_snapshot* call) {
    return dsd_call_state_get(st, slot, call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE;
}

static uint64_t
call_policy_target(const dsd_state* st, uint8_t slot) {
    dsd_call_snapshot call;
    return call_on(st, slot, &call) ? call.policy_target_id : 0U;
}

/* Whether a preempting candidate of @p priority on another carrier would displace what the ledger holds now. */
static int
ledger_yields_to(const dsd_opts* opts, const dsd_state* st, int priority) {
    const dsd_tg_policy_call_route route = {
        .target_id = 9999U, .source_id = 0U, .freq_hz = FREQ_PRIO, .channel = CH_PRIO, .slot = -1};
    dsd_tg_policy_decision candidate;
    DSD_MEMSET(&candidate, 0, sizeof(candidate));
    candidate.target_id = 9999U;
    candidate.tune_allowed = 1;
    candidate.tg_tune_allowed = 1;
    candidate.preempt_requested = 1;
    candidate.priority = priority;
    return dsd_tg_policy_should_preempt(opts, st, &route, &candidate, dsd_decode_now_mono_s());
}

/* A member re-selected through an unchanged voice identity, then a re-grant of the supergroup, is the call already
 * followed: no release, the dwell runs from the first follow, the epoch stays, and no takeover cooldown is stamped
 * (a priority call may still preempt at once). */
static int
test_reselection_is_the_same_call(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("same call: seed members", seed_row(st, 1501, 50, 0) == 0 && seed_row(st, 1502, 40, 1) == 0);
    p25_patch_add_wgid(st, 1500, 1501);
    p25_patch_add_wgid(st, 1500, 1502);
    set_clock_s(100.0);
    follow(opts, st, 1500, CH_HELD, 0x00);
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    dsd_call_snapshot before;
    rc |= expect_true("same call: judged on the first member",
                      call_on(st, 0, &before) && before.policy_target_id == 1501U && ctx->slots[0].target_id == 1501);
    const uint32_t releases = ctx->release_count;
    const double followed_since = ctx->slots[0].followed_since_m;

    set_clock_s(102.0);
    rc |= expect_true("same call: the first member drops", seed_row(st, 1501, 30, 0) == 0);
    p25_sm_event_t lc = p25_sm_ev_active_call(0, 1500, 0, 9500, 1, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &lc);
    rc |= expect_true("same call: an unchanged voice identity re-selects the member",
                      call_policy_target(st, 0) == 1502U && ctx->slots[0].target_id == 1502);

    p25_sm_event_t regrant = p25_sm_ev_group_grant_update(CH_HELD, 0, 1500, 9500, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &regrant);
    dsd_call_snapshot after;
    rc |= expect_true("same call: the re-grant releases nothing",
                      following_freq(FREQ_HELD) && ctx->release_count == releases);
    rc |= expect_true("same call: the dwell runs from the first follow",
                      fabs(ctx->slots[0].followed_since_m - followed_since) < 1e-9);
    rc |= expect_true("same call: the epoch stays", call_on(st, 0, &after) && after.epoch == before.epoch);
    rc |= expect_true("same call: still judged on the re-selected member", after.policy_target_id == 1502U);

    set_clock_s(102.2);
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("same call: no takeover cooldown was stamped", following_freq(FREQ_PRIO));
    return rc;
}

/* Preservation: a re-grant of a plain talkgroup is the same call, as before. */
static int
test_plain_regrant_is_the_same_call(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("plain: seed", seed_row(st, 1600, 40, 1) == 0);
    set_clock_s(110.0);
    follow(opts, st, 1600, CH_HELD, 0x00);
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const uint32_t releases = ctx->release_count;
    dsd_call_snapshot before;
    rc |= expect_true("plain: followed", call_on(st, 0, &before) && before.policy_target_id == 1600U);
    set_clock_s(112.0);
    p25_sm_tick_ctx(p25_sm_get_ctx(), opts, st);
    p25_sm_event_t regrant = p25_sm_ev_group_grant_update(CH_HELD, 0, 1600, 9600, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &regrant);
    dsd_call_snapshot after;
    rc |= expect_true("plain: no release", following_freq(FREQ_HELD) && ctx->release_count == releases);
    rc |= expect_true("plain: dwell kept", fabs(ctx->slots[0].followed_since_m - 110.0) < 1e-9);
    rc |= expect_true("plain: epoch and target kept",
                      call_on(st, 0, &after) && after.epoch == before.epoch && after.policy_target_id == 1600U);
    return rc;
}

/* The held call is described by the patch as it stands: a deleted high-priority member protects nothing. */
static int
test_held_deleted_member(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("deleted: seed members", seed_row(st, 1511, 90, 0) == 0 && seed_row(st, 1512, 10, 0) == 0);
    p25_patch_add_wgid(st, 1510, 1511);
    p25_patch_add_wgid(st, 1510, 1512);
    set_clock_s(120.0);
    follow(opts, st, 1510, CH_HELD, 0x00);
    rc |= expect_true("deleted: judged on the high member", call_policy_target(st, 0) == 1511U);
    set_clock_s(122.0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("deleted: the high member protects the patch", following_freq(FREQ_HELD));
    p25_patch_remove_wgid(st, 1510, 1511);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("deleted: once deleted it protects nothing", following_freq(851162500L));
    return rc;
}

/* Same decode-clock reading: a radio block leaves the patch's protection (radio rules never strip a talkgroup's
 * priority; preservation), and a call skip on the supergroup during the call leaves nothing that may be followed, so
 * the held call is preemptible. */
static int
test_held_same_second_rejudge(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("rejudge: seed member", seed_row(st, 1521, 90, 0) == 0);
    p25_patch_add_wgid(st, 1520, 1521);
    set_clock_s(130.0);
    follow(opts, st, 1520, CH_HELD, 0x00);
    set_clock_s(132.0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("rejudge: protected", following_freq(FREQ_HELD));
    rc |= expect_true("rejudge: radio list", dsd_test_install_radio_policy(st, 8000 + 1520, "B", 0, 0, 0) == 0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("rejudge: a radio block keeps the protection", following_freq(FREQ_HELD));
    rc |= expect_true("rejudge: skip armed", dsd_tg_policy_call_skip_arm(st, 1520, 0, 0, dsd_decode_now_mono_s()) == 0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("rejudge: a skipped supergroup is preemptible in the same second", following_freq(851162500L));
    return rc;
}

/* A session avoid on the supergroup during the call: nothing in the patch may be followed, so it is preemptible. */
static int
test_held_sg_final_block(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("final: seed member", seed_row(st, 1531, 90, 0) == 0);
    p25_patch_add_wgid(st, 1530, 1531);
    set_clock_s(140.0);
    follow(opts, st, 1530, CH_HELD, 0x00);
    set_clock_s(142.0);
    rc |= expect_true("final: avoid", dsd_tg_policy_session_avoid_add(st, 1530) == 0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("final: an avoided supergroup is preemptible", following_freq(851162500L));
    return rc;
}

/* The chosen member's priority drops below another member's: a competing call between the two is refused, because the
 * patch is held by the other member's priority (and re-selects it). */
static int
test_held_keeps_other_member_priority(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("other: seed members", seed_row(st, 1541, 90, 0) == 0 && seed_row(st, 1542, 70, 0) == 0);
    p25_patch_add_wgid(st, 1540, 1541);
    p25_patch_add_wgid(st, 1540, 1542);
    set_clock_s(150.0);
    follow(opts, st, 1540, CH_HELD, 0x00);
    rc |= expect_true("other: judged on the high member", call_policy_target(st, 0) == 1541U);
    set_clock_s(152.0);
    rc |= expect_true("other: the chosen member drops", seed_row(st, 1541, 10, 0) == 0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("other: the other member's priority holds the patch", following_freq(FREQ_HELD));
    rc |= expect_true("other: and judges it", call_policy_target(st, 0) == 1542U);
    return rc;
}

/* A received service priority of 5 stays the call's displayed priority through a re-selection on a tick, while the
 * ledger follows the member's policy priority (100, then 50). */
static int
test_service_priority_vs_ledger(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 0);
    set_dwell_cooldown_ms("750", "1000");
    rc |= expect_true("svc: seed members", seed_row(st, 1551, 100, 0) == 0 && seed_row(st, 1552, 50, 0) == 0);
    p25_patch_add_wgid(st, 1550, 1551);
    p25_patch_add_wgid(st, 1550, 1552);
    set_clock_s(160.0);
    follow(opts, st, 1550, CH_HELD, 0x05);
    dsd_call_snapshot call;
    rc |= expect_true("svc: received priority 5", call_on(st, 0, &call) && call.priority == 5U);
    set_clock_s(162.0);
    rc |= expect_true("svc: the ledger holds 100", !ledger_yields_to(opts, st, 99));
    p25_patch_remove_wgid(st, 1550, 1551);
    p25_sm_tick_ctx(p25_sm_get_ctx(), opts, st);
    rc |= expect_true("svc: the tick re-selects the member", call_policy_target(st, 0) == 1552U);
    rc |= expect_true("svc: the received priority stays 5",
                      call_on(st, 0, &call) && call.priority == 5U && call.service_options == 0x05U);
    rc |= expect_true("svc: the ledger follows the member (50)",
                      !ledger_yields_to(opts, st, 50) && ledger_yields_to(opts, st, 51));
    return rc;
}

/* Phase 2: the patch moves to the other slot of the carrier while its resolution re-selects a member. The call that
 * left slot 0 is ended there, with its grant and crypto state. */
static int
test_slot_move_with_reselection(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st, 1);
    set_dwell_cooldown_ms("0", "0");
    rc |= expect_true("move: seed members", seed_row(st, 1561, 90, 0) == 0 && seed_row(st, 1562, 70, 0) == 0);
    p25_patch_add_wgid(st, 1560, 1561);
    p25_patch_add_wgid(st, 1560, 1562);
    set_clock_s(170.0);
    follow(opts, st, 1560, CH_HELD, 0x00);
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    rc |= expect_true("move: slot 0 judged on the high member",
                      ctx->vc_is_tdma && ctx->slots[0].grant_active && call_policy_target(st, 0) == 1561U);
    st->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st->p25_p2_audio_allowed[0] = 1;
    set_clock_s(172.0);
    rc |= expect_true("move: the chosen member drops", seed_row(st, 1561, 10, 0) == 0);
    p25_sm_event_t moved = p25_sm_ev_group_grant(CH_HELD_1, 0, 1560, 9560, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &moved);
    dsd_call_snapshot old_slot;
    rc |= expect_true("move: slot 1 holds the patch on the re-selected member",
                      ctx->slots[1].grant_active && ctx->slots[1].target_id == 1562 && ctx->slots[1].ota_tg == 1560);
    rc |= expect_true("move: slot 0's grant is cleared", ctx->slots[0].grant_active == 0);
    rc |= expect_true("move: slot 0's call ended",
                      dsd_call_state_get(st, 0, &old_slot) > 0 && old_slot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("move: slot 0's audio and crypto reset",
                      st->p25_p2_audio_allowed[0] == 0 && st->p25_crypto_state[0] != DSD_P25_CRYPTO_CLEAR);
    return rc;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    dsd_decode_clock_use_test(1000000000ULL);
    int rc = 0;
    rc |= test_reselection_is_the_same_call(&opts, &st);
    rc |= test_plain_regrant_is_the_same_call(&opts, &st);
    rc |= test_held_deleted_member(&opts, &st);
    rc |= test_held_same_second_rejudge(&opts, &st);
    rc |= test_held_sg_final_block(&opts, &st);
    rc |= test_held_keeps_other_member_priority(&opts, &st);
    rc |= test_service_priority_vs_ledger(&opts, &st);
    rc |= test_slot_move_with_reselection(&opts, &st);
    dsd_state_ext_free_all(&st);
    dsd_decode_clock_use_system();
    if (rc == 0) {
        printf("P25_PATCH_FOLLOW: OK\n");
    }
    return rc;
}
