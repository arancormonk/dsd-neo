// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR talkgroup priority and preemption: a Tier III grant for another carrier while the receiver holds a call is
 * followed only by Hold over a non-held call or by preemption over every held call, with the busy view taken from the
 * trunk state machine's own retention rule (live voice, hangtime tail, acquisition, pending receive check) and from
 * calls resident on a parked carrier. Driven through dmr_cspdu() grants on the decode clock's TEST source.
 */

#include <dsd-neo/protocol/dmr/dmr_utils_api.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* ---- Stubs for symbols dmr_csbk.c references ---- */

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation;
    (void)notice;
    (void)slot;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
rotate_symbol_out_file(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

uint8_t
crc8(uint8_t bits[], unsigned int len) {
    (void)bits;
    (void)len;
    return 0xFF;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

extern void dmr_cspdu(dsd_opts*, dsd_state*, uint8_t*, uint8_t*, uint32_t, uint32_t);

/* ---- Fixture ---- */

#define CC_HZ  851000000L
#define VC_A   852000000L
#define VC_B   853000000L
#define VC_C   854000000L
#define LPCN_A 0x0010U
#define LPCN_B 0x0011U
#define LPCN_C 0x0012U

static const uint64_t k_t0_ns = 1000000ULL * 1000000000ULL; // the TEST clock's origin, well past any zero sentinel

static int g_tunes = 0;
static dsd_trunk_tune_result g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static void
clock_at(double seconds) {
    dsd_decode_clock_test_set_ns(k_t0_ns + (uint64_t)(seconds * 1e9));
}

static dsd_trunk_tune_result
test_tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    if (g_tune_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_tune_result;
    }
    g_tunes++;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = dsd_decode_time();
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
test_return_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
init_env(dsd_opts* opts, dsd_state* state, double hangtime_s) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_hangtime = (float)hangtime_s;
    state->trunk_cc_freq = CC_HZ;
    state->trunk_chan_map[LPCN_A] = VC_A;
    state->trunk_chan_map[LPCN_B] = VC_B;
    state->trunk_chan_map[LPCN_C] = VC_C;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    g_tunes = 0;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    clock_at(0.0);
    dmr_sm_init(opts, state);
}

static void
seed_row(dsd_state* st, uint32_t id, const char* mode, int priority, int preempt) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_exact_entry(id, mode, "ROW", DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        abort();
    }
    row.priority = priority;
    row.preempt = preempt ? 1U : 0U;
    if (dsd_tg_policy_upsert_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) != 0) {
        abort();
    }
}

static void
write_bits_u32(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> ((nbits - 1U) - i)) & 1U);
    }
}

/* A CRC-valid Tier III talkgroup voice grant (TV_GRANT) through the CSBK layer, which judges its policy. */
static void
tv_grant(dsd_opts* opts, dsd_state* st, uint16_t lpcn, uint8_t slot, uint32_t tg, uint32_t src) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 49U;
    write_bits_u32(bits, 16U, lpcn, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, tg, 24U);
    write_bits_u32(bits, 56U, src, 24U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
}

/* Accepted voice on @p slot: its own voice passed the colour-code gate just now. */
static void
voice(dsd_opts* opts, dsd_state* st, int slot) {
    st->dmr_confidence_voice_open[slot] = 1;
    st->dmr_confidence_voice_m[slot] = dsd_decode_now_mono_s();
    dmr_sm_emit_voice_sync(opts, st, slot);
}

static long
vc(const dsd_state* st) {
    return st->trunk_vc_freq[0];
}

static dmr_sm_ctx_t*
sm(void) {
    return dmr_sm_get_ctx();
}

/* ---- Cases ---- */

/* 1, 2: live voice holds the carrier against a grant without preemption; an outranking preempt candidate takes it
   after its dwell, and its takeover's cooldown keeps a third candidate out until it runs out. */
static int
test_live_voice_and_cooldown(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 200, "A", 50, 0);
    seed_row(st, 300, "A", 60, 1);
    seed_row(st, 400, "A", 80, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    int rc = expect_true("A followed", vc(st) == VC_A && g_tunes == 1);
    clock_at(1.0);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("live voice: a grant without preempt is busy", vc(st) == VC_A && g_tunes == 1);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("p60 preempt displaces p10 after its dwell", vc(st) == VC_B && sm()->vc_tg == 300);
    clock_at(1.8);
    tv_grant(opts, st, LPCN_C, 0, 400, 4);
    rc |= expect_true("p80 preempt inside the takeover's cooldown is refused", vc(st) == VC_B && sm()->vc_tg == 300);
    clock_at(2.1);
    tv_grant(opts, st, LPCN_C, 0, 400, 4);
    rc |= expect_true("p80 preempt past the cooldown is followed", vc(st) == VC_C && sm()->vc_tg == 400);
    return rc;
}

/* 3: dwell runs from the held call's grant. */
static int
test_dwell(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.4);
    voice(opts, st, 0);
    clock_at(0.5);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    int rc = expect_true("preempt within the dwell is refused", vc(st) == VC_A);
    clock_at(0.8);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("the same preempt past the dwell is followed", vc(st) == VC_B);
    return rc;
}

/* 4: with zero hangtime only live voice holds the carrier (voice 0.2 s stale, no tick needed). */
static int
test_zero_hangtime(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 0.0);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    clock_at(1.1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    int rc = expect_true("zero hangtime: live voice is busy", vc(st) == VC_A);
    clock_at(1.3);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("zero hangtime: voice 0.3 s old is not busy", vc(st) == VC_B);
    return rc;
}

/* 5: before its first voice the followed grant holds the carrier for the grant timeout. */
static int
test_acquisition(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    int rc = expect_true("acquisition: another grant is busy", vc(st) == VC_A);
    clock_at(4.1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("past the grant timeout the grant is followed", vc(st) == VC_B);
    return rc;
}

/* 6: the hangtime tail holds the carrier; an outranking preempt passes with its dwell measured from the grant. */
static int
test_tail(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.3);
    voice(opts, st, 0);
    clock_at(1.0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    int rc = expect_true("tail: a grant without preempt is busy", vc(st) == VC_A);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("tail: an outranking preempt is followed (dwell from the grant)", vc(st) == VC_B);
    return rc;
}

/* 7, 8: a call heard on the control channel's carrier (composite TSCC plus payload) holds the receiver while its
   identity is admitted; a blocked one never makes it busy. */
static int
test_resident(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    clock_at(1.0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    int rc = expect_true("resident TS2 call: a grant elsewhere is busy", g_tunes == 0 && opts->trunk_is_tuned == 0);
    clock_at(3.1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("resident call past its hangtime: followed", vc(st) == VC_B);

    init_env(opts, st, 2.0);
    seed_row(st, 100, "B", 0, 0);
    clock_at(1.0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a resident call the policy blocks is not busy", vc(st) == VC_B);
    return rc;
}

/* A call heard on the control channel's carrier (composite TSCC plus payload) holds the receiver only while it lasts:
   once a verified terminator ended it, a new call there that no LC names (here past the old call's hangtime) is not
   the ended one, so it never makes the receiver busy, and a grant elsewhere is followed. */
static int
test_resident_identity_ends_with_its_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    clock_at(1.0);
    const dsd_call_observation named = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
        .ota_source_id = 1U,
    };
    (void)dsd_call_state_observe(st, &named, DSD_CALL_BOUNDARY_BEGIN);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    dmr_sm_busy_view_t view;
    int rc = expect_true("the resident call holds the receiver",
                         dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 1);
    clock_at(1.5);
    (void)dsd_call_state_end_ex(st, 1U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    clock_at(3.5);
    const dsd_call_observation anonymous = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(st, &anonymous, DSD_CALL_BOUNDARY_BEGIN);
    voice(opts, st, 1);
    rc |= expect_true("a new call no LC names is not the ended resident call",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 0
                          && sm()->slots[1].prov == (uint8_t)DMR_SM_ID_NONE);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a grant elsewhere is followed", vc(st) == VC_B && g_tunes == 1);
    return rc;
}

/* A resident call on a slot the receive policy does not admit never holds the receiver: with a slot-1-only filter,
   validated slot-2 traffic on the parked carrier leaves an eligible grant elsewhere free to be followed. */
static int
test_resident_receive_admission(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    int rc = expect_true("slot filter parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    clock_at(1.0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    dmr_sm_busy_view_t view;
    rc |= expect_true("an excluded slot's resident call is not held",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("an eligible slot-1 grant elsewhere is followed", vc(st) == VC_B && g_tunes == 1);

    /* The admitted slot's resident call still holds it. */
    init_env(opts, st, 2.0);
    rc |= expect_true("slot filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    clock_at(1.0);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("an admitted slot's resident call is busy", g_tunes == 0 && opts->trunk_is_tuned == 0);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* On the followed carrier a slot a receive-slot edit stopped admitting holds nothing, its hangtime tail included: a
   rejected p90 companion in its tail never blocks a p50 candidate over the admitted p10 call, and when only rejected
   traffic is left in its tail (the followed grant's own slot rejected) the carrier is not held. */
static int
test_tuned_tail_receive_admission(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 500, "A", 90, 0);
    seed_row(st, 300, "A", 50, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 500, 5);
    int rc = expect_true("slot filter parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    clock_at(1.8);
    voice(opts, st, 0);
    dmr_sm_busy_view_t view;
    rc |= expect_true("the rejected slot's tail is no held call",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 1
                          && view.members == 0x1);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("p50 outranks the admitted p10 call", vc(st) == VC_B && sm()->vc_tg == 300);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));

    init_env(opts, st, 2.0);
    seed_row(st, 500, "A", 90, 0);
    tv_grant(opts, st, LPCN_A, 1, 500, 5);
    clock_at(1.0);
    voice(opts, st, 1);
    rc |= expect_true("slot filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    clock_at(1.5);
    rc |= expect_true("only a rejected slot's tail: not held",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("an ordinary grant is followed", vc(st) == VC_B && sm()->vc_tg == 200);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* Before its first voice a followed grant whose own slot a receive-slot edit stopped admitting holds nothing: its
   identity no longer stands for the carrier, so an ordinary grant elsewhere is followed. A grant slot merely awaiting
   its colour-code check (a DCC filter, the carrier's code not verified yet) still holds the carrier. */
static int
test_acquisition_receive_admission(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 500, "A", 90, 0);
    tv_grant(opts, st, LPCN_A, 1, 500, 5);
    int rc = expect_true("the TS2 grant is followed", vc(st) == VC_A && g_tunes == 1);
    clock_at(0.5);
    rc |= expect_true("slot filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    dmr_sm_busy_view_t view;
    rc |= expect_true("a rejected grant slot's acquisition is not held",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 0);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("an ordinary grant elsewhere is followed", vc(st) == VC_B && sm()->vc_tg == 200);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));

    init_env(opts, st, 2.0);
    seed_row(st, 500, "A", 90, 0);
    rc |= expect_true("dcc filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "7", "", "", NULL, 0) == 0);
    tv_grant(opts, st, LPCN_A, 1, 500, 5);
    rc |= expect_true("the TS2 grant is followed under a pending check", vc(st) == VC_A && g_tunes == 1);
    clock_at(0.5);
    rc |= expect_true("a grant slot awaiting its colour-code check holds the carrier",
                      dmr_sm_describe_held_calls(sm(), opts, st, dsd_decode_now_mono_s(), &view) == 1
                          && view.members == 0x2);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("an ordinary grant elsewhere is busy", vc(st) == VC_A && g_tunes == 1);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* 9: Hold takes the receiver from a non-held call, not from a carrier whose companion slot carries the Hold group. */
static int
test_hold(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    st->tg_hold = 200;
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    int rc = expect_true("hold over a non-held call is followed", vc(st) == VC_B && sm()->vc_tg == 200);

    /* A Hold takeover is no preemption: it stamps no cooldown, even for a group that asks to preempt, so an outranking
       candidate past its dwell is followed. */
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 200, "A", 60, 1);
    seed_row(st, 400, "A", 80, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.2);
    voice(opts, st, 0);
    st->tg_hold = 200;
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("hold takeover of a preempt group", vc(st) == VC_B && sm()->vc_tg == 200);
    st->tg_hold = 0;
    clock_at(1.0);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_C, 0, 400, 4);
    rc |= expect_true("a hold takeover leaves no cooldown", vc(st) == VC_C && sm()->vc_tg == 400);

    init_env(opts, st, 2.0);
    st->tg_hold = 200;
    tv_grant(opts, st, LPCN_A, 0, 200, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 200, 9);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a held carrier carrying the Hold group keeps it", vc(st) == VC_A);
    return rc;
}

/* 10, 11: a cross-carrier candidate must outrank both held slots; an unknown companion is displaced only by Hold. */
static int
test_both_slots(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 500, "A", 90, 0);
    seed_row(st, 300, "A", 50, 1);
    seed_row(st, 310, "A", 95, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 500, 5);
    clock_at(1.8);
    voice(opts, st, 0);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    int rc = expect_true("p50 does not outrank the p90 companion", vc(st) == VC_A);
    tv_grant(opts, st, LPCN_B, 0, 310, 3);
    rc |= expect_true("p95 outranks both slots", vc(st) == VC_B && sm()->vc_tg == 310);

    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 320, "A", 100, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    voice(opts, st, 1); // the companion's call has no identity yet
    tv_grant(opts, st, LPCN_B, 0, 320, 3);
    rc |= expect_true("an unknown companion is not preempted", vc(st) == VC_A);
    st->tg_hold = 330;
    tv_grant(opts, st, LPCN_B, 0, 330, 3);
    rc |= expect_true("an unknown companion yields to Hold", vc(st) == VC_B && sm()->vc_tg == 330);
    return rc;
}

/* 12: a preemption whose tune is not accepted keeps the held call and stamps no cooldown; the retry is followed. */
static int
test_deferred_preemption(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    int rc = expect_true("deferred preemption keeps the held call", vc(st) == VC_A && sm()->vc_tg == 100);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    clock_at(1.1);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("the retry is followed (no cooldown from the deferred attempt)", vc(st) == VC_B);
    return rc;
}

/* 13: a decision-less grant (dmr_sm_emit_group_grant()) is never a hold or preempt candidate. */
static int
test_decision_less(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    st->tg_hold = 200;
    dmr_sm_emit_group_grant(opts, st, VC_A, LPCN_A, 100, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    dmr_sm_emit_group_grant(opts, st, VC_B, LPCN_B, 200, 2);
    int rc = expect_true("decision-less grant is busy while held", vc(st) == VC_A);
    clock_at(3.0);
    dmr_sm_emit_group_grant(opts, st, VC_B, LPCN_B, 200, 2);
    rc |= expect_true("decision-less grant is followed once nothing is held", vc(st) == VC_B);
    return rc;
}

/* A carrier whose colour code is still being checked (the receive policy cannot admit it yet) is held while the tick's
   receive window runs, even past the acquisition window: an ordinary grant is busy. A carrier whose verified code the
   policy rejects is not held. */
static int
test_receive_check_is_acquisition(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    opts->frame_dmr = 1; // the DMR tick runs only while DMR trunking can recover
    int rc = expect_true("dcc filter parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_DCC, 1, "", "7", "", NULL, 0) == 0);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    rc |= expect_true("followed while the code is unknown", vc(st) == VC_A);
    st->dmr_color_code = 16U; // unknown, as the voice-channel tune leaves it
    clock_at(3.0);
    dmr_sm_tick_ctx(sm(), opts, st); // opens the receive window
    rc |= expect_true("the receive window runs", sm()->state == DMR_SM_TUNED && sm()->receive_check_started_m > 0.0);
    clock_at(4.5);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a pending receive check keeps the carrier past the acquisition window", vc(st) == VC_A);
    sm()->receive_carriers.entries[0] = (dsd_receive_carrier){.freq_hz = VC_A, .value = 7, .kind = DSD_RECEIVE_DCC};
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a carrier whose verified code is rejected is not held", vc(st) == VC_B);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* Following a call into a ledger a stale record still occupies stamps no cooldown: only a takeover does. */
static int
test_follow_stamps_no_false_cooldown(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 300, "A", 60, 1);
    seed_row(st, 400, "A", 80, 1);
    const dsd_tg_policy_call_route stale_route = {.target_id = 77, .freq_hz = VC_C, .channel = 3, .slot = 0};
    dsd_tg_policy_decision stale = {0};
    stale.priority = 0;
    (void)dsd_tg_policy_note_active_call(st, &stale_route, &stale, dsd_decode_now_mono_s());
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    int rc = expect_true("idle follow of a preempt group", vc(st) == VC_A);
    clock_at(0.8);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 400, 4);
    rc |= expect_true("no false cooldown: an outranking preempt past the dwell is followed", vc(st) == VC_B);
    return rc;
}

/* A release clears the ledger: nothing is left for a later candidate to preempt. */
static int
test_release_clears_ledger(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(1.0);
    voice(opts, st, 0);
    dmr_sm_abandon_carrier(sm(), opts, st, "test");
    dsd_tg_policy_decision d;
    (void)dsd_tg_policy_evaluate_group_call(opts, st, 300, 3, 0, 0, &d);
    const dsd_tg_policy_call_route candidate = {
        .target_id = 300, .freq_hz = VC_B, .channel = LPCN_B, .slot = 0, .requires_tuner_retune = 1};
    return expect_true("released carrier leaves nothing to preempt",
                       dsd_tg_policy_should_preempt(opts, st, &candidate, &d, dsd_decode_now_mono_s()) == 0);
}

/* A grant assigning another call to the followed slot of the carrier already tuned (IN_PLACE) names that slot's call
   anew, its follow time now; a repeat of that call keeps the follow time. */
static int
test_in_place_reassignment(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 100, 0);
    seed_row(st, 10, "A", 10, 0);
    seed_row(st, 50, "A", 50, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    clock_at(1.0);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_A, 0, 10, 2);
    int rc = expect_true("in-place reassignment names the slot's new call",
                         vc(st) == VC_A && g_tunes == 1 && sm()->slots[0].target == 10U);
    clock_at(1.5);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 50, 3);
    rc |= expect_true("the reassigned call's dwell runs from its assignment", vc(st) == VC_A);
    clock_at(1.6);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_A, 0, 10, 2); // a repeat of the call now on the slot
    rc |= expect_true("a repeat keeps the slot's call", sm()->slots[0].target == 10U);
    clock_at(1.8);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 50, 3);
    rc |= expect_true("a p50 preempt outranks the p10 reassignment (a repeat kept its follow time)",
                      vc(st) == VC_B && sm()->vc_tg == 50);

    /* A repeat of the grant the machine followed is no reassignment, though a checked LC named the call otherwise. */
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 202, 2);
    clock_at(0.6);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    rc |= expect_true("a repeat of the followed grant keeps the LC's identity",
                      sm()->slots[0].target == 202U && sm()->slots[0].prov == (uint8_t)DMR_SM_ID_LC);
    return rc;
}

/* An in-place group grant for TG 300 arrives while the followed slot still carries a private call to unit 300 (a checked
   LC names it): the private call is not the group's, whatever the number, so the grant's record awaits its own call
   instead of binding to the private one. Once the private call ends, the group's unnamed voice is that call: it stays
   known (group 300, p10, its dwell from the grant), so a p100 preempt elsewhere takes the receiver after the dwell. */
static int
test_in_place_group_grant_never_binds_private_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 300, "A", 10, 0);
    seed_row(st, 999, "A", 100, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.3);
    const dsd_call_observation private_call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = 300U,
        .policy_target_id = 300U,
        .ota_source_id = 7U,
    };
    voice(opts, st, 0);
    (void)dsd_call_state_observe(st, &private_call, DSD_CALL_BOUNDARY_BEGIN);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_PRIVATE_VOICE, 300, 7);
    dsd_call_snapshot call;
    const uint64_t private_epoch = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;

    clock_at(0.5);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    int rc = expect_true("the in-place group grant names the slot",
                         vc(st) == VC_A && sm()->slots[0].target == 300U
                             && sm()->slots[0].kind == (uint8_t)DSD_CALL_KIND_GROUP_VOICE);
    rc |= expect_true("the group grant is not bound to the private call",
                      private_epoch != 0U && sm()->slots[0].call_epoch != private_epoch);

    clock_at(1.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    clock_at(1.2);
    const dsd_call_observation unnamed = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(st, &unnamed, DSD_CALL_BOUNDARY_BEGIN);
    voice(opts, st, 0);
    rc |= expect_true("the group's unnamed voice is the grant's call, still known",
                      sm()->slots[0].target == 300U && sm()->slots[0].kind == (uint8_t)DSD_CALL_KIND_GROUP_VOICE
                          && sm()->vc_identity_known == 1);
    clock_at(1.5);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 999, 9);
    rc |= expect_true("a p100 preempt takes the p10 group call after the dwell", vc(st) == VC_B && sm()->vc_tg == 999);
    return rc;
}

/* The CRC provenance of slot 0's active call (0xFF: none active). */
static uint8_t
slot0_call_crc(const dsd_state* st, uint32_t* target) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(st, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0xFFU;
    }
    *target = (uint32_t)call.policy_target_id;
    return call.crc_invalid;
}

/* A grant that passed its CRC is followed; before its first voice, a RAS-accepted grant (-F) reassigns the followed
   slot in place. Its identity is published at the first voice with its own provenance, never the verified grant's: the
   call reads RAS-unverified. And the reverse: a verified reassignment of a RAS grant reads verified. */
static int
test_in_place_reassignment_carries_crc_provenance(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int variant = 0; variant < 2; variant++) {
        const uint8_t first = variant == 0 ? 0U : (uint8_t)DSD_CRC_RAS_UNVERIFIED;
        const uint8_t second = variant == 0 ? (uint8_t)DSD_CRC_RAS_UNVERIFIED : 0U;
        init_env(opts, st, 2.0);
        st->currentslot = 0;
        st->event_crc_invalid[0] = first;
        tv_grant(opts, st, LPCN_A, 0, 100, 1);
        st->event_crc_invalid[0] = 0U;
        clock_at(0.2);
        st->event_crc_invalid[0] = second;
        tv_grant(opts, st, LPCN_A, 0, 200, 2);
        st->event_crc_invalid[0] = 0U;
        rc |= expect_true("the reassignment carries its own provenance",
                          vc(st) == VC_A && sm()->vc_tg == 200 && sm()->vc_crc_flags == second);
        clock_at(0.3);
        voice(opts, st, 0);
        uint32_t target = 0U;
        const uint8_t crc = slot0_call_crc(st, &target);
        rc |= expect_true(variant == 0 ? "the RAS reassignment publishes as unverified"
                                       : "the verified reassignment publishes as verified",
                          target == 200U && crc == second);
    }
    return rc;
}

/* A C_MOVE event, as the CSBK layer submits it. */
static dmr_sm_grant_result_e
submit_move(dsd_opts* opts, dsd_state* st, long freq, uint16_t lpcn, int slot, int is_group, uint32_t target,
            uint32_t src) {
    dmr_sm_event_t ev;
    DSD_MEMSET(&ev, 0, sizeof(ev));
    ev.type = DMR_SM_EV_GRANT;
    ev.slot = slot;
    ev.freq_hz = freq;
    ev.lpcn = lpcn;
    ev.tg = is_group ? (int)target : 0;
    ev.dst = is_group ? 0 : (int)target;
    ev.src = (int)src;
    ev.is_group = is_group;
    ev.kind = is_group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
    ev.identity_known = 1;
    ev.origin = DMR_SM_ORIGIN_MOVE;
    ev.policy_valid = 1;
    if (is_group) {
        (void)dsd_tg_policy_evaluate_group_call(opts, st, target, src, 0, 0, &ev.policy);
    } else {
        (void)dsd_tg_policy_evaluate_private_call(opts, st, src, target, 0, 0, &ev.policy);
    }
    return dmr_sm_submit_grant(opts, st, &ev);
}

/* A matched C_MOVE whose tune is deferred changes nothing: a repeat of the followed grant still matches it, so the
   identity a checked LC gave the slot and its follow time stay. */
static int
test_deferred_move_keeps_identity(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 202, 2);
    const double follow_m = sm()->slots[0].follow_m;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    int rc = expect_true("a deferred move of the followed call",
                         submit_move(opts, st, VC_B, LPCN_B, 0, 1, 202, 2) == DMR_SM_GRANT_DEFERRED && vc(st) == VC_A);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    rc |= expect_true("the deferred move left the grant identity", sm()->vc_tg == 101 && sm()->vc_is_group == 1);
    clock_at(0.6);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    rc |= expect_true("a repeat of the followed grant keeps the LC's identity and follow time",
                      sm()->slots[0].target == 202U && sm()->slots[0].prov == (uint8_t)DMR_SM_ID_LC
                          && fabs(sm()->slots[0].follow_m - follow_m) < 1e-9);
    return rc;
}

/* A same-carrier C_MOVE of a call whose identity was already published moves the publication to the new slot: its
   first voice there names the call, and the move keeps the follow time. */
static int
test_slot_move_republishes(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 7);
    clock_at(0.3);
    voice(opts, st, 0);
    dsd_call_snapshot call;
    int rc = expect_true("the grant's identity is published on its slot",
                         dsd_call_state_get(st, 0U, &call) > 0 && call.ota_target_id == 100U);
    clock_at(0.4);
    rc |= expect_true("same-carrier move is in place",
                      submit_move(opts, st, VC_A, LPCN_A, 1, 1, 100, 7) == DMR_SM_GRANT_IN_PLACE && sm()->vc_slot == 1);
    rc |= expect_true("the vacated slot's canonical call ended", dsd_call_state_get(st, 0U, &call) > 0
                                                                     && call.ota_target_id == 100U
                                                                     && call.phase == DSD_CALL_PHASE_ENDED);
    clock_at(0.5);
    voice(opts, st, 1);
    rc |= expect_true("the moved call's identity is published on its new slot",
                      dsd_call_state_get(st, 1U, &call) > 0 && call.ota_target_id == 100U
                          && call.phase == DSD_CALL_PHASE_ACTIVE);
    rc |= expect_true("one ACTIVE snapshot of the moved call",
                      dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    clock_at(0.8);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("the move kept the follow time", vc(st) == VC_B);
    return rc;
}

/* A C_MOVE is the followed call's only when it names the call the followed slot carries (either party of a private
   call): a checked LC that renamed the call makes the original grant's talkgroup another call. */
static int
test_move_matches_slot_identity(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 101, "A", 10, 0);
    seed_row(st, 202, "A", 10, 0);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 202, 2);
    int rc = expect_true("a move of the grant's renamed talkgroup is another call's (busy)",
                         submit_move(opts, st, VC_B, LPCN_B, 0, 1, 101, 1) == DMR_SM_GRANT_BUSY && vc(st) == VC_A);
    rc |= expect_true("the followed slot keeps its call", sm()->slots[0].target == 202U);
    rc |= expect_true("a move of the call the slot carries is followed",
                      submit_move(opts, st, VC_B, LPCN_B, 0, 1, 202, 2) == DMR_SM_GRANT_FOLLOWED && vc(st) == VC_B
                          && sm()->slots[0].target == 202U);

    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 101, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_PRIVATE_VOICE, 7001, 7002);
    rc |= expect_true("a move naming the private call's source is the followed call's",
                      submit_move(opts, st, VC_B, LPCN_B, 0, 0, 7002, 7001) == DMR_SM_GRANT_FOLLOWED && vc(st) == VC_B
                          && sm()->slots[0].target == 7001U);
    return rc;
}

/* A call on @p slot as the canonical layer opens it: a voice LC naming group @p tg (0: anonymous late-entry voice). */
static void
canonical_call_begins(dsd_state* st, int slot, uint32_t tg, uint32_t src) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = (uint8_t)slot,
        .kind = tg != 0U ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_VOICE,
        .ota_target_id = tg,
        .policy_target_id = tg,
        .ota_source_id = src,
    };
    (void)dsd_call_state_observe(st, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

/* Whether @p slot's canonical call is ACTIVE naming group @p tg, and the slot's record is bound to it. */
static int
slot_publishes_bound(const dsd_state* st, int slot, uint32_t tg) {
    dsd_call_snapshot call;
    return dsd_call_state_get(st, (uint8_t)slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && call.ota_target_id == tg && sm()->slots[slot].target == tg
           && sm()->slots[slot].call_epoch == dsd_call_snapshot_transmission(&call);
}

/* The followed grant named call A; a checked LC then named another call, B, on the followed slot (A's end withdrew the
   grant's identity there). A matched move of B republishes B where it went: its first voice on the new carrier, or on
   the new slot of the same carrier, names B. */
static int
test_move_republishes_renamed_call(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int in_place = 0; in_place < 2; in_place++) {
        init_env(opts, st, 2.0);
        tv_grant(opts, st, LPCN_A, 0, 100, 1);
        clock_at(0.3);
        voice(opts, st, 0);
        clock_at(0.5);
        canonical_call_begins(st, 0, 200, 2);
        dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 200, 2);
        rc |= expect_true("B's LC names the followed slot, the grant's identity withdrawn",
                          sm()->slots[0].target == 200U && sm()->vc_identity_known == 0);
        clock_at(0.6);
        const int new_slot = in_place ? 1 : 0;
        const dmr_sm_grant_result_e moved = in_place ? submit_move(opts, st, VC_A, LPCN_A, 1, 1, 200, 2)
                                                     : submit_move(opts, st, VC_B, LPCN_B, 0, 1, 200, 2);
        rc |=
            expect_true("the move of B is followed", moved == (in_place ? DMR_SM_GRANT_IN_PLACE : DMR_SM_GRANT_FOLLOWED)
                                                         && sm()->vc_tg == 200 && sm()->vc_identity_known == 1);
        clock_at(0.7);
        voice(opts, st, new_slot);
        rc |= expect_true(in_place ? "B's first voice on its new slot publishes B"
                                   : "B's first voice on the new carrier publishes B",
                          slot_publishes_bound(st, new_slot, 200U));
    }
    return rc;
}

/* A matched move to another carrier ends the moved call's epoch on the carrier left (as any follow does), so its first
   voice on the new carrier publishes it in an epoch of its own and the moved identity binds to that call. When the
   moved call ends, the next, anonymous call on the slot is unknown (a gap record): it yields only to Hold, never to a
   preempting call that merely outranks the moved call. */
static int
test_move_to_carrier_binds_moved_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 60, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.3);
    voice(opts, st, 0);
    dsd_call_snapshot call;
    int rc = expect_true("the followed call is published", slot_publishes_bound(st, 0, 100U));
    const uint64_t left_epoch = dsd_call_state_get(st, 0U, &call) > 0 ? call.epoch : 0U;
    clock_at(0.4);
    rc |= expect_true("the move to another carrier is followed",
                      submit_move(opts, st, VC_B, LPCN_B, 0, 1, 100, 1) == DMR_SM_GRANT_FOLLOWED && vc(st) == VC_B);
    rc |= expect_true("the moved call's epoch on the carrier left ended",
                      dsd_call_state_get(st, 0U, &call) > 0 && call.epoch == left_epoch
                          && call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    clock_at(0.5);
    voice(opts, st, 0);
    rc |= expect_true("the moved call is published in an epoch of its own, which its identity binds to",
                      slot_publishes_bound(st, 0, 100U) && dsd_call_state_get(st, 0U, &call) > 0
                          && call.epoch != left_epoch);
    clock_at(1.0);
    (void)dsd_call_state_end_ex(st, 0U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    clock_at(1.2);
    canonical_call_begins(st, 0, 0U, 0U);
    voice(opts, st, 0);
    clock_at(1.3);
    tv_grant(opts, st, LPCN_C, 0, 300, 3);
    rc |= expect_true("the anonymous call after the moved one yields only to Hold", vc(st) == VC_B && g_tunes == 2);
    return rc;
}

/* A Tier III grant for the carrier the receiver already listens on (the composite control channel's own payload) while
   resident calls play on it is adopted in place: the receiver does not leave the carrier, so neither its granted call
   nor the companion call ends, and both keep their identities, bindings and follow times. A preempting call is then
   judged against the dwell from when the calls were first named, and taking it (another carrier) ends both calls. */
static int
test_composite_grant_adopts_resident_calls(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    st->trunk_chan_map[0x0020U] = CC_HZ;
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 500, "A", 20, 0);
    seed_row(st, 300, "A", 60, 1);
    clock_at(1.0);
    canonical_call_begins(st, 1, 100U, 1U);
    canonical_call_begins(st, 0, 500U, 5U);
    voice(opts, st, 1);
    voice(opts, st, 0);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 500, 5);
    dsd_call_snapshot call;
    const uint64_t epoch1 = dsd_call_state_get(st, 1U, &call) > 0 ? call.epoch : 0U;
    const uint64_t epoch0 = dsd_call_state_get(st, 0U, &call) > 0 ? call.epoch : 0U;
    const double follow1 = sm()->slots[1].follow_m;
    const double follow0 = sm()->slots[0].follow_m;
    clock_at(1.5);
    voice(opts, st, 1);
    voice(opts, st, 0);
    tv_grant(opts, st, 0x0020U, 1, 100, 1); // the grant of the call playing on TS2 of this carrier
    int rc = expect_true("the composite carrier's grant is followed in place",
                         sm()->state == DMR_SM_TUNED && sm()->vc_freq_hz == CC_HZ && sm()->vc_slot == 1);
    rc |=
        expect_true("the granted call goes on as the same call",
                    slot_publishes_bound(st, 1, 100U) && dsd_call_state_get(st, 1U, &call) > 0 && call.epoch == epoch1);
    rc |=
        expect_true("the companion call goes on as the same call",
                    slot_publishes_bound(st, 0, 500U) && dsd_call_state_get(st, 0U, &call) > 0 && call.epoch == epoch0);
    rc |= expect_true("both calls keep their LC identities and follow times",
                      sm()->slots[1].prov == (uint8_t)DMR_SM_ID_LC && sm()->slots[0].prov == (uint8_t)DMR_SM_ID_LC
                          && fabs(sm()->slots[1].follow_m - follow1) < 1e-9
                          && fabs(sm()->slots[0].follow_m - follow0) < 1e-9);
    clock_at(1.8);
    voice(opts, st, 1);
    voice(opts, st, 0);
    tv_grant(opts, st, LPCN_B, 0, 300, 3);
    rc |= expect_true("a p60 preempt past the calls' dwell (from their first naming) is followed",
                      vc(st) == VC_B && sm()->vc_tg == 300);
    rc |= expect_true("leaving the carrier ends both calls",
                      dsd_call_state_get(st, 1U, &call) > 0 && call.epoch == epoch1
                          && call.phase == DSD_CALL_PHASE_ENDED && dsd_call_state_get(st, 0U, &call) > 0
                          && call.epoch == epoch0 && call.phase == DSD_CALL_PHASE_ENDED);
    return rc;
}

/* Slot identities live in the machine's context, which trunk scan keeps per target: each target's context holds its own,
   and the acquisition a target switch starts (dmr_sm_begin_cc_acquisition()) or an abandoned carrier clears them. */
static dmr_sm_ctx_t g_scan_ctx[2];
static int g_scan_active = 0;

static void*
scan_dmr_ctx(void) {
    return &g_scan_ctx[g_scan_active];
}

static int
test_scan_target_contexts(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    dmr_sm_init_ctx(&g_scan_ctx[0], opts, st);
    dmr_sm_init_ctx(&g_scan_ctx[1], opts, st);
    dsd_trunk_scan_hooks hooks = {0};
    hooks.dmr_ctx = scan_dmr_ctx;
    dsd_trunk_scan_hooks_set(&hooks);
    opts->trunk_scan_enabled = 1;
    g_scan_active = 0;
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    clock_at(0.5);
    voice(opts, st, 0);
    int rc = expect_true("target A follows with a slot identity",
                         g_scan_ctx[0].state == DMR_SM_TUNED && g_scan_ctx[0].slots[0].target == 100U
                             && g_scan_ctx[0].slots[0].prov == (uint8_t)DMR_SM_ID_GRANT);
    rc |= expect_true("target B's context holds nothing", g_scan_ctx[1].slots[0].target == 0U);
    // Rotation to B: the switch restarts B's acquisition; A's context keeps its call while parked elsewhere.
    g_scan_active = 1;
    opts->trunk_is_tuned = 0;
    dmr_sm_begin_cc_acquisition(&g_scan_ctx[1], opts, st, CC_HZ, 0U);
    dmr_sm_busy_view_t view;
    rc |= expect_true("target B is not held by A's call",
                      dmr_sm_describe_held_calls(&g_scan_ctx[1], opts, st, dsd_decode_now_mono_s(), &view) == 0);
    rc |= expect_true("A's identity survives the rotation", g_scan_ctx[0].slots[0].target == 100U);
    // Back to A: its revisit starts a new acquisition, which forgets the call.
    g_scan_active = 0;
    dmr_sm_begin_cc_acquisition(&g_scan_ctx[0], opts, st, CC_HZ, 0U);
    rc |= expect_true("A's revisit forgets the slot identity",
                      g_scan_ctx[0].slots[0].target == 0U && g_scan_ctx[0].slots[0].prov == (uint8_t)DMR_SM_ID_NONE);
    // An abandoned carrier (a visit limit) clears them too.
    opts->trunk_is_tuned = 0;
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    dmr_sm_abandon_carrier(&g_scan_ctx[0], opts, st, "scan-visit-limit");
    rc |= expect_true("an abandoned carrier forgets the slot identity", g_scan_ctx[0].slots[0].target == 0U);
    dsd_trunk_scan_hooks_set(NULL);
    opts->trunk_scan_enabled = 0;
    return rc;
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(opts);
        free(st);
        return 1;
    }
    dsd_decode_clock_use_test(k_t0_ns);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = test_tune_request,
        .tune_to_cc_request = test_tune_request,
        .return_to_cc_request = test_return_request,
    });

    int rc = 0;
    rc |= test_live_voice_and_cooldown(opts, st);
    rc |= test_dwell(opts, st);
    rc |= test_zero_hangtime(opts, st);
    rc |= test_acquisition(opts, st);
    rc |= test_tail(opts, st);
    rc |= test_resident(opts, st);
    rc |= test_resident_identity_ends_with_its_call(opts, st);
    rc |= test_resident_receive_admission(opts, st);
    rc |= test_tuned_tail_receive_admission(opts, st);
    rc |= test_acquisition_receive_admission(opts, st);
    rc |= test_hold(opts, st);
    rc |= test_both_slots(opts, st);
    rc |= test_deferred_preemption(opts, st);
    rc |= test_decision_less(opts, st);
    rc |= test_receive_check_is_acquisition(opts, st);
    rc |= test_follow_stamps_no_false_cooldown(opts, st);
    rc |= test_release_clears_ledger(opts, st);
    rc |= test_scan_target_contexts(opts, st);
    rc |= test_in_place_reassignment(opts, st);
    rc |= test_in_place_group_grant_never_binds_private_call(opts, st);
    rc |= test_in_place_reassignment_carries_crc_provenance(opts, st);
    rc |= test_slot_move_republishes(opts, st);
    rc |= test_move_matches_slot_identity(opts, st);
    rc |= test_deferred_move_keeps_identity(opts, st);
    rc |= test_move_republishes_renamed_call(opts, st);
    rc |= test_move_to_carrier_binds_moved_call(opts, st);
    rc |= test_composite_grant_adopts_resident_calls(opts, st);

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();
    dsd_state_ext_free_all(st);
    free(st);
    free(opts);
    if (rc == 0) {
        printf("DMR_PREEMPT: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
