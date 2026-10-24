// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* A talkgroup set to preempt takes the receiver from a lower-priority call when the voice channel announces it:
 * Phase 2 grant updates for another carrier, Phase 1 link control (0x42, 0x44 without -j), during the held call's
 * hangtime, with dwell measured from when the held call was first followed, and a takeover's cooldown surviving a
 * group list reload. */

#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define CH_HELD   0x100A /* 851.1250 MHz */
#define CH_OTHER  0x100B /* 851.1375 MHz */
#define CH_PRIO   0x100C /* 851.1500 MHz */
#define CH_THIRD  0x100D /* 851.1625 MHz */
#define FREQ_HELD 851125000L
#define FREQ_PRIO 851150000L

/* The Phase 2 MAC path links GPS decoding, whose text helper lives in the DMR library. */
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

static dsd_trunk_tune_result g_return_result = DSD_TRUNK_TUNE_RESULT_OK;

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
    return g_return_result;
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
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", dwell, 1);
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", cooldown, 1);
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

/* A fresh P25 Phase 1 site: control channel known, rows for the held talkgroup 100 (priority 10), an ordinary 300,
 * a mid-priority preempt 250 (60) and the priority talkgroup 200 (100, preempt). */
static void
fresh_site(dsd_opts* opts, dsd_state* st) {
    dsd_state_ext_free_all(st);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(st, 0, sizeof(*st));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->p25_lcw_retune = 1;
    opts->trunk_hangtime = 2.0f;
    st->p25_cc_freq = 851000000;
    st->synctype = DSD_SYNC_P25P1_POS;
    seed_fdma_iden(st);
    (void)seed_row(st, 100, 10, 0);
    (void)seed_row(st, 300, 0, 0);
    (void)seed_row(st, 250, 60, 1);
    (void)seed_row(st, 200, 100, 1);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_init_ctx(p25_sm_get_ctx(), opts, st);
}

/* Follow talkgroup @p tg on @p channel from the control channel and hear its voice. */
static void
follow(dsd_opts* opts, dsd_state* st, int tg, int channel) {
    p25_sm_event_t grant = p25_sm_ev_group_grant(channel, 0, tg, 7000 + tg, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant);
    p25_sm_event_t voice = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice);
}

static void
offer_update(dsd_opts* opts, dsd_state* st, int tg, int channel) {
    p25_sm_event_t ev = p25_sm_ev_group_grant_update(channel, 0, tg, 0, P25_SM_SVC_UNKNOWN);
    ev.preempt_only = 1;
    p25_sm_event(p25_sm_get_ctx(), opts, st, &ev);
}

static void
mac_group_update_pair(dsd_opts* opts, dsd_state* st, int ch1, int tg1, int ch2, int tg2) {
    unsigned long long int mac[24] = {0};
    mac[1] = 0x42; // Group Voice Channel Grant Update - Implicit
    mac[2] = (unsigned)(ch1 >> 8) & 0xFFU;
    mac[3] = (unsigned)ch1 & 0xFFU;
    mac[4] = (unsigned)(tg1 >> 8) & 0xFFU;
    mac[5] = (unsigned)tg1 & 0xFFU;
    mac[6] = (unsigned)(ch2 >> 8) & 0xFFU;
    mac[7] = (unsigned)ch2 & 0xFFU;
    mac[8] = (unsigned)(tg2 >> 8) & 0xFFU;
    mac[9] = (unsigned)tg2 & 0xFFU;
    process_MAC_VPDU(opts, st, 0, P25_MAC_PDU_ACTIVE, mac);
}

static void
set_bits(uint8_t* bits, int off, int len, unsigned value) {
    for (int i = 0; i < len; i++) {
        bits[off + i] = (uint8_t)((value >> (len - 1 - i)) & 1U);
    }
}

static void
lcw_group_update_pair(dsd_opts* opts, dsd_state* st, int ch1, int tg1, int ch2, int tg2) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits(lcw, 0, 8, 0x42);
    set_bits(lcw, 8, 16, (unsigned)ch1);
    set_bits(lcw, 24, 16, (unsigned)tg1);
    set_bits(lcw, 40, 16, (unsigned)ch2);
    set_bits(lcw, 56, 16, (unsigned)tg2);
    p25_lcw(opts, st, lcw, 0);
}

static void
lcw_group_update_explicit(dsd_opts* opts, dsd_state* st, int ch, int tg) {
    uint8_t lcw[72];
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    set_bits(lcw, 0, 8, 0x44);
    set_bits(lcw, 24, 16, (unsigned)tg);
    set_bits(lcw, 40, 16, (unsigned)ch);
    p25_lcw(opts, st, lcw, 0);
}

static int
following(int tg, long freq) {
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    return ctx->state == P25_SM_TUNED && ctx->vc_tg == tg && ctx->vc_freq_hz == freq;
}

/* Phase 2: a grant update the voice channel carries for another carrier preempts only for a preempt talkgroup, and a
 * plain one leaves the held call and its timers untouched (the regression the off-carrier gate was added for). */
static int
test_phase2_update_preempts(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(100.0);
    follow(opts, st, 100, CH_HELD);
    rc |= expect_true("p2: following the held call", following(100, FREQ_HELD));
    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const unsigned grants = ctx->grant_count;
    const double tune_m = ctx->t_tune_m;
    const double grant_m = ctx->slots[0].last_grant_m;

    set_clock_s(102.0);
    mac_group_update_pair(opts, st, CH_OTHER, 300, CH_OTHER, 300);
    rc |= expect_true("p2: plain update elsewhere ignored", following(100, FREQ_HELD) && ctx->grant_count == grants);
    rc |= expect_true("p2: plain update leaves timers",
                      fabs(ctx->t_tune_m - tune_m) < 1e-9 && fabs(ctx->slots[0].last_grant_m - grant_m) < 1e-9);

    mac_group_update_pair(opts, st, CH_OTHER, 300, CH_PRIO, 200);
    rc |= expect_true("p2: preempt talkgroup announced elsewhere takes the receiver", following(200, FREQ_PRIO));
    return rc;
}

/* Several calls announced together go highest priority first, on the control channel and as preemption candidates. */
static int
test_phase2_ranking(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int order = 0; order < 2; order++) {
        fresh_site(opts, st);
        set_dwell_cooldown_ms("0", "0");
        set_clock_s(200.0);
        (void)seed_row(st, 300, 50, 0);
        if (order == 0) {
            mac_group_update_pair(opts, st, CH_OTHER, 300, CH_PRIO, 200);
        } else {
            mac_group_update_pair(opts, st, CH_PRIO, 200, CH_OTHER, 300);
        }
        rc |= expect_true(order == 0 ? "cc ranking: priority listed second" : "cc ranking: priority listed first",
                          following(200, FREQ_PRIO));
    }

    /* While tuned: 250 (60) listed before 200 (100); taking 250 first would put 200 behind its dwell. */
    fresh_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(300.0);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(302.0);
    mac_group_update_pair(opts, st, CH_THIRD, 250, CH_PRIO, 200);
    rc |= expect_true("tuned ranking: highest priority preempts", following(200, FREQ_PRIO));
    return rc;
}

/* Phase 1 link control: 0x42 (both pairs) and 0x44 with explicit retune off reach the SM as preemption candidates. */
static int
test_phase1_link_control(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(400.0);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(402.0);
    lcw_group_update_pair(opts, st, CH_OTHER, 300, CH_OTHER, 301);
    rc |= expect_true("p1 0x42: plain updates ignored", following(100, FREQ_HELD));
    lcw_group_update_pair(opts, st, CH_OTHER, 300, CH_PRIO, 200);
    rc |= expect_true("p1 0x42: preempt talkgroup in the second pair", following(200, FREQ_PRIO));

    fresh_site(opts, st);
    opts->p25_lcw_retune = 0;
    set_clock_s(500.0);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(502.0);
    lcw_group_update_explicit(opts, st, CH_OTHER, 300);
    rc |= expect_true("p1 0x44 without -j: plain update not followed", following(100, FREQ_HELD));
    lcw_group_update_explicit(opts, st, CH_PRIO, 200);
    rc |= expect_true("p1 0x44 without -j: preempt talkgroup still preempts", following(200, FREQ_PRIO));
    return rc;
}

/* The gap between transmissions: the transmission's end cleared its record, but the conversation still holds the
 * carrier and is outranked only by a strictly higher priority. */
static int
test_hangtime(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(600.0);
    (void)seed_row(st, 260, 5, 1);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(601.5);
    p25_sm_event_t tdu = p25_sm_ev_tdu();
    p25_sm_event(p25_sm_get_ctx(), opts, st, &tdu);
    rc |= expect_true("hangtime: still on the carrier", following(100, FREQ_HELD));
    set_clock_s(601.6);
    offer_update(opts, st, 260, CH_THIRD);
    rc |= expect_true("hangtime: lower priority preempt refused", following(100, FREQ_HELD));
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("hangtime: higher priority preempts the held conversation", following(200, FREQ_PRIO));
    return rc;
}

/* After a transmission's end, a repeat or a re-assignment of the held call keeps the dwell running from when the call
 * was first followed and stamps no cooldown, even when the held call itself has preempt. */
static int
test_dwell_from_follow(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int reassign = 0; reassign < 2; reassign++) {
        fresh_site(opts, st);
        // The repeat must clear the stale-regrant probe delay (2 s) yet stay inside the conversation's hangtime.
        opts->trunk_hangtime = 3.0f;
        p25_sm_init_ctx(p25_sm_get_ctx(), opts, st);
        set_dwell_cooldown_ms("750", "1000");
        (void)seed_row(st, 100, 10, 1);
        set_clock_s(700.0);
        follow(opts, st, 100, CH_HELD);
        set_clock_s(702.0);
        p25_sm_event_t tdu = p25_sm_ev_tdu();
        p25_sm_event(p25_sm_get_ctx(), opts, st, &tdu);
        // An update past the stale-regrant probe delay reaches the duplicate path; an assignment is a re-grant.
        set_clock_s(reassign ? 702.05 : 704.1);
        p25_sm_event_t repeat = reassign ? p25_sm_ev_group_grant(CH_HELD, 0, 100, 7100, 0x00)
                                         : p25_sm_ev_group_grant_update(CH_HELD, 0, 100, 0, 0x00);
        p25_sm_event(p25_sm_get_ctx(), opts, st, &repeat);
        rc |= expect_true(reassign ? "dwell: re-grant keeps the held call" : "dwell: repeat keeps the held call",
                          following(100, FREQ_HELD));
        if (!reassign) {
            rc |= expect_true("dwell: the repeat accepted as the stale guard's probe spends it",
                              p25_sm_get_ctx()->recent_call_ends[0].probe_attempted == 1);
        }
        set_clock_s(reassign ? 702.1 : 704.2);
        offer_update(opts, st, 200, CH_PRIO);
        rc |= expect_true(reassign ? "dwell: re-grant keeps the first follow time" : "dwell: repeat keeps it",
                          following(200, FREQ_PRIO));
    }
    return rc;
}

/* A takeover's cooldown survives the release that cleared the displaced record and a group list reload. */
static int
test_cooldown_survives_reload(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    char path[64];
    fresh_site(opts, st);
    DSD_SNPRINTF(path, sizeof(path), "%s", "dsd-neo-test-p25-preempt-XXXXXX");
    const int fd = dsd_mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    (void)dsd_close(fd);
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        (void)remove(path);
        return 1;
    }
    DSD_FPRINTF(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n"
                    "100,A,held,10,false,on,on,on,\n250,A,mid,60,true,on,on,on,\n200,A,top,100,true,on,on,on,\n");
    fclose(fp);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path);

    set_dwell_cooldown_ms("0", "1000");
    set_clock_s(800.0);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(802.0);
    offer_update(opts, st, 250, CH_THIRD);
    rc |= expect_true("cooldown: first takeover", following(250, 851162500L));
    rc |= expect_true("cooldown: reload", dsd_tg_policy_reload_group_file(opts, st) == 0);
    set_clock_s(802.5);
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("cooldown: second takeover inside the cooldown refused", following(250, 851162500L));
    set_clock_s(803.2);
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("cooldown: second takeover after it", following(200, FREQ_PRIO));
    (void)remove(path);
    return rc;
}

/* A clear-claiming grant for a locked-out priority talkgroup refused during the dwell leaves the ledger alone; the
 * accepted repeat releases it and is followed as a reprobe. */
static int
test_lockout_release_deferred(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    opts->trunk_tune_enc_calls = 0;
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(900.0);
    follow(opts, st, 100, CH_HELD);
    rc |= expect_true("lockout: ledger armed", dsd_enc_lockout_note(st, 200, 1, 0x84, 0x1234) == 1);
    p25_sm_event_t clear = p25_sm_ev_group_grant_update(CH_PRIO, 0, 200, 0, 0x00);
    clear.preempt_only = 1;
    set_clock_s(900.2);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &clear);
    rc |= expect_true("lockout: refused inside the dwell", following(100, FREQ_HELD));
    rc |= expect_true("lockout: ledger kept for the repeat", dsd_enc_lockout_entry_active(st, 200, 1));
    set_clock_s(901.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &clear);
    rc |= expect_true("lockout: repeat preempts", following(200, FREQ_PRIO));
    rc |= expect_true("lockout: released on acceptance", !dsd_enc_lockout_entry_active(st, 200, 1));
    rc |= expect_true("lockout: followed as a reprobe", p25_sm_get_ctx()->slots[0].enc_lockout_reprobe == 1);
    return rc;
}

/* A preemption whose return to the control channel is refused keeps the held call: no release latch left for the
 * next tick, no cooldown, and a later repeat retries. */
static int
test_refused_release_keeps_call(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("0", "1000");
    set_clock_s(1000.0);
    follow(opts, st, 100, CH_HELD);
    for (int i = 0; i < 2; i++) {
        g_return_result = i == 0 ? DSD_TRUNK_TUNE_RESULT_FAILED : DSD_TRUNK_TUNE_RESULT_DEFERRED;
        set_clock_s(1001.0 + i);
        offer_update(opts, st, 200, CH_PRIO);
        rc |= expect_true("refused release: held call kept", following(100, FREQ_HELD));
        rc |= expect_true("refused release: no release latch", st->p25_sm_force_release == 0);
        p25_sm_tick_ctx(p25_sm_get_ctx(), opts, st);
        rc |= expect_true("refused release: next tick keeps the call", following(100, FREQ_HELD));
    }
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    set_clock_s(1003.0);
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("refused release: retry preempts", following(200, FREQ_PRIO));
    return rc;
}

/* A skipped priority talkgroup announced on the voice channel keeps its skip alive: the skip does not expire while
 * its call is still on the air, so the skipped call never takes the receiver. */
static int
test_skip_kept_alive(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("0", "0");
    set_clock_s(1100.0);
    follow(opts, st, 100, CH_HELD);
    rc |= expect_true("skip: armed", dsd_tg_policy_call_skip_arm(st, 200, 0, 0, 1100.0) == 0);
    for (int i = 1; i <= 20; i++) {
        set_clock_s(1100.0 + i);
        offer_update(opts, st, 200, CH_PRIO);
    }
    rc |= expect_true("skip: still active while announced", dsd_tg_policy_call_skip_active(st, 200, 1120.0));
    rc |= expect_true("skip: skipped call never preempts", following(100, FREQ_HELD));
    return rc;
}

/* Phase 2: a slot whose call ended longer ago than the hangtime keeps its assignment while the companion slot goes on,
 * but holds nothing a candidate must outrank. */
static int
test_tdma_ended_slot_not_held(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    st->p25_iden_tdma[1] = st->p25_iden_fdma[1];
    st->p25_iden_tdma[1].chan_type = 3;
    st->p25_chan_tdma_explicit[1] = 2;
    st->p2_wacn = 0xBEE00;
    st->p2_sysid = 0x1A2;
    st->p2_cc = 0x293;
    st->synctype = DSD_SYNC_P25P2_POS;
    (void)seed_row(st, 300, 100, 0);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1200.0);
    p25_sm_event_t grant0 = p25_sm_ev_group_grant(0x100A, 0, 300, 7300, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant0);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    p25_sm_event_t grant1 = p25_sm_ev_group_grant(0x100B, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant1);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    const long held_freq = p25_sm_get_ctx()->vc_freq_hz;
    rc |= expect_true("tdma: both slots followed on one carrier",
                      p25_sm_get_ctx()->slots[0].grant_active && p25_sm_get_ctx()->slots[1].grant_active);
    set_clock_s(1201.0);
    p25_sm_event_t end0 = p25_sm_ev_end_call_at(0, 300, 7300, 1201.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &end0);
    set_clock_s(1209.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 250, 0x1020);
    rc |= expect_true("tdma: the ended slot's priority 100 no longer blocks a priority 60 candidate",
                      p25_sm_get_ctx()->vc_tg == 250 && p25_sm_get_ctx()->vc_freq_hz != held_freq);
    return rc;
}

/* Phase 2: a slot whose conversation lapsed (ended, then silent for a hangtime while the companion slot kept the
 * carrier) starts a new conversation when the same talkgroup is assigned again: its dwell restarts. */
static int
test_tdma_lapsed_slot_restarts_dwell(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    st->p25_iden_tdma[1] = st->p25_iden_fdma[1];
    st->p25_iden_tdma[1].chan_type = 3;
    st->p25_chan_tdma_explicit[1] = 2;
    st->p2_wacn = 0xBEE00;
    st->p2_sysid = 0x1A2;
    st->p2_cc = 0x293;
    st->synctype = DSD_SYNC_P25P2_POS;
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1300.0);
    p25_sm_event_t grant0 = p25_sm_ev_group_grant(0x100A, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant0);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    p25_sm_event_t grant1 = p25_sm_ev_group_grant(0x100B, 0, 300, 7300, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant1);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    set_clock_s(1301.0);
    p25_sm_event_t end0 = p25_sm_ev_end_call_at(0, 100, 7100, 1301.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &end0);
    set_clock_s(1309.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    p25_sm_event_t regrant0 = p25_sm_ev_group_grant(0x100A, 0, 100, 7101, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &regrant0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    rc |= expect_true("tdma lapse: the slot's new conversation is followed from now",
                      p25_sm_get_ctx()->slots[0].followed_since_m > 1308.9);
    const long held = p25_sm_get_ctx()->vc_freq_hz;
    set_clock_s(1309.1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 200, 0x1020);
    rc |= expect_true("tdma lapse: the new conversation's dwell holds off a preemption",
                      p25_sm_get_ctx()->vc_freq_hz == held);
    return rc;
}

/* Phase 2: an assignment on the ended slot that never produces voice (a source-less update let through as the
 * stale-regrant guard's probe) holds the slot only for the acquisition window, not while the companion goes on. */
static int
test_tdma_pending_slot_bounded(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    st->p25_iden_tdma[1] = st->p25_iden_fdma[1];
    st->p25_iden_tdma[1].chan_type = 3;
    st->p25_chan_tdma_explicit[1] = 2;
    st->p2_wacn = 0xBEE00;
    st->p2_sysid = 0x1A2;
    st->p2_cc = 0x293;
    st->synctype = DSD_SYNC_P25P2_POS;
    (void)seed_row(st, 300, 100, 0);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1400.0);
    p25_sm_event_t grant0 = p25_sm_ev_group_grant(0x100A, 0, 300, 7300, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant0);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    p25_sm_event_t grant1 = p25_sm_ev_group_grant(0x100B, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant1);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    const long held_freq = p25_sm_get_ctx()->vc_freq_hz;
    set_clock_s(1401.0);
    p25_sm_event_t end0 = p25_sm_ev_end_call_at(0, 300, 7300, 1401.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &end0);
    set_clock_s(1403.5);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    p25_sm_event_t probe = p25_sm_ev_group_grant_update(0x100A, 0, 300, 0, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &probe);
    rc |= expect_true("pending: the update reassigned the ended slot",
                      p25_sm_get_ctx()->slots[0].grant_active && p25_sm_get_ctx()->slots[0].last_grant_m > 1403.0);
    set_clock_s(1410.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 250, 0x1020);
    rc |= expect_true("pending: a voiceless assignment no longer shields the companion's call",
                      p25_sm_get_ctx()->vc_tg == 250 && p25_sm_get_ctx()->vc_freq_hz != held_freq);
    return rc;
}

/* A conversation that resumed within its hangtime and then changes talker after a long transmission has not lapsed:
 * the stop before the resume is not its end, so dwell still runs from the first follow. */
static int
test_resumed_conversation_keeps_dwell(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    fresh_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1500.0);
    follow(opts, st, 100, CH_HELD);
    set_clock_s(1501.0);
    p25_sm_event_t tdu = p25_sm_ev_tdu();
    p25_sm_event(p25_sm_get_ctx(), opts, st, &tdu);
    set_clock_s(1501.5);
    p25_sm_event_t resume = p25_sm_ev_active_call(0, 100, 0, 7100, 1, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &resume);
    set_clock_s(1505.0);
    p25_sm_event_t talker = p25_sm_ev_ptt_call(0, 100, 0, 7999, 1, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &talker);
    rc |= expect_true("resumed: still following the conversation", following(100, FREQ_HELD));
    rc |= expect_true("resumed: follow time kept", p25_sm_get_ctx()->slots[0].followed_since_m < 1500.5);
    set_clock_s(1505.1);
    offer_update(opts, st, 200, CH_PRIO);
    rc |= expect_true("resumed: dwell from the first follow lets the priority call in", following(200, FREQ_PRIO));
    return rc;
}

static void
tdma_site(dsd_opts* opts, dsd_state* st) {
    fresh_site(opts, st);
    st->p25_iden_tdma[1] = st->p25_iden_fdma[1];
    st->p25_iden_tdma[1].chan_type = 3;
    st->p25_chan_tdma_explicit[1] = 2;
    st->p2_wacn = 0xBEE00;
    st->p2_sysid = 0x1A2;
    st->p2_cc = 0x293;
    st->synctype = DSD_SYNC_P25P2_POS;
}

/* Phase 2: a slot whose assignment was abandoned (its next talker's group is locked out) follows a later call there
 * anew, even of the talkgroup it held before. */
static int
test_tdma_abandoned_slot_restarts_dwell(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    tdma_site(opts, st);
    dsd_tg_policy_entry blocked;
    (void)dsd_tg_policy_make_exact_entry(400, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &blocked);
    (void)dsd_tg_policy_upsert_exact(st, &blocked, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1600.0);
    p25_sm_event_t grant0 = p25_sm_ev_group_grant(0x100A, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant0);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    p25_sm_event_t grant1 = p25_sm_ev_group_grant(0x100B, 0, 300, 7300, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant1);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    const long held = p25_sm_get_ctx()->vc_freq_hz;
    set_clock_s(1601.0);
    p25_sm_event_t locked = p25_sm_ev_ptt_call(0, 400, 0, 7400, 1, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &locked);
    rc |= expect_true("abandoned: the locked-out talker gave up the slot", !p25_sm_get_ctx()->slots[0].grant_active);
    set_clock_s(1610.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    p25_sm_event_t regrant0 = p25_sm_ev_group_grant(0x100A, 0, 100, 7101, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &regrant0);
    rc |= expect_true("abandoned: the call there is followed from now",
                      p25_sm_get_ctx()->slots[0].followed_since_m > 1609.9);
    set_clock_s(1610.1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 200, 0x1020);
    rc |= expect_true("abandoned: its dwell holds off a preemption", p25_sm_get_ctx()->vc_freq_hz == held);
    return rc;
}

/* Phase 2: a unit-to-unit update naming the held destination with a new source after the slot's conversation lapsed
 * is a new call taken as a repeat: it is followed from now. */
static int
test_tdma_lapsed_duplicate_restarts_dwell(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    tdma_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1700.0);
    p25_sm_event_t grant0 = p25_sm_ev_indiv_grant(0x100A, 0, 5001, 6001, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant0);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    p25_sm_event_t grant1 = p25_sm_ev_group_grant(0x100B, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &grant1);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    const long held = p25_sm_get_ctx()->vc_freq_hz;
    set_clock_s(1701.0);
    p25_sm_event_t end0 = p25_sm_ev_end_call_at(0, 0, 6001, 1701.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &end0);
    set_clock_s(1710.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    p25_sm_event_t update = p25_sm_ev_indiv_grant_update(0x100A, 0, 5001, 6002, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &update);
    rc |= expect_true("lapsed repeat: the new call is followed from now",
                      p25_sm_get_ctx()->slots[0].followed_since_m > 1709.9);
    set_clock_s(1710.1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 200, 0x1020);
    rc |= expect_true("lapsed repeat: its dwell holds off a preemption", p25_sm_get_ctx()->vc_freq_hz == held);
    return rc;
}

/* Phase 2: a call that moved to the other slot left its old slot's follow history behind; when its talkgroup is later
 * assigned to that slot again, after its conversation ended, it is followed anew. */
static int
test_tdma_vacated_slot_restarts_dwell(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    tdma_site(opts, st);
    set_dwell_cooldown_ms("750", "1000");
    set_clock_s(1800.0);
    p25_sm_event_t a_ts1 = p25_sm_ev_group_grant(0x100A, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &a_ts1);
    p25_sm_event_t voice0 = p25_sm_ev_active(0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice0);
    set_clock_s(1801.0);
    p25_sm_event_t a_ts2 = p25_sm_ev_group_grant(0x100B, 0, 100, 7100, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &a_ts2);
    rc |= expect_true("vacated: the call moved to TS2",
                      !p25_sm_get_ctx()->slots[0].grant_active && p25_sm_get_ctx()->slots[1].grant_active);
    p25_sm_event_t voice1 = p25_sm_ev_active(1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    set_clock_s(1802.0);
    p25_sm_event_t end1 = p25_sm_ev_end_call_at(1, 100, 7100, 1802.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &end1);
    set_clock_s(1803.0);
    p25_sm_event_t b_ts2 = p25_sm_ev_group_grant(0x100B, 0, 300, 7300, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &b_ts2);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    const long held = p25_sm_get_ctx()->vc_freq_hz;
    set_clock_s(1810.0);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    p25_sm_event_t a_again = p25_sm_ev_group_grant(0x100A, 0, 100, 7101, 0x00);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &a_again);
    rc |= expect_true("vacated: the talkgroup back on its old slot is followed from now",
                      p25_sm_get_ctx()->slots[0].followed_since_m > 1809.9);
    set_clock_s(1810.1);
    p25_sm_event(p25_sm_get_ctx(), opts, st, &voice1);
    offer_update(opts, st, 200, 0x1020);
    rc |= expect_true("vacated: its dwell holds off a preemption", p25_sm_get_ctx()->vc_freq_hz == held);
    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state st;
    dsd_decode_clock_use_test(1000000000ULL);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });
    rc |= test_phase2_update_preempts(&opts, &st);
    rc |= test_phase2_ranking(&opts, &st);
    rc |= test_phase1_link_control(&opts, &st);
    rc |= test_hangtime(&opts, &st);
    rc |= test_dwell_from_follow(&opts, &st);
    rc |= test_cooldown_survives_reload(&opts, &st);
    rc |= test_lockout_release_deferred(&opts, &st);
    rc |= test_refused_release_keeps_call(&opts, &st);
    rc |= test_skip_kept_alive(&opts, &st);
    rc |= test_tdma_ended_slot_not_held(&opts, &st);
    rc |= test_tdma_lapsed_slot_restarts_dwell(&opts, &st);
    rc |= test_tdma_pending_slot_bounded(&opts, &st);
    rc |= test_resumed_conversation_keeps_dwell(&opts, &st);
    rc |= test_tdma_abandoned_slot_restarts_dwell(&opts, &st);
    rc |= test_tdma_lapsed_duplicate_restarts_dwell(&opts, &st);
    rc |= test_tdma_vacated_slot_restarts_dwell(&opts, &st);
    dsd_state_ext_free_all(&st);
    dsd_decode_clock_use_system();
    return rc;
}
