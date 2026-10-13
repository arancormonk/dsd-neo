// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA calls (tetra_calls.h): the trunked mode call table, the allocations attached to it and the decision to follow.
 */

#include "tetra_calls.h"

#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_confirm.h"
#include "tetra_log.h"
#include "tetra_state.h"

/* Basic service information, EN 300 392-2 table 14.35: circuit mode type (3 bits, 000 speech), encryption flag
 * (end-to-end encryption), communication type (2 bits, 00 point to point) and slots per frame or speech service. */
static int
basic_is_group(uint8_t basic) {
    return ((basic >> 2) & 3u) != 0u;
}

static int
basic_is_data(uint8_t basic) {
    return (basic >> 5) != 0u;
}

static int
basic_e2ee(uint8_t basic) {
    return (basic >> 4) & 1u;
}

/* Table 14.80: transmission granted, or granted to another user. Both mean a transmission is on. */
static int
grant_is_transmission(uint8_t grant) {
    return grant == 0u || grant == 3u;
}

/* The 24-bit identity a MAC address names in the clear (table 21.55): an SSI, USSI or SMI that is not an encrypted
 * short identity. Event labels and usage markers stand for a party, they do not name one. */
static int
address_ssi(const tetra_mac_address* addr, uint32_t* ssi) {
    switch (addr->type) {
        case TETRA_MAC_ADDR_SSI:
        case TETRA_MAC_ADDR_USSI:
        case TETRA_MAC_ADDR_SMI:
        case TETRA_MAC_ADDR_SSI_EVENT:
        case TETRA_MAC_ADDR_SSI_USAGE:
        case TETRA_MAC_ADDR_SMI_EVENT:
            if (addr->esi) {
                return 0;
            }
            *ssi = addr->ssi;
            return 1;
        default: return 0;
    }
}

static int
address_has_event_label(const tetra_mac_address* addr) {
    return addr->type == TETRA_MAC_ADDR_EVENT_LABEL || addr->type == TETRA_MAC_ADDR_SSI_EVENT
           || addr->type == TETRA_MAC_ADDR_SMI_EVENT;
}

static int
sdu_ssi(const tetra_mac_sdu* sdu, uint32_t* ssi) {
    return sdu && sdu->has_addr && address_ssi(&sdu->addr, ssi);
}

/* ---- The table ------------------------------------------------------------------------------------------------- */

static tetra_call_t*
calls_find(tetra_state_t* ts, uint16_t call_id) {
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && c->call_id == call_id) {
            return c;
        }
    }
    return NULL;
}

static void
calls_free(tetra_call_t* c) {
    DSD_MEMSET(c, 0, sizeof(*c));
}

/* The call identifier of the followed call, or -1 when the state machine follows none. */
static int
followed_id(const dsd_state* state) {
    tetra_sm_grant g;
    return tetra_sm_followed(state, &g) ? (int)g.call_id : -1;
}

/* A new record for @p call_id: a free entry, else the one heard from least recently that is not followed. */
static tetra_call_t*
calls_new(const dsd_state* state, tetra_state_t* ts, uint16_t call_id) {
    const int followed = followed_id(state);
    tetra_call_t* victim = NULL;
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase == TETRA_CALL_FREE) {
            victim = c;
            break;
        }
        if ((int)c->call_id != followed && (!victim || c->seen_m < victim->seen_m)) {
            victim = c;
        }
    }
    if (!victim) {
        return NULL;
    }
    calls_free(victim);
    victim->call_id = call_id;
    victim->phase = TETRA_CALL_SETUP;
    return victim;
}

/* The call a party of which @p addr names: by its called, calling or talking SSI, or by the event label assigned to
 * one of them. The most recently heard of several. */
static tetra_call_t*
calls_by_address(tetra_state_t* ts, const tetra_mac_address* addr) {
    uint32_t ssi = 0;
    const int by_ssi = address_ssi(addr, &ssi);
    const int by_label = address_has_event_label(addr);
    tetra_call_t* best = NULL;
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase == TETRA_CALL_FREE) {
            continue;
        }
        const int ssi_match =
            by_ssi && ssi != 0u && (c->called_ssi == ssi || c->calling_ssi == ssi || c->talker_ssi == ssi);
        const int label_match =
            by_label && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && c->event_label == addr->event_label;
        if ((ssi_match || label_match) && (!best || c->seen_m > best->seen_m)) {
            best = c;
        }
    }
    return best;
}

tetra_call_t*
tetra_calls_find(dsd_state* state, uint16_t call_id) {
    tetra_state_t* const ts = tetra_st_existing(state);
    return ts ? calls_find(ts, (uint16_t)(call_id & 0x3FFFu)) : NULL;
}

const tetra_call_t*
tetra_calls_peek(const dsd_state* state, uint16_t call_id) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts) {
        return NULL;
    }
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        const tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && c->call_id == (call_id & 0x3FFFu)) {
            return c;
        }
    }
    return NULL;
}

const tetra_call_t*
tetra_calls_followed(const dsd_state* state) {
    const int id = followed_id(state);
    return id >= 0 ? tetra_calls_peek(state, (uint16_t)id) : NULL;
}

/* ---- Following ------------------------------------------------------------------------------------------------- */

/* Speech of @p c the listener cannot decrypt: end-to-end encrypted, or air encrypted without a key stream for it. */
static int
call_undecryptable(const dsd_state* state, const tetra_call_t* c) {
    return c->e2ee || (c->air_enc != TETRA_ENC_MODE_NONE && !tetra_air_key_usable(state));
}

static uint32_t
call_source(const tetra_call_t* c) {
    return c->talker_ssi != 0u ? c->talker_ssi : c->calling_ssi;
}

/* The talkgroup policy's decision on @p c: a group call on its GSSI, an individual call on its two parties. */
static int
call_decision(const dsd_opts* opts, const dsd_state* state, const tetra_call_t* c, dsd_tg_policy_decision* out) {
    const int encrypted = call_undecryptable(state, c);
    const int data = basic_is_data(c->basic_service);
    if (basic_is_group(c->basic_service)) {
        return dsd_tg_policy_evaluate_group_call(opts, state, c->called_ssi, call_source(c), encrypted, data, out);
    }
    return dsd_tg_policy_evaluate_private_call(opts, state, c->calling_ssi, c->called_ssi, encrypted, data, out);
}

/* The route the talkgroup policy's preemption bookkeeping knows a TETRA call by: its carrier, any timeslot. */
static dsd_tg_policy_call_route
call_route(const tetra_call_t* c, int requires_retune) {
    dsd_tg_policy_call_route route;
    DSD_MEMSET(&route, 0, sizeof route);
    route.target_id = c->called_ssi;
    route.source_id = call_source(c);
    route.freq_hz = c->alloc.freq_hz;
    route.channel = (int)c->alloc.carrier;
    route.slot = -1;
    route.requires_tuner_retune = requires_retune;
    return route;
}

/* A downlink pi/4-DQPSK channel with a frequency and a timeslot: what the state machine can follow. */
static int
alloc_followable(const tetra_chan_alloc* a) {
    return a->timeslots != 0u && (a->up_down == 1u || a->up_down == 3u) && a->freq_hz > 0L && a->modulation == 0u;
}

/* The encryption lockout ledger (core/enc_lockout.h) takes a voice call the listener cannot decrypt, and lets go of
 * one it can. The ledger only matters while encrypted calls are not followed. */
static void
call_note_lockout(const dsd_opts* opts, dsd_state* state, const tetra_call_t* c) {
    if (opts->trunk_tune_enc_calls != 0 || basic_is_data(c->basic_service)) {
        return;
    }
    const int group = basic_is_group(c->basic_service);
    if (!call_undecryptable(state, c)) {
        (void)dsd_enc_lockout_release(state, c->called_ssi, group);
        return;
    }
    if (dsd_enc_lockout_note(state, c->called_ssi, group, DSD_ENC_LOCKOUT_ALGID_UNKNOWN, 0)) {
        DSD_FPRINTF(stderr, "[TETRA CALL] call=%u %s=%u %s encrypted; locked out\n", (unsigned)c->call_id,
                    group ? "GSSI" : "ISSI", (unsigned)c->called_ssi, c->e2ee ? "end-to-end" : "air interface");
    }
}

/* Whether @p c, which the policy allows, takes the receiver from the call it follows: a call of the held talkgroup
 * over one that is not, else a policy preemption (dsd_tg_policy_should_preempt()). */
static int
call_takes_over(const dsd_opts* opts, dsd_state* state, const tetra_call_t* c, const dsd_tg_policy_decision* d,
                const tetra_sm_grant* followed) {
    const tetra_call_t* const cur = tetra_calls_followed(state);
    const double now_m = dsd_decode_now_mono_s();
    dsd_tg_policy_decision cur_d;
    const int cur_judged = cur && call_decision(opts, state, cur, &cur_d) == 0;
    if (d->tg_hold_match && !(cur_judged && cur_d.tg_hold_match)) {
        return 1;
    }
    if (!d->preempt_requested) {
        return 0;
    }
    if (cur_judged) {
        const dsd_tg_policy_call_route cur_route = call_route(cur, 0);
        (void)dsd_tg_policy_note_active_call(state, &cur_route, &cur_d, now_m);
    }
    const dsd_tg_policy_call_route route = call_route(c, c->alloc.freq_hz != followed->freq_hz);
    return dsd_tg_policy_should_preempt(opts, state, &route, d, now_m);
}

static void
call_log_skip(const tetra_state_t* ts, const tetra_call_t* c, const char* why) {
    TETRA_LOG_VERBOSE(ts, 1, "[TETRA CALL] call=%u carrier=%u slots=0x%X not followed: %s\n", (unsigned)c->call_id,
                      (unsigned)c->alloc.carrier, (unsigned)c->alloc.timeslots, why);
}

/* Trunking: hand the state machine the channel just allocated to @p c, when the policy lets it be followed. The
 * followed call's own allocation moves it. */
static void
call_consider(dsd_opts* opts, dsd_state* state, const tetra_state_t* ts, const tetra_call_t* c) {
    if (!opts || opts->trunk_enable != 1 || !alloc_followable(&c->alloc)) {
        return;
    }
    const tetra_sm_grant grant = {c->alloc.freq_hz, c->alloc.carrier, c->alloc.timeslots, c->call_id};
    tetra_sm_grant followed;
    const int tuned = tetra_sm_followed(state, &followed);
    if (tuned && followed.call_id == c->call_id) {
        (void)tetra_sm_on_grant(opts, state, &grant);
        return;
    }
    if ((c->flags & TETRA_CALL_F_SETUP_SEEN) == 0u || c->called_ssi == 0u) {
        call_log_skip(ts, c, "no D-SETUP named its called party");
        return;
    }
    call_note_lockout(opts, state, c);
    dsd_tg_policy_decision d;
    if (call_decision(opts, state, c, &d) != 0 || !d.tune_allowed) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CALL] call=%u target=%u blocked by %s\n", (unsigned)c->call_id,
                          (unsigned)c->called_ssi, dsd_tg_policy_block_reason_label(d.block_reasons));
        return;
    }
    if (tuned && !call_takes_over(opts, state, c, &d, &followed)) {
        call_log_skip(ts, c, "another call is followed");
        return;
    }
    if (tetra_sm_on_grant(opts, state, &grant) == 1) {
        const dsd_tg_policy_call_route route = call_route(c, 0);
        (void)dsd_tg_policy_note_active_call(state, &route, &d, dsd_decode_now_mono_s());
    }
}

/* @p alloc is @p c's traffic channel now. A "go to the MCCH" allocation (timeslot assigned 0000, table 21.87) ends the
 * followed call; any other may be followed when @p may_follow. */
static void
call_attach(dsd_opts* opts, dsd_state* state, const tetra_state_t* ts, tetra_call_t* c, const tetra_chan_alloc* alloc,
            int may_follow) {
    const uint16_t id = c->call_id;
    if (alloc->timeslots == 0u) {
        DSD_MEMSET(&c->alloc, 0, sizeof c->alloc);
        c->alloc_m = 0.0;
        if (followed_id(state) == (int)id && opts) {
            tetra_sm_on_release(opts, state, id);
        }
        return;
    }
    c->alloc = *alloc;
    c->alloc_m = dsd_decode_now_mono_s();
    c->air_enc = TETRA_ENC_MODE_NONE;
    if (may_follow) {
        call_consider(opts, state, ts, c);
    }
}

/* ---- CMCE PDUs ------------------------------------------------------------------------------------------------- */

/* A D-SETUP for a known call identifier that names another called party or another kind of call is a new call that
 * reuses the identifier of one whose release went unheard. */
static int
setup_reuses_id(const tetra_call_t* c, const tetra_call_event* ev, int has_called, uint32_t called) {
    if ((c->flags & TETRA_CALL_F_SETUP_SEEN) == 0u) {
        return 0;
    }
    const uint8_t kind_bits = 0xECu; /* circuit mode type and communication type */
    return (has_called && c->called_ssi != 0u && c->called_ssi != called)
           || ((c->basic_service ^ ev->basic_service) & kind_bits) != 0u;
}

static tetra_call_t*
call_setup(const dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev,
           const tetra_mac_sdu* sdu) {
    uint32_t called = 0;
    const int has_called = sdu_ssi(sdu, &called);
    if (c && setup_reuses_id(c, ev, has_called, called) && followed_id(state) != (int)c->call_id) {
        calls_free(c);
        c = NULL;
    }
    if (!c) {
        c = calls_new(state, ts, ev->call_id);
        if (!c) {
            return NULL;
        }
    }
    c->flags = (uint8_t)(c->flags | TETRA_CALL_F_SETUP_SEEN);
    c->basic_service = ev->basic_service;
    c->e2ee = (uint8_t)basic_e2ee(ev->basic_service);
    c->priority = ev->priority;
    if (has_called) {
        c->called_ssi = called;
    }
    if ((ev->has & TETRA_CALL_HAS_CALLING) != 0u) {
        c->calling_ssi = ev->calling_ssi;
    }
    /* A late-entry D-SETUP repeats the calling party; it names the talker only while none is known. */
    if (c->talker_ssi == 0u && (ev->has & TETRA_CALL_HAS_GRANT) != 0u && grant_is_transmission(ev->grant)) {
        c->talker_ssi = c->calling_ssi;
    }
    return c;
}

/* D-INFO and D-CALL RESTORE may give the call a new identifier; a record already holding it is stale. */
static void
call_rename(tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev) {
    if ((ev->has & TETRA_CALL_HAS_NEW_ID) == 0u) {
        return;
    }
    const uint16_t id = (uint16_t)(ev->new_call_id & 0x3FFFu);
    tetra_call_t* const other = calls_find(ts, id);
    if (other && other != c) {
        calls_free(other);
    }
    c->call_id = id;
}

static void
call_connected(tetra_call_t* c) {
    if (c->phase < TETRA_CALL_CONNECTED) {
        c->phase = TETRA_CALL_CONNECTED;
    }
}

/* D-TX GRANTED and D-TX INTERRUPT: a transmission granted (to the addressed party, or to the transmitting party the PDU
 * names) starts; their encryption control says whether it is end-to-end encrypted (table 14.18). */
static void
call_transmission(tetra_call_t* c, const tetra_call_event* ev, const tetra_mac_sdu* sdu) {
    if ((ev->has & TETRA_CALL_HAS_ENCRYPTION) != 0u) {
        c->e2ee = (uint8_t)(ev->encryption != 0u);
    }
    if ((ev->has & TETRA_CALL_HAS_GRANT) == 0u || !grant_is_transmission(ev->grant)) {
        return;
    }
    c->phase = TETRA_CALL_TX_GRANTED;
    uint32_t ssi = 0;
    if ((ev->has & TETRA_CALL_HAS_TALKER) != 0u) {
        c->talker_ssi = ev->talker_ssi;
    } else if (ev->grant == 0u && sdu_ssi(sdu, &ssi)) {
        c->talker_ssi = ssi;
    } else {
        c->talker_ssi = 0u;
    }
}

/* D-CONNECT goes to the calling party (clause 14.7.1.4): its address is the caller, who talks when it is granted. */
static void
call_connect(tetra_call_t* c, const tetra_call_event* ev, const tetra_mac_sdu* sdu) {
    uint32_t ssi = 0;
    call_connected(c);
    if (!sdu_ssi(sdu, &ssi)) {
        return;
    }
    if (c->calling_ssi == 0u) {
        c->calling_ssi = ssi;
    }
    if (c->talker_ssi == 0u && (ev->has & TETRA_CALL_HAS_GRANT) != 0u && ev->grant == 0u) {
        c->talker_ssi = ssi;
    }
}

static void
call_apply(tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev, const tetra_mac_sdu* sdu) {
    if ((ev->has & TETRA_CALL_HAS_BASIC) != 0u && ev->kind != TETRA_CALL_EV_SETUP) {
        c->basic_service = ev->basic_service;
        c->e2ee = (uint8_t)basic_e2ee(ev->basic_service);
    }
    if ((ev->has & TETRA_CALL_HAS_TIMEOUT) != 0u) {
        c->timeout = ev->timeout;
    }
    switch ((tetra_call_event_kind)ev->kind) {
        case TETRA_CALL_EV_CONNECT: call_connect(c, ev, sdu); break;
        case TETRA_CALL_EV_CONNECT_ACK: call_connected(c); break;
        case TETRA_CALL_EV_INFO: call_rename(ts, c, ev); break;
        case TETRA_CALL_EV_RESTORE:
            call_rename(ts, c, ev);
            call_connected(c);
            break;
        case TETRA_CALL_EV_TX_GRANTED:
        case TETRA_CALL_EV_TX_INTERRUPT: call_transmission(c, ev, sdu); break;
        case TETRA_CALL_EV_TX_CEASED:
            c->phase = TETRA_CALL_CEASED;
            c->talker_ssi = 0u;
            break;
        default: break;
    }
}

/* Event labels (address types 101 and 111) and usage markers (110) the PDU assigned to a party of @p c. An event
 * label stands for one party on the channel, so another call loses it. */
static void
call_bind_address(tetra_state_t* ts, tetra_call_t* c, const tetra_mac_address* addr) {
    if (addr->esi) {
        return;
    }
    if (addr->type == TETRA_MAC_ADDR_SSI_EVENT || addr->type == TETRA_MAC_ADDR_SMI_EVENT) {
        for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
            tetra_call_t* const o = &ts->calls.call[i];
            if (o != c && (o->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && o->event_label == addr->event_label) {
                o->flags = (uint8_t)(o->flags & ~TETRA_CALL_F_EVENT_LABEL);
            }
        }
        c->event_label = addr->event_label;
        c->flags = (uint8_t)(c->flags | TETRA_CALL_F_EVENT_LABEL);
    } else if (addr->type == TETRA_MAC_ADDR_SSI_USAGE) {
        c->usage_marker = addr->usage_marker;
        c->flags = (uint8_t)(c->flags | TETRA_CALL_F_USAGE_MARKER);
    }
}

static const char* const k_event_names[] = {
    "D-SETUP",        "D-ALERT",     "D-CALL-PROCEEDING", "D-CONNECT", "D-CONNECT-ACK",  "D-INFO",    "D-TX-GRANTED",
    "D-TX-INTERRUPT", "D-TX-CEASED", "D-TX-CONTINUE",     "D-TX-WAIT", "D-CALL-RESTORE", "D-RELEASE",
};
_Static_assert(sizeof k_event_names / sizeof k_event_names[0] == (size_t)TETRA_CALL_EV_RELEASE + 1U,
               "one name per call event");

/* The followed call took a new identifier (D-INFO, D-CALL RESTORE): the state machine follows it under that one, on
 * the channel it is on, so its release still ends it. */
static void
call_follow_renamed(dsd_opts* opts, dsd_state* state, uint16_t old_id, uint16_t new_id) {
    tetra_sm_grant g;
    if (old_id == new_id || !opts || !tetra_sm_followed(state, &g) || g.call_id != old_id) {
        return;
    }
    g.call_id = new_id;
    (void)tetra_sm_on_grant(opts, state, &g);
}

/* The followed call turned out encrypted beyond the listener's keys (D-TX GRANTED encryption control) while encrypted
 * calls are not followed: it is locked out and left, as a grant for it would have been refused. */
static void
call_check_followed_crypto(dsd_opts* opts, dsd_state* state, const tetra_call_t* c) {
    if (!opts || opts->trunk_enable != 1 || opts->trunk_tune_enc_calls != 0 || basic_is_data(c->basic_service)
        || !call_undecryptable(state, c) || c->called_ssi == 0u) {
        return;
    }
    call_note_lockout(opts, state, c);
    tetra_sm_on_release(opts, state, c->call_id);
}

/* A release ends the record it names: the followed call's goes back to the control channel. */
static void
call_release(dsd_opts* opts, dsd_state* state, tetra_call_t* c) {
    const uint16_t id = c->call_id;
    const int followed = followed_id(state) == (int)id;
    calls_free(c);
    if (followed && opts) {
        tetra_sm_on_release(opts, state, id);
    }
}

void
tetra_calls_on_event(dsd_opts* opts, dsd_state* state, const tetra_call_event* ev, tetra_mac_sdu* sdu) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !ev || ev->kind > TETRA_CALL_EV_RELEASE) {
        return;
    }
    tetra_call_t* c = calls_find(ts, (uint16_t)(ev->call_id & 0x3FFFu));
    if (ev->kind == TETRA_CALL_EV_SETUP) {
        c = call_setup(state, ts, c, ev, sdu);
    }
    /* An allocation that came with a PDU of a call belongs to that call alone, tracked or not. */
    const int alloc = sdu && sdu->has_alloc && !sdu->alloc_taken;
    if (alloc) {
        sdu->alloc_taken = 1;
    }
    if (!c) {
        TETRA_LOG_VERBOSE(ts, 2, "[TETRA CALL] %s for call %u, which no record holds; ignored\n",
                          k_event_names[ev->kind], (unsigned)ev->call_id);
        return;
    }
    if (ev->kind == TETRA_CALL_EV_RELEASE) {
        call_release(opts, state, c);
        return;
    }
    const uint16_t old_id = c->call_id;
    call_apply(ts, c, ev, sdu);
    call_follow_renamed(opts, state, old_id, c->call_id);
    c->seen_m = dsd_decode_now_mono_s();
    if (sdu && sdu->has_addr) {
        call_bind_address(ts, c, &sdu->addr);
    }
    if (followed_id(state) == (int)c->call_id) {
        tetra_confirm_note_followed_call(state);
        call_check_followed_crypto(opts, state, c);
    }
    if (alloc) {
        call_attach(opts, state, ts, c, &sdu->alloc, 1);
    }
}

/* ---- MAC addresses and allocations ----------------------------------------------------------------------------- */

void
tetra_calls_on_allocation(dsd_opts* opts, dsd_state* state, tetra_mac_sdu* sdu) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !sdu || !sdu->has_alloc || sdu->alloc_taken) {
        return;
    }
    sdu->alloc_taken = 1;
    tetra_call_t* const c = sdu->has_addr ? calls_by_address(ts, &sdu->addr) : NULL;
    if (!c) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CALL] allocation carrier=%u slots=0x%X names no tracked call; not followed\n",
                          (unsigned)sdu->alloc.carrier, (unsigned)sdu->alloc.timeslots);
        return;
    }
    call_attach(opts, state, ts, c, &sdu->alloc, 0);
}

void
tetra_calls_on_addressed(dsd_state* state, const tetra_mac_address* addr, uint8_t enc_mode, int alloc_present,
                         uint8_t tn) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !addr) {
        return;
    }
    ts->calls.slot_enc[tn <= 4u ? tn : 0u] = (uint8_t)(1u + (enc_mode & 3u));
    tetra_call_t* const c = calls_by_address(ts, addr);
    if (!c) {
        return;
    }
    call_bind_address(ts, c, addr);
    if (enc_mode != TETRA_ENC_MODE_NONE && alloc_present) {
        /* The allocation is ciphertext (EN 300 392-7 clause 6.7.1.2): the channel stays as known, the call's traffic is
         * now air encrypted. */
        c->air_enc = (uint8_t)(enc_mode & 3u);
    }
    if (followed_id(state) == (int)c->call_id) {
        tetra_confirm_note_followed_call(state);
    }
}

/* ---- Housekeeping ---------------------------------------------------------------------------------------------- */

void
tetra_calls_unfollowed(dsd_state* state) {
    if (tetra_st_existing(state)) {
        (void)dsd_tg_policy_clear_active_call(state, -1);
    }
}

void
tetra_calls_tick(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    (void)opts;
    if (!ts) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    const int followed = followed_id(state);
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && (int)c->call_id != followed && now_m - c->seen_m >= TETRA_CALL_IDLE_S) {
            TETRA_LOG_VERBOSE(ts, 2, "[TETRA CALL] call=%u not heard from for %.0f s; dropped\n", (unsigned)c->call_id,
                              TETRA_CALL_IDLE_S);
            calls_free(c);
        }
    }
}

void
tetra_calls_forget_carrier(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        DSD_MEMSET(ts->calls.slot_enc, 0, sizeof ts->calls.slot_enc);
    }
}

int
tetra_calls_traffic(const dsd_state* state, tetra_chan_alloc* out) {
    tetra_sm_grant g;
    if (tetra_sm_followed(state, &g)) {
        DSD_MEMSET(out, 0, sizeof(*out));
        out->freq_hz = g.freq_hz;
        out->carrier = g.carrier;
        out->timeslots = g.timeslots;
        return out->timeslots != 0u;
    }
    const tetra_state_t* const ts = tetra_st_peek(state);
    const tetra_call_t* best = NULL;
    for (size_t i = 0; ts && i < TETRA_CALLS_MAX; i++) {
        const tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && c->alloc.timeslots != 0u && (!best || c->alloc_m > best->alloc_m)) {
            best = c;
        }
    }
    if (!best) {
        return 0;
    }
    *out = best->alloc;
    return 1;
}
