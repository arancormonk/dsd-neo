// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA call records (tetra_calls.h): each CMCE PDU acts on the call its call identifier names, channel allocations
 * attach to the call they came with or to the call of the party they address, and the call to follow is chosen by the
 * talkgroup policy (hold, priority and preemption) and the encryption lockout. Blocks are built field by field from the
 * ETSI tables (tetra_test_pdu.h) and decoded by the real MAC; the tuning backend is a pair of stand-in hooks; time is
 * the decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"

#define SECOND_NS     1000000000ULL

/* Downlink frequency of carrier @p n in band 4 with offset -6,25 kHz (EN 300 392-2 clause 21.4.4.1). */
#define CARRIER_HZ(n) (400000000L + ((long)(n) * 25000L) - 6250L)

static int g_tunes;
static int g_returns;
static long g_tuned_hz;

static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    g_tunes++;
    g_tuned_hz = freq;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_hook(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_returns++;
    opts->trunk_is_tuned = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
at_seconds(double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * (double)SECOND_NS));
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} session;

/* A control channel on carrier 100 of band 4 that a CRC-passed block has proved, following every kind of call. */
static session
session_open(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_hook;
    hooks.return_to_cc_request = return_hook;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_trunk_tuning_requests_reset();
    dsd_decode_clock_use_test(SECOND_NS * 1000ULL);
    at_seconds(1000.0);
    g_tunes = 0;
    g_returns = 0;
    g_tuned_hz = 0;
    session s;
    s.opts = (dsd_opts*)calloc(1U, sizeof(*s.opts));
    s.state = (dsd_state*)calloc(1U, sizeof(*s.state));
    assert(s.opts != NULL && s.state != NULL);
    s.opts->trunk_enable = 1;
    s.opts->trunk_hangtime = 2.0F;
    s.opts->trunk_tune_group_calls = 1;
    s.opts->trunk_tune_private_calls = 1;
    s.opts->trunk_tune_data_calls = 1;
    s.opts->trunk_tune_enc_calls = 1;
    tetra_state_t* const ts = tetra_st(s.state);
    assert(ts != NULL);
    ts->freq_band = 4;
    ts->freq_offset = 2;
    s.state->trunk_cc_freq = CARRIER_HZ(100);
    tetra_sm_on_cc_sync(s.opts, s.state);
    tetra_confirm_note_signalling(s.state);
    return s;
}

static void
session_close(session* s) {
    dsd_state_ext_free_all(s->state);
    free(s->state);
    free(s->opts);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* A MAC-RESOURCE to @p ssi carrying @p cmce (the CMCE PDU after its protocol discriminator, @p cmce_bits long) and,
 * when @p timeslots is not 0, a channel allocation on @p carrier; decoded as one SCH/F block. */
static void
send_block(session* s, uint32_t ssi, const ttb_block* cmce, uint32_t timeslots, uint32_t carrier) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI, ssi);
    ttb_resource_flags(&b, timeslots != 0u ? 1u : 0u);
    if (timeslots != 0u) {
        ttb_allocation(&b, timeslots, 1, carrier);
    }
    if (cmce) {
        ttb_put(&b, 2, 4); /* BL-UDATA */
        ttb_put_bits(&b, cmce->bits, cmce->n);
    }
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    /* A CRC-passed block before this one proved the carrier it is on (tetra_confirm.h). */
    tetra_confirm_note_signalling(s->state);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* D-SETUP (table 14.15) of call @p call_id with @p basic_service, to @p ssi with an allocation of @p timeslots on
 * @p carrier (none when 0). */
static void
send_setup(session* s, uint32_t ssi, uint16_t call_id, uint32_t basic_service, uint32_t timeslots, uint32_t carrier) {
    ttb_block cmce = {0};
    ttb_d_setup(&cmce, call_id, basic_service);
    send_block(s, ssi, &cmce, timeslots, carrier);
}

/* A CMCE PDU of call @p call_id whose remaining type-1 elements are all zero: D-CONNECT, D-RELEASE and the like. */
static void
send_call_pdu(session* s, uint32_t ssi, uint32_t pdu_type, uint16_t call_id, int type1_bits) {
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, pdu_type, 5);
    ttb_put(&cmce, call_id, 14);
    ttb_put(&cmce, 0, type1_bits);
    ttb_put(&cmce, 0, 1); /* O-bit */
    send_block(s, ssi, &cmce, 0, 0);
}

static int
followed_call(const dsd_state* state) {
    tetra_sm_grant g;
    return tetra_sm_followed(state, &g) ? (int)g.call_id : -1;
}

/* A PDU names one call: a D-CONNECT or D-RELEASE of another call leaves the followed one as it is. */
static void
test_pdus_act_on_their_own_call(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(g_tunes == 1 && g_tuned_hz == CARRIER_HZ(120) && followed_call(s.state) == 0x0A);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const a = tetra_calls_peek(s.state, 0x0A);
    const tetra_call_t* const b = tetra_calls_peek(s.state, 0x0B);
    assert(a && b && a->phase == TETRA_CALL_SETUP && b->phase == TETRA_CALL_SETUP);

    send_call_pdu(&s, 0x200u, TETRA_CMCE_D_CONNECT, 0x0B, 4 + 1 + 1 + 2 + 1 + 1);
    assert(b->phase == TETRA_CALL_CONNECTED && a->phase == TETRA_CALL_SETUP);
    assert(followed_call(s.state) == 0x0A && g_returns == 0);

    /* D-RELEASE (table 14.12): call identifier and a 5-bit disconnect cause. */
    send_call_pdu(&s, 0x200u, TETRA_CMCE_D_RELEASE, 0x0B, 5);
    assert(tetra_calls_peek(s.state, 0x0B) == NULL && tetra_calls_peek(s.state, 0x0A) != NULL);
    assert(followed_call(s.state) == 0x0A && g_returns == 0);
    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0C, 5);
    assert(followed_call(s.state) == 0x0A && g_returns == 0);

    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0A, 5);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* Only a call whose D-SETUP named its called party is followed: one addressed by event label alone is not. */
static void
test_unknown_party_never_tunes(void) {
    session s = session_open();
    tetra_call_event setup;
    DSD_MEMSET(&setup, 0, sizeof setup);
    setup.kind = TETRA_CALL_EV_SETUP;
    setup.call_id = 0x0D;
    setup.basic_service = TTB_SPEECH_GROUP;
    tetra_mac_sdu sdu;
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_EVENT_LABEL;
    sdu.addr.event_label = 0x55;
    sdu.has_addr = 1;
    sdu.has_alloc = 1;
    sdu.alloc.timeslots = 4;
    sdu.alloc.up_down = 1;
    sdu.alloc.carrier = 120;
    sdu.alloc.freq_hz = CARRIER_HZ(120);
    tetra_calls_on_event(s.opts, s.state, &setup, &sdu);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0D);
    assert(c && c->called_ssi == 0u && c->alloc.carrier == 120 && sdu.alloc_taken);
    assert(g_tunes == 0 && followed_call(s.state) == -1);
    session_close(&s);
}

/* A grant for the held talkgroup takes the receiver from a call of another one. */
static void
test_hold_takes_over(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A);
    s.state->tg_hold = 0x200u;
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 140);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 2, 130);
    assert(followed_call(s.state) == 0x0B && g_tunes == 2 && g_tuned_hz == CARRIER_HZ(130));
    session_close(&s);
}

static void
policy_row(dsd_state* state, uint32_t id, int priority, int preempt) {
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(id, "A", "test", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    row.priority = priority;
    row.preempt = (uint8_t)preempt;
    assert(dsd_tg_policy_upsert_exact(state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
}

/* A call whose policy row asks to preempt takes the receiver from a lower priority call once that one has been
 * followed for the minimum dwell; without the flag it does not. */
static void
test_priority_preemption(void) {
    session s = session_open();
    policy_row(s.state, 0x100u, 10, 0);
    policy_row(s.state, 0x200u, 50, 0);
    policy_row(s.state, 0x300u, 50, 1);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A);
    at_seconds(1002.0);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 2, 130);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 2, 140);
    assert(followed_call(s.state) == 0x0C && g_tunes == 2 && g_tuned_hz == CARRIER_HZ(140));
    session_close(&s);
}

/* The followed call becomes end-to-end encrypted (D-TX GRANTED encryption control, table 14.18) while encrypted calls
 * are not followed: its group is locked out and the receiver goes back to the control channel. */
static void
test_followed_call_turns_encrypted(void) {
    session s = session_open();
    s.opts->trunk_tune_enc_calls = 0;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && !dsd_enc_lockout_entry_active(s.state, 0x100u, 1));
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_TX_GRANTED, 5);
    ttb_put(&cmce, 0x0A, 14);
    ttb_put(&cmce, 3, 2); /* granted to another user */
    ttb_put(&cmce, 0, 1); /* request permission */
    ttb_put(&cmce, 1, 1); /* encryption control: end-to-end encrypted */
    ttb_put(&cmce, 0, 1); /* reserved */
    ttb_put(&cmce, 0, 1); /* O-bit */
    send_block(&s, 0x100u, &cmce, 0, 0);
    assert(tetra_calls_peek(s.state, 0x0A)->e2ee == 1);
    assert(dsd_enc_lockout_entry_active(s.state, 0x100u, 1) && g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* D-INFO (table 14.11) gives the followed call a new identifier: it is followed under that one, which releases it. */
static void
test_followed_call_renamed(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_INFO, 5);
    ttb_put(&cmce, 0x0A, 14);
    ttb_put(&cmce, 0, 2); /* reset call time-out timer, poll request */
    ttb_put(&cmce, 1, 1); /* O-bit */
    ttb_put(&cmce, 1, 1); /* new call identifier present */
    ttb_put(&cmce, 0x0E, 14);
    ttb_put(&cmce, 0, 9); /* the nine other type-2 elements absent */
    ttb_put(&cmce, 0, 1); /* M-bit */
    send_block(&s, 0x100u, &cmce, 0, 0);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0E) != NULL);
    assert(followed_call(s.state) == 0x0E && g_returns == 0);
    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0E, 5);
    assert(g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* The table holds TETRA_CALLS_MAX calls: a new one takes the place of the call heard from least recently, never the
 * followed one, and calls no PDU names for TETRA_CALL_IDLE_S are dropped. */
static void
test_table_eviction_and_ageing(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 1, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 1);
    for (uint16_t id = 2; id <= TETRA_CALLS_MAX + 1; id++) {
        at_seconds(1000.0 + id);
        send_setup(&s, 0x100u + id, id, TTB_SPEECH_GROUP, 0, 0);
    }
    assert(tetra_calls_peek(s.state, 1) != NULL && tetra_calls_peek(s.state, 2) == NULL);
    assert(tetra_calls_peek(s.state, TETRA_CALLS_MAX + 1) != NULL);

    at_seconds(1000.0 + TETRA_CALLS_MAX + 1 + TETRA_CALL_IDLE_S);
    tetra_calls_tick(s.opts, s.state);
    assert(tetra_calls_peek(s.state, TETRA_CALLS_MAX + 1) == NULL);
    assert(tetra_calls_peek(s.state, 1) != NULL);
    session_close(&s);
}

int
main(void) {
    test_pdus_act_on_their_own_call();
    test_unknown_party_never_tunes();
    test_hold_takes_over();
    test_priority_preemption();
    test_followed_call_turns_encrypted();
    test_followed_call_renamed();
    test_table_eviction_and_ageing();
    DSD_FPRINTF(stdout, "TETRA_CALLS: OK\n");
    return 0;
}
