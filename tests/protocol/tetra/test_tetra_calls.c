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
#include "tetra_voice_gate.h"

#define SECOND_NS     1000000000ULL

/* Downlink frequency of carrier @p n in band 4 with offset -6,25 kHz (EN 300 392-2 clause 21.4.4.1). */
#define CARRIER_HZ(n) (400000000L + ((long)(n) * 25000L) - 6250L)

static int g_tunes;
static int g_returns;
static long g_tuned_hz;
static dsd_trunk_tune_result g_tune_result;
static dsd_trunk_tune_result g_return_result;
static int g_tunes_failing_at_once; /* the next tunes the backend reports in flight and answers FAILED at once */

static dsd_trunk_tune_result
tune_hook(dsd_opts* opts, dsd_state* state, long freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    g_tunes++;
    g_tuned_hz = freq;
    if (g_tunes_failing_at_once > 0) {
        g_tunes_failing_at_once--;
        dsd_trunk_tuning_request_publish(request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
        return DSD_TRUNK_TUNE_RESULT_PENDING;
    }
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return g_tune_result;
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
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_tunes_failing_at_once = 0;
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
 * when @p timeslots is not 0, a channel allocation on @p carrier; decoded as one SCH/F block, the first CRC-passed
 * block on its carrier unless @p proven says one before it proved the carrier (tetra_confirm.h). */
static void
send_block_on(session* s, uint32_t ssi, const ttb_block* cmce, uint32_t timeslots, uint32_t carrier, int proven) {
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
    if (proven) {
        tetra_confirm_note_signalling(s->state);
    }
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* send_block_on() after a CRC-passed block that proved the carrier. */
static void
send_block(session* s, uint32_t ssi, const ttb_block* cmce, uint32_t timeslots, uint32_t carrier) {
    send_block_on(s, ssi, cmce, timeslots, carrier, 1);
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

/* A MAC-RESOURCE to @p ssi that assigns it event label @p label (address type 101, EN 300 392-2 table 21.55), with no
 * TM-SDU; when @p timeslots is not 0, with a channel allocation on @p carrier. */
static void
send_event_label(session* s, uint32_t ssi, uint32_t label, uint32_t timeslots, uint32_t carrier) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI_EVENT, ssi);
    ttb_put(&b, label, 10);
    ttb_resource_flags(&b, timeslots != 0u ? 1u : 0u);
    if (timeslots != 0u) {
        ttb_allocation(&b, timeslots, 1, carrier);
    }
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* A MAC-RESOURCE to @p ssi that assigns it event label @p label with a "go to the control channel" allocation
 * (timeslots 0000, table 21.87). */
static void
send_event_label_to_cc(session* s, uint32_t ssi, uint32_t label) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_SSI_EVENT, ssi);
    ttb_put(&b, label, 10);
    ttb_resource_flags(&b, 1u);
    ttb_allocation(&b, 0, 1, 100);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* A MAC-RESOURCE addressed by event label @p label alone, with a channel allocation on @p carrier and no TM-SDU. */
static void
send_by_event_label(session* s, uint32_t label, uint32_t timeslots, uint32_t carrier) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, TETRA_MAC_ADDR_EVENT_LABEL, 0u);
    ttb_put(&b, label, 10);
    ttb_resource_flags(&b, 1u);
    ttb_allocation(&b, timeslots, 1, carrier);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
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

/* D-INFO (table 14.11) of call @p call_id giving it the identifier @p new_id, as the first CRC-passed block on its
 * carrier unless @p proven. */
static void
send_info_rename_on(session* s, uint32_t ssi, uint16_t call_id, uint16_t new_id, int proven) {
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_INFO, 5);
    ttb_put(&cmce, call_id, 14);
    ttb_put(&cmce, 0, 2); /* reset call time-out timer, poll request */
    ttb_put(&cmce, 1, 1); /* O-bit */
    ttb_put(&cmce, 1, 1); /* new call identifier present */
    ttb_put(&cmce, new_id, 14);
    ttb_put(&cmce, 0, 9); /* the nine other type-2 elements absent */
    ttb_put(&cmce, 0, 1); /* M-bit */
    send_block_on(s, ssi, &cmce, 0, 0, proven);
}

static void
send_info_rename(session* s, uint32_t ssi, uint16_t call_id, uint16_t new_id) {
    send_info_rename_on(s, ssi, call_id, new_id, 1);
}

/* D-INFO gives the followed call a new identifier: it is followed under that one, which releases it. */
static void
test_followed_call_renamed(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    const uint64_t epoch = canonical(s.state).epoch;
    send_info_rename(&s, 0x100u, 0x0A, 0x0E);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0E) != NULL);
    assert(followed_call(s.state) == 0x0E && g_returns == 0);
    /* The call goes on under its new identifier: one canonical call. */
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE && canonical(s.state).epoch == epoch);
    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0E, 5);
    assert(g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* The followed call renamed (D-INFO, or the D-CALL RESTORE of a D-RESTORE-ACK) by the first CRC-passed block decoded
 * on its traffic carrier, before anything else proved the carrier: a rename names the call followed already, it is no
 * grant to admit, so the state machine follows the call under its new identifier, whose release returns to the control
 * channel. Proved or not, the rename restarts none of its timers: the hangtime its speech started goes on. */
static void
test_followed_call_renamed_first_on_its_carrier(void) {
    for (int pass = 0; pass < 3; pass++) {
        const int restore = pass == 1;
        session s = session_open();
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        assert(followed_call(s.state) == 0x0A && g_tunes == 1 && !tetra_confirm_carrier_proven(s.state));
        at_seconds(1001.0);
        tetra_sm_note_voice(s.state, 2U);
        at_seconds(1001.5);
        if (restore) {
            tetra_call_event ev;
            DSD_MEMSET(&ev, 0, sizeof ev);
            ev.kind = TETRA_CALL_EV_RESTORE;
            ev.call_id = 0x0A;
            ev.new_call_id = 0x0E;
            ev.has = TETRA_CALL_HAS_NEW_ID;
            tetra_calls_on_event(s.opts, s.state, &ev, NULL);
        } else {
            send_info_rename_on(&s, 0x100u, 0x0A, 0x0E, pass == 2);
        }
        assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0E) != NULL);
        assert(followed_call(s.state) == 0x0E && tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0E));
        const tetra_state_t* const ts = tetra_st(s.state);
        assert(ts->sm.t_voice_m > 1000.9 && ts->sm.t_voice_m < 1001.1 && ts->sm.t_tune_m < 1000.1);
        assert(g_tunes == 1 && g_returns == 0);
        send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0E, 5);
        assert(tetra_calls_peek(s.state, 0x0E) == NULL && g_returns == 1 && followed_call(s.state) == -1);
        session_close(&s);
    }
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
    session_close(&s);
}

/* 1 while canonical slot 0 carries an active call to @p target. */
static int
publishes_call_to(const dsd_state* state, uint32_t target) {
    const dsd_call_snapshot call = canonical(state);
    return call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == target;
}

/* The receiver cannot leave a followed call whose identifier a D-SETUP gave to a call of a blocked talkgroup: the
 * return is deferred, or a return in flight fails and puts the call back. Until following ends the two calls stay
 * apart: nothing of the new call -- the D-SETUP itself, a repeat of it, its D-CONNECT, a later allocation -- counts
 * as the followed call's, so none proves or publishes it and none tunes to the blocked talkgroup; and once the grant
 * timeout gave up following, the new call is judged like any other and is still not followed. */
static void
test_reused_id_while_following_held(void) {
    for (int in_flight = 0; in_flight < 2; in_flight++) {
        session s = session_open();
        session_history(&s);
        dsd_tg_policy_entry row;
        assert(dsd_tg_policy_make_exact_entry(0x200u, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
        assert(dsd_tg_policy_upsert_exact(s.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        send_call_pdu(&s, 0x100u, TETRA_CMCE_D_CONNECT, 0x0A, 4 + 1 + 1 + 2 + 1 + 1);
        assert(followed_call(s.state) == 0x0A && g_tunes == 1 && tetra_confirm_followed_call_proven(s.state));

        g_return_result = in_flight ? DSD_TRUNK_TUNE_RESULT_PENDING : DSD_TRUNK_TUNE_RESULT_DEFERRED;
        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
        assert(g_returns == 1 && g_tunes == 1);
        if (in_flight) {
            const uint64_t request = dsd_trunk_tuning_pending_request();
            assert(request != 0U && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
            tetra_sm_tick(s.opts, s.state);
        }
        /* The receiver stays on the old call's channel and follows no call record. */
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
        assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));
        assert(tetra_calls_peek(s.state, 0x0A)->called_ssi == 0x200u && !publishes_call_to(s.state, 0x200u));

        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
        send_call_pdu(&s, 0x200u, TETRA_CMCE_D_CONNECT, 0x0A, 4 + 1 + 1 + 2 + 1 + 1);
        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 2, 160);
        assert(g_tunes == 1 && g_tuned_hz == CARRIER_HZ(120) && followed_call(s.state) == 0x0A);
        assert(!tetra_confirm_followed_call_proven(s.state) && !publishes_call_to(s.state, 0x200u));

        /* The grant timeout retries the return; one in flight that fails puts the channel back still without a call
         * record. */
        g_return_result = DSD_TRUNK_TUNE_RESULT_PENDING;
        at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S);
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && dsd_trunk_tuning_pending_request() != 0U);
        g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
        dsd_trunk_tuning_request_publish(dsd_trunk_tuning_pending_request(), DSD_TRUNK_TUNE_RESULT_FAILED);
        tetra_sm_tick(s.opts, s.state);
        assert(followed_call(s.state) == 0x0A && tetra_calls_followed(s.state) == NULL);
        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 170);
        assert(g_tunes == 1 && !tetra_confirm_followed_call_proven(s.state) && !publishes_call_to(s.state, 0x200u));
        /* Renaming the new call does not hand it the followed channel. */
        send_info_rename(&s, 0x200u, 0x0A, 0x0E);
        assert(followed_call(s.state) == 0x0A && tetra_calls_peek(s.state, 0x0E) != NULL);
        assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));

        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
        assert(g_tunes == 1 && followed_call(s.state) == -1 && !publishes_call_to(s.state, 0x200u));
        session_close(&s);
    }

    /* A call of the held talkgroup that took the identifier, allocated the channel the receiver is on, is a new call
     * the state machine follows there, not the old call's grant. */
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    assert(tetra_calls_followed(s.state) == NULL && followed_call(s.state) == 0x0A);
    s.state->tg_hold = 0x200u;
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(g_tunes == 1 && tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0A));
    assert(tetra_confirm_followed_call_proven(s.state) && publishes_call_to(s.state, 0x200u));
    session_close(&s);
}

/* A D-RELEASE of the followed call lets go of its identifier as a D-SETUP that reuses it does: when the return to the
 * control channel is deferred, or in flight and then fails and puts the call back, the receiver stays on the released
 * call's channel but follows no call record, so a D-SETUP that gives the identifier to a call of a blocked talkgroup is
 * judged by the policy like any other and is neither followed nor published. */
static void
test_released_followed_id_retired(void) {
    for (int in_flight = 0; in_flight < 2; in_flight++) {
        session s = session_open();
        session_history(&s);
        dsd_tg_policy_entry row;
        assert(dsd_tg_policy_make_exact_entry(0x200u, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
        assert(dsd_tg_policy_upsert_exact(s.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        send_call_pdu(&s, 0x100u, TETRA_CMCE_D_CONNECT, 0x0A, 4 + 1 + 1 + 2 + 1 + 1);
        assert(followed_call(s.state) == 0x0A && g_tunes == 1 && tetra_confirm_followed_call_proven(s.state));

        g_return_result = in_flight ? DSD_TRUNK_TUNE_RESULT_PENDING : DSD_TRUNK_TUNE_RESULT_DEFERRED;
        send_call_pdu(&s, 0x100u, TETRA_CMCE_D_RELEASE, 0x0A, 5);
        assert(g_returns == 1 && tetra_calls_peek(s.state, 0x0A) == NULL);
        if (in_flight) {
            const uint64_t request = dsd_trunk_tuning_pending_request();
            assert(request != 0U && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
            tetra_sm_tick(s.opts, s.state);
        }
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
        assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));

        send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
        assert(g_tunes == 1 && g_tuned_hz == CARRIER_HZ(120) && followed_call(s.state) == 0x0A);
        assert(tetra_calls_peek(s.state, 0x0A)->called_ssi == 0x200u && tetra_calls_followed(s.state) == NULL);
        assert(!tetra_confirm_followed_call_proven(s.state) && !publishes_call_to(s.state, 0x200u));
        session_close(&s);
    }
}

/* A D-INFO that gives another call the followed call's identifier says the followed call's release went unheard: it
 * ends and is let go of as its release would have made it, and where the return is deferred the receiver follows the
 * channel but no call record, so the renamed call is never taken for the followed one. */
static void
test_rename_onto_followed_id(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 0, 0);
    assert(followed_call(s.state) == 0x0A && publishes_call_to(s.state, 0x100u));
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    send_info_rename(&s, 0x200u, 0x0B, 0x0A);
    assert(g_returns == 1 && tetra_calls_peek(s.state, 0x0B) == NULL);
    assert(tetra_calls_peek(s.state, 0x0A) != NULL && tetra_calls_peek(s.state, 0x0A)->called_ssi == 0x200u);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_calls_followed(s.state) == NULL);
    assert(!tetra_confirm_followed_call_proven(s.state) && !publishes_call_to(s.state, 0x100u));
    assert(!publishes_call_to(s.state, 0x200u));

    /* Its next PDU or allocation is the renamed call's own: judged by the policy, not the followed call's grant. */
    s.state->tg_hold = 0x300u;
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 2, 160);
    assert(g_tunes == 1 && !tetra_confirm_followed_call_proven(s.state) && !publishes_call_to(s.state, 0x200u));
    session_close(&s);
}

/* A record that ages out lets go of its identifier too: the followed call, whose record aged while a tune to another
 * call was in flight, comes back without a call record when that tune fails, so a D-SETUP that gives its identifier to
 * a call of a blocked talkgroup does not take the followed call's grant path. */
static void
test_aged_rollback_call_retired(void) {
    session s = session_open();
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(0x200u, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    assert(dsd_tg_policy_upsert_exact(s.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    s.state->tg_hold = 0x300u;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x300u, 0x0B, TTB_SPEECH_GROUP, 2, 130);
    const uint64_t request = dsd_trunk_tuning_pending_request();
    assert(g_tunes == 2 && request != 0U && followed_call(s.state) == 0x0B);

    at_seconds(1000.0 + TETRA_CALL_IDLE_S + 1.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0B) != NULL);
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
    assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));

    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 4, 150);
    assert(g_tunes == 2 && followed_call(s.state) == 0x0A && tetra_calls_followed(s.state) == NULL);
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

/* D-TX GRANTED (table 14.18) of call @p call_id to another user, @p talker transmitting, with an allocation of
 * @p timeslots on @p carrier (none when 0). */
static void
send_tx_granted_on(session* s, uint32_t ssi, uint16_t call_id, uint32_t talker, uint32_t timeslots, uint32_t carrier) {
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
    send_block(s, ssi, &cmce, timeslots, carrier);
}

/* D-TX GRANTED of call @p call_id to another user, @p talker transmitting. */
static void
send_tx_granted(session* s, uint32_t ssi, uint16_t call_id, uint32_t talker) {
    send_tx_granted_on(s, ssi, call_id, talker, 0, 0);
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

/* A MAC-RESOURCE whose encryption mode is not 00 addressed to a party of the followed call, with a channel allocation,
 * makes the call's traffic air encrypted (EN 300 392-7 clause 6.7.1.2), the allocation being ciphertext: its canonical
 * call reads encrypted at once, in the same epoch, not at the next PDU that would publish it again. Addressed by the
 * called group's USSI, which encrypted signalling leaves clear (clause 4.2.6), or by the event label assigned to it.
 * While encrypted calls are not followed, the followed call this leaves undecryptable (no cipher key loaded) is locked
 * out and left at once, as a grant for it would have been refused. */
static void
test_header_encryption_reaches_the_call(void) {
    for (int by_label = 0; by_label < 2; by_label++) {
        session s = session_open();
        session_history(&s);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        dsd_call_snapshot call = canonical(s.state);
        assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_CLEAR && call.audio_permitted);
        const uint64_t epoch = call.epoch;
        const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
        send_event_label(&s, 0x100u, 0x55u, 0, 0); /* on the traffic carrier */
        assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);

        ttb_block b = {0};
        const uint32_t type = by_label ? TETRA_MAC_ADDR_EVENT_LABEL : TETRA_MAC_ADDR_USSI;
        const int start = ttb_resource(&b, TETRA_ENC_MODE_ODD, type, 0x100u);
        if (by_label) {
            ttb_put(&b, 0x55u, 10); /* event label */
        }
        ttb_resource_flags(&b, 1);
        ttb_put(&b, 0x2A5u, 30); /* the channel allocation and TM-SDU, ciphertext */
        ttb_close_resource(&b, start);
        ttb_finish(&b, TTB_SCH_F_BITS, -1);
        tetra_mac_parse_schd(b.bits, b.n, 1, s.opts, s.state);
        assert(c->air_enc == TETRA_ENC_MODE_ODD && followed_call(s.state) == 0x0A);
        call = canonical(s.state);
        assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch
               && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING && !call.audio_permitted);
        session_close(&s);
    }
    session s = session_open();
    s.opts->trunk_tune_enc_calls = 0;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_returns == 0);
    ttb_block b = {0};
    const int start = ttb_resource(&b, TETRA_ENC_MODE_EVEN, TETRA_MAC_ADDR_USSI, 0x100u);
    ttb_resource_flags(&b, 1);
    ttb_put(&b, 0x2A5u, 30);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s.opts, s.state);
    assert(dsd_enc_lockout_entry_active(s.state, 0x100u, 1) && g_returns == 1 && followed_call(s.state) == -1);
    session_close(&s);
}

/* A fragmented TM-SDU encrypted from its first fragment carries its channel allocation, ciphertext too, in its MAC-END
 * (EN 300 392-2 clause 23.4.2.1.1, EN 300 392-7 clause 6.7.1.2): the call of the party its MAC-RESOURCE addressed is air
 * encrypted from there, as for an encrypted MAC-RESOURCE that carries the whole TM-SDU, and its canonical call says so
 * at once. */
static void
test_fragmented_header_encryption_reaches_the_call(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 1;
    ttb_block b = {0};
    const int start = ttb_resource(&b, TETRA_ENC_MODE_ODD, TETRA_MAC_ADDR_USSI, 0x100u);
    ttb_set(&b, start + 7, 63, 6); /* length indication 111111: a first fragment */
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 0x5A5Au, 16); /* ciphertext */
    ttb_finish(&b, TTB_SCH_HD_BITS, start + 2);
    tetra_mac_parse_schd(b.bits, b.n, 1, s.opts, s.state);
    assert(ts->mac_frag[1].active && tetra_calls_peek(s.state, 0x0A)->air_enc == TETRA_ENC_MODE_NONE);
    b = (ttb_block){0};
    const int end = ttb_end(&b);
    ttb_put(&b, 1, 1);       /* channel allocation flag */
    ttb_put(&b, 0x2A5u, 30); /* the channel allocation and the last fragment, ciphertext */
    ttb_close_end(&b, end);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s.opts, s.state);
    assert(!ts->mac_frag[1].active && tetra_calls_peek(s.state, 0x0A)->air_enc == TETRA_ENC_MODE_ODD);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    session_close(&s);
}

/* A grant is the SwMI's floor decision, never sent unsolicited (EN 300 392-2 clause 14.5.1.2.1): one granted to
 * another user that names no transmitting party, where the last grant named one, is the next transmission with its
 * D-TX CEASED unheard. The canonical call of the talker no longer known ends, and the next transmission's goes on
 * under no talker rather than the last one's. */
static void
test_grant_without_talker_is_the_next_transmission(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 0x4444u);
    const uint64_t first = call.epoch;
    tetra_call_event granted;
    DSD_MEMSET(&granted, 0, sizeof granted);
    granted.kind = TETRA_CALL_EV_TX_GRANTED;
    granted.call_id = 0x0A;
    granted.has = TETRA_CALL_HAS_GRANT;
    granted.grant = 3U; /* granted to another user, no transmitting party named */
    tetra_confirm_note_signalling(s.state);
    tetra_calls_on_event(s.opts, s.state, &granted, NULL);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != first && call.ota_source_id == 0U);
    session_close(&s);
}

/* D-SETUP (table 14.15) of call @p call_id with @p basic_service from @p caller to @p called, the MAC address, with
 * transmission grant @p grant (table 14.80) and an allocation of @p timeslots on @p carrier. The calling party is of
 * type identifier 1, its SSI, when @p extension is 0, else of type 2, its SSI and @p extension, the MCC and MNC of its
 * network (tables 14.43, 14.44). */
static void
send_setup_from(session* s, uint32_t called, uint16_t call_id, uint32_t basic_service, uint32_t caller,
                uint32_t extension, uint32_t grant, uint32_t timeslots, uint32_t carrier) {
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_SETUP, 5);
    ttb_put(&cmce, call_id, 14);
    ttb_put(&cmce, 4, 4);             /* call time-out */
    ttb_put(&cmce, 0, 2);             /* hook method and simplex/duplex selection */
    ttb_put(&cmce, basic_service, 8); /* basic service information (table 14.35) */
    ttb_put(&cmce, grant, 2);         /* transmission grant */
    ttb_put(&cmce, 0, 1);             /* transmission request permission */
    ttb_put(&cmce, 0, 4);             /* call priority */
    ttb_put(&cmce, 1, 1);             /* O-bit */
    ttb_put(&cmce, 0, 2);             /* no notification indicator, no temporary address */
    ttb_put(&cmce, 1, 1);             /* calling party type identifier present */
    ttb_put(&cmce, extension != 0u ? 2u : 1u, 2);
    ttb_put(&cmce, caller, 24);
    if (extension != 0u) {
        ttb_put(&cmce, extension, 24);
    }
    ttb_put(&cmce, 0, 1); /* M-bit */
    send_block(s, called, &cmce, timeslots, carrier);
}

/* D-SETUP of individual call @p call_id from @p caller (type identifier 1, its SSI) to @p callee, with transmission
 * grant @p grant and an allocation of @p timeslots on @p carrier. */
static void
send_private_setup(session* s, uint32_t callee, uint16_t call_id, uint32_t caller, uint32_t grant, uint32_t timeslots,
                   uint32_t carrier) {
    send_setup_from(s, callee, call_id, TTB_SPEECH_P2P, caller, 0u, grant, timeslots, carrier);
}

/* The talkgroup policy blocks @p id (mode B). */
static void
block_id(session* s, uint32_t id) {
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(id, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    assert(dsd_tg_policy_upsert_exact(s->state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
}

/* A D-SETUP that reuses a known call identifier with another calling party is another call whose predecessor's release
 * went unheard, for individual and group calls alike: the old call ends and is let go of, and the new one is judged by
 * the talkgroup policy and named by its own parties -- a blocked caller's is not followed and inherits neither the
 * follow nor the old talker; an allowed caller's is followed as a call of its own once the return the old call's end
 * made has left the block that carried it (its late-entry D-SETUP). A calling party is its SSI in its network: the
 * extension of a party of type 2, the current network for one of type 1 (EN 300 392-2 clause 14.2.3, tables 14.43,
 * 14.44), so the same SSI as a TSI of the current network is the same caller, of another network another. The same
 * caller named again (a late-entry D-SETUP) or none named is the same call. */
static void
test_setup_reusing_id_with_another_caller(void) {
    /* Following individual call 14 from ISSI 0x111 to ISSI 0x222; its release unheard, a D-SETUP gives 14 to blocked
     * ISSI 0x333 calling 0x222, with the same service and allocation and a transmission grant. */
    session s = session_open();
    session_history(&s);
    block_id(&s, 0x333u);
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    assert(followed_call(s.state) == 0x0E && g_tunes == 1);
    const uint64_t first = canonical(s.state).epoch;
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    send_setup(&s, 0x222u, 0x0E, TTB_SPEECH_P2P, 4, 120);
    assert(followed_call(s.state) == 0x0E && g_returns == 0 && canonical(s.state).epoch == first);
    send_private_setup(&s, 0x222u, 0x0E, 0x333u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 1 && followed_call(s.state) == -1);
    const tetra_call_t* c = tetra_calls_peek(s.state, 0x0E);
    assert(c && c->calling_ssi == 0x333u && c->talker_ssi == 0x333u && c->called_ssi == 0x222u);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.epoch == first && call.phase == DSD_CALL_PHASE_ENDED && call.ota_source_id == 0x111u);
    session_close(&s);

    /* An allowed caller's call under the identifier is followed as a call of its own. */
    s = session_open();
    session_history(&s);
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    uint64_t epoch = canonical(s.state).epoch;
    send_private_setup(&s, 0x222u, 0x0E, 0x333u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 1 && followed_call(s.state) == -1);
    send_private_setup(&s, 0x222u, 0x0E, 0x333u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 2 && followed_call(s.state) == 0x0E);
    call = canonical(s.state);
    assert(call.epoch != epoch && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 0x333u
           && call.ota_target_id == 0x222u);
    session_close(&s);

    /* A group call: the same group, another calling party. */
    s = session_open();
    session_history(&s);
    send_setup_from(&s, 0x500u, 0x0B, TTB_SPEECH_GROUP, 0x111u, 0u, 0, 4, 120);
    assert(followed_call(s.state) == 0x0B && g_tunes == 1 && canonical(s.state).ota_source_id == 0x111u);
    epoch = canonical(s.state).epoch;
    send_setup_from(&s, 0x500u, 0x0B, TTB_SPEECH_GROUP, 0x333u, 0u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 1 && followed_call(s.state) == -1);
    send_setup_from(&s, 0x500u, 0x0B, TTB_SPEECH_GROUP, 0x333u, 0u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 2 && followed_call(s.state) == 0x0B);
    call = canonical(s.state);
    assert(call.epoch != epoch && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 0x333u
           && call.ota_target_id == 0x500u);
    session_close(&s);

    /* The calling party's network, on network 262/1001: SSI 0x111 as a TSI of 262/1001 is the caller of type 1; as
     * one of 262/1002 it is another. */
    s = session_open();
    session_history(&s);
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    epoch = canonical(s.state).epoch;
    send_setup_from(&s, 0x222u, 0x0E, TTB_SPEECH_P2P, 0x111u, (262u << 14) | 1001u, 0, 4, 120);
    assert(followed_call(s.state) == 0x0E && g_returns == 0 && canonical(s.state).epoch == epoch);
    send_setup_from(&s, 0x222u, 0x0E, TTB_SPEECH_P2P, 0x111u, (262u << 14) | 1002u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 1 && followed_call(s.state) == -1);
    send_setup_from(&s, 0x222u, 0x0E, TTB_SPEECH_P2P, 0x111u, (262u << 14) | 1002u, 0, 4, 120);
    assert(g_returns == 1 && g_tunes == 2 && followed_call(s.state) == 0x0E && canonical(s.state).epoch != epoch);
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    assert(g_returns == 2 && followed_call(s.state) == -1);
    session_close(&s);
}

/* An individual call names both its parties whoever transmits, as DMR and P25 name a unit-to-unit transmission: the
 * talker as source and the other party as target, so the talkgroup policy's private-call audio and record gates judge
 * the caller and the callee. The followed call from ISSI 0x111 to ISSI 0x222 publishes 0x111 -> 0x222 while the
 * caller talks, and 0x222 -> 0x111 once D-TX GRANTED names the callee; a talker not known leaves the caller the
 * source. */
static void
test_private_call_names_both_parties(void) {
    session s = session_open();
    /* Transmission not granted yet (table 14.80): no talker, the caller the source. */
    send_private_setup(&s, 0x222u, 0x0D, 0x111u, 1, 4, 120);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(call.ota_source_id == 0x111u && call.ota_target_id == 0x222u);
    session_close(&s);

    s = session_open();
    send_private_setup(&s, 0x222u, 0x0E, 0x111u, 0, 4, 120);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(call.ota_source_id == 0x111u && call.ota_target_id == 0x222u && call.policy_target_id == 0x222u);
    send_tx_granted(&s, 0x222u, 0x0E, 0x222u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_PRIVATE_VOICE);
    assert(call.ota_source_id == 0x222u && call.ota_target_id == 0x111u && call.policy_target_id == 0x111u);
    send_tx_granted(&s, 0x111u, 0x0E, 0x111u);
    call = canonical(s.state);
    assert(call.ota_source_id == 0x111u && call.ota_target_id == 0x222u);
    send_call_pdu(&s, 0x222u, TETRA_CMCE_D_TX_CEASED, 0x0E, 1);
    send_tx_granted(&s, 0x222u, 0x0E, 0x222u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 0x222u && call.ota_target_id == 0x111u);
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

/* dmo_dsb() with its SCH/H carrying message type @p msg_type in place of DM-OCCUPIED (bits 90..94 of the vector, after
 * the addresses and the MNI); the message dependent elements read the same for DM-SETUP, DM-SETUP PRES and DM-CONNECT
 * ACK (EN 300 396-3 tables 31, 32, 35). */
static void
dmo_dsb_msg(session* s, uint8_t enc_state, uint32_t msg_type) {
    uint8_t bits[128];
    DSD_MEMSET(bits, 0, sizeof bits);
    bits_of(k_dmo_sch_s, bits);
    tetra_dmo_sync_s_t sync_s;
    (void)tetra_dmo_parse_sync_s(bits, &sync_s);
    sync_s.enc_state = enc_state;
    DSD_MEMSET(bits, 0, sizeof bits);
    bits_of(k_dmo_sch_h, bits);
    for (int i = 0; i < 5; i++) {
        bits[90 + i] = (uint8_t)((msg_type >> (4 - i)) & 1u);
    }
    tetra_dmo_sync_h_t sync_h;
    (void)tetra_dmo_parse_sync_h(bits, 0, &sync_h);
    assert(sync_h.has_addrs && sync_h.msg_type == msg_type && sync_h.src == 100u);
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

/* A direct mode transmission keeps its source and security class (EN 300 396-3 clauses 8.2.4, 8.5.3), so one of
 * another class is another call even when neither call names the other's parties: a clear call after an encrypted DM-2-C
 * one whose end went unheard, and a DM-2-C call after a clear one, each open a canonical call of their own instead of
 * taking the last one's epoch, parties or crypto class. A call announced before its transmission began is the call of
 * that transmission. */
static void
test_direct_mode_transmissions_apart(void) {
    session s = session_open();
    session_history(&s);
    dmo_dsb(&s, TETRA_DMO_AIE_DM2C);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    const uint64_t encrypted = call.epoch;
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != encrypted && call.ota_source_id == 100u
           && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    const uint64_t clear = call.epoch;
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    assert(canonical(s.state).epoch == clear);
    dmo_dsb(&s, TETRA_DMO_AIE_DM2C);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != clear && call.ota_source_id == 0u
           && call.ota_target_id == 0u && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    session_close(&s);

    /* The security class itself tells transmissions apart, not only whether it encrypts the air interface: a DM-2-C
     * DMAC-SYNC after a DM-2-B transmission of source 100, with no end of that transmission heard, is another
     * transmission whose source is unknown, not source 100's; and a DM-SETUP of source 100 under DM-2-A after its
     * DM-2-B one sets up another transmission too. */
    s = session_open();
    session_history(&s);
    dmo_dsb(&s, TETRA_DMO_AIE_DM2B);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 100u
           && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    const uint64_t dm2b = call.epoch;
    dmo_dsb(&s, TETRA_DMO_AIE_DM2B);
    assert(canonical(s.state).epoch == dm2b);
    dmo_dsb(&s, TETRA_DMO_AIE_DM2C);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != dm2b && call.ota_source_id == 0u
           && call.ota_target_id == 0u && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    const uint64_t dm2c = call.epoch;
    dmo_dsb(&s, TETRA_DMO_AIE_DM2C);
    assert(canonical(s.state).epoch == dm2c);
    session_close(&s);

    s = session_open();
    session_history(&s);
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM2B, TETRA_DMO_MSG_SETUP);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 100u && call.ota_target_id == 0u);
    const uint64_t setup_2b = call.epoch;
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM2B, TETRA_DMO_MSG_SETUP);
    assert(canonical(s.state).epoch == setup_2b);
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM2A, TETRA_DMO_MSG_SETUP);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != setup_2b && call.ota_source_id == 100u
           && call.ota_target_id == 0x800001u);
    session_close(&s);

    /* Set-up with presence check (EN 300 396-3 clause 6.2): DM-SETUP PRES announces the call, the called party's
     * DM-CONNECT answers, and the calling party's DM-CONNECT ACK starts the traffic, which DM-OCCUPIED goes on with:
     * one call, one canonical call. */
    s = session_open();
    session_history(&s);
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM1, TETRA_DMO_MSG_SETUP_PRES);
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM1, TETRA_DMO_MSG_SETUP_PRES);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 100u && call.ota_target_id == 0x800001u);
    const uint64_t announced = call.epoch;
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM1, TETRA_DMO_MSG_CONNECT_ACK);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == announced && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    dmo_dsb_msg(&s, TETRA_DMO_AIE_DM1, TETRA_DMO_MSG_OCCUPIED);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == announced);
    session_close(&s);
}

/* A direct mode call nothing of which was heard for TETRA_CALL_DMO_IDLE_S is over, and so is its transmission: its
 * colour code and protection go with it (EN 300 396-3 clauses 8.2.4, 8.5.3), so speech of a next transmission whose
 * DMAC-SYNC went unread is not taken for clear by the last call's protection. */
static void
test_direct_mode_idle_ends_the_transmission(void) {
    session s = session_open();
    dmo_dsb(&s, TETRA_DMO_AIE_DM1);
    const tetra_state_t* const ts = tetra_st(s.state);
    assert(ts->calls.dmo.active && ts->dmo_dcc_valid && tetra_dmo_call_crypt(s.state) == TETRA_DMO_CRYPT_CLEAR);
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S - 0.5);
    tetra_calls_tick(s.opts, s.state);
    assert(ts->calls.dmo.active && ts->dmo_dcc_valid && tetra_dmo_call_crypt(s.state) == TETRA_DMO_CRYPT_CLEAR);
    at_seconds(1000.0 + TETRA_CALL_DMO_IDLE_S + 0.5);
    tetra_calls_tick(s.opts, s.state);
    assert(!ts->calls.dmo.active && !ts->dmo_dcc_valid && tetra_dmo_call_crypt(s.state) == TETRA_DMO_CRYPT_UNKNOWN);
    session_close(&s);
}

/* A SYNC PDU (EN 300 392-2 table 21.76) of network 262/1001 with colour code @p colour. */
static void
bsch_of(ttb_block* b, uint32_t colour) {
    ttb_sync_pdu(b, 262, 1001, colour);
}

/* A BSCH of another cell (another MCC, MNC or colour code) leaves nothing of the last cell's SYSINFO: its main carrier,
 * band, offset and location area go, and the control channel that SYSINFO gave, so a D-SETUP whose allocation has no
 * extended carrier numbering, decoded before the new cell's SYSINFO, resolves to no frequency and tunes nowhere, and
 * the state machine rests IDLE. A control channel that came from elsewhere stays, and the same cell's BSCH keeps it
 * all. */
static void
test_cell_change_forgets_sysinfo(void) {
    for (int from_sysinfo = 1; from_sysinfo >= 0; from_sysinfo--) {
        session s = session_open();
        session_history(&s);
        tetra_state_t* const ts = tetra_st(s.state);
        ts->dl_carrier_hz = from_sysinfo ? CARRIER_HZ(100) : CARRIER_HZ(101);
        ts->sysinfo_main_carrier = from_sysinfo ? 100u : 101u;
        ts->frames_sysinfo = 1;
        ts->la = 77;
        ts->sysinfo_known = 1;
        ttb_block same = {0};
        bsch_of(&same, 25u);
        assert(tetra_bsch_parse(same.bits, same.n, s.opts, s.state) == 1);
        assert(ts->freq_band == 4 && ts->freq_offset == 2 && ts->la == 77 && ts->sysinfo_known);
        assert(s.state->trunk_cc_freq == CARRIER_HZ(100) && s.state->tetra_pub.cc_known);

        ttb_block other = {0};
        bsch_of(&other, 30u);
        assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
        assert(ts->freq_band == 0 && ts->freq_offset == 0 && ts->dl_carrier_hz == 0 && ts->sysinfo_main_carrier == 0);
        assert(ts->la == 0 && !ts->sysinfo_known && !s.state->tetra_pub.cc_known);
        tetra_sm_tick(s.opts, s.state);
        if (!from_sysinfo) {
            assert(s.state->trunk_cc_freq == CARRIER_HZ(100) && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
            session_close(&s);
            continue;
        }
        assert(s.state->trunk_cc_freq == 0 && tetra_sm_get_state(s.state) == TETRA_SM_IDLE);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        assert(g_tunes == 0 && followed_call(s.state) == -1);
        assert(tetra_calls_peek(s.state, 0x0A)->alloc.carrier == 120
               && tetra_calls_peek(s.state, 0x0A)->alloc.freq_hz == 0);
        session_close(&s);
    }
}

/* The SYSINFO of a cell of the network a BSCH just named: band 4, offset -6,25 kHz, main carrier 100 its control
 * channel. */
static void
sysinfo_again(session* s) {
    tetra_state_t* const ts = tetra_st(s->state);
    ts->freq_band = 4;
    ts->freq_offset = 2;
    ts->sysinfo_known = 1;
    s->state->trunk_cc_freq = CARRIER_HZ(100);
    tetra_sm_on_cc_sync(s->opts, s->state);
}

/* Call identifiers and party SSIs are numbered by each network: a BSCH naming another network (another MCC or MNC)
 * ends every call record of the last one, so a D-TX GRANTED of the new network that reuses an old call identifier, its
 * allocation with it, names no record and tunes nowhere, and the D-MM STATUS the last network sent is not shown any
 * more. A retune within the network (a carrier boundary) and a BSCH of another cell of it keep the records and the
 * status. */
static void
test_network_change_drops_calls(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    send_setup(&s, 0x101u, 0x0B, TTB_SPEECH_GROUP, 0, 0);
    assert(g_tunes == 0 && tetra_calls_peek(s.state, 0x0A) != NULL);

    tetra_forget_carrier(s.state);
    tetra_st(s.state)->mm_status_valid = 1; /* a D-MM STATUS of the network */
    tetra_st(s.state)->mm_status_code = 5;
    ttb_block cell = {0};
    ttb_sync_pdu(&cell, 262, 1001, 30);
    assert(tetra_bsch_parse(cell.bits, cell.n, s.opts, s.state) == 1);
    assert(tetra_calls_peek(s.state, 0x0A) != NULL && tetra_calls_peek(s.state, 0x0B) != NULL);
    assert(s.state->tetra_pub.mm_status_valid && s.state->tetra_pub.mm_status_code == 5);

    ttb_block other = {0};
    ttb_sync_pdu(&other, 262, 1002, 30);
    assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_calls_peek(s.state, 0x0B) == NULL);
    assert(!s.state->tetra_pub.mm_status_valid && s.state->tetra_pub.mm_status_code == 0);
    sysinfo_again(&s);
    send_tx_granted_on(&s, 0x100u, 0x0A, 0x4444u, 4, 120);
    assert(g_tunes == 0 && followed_call(s.state) == -1 && tetra_calls_peek(s.state, 0x0A) == NULL);
    session_close(&s);
}

/* The call the state machine follows when a BSCH names another network ends with the network's other calls: its
 * canonical call ends, and the receiver follows its channel but no call record, so a D-SETUP of the new network that
 * reuses the identifier for a blocked talkgroup is judged by the policy and not followed; the grant timeout then
 * returns. */
static void
test_network_change_retires_the_followed_call(void) {
    session s = session_open();
    session_history(&s);
    dsd_tg_policy_entry row;
    assert(dsd_tg_policy_make_exact_entry(0x200u, "B", "blocked", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0);
    assert(dsd_tg_policy_upsert_exact(s.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_call_pdu(&s, 0x100u, TETRA_CMCE_D_CONNECT, 0x0A, 4 + 1 + 1 + 2 + 1 + 1);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1 && publishes_call_to(s.state, 0x100u));

    ttb_block other = {0};
    ttb_sync_pdu(&other, 262, 1002, 25);
    assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
    assert(!publishes_call_to(s.state, 0x100u) && canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_calls_followed(s.state) == NULL);
    assert(!tetra_confirm_followed_call_proven(s.state) && g_returns == 0);
    send_setup(&s, 0x200u, 0x0A, TTB_SPEECH_GROUP, 2, 150);
    assert(g_tunes == 1 && !publishes_call_to(s.state, 0x200u) && !tetra_confirm_followed_call_proven(s.state));

    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S);
    tetra_sm_tick(s.opts, s.state);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    session_close(&s);
}

/* The SwMI's call identifiers may differ from one location area to the next (EN 300 392-2 clause 14.2.3): the records
 * belong to the area the SYSINFO named, the same area's SYSINFO keeps them (a carrier or a cell of it), and another
 * area's ends them, so its D-TX GRANTED reusing an identifier names no record and tunes nowhere. */
static void
test_location_area_change_drops_calls(void) {
    session s = session_open();
    session_history(&s);
    ttb_block si = {0};
    ttb_sysinfo(&si, 100, 4, 2, 77);
    tetra_mac_parse_schd(si.bits, si.n, 1, s.opts, s.state);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    assert(tetra_calls_peek(s.state, 0x0A) != NULL);

    tetra_forget_carrier(s.state);
    ttb_block cell = {0};
    ttb_sync_pdu(&cell, 262, 1001, 30);
    assert(tetra_bsch_parse(cell.bits, cell.n, s.opts, s.state) == 1);
    tetra_mac_parse_schd(si.bits, si.n, 30, s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) != NULL && tetra_st(s.state)->la == 77);

    ttb_block other = {0};
    ttb_sysinfo(&other, 100, 4, 2, 78);
    tetra_mac_parse_schd(other.bits, other.n, 30, s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && tetra_st(s.state)->la == 78);
    send_tx_granted_on(&s, 0x100u, 0x0A, 0x4444u, 4, 120);
    assert(g_tunes == 0 && followed_call(s.state) == -1 && tetra_calls_peek(s.state, 0x0A) == NULL);
    session_close(&s);
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

/* @p nbits bits of @p value into @p bits, MSB first. */
static void
bits_of_value(uint8_t* bits, uint64_t value, int nbits) {
    for (int i = 0; i < nbits; i++) {
        bits[i] = (uint8_t)((value >> (nbits - 1 - i)) & 1u);
    }
}

/* A DMAC-DATA in an SCH/F (EN 300 396-3 table 23), clear (DM-1), carrying a DM-SDS UDATA (table 45) from @p src to
 * @p dst without additional addressing: short data type @p sdti and its user data, @p data_bits of @p data (for SDTI 3
 * after a length indicator of @p length_bits, which may claim more than follows), then with @p fcs an FCS over the
 * DM-SDU elements before it (clause 9.7.10, annex B). Bit @p flip of the DM-SDU, counted from its first element, is
 * flipped once the FCS is in (-1: none), as a bit error the channel CRC let through. Decoded as a DNB's SCH/F reaches
 * the decoder (tetra_dmo_log_dnb_pdu()). */
static void
send_dmo_sds(session* s, uint32_t src, uint32_t dst, uint32_t sdti, const uint8_t* data, int data_bits, int length_bits,
             int fcs, int flip) {
    ttb_block b = {0};
    ttb_put(&b, 0, 2);        /* MAC PDU type 00: DMAC-DATA */
    ttb_put(&b, 0, 4);        /* fill bit, second half slot stolen, fragmentation and null PDU flags */
    ttb_put(&b, 0, 2);        /* frame countdown */
    ttb_put(&b, 0, 2);        /* air interface encryption state DM-1 */
    ttb_put(&b, 0, 2);        /* destination address type: true SSI */
    ttb_put(&b, dst, 24);     /* destination address */
    ttb_put(&b, 0, 2);        /* source address type: true SSI */
    ttb_put(&b, src, 24);     /* source address */
    ttb_put(&b, 0x41901, 24); /* mobile network identity */
    ttb_put(&b, TETRA_DMO_MSG_SDS_UDATA, 5);
    ttb_put(&b, 0, 4); /* SDS time remaining */
    ttb_put(&b, 0, 1); /* SDS transaction type: stand-alone */
    ttb_put(&b, 0, 2); /* priority level */
    ttb_put(&b, fcs ? 1u : 0u, 1);
    const int sdu = b.n;
    ttb_put(&b, 0, 1); /* additional addressing flag */
    ttb_put(&b, sdti, 4);
    if (sdti == 3u) {
        ttb_put(&b, (uint32_t)length_bits, 11);
    }
    ttb_put_bits(&b, data, data_bits);
    if (fcs) {
        ttb_put(&b, tetra_llc_fcs32(b.bits + sdu, b.n - sdu), 32);
    }
    if (flip >= 0) {
        assert(sdu + flip < b.n);
        b.bits[sdu + flip] ^= 1u;
    }
    (void)tetra_dmo_log_dnb_pdu(b.bits, TTB_SCH_F_BITS, "SCH/F", s->opts, s->state);
}

/* Direct mode short data whose FCS fails is discarded (EN 300 396-3 clauses 6.3.2 and 6.3.4): no notice, and nothing of
 * it kept as the last short data noted, so a good copy of the same message is still noted. The FCS is checked over the
 * DM-SDU elements, whether the bit error is in the FCS or in the data it protects, and a DM-SDU the block ends in before
 * its FCS has none to pass. Short data without an FCS is noted as before. */
static void
test_direct_mode_short_data_fcs(void) {
    enum { STATUS_FCS_AT = 1 + 4 + 16 };

    session s = session_open();
    session_history(&s);
    const Event_History_I* const history = &s.state->event_history_s[0];
    uint8_t data[128];
    bits_of_value(data, 0x1234u, 16);
    uint64_t pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 4u, data, 16, 0, 1, STATUS_FCS_AT + 15);
    assert(history->push_seq == pushes);
    send_dmo_sds(&s, 200u, 300u, 4u, data, 16, 0, 1, 1 + 4 + 3);
    assert(history->push_seq == pushes);
    send_dmo_sds(&s, 200u, 300u, 4u, data, 16, 0, 1, -1);
    assert(history->push_seq > pushes);
    assert(strstr(history->Event_History_Items[1].event_string, "DMO SDS SRC: 200; TGT: 300;") != NULL);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 4u, data, 16, 0, 1, -1);
    assert(history->push_seq == pushes);

    /* No FCS: noted. A long message the block ends in before its FCS: discarded. */
    bits_of_value(data, 0x4321u, 16);
    send_dmo_sds(&s, 201u, 300u, 4u, data, 16, 0, 0, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    DSD_MEMSET(data, 1, sizeof data);
    send_dmo_sds(&s, 202u, 300u, 3u, data, 100, 160, 1, -1);
    assert(history->push_seq == pushes);
    session_close(&s);
}

/* What counts as a repeat of direct mode short data is the whole message: two that differ in their short data type, in
 * the length of their user data, or anywhere in it, are two messages, each noted, though the first 64 bits of their
 * data (sds_value) match; only the same message again within 10 s is not noted again. */
static void
test_direct_mode_short_data_repeats(void) {
    session s = session_open();
    session_history(&s);
    const Event_History_I* const history = &s.state->event_history_s[0];
    uint8_t data[128];
    bits_of_value(data, 0x1234u, 16);
    uint64_t pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 4u, data, 16, 0, 0, -1); /* pre-coded status 0x1234 */
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 0u, data, 16, 0, 0, -1); /* user defined data 1, 0x1234 */
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 0u, data, 16, 0, 0, -1);
    assert(history->push_seq == pushes);

    /* User defined data 4 (SDTI 3): the same first 64 bits, then other bits, or fewer. */
    DSD_MEMSET(data, 0, sizeof data);
    bits_of_value(data, 0xA5A5A5A5u, 32);
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 0, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 0, -1);
    assert(history->push_seq == pushes);
    data[99] = 1u;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 0, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 99, 99, 0, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 99, 99, 0, -1);
    assert(history->push_seq == pushes);

    /* With an FCS (annex B) each: two messages that differ only in their last bits, past the first 64, are two as well,
       and a repeat of the second is still a repeat. A CRC over the data and its own valid FCS is the same residue for
       every message, so the FCS adds nothing that tells them apart. */
    DSD_MEMSET(data, 0, sizeof data);
    bits_of_value(data, 0x5A5A5A5Au, 32);
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 1, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    data[98] = 1u;
    data[99] = 1u;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 1, -1);
    assert(history->push_seq > pushes);
    pushes = history->push_seq;
    send_dmo_sds(&s, 200u, 300u, 3u, data, 100, 100, 1, -1);
    assert(history->push_seq == pushes);
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

/* A BSCH of another cell of the same network says the receiver is on another carrier, whatever the engine saw (a radio
 * retuned outside the decoder): not trunking, the anonymous speech canonical slot 0 shows ends, and what the last
 * carrier's speech left goes as at a carrier boundary -- its timeslots' signalling encryption, the heard timeslot, the
 * speech in progress, the followed call's proof -- with the encryption mode the last cell's signalling showed. A BSCH
 * of the same cell keeps it all. */
static void
test_cell_change_forgets_the_carrier(void) {
    session s = session_open();
    session_history(&s);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->calls.slot_enc[2] = 1u + TETRA_ENC_MODE_EVEN;
    ts->enc_mode = TETRA_ENC_MODE_EVEN;
    ts->frames_resource = 1;
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_VOICE && ts->calls.heard_tn == 2);
    assert(ts->voice.bound && ts->calls.heard);
    ttb_block same = {0};
    bsch_of(&same, 25u);
    assert(tetra_bsch_parse(same.bits, same.n, s.opts, s.state) == 1);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE && ts->calls.heard && ts->voice.bound);
    assert(ts->calls.slot_enc[2] != 0u && s.state->tetra_pub.air_encryption == DSD_TETRA_AIR_ENC_ENCRYPTED);

    ttb_block other = {0};
    bsch_of(&other, 30u);
    assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_EXPLICIT);
    assert(!ts->calls.heard && !ts->calls.held && ts->calls.slot_enc[0] == 0u && ts->calls.slot_enc[2] == 0u);
    assert(!ts->voice.bound && ts->voice.carry_len == 0U && !ts->voice.confirmed);
    assert(s.state->tetra_pub.air_encryption == DSD_TETRA_AIR_ENC_UNKNOWN);
    session_close(&s);

    /* Trunking, the followed call's proof on the carrier was the last cell's carrier's; its canonical call and its
     * following are the state machine's. */
    s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    tetra_confirm_note_followed_call(s.state);
    assert(followed_call(s.state) == 0x0A && tetra_confirm_followed_call_proven(s.state));
    assert(tetra_bsch_parse(same.bits, same.n, s.opts, s.state) == 1);
    assert(tetra_confirm_followed_call_proven(s.state));
    assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
    assert(!tetra_confirm_followed_call_proven(s.state) && followed_call(s.state) == 0x0A);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE);
    session_close(&s);
}

/* A channel allocation names a traffic channel of the cell that made it (EN 300 392-2 clause 21.5.2: the carrier
 * number and timeslots of that cell's carriers), and an event label is valid on one MAC channel of one cell (clause
 * 23.4.1.2.3): a BSCH of another cell of the same network keeps the call records, which are the network's, but not
 * their channels or event labels. Not trunking, speech on the timeslot of the tuned frequency the last cell had
 * allocated to a call is not that call's but anonymous speech, and an allocation the new cell addresses by the event
 * label the last cell assigned attaches to no record; the call's own allocation in the new cell gives it a channel
 * again. A BSCH of the same cell keeps the channels and labels. */
static void
test_cell_change_forgets_allocations(void) {
    session s = session_open();
    session_history(&s);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of the carrier being received */
    send_event_label(&s, 0x100u, 0x55u, 0, 0);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    assert(c && c->alloc.carrier == 120 && c->alloc.timeslots == 4 && c->alloc.freq_hz == CARRIER_HZ(120));
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);
    ttb_block same = {0};
    bsch_of(&same, 25u);
    assert(tetra_bsch_parse(same.bits, same.n, s.opts, s.state) == 1);
    assert(c->alloc.timeslots == 4 && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u);

    ttb_block other = {0};
    bsch_of(&other, 30u);
    assert(tetra_bsch_parse(other.bits, other.n, s.opts, s.state) == 1);
    assert(tetra_calls_peek(s.state, 0x0A) == c && c->called_ssi == 0x100u);
    assert(c->alloc.timeslots == 0u && c->alloc.carrier == 0u && c->alloc.freq_hz == 0L && c->alloc_m <= 0.0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) == 0u);
    assert(tetra_calls_on_timeslot(s.opts, s.state, 2) == NULL);
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_VOICE && call.ota_target_id == 0u);
    sysinfo_again(&s);
    send_by_event_label(&s, 0x55u, 4, 120);
    assert(c->alloc.timeslots == 0u);

    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 2, 130);
    assert(c->alloc.carrier == 130 && c->alloc.timeslots == 2 && c->alloc.freq_hz == CARRIER_HZ(130));
    session_close(&s);
}

/* An event label stands for one party on its channel (EN 300 392-2 clause 23.4.1.2.3): an SSI + event label address
 * (type 101) names its party by the SSI and assigns it the label, so a label assigned to a party no record holds is no
 * record's party's any more, and the PDU that assigns it, its channel allocation with it, is not addressed to the
 * record that held it. Label 0 withdraws the party's label, and the all-ones label, which says the current labels stay
 * valid after a channel change, assigns none. */
static void
test_event_label_assignment(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 0, 0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);

    send_event_label(&s, 0x999u, 0x55u, 2, 130);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) == 0u && c->alloc.carrier == 120 && c->alloc.timeslots == 4);
    send_by_event_label(&s, 0x55u, 2, 130);
    assert(c->alloc.carrier == 120 && c->alloc.timeslots == 4);

    send_event_label(&s, 0x100u, 0x55u, 0, 0);
    send_event_label(&s, 0x100u, 0x3FFu, 0, 0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);
    send_by_event_label(&s, 0x55u, 2, 130);
    assert(c->alloc.carrier == 130 && c->alloc.timeslots == 2);

    send_event_label(&s, 0x100u, 0u, 0, 0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) == 0u);
    send_by_event_label(&s, 0x55u, 4, 140);
    send_by_event_label(&s, 0u, 4, 140);
    assert(c->alloc.carrier == 130 && c->alloc.timeslots == 2);
    session_close(&s);
}

static int
has_label(const tetra_call_t* c, uint16_t label) {
    return (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == label;
}

/* A label stands for one party on one MAC channel (EN 300 392-2 clause 23.4.1.2.3), so the same number on another
 * channel is another label: 0x55 assigned to call 10's party for carrier 120 and then to call 11's for carrier 130 is
 * each one's on its own carrier, where a label-addressed allocation finds it. Assigned on a channel that meets the
 * holder's (the same carrier and an overlapping timeslot; the carrier the receiver is on, for one assigned where it
 * came, is the one it is tuned to), the label goes to the new party. One whose channel the decoder cannot tell takes
 * it from every other party, since it may be any channel's: a "go to the control channel" allocation, or one assigned
 * where it came on a carrier whose number is not known. */
static void
test_event_label_reuse_on_another_channel(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const a = tetra_calls_peek(s.state, 0x0A);
    const tetra_call_t* const b = tetra_calls_peek(s.state, 0x0B);
    send_event_label(&s, 0x100u, 0x55u, 4, 120);
    send_event_label(&s, 0x200u, 0x55u, 4, 130);
    assert(has_label(a, 0x55u) && has_label(b, 0x55u));
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    send_by_event_label(&s, 0x55u, 2, 120);
    assert(a->alloc.carrier == 120 && a->alloc.timeslots == 2 && b->alloc.carrier == 130 && b->alloc.timeslots == 4);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(130);
    send_by_event_label(&s, 0x55u, 1, 130);
    assert(b->alloc.timeslots == 1 && a->alloc.carrier == 120);

    send_event_label(&s, 0x100u, 0x55u, 1, 130);
    assert(has_label(a, 0x55u) && has_label(b, 0x55u)); /* TN4 of carrier 130 is not call 11's TN2 */
    send_event_label(&s, 0x100u, 0x55u, 4, 130);
    assert(has_label(a, 0x55u) && !has_label(b, 0x55u));
    send_event_label(&s, 0x200u, 0x55u, 0, 0);
    assert(has_label(b, 0x55u) && !has_label(a, 0x55u));

    send_event_label(&s, 0x100u, 0x66u, 4, 120);
    send_event_label_to_cc(&s, 0x200u, 0x66u);
    assert(!has_label(a, 0x66u) && !has_label(b, 0x66u));
    send_event_label(&s, 0x100u, 0x77u, 4, 120);
    s.opts->audio_in_type = AUDIO_IN_PULSE;
    send_event_label(&s, 0x200u, 0x77u, 0, 0);
    assert(!has_label(a, 0x77u) && has_label(b, 0x77u));
    session_close(&s);
}

/* The all-ones label of an SSI + event label address keeps the party's current label valid after the channel change
 * its PDU makes (EN 300 392-2 clause 23.4.1.2.3.0): the label moves with the party to the allocated channel, where its
 * label-addressed PDUs are the party's, and is the party's no more on the channel left: from carrier 120 to 140, and
 * from TN2 of the followed carrier to its TN3, where a "go to the MCCH" allocation by the label releases the followed
 * call. Without an allocation no channel changes; one to the control channel names none the decoder can tell, so the
 * label goes. */
static void
test_event_label_kept_across_a_channel_change(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 4, 120);
    send_event_label(&s, 0x100u, 0x3FFu, 4, 140);
    assert(has_label(c, 0x55u) && c->alloc.carrier == 140 && c->alloc.timeslots == 4);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(140);
    send_by_event_label(&s, 0x55u, 2, 140);
    assert(c->alloc.carrier == 140 && c->alloc.timeslots == 2);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    send_by_event_label(&s, 0x55u, 1, 120);
    assert(c->alloc.carrier == 140 && c->alloc.timeslots == 2);
    send_event_label(&s, 0x100u, 0x3FFu, 0, 0);
    assert(has_label(c, 0x55u) && c->label_chan.allocated && c->label_chan.carrier == 140);
    send_event_label_to_cc(&s, 0x100u, 0x3FFu);
    assert(!has_label(c, 0x55u));
    session_close(&s);

    s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_returns == 0);
    tetra_confirm_note_signalling(s.state);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 2;
    c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 0, 0);
    send_event_label(&s, 0x100u, 0x3FFu, 2, 120);
    assert(has_label(c, 0x55u) && c->alloc.timeslots == 2 && followed_call(s.state) == 0x0A);
    ts->tn = 3;
    send_by_event_label(&s, 0x55u, 0, 0);
    assert(followed_call(s.state) == -1 && g_returns == 1);
    session_close(&s);
}

/* A channel allocation to the party by its SSI alone moves the call, not its label: without the all-ones label its
 * label is valid on the channel left and no other (EN 300 392-2 clause 23.4.1.2.3.0), so it stays bound there and no
 * label-addressed PDU of the new channel is the party's. T.201 is not modelled: the label lives as long as the record
 * holding it, which ages out TETRA_CALL_IDLE_S after the last PDU or speech that named it, taking the label with it. */
static void
test_event_label_reallocation_and_ageing(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(140);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 4, 120);
    send_block(&s, 0x100u, NULL, 4, 140);
    assert(c->alloc.carrier == 140 && has_label(c, 0x55u) && c->label_chan.carrier == 120);
    send_by_event_label(&s, 0x55u, 2, 140);
    assert(c->alloc.carrier == 140 && c->alloc.timeslots == 4);

    at_seconds(1000.0 + TETRA_CALL_IDLE_S + 1.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const again = tetra_calls_peek(s.state, 0x0A);
    assert(again && (again->flags & TETRA_CALL_F_EVENT_LABEL) == 0u);
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    send_by_event_label(&s, 0x55u, 2, 120);
    assert(again->alloc.timeslots == 0u);
    session_close(&s);
}

/* A MAC-RESOURCE whose address is @p type @p ssi with event label @p label, starting a fragmented TM-SDU (length
 * indication 111111), in the clear; then, with @p mac_end, the MAC-END that completes it, without a channel allocation. */
static void
send_label_fragments(session* s, uint32_t type, uint32_t ssi, uint32_t label, int mac_end) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, 0, type, ssi);
    ttb_put(&b, label, 10);
    ttb_set(&b, start + 7, 63, 6);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 0x5Au, 8);
    ttb_finish(&b, TTB_SCH_HD_BITS, start + 2);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
    if (!mac_end) {
        return;
    }
    b = (ttb_block){0};
    const int end = ttb_end(&b);
    ttb_put(&b, 0, 1); /* channel allocation flag */
    ttb_put(&b, 0x5Au, 8);
    ttb_close_end(&b, end);
    ttb_finish(&b, TTB_SCH_HD_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* An encrypted MAC-RESOURCE (encryption mode 01) whose address is @p type @p ssi with event label @p label: no channel
 * allocation, its TM-SDU ciphertext. */
static void
send_encrypted_label(session* s, uint32_t type, uint32_t ssi, uint32_t label) {
    ttb_block b = {0};
    const int start = ttb_resource(&b, TETRA_ENC_MODE_ODD, type, ssi);
    ttb_put(&b, label, 10);
    ttb_resource_flags(&b, 0);
    ttb_put(&b, 0x2A5u, 30);
    ttb_close_resource(&b, start);
    ttb_finish(&b, TTB_SCH_F_BITS, -1);
    tetra_mac_parse_schd(b.bits, b.n, 1, s->opts, s->state);
}

/* An event label assignment takes effect once its PDU is delivered (EN 300 392-2 clause 23.4.1.2.3.1), a fragmented
 * TM-SDU's at its MAC-END: a first fragment assigning another party the label a call holds takes it from no one, nor
 * does a TM-SDU whose MAC-END never comes (the timeslot's next block failed); its MAC-END does. An encrypted PDU assigns
 * as a clear one, its address being clear but for an SSI (EN 300 392-7 clause 4.2.6): an encrypted short identity names
 * no party, so its assignment only takes the label from the call that held it, and an SMI names its party, whose call
 * takes the label. */
static void
test_event_label_takes_effect_at_delivery(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 1;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 0, 0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);

    send_label_fragments(&s, TETRA_MAC_ADDR_SSI_EVENT, 0x999u, 0x55u, 0);
    assert(ts->mac_frag[1].active && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u);
    tetra_mac_block_failed(s.state);
    assert(!ts->mac_frag[1].active && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);
    send_label_fragments(&s, TETRA_MAC_ADDR_SSI_EVENT, 0x999u, 0x55u, 1);
    assert(!ts->mac_frag[1].active && (c->flags & TETRA_CALL_F_EVENT_LABEL) == 0u);

    send_event_label(&s, 0x100u, 0x66u, 0, 0);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x66u);
    send_encrypted_label(&s, TETRA_MAC_ADDR_SSI_EVENT, 0x999u, 0x66u);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) == 0u);
    send_encrypted_label(&s, TETRA_MAC_ADDR_SMI_EVENT, 0x100u, 0x77u);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x77u);
    send_by_event_label(&s, 0x77u, 2, 130);
    assert(c->alloc.carrier == 130 && c->alloc.timeslots == 2);
    session_close(&s);
}

/* An event label is valid on one MAC channel (EN 300 392-2 clause 23.4.1.2.3): one assigned in a PDU with a channel
 * allocation applies on the allocated channel, one assigned without on the channel the PDU came on, until the receiver
 * leaves it. A PDU addressed by the label on another channel is for whichever party holds the label there, so its
 * allocation attaches to no record: on the control channel, a label assigned for traffic carrier 120 names no one, and
 * a label of the control channel names no one once a carrier boundary took the receiver off it. Followed onto carrier
 * 120, the label assigned for it there names the followed call, whose "go to the MCCH" allocation by it ends it. */
static void
test_event_label_channel(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 1;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 4, 120);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u);
    send_by_event_label(&s, 0x55u, 2, 140);
    assert(c->alloc.carrier == 120 && c->alloc.timeslots == 4);

    send_event_label(&s, 0x100u, 0x66u, 0, 0);
    send_by_event_label(&s, 0x66u, 2, 140);
    assert(c->alloc.carrier == 140 && c->alloc.timeslots == 2);
    ts->tn = 2;
    send_by_event_label(&s, 0x66u, 4, 150);
    assert(c->alloc.carrier == 140); /* assigned on TN1, the control channel: not TN2's */
    ts->tn = 1;
    tetra_forget_carrier(s.state);
    tetra_confirm_note_signalling(s.state);
    ts->tdma_valid = 1;
    send_by_event_label(&s, 0x66u, 4, 150);
    assert(c->alloc.carrier == 140 && c->alloc.timeslots == 2);
    session_close(&s);

    s = session_open();
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 0, 0);
    send_event_label(&s, 0x200u, 0x77u, 0, 0); /* call 11's party, on the control channel */
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_event_label(&s, 0x100u, 0x55u, 4, 120); /* call 10's party, for its traffic channel */
    assert(followed_call(s.state) == 0x0A && g_returns == 0);
    tetra_forget_carrier(s.state); /* the tune to carrier 120 completed */
    tetra_confirm_note_signalling(s.state);
    send_by_event_label(&s, 0x77u, 2, 140);
    assert(tetra_calls_peek(s.state, 0x0B)->alloc.timeslots == 0u);
    send_by_event_label(&s, 0x55u, 0, 0);
    assert(followed_call(s.state) == -1 && g_returns == 1);
    session_close(&s);
}

/* The cells of a network may share a colour code, so a BSCH does not tell every change of cell: a SYSINFO naming
 * another main carrier than the cell's (EN 300 392-2 clause 21.4.4.1: the main carrier is the cell's), or another
 * location area, is another cell too, and forgets what a BSCH of another cell forgets before it is taken -- the
 * records' channels, the heard speech's call, the partial TM-SDUs and the TDMA clock -- while the call records, the
 * network's, stay in the same location area. The same cell's SYSINFO keeps it all. */
static void
test_sysinfo_tells_another_cell(void) {
    session s = session_open();
    session_history(&s);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(120);
    tetra_state_t* const ts = tetra_st(s.state);
    ttb_block si = {0};
    ttb_sysinfo(&si, 100, 4, 2, 77);
    tetra_mac_parse_schd(si.bits, si.n, 25, s.opts, s.state);
    assert(ts->dl_carrier_hz == CARRIER_HZ(100) && ts->la == 77 && ts->frames_sysinfo == 1U);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of the carrier being received */
    speech(&s, 2);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE && canonical(s.state).ota_target_id == 0x100u);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    ts->mac_frag[3].active = 1;
    ts->mac_frag[3].nbits = 40;
    ts->tdma_valid = 1;
    ts->tn = 1;
    ts->fn = 1;
    ts->mn = 1;
    tetra_mac_parse_schd(si.bits, si.n, 25, s.opts, s.state);
    assert(c->alloc.timeslots == 4 && ts->mac_frag[3].active && ts->tdma_valid);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE);

    ttb_block other = {0};
    ttb_sysinfo(&other, 101, 4, 2, 77);
    tetra_mac_parse_schd(other.bits, other.n, 25, s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == c && c->alloc.timeslots == 0u && c->alloc_m <= 0.0);
    assert(!ts->mac_frag[3].active && !ts->tdma_valid && !ts->calls.heard);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    assert(ts->dl_carrier_hz == CARRIER_HZ(101) && ts->la == 77 && ts->sysinfo_known && ts->freq_band == 4);
    tetra_publish(s.state); /* as the burst's end does */
    assert(s.state->tetra_pub.cc_known && s.state->tetra_pub.cc_carrier == 101U);

    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 130);
    ts->mac_frag[3].active = 1;
    ts->mac_frag[3].nbits = 40;
    ttb_block area = {0};
    ttb_sysinfo(&area, 101, 4, 2, 78);
    tetra_mac_parse_schd(area.bits, area.n, 25, s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && !ts->mac_frag[3].active && ts->la == 78);
    session_close(&s);
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

/* Speech no call record names is one call per timeslot: each timeslot is a traffic channel of its own (EN 300 392-2
 * clause 9.4.2.2), so when the heard timeslot moves, the anonymous call of the last one ends and the new timeslot's
 * publishes as a call of its own, with its own epoch, media and crypto class. Here TN2's clear speech turns encrypted
 * and muted, its bursts going on past its hold, and audible TN3 speech takes the receiver. */
static void
test_anonymous_call_per_timeslot(void) {
    session s = session_open();
    session_history(&s);
    s.opts->trunk_enable = 0;
    s.opts->dmr_mute_encL = 1;
    tetra_sm_tick(s.opts, s.state);
    tetra_state_t* const ts = tetra_st(s.state);
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.kind == DSD_CALL_KIND_VOICE && call.ota_target_id == 0u);
    assert(call.crypto == DSD_CALL_CRYPTO_CLEAR && ts->calls.heard_tn == 2 && ts->calls.held);
    const uint64_t tn2 = call.epoch;
    ts->calls.slot_enc[2] = 1u + TETRA_ENC_MODE_EVEN; /* no key for it: muted */
    for (int i = 1; i <= 3; i++) {
        at_seconds(1000.0 + 0.5 * i);
        speech(&s, 2);
        call = canonical(s.state);
        assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == tn2 && ts->calls.heard_tn == 2);
    }
    assert(call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    const uint64_t pushes = s.state->event_history_s[0].push_seq;
    at_seconds(1001.6);
    speech(&s, 3);
    call = canonical(s.state);
    assert(ts->calls.heard_tn == 3 && ts->calls.held);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != tn2 && call.ota_target_id == 0u
           && call.crypto == DSD_CALL_CRYPTO_CLEAR && call.media_active == 1U);
    assert(s.state->event_history_s[0].push_seq > pushes); /* TN2's call ended into the history */
    session_close(&s);
}

/* What a CRC-passed PDU or allocation changes in a record reaches the canonical call carrying it at once, not at the
 * next PDU or speech that happens to publish the call again: the air encryption a clear allocation of a late-entry
 * D-SETUP clears, the channel an allocation addressed to the call's party moves it to, and, on a traffic carrier that
 * no block proved yet, the talker a D-TX GRANTED names (the call was published before and the PDU passed its CRC). */
static void
test_record_changes_reach_the_call(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100); /* TN2 of the carrier being received */
    tetra_calls_find(s.state, 0x0C)->air_enc = TETRA_ENC_MODE_EVEN;
    speech(&s, 2);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING);
    const uint64_t epoch = call.epoch;
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100);
    call = canonical(s.state);
    assert(tetra_calls_peek(s.state, 0x0C)->air_enc == TETRA_ENC_MODE_NONE);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch && call.crypto == DSD_CALL_CRYPTO_CLEAR);
    send_block(&s, 0x300u, NULL, 4, 101);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch && call.channel == 101U
           && call.frequency_hz == CARRIER_HZ(101));
    session_close(&s);

    s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && !tetra_confirm_carrier_proven(s.state));
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_source_id == 0U);
    tetra_call_event granted;
    DSD_MEMSET(&granted, 0, sizeof granted);
    granted.kind = TETRA_CALL_EV_TX_GRANTED;
    granted.call_id = 0x0A;
    granted.has = (uint8_t)(TETRA_CALL_HAS_GRANT | TETRA_CALL_HAS_TALKER);
    granted.grant = 3U;
    granted.talker_ssi = 0x4444u;
    tetra_calls_on_event(s.opts, s.state, &granted, NULL);
    assert(!tetra_confirm_carrier_proven(s.state));
    const dsd_call_snapshot named = canonical(s.state);
    assert(named.phase == DSD_CALL_PHASE_ACTIVE && named.epoch == call.epoch && named.ota_source_id == 0x4444u);
    session_close(&s);
}

/* A reallocation of the followed call to another carrier keeps it the canonical call: while its tune is in flight the
 * canonical call stays on the channel the receiver is on, and once the tune completes it is on the new one, without
 * waiting for a PDU of the call on the new carrier. */
static void
test_followed_reallocation_moves_the_call(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    dsd_call_snapshot call = canonical(s.state);
    assert(followed_call(s.state) == 0x0A && call.phase == DSD_CALL_PHASE_ACTIVE && call.channel == 120U);
    const uint64_t epoch = call.epoch;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 130);
    const uint64_t request = dsd_trunk_tuning_pending_request();
    assert(g_tunes == 2 && request != 0U);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch && call.channel == 120U);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    call = canonical(s.state);
    assert(followed_call(s.state) == 0x0A && call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch);
    assert(call.channel == 130U && call.frequency_hz == CARRIER_HZ(130));
    session_close(&s);
}

/* A grant of another call takes the receiver from the followed call: while the tune is in flight the call followed
 * before stays the canonical call, on the carrier the receiver is on, and once it completes that call ends as a
 * teardown, as the state machine leaving a call ends it, rather than staying active until the new call publishes. */
static void
test_takeover_ends_the_call_left(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    dsd_call_snapshot call = canonical(s.state);
    assert(followed_call(s.state) == 0x0A && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0x100u);
    const uint64_t epoch = call.epoch;
    s.state->tg_hold = 0x200u;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 2, 130);
    const uint64_t request = dsd_trunk_tuning_pending_request();
    assert(request != 0U && followed_call(s.state) == 0x0B);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch && call.ota_target_id == 0x100u);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
    tetra_sm_tick(s.opts, s.state);
    call = canonical(s.state);
    assert(followed_call(s.state) == 0x0B && call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED
           && call.end_reason == DSD_CALL_END_EXPLICIT);
    send_tx_granted(&s, 0x200u, 0x0B, 0x4444u);
    call = canonical(s.state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch != epoch && call.ota_target_id == 0x200u);
    session_close(&s);
}

/* The followed call's speech is heard on the followed channel only while its record does not place the call elsewhere:
 * an allocation to its group that came with no call PDU, which never tunes, or a reallocation whose tune failed,
 * moves the call off the channel the receiver stays on, which the SwMI may give to any other call next, so speech
 * there is not the followed call's. */
static void
test_followed_record_moved_away(void) {
    for (int failed_tune = 0; failed_tune < 2; failed_tune++) {
        session s = session_open();
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of carrier 120 */
        tetra_confirm_note_followed_call(s.state);
        tetra_state_t* const ts = tetra_st(s.state);
        ts->tdma_valid = 1;
        ts->tn = 2;
        tetra_speech sp;
        assert(tetra_voice_gate(s.opts, s.state, &sp) && sp.followed && sp.call_id == 0x0A);
        if (failed_tune) {
            g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
            send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 130);
            const uint64_t request = dsd_trunk_tuning_pending_request();
            assert(request != 0U);
            dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
            tetra_sm_tick(s.opts, s.state);
            tetra_confirm_note_followed_call(s.state);
        } else {
            send_block(&s, 0x100u, NULL, 4, 130);
        }
        tetra_sm_grant g;
        assert(tetra_sm_followed(s.state, &g) && g.carrier == 120 && g.call_id == 0x0A);
        assert(tetra_calls_peek(s.state, 0x0A)->alloc.carrier == 130 && tetra_confirm_followed_call_proven(s.state));
        ts->tdma_valid = 1;
        ts->tn = 2;
        assert(!tetra_voice_gate(s.opts, s.state, &sp));
        session_close(&s);
    }
}

/* One call holds the downlink of a traffic channel at a time: it carries one call's traffic, under one downlink traffic
 * usage marker, and an MS whose call's downlink carries another call's traffic leaves the channel (EN 300 392-2 clauses
 * 23.5.5, 23.5.6.1.1 c)). Following call A on TN2 of carrier 120, its release unheard, a CRC-passed D-SETUP gives that
 * channel to call B of a blocked group: A's record holds the channel no more, the state machine lets go of A at once as
 * a "go to the MCCH" allocation of A makes it (the return to the control channel), A's canonical call ends, and B is
 * not followed. Where the return is deferred, the receiver stays on the channel but follows no call there: B's speech
 * is not A's, so it plays under no identity and renews no hangtime, and the hangtime from A's own last speech takes
 * the receiver back. */
static void
test_blocked_call_takes_the_followed_channel(void) {
    for (int deferred = 0; deferred < 2; deferred++) {
        session s = session_open();
        session_history(&s);
        block_id(&s, 0x200u);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of carrier 120 */
        send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
        speech(&s, 2);
        tetra_state_t* const ts = tetra_st(s.state);
        dsd_call_snapshot call = canonical(s.state);
        assert(followed_call(s.state) == 0x0A && ts->sm.t_voice_m > 0.0 && call.media_active == 1U);
        assert(publishes_call_to(s.state, 0x100u));
        const uint64_t epoch = call.epoch;

        at_seconds(1000.5);
        g_return_result = deferred ? DSD_TRUNK_TUNE_RESULT_DEFERRED : DSD_TRUNK_TUNE_RESULT_OK;
        send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 4, 120);
        const tetra_call_t* const a = tetra_calls_peek(s.state, 0x0A);
        const tetra_call_t* const b = tetra_calls_peek(s.state, 0x0B);
        assert(a && b && a->alloc.timeslots == 0u && b->alloc.carrier == 120 && b->alloc.timeslots == 4);
        assert(g_tunes == 1 && g_returns == 1 && tetra_calls_followed(s.state) == NULL);
        call = canonical(s.state);
        assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED && call.end_reason == DSD_CALL_END_EXPLICIT);
        if (!deferred) {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && followed_call(s.state) == -1);
            session_close(&s);
            continue;
        }
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && !tetra_confirm_followed_call_proven(s.state));
        at_seconds(1001.0);
        ts->tdma_valid = 1;
        ts->tn = 2;
        tetra_speech sp;
        assert(!tetra_voice_gate(s.opts, s.state, &sp));
        speech(&s, 2);
        call = canonical(s.state);
        assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED && ts->sm.t_voice_m < 1000.5);
        g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
        at_seconds(1000.0 + TETRA_SM_SPEECH_GAP_S + (double)s.opts->trunk_hangtime + 0.1);
        tetra_sm_tick(s.opts, s.state);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 2);
        session_close(&s);
    }
}

/* The same with B's group allowed: the channel is B's now, so the state machine follows B where it is, without a
 * retune and without the takeover a call competing with the followed one needs (A holds no channel to compete for):
 * A's canonical call ends, B's publishes, and the speech on the channel is B's. */
static void
test_allowed_call_takes_the_followed_channel(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    speech(&s, 2);
    const uint64_t epoch = canonical(s.state).epoch;
    at_seconds(1000.5);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0B && tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0B));
    assert(g_tunes == 1 && g_returns == 0 && tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 0u);
    assert(canonical(s.state).epoch != epoch && publishes_call_to(s.state, 0x200u));
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 2;
    tetra_speech sp;
    assert(tetra_voice_gate(s.opts, s.state, &sp) && sp.followed && sp.call_id == 0x0B && sp.target == 0x200u);
    session_close(&s);
}

/* The followed channel is taken whatever the followed call's record holds: once an allocation that came with no call
 * PDU moved the record off the channel the receiver stays on (test_followed_record_moved_away()), a D-SETUP that gives
 * that channel to a call of a blocked group lets go of the follow; with the return deferred, the followed call's
 * canonical call still ends there and the receiver follows the channel without a call record. */
static void
test_moved_followed_channel_taken(void) {
    session s = session_open();
    session_history(&s);
    block_id(&s, 0x200u);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of carrier 120 */
    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    send_block(&s, 0x100u, NULL, 2, 120); /* the record to TN3, no tune */
    assert(followed_call(s.state) == 0x0A && tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 2);
    assert(publishes_call_to(s.state, 0x100u));
    const uint64_t epoch = canonical(s.state).epoch;
    g_return_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 4, 120);
    assert(g_returns == 1 && tetra_sm_get_state(s.state) == TETRA_SM_TUNED && tetra_calls_followed(s.state) == NULL);
    assert(tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 2 && !tetra_confirm_followed_call_proven(s.state));
    const dsd_call_snapshot call = canonical(s.state);
    assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED);
    session_close(&s);
}

/* A followed call whose channel another call is allocated while a tune to a third call is in flight comes back without
 * a call record when that tune fails, as one whose record aged does (test_aged_rollback_call_retired()): what its
 * channel carries is the other call's. */
static void
test_rollback_channel_taken(void) {
    session s = session_open();
    block_id(&s, 0x200u);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    s.state->tg_hold = 0x300u;
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 2, 130);
    const uint64_t request = dsd_trunk_tuning_pending_request();
    assert(g_tunes == 2 && request != 0U && followed_call(s.state) == 0x0C);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 4, 120);
    assert(tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 0u && g_returns == 0);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
    assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));
    session_close(&s);
}

/* A MAC-RESOURCE to @p ssi with an allocation of @p timeslots on @p carrier and, for @p call_id not 0, a D-SETUP (table
 * 14.15) of that call with @p basic_service; one of several in a block. */
static void
put_resource(ttb_block* b, uint32_t ssi, uint16_t call_id, uint32_t basic_service, uint32_t timeslots,
             uint32_t carrier) {
    const int start = ttb_resource(b, 0, TETRA_MAC_ADDR_SSI, ssi);
    ttb_resource_flags(b, 1u);
    ttb_allocation(b, timeslots, 1, carrier);
    if (call_id != 0U) {
        ttb_put(b, 2, 4); /* BL-UDATA */
        ttb_d_setup(b, call_id, basic_service);
    }
    ttb_close_resource(b, start);
}

/* Block @p b decoded as one SCH/F block, after a CRC-passed block that proved the carrier. */
static void
send_built_block(session* s, ttb_block* b) {
    ttb_finish(b, TTB_SCH_F_BITS, -1);
    tetra_confirm_note_signalling(s->state);
    tetra_mac_parse_schd(b->bits, b->n, 1, s->opts, s->state);
}

/* One SCH/F block of two MAC-RESOURCEs, each a D-SETUP of group call @p call_id[i] to @p ssi[i] with an allocation of
 * TN2 of @p carrier. */
static void
send_two_setups(session* s, const uint32_t* ssi, const uint16_t* call_id, uint32_t carrier) {
    ttb_block b = {0};
    for (int i = 0; i < 2; i++) {
        put_resource(&b, ssi[i], call_id[i], TTB_SPEECH_GROUP, 4, carrier);
    }
    send_built_block(s, &b);
}

/* A burst as the decoder takes one (processTetraFrame() in tetra.c): the state machine settles once, at its entry and
 * before any of its blocks, and a settle that moved the receiver leaves the burst to the carrier it was captured on,
 * so it is dropped whole. 1 when the burst is decoded. Nothing inside the burst settles. */
static int
burst_begin(session* s) {
    const uint32_t captured = tetra_confirm_carrier_epoch(s->state);
    (void)tetra_sm_settle(s->opts, s->state);
    tetra_confirm_burst_begin_captured(s->state, captured);
    return tetra_confirm_burst_current(s->state);
}

static void
burst_end(session* s) {
    (void)tetra_confirm_burst_end(s->state);
}

/* What a burst decodes is judged against the state machine its entry settled. Following call A on TN2 of carrier 120,
 * one block grants call C TN2 of carrier 130, taking the receiver from A (a preempting priority, or the held group),
 * whose tune the backend reports in flight and fails at once, and then gives that channel to call B. The rest of the
 * burst still sees C's tune in flight: B takes C's channel, and the state machine takes no grant then, so B is not
 * tuned to, whether its tune would succeed or fail and whether it may take the receiver from A. The next burst's entry
 * puts A back, untouched, and nothing is let go of. */
static void
test_allocation_judged_after_a_failed_tune(void) {
    const struct {
        uint32_t hold;
        uint32_t c_ssi;
        dsd_trunk_tune_result b_tune;
    } k_cases[] = {
        {0u, 0x300u, DSD_TRUNK_TUNE_RESULT_OK},
        {0u, 0x300u, DSD_TRUNK_TUNE_RESULT_FAILED},
        {0x200u, 0x200u, DSD_TRUNK_TUNE_RESULT_FAILED},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        session s = session_open();
        policy_row(s.state, 0x100u, 10, 0);
        policy_row(s.state, 0x300u, 50, 1);
        policy_row(s.state, 0x200u, 60, 0);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        assert(followed_call(s.state) == 0x0A && g_tunes == 1);
        at_seconds(1002.0);
        s.state->tg_hold = k_cases[i].hold;
        g_tunes_failing_at_once = 1;
        g_tune_result = k_cases[i].b_tune;
        const uint32_t ssi[2] = {k_cases[i].c_ssi, 0x200u};
        const uint16_t ids[2] = {0x0C, 0x0B};
        assert(burst_begin(&s));
        send_two_setups(&s, ssi, ids, 130);
        burst_end(&s);
        assert(tetra_calls_peek(s.state, 0x0B)->alloc.carrier == 130);
        assert(tetra_calls_peek(s.state, 0x0C)->alloc.timeslots == 0u);
        assert(g_tunes == 2 && g_returns == 0 && dsd_trunk_tuning_pending_request() != 0U);
        assert(burst_begin(&s));
        burst_end(&s);
        assert(g_tunes == 2 && g_returns == 0 && dsd_trunk_tuning_pending_request() == 0U);
        assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
        assert(tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0A));
        session_close(&s);
    }
}

/* So is a "go to the MCCH" allocation (timeslots 0000, table 21.87): one block grants call C of a preempting priority,
 * whose tune the backend reports in flight and fails at once, and then sends the followed call A's party to the
 * control channel. A is the call that tune would put back, which has left its channel: the next burst's entry puts the
 * channel back without a call, so nothing played there is A's, until the grant timeout or hangtime gives it up. */
static void
test_go_to_mcch_after_a_failed_tune(void) {
    session s = session_open();
    policy_row(s.state, 0x100u, 10, 0);
    policy_row(s.state, 0x300u, 50, 1);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1);
    at_seconds(1002.0);
    g_tunes_failing_at_once = 1;
    ttb_block b = {0};
    put_resource(&b, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 130);
    put_resource(&b, 0x100u, 0U, 0u, 0, 100);
    assert(burst_begin(&s));
    send_built_block(&s, &b);
    burst_end(&s);
    assert(burst_begin(&s));
    burst_end(&s);
    assert(g_tunes == 2 && g_returns == 0 && dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
    assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));
    session_close(&s);
}

/* "Go to the MCCH" to the followed call itself while its own reallocation's tune is in flight: following A on TN2 of
 * carrier 120, a burst reallocates A to carrier 130, the backend fails that tune during the burst, and the burst then
 * sends A's party to the control channel. The release cannot be made then (the tune is in flight for the rest of the
 * burst), so A is retired, in what the failure puts back too: the next tick returns the receiver to carrier 120
 * without A, whose allocation went, so nothing of the channel A left plays as A's or keeps the follow, which the grant
 * timeout then gives up. */
static void
test_go_to_mcch_during_own_tune(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    send_tx_granted(&s, 0x100u, 0x0A, 0x4444u);
    assert(followed_call(s.state) == 0x0A && tetra_confirm_followed_call_proven(s.state) && g_tunes == 1);
    assert(burst_begin(&s));
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 130);
    const uint64_t request = dsd_trunk_tuning_pending_request();
    assert(g_tunes == 2 && request != 0U && followed_call(s.state) == 0x0A);
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    ttb_block b = {0};
    put_resource(&b, 0x100u, 0U, 0u, 0, 100);
    send_built_block(&s, &b);
    burst_end(&s);
    assert(tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 0u && g_returns == 0);
    tetra_sm_tick(s.opts, s.state);
    tetra_sm_grant g;
    assert(tetra_sm_followed(s.state, &g) && g.carrier == 120 && dsd_trunk_tuning_pending_request() == 0U);
    assert(tetra_calls_followed(s.state) == NULL && !tetra_confirm_followed_call_proven(s.state));
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 2;
    tetra_speech sp;
    assert(!tetra_voice_gate(s.opts, s.state, &sp));
    speech(&s, 2);
    assert(ts->sm.t_voice_m <= 0.0);
    at_seconds(1000.0 + TETRA_SM_GRANT_TIMEOUT_S + 0.1);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && g_returns == 1);
    session_close(&s);
}

/* And so is a followed call's encryption: with encrypted calls not followed, one block grants call C of a preempting
 * priority, whose tune the backend reports in flight and fails at once, and then carries a D-SETUP of the followed
 * call A that makes it end-to-end encrypted and reallocates it to carrier 140. The rest of the burst sees C's tune in
 * flight, so nothing tunes to A on carrier 140; the next burst's entry puts A back, and A's next PDU, its late-entry
 * D-SETUP, judges it as the followed call: it is locked out and let go of, and still not tuned to on carrier 140
 * whether the return completes or is deferred (which keeps the receiver where it is, but moves it nowhere). */
static void
test_encrypted_reallocation_after_a_failed_tune(void) {
    for (int deferred = 0; deferred < 2; deferred++) {
        session s = session_open();
        s.opts->trunk_tune_enc_calls = 0;
        policy_row(s.state, 0x100u, 10, 0);
        policy_row(s.state, 0x300u, 50, 1);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
        assert(followed_call(s.state) == 0x0A && g_tunes == 1);
        at_seconds(1002.0);
        g_tunes_failing_at_once = 1;
        g_return_result = deferred ? DSD_TRUNK_TUNE_RESULT_DEFERRED : DSD_TRUNK_TUNE_RESULT_OK;
        ttb_block b = {0};
        put_resource(&b, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 130);
        put_resource(&b, 0x100u, 0x0A, TTB_SPEECH_GROUP | 0x10u, 4, 140); /* encryption flag (table 14.35) */
        assert(burst_begin(&s));
        send_built_block(&s, &b);
        burst_end(&s);
        assert(g_tunes == 2 && tetra_calls_peek(s.state, 0x0A)->e2ee);
        assert(burst_begin(&s));
        assert(tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0A));
        ttb_block late = {0};
        put_resource(&late, 0x100u, 0x0A, TTB_SPEECH_GROUP | 0x10u, 4, 140);
        send_built_block(&s, &late);
        burst_end(&s);
        assert(g_tunes == 2 && g_returns == 1 && dsd_enc_lockout_entry_active(s.state, 0x100u, 1) == 1);
        tetra_sm_grant g;
        if (deferred) {
            assert(tetra_sm_followed(s.state, &g) && g.call_id == 0x0A && g.carrier == 120);
        } else {
            assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && !tetra_sm_followed(s.state, &g));
        }
        session_close(&s);
    }
}

/* Following call B (group 0x200) on TN2 of carrier 120, event label 0x55 assigned to its party there, a preempting call
 * C (group 0x300) granted TN2 of carrier 130, whose tune is in flight, and label 0x55 assigned to C's party there too,
 * another label on another channel. @p request is the tune's. */
static session
session_with_tune_in_flight(uint64_t* request) {
    session s = session_open();
    policy_row(s.state, 0x200u, 10, 0);
    policy_row(s.state, 0x300u, 50, 1);
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 4, 120);
    send_event_label(&s, 0x200u, 0x55u, 4, 120);
    tetra_confirm_forget_followed_call(s.state); /* as a retune leaves it: nothing proved B on its carrier yet */
    assert(followed_call(s.state) == 0x0B && g_tunes == 1);
    at_seconds(1002.0);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 130);
    *request = dsd_trunk_tuning_pending_request();
    assert(*request != 0U && followed_call(s.state) == 0x0C && g_tunes == 2);
    send_event_label(&s, 0x300u, 0x55u, 4, 130);
    const tetra_call_t* const b = tetra_calls_peek(s.state, 0x0B);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0C);
    assert((b->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && b->event_label == 0x55u && b->label_chan.carrier == 120);
    assert((c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == 0x55u && c->label_chan.carrier == 130);
    g_tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    return s;
}

/* A burst on carrier 120 whose first half decodes before the backend completes the tune to C and whose second half is
 * a MAC-RESOURCE addressed by event label 0x55: a "go to the MCCH" allocation (timeslots 0000), or, with encrypted
 * calls not followed, an encrypted one, its allocation ciphertext. Sent on carrier 120, the label is B's there, never
 * C's on carrier 130: the rest of the burst still sees C's tune in flight, which names no carrier being received, so
 * the PDU is no call's, and C is neither let go of nor marked encrypted. The completion is taken at the next burst's
 * entry, which commits the follow of C and drops that burst, captured on carrier 120, whole. */
static void
test_tune_answered_mid_burst(void) {
    for (int encrypted = 0; encrypted < 2; encrypted++) {
        uint64_t request = 0U;
        session s = session_with_tune_in_flight(&request);
        s.opts->trunk_tune_enc_calls = encrypted ? 0 : 1;
        assert(burst_begin(&s));
        send_block(&s, 0x999u, NULL, 0, 0); /* first half: a MAC-RESOURCE to another party */
        dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_OK);
        ttb_block b = {0};
        const int start = ttb_resource(&b, encrypted ? TETRA_ENC_MODE_ODD : 0u, TETRA_MAC_ADDR_EVENT_LABEL, 0u);
        ttb_put(&b, 0x55u, 10);
        ttb_resource_flags(&b, 1u);
        if (encrypted) {
            ttb_put(&b, 0x2A5u, 30); /* the channel allocation and TM-SDU, ciphertext */
        } else {
            ttb_allocation(&b, 0, 1, 100);
        }
        ttb_close_resource(&b, start);
        send_built_block(&s, &b);
        burst_end(&s);
        const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0C);
        assert(g_returns == 0 && c->alloc.carrier == 130 && c->alloc.timeslots == 4);
        assert(c->air_enc == TETRA_ENC_MODE_NONE && dsd_enc_lockout_entry_active(s.state, 0x300u, 1) == 0);
        /* The state machine has not taken the completion: the tune is still its request in flight. */
        assert(followed_call(s.state) == 0x0C && tetra_st(s.state)->sm.pending_request_id == request);
        assert(!burst_begin(&s));
        burst_end(&s);
        tetra_sm_grant g;
        assert(tetra_sm_followed(s.state, &g) && g.call_id == 0x0C && g.carrier == 130);
        assert(tetra_st(s.state)->sm.pending_request_id == 0U && dsd_trunk_tuning_pending_request() == 0U);
        assert(g_returns == 0);
        session_close(&s);
    }
}

/* Every PDU of a burst sees the one state machine the burst's entry settled: an answer that lands during the burst,
 * here the failure of C's tune, is taken at the next burst. Until then neither the call records' entry points
 * (tetra_calls_on_addressed(), tetra_calls_on_event(), tetra_calls_on_allocation()) nor the state machine's
 * (tetra_sm_on_release(), tetra_sm_on_grant(), tetra_sm_on_cc_sync()) settle it: C's tune stays in flight, so the
 * state machine follows C, takes no grant and lets nothing go, and the PDU to B's party proves nothing of B; the next
 * burst puts B back as the tune left it. */
static void
test_burst_sees_one_state_machine(void) {
    uint64_t request = 0U;
    session s = session_with_tune_in_flight(&request);
    assert(burst_begin(&s));
    dsd_trunk_tuning_request_publish(request, DSD_TRUNK_TUNE_RESULT_FAILED);
    tetra_calls_on_addressed(s.opts, s.state, &(tetra_mac_address){.type = TETRA_MAC_ADDR_SSI, .ssi = 0x200u},
                             TETRA_ENC_MODE_NONE, 0, 2);
    tetra_call_event setup;
    DSD_MEMSET(&setup, 0, sizeof setup);
    setup.kind = TETRA_CALL_EV_SETUP;
    setup.call_id = 0x0B;
    setup.basic_service = TTB_SPEECH_GROUP;
    tetra_mac_sdu sdu;
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_SSI;
    sdu.addr.ssi = 0x200u;
    sdu.has_addr = 1;
    tetra_calls_on_event(s.opts, s.state, &setup, &sdu);
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_EVENT_LABEL;
    sdu.addr.event_label = 0x55u;
    sdu.has_addr = 1;
    tetra_calls_on_allocation(s.opts, s.state, &sdu);
    tetra_sm_on_release(s.opts, s.state, 0x0C);
    const tetra_sm_grant grant = {CARRIER_HZ(140), 140, 4, 0x0D};
    assert(tetra_sm_on_grant(s.opts, s.state, &grant) == 0);
    tetra_sm_on_cc_sync(s.opts, s.state);
    assert(followed_call(s.state) == 0x0C && dsd_trunk_tuning_pending_request() == request);
    assert(g_tunes == 2 && g_returns == 0);
    burst_end(&s);
    assert(burst_begin(&s));
    burst_end(&s);
    /* B is back as the tune left it: the PDU addressed to its party during the burst proved nothing of it. */
    assert(followed_call(s.state) == 0x0B && tetra_calls_followed(s.state) == tetra_calls_peek(s.state, 0x0B));
    assert(!tetra_confirm_followed_call_proven(s.state));
    assert(dsd_trunk_tuning_pending_request() == 0U && g_tunes == 2 && g_returns == 0);
    session_close(&s);
}

/* Not trunking, the heard timeslot's speech is the call whose traffic channel it is now: a D-SETUP that gives TN2 of
 * the carrier being received to call B of a blocked group takes it from the heard call A, whose canonical call ends
 * there and whose later PDUs publish nothing as the heard call's; B's speech on it is B's, muted; and once B moves on
 * to TN3 the speech of TN2 is no record's, not A's. */
static void
test_heard_channel_taken(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    block_id(&s, 0x700u);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100); /* TN2 of the carrier being received */
    speech(&s, 2);
    assert(publishes_call_to(s.state, 0x300u));
    const uint64_t epoch = canonical(s.state).epoch;
    at_seconds(1000.5);
    send_setup(&s, 0x700u, 0x0D, TTB_SPEECH_GROUP, 4, 100);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED);
    const tetra_call_t* const b = tetra_calls_peek(s.state, 0x0D);
    assert(tetra_calls_peek(s.state, 0x0C)->alloc.timeslots == 0u && tetra_calls_on_timeslot(s.opts, s.state, 2) == b);
    send_tx_granted(&s, 0x300u, 0x0C, 0x5555u);
    call = canonical(s.state);
    assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 2;
    tetra_speech sp;
    assert(tetra_voice_gate(s.opts, s.state, &sp) && sp.kind == TETRA_SPEECH_CALL && sp.call_id == 0x0D);
    assert(tetra_calls_speech_muted(s.opts, s.state, &sp));
    send_setup(&s, 0x700u, 0x0D, TTB_SPEECH_GROUP, 2, 100); /* TN3 */
    assert(tetra_calls_on_timeslot(s.opts, s.state, 2) == NULL && tetra_calls_on_timeslot(s.opts, s.state, 3) == b);
    session_close(&s);
}

/* A D-SETUP of call @p call_id to @p ssi whose allocation assigns @p up_down (table 21.87: 01 the downlink, 10 the
 * uplink) of @p timeslots on carrier 120, as the MLE hands it to the call records. */
static void
setup_allocating(session* s, uint32_t ssi, uint16_t call_id, uint8_t up_down, uint8_t timeslots) {
    tetra_call_event setup;
    DSD_MEMSET(&setup, 0, sizeof setup);
    setup.kind = TETRA_CALL_EV_SETUP;
    setup.call_id = call_id;
    setup.basic_service = TTB_SPEECH_GROUP;
    tetra_mac_sdu sdu;
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_SSI;
    sdu.addr.ssi = ssi;
    sdu.has_addr = 1;
    sdu.has_alloc = 1;
    sdu.alloc.timeslots = timeslots;
    sdu.alloc.up_down = up_down;
    sdu.alloc.carrier = 120;
    sdu.alloc.freq_hz = CARRIER_HZ(120);
    tetra_calls_on_event(s->opts, s->state, &setup, &sdu);
}

/* An allocation takes the downlink it assigns and nothing else: one of another timeslot or carrier, one of the uplink
 * alone (the uplink and downlink of a timeslot may be allocated to two calls, EN 300 392-2 clause 23.5.5; the decoder
 * receives the downlink), and "go to the control channel" (timeslots 0000, no channel the decoder can place) leave the
 * followed call its channel and the state machine its follow. */
static void
test_other_channels_take_nothing(void) {
    session s = session_open();
    block_id(&s, 0x200u);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* TN2 of carrier 120 */
    send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 2, 120); /* TN3 of carrier 120 */
    send_setup(&s, 0x200u, 0x0C, TTB_SPEECH_GROUP, 4, 130); /* TN2 of carrier 130 */
    setup_allocating(&s, 0x200u, 0x0D, 2u, 4u);             /* the uplink of TN2 of carrier 120 */
    setup_allocating(&s, 0x200u, 0x0E, 1u, 0u);             /* the control channel */
    const tetra_call_t* const a = tetra_calls_peek(s.state, 0x0A);
    assert(a && a->alloc.carrier == 120 && a->alloc.timeslots == 4 && tetra_calls_followed(s.state) == a);
    assert(tetra_calls_peek(s.state, 0x0B)->alloc.timeslots == 2
           && tetra_calls_peek(s.state, 0x0C)->alloc.carrier == 130);
    assert(tetra_calls_peek(s.state, 0x0D)->alloc.timeslots == 4
           && tetra_calls_peek(s.state, 0x0E)->alloc.timeslots == 0);
    assert(followed_call(s.state) == 0x0A && g_tunes == 1 && g_returns == 0);
    session_close(&s);
}

/* D-INFO (table 14.11) of call @p call_id with no optional element, and an allocation of @p timeslots on @p carrier. */
static void
send_info_allocating(session* s, uint32_t ssi, uint16_t call_id, uint32_t timeslots, uint32_t carrier) {
    ttb_block cmce = {0};
    ttb_put(&cmce, TETRA_MLE_PD_CMCE, 3);
    ttb_put(&cmce, TETRA_CMCE_D_INFO, 5);
    ttb_put(&cmce, call_id, 14);
    ttb_put(&cmce, 0, 2); /* reset call time-out timer, poll request */
    ttb_put(&cmce, 0, 1); /* O-bit */
    send_block(s, ssi, &cmce, timeslots, carrier);
}

/* Every path that gives a call a channel takes it from the call that held it, followed or not: an allocation addressed
 * to a party of call C (of a blocked group) with no call PDU, which never tunes, a reallocation of C in its D-INFO, and
 * one in the D-CALL RESTORE of a D-RESTORE-ACK, each give C the followed channel and let go of the followed call A. */
static void
test_every_allocation_takes_the_channel(void) {
    for (int path = 0; path < 3; path++) {
        session s = session_open();
        block_id(&s, 0x200u);
        send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120); /* followed: TN2 of carrier 120 */
        send_setup(&s, 0x200u, 0x0B, TTB_SPEECH_GROUP, 2, 130);
        assert(followed_call(s.state) == 0x0A && g_returns == 0);
        if (path == 0) {
            send_block(&s, 0x200u, NULL, 4, 120);
        } else if (path == 1) {
            send_info_allocating(&s, 0x200u, 0x0B, 4, 120);
        } else {
            tetra_call_event restore;
            DSD_MEMSET(&restore, 0, sizeof restore);
            restore.kind = TETRA_CALL_EV_RESTORE;
            restore.call_id = 0x0B;
            tetra_mac_sdu sdu;
            DSD_MEMSET(&sdu, 0, sizeof sdu);
            sdu.addr.type = TETRA_MAC_ADDR_SSI;
            sdu.addr.ssi = 0x200u;
            sdu.has_addr = 1;
            sdu.has_alloc = 1;
            sdu.alloc.timeslots = 4;
            sdu.alloc.up_down = 1;
            sdu.alloc.carrier = 120;
            sdu.alloc.freq_hz = CARRIER_HZ(120);
            tetra_calls_on_event(s.opts, s.state, &restore, &sdu);
        }
        assert(tetra_calls_peek(s.state, 0x0B)->alloc.carrier == 120);
        assert(tetra_calls_peek(s.state, 0x0A)->alloc.timeslots == 0u);
        assert(followed_call(s.state) == -1 && g_returns == 1 && g_tunes == 1);
        session_close(&s);
    }
}

/* Not trunking, the heard call's identifier goes with its record, as the followed call's does: once the heard call is
 * released, a D-SETUP gave its identifier to another call or a full table evicted it, the call given the identifier is
 * not the heard call until its own speech is heard, so its signalling publishes nothing (its channel may be another
 * carrier's), and its speech on the heard timeslot publishes it. A record that leaves the table ends its canonical
 * call, which nothing names from there. */
static void
test_heard_call_identifier_let_go(void) {
    session s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100); /* TN2 of the carrier being received */
    speech(&s, 2);
    assert(publishes_call_to(s.state, 0x300u));
    send_call_pdu(&s, 0x300u, TETRA_CMCE_D_RELEASE, 0x0C, 5);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    at_seconds(1000.0 + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    send_setup(&s, 0x700u, 0x0C, TTB_SPEECH_GROUP, 2, 130);
    send_tx_granted(&s, 0x700u, 0x0C, 0x4444u);
    assert(canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    session_close(&s);

    /* The heard call's identifier taken by a D-SETUP for another group on the same timeslot, its release unheard. */
    s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100);
    speech(&s, 2);
    const uint64_t epoch = canonical(s.state).epoch;
    at_seconds(1000.0 + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    send_setup(&s, 0x700u, 0x0C, TTB_SPEECH_GROUP, 4, 100);
    send_tx_granted(&s, 0x700u, 0x0C, 0x4444u);
    dsd_call_snapshot call = canonical(s.state);
    assert(call.epoch == epoch && call.phase == DSD_CALL_PHASE_ENDED && call.ota_target_id == 0x300u);
    speech(&s, 2);
    call = canonical(s.state);
    assert(call.epoch != epoch && call.phase == DSD_CALL_PHASE_ACTIVE && call.ota_target_id == 0x700u
           && call.ota_source_id == 0x4444u);
    session_close(&s);

    /* The heard call evicted from a full table while its speech is heard, its release unheard: its canonical call ends
     * with its record, and a call later given its identifier is not the heard call either. */
    s = session_open();
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    send_setup(&s, 0x300u, 0x0C, TTB_SPEECH_GROUP, 4, 100);
    speech(&s, 2);
    const uint64_t heard = canonical(s.state).epoch;
    at_seconds(1000.0 + 2.0 * DSD_CALL_REACQUIRE_GAP_S);
    for (uint16_t id = 0x20; id < 0x20 + TETRA_CALLS_MAX; id++) {
        send_setup(&s, 0x400u + id, id, TTB_SPEECH_GROUP, 2, 130);
    }
    call = canonical(s.state);
    assert(tetra_calls_peek(s.state, 0x0C) == NULL && call.epoch == heard && call.phase == DSD_CALL_PHASE_ENDED);
    send_setup(&s, 0x700u, 0x0C, TTB_SPEECH_GROUP, 2, 130);
    send_tx_granted(&s, 0x700u, 0x0C, 0x4444u);
    call = canonical(s.state);
    assert(call.epoch == heard && call.phase == DSD_CALL_PHASE_ENDED);
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

/* Not trunking, speech belongs to the record whose traffic channel is its timeslot of the frequency the receiver is on.
 * Trunking turned off while following keeps the receiver, and the state machine the channel, on the followed carrier,
 * whatever the input reports as tuned (an audio input, a rigctl peer's included, reports none): the speech of the call's
 * timeslot is the call's, judged by its own encryption, not anonymous speech judged by the timeslot's signalling. */
static void
test_held_channel_speech_is_the_records(void) {
    session s = session_open();
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP | 0x10u, 4, 120);
    assert(followed_call(s.state) == 0x0A && s.opts->audio_in_type == AUDIO_IN_PULSE);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    assert(c && tetra_calls_on_timeslot(s.opts, s.state, 2) == c
           && tetra_calls_on_timeslot(s.opts, s.state, 3) == NULL);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->tdma_valid = 1;
    ts->tn = 2;
    tetra_speech sp;
    assert(tetra_voice_gate(s.opts, s.state, &sp) && sp.kind == TETRA_SPEECH_CALL && sp.call_id == 0x0A && sp.sealed);
    session_close(&s);
}

/* Trunking started on a secondary carrier rests ON_CC there without a retune once the SYSINFO names the main carrier,
 * which every carrier of the cell broadcasts (EN 300 392-2 clause 21.4.4.1): the carrier being received is the one the
 * receiver is tuned to, not the main carrier, for TB5 and for an event label bound to an allocated channel alike. A
 * label assigned for the secondary carrier names its party there, so its label-addressed allocation and the encryption
 * of its label-addressed MAC-RESOURCE reach the call. */
static void
test_on_cc_carrier_is_the_tuned_one(void) {
    session s = session_open();
    s.opts->trunk_tune_group_calls = 0;
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(130);
    tetra_state_t* const ts = tetra_st(s.state);
    ts->dl_carrier_hz = CARRIER_HZ(100);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC);
    uint16_t cn = 0;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 130);

    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 0, 0);
    const tetra_call_t* const c = tetra_calls_peek(s.state, 0x0A);
    send_event_label(&s, 0x100u, 0x55u, 4, 130);
    assert(c && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->alloc.carrier == 130 && c->alloc.timeslots == 4);
    send_by_event_label(&s, 0x55u, 2, 130);
    assert(c->alloc.carrier == 130 && c->alloc.timeslots == 2 && g_tunes == 0);
    tetra_calls_on_addressed(s.opts, s.state,
                             &(tetra_mac_address){.type = TETRA_MAC_ADDR_EVENT_LABEL, .event_label = 0x55u},
                             TETRA_ENC_MODE_ODD, 1, 0);
    assert(c->air_enc == TETRA_ENC_MODE_ODD);
    session_close(&s);
}

/* Trunking turned off while following leaves the receiver on the followed channel and lets go of the call there: its
 * record is no followed call's any more, so it ages like any other (the followed record never does) and its canonical
 * call ends with it, while the state machine keeps the channel and asks for no return. */
static void
test_trunking_off_lets_go_of_the_call(void) {
    session s = session_open();
    session_history(&s);
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(followed_call(s.state) == 0x0A && canonical(s.state).phase == DSD_CALL_PHASE_ACTIVE);
    s.opts->trunk_enable = 0;
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && followed_call(s.state) == 0x0A);
    assert(tetra_calls_followed(s.state) == NULL && tetra_calls_peek(s.state, 0x0A) != NULL);
    at_seconds(1000.0 + TETRA_CALL_IDLE_S + 1.0);
    tetra_sm_tick(s.opts, s.state);
    assert(tetra_calls_peek(s.state, 0x0A) == NULL && canonical(s.state).phase == DSD_CALL_PHASE_ENDED);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_TUNED && g_returns == 0);
    session_close(&s);
}

/* The carrier being received, for TB5: the followed call's, else the one the receiver is tuned to exactly, on the
 * control channel too (not the SYSINFO main carrier, which a secondary carrier broadcasts as well); none between
 * carriers, in direct mode or off any carrier. */
static void
test_receiving_carrier(void) {
    session s = session_open();
    tetra_state_t* const ts = tetra_st(s.state);
    uint16_t cn = 0;
    ts->dl_carrier_hz = CARRIER_HZ(100);
    assert(tetra_sm_get_state(s.state) == TETRA_SM_ON_CC && tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    s.opts->audio_in_type = AUDIO_IN_RTL;
    s.opts->rtlsdr_center_freq = (uint32_t)CARRIER_HZ(100);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 100);
    s.opts->audio_in_type = AUDIO_IN_PULSE;
    send_setup(&s, 0x100u, 0x0A, TTB_SPEECH_GROUP, 4, 120);
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 1 && cn == 120);
    ts->sm.pending_request_id = 9;
    assert(tetra_calls_carrier(s.opts, s.state, &cn) == 0);
    ts->sm.pending_request_id = 0;

    s.opts->trunk_enable = 0;
    tetra_sm_on_external_cc_return(s.opts, s.state);
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
    test_followed_call_renamed_first_on_its_carrier();
    test_setup_reusing_followed_id();
    test_setup_reusing_id_with_another_caller();
    test_reused_id_while_following_held();
    test_released_followed_id_retired();
    test_rename_onto_followed_id();
    test_aged_rollback_call_retired();
    test_table_eviction_and_ageing();
    test_followed_call_publishes();
    test_header_encryption_reaches_the_call();
    test_fragmented_header_encryption_reaches_the_call();
    test_grant_without_talker_is_the_next_transmission();
    test_private_call_names_both_parties();
    test_encrypted_and_unfollowed_calls();
    test_sds_and_status_notices();
    test_direct_mode_calls();
    test_direct_mode_idle_ends_the_transmission();
    test_direct_mode_transmissions_apart();
    test_direct_mode_short_data();
    test_direct_mode_short_data_fcs();
    test_direct_mode_short_data_repeats();
    test_events_read_the_new_carrier();
    test_cell_change_forgets_sysinfo();
    test_cell_change_forgets_the_carrier();
    test_cell_change_forgets_allocations();
    test_sysinfo_tells_another_cell();
    test_event_label_assignment();
    test_event_label_takes_effect_at_delivery();
    test_event_label_reuse_on_another_channel();
    test_event_label_kept_across_a_channel_change();
    test_event_label_reallocation_and_ageing();
    test_event_label_channel();
    test_network_change_drops_calls();
    test_network_change_retires_the_followed_call();
    test_location_area_change_drops_calls();
    test_followed_call_speech();
    test_heard_call_speech();
    test_anonymous_call_per_timeslot();
    test_record_changes_reach_the_call();
    test_followed_reallocation_moves_the_call();
    test_takeover_ends_the_call_left();
    test_followed_record_moved_away();
    test_blocked_call_takes_the_followed_channel();
    test_allowed_call_takes_the_followed_channel();
    test_moved_followed_channel_taken();
    test_rollback_channel_taken();
    test_allocation_judged_after_a_failed_tune();
    test_go_to_mcch_after_a_failed_tune();
    test_go_to_mcch_during_own_tune();
    test_encrypted_reallocation_after_a_failed_tune();
    test_tune_answered_mid_burst();
    test_burst_sees_one_state_machine();
    test_heard_channel_taken();
    test_other_channels_take_nothing();
    test_every_allocation_takes_the_channel();
    test_heard_call_identifier_let_go();
    test_direct_mode_speech();
    test_trunking_off_lets_go_of_the_call();
    test_held_channel_speech_is_the_records();
    test_on_cc_carrier_is_the_tuned_one();
    test_receiving_carrier();
    DSD_FPRINTF(stdout, "TETRA_CALLS: OK\n");
    return 0;
}
