// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA call records (tetra_calls.h): each CMCE PDU acts on the call its call identifier names, channel allocations
 * attach to the call they came with or to the call of the party they address, and the call to follow is chosen by the
 * talkgroup policy (hold, priority and preemption) and the encryption lockout; the followed call, the heard call and
 * the direct mode call publish as canonical calls, from their signalling and their speech. Blocks are built field by
 * field from the ETSI tables (tetra_test_pdu.h) and decoded by the real MAC; speech goes through the real voice path
 * without a codec; the tuning backend is a pair of stand-in hooks; time is the decode clock's TEST source.
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static dsd_trunk_tune_result g_return_result;

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
    if (g_return_result != DSD_TRUNK_TUNE_RESULT_OK) {
        return g_return_result;
    }
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
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
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
    free(s->state->event_history_s);
    free(s->state);
    free(s->opts);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
}

/* The event history the canonical calls and the notices go to, on network 262/1001 with colour code 25. */
static void
session_history(session* s) {
    s->state->event_history_s = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    assert(s->state->event_history_s != NULL);
    init_event_history(&s->state->event_history_s[0], 0, 255);
    init_event_history(&s->state->event_history_s[1], 0, 255);
    tetra_state_t* const ts = tetra_st(s->state);
    ts->decode_ok = 1U;
    ts->net_known = 1U;
    ts->mcc = 262U;
    ts->mnc = 1001U;
    ts->colour = 25U;
    tetra_publish(s->state);
}

static dsd_call_snapshot
canonical(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    (void)dsd_call_state_get(state, 0U, &call);
    return call;
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
    const uint64_t epoch = canonical(s.state).epoch;
    send_block(&s, 0x100u, &cmce, 0, 0);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0E) != NULL);
    assert(followed_call(s.state) == 0x0E && g_returns == 0);
    /* The call goes on under its new identifier: one canonical call. */
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE && canonical(s.state).epoch == epoch);
    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0E, 5);
    assert(g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* A D-SETUP that reuses the followed call's identifier for another talkgroup is another call: the followed one, whose
 * release went unheard, ends and is let go of, and the new one is judged by the talkgroup policy like any other call
 * -- one of a blocked talkgroup is not followed, one of an allowed talkgroup is. */
static void
test_setup_reusing_followed_id(void) {
    session s = session_open();
    session_history(&s);
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(0x200u, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    assert(dsd_tg_policy_upsert_exact(s.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    const uint64_t first = canonical(s.state).epoch;

    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 130);
    assert(g_tunes == 1 && g_tuned_hz == CARRIER_HZ(120) && g_returns == 1 && followed_call(s.state) == -1);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    assert(c && c->called_ssi == 0x200u && c->alloc.carrier == 130);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.epoch == first && call.phase == DSD_CALL_PHASE_ENDED && call.ota_target_id == 0x100u);

    send_setup(&s, 0x300u, 0x0A, TTB_SPEECH_GROUP, 2, 140);
    assert(g_tunes == 2 && g_tuned_hz == CARRIER_HZ(140) && followed_call(s.state) == 0x0A);
    call = canonical(s.state);
    assert(call.epoch != first && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0x300u);

    /* A return the backend refuses keeps the receiver on the old call's channel, and the new call's allocation is
     * still not followed as if it were the old call's. */
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
    assert(g_returns == 2 && g_tunes == 2 && g_tuned_hz == CARRIER_HZ(140));
    assert(tetra_calls_peek(s.state, 0x0A)->called_ssi == 0x200u);
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

/* D-TX GRANTED (table 14.18) of call @p call_id to another user, @p talker transmitting. */
static void
send_tx_granted(session* s, uint32_t ssi, uint16_t call_id, uint32_t talker) {
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_TX_GRANTED, 5);
    ttb_put(&cmce, call_id, 14);
    ttb_put(&cmce, 3, 2); /* granted to another user */
    ttb_put(&cmce, 0, 3); /* request permission, encryption control, reserved */
    ttb_put(&cmce, 1, 1); /* O-bit */
    ttb_put(&cmce, 0, 1); /* no notification indicator */
    ttb_put(&cmce, 1, 1); /* transmitting party type identifier present */
    ttb_put(&cmce, 1, 2); /* SSI */
    ttb_put(&cmce, talker, 24);
    ttb_put(&cmce, 0, 1); /* M-bit */
    send_block(s, ssi, &cmce, 0, 0);
}

/* The followed call is the canonical call: its called group, its talker, its traffic channel and basic service, from
 * its CRC-passed signalling; D-TX CEASED and D-RELEASE end it as terminators, a new talker starts the next
 * transmission, and the state machine leaving a call ends it as a teardown. Its event history row names TETRA, the
 * network and the colour code. */
static void
test_followed_call_publishes(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && DSD_SYNC_IS_TETRA(call.protocol));
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE && call.ota_target_id == 0x100u && call.policy_target_id == 0x100u);
    assert(call.channel == 120U && call.frequency_hz == CARRIER_HZ(120) && call.service_options == TTB_SPEECH_GROUP);
    assert(call.crypto == DSD_CALL_CRYPTO_CLEAR && call.audio_permitted == 1U);
    const Event_History* row = &s.state->event_history_s[0].Event_History_Items[0];
    assert(strstr(row->event_string, "TETRA TGT: 00000256; SRC: 00000000; CC: 25; CARRIER: 120;") != NULL);
    assert(strstr(row->event_string, "Group;") != NULL && strcmp(row->sysid_string, "TETRA_262_1001_CC_25") == 0);
    const uint64_t first = call.epoch;

    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == first && call.ota_source_id == 0x4444u);

    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_TX_CEASED, 0x0A, 1);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_TERMINATOR && call.epoch == first);
    /* A late-entry D-SETUP of the call while no one transmits opens nothing. */
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);

    send_tx_granted(&s, 0x100u, 0x0A, 0x5555u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != first && call.ota_source_id == 0x5555u);

    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0A, 5);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_TERMINATOR && g_returns == 1);

    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_P2P, 2, 130);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_PRIVATE_VOICE
           && call.ota_target_id == 0x200u);
    tetra_sm_abandon_carrier(s.opts, s.state);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_EXPLICIT);
    session_close(&s);
}

/* An end-to-end encrypted call publishes as encrypted, without audio; a call no one follows publishes nothing. */
static void
test_encrypted_and_unfollowed_calls(void) {
    session s = session_open();
    session_history(&s);
    s.opts->trunk_enable = 0;
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 120);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_IDLE);
    s.opts->trunk_enable = 1;
    tetra_sm_on_cc_sync(s.opts, s.state);
    send_setup(&s, 0x300u, 0x0D, TTB_SPEECH_GROUP | 0x10u, 4, 120);
    const dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING
           && call.audio_permitted == 0U);
    assert(strstr(s.state->event_history_s[0].Event_History_Items[0].event_string, "ENC;") != NULL);
    session_close(&s);
}

/* D-SDS-DATA (table 14.13) with a simple text message (EN 300 392-2 clause 29.5.2, Latin-1). */
static void
send_sds_text(session* s, uint32_t dst, uint32_t src, const char* text) {
    const int text_bits = (int)strlen(text) * 8;
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_SDS_DATA, 5);
    ttb_put(&cmce, 1, 2); /* calling party type identifier: SSI */
    ttb_put(&cmce, src, 24);
    ttb_put(&cmce, 3, 2);                           /* short data type identifier: user defined data-4 */
    ttb_put(&cmce, (uint32_t)(16 + text_bits), 11); /* length indicator */
    ttb_put(&cmce, 2, 8);                           /* protocol identifier: simple text messaging */
    ttb_put(&cmce, 0, 1);                           /* reserved */
    ttb_put(&cmce, 1, 7);                           /* text coding scheme: ISO/IEC 8859-1 */
    for (const char* c = text; *c != '\0'; ++c) {
        ttb_put(&cmce, (uint32_t)(unsigned char)*c, 8);
    }
    ttb_put(&cmce, 0, 1); /* O-bit */
    send_block(s, dst, &cmce, 0, 0);
}

/* Short data becomes a data notice, its text the message; a status a control notice. */
static void
test_sds_and_status_notices(void) {
    session s = session_open();
    session_history(&s);
    send_sds_text(&s, 0x101u, 0x2222u, "Hi there");
    const Event_History* committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(committed->category == DSD_EVENT_CATEGORY_DATA && committed->source_id == 0x2222u
           && committed->target_id == 0x101u);
    assert(strstr(committed->event_string, "SDS SRC: 8738; TGT: 257;") != NULL);
    assert(strcmp(committed->text_message, "Hi there") == 0);

    /* D-STATUS (table 14.14): calling party SSI and a 16-bit pre-coded status. */
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_STATUS, 5);
    ttb_put(&cmce, 1, 2);
    ttb_put(&cmce, 0x3333u, 24);
    ttb_put(&cmce, 0x8001u, 16);
    ttb_put(&cmce, 0, 1); /* O-bit */
    send_block(&s, 0x102u, &cmce, 0, 0);
    committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(committed->category == DSD_EVENT_CATEGORY_CONTROL
           && strstr(committed->event_string, "STATUS SRC: 13107; TGT: 258; STATUS: 0x8001;") != NULL);

    /* D-DISABLE (EN 300 392-7 table A.31): confirm, permanent, subscription 262/1001 SSI 0x1234. */
    ttb_block mm = {0};
    ttb_put(&mm, 3, 4); /* PDU type: D-DISABLE */
    ttb_put(&mm, 1, 1); /* intent/confirm: confirm */
    ttb_put(&mm, 1, 1); /* disabling type: permanent */
    ttb_put(&mm, 0, 1); /* equipment disable */
    ttb_put(&mm, 1, 1); /* subscription disable */
    ttb_put(&mm, 262, 10);
    ttb_put(&mm, 1001, 14);
    ttb_put(&mm, 0x1234u, 24);
    ttb_put(&mm, 0, 1); /* O-bit */
    tetra_mm_dispatch(mm.bits, mm.n, 1, s.opts, s.state);
    committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(committed->category == DSD_EVENT_CATEGORY_CONTROL && committed->target_id == 0x1234u
           && strstr(committed->event_string, "D-DISABLE CONFIRM; SSI: 4660; Permanent;") != NULL);

    /* Nothing is noted before a CRC-passed block proved the carrier. */
    tetra_confirm_reset(s.state);
    ttb_block sds = {0};
    ttb_put(&sds, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&sds, TETRA_CMCE_D_STATUS, 5);
    ttb_put(&sds, 1, 2);
    ttb_put(&sds, 0x4444u, 24);
    ttb_put(&sds, 0x8002u, 16);
    ttb_put(&sds, 0, 1);
    tetra_mle_dispatch(sds.bits, sds.n, 1, s.opts, s.state);
    assert(strstr(s.state->event_history_s[0].Event_History_Items[1].event_string, "0x8002") == NULL);
    session_close(&s);
}

/* A real direct mode DSB (EN 300 396-3 table 21 SCH/S, table 22 SCH/H): DM-OCCUPIED of a clear group speech call,
 * source 100, destination 0x800001 (the vector of TETRA_DMO). */
static const char* k_dmo_sch_s = "110100000000000001100000000000000000000000000000000000000000";
static const char* k_dmo_sch_h =
    "00000000001000001000000000000000000000010000000000000000000110010010111100010010010011100"
    "10110101110000000000000000"
    "001000100";

static void
bits_of(const char* text, uint8_t* bits) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        bits[i] = (uint8_t)(text[i] == '1');
    }
}

/* The DSB as the decoder takes it: SCH/S, SCH/H settled to the security class @p enc_state, then the call records. */
static void
dmo_dsb(session* s, uint8_t enc_state) {
    uint8_t bits[128];
    DSD_MEMSET(bits, 0, sizeof bits);
    bits_of(k_dmo_sch_s, bits);
    tetra_dmo_sync_s_t sync_s;
    (void)tetra_dmo_parse_sync_s(bits, &sync_s);
    sync_s.enc_state = enc_state;
    DSD_MEMSET(bits, 0, sizeof bits);
    bits_of(k_dmo_sch_h, bits);
    tetra_dmo_sync_h_t sync_h;
    (void)tetra_dmo_parse_sync_h(bits, 0, &sync_h);
    tetra_dmo_apply_sync(s->state, &sync_s, &sync_h);
    tetra_calls_dmo_pdu(s->opts, s->state, sync_s.enc_state, &sync_h);
}

/* A DMAC-DATA of the channel with message type @p msg_type from @p src to @p dst. */
static void
dmo_data(session* s, uint8_t msg_type, uint32_t src, uint32_t dst, uint64_t sds_value) {
    tetra_dmo_sync_h_t h;
    DSD_MEMSET(&h, 0, sizeof h);
    h.has_addrs = 1;
    h.md_ok = 1;
    h.msg_type = msg_type;
    h.src = src;
    h.dst = dst;
    h.sds_value = sds_value;
    h.e[TETRA_DMO_E_SDTI] = 4; /* pre-coded status */
    tetra_calls_dmo_pdu(s->opts, s->state, -1, &h);
}

/* A direct mode call publishes from its DM-OCCUPIED with what the security class leaves clear, trunking on or off, and
 * ends at DM-TX CEASED or when nothing of it is heard for TETRA_CALL_DMO_IDLE_S. */
static void
test_direct_mode_calls(void) {
    session s = session_open();
    session_history(&s);
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && DSD_SYNC_IS_TETRA(call.protocol));
    assert(call.kind == DSD_CALL_KIND_GROUP_VOICE && call.ota_target_id == 0x800001u && call.ota_source_id == 100u);
    assert(call.crypto == DSD_CALL_CRYPTO_CLEAR);
    dsd_event_sync_slot(s.opts, s.state, 0U);
    const Event_History* row = &s.state->event_history_s[0].Event_History_Items[0];
    assert(strcmp(row->sysid_string, "TETRA_DMO") == 0
           && strstr(row->event_string, "TETRA DMO TGT: 08388609; SRC: 00000100;"));

    dmo_data(&s, TETRA_DMO_MSG_TX_CEASED, 100u, 0x800001u, 0);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_TERMINATOR);

    /* DM-2-B hides the destination and the DM-SDU (its call type). */
    dmo_dsb(&s, TETRA_DMO_AIE_DM2B);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0u && call.ota_source_id == 100u);
    assert(call.kind == DSD_CALL_KIND_VOICE && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S + 0.5);
    tetra_calls_tick(s.opts, s.state);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_SYNC_LOSS);

    /* DM-2-C hides the addresses and the message type: an encrypted call of no one known. Past the reacquisition
     * window, so it is not taken for the call that faded. */
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S + 0.5 + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    dmo_dsb(&s, TETRA_DMO_AIE_DM2C);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0u && call.ota_source_id == 0u
           && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    session_close(&s);
}

/* A SYNC PDU (EN 300 392-2 table 21.76) of network 262/1001 with colour code @p colour, timeslot 1 of frame 1 of
 * multiframe 1. */
static void
bsch_of(ttb_block* b, uint32_t colour) {
    ttb_put(b, 0x4u, 4); /* system code */
    ttb_put(b, colour, 6);
    ttb_put(b, 0, 2); /* timeslot 1 */
    ttb_put(b, 1, 5); /* frame 1 */
    ttb_put(b, 1, 6); /* multiframe 1 */
    ttb_put(b, 0, 8); /* sharing mode, reserved frames, U-plane DTX, frame 18 extension, reserved */
    ttb_put(b, 262, 10);
    ttb_put(b, 1001, 14);
    ttb_put(b, 0, 5); /* neighbour cell broadcast, cell load CA, late entry supported */
}

/* An event takes the colour code of the carrier from what a synchronisation burst just set, not from the last burst's
 * view: the direct mode call a DSB sets up after a trunked cell, and its short data, carry no colour code, and short
 * data decoded after the BSCH of another cell carries that cell's. */
static void
test_events_read_the_new_carrier(void) {
    session s = session_open();
    session_history(&s);
    assert(s.state->tetra_pub.mode == DSD_TETRA_MODE_TMO_DOWNLINK && s.state->tetra_pub.colour == 25U);
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    const Event_History* const staged = &s.state->event_history_s[0].Event_History_Items[0];
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE && staged->target_id == 0x800001u);
    assert(staged->access_code_kind == DSD_ACCESS_CODE_NONE && strcmp(staged->sysid_string, "TETRA_DMO") == 0);
    dmo_data(&s, TETRA_DMO_MSG_SDS_DATA, 200u, 300u, 0x1234u);
    const Event_History* committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(strstr(committed->event_string, "DMO SDS SRC: 200; TGT: 300;") != NULL);
    assert(committed->access_code_kind == DSD_ACCESS_CODE_NONE);

    ttb_block sync = {0};
    bsch_of(&sync, 30u);
    tetra_st(s.state)->dmo = 0; /* as a BSCH with a trunked system code leaves it */
    assert(tetra_bsch_parse(sync.bits, sync.n, s.opts, s.state) == 1);
    send_sds_text(&s, 0x101u, 0x2222u, "cell");
    committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(strcmp(committed->text_message, "cell") == 0);
    assert(committed->access_code_kind == DSD_ACCESS_CODE_COLOR_CODE && committed->access_code == 30U);
    session_close(&s);
}

/* Direct mode short data becomes a data notice; the repeats a DMAC-SYNC sends are noted once. */
static void
test_direct_mode_short_data(void) {
    session s = session_open();
    session_history(&s);
    const uint64_t pushes = s.state->event_history_s[0].push_seq;
    dmo_data(&s, TETRA_DMO_MSG_SDS_DATA, 200u, 300u, 0x1234u);
    const Event_History* committed = &s.state->event_history_s[0].Event_History_Items[1];
    assert(committed->category == DSD_EVENT_CATEGORY_DATA && committed->source_id == 200u
           && committed->target_id == 300u && strstr(committed->event_string, "DMO SDS SRC: 200; TGT: 300;") != NULL);
    const uint64_t after_one = s.state->event_history_s[0].push_seq;
    assert(after_one > pushes);
    dmo_data(&s, TETRA_DMO_MSG_SDS_DATA, 200u, 300u, 0x1234u);
    assert(s.state->event_history_s[0].push_seq == after_one);
    dmo_data(&s, TETRA_DMO_MSG_SDS_DATA, 200u, 300u, 0x1235u);
    assert(s.state->event_history_s[0].push_seq > after_one);
    session_close(&s);
}

/* One good full-slot speech burst on timeslot @p tn, through the voice path (no codec is loaded). */
static void
speech(session* s, uint8_t tn) {
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    DSD_MEMSET(bits, 0, sizeof bits);
    tetra_state_t* const ts = tetra_st(s->state);
    ts->tdma_valid = 1;
    ts->tn = tn;
    tetra_voice_full_slot(s->opts, s->state, bits, 1);
}

/* Speech of the followed call, once its signalling proved it on its carrier, marks its canonical call's media and keeps
 * the state machine on it; speech that trails its D-TX CEASED opens nothing; and a followed call whose speech the
 * listener cannot decrypt is locked out and left while encrypted calls are not followed. */
static void
test_followed_call_speech(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of carrier 120 */
    assert(followed_call(s.state) == 0x0A && canonical(s.state).media_active == 0U);
    tetra_confirm_forget_followed_call(s.state); /* as the retune to carrier 120 left it */
    speech(&s, 2);
    assert(canonical(s.state).media_active == 0U && tetra_st(s.state)->sm.t_voice_m <= 0.0);

    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    speech(&s, 3);
    assert(canonical(s.state).media_active == 0U && tetra_st(s.state)->sm.t_voice_m <= 0.0);
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.media_active == 1U && call.ota_source_id == 0x4444u);
    assert(tetra_st(s.state)->sm.t_voice_m > 0.0);

    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_TX_CEASED, 0x0A, 1);
    const uint64_t ended = canonical(s.state).epoch;
    speech(&s, 2);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.epoch == ended);

    send_tx_granted(&s, 0x100u, 0x0A, 0x5555u);
    s.opts->trunk_tune_enc_calls = 0;
    tetra_calls_find(s.state, 0x0A)->air_enc = TETRA_ENC_MODE_EVEN; /* no key for the network */
    speech(&s, 2);
    assert(followed_call(s.state) == -1 && g_returns == 1);
    assert(dsd_enc_lockout_entry_active(s.state, 0x100u, 1) == 1);
    session_close(&s);
}

/* Not trunking, the heard speech's call publishes: the record whose traffic channel is its timeslot of the tuned
 * carrier, updated by the call's signalling and ended when its speech stops; else an anonymous call, only on a carrier a
 * CRC-16 block proved. */
static void
test_heard_call_speech(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100); /* TN2 of the carrier being received */
    assert(canonical(s.state).phase == DSD_CALL_PHASE_IDLE);
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0x300u && call.channel == 100U
           && call.media_active == 1U);
    const uint64_t epoch = call.epoch;
    send_tx_granted(&s, 0x300u, 0x0C, 0x4444u);
    call = canonical(s.state);
    assert(call.epoch == epoch && call.ota_source_id == 0x4444u);

    /* D-TX CEASED ends the transmission; the speech that trails it opens nothing, the next grant opens the next. */
    send_call_pdu(&s, 0x300u, TETRA_CMCE_D_TX_CEASED, 0x0C, 1);
    speech(&s, 2);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_TERMINATOR && call.epoch == epoch);
    send_tx_granted(&s, 0x300u, 0x0C, 0x5555u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != epoch && call.ota_source_id == 0x5555u);

    at_seconds(1000.0 + TETRA_CALL_SPEECH_END_S);
    tetra_calls_tick(s.opts, s.state);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_SYNC_LOSS);
    send_tx_granted(&s, 0x300u, 0x0C, 0x6666u);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);

    at_seconds(1000.0 + TETRA_CALL_SPEECH_END_S + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    speech(&s, 3);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_VOICE && call.ota_target_id == 0u
           && call.media_active == 1U && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    at_seconds(1000.0 + 2.0 * TETRA_CALL_SPEECH_END_S + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    tetra_calls_tick(s.opts, s.state);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);

    at_seconds(1000.0 + 2.0 * TETRA_CALL_SPEECH_END_S + 4.0 * DSD_CALL_REACQUIRE_GAP_S);
    tetra_confirm_reset(s.state);
    speech(&s, 3);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    session_close(&s);
}

/* Speech of the direct mode call keeps it from ending idle; with trunking on, it is heard all the same. */
static void
test_direct_mode_speech(void) {
    session s = session_open();
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    const uint64_t epoch = canonical(s.state).epoch;
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S - 0.5);
    speech(&s, 1);
    assert(canonical(s.state).media_active == 1U);
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S + 0.5);
    tetra_calls_tick(s.opts, s.state);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch);
    at_seconds(1000.0 + 2.0 * TETRA_CALL_DMO_IDLE_S);
    tetra_calls_tick(s.opts, s.state);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    session_close(&s);
}

/* The carrier being received, for TB5: the followed call's, the main carrier on the control channel, else the one the
 * receiver is tuned to exactly; none between carriers, in direct mode or off any carrier. */
static void
test_receiving_carrier(void) {
    session s = session_open();
    tetra_state_t* const ts = tetra_st(s.state);
    uint16_t cn = 0;
    ts->dl_carrier_hz = CARRIER_HZ(100);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 100);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 120);
    ts->sm.pending_request_id = 9;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    ts->sm.pending_request_id = 0;

    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(130);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 130);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(130) + 6250U;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(130);
    ts->freq_band = 0;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    ts->freq_band = 4;
    ts->dmo = 1;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    ts->dmo = 0;
    assert(tetra_calls_carrier(s.opts, s.state, NULL) == 0 && tetra_calls_carrier(s.opts, NULL, &cn) == 0);
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
    test_setup_reusing_followed_id();
    test_table_eviction_and_ageing();
    test_followed_call_publishes();
    test_encrypted_and_unfollowed_calls();
    test_sds_and_status_notices();
    test_direct_mode_calls();
    test_direct_mode_short_data();
    test_events_read_the_new_carrier();
    test_followed_call_speech();
    test_heard_call_speech();
    test_direct_mode_speech();
    test_receiving_carrier();
    DSD_FPRINTF(stdout, "TETRA_CALLS: OK\n");
    return 0;
}
