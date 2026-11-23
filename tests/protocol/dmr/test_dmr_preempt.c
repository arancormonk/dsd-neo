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

#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/dmr/dmr.h>
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

#include "../../test_support/radio_policy_test.h"

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
static dsd_trunk_tune_result g_return_result = DSD_TRUNK_TUNE_RESULT_OK;

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
    if (g_return_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_result;
    }
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
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
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

/* A CRC-valid Tier III channel grant of opcode @p op (48 PV_GRANT, 49 TV_GRANT, 51 PD_GRANT, 52 TD_GRANT) naming
   @p target from @p src: the layout every one of them shares (TS 102 361-4 Tables 7.9-7.15). */
static void
grant_op(dsd_opts* opts, dsd_state* st, uint8_t op, uint16_t lpcn, uint8_t slot, uint32_t target, uint32_t src) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = op;
    write_bits_u32(bits, 16U, lpcn, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, target, 24U);
    write_bits_u32(bits, 56U, src, 24U);
    dmr_cspdu(opts, st, bits, bytes, 1U, 0U);
}

/* The CdefParms of an absolute channel (TS 102 361-4 Table 7.103, Cdeftype 0) naming LPCN @p lpcn on receive frequency
   @p freq_hz (a whole number of 125 Hz steps), as the block appended to an MBC header carries them. */
static void
write_abs_channel(uint8_t* bits, uint16_t lpcn, long freq_hz) {
    write_bits_u32(bits, 112U, 0U, 4U);
    write_bits_u32(bits, 118U, lpcn, 12U);
    write_bits_u32(bits, 153U, (uint32_t)(freq_hz / 1000000L), 10U);
    write_bits_u32(bits, 163U, (uint32_t)((freq_hz % 1000000L) / 125L), 13U);
}

/* A CRC-valid absolute TV_GRANT (call channel 0xFFF) with its appended block naming LPCN @p lpcn on @p freq_hz. */
static void
abs_tv_grant(dsd_opts* opts, dsd_state* st, uint16_t lpcn, long freq_hz, uint8_t slot, uint32_t tg, uint32_t src) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 49U;
    write_bits_u32(bits, 16U, 0x0FFFU, 12U);
    bits[28] = (uint8_t)(slot & 1U);
    write_bits_u32(bits, 32U, tg, 24U);
    write_bits_u32(bits, 56U, src, 24U);
    write_abs_channel(bits, lpcn, freq_hz);
    dmr_cspdu_mbc(opts, st, bits, bytes, 1U, 0U, 1U);
}

/* A CRC-valid C_BCAST Chan_Freq (TS 102 361-4 Table 7.89) with its appended block announcing LPCN @p lpcn on
   @p freq_hz. */
static void
c_bcast_chan_freq(dsd_opts* opts, dsd_state* st, uint16_t lpcn, long freq_hz) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 40U;
    write_bits_u32(bits, 16U, 5U, 5U);
    write_bits_u32(bits, 68U, lpcn, 12U);
    write_abs_channel(bits, lpcn, freq_hz);
    dmr_cspdu_mbc(opts, st, bits, bytes, 1U, 0U, 1U);
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

/* A grant assigning another call to the followed slot of the carrier already tuned ends the followed assignment: with
   the companion slot silent the receiver returns to the control channel, and the grant repeated there is followed with
   a tune of its own, its follow time then; a repeat of that call keeps the follow time. */
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
    int rc = expect_true("a reassignment of the followed slot returns to the control channel",
                         sm()->state == DMR_SM_ON_CC && g_tunes == 1);
    tv_grant(opts, st, LPCN_A, 0, 10, 2); // its repeat on the control channel
    rc |= expect_true("the repeated reassignment is followed anew",
                      vc(st) == VC_A && g_tunes == 2 && sm()->slots[0].target == 10U);
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

/* A grant that changes the followed slot's media (voice to data, data to voice) ends the follow as a release does: the
   receiver goes back to the control channel, and the grant repeated there is followed as any grant is, with a tune of
   its own. Voice for TG 300 from caller 3 on LPCN 16 (a RAS-accepted grant) is published; a data grant for TG 300 on
   the followed slot returns the receiver to the control channel; the checked voice grant that follows for TG 300 on
   LPCN 19 (the same carrier), from caller 3 again or from caller 2, opens its own call at its first voice: a new
   transmission, with its own channel, caller and provenance. A private voice grant followed by a data grant for the
   same radio likewise returns, and its first voice there publishes nothing of the obsolete voice grant. */
static int
test_followed_media_change_returns_to_cc(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (uint32_t caller = 2U; caller <= 3U; caller++) {
        init_env(opts, st, 2.0);
        st->trunk_chan_map[0x0013] = VC_A;
        st->currentslot = 0;
        st->event_crc_invalid[0] = (uint8_t)DSD_CRC_RAS_UNVERIFIED;
        tv_grant(opts, st, LPCN_A, 0, 300, 3);
        st->event_crc_invalid[0] = 0U;
        clock_at(0.2);
        voice(opts, st, 0);
        dsd_call_snapshot call;
        const uint64_t first = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
        rc |= expect_true("the first voice call is published on LPCN 16",
                          first != 0U && call.channel == LPCN_A && call.ota_source_id == 3U);
        clock_at(0.4);
        grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for the followed slot
        rc |= expect_true("a media change on the followed slot returns to the control channel",
                          sm()->state == DMR_SM_ON_CC && opts->trunk_is_tuned == 0 && sm()->vc_freq_hz == 0
                              && g_tunes == 1);
        clock_at(0.6);
        grant_op(opts, st, 49U, 0x0013U, 0, 300, caller); // TV_GRANT repeated on the control channel
        rc |= expect_true("the repeated voice grant is followed with a tune of its own",
                          sm()->state == DMR_SM_TUNED && g_tunes == 2 && sm()->vc_lpcn == 0x0013
                              && sm()->vc_data_call == 0 && sm()->vc_crc_flags == 0U);
        clock_at(0.8);
        voice(opts, st, 0);
        rc |= expect_true(caller == 3U ? "its first voice opens its own call (same caller)"
                                       : "its first voice opens its own call (another caller)",
                          dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                              && dsd_call_snapshot_transmission(&call) != first && call.policy_target_id == 300U
                              && call.channel == 0x0013U && call.ota_source_id == caller && call.crc_invalid == 0U);
    }

    init_env(opts, st, 2.0);
    grant_op(opts, st, 48U, LPCN_A, 0, 300, 1); // PV_GRANT
    rc |= expect_true("the private voice grant is followed", vc(st) == VC_A && sm()->vc_data_call == 0);
    clock_at(0.2);
    grant_op(opts, st, 51U, LPCN_A, 0, 300, 2); // PD_GRANT, same radio, carrier and slot
    rc |= expect_true("the data grant returns to the control channel", sm()->state == DMR_SM_ON_CC && g_tunes == 1);
    clock_at(0.3);
    voice(opts, st, 0);
    dsd_call_snapshot call;
    const int published =
        dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 1U;
    rc |= expect_true("the obsolete voice grant's caller is not published", !published);
    return rc;
}

/* An in-place group grant for TG 300 arrives while the companion slot of the followed carrier still carries a private
   call to unit 300 (a checked LC names it): the private call is not the group's, whatever the number, so the grant's
   record awaits its own call instead of binding to the private one. Once the private call ends, the group's unnamed
   voice is that call: it stays known (group 300, p10, its dwell from the grant), so a p100 preempt elsewhere takes the
   receiver after the dwell. */
static int
test_in_place_group_grant_never_binds_private_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 300, "A", 10, 0);
    seed_row(st, 999, "A", 100, 1);
    tv_grant(opts, st, LPCN_A, 1, 100, 1); // TS2 is followed
    clock_at(0.3);
    const dsd_call_observation private_call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = 300U,
        .policy_target_id = 300U,
        .ota_source_id = 7U,
    };
    voice(opts, st, 1);
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
    voice(opts, st, 1);
    voice(opts, st, 0);
    rc |= expect_true("the group's unnamed voice is the grant's call, still known",
                      sm()->slots[0].target == 300U && sm()->slots[0].kind == (uint8_t)DSD_CALL_KIND_GROUP_VOICE
                          && sm()->slots[0].call_epoch != 0U && sm()->slots[0].call_epoch != private_epoch);
    clock_at(1.5);
    voice(opts, st, 1);
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
   slot, which returns the receiver to the control channel, and its repeat there is followed. Its identity is published
   at the first voice with its own provenance, never the verified grant's: the call reads RAS-unverified. And the
   reverse: a verified reassignment of a RAS grant reads verified. */
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
        rc |= expect_true("the reassignment returns to the control channel", sm()->state == DMR_SM_ON_CC);
        tv_grant(opts, st, LPCN_A, 0, 200, 2); // its repeat there
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

/* D5 enrichment: a resident call on the control channel's carrier whose checked LC names a talker the radio list blocks
   still holds the receiver (the media gates mute it); a talkgroup block still lets it go. */
static int
test_radio_block_keeps_resident_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    int rc = expect_true("radio list", dsd_test_install_radio_policy(st, 7, "B", 0, 0, 0) == 0);
    clock_at(1.0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 7);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a resident call with a blocked talker is still busy", g_tunes == 0 && opts->trunk_is_tuned == 0);

    init_env(opts, st, 2.0);
    rc |= expect_true("radio list (resident talkgroup block)", dsd_test_install_radio_policy(st, 7, "B", 0, 0, 0) == 0);
    seed_row(st, 100, "B", 0, 0);
    clock_at(1.0);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 7);
    tv_grant(opts, st, LPCN_B, 0, 200, 2);
    rc |= expect_true("a talkgroup-blocked resident call with a blocked talker is not busy", vc(st) == VC_B);
    return rc;
}

/* A checked voice LC that names the followed slot before its first voice sync names the call: a voice grant for TG 300
   (caller 2), then a voice LC header naming caller 7. The voice sync publishes nothing over the LC's call: one
   transmission, caller 7. */
static int
test_grant_lc_names_the_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    grant_op(opts, st, 49U, LPCN_A, 0, 300, 2); // TV_GRANT, caller 2
    clock_at(0.3);
    canonical_call_begins(st, 0, 300, 7); // the voice LC header, as dmr_flco() publishes it
    dmr_sm_note_lc_identity(opts, st, 0, DSD_CALL_KIND_GROUP_VOICE, 300, 7);
    dsd_call_snapshot call;
    const uint64_t lc_epoch = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    clock_at(0.4);
    voice(opts, st, 0);
    return expect_true("the checked LC's caller stands at the voice sync",
                       lc_epoch != 0U && dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                           && call.ota_source_id == 7U && dsd_call_snapshot_transmission(&call) == lc_epoch
                           && g_tunes == 1);
}

/* A data grant never binds to an active voice call on its slot, whatever the numbers: on the companion slot of a
   followed carrier, a private data grant from radio 100 to radio 200 while group voice on TG 100 plays, and a private
   data grant to radio 300 while a private voice call to radio 300 plays, each await a call of their own. The second
   stays known once the voice call has ended and its hangtime run out, while data keeps flowing. */
static int
test_data_grant_never_binds_a_voice_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 999, 9); // the followed slot
    clock_at(0.2);
    canonical_call_begins(st, 1, 100, 1); // group voice on TG 100 on the companion slot
    voice(opts, st, 1);
    dsd_call_snapshot call;
    const uint64_t group_epoch = dsd_call_state_get(st, 1U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    clock_at(0.4);
    grant_op(opts, st, 51U, LPCN_A, 1, 200, 100); // PD_GRANT from radio 100 to radio 200
    int rc = expect_true("a private data grant is not bound to the group voice call",
                         group_epoch != 0U && sm()->slots[1].target == 200U && sm()->slots[1].data_call == 1U
                             && sm()->slots[1].call_epoch != group_epoch);

    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 999, 9);
    grant_op(opts, st, 48U, LPCN_A, 1, 300, 1); // PV_GRANT to radio 300 on the companion slot
    clock_at(0.2);
    const dsd_call_observation private_call = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = 300U,
        .policy_target_id = 300U,
        .ota_source_id = 1U,
    };
    (void)dsd_call_state_observe(st, &private_call, DSD_CALL_BOUNDARY_BEGIN);
    voice(opts, st, 1);
    const uint64_t private_epoch = dsd_call_state_get(st, 1U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    clock_at(0.4);
    grant_op(opts, st, 51U, LPCN_A, 1, 300, 1); // PD_GRANT to radio 300
    rc |= expect_true("a private data grant is not bound to the private voice call",
                      private_epoch != 0U && sm()->slots[1].data_call == 1U
                          && sm()->slots[1].call_epoch != private_epoch);
    clock_at(0.5);
    (void)dsd_call_state_end_ex(st, 1U, dsd_decode_now_mono_s(), DSD_CALL_END_TERMINATOR);
    for (int i = 1; i <= 6; i++) {
        clock_at(0.5 + (0.5 * i));
        dmr_sm_emit_data_sync(opts, st, 1);
        dmr_sm_tick_ctx(sm(), opts, st);
    }
    rc |= expect_true("the data grant's identity outlives the voice call's hangtime",
                      sm()->state == DMR_SM_TUNED && sm()->slots[1].target == 300U
                          && sm()->slots[1].kind == (uint8_t)DSD_CALL_KIND_DATA);
    return rc;
}

/* A data grant's record awaiting its call never binds to a voice call that opens on its slot afterwards: a private data
   grant to radio 300 is followed, then anonymous voice (no LC) opens on its slot. The record keeps awaiting a call of
   its own, and the grant's channel is not published into the voice call. */
static int
test_awaiting_data_grant_never_binds_later_voice(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    grant_op(opts, st, 51U, LPCN_A, 0, 300, 1); // PD_GRANT to radio 300
    clock_at(0.2);
    canonical_call_begins(st, 0, 0, 0); // late-entry voice, no LC yet
    voice(opts, st, 0);
    dsd_call_snapshot call;
    const uint64_t voice_epoch = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    int rc = expect_true("an awaiting data grant is not bound to a later voice call",
                         voice_epoch != 0U && sm()->slots[0].data_call == 1U && sm()->slots[0].call_epoch == 0U);
    rc |= expect_true("the data grant's channel is not published into the voice call",
                      dsd_call_state_get(st, 0U, &call) > 0 && call.channel == 0U);
    return rc;
}

/* A grant that changes the media of the companion slot's call is a new call there: it takes the slot's record (its
   identity and its follow time), while the followed slot's grant is not touched. Slot 0 is followed; on slot 1 a
   private voice grant to radio 300 from radio 2, then a private data grant to radio 300 from radio 3, name the data
   call; and a data grant then a voice grant for radio 300 give the voice call its own dwell, so a preempting candidate
   50 ms into that voice call waits it out. */
static int
test_companion_media_change_is_a_new_call(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    const double first_tune_m = sm()->t_tune_m;
    clock_at(0.2);
    grant_op(opts, st, 48U, LPCN_A, 1, 300, 2); // PV_GRANT on slot 1
    clock_at(0.4);
    grant_op(opts, st, 51U, LPCN_A, 1, 300, 3); // PD_GRANT on slot 1, same radio
    int rc = expect_true("the companion's data grant names its call",
                         sm()->slots[1].data_call == 1U && sm()->slots[1].source == 3U
                             && sm()->slots[1].kind == (uint8_t)DSD_CALL_KIND_DATA
                             && fabs(sm()->slots[1].follow_m - dsd_decode_now_mono_s()) < 1e-3);
    rc |= expect_true("the followed slot's grant is not restarted", sm()->vc_slot == 0 && sm()->vc_tg == 100
                                                                        && sm()->vc_src == 1 && sm()->vc_data_call == 0
                                                                        && fabs(sm()->t_tune_m - first_tune_m) < 1e-9);

    init_env(opts, st, 2.0);
    seed_row(st, 100, "A", 10, 0);
    seed_row(st, 50, "A", 50, 1);
    tv_grant(opts, st, LPCN_A, 0, 100, 1);
    const double followed_tune_m = sm()->t_tune_m;
    clock_at(0.1);
    grant_op(opts, st, 51U, LPCN_A, 1, 300, 1); // PD_GRANT on slot 1
    clock_at(3.0);
    grant_op(opts, st, 48U, LPCN_A, 1, 300, 1); // PV_GRANT on slot 1, same radio
    rc |= expect_true("the companion's voice grant follows from its own assignment",
                      sm()->slots[1].data_call == 0U && fabs(sm()->slots[1].follow_m - dsd_decode_now_mono_s()) < 1e-3
                          && fabs(sm()->t_tune_m - followed_tune_m) < 1e-9);
    clock_at(3.05);
    voice(opts, st, 0);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 50, 5);
    rc |=
        expect_true("a preempting candidate 50 ms into the companion's voice call waits out its dwell", vc(st) == VC_A);
    return rc;
}

/* Voice on the companion slot does not take the followed grant's slot out of the busy view while that grant still
   awaits its first voice: a p90 grant for TS1 is followed, p10 voice on TS2 starts 50 ms later, and at 1.5 s, the p90
   call not yet on the air, a p50 preempt candidate elsewhere outranks only the companion. It waits; once the p90 call's
   acquisition window has run out with its slot still silent, the companion alone holds the carrier and p50 takes it. */
static int
test_companion_voice_keeps_acquiring_slot_held(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 900, "A", 90, 0);
    seed_row(st, 10, "A", 10, 0);
    seed_row(st, 50, "A", 50, 1);
    tv_grant(opts, st, LPCN_A, 0, 900, 1);
    clock_at(0.05);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 10, 2);
    clock_at(1.5);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 50, 5);
    int rc = expect_true("p50 does not outrank the p90 grant still awaiting its voice", vc(st) == VC_A);
    clock_at(4.1);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 50, 5);
    rc |= expect_true("past the p90 grant's acquisition window, p50 outranks the companion",
                      vc(st) == VC_B && sm()->vc_tg == 50);
    return rc;
}

/* A checked grant that changes the followed slot's media ends that slot's assignment whether or not the receiver would
   follow it: with data following off (the default), a TD_GRANT for the followed slot is not followed, and a data grant
   or a voice grant the talkgroup policy blocks is not either; each still returns the receiver to the control channel,
   so the voice grant repeated there is followed with a tune of its own and its first voice opens its own call. */
static int
test_unfollowed_media_change_ends_the_assignment(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int variant = 0; variant < 2; variant++) {
        init_env(opts, st, 2.0);
        if (variant == 0) {
            opts->trunk_tune_data_calls = 0;
        } else {
            seed_row(st, 400, "B", 0, 0);
        }
        tv_grant(opts, st, LPCN_A, 0, 300, 3);
        clock_at(0.2);
        voice(opts, st, 0);
        dsd_call_snapshot call;
        const uint64_t first = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
        clock_at(0.4);
        grant_op(opts, st, 52U, LPCN_A, 0, variant == 0 ? 300U : 400U, 9); // TD_GRANT, not followed
        rc |= expect_true(variant == 0 ? "an unfollowed data grant (data following off) returns to the control channel"
                                       : "a policy-blocked data grant returns to the control channel",
                          sm()->state == DMR_SM_ON_CC && opts->trunk_is_tuned == 0 && g_tunes == 1);
        clock_at(0.6);
        tv_grant(opts, st, LPCN_A, 0, 300, 3);
        clock_at(0.8);
        voice(opts, st, 0);
        rc |= expect_true("the repeated voice grant opens its own call",
                          g_tunes == 2 && dsd_call_state_get(st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                              && dsd_call_snapshot_transmission(&call) != first && call.policy_target_id == 300U);
    }

    init_env(opts, st, 2.0);
    seed_row(st, 400, "B", 0, 0);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 1); // TD_GRANT, followed for data
    rc |= expect_true("the data grant is followed", vc(st) == VC_A && sm()->vc_data_call == 1);
    clock_at(0.2);
    dmr_sm_emit_data_sync(opts, st, 0);
    clock_at(0.3);
    tv_grant(opts, st, LPCN_A, 0, 400, 2); // a voice grant the policy blocks, for the followed slot
    rc |= expect_true("a policy-blocked voice grant ends the data follow", sm()->state == DMR_SM_ON_CC && g_tunes == 1);
    return rc;
}

/* A checked grant for the followed slot that names another call (another talkgroup, or a private call) ends the
   followed assignment whether or not it is admitted: TS1 is followed for TG 300; a TV_GRANT for TS1 naming TG 400,
   which the policy blocks, and a PV_GRANT for TS1 with private following off, each return the receiver to the control
   channel, so later voice on TS1 is never read as TG 300's. A repeat of the followed grant (another caller, same
   talkgroup) is the same call and changes nothing. */
static int
test_unfollowed_reassignment_ends_the_assignment(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int variant = 0; variant < 2; variant++) {
        init_env(opts, st, 2.0);
        if (variant == 0) {
            seed_row(st, 400, "B", 0, 0);
        } else {
            opts->trunk_tune_private_calls = 0;
        }
        tv_grant(opts, st, LPCN_A, 0, 300, 3);
        clock_at(0.2);
        voice(opts, st, 0);
        clock_at(0.3);
        tv_grant(opts, st, LPCN_A, 0, 300, 4); // a repeat with another caller: the same call
        rc |= expect_true("a repeat of the followed talkgroup keeps the follow",
                          sm()->state == DMR_SM_TUNED && sm()->vc_tg == 300 && g_tunes == 1);
        clock_at(0.4);
        if (variant == 0) {
            tv_grant(opts, st, LPCN_A, 0, 400, 9); // blocked talkgroup on the followed slot
        } else {
            grant_op(opts, st, 48U, LPCN_A, 0, 777, 9); // PV_GRANT, private following off
        }
        rc |= expect_true(
            variant == 0 ? "a policy-blocked reassignment of the followed slot returns to the control channel"
                         : "an unfollowed private reassignment of the followed slot returns to the control channel",
            sm()->state == DMR_SM_ON_CC && opts->trunk_is_tuned == 0 && g_tunes == 1);
    }
    return rc;
}

/* A grant's carrier from the channel map counts off the control channel unless lpcn_is_trusted() refuses the mapping:
   one learned off the control channel and still unconfirmed (trust 1). TS1 is followed for TG 1100; a TV_GRANT for
   TG 2200 on TS1 names an LPCN mapped to the followed carrier. Learned off the control channel, the mapping is refused
   and the follow stands; imported (trust 0) or learned on the control channel (trust 2), the grant ends the
   assignment. Likewise a Hold grant for TG 2200 on an LPCN mapped to another carrier is refused at trust 1 and followed
   at trust 0 or 2. */
static int
test_untrusted_lpcn_mapping_names_no_carrier(dsd_opts* opts, dsd_state* st) {
    const uint16_t lpcn_d = 0x0013U;
    int rc = 0;
    for (uint8_t trust = 0U; trust <= 2U; trust++) {
        init_env(opts, st, 2.0);
        tv_grant(opts, st, LPCN_A, 0, 1100, 1);
        clock_at(0.2);
        voice(opts, st, 0);
        st->trunk_chan_map[lpcn_d] = VC_A;
        st->dmr_lcn_trust[lpcn_d] = trust;
        clock_at(0.3);
        tv_grant(opts, st, lpcn_d, 0, 2200, 2);
        const int kept = sm()->state == DMR_SM_TUNED && sm()->vc_tg == 1100 && opts->trunk_is_tuned == 1;
        const int ended = sm()->state == DMR_SM_ON_CC && opts->trunk_is_tuned == 0;
        if (trust == 1U) {
            rc |= expect_true("an unconfirmed learned mapping to the followed carrier keeps the follow", kept);
        } else {
            rc |= expect_true("an imported or confirmed mapping to the followed carrier ends the assignment", ended);
        }

        init_env(opts, st, 2.0);
        tv_grant(opts, st, LPCN_A, 0, 1100, 1);
        clock_at(0.2);
        voice(opts, st, 0);
        st->trunk_chan_map[lpcn_d] = VC_B;
        st->dmr_lcn_trust[lpcn_d] = trust;
        st->tg_hold = 2200;
        tv_grant(opts, st, lpcn_d, 0, 2200, 2);
        if (trust == 1U) {
            rc |= expect_true("a hold grant with an unconfirmed learned mapping is not followed",
                              vc(st) == VC_A && sm()->vc_tg == 1100 && g_tunes == 1);
        } else {
            rc |= expect_true("a hold grant with an imported or confirmed mapping is followed",
                              vc(st) == VC_B && sm()->vc_tg == 2200 && g_tunes == 2);
        }
        st->tg_hold = 0;
    }
    return rc;
}

/* TS1 is followed for TG 1100 with live voice, and a Hold grant for TG 2200 names LPCN @p lpcn only: whether the
   receiver follows it there (off the control channel). */
static int
hold_grant_followed_off_cc(dsd_opts* opts, dsd_state* st, uint16_t lpcn, long freq_hz) {
    if (sm()->state != DMR_SM_TUNED) {
        tv_grant(opts, st, LPCN_A, 0, 1100, 1);
    }
    clock_at(0.2);
    voice(opts, st, 0);
    const int tunes = g_tunes;
    st->tg_hold = 2200;
    tv_grant(opts, st, lpcn, 0, 2200, 2);
    st->tg_hold = 0;
    return vc(st) == freq_hz && sm()->vc_tg == 2200 && g_tunes == tunes + 1;
}

/* An LPCN mapping learned off the control channel (trust 1) is confirmed (2) when a checked absolute grant heard on the
   control channel names the same frequency for it: a later LPCN-only Hold grant heard off the control channel is then
   followed. An absolute grant naming another frequency for it changes neither its frequency nor its provenance, nor
   does one heard off the control channel, and an imported mapping (0) is never rewritten. */
static int
test_control_channel_confirms_learned_lpcn(dsd_opts* opts, dsd_state* st) {
    const uint16_t lpcn_d = 0x0013U;
    int rc = 0;
    for (int variant = 0; variant < 4; variant++) {
        init_env(opts, st, 2.0);
        seed_row(st, 2300, "B", 0, 0); // the confirming grant's own call is not followed
        const uint8_t trust = variant == 2 ? 0U : 1U;
        st->trunk_chan_map[lpcn_d] = VC_B;
        st->dmr_lcn_trust[lpcn_d] = trust;
        if (variant == 3) {
            tv_grant(opts, st, LPCN_A, 0, 1100, 1);
            clock_at(0.1);
            voice(opts, st, 0);
        }
        abs_tv_grant(opts, st, lpcn_d, variant == 0 || variant == 3 ? VC_B : VC_C, 1, 2300, 3);
        const int followed = hold_grant_followed_off_cc(opts, st, lpcn_d, VC_B);
        if (variant == 0) {
            rc |= expect_true("the control channel confirms a learned mapping of the same frequency",
                              st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 2U);
            rc |= expect_true("a hold grant on the confirmed mapping is followed off the control channel", followed);
        } else if (variant == 1) {
            rc |= expect_true("another frequency on the control channel neither confirms nor rewrites the mapping",
                              st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 1U);
            rc |= expect_true("a hold grant on the still unconfirmed mapping is not followed", !followed);
        } else if (variant == 2) {
            rc |= expect_true("an imported mapping keeps its frequency and provenance",
                              st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 0U);
        } else {
            rc |= expect_true("an absolute grant heard off the control channel confirms nothing",
                              st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 1U && !followed);
        }
    }
    return rc;
}

/* A C_BCAST Chan_Freq records its mapping's provenance as an absolute grant does: heard while a call is followed off
   the control channel it is unconfirmed (1), so a Hold grant on that LPCN heard there is not followed; heard on the
   control channel it is confirmed (2) and such a grant is followed. */
static int
test_c_bcast_mapping_records_provenance(dsd_opts* opts, dsd_state* st) {
    const uint16_t lpcn_d = 0x0013U;
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 1100, 1);
    clock_at(0.1);
    voice(opts, st, 0);
    c_bcast_chan_freq(opts, st, lpcn_d, VC_B);
    int rc = expect_true("a channel announced off the control channel is unconfirmed",
                         st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 1U);
    rc |= expect_true("a hold grant on an off-channel announcement is not followed",
                      !hold_grant_followed_off_cc(opts, st, lpcn_d, VC_B));

    init_env(opts, st, 2.0);
    c_bcast_chan_freq(opts, st, lpcn_d, VC_B);
    rc |= expect_true("a channel announced on the control channel is confirmed",
                      st->trunk_chan_map[lpcn_d] == VC_B && st->dmr_lcn_trust[lpcn_d] == 2U);
    rc |= expect_true("a hold grant on a control channel announcement is followed",
                      hold_grant_followed_off_cc(opts, st, lpcn_d, VC_B));
    return rc;
}

/* A grant followed in place on the composite control channel's own carrier keeps the receiver on the control channel:
   what a checked CSBK teaches the channel map there is confirmed (absolute grant and C_BCAST alike), and an LPCN-only
   Hold grant there acts on a mapping still unconfirmed (trust 1), as on the control channel. */
static int
test_composite_payload_counts_as_control_channel(dsd_opts* opts, dsd_state* st) {
    const uint16_t lpcn_d = 0x0013U;
    const uint16_t lpcn_e = 0x0014U;
    const uint16_t lpcn_f = 0x0015U;
    init_env(opts, st, 2.0);
    st->trunk_chan_map[0x0020U] = CC_HZ;
    st->trunk_chan_map[lpcn_d] = VC_B;
    st->dmr_lcn_trust[lpcn_d] = 1U;
    seed_row(st, 2300, "B", 0, 0);
    clock_at(1.0);
    tv_grant(opts, st, 0x0020U, 1, 100, 1);
    voice(opts, st, 1);
    int rc = expect_true("the composite carrier's grant is followed in place",
                         sm()->state == DMR_SM_TUNED && sm()->vc_freq_hz == CC_HZ && opts->trunk_is_tuned == 1);
    abs_tv_grant(opts, st, lpcn_e, VC_C, 0, 2300, 3);
    c_bcast_chan_freq(opts, st, lpcn_f, VC_C + 12500L);
    rc |= expect_true("an absolute grant on the composite payload teaches a confirmed mapping",
                      st->trunk_chan_map[lpcn_e] == VC_C && st->dmr_lcn_trust[lpcn_e] == 2U);
    rc |= expect_true("a C_BCAST on the composite payload teaches a confirmed mapping",
                      st->trunk_chan_map[lpcn_f] == VC_C + 12500L && st->dmr_lcn_trust[lpcn_f] == 2U);
    const int tunes = g_tunes;
    st->tg_hold = 2200;
    tv_grant(opts, st, lpcn_d, 0, 2200, 2);
    st->tg_hold = 0;
    rc |= expect_true("a hold grant on the composite payload acts on an unconfirmed mapping",
                      vc(st) == VC_B && sm()->vc_tg == 2200 && g_tunes == tunes + 1);
    return rc;
}

/* An adopted companion's unnamed call takes the identity of the voice grant that first names it: TS1 is followed for
   TG 300 while TS2 carries voice no LC has named. A data grant for TS1 retires TS1 and the receiver stays for TS2. A
   voice grant then names TS2's call (TG 700 from 7), and TS2's next voice sync publishes that identity into it. */
static int
test_adopted_companion_takes_its_grant_identity(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    canonical_call_begins(st, 1, 0U, 0U);
    voice(opts, st, 1);
    clock_at(0.4);
    voice(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1
    int rc = expect_true("the receiver stays for the unnamed companion",
                         sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1 && g_tunes == 1);
    clock_at(0.5);
    tv_grant(opts, st, LPCN_A, 1, 700, 7); // the voice grant naming TS2's call
    voice(opts, st, 1);
    dsd_call_snapshot call;
    rc |= expect_true("the companion's next voice sync publishes the grant's identity",
                      dsd_call_state_get(st, 1U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                          && call.policy_target_id == 700U && call.ota_source_id == 7U);
    return rc;
}

/* A media change on the followed slot retires that slot's assignment alone: TS1 is followed for TG 300 while TS2 carries
   live voice for TG 100. A data grant for TS1 ends TS1's call and record, while the receiver stays on the carrier for
   the TS2 call, which goes on as the same call. A voice grant repeated for TS1 is then a new assignment of that slot;
   once TS2's voice has stopped and its hangtime has run out, the receiver returns to the control channel. */
static int
test_media_change_keeps_a_live_companion(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    opts->frame_dmr = 1; // the DMR tick runs only while DMR trunking can recover
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    canonical_call_begins(st, 1, 100U, 1U);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    dsd_call_snapshot call;
    const uint64_t first = dsd_call_state_get(st, 0U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    const uint64_t companion = dsd_call_state_get(st, 1U, &call) > 0 ? dsd_call_snapshot_transmission(&call) : 0U;
    clock_at(0.4);
    voice(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1
    int rc = expect_true("the receiver stays on the carrier for the companion's live call",
                         sm()->state == DMR_SM_TUNED && vc(st) == VC_A && opts->trunk_is_tuned == 1 && g_tunes == 1);
    rc |= expect_true("TS1's assignment is retired: its call ended, its record cleared",
                      dsd_call_state_get(st, 0U, &call) > 0 && dsd_call_snapshot_transmission(&call) == first
                          && call.phase != DSD_CALL_PHASE_ACTIVE
                          && sm()->slots[0].kind == (uint8_t)DSD_CALL_KIND_UNKNOWN && sm()->slots[0].voice_active == 0);
    rc |= expect_true("the companion's call goes on as the same call",
                      slot_publishes_bound(st, 1, 100U) && dsd_call_state_get(st, 1U, &call) > 0
                          && dsd_call_snapshot_transmission(&call) == companion);
    clock_at(0.6);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_A, 0, 300, 2); // the voice grant repeated for TS1
    rc |= expect_true("the repeated voice grant names TS1 anew",
                      sm()->state == DMR_SM_TUNED && g_tunes == 1 && sm()->slots[0].target == 300U
                          && sm()->slots[0].source == 2U && sm()->slots[0].data_call == 0U
                          && fabs(sm()->slots[0].follow_m - dsd_decode_now_mono_s()) < 1e-3);
    clock_at(2.5);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("within the companion's hangtime the carrier is kept", sm()->state == DMR_SM_TUNED);
    clock_at(2.7);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("past the companion's hangtime the receiver returns", sm()->state == DMR_SM_ON_CC);
    return rc;
}

/* What a slot's encrypted call and its packet left behind: privacy state, key selection, MI chain and an armed data
   header, each slot's own values told apart by @p base. */
static void
seed_slot_call_state(dsd_state* st, int slot, int base) {
    unsigned int* so = slot == 0 ? &st->dmr_so : &st->dmr_soR;
    unsigned int* fid = slot == 0 ? &st->dmr_fid : &st->dmr_fidR;
    int* algid = slot == 0 ? &st->payload_algid : &st->payload_algidR;
    int* keyid = slot == 0 ? &st->payload_keyid : &st->payload_keyidR;
    unsigned long long int* mi = slot == 0 ? &st->payload_mi : &st->payload_miR;
    *so = 0x40U;
    *fid = 0x10U;
    *algid = 0x24;
    *keyid = base;
    *mi = 0x1000ULL + (unsigned long long int)base;
    st->dmr_enc_class[slot] = DMR_ENC_CLASS_ENC;
    st->dmr_enc_class_est[slot] = 1U;
    st->dmr_enc_class_held[slot] = 0U;
    st->dmr_vc_ref_state[slot] = 2U; // DMR_VC_REF_VALID: a trusted MI chain
    st->dmr_vc_untrusted[slot] = 0U;
    st->dmr_heal_valid[slot] = 1U;
    st->dmr_tg_autoload.slot_tg[slot] = (uint32_t)base;
    st->data_header_valid[slot] = 1U;
    st->data_header_blocks[slot] = 1;
    st->data_header_format[slot] = 2U;
    st->data_header_sap[slot] = 4U;
    st->dmr_lrrp_source[slot] = (unsigned long long int)base;
    st->dmr_lrrp_target[slot] = (unsigned long long int)base + 1ULL;
    DSD_SNPRINTF(st->generic_talker_alias[slot], sizeof(st->generic_talker_alias[slot]), "%s", "ALIAS");
}

/* Whether @p slot still holds what seed_slot_call_state() gave it. */
static int
slot_call_state_kept(const dsd_state* st, int slot, int base) {
    const int algid = slot == 0 ? st->payload_algid : st->payload_algidR;
    const int keyid = slot == 0 ? st->payload_keyid : st->payload_keyidR;
    const unsigned long long int mi = slot == 0 ? st->payload_mi : st->payload_miR;
    const unsigned int so = slot == 0 ? st->dmr_so : st->dmr_soR;
    return algid == 0x24 && keyid == base && mi == 0x1000ULL + (unsigned long long int)base && so == 0x40U
           && st->dmr_enc_class[slot] == DMR_ENC_CLASS_ENC && st->dmr_enc_class_held[slot] == 0U
           && st->dmr_vc_ref_state[slot] == 2U && st->dmr_vc_untrusted[slot] == 0U
           && st->dmr_tg_autoload.slot_tg[slot] == (uint32_t)base && st->data_header_valid[slot] == 1U
           && st->data_header_sap[slot] == 4U && st->dmr_lrrp_source[slot] == (unsigned long long int)base;
}

/* Whether @p slot is as a fresh call finds it: no privacy state, key selection or MI chain, its voice held until the
   call says how it is encrypted, and no armed data header nor packet addressing. */
static int
slot_call_state_fresh(const dsd_state* st, int slot) {
    const int algid = slot == 0 ? st->payload_algid : st->payload_algidR;
    const int keyid = slot == 0 ? st->payload_keyid : st->payload_keyidR;
    const unsigned long long int mi = slot == 0 ? st->payload_mi : st->payload_miR;
    const unsigned int so = slot == 0 ? st->dmr_so : st->dmr_soR;
    const unsigned int fid = slot == 0 ? st->dmr_fid : st->dmr_fidR;
    return algid == 0 && keyid == 0 && mi == 0ULL && so == 0U && fid == 0U
           && st->dmr_enc_class[slot] == DMR_ENC_CLASS_NONE && st->dmr_enc_class_held[slot] == 1U
           && st->dmr_vc_ref_state[slot] == 0U && st->dmr_vc_untrusted[slot] == 1U && st->dmr_heal_valid[slot] == 0U
           && st->dmr_tg_autoload.slot_tg[slot] == 0U && st->data_header_valid[slot] == 0U
           && st->data_header_format[slot] == 7U && st->data_header_sap[slot] == 0U && st->dmr_lrrp_source[slot] == 0ULL
           && st->dmr_lrrp_target[slot] == 0ULL && st->generic_talker_alias[slot][0] == '\0';
}

/* A slot retired for its companion is left as a fresh call finds it, and the companion is not touched: TS1 is followed
   for an encrypted TG 300 call with a data header armed, while TS2 carries a live encrypted TG 100 call. A TD_GRANT for
   TS1 retires TS1 and the receiver stays for TS2. TS1's privacy state, key selection and MI chain are gone, its voice
   is held until its next call says how it is encrypted, and its armed header is disarmed, so the replacement call's
   voice or blocks before their own signalling cannot be decrypted or addressed with the previous call's. TS2 keeps
   every one of its own. */
static int
test_retired_slot_starts_fresh(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    canonical_call_begins(st, 1, 100U, 1U);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    seed_slot_call_state(st, 0, 5);
    seed_slot_call_state(st, 1, 7);
    clock_at(0.4);
    voice(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1
    int rc = expect_true("the receiver stays for the companion", sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1);
    rc |= expect_true("the retired slot is left as a fresh call finds it", slot_call_state_fresh(st, 0));
    rc |= expect_true("the companion keeps its call's crypto and packet state", slot_call_state_kept(st, 1, 7));
    return rc;
}

/* Whether every embedded-signalling fragment of @p slot is clear. */
static int
embedded_lc_clear(const dsd_state* st, int slot) {
    for (size_t b = 0; b < sizeof(st->dmr_embedded_signalling[0]) / sizeof(st->dmr_embedded_signalling[0][0]); b++) {
        for (size_t i = 0; i < sizeof(st->dmr_embedded_signalling[0][0]); i++) {
            if (st->dmr_embedded_signalling[slot][b][i] != 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* A retired slot's pending embedded LC dies with its assignment: TS1 is followed for TG 300 and has collected bursts
   B-E of a voice superframe, while TS2 carries TG 100. A TD_GRANT for TS1 retires TS1 before its burst F, and the
   receiver stays for TS2. TS1's fragments are dropped and its voice gate is closed with no voice sync seen, so its F
   burst is not decoded and no LC is dispatched from the retired call's fragments: the next LC there needs a fresh
   superframe (its burst A's sync, then fresh B-E). TS2 keeps its fragments and open gate. */
static int
test_retired_slot_drops_embedded_lc(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    canonical_call_begins(st, 1, 100U, 1U);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    for (int s = 0; s < 2; s++) {
        st->dmr_confidence_voice_sync_seen[s] = 1U;
        st->dmr_confidence_voice_count[s] = 2U;
        for (int b = 1; b <= 4; b++) { // bursts B-E
            DSD_MEMSET(st->dmr_embedded_signalling[s][b], 1, sizeof(st->dmr_embedded_signalling[s][b]));
        }
    }
    clock_at(0.4);
    voice(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1, before its burst F
    int rc = expect_true("the receiver stays for the companion", sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1);
    rc |= expect_true("the retired slot's pending embedded LC is dropped", embedded_lc_clear(st, 0));
    rc |= expect_true("the retired slot's voice gate waits for a fresh superframe",
                      st->dmr_confidence_voice_open[0] == 0U && st->dmr_confidence_voice_sync_seen[0] == 0U
                          && st->dmr_confidence_voice_count[0] == 0U);
    rc |= expect_true("the companion keeps its embedded LC and voice gate",
                      st->dmr_embedded_signalling[1][1][0] == 1 && st->dmr_embedded_signalling[1][4][47] == 1
                          && st->dmr_confidence_voice_open[1] == 1U && st->dmr_confidence_voice_sync_seen[1] == 1U);
    return rc;
}

/* The grant origins keep the numbers the public header always gave them: a producer built against an earlier header
   sends 2 for a channel status. The retired C_MOVE value 1 names no origin: an event carrying it, here for the followed
   slot naming another call and for another carrier, is refused before it can end, reconcile or follow anything. */
static void
set_raw_origin(dmr_sm_event_t* ev, int raw) {
    // As a producer built against another header stores it: the raw integer, no cast to a listed enumerator.
    if (sizeof(ev->origin) != sizeof(raw)) {
        abort();
    }
    DSD_MEMCPY(&ev->origin, &raw, sizeof(raw));
}

static int
test_grant_origin_values(dsd_opts* opts, dsd_state* st) {
    int rc = expect_true("grant origins keep their values", DMR_SM_ORIGIN_GRANT == 0 && DMR_SM_ORIGIN_STATUS == 2);
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    dmr_sm_event_t same = dmr_sm_ev_group_grant(VC_A, LPCN_A, 500, 3);
    same.slot = 0;
    set_raw_origin(&same, 1);
    rc |= expect_true("an unknown origin ends no assignment", dmr_sm_note_grant_assignment(opts, st, &same) == 0);
    rc |= expect_true("an unknown origin on the followed slot is refused",
                      dmr_sm_submit_grant(opts, st, &same) == DMR_SM_GRANT_REFUSED);
    dmr_sm_event_t other = dmr_sm_ev_group_grant(VC_B, LPCN_B, 500, 3);
    other.slot = 0;
    set_raw_origin(&other, 1);
    rc |= expect_true("an unknown origin elsewhere is refused",
                      dmr_sm_submit_grant(opts, st, &other) == DMR_SM_GRANT_REFUSED);
    rc |= expect_true("the followed call is untouched", sm()->state == DMR_SM_TUNED && sm()->vc_slot == 0
                                                            && sm()->vc_tg == 300 && vc(st) == VC_A && g_tunes == 1
                                                            && sm()->slots[0].target == 300U);
    return rc;
}

/* A checked grant for slot @p slot of VC_A on channel @p lsn of kind @p kind naming group @p tg. */
static void
lsn_grant(dsd_opts* opts, dsd_state* st, int slot, int lsn, uint8_t kind, int tg) {
    dmr_sm_event_t ev = dmr_sm_ev_group_grant(VC_A, lsn, tg, 3);
    ev.slot = slot;
    ev.channel_kind = kind;
    ev.origin = DMR_SM_ORIGIN_GRANT;
    (void)dmr_sm_submit_grant(opts, st, &ev);
}

/* The companion adopted for a retired slot takes its own channel, never the retired slot's: on an LSN carrier (Cap+,
   XPT) TS1 is followed on LSN 3 for TG 300 while TS2 carries voice with no channel record; a grant for TS1 naming TG
   500 retires TS1 and the receiver stays for TS2, whose first LC then names no channel (the retired LSN 3 is TS1's
   alone). With TS2's own LSN 4 known (a status named it), TS2's LC carries LSN 4. A Tier III LPCN names the physical
   channel, both slots: the adopted companion keeps it. */
static int
test_companion_adoption_takes_its_own_channel(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    for (int variant = 0; variant < 3; variant++) {
        init_env(opts, st, 2.0);
        const uint8_t kind = variant == 2 ? (uint8_t)DSD_CALL_CHANNEL_LPCN : (uint8_t)DSD_CALL_CHANNEL_LSN;
        const int channel = variant == 2 ? (int)LPCN_A : 3;
        lsn_grant(opts, st, 0, channel, kind, 300);
        clock_at(0.2);
        voice(opts, st, 0);
        canonical_call_begins(st, 1, 0U, 0U);
        voice(opts, st, 1);
        if (variant == 1) {
            dmr_sm_status_slot_reconcile(sm(), st, 1, 4, VC_A, 777U, 1); // a status names TS2 on its own LSN 4
        }
        clock_at(0.4);
        voice(opts, st, 1);
        lsn_grant(opts, st, 0, channel, kind, 500); // another call on TS1
        rc |= expect_true("the receiver stays for the companion", sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1);
        dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
        const dmr_sm_slot_ctx_t* rec = &sm()->slots[1];
        if (variant == 0) {
            rc |= expect_true("an adopted companion with no channel takes none of the retired slot's LSN",
                              sm()->vc_lpcn == 0 && sm()->vc_channel_kind == (uint8_t)DSD_CALL_CHANNEL_NONE
                                  && sm()->vc_crc_flags == 0U && rec->channel == 0
                                  && rec->channel_kind == (uint8_t)DSD_CALL_CHANNEL_NONE);
        } else if (variant == 1) {
            rc |= expect_true("an adopted companion keeps its own LSN",
                              sm()->vc_lpcn == 4 && sm()->vc_channel_kind == (uint8_t)DSD_CALL_CHANNEL_LSN
                                  && rec->channel == 4 && rec->channel_kind == (uint8_t)DSD_CALL_CHANNEL_LSN);
        } else {
            rc |=
                expect_true("an adopted companion keeps the carrier's LPCN",
                            sm()->vc_lpcn == (int)LPCN_A && sm()->vc_channel_kind == (uint8_t)DSD_CALL_CHANNEL_LPCN
                                && rec->channel == (int)LPCN_A && rec->channel_kind == (uint8_t)DSD_CALL_CHANNEL_LPCN);
        }
    }
    return rc;
}

/* An adopted companion whose call nothing identified keeps an unknown media, which no grant's media contradicts: data
   following on, TS1 is followed for TG 300 while TS2 carries data that no grant or LC named. A TD_GRANT for TS1 retires
   TS1 and the receiver stays for TS2. A TD_GRANT for TS2 then describes the data already received there: it is no
   other call, so the receiver stays on the carrier, now following TS2's data call. */
static int
test_adopted_companion_media_stays_unknown(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    clock_at(0.4);
    dmr_sm_emit_data_sync(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1
    int rc = expect_true("the receiver stays for the data companion",
                         sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1 && g_tunes == 1);
    clock_at(0.5);
    dmr_sm_emit_data_sync(opts, st, 1);
    grant_op(opts, st, 52U, LPCN_A, 1, 900, 9); // TD_GRANT for TS2: the data on the air there
    rc |= expect_true("a data grant for the unidentified companion does not end its follow",
                      sm()->state == DMR_SM_TUNED && sm()->vc_slot == 1 && opts->trunk_is_tuned == 1 && g_tunes == 1);
    rc |= expect_true("the grant establishes the companion's media", sm()->vc_data_call == 1);
    return rc;
}

/* A companion slot the receive policy rejects never keeps the receiver, its live voice or hangtime tail alike: TS1 is
   followed for TG 300 while TS2 carries TG 100, and the receive filter then admits TS1 alone. A TD_GRANT for TS1 retires
   TS1 and returns the receiver to the control channel, TS2's live call being one it cannot receive. Likewise on the
   tick: once the admitted TS1 call's tail has run out, TS2's later activity, heard before the edit, holds nothing. */
static int
test_rejected_companion_keeps_nothing(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    canonical_call_begins(st, 1, 100U, 1U);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 100, 1);
    clock_at(0.4);
    voice(opts, st, 1);
    int rc = expect_true("slot filter parsed",
                         dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for TS1
    rc |= expect_true("a rejected companion does not keep the receiver on a retired slot's carrier",
                      sm()->state == DMR_SM_ON_CC && opts->trunk_is_tuned == 0 && g_tunes == 1);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));

    init_env(opts, st, 2.0);
    opts->frame_dmr = 1; // the DMR tick runs only while DMR trunking can recover
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    clock_at(1.0);
    voice(opts, st, 1);
    rc |= expect_true("slot filter parsed",
                      dsd_receive_edit_parse(&opts->receive, DSD_RECEIVE_SLOT, 1, "1", "", "", NULL, 0) == 0);
    clock_at(1.1);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("the admitted slot's tail keeps the carrier", sm()->state == DMR_SM_TUNED);
    clock_at(2.3);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |=
        expect_true("past the admitted slot's tail a rejected slot's tail holds nothing", sm()->state == DMR_SM_ON_CC);
    DSD_MEMSET(&opts->receive, 0, sizeof(opts->receive));
    return rc;
}

/* The accepted voice that binds a slotless follow to its slot is that grant's first voice: a slotless p90 grant is
   followed and its only voice arrives on TS1 at 0.1 s, then p10 voice on TS2 runs on. At 2.5 s, TS1's tail over, TS1
   no longer holds the carrier as an acquisition still to come, so a p50 preempt candidate elsewhere outranks the
   companion alone and is followed. */
static int
test_slotless_follow_first_voice_is_heard(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 900, "A", 90, 0);
    seed_row(st, 10, "A", 10, 0);
    seed_row(st, 50, "A", 50, 1);
    dmr_sm_emit_group_grant(opts, st, VC_A, 0, 900, 1); // slotless
    int rc = expect_true("the slotless grant is followed", vc(st) == VC_A && sm()->vc_slot == -1);
    clock_at(0.1);
    voice(opts, st, 0);
    rc |= expect_true("the binding voice is the grant's first voice", sm()->vc_slot == 0 && sm()->vc_media_heard == 1);
    clock_at(0.2);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 10, 2);
    clock_at(2.5);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 50, 5);
    rc |= expect_true("past TS1's tail, p50 outranks the companion", vc(st) == VC_B && sm()->vc_tg == 50);
    return rc;
}

/* A slotless follow binds only to a slot whose voice passed its gate: a slotless grant for TG 900 is followed, and a
   voice sync on TS1 whose burst supplied no accepted voice (its gate closed) neither binds the grant to TS1 nor
   publishes the grant's call there. The first accepted voice, on TS2, binds the grant and is its first voice, and the
   grant's call is published on TS2 alone. */
static int
test_slotless_follow_binds_on_accepted_voice(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    dmr_sm_emit_group_grant(opts, st, VC_A, 0, 900, 1); // slotless
    int rc = expect_true("the slotless grant is followed", vc(st) == VC_A && sm()->vc_slot == -1);
    clock_at(0.1);
    dmr_sm_emit_voice_sync(opts, st, 0); // TS1's gate closed: no accepted voice
    rc |= expect_true("a voice sync with no accepted voice does not bind the follow",
                      sm()->vc_slot == -1 && sm()->vc_media_heard == 0 && st->dmr_mono_slot != 0);
    dsd_call_snapshot call;
    rc |= expect_true("a voice sync with no accepted voice does not publish the grant's call",
                      dsd_call_state_get(st, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    clock_at(0.2);
    voice(opts, st, 1);
    rc |= expect_true("the first accepted voice binds the follow and is its first voice",
                      sm()->vc_slot == 1 && sm()->vc_media_heard == 1 && st->dmr_mono_slot == 1);
    rc |= expect_true("the grant's call is published on the bound slot", dsd_call_state_get(st, 1U, &call) > 0
                                                                             && call.phase == DSD_CALL_PHASE_ACTIVE
                                                                             && call.ota_target_id == 900U);
    return rc;
}

/* A media change's release survives a return the tuner refused: the next tick asks again, though the followed slot's
   voice tail would otherwise keep the carrier. */
static int
test_media_change_release_retries(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    opts->frame_dmr = 1;
    tv_grant(opts, st, LPCN_A, 0, 300, 3);
    clock_at(0.2);
    voice(opts, st, 0);
    clock_at(0.4);
    g_return_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    grant_op(opts, st, 52U, LPCN_A, 0, 300, 9); // TD_GRANT for the followed slot
    int rc = expect_true("a refused return leaves the receiver where it is", sm()->state == DMR_SM_TUNED);
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    clock_at(0.5);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("the next tick returns to the control channel", sm()->state == DMR_SM_ON_CC && g_tunes == 1);
    return rc;
}

/* Data on a voice grant's slot is not its first voice: a p90 voice grant for TS1 is followed (data following on), p10
   voice on TS2 starts, and one data burst arrives on TS1 at 0.1 s. At 2.5 s, its tail over, the p90 call is still not
   on the air within its acquisition window, so a p50 preempt candidate elsewhere waits. */
static int
test_data_burst_is_no_first_voice(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    seed_row(st, 900, "A", 90, 0);
    seed_row(st, 10, "A", 10, 0);
    seed_row(st, 50, "A", 50, 1);
    tv_grant(opts, st, LPCN_A, 0, 900, 1);
    clock_at(0.05);
    voice(opts, st, 1);
    dmr_sm_note_lc_identity(opts, st, 1, DSD_CALL_KIND_GROUP_VOICE, 10, 2);
    clock_at(0.1);
    dmr_sm_emit_data_sync(opts, st, 0);
    clock_at(2.5);
    voice(opts, st, 1);
    tv_grant(opts, st, LPCN_B, 0, 50, 5);
    return expect_true("p50 does not outrank the p90 grant whose slot carried only data", vc(st) == VC_A);
}

/* Companion voice heard before a follow is not the followed grant's: on the composite control channel's carrier, voice
   on TS1 at 3.90 s, then at 3.95 s a grant for TS2 of that carrier, adopted in place, and ticks at 4.05 s and 6.05 s.
   Once the companion is quiet, the grant holds its own acquisition window (to 7.95 s), not a hangtime tail from
   3.90 s. */
static int
test_pre_grant_companion_voice_keeps_acquisition(dsd_opts* opts, dsd_state* st) {
    init_env(opts, st, 2.0);
    opts->frame_dmr = 1; // the DMR tick runs only while DMR trunking can recover
    st->trunk_chan_map[0x0020U] = CC_HZ;
    clock_at(3.90);
    voice(opts, st, 0);
    clock_at(3.95);
    tv_grant(opts, st, 0x0020U, 1, 300, 1); // TS2 of the carrier the receiver is on
    int rc = expect_true("the composite carrier's grant is adopted in place",
                         sm()->state == DMR_SM_TUNED && sm()->vc_freq_hz == CC_HZ && sm()->vc_slot == 1);
    clock_at(4.05);
    dmr_sm_tick_ctx(sm(), opts, st);
    clock_at(6.05);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("pre-grant companion voice ends no acquisition",
                      sm()->state == DMR_SM_TUNED && sm()->vc_freq_hz == CC_HZ && sm()->t_voice_m <= 0.0);
    clock_at(8.0);
    dmr_sm_tick_ctx(sm(), opts, st);
    rc |= expect_true("the acquisition window still closes", sm()->state != DMR_SM_TUNED);
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
    rc |= test_followed_media_change_returns_to_cc(opts, st);
    rc |= test_in_place_group_grant_never_binds_private_call(opts, st);
    rc |= test_in_place_reassignment_carries_crc_provenance(opts, st);
    rc |= test_composite_grant_adopts_resident_calls(opts, st);
    rc |= test_radio_block_keeps_resident_call(opts, st);
    rc |= test_grant_lc_names_the_call(opts, st);
    rc |= test_data_grant_never_binds_a_voice_call(opts, st);
    rc |= test_awaiting_data_grant_never_binds_later_voice(opts, st);
    rc |= test_companion_media_change_is_a_new_call(opts, st);
    rc |= test_companion_voice_keeps_acquiring_slot_held(opts, st);
    rc |= test_pre_grant_companion_voice_keeps_acquisition(opts, st);
    rc |= test_data_burst_is_no_first_voice(opts, st);
    rc |= test_unfollowed_media_change_ends_the_assignment(opts, st);
    rc |= test_media_change_keeps_a_live_companion(opts, st);
    rc |= test_media_change_release_retries(opts, st);
    rc |= test_unfollowed_reassignment_ends_the_assignment(opts, st);
    rc |= test_untrusted_lpcn_mapping_names_no_carrier(opts, st);
    rc |= test_control_channel_confirms_learned_lpcn(opts, st);
    rc |= test_c_bcast_mapping_records_provenance(opts, st);
    rc |= test_composite_payload_counts_as_control_channel(opts, st);
    rc |= test_adopted_companion_takes_its_grant_identity(opts, st);
    rc |= test_retired_slot_starts_fresh(opts, st);
    rc |= test_retired_slot_drops_embedded_lc(opts, st);
    rc |= test_companion_adoption_takes_its_own_channel(opts, st);
    rc |= test_adopted_companion_media_stays_unknown(opts, st);
    rc |= test_rejected_companion_keeps_nothing(opts, st);
    rc |= test_slotless_follow_first_voice_is_heard(opts, st);
    rc |= test_slotless_follow_binds_on_accepted_voice(opts, st);
    rc |= test_grant_origin_values(opts, st);

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
