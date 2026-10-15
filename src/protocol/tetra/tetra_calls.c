// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA calls (tetra_calls.h): the trunked mode call table, the allocations attached to it and the decision to follow.
 */

#include "tetra_calls.h"

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_confirm.h"
#include "tetra_log.h"
#include "tetra_state.h"
#include "tetra_voice_state.h"

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

/* An address that assigns an event label (types 101, 111): it names its party by the SSI or SMI, and gives it the label
 * (EN 300 392-2 clause 23.4.1.2.3.1). */
static int
address_assigns_event_label(const tetra_mac_address* addr) {
    return addr->type == TETRA_MAC_ADDR_SSI_EVENT || addr->type == TETRA_MAC_ADDR_SMI_EVENT;
}

/* Event label values not valid for normal use (clause 23.4.1.2.3.0): all zeros withdraws an assignment, all ones says
 * the current labels stay valid after a channel change. */
#define TETRA_EVENT_LABEL_WITHDRAW 0x000u
#define TETRA_EVENT_LABEL_KEEP     0x3FFu

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

static const tetra_call_t*
calls_find_const(const tetra_state_t* ts, uint16_t call_id) {
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        const tetra_call_t* const c = &ts->calls.call[i];
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

/* Not trunking, the heard speech's call is the call @p call_id names no more: its record went. Speech goes on being
 * heard on the timeslot, and belongs to the record that holds the timeslot now. */
static void
calls_forget_heard_call(tetra_state_t* ts, uint16_t call_id) {
    if (ts->calls.heard_known && ts->calls.heard_call_id == call_id) {
        ts->calls.heard_known = 0;
        ts->calls.heard_call_id = 0U;
    }
}

static void calls_end_published(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, dsd_call_end_reason reason);

/* @p c leaves the table, however it goes (a release, its identifier given to another call, ageing, eviction). Its
 * canonical call ends, as nothing names it from here, and so does its identifier: the state machine stops taking it
 * for the followed call's, in what a tune in flight would put back too (tetra_sm_retire_call()), and the heard speech
 * for its. A later call given the identifier is judged as a call of its own, never through the followed call's grant
 * path, and is heard only from its own speech. */
static void
calls_drop(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c) {
    if (c->phase != TETRA_CALL_FREE) {
        if (ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == c->call_id) {
            calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
        }
        tetra_sm_retire_call(state, c->call_id);
        calls_forget_heard_call(ts, c->call_id);
    }
    calls_free(c);
}

/* The call identifier of the followed call, or -1 when the state machine follows none, or follows a channel whose call
 * identifier names another call now (tetra_sm_retire_call()). */
static int
followed_id(const dsd_state* state) {
    uint16_t id = 0;
    return tetra_sm_followed_call_id(state, &id) ? (int)id : -1;
}

/* A new record for @p call_id: a free entry, else the one heard from least recently that is not followed. */
static tetra_call_t*
calls_new(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, uint16_t call_id) {
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
    calls_drop(opts, state, ts, victim);
    victim->call_id = call_id;
    victim->phase = TETRA_CALL_SETUP;
    victim->calling_net = TETRA_CALL_NET_UNKNOWN;
    return victim;
}

/* The timeslot the TDMA clock places the PDU being decoded in, 0 while it is stopped. */
static uint8_t
calls_pdu_timeslot(const tetra_state_t* ts) {
    return (ts->tdma_valid && ts->tn >= 1u && ts->tn <= 4u) ? ts->tn : 0u;
}

/* @p ssi (0 for none) is a party of @p c: its called, calling or talking party. */
static int
call_has_party(const tetra_call_t* c, uint32_t ssi) {
    return ssi != 0u && (c->called_ssi == ssi || c->calling_ssi == ssi || c->talker_ssi == ssi);
}

/* ---- Event labels ---------------------------------------------------------------------------------------------- */

/* An event label (EN 300 392-2 clause 23.4.1.2.3) stands for one party on one MAC channel of one cell, in place of its
 * address; a record holds the label of a party of its call with the channel it is valid on (tetra_label_channel). All
 * that happens to it happens here, once its PDU is delivered (calls_label_delivered()), and where its cell or record
 * ends (tetra_calls_forget_cell(), calls_free()): an assignment binds it on the channel it applies on and takes the
 * number from the other parties of that channel only (label_bind()), the all-ones label moves the party's label to the
 * channel its PDU allocates, label 0 withdraws it, and a channel the decoder cannot tell binds none and takes the number
 * from every party. A reallocation without the all-ones label leaves the label on the channel left, the only one it is
 * valid on. Its expiry (T.201) is not modelled: the SwMI uses an expired number again only by assigning it, which takes
 * it, and the label lives no longer than its record, which ages (tetra_calls_tick()). */

/* The channel the PDU being decoded came on: the carrier the receiver is on and the timeslot the TDMA clock places the
 * PDU in. */
static tetra_label_channel
label_channel_here(const dsd_state* state, const tetra_state_t* ts) {
    tetra_label_channel ch;
    DSD_MEMSET(&ch, 0, sizeof ch);
    ch.epoch = tetra_confirm_carrier_epoch(state);
    ch.slots = tetra_slot_bit(calls_pdu_timeslot(ts));
    return ch;
}

/* The channel the label @p sdu's address assigns applies on (clause 23.4.1.2.3.1): the one the PDU allocates, else the
 * one it came on. 0 when the allocation names none the decoder can tell: "go to the control channel" (timeslots 0000,
 * table 21.87), which may be the MCCH or a common SCCH, or a ciphertext allocation. */
static int
label_channel_of(const dsd_state* state, const tetra_state_t* ts, const tetra_mac_sdu* sdu, tetra_label_channel* ch) {
    if (!sdu->has_alloc) {
        *ch = label_channel_here(state, ts);
        return 1;
    }
    if (sdu->alloc_unread || sdu->alloc.timeslots == 0u) {
        return 0;
    }
    DSD_MEMSET(ch, 0, sizeof *ch);
    ch->allocated = 1;
    ch->carrier = sdu->alloc.carrier;
    ch->slots = sdu->alloc.timeslots;
    return 1;
}

static void
label_drop(tetra_call_t* c) {
    c->flags = (uint8_t)(c->flags & ~TETRA_CALL_F_EVENT_LABEL);
    c->event_label = 0U;
    DSD_MEMSET(&c->label_chan, 0, sizeof c->label_chan);
}

/* Channels @p a and @p b are one: the same carrier, and timeslots that overlap (one not known on either side is any).
 * A carrier the receiver was on is one no more once it left it; while it is on it, it is told from a carrier number by
 * the carrier being received (tetra_calls_carrier()), and @p unknown answers when that number is not known. */
static int
label_channels_meet(const dsd_opts* opts, const dsd_state* state, const tetra_label_channel* a,
                    const tetra_label_channel* b, int unknown) {
    if (a->slots != 0u && b->slots != 0u && (a->slots & b->slots) == 0u) {
        return 0;
    }
    if (a->allocated && b->allocated) {
        return a->carrier == b->carrier;
    }
    if (!a->allocated && !b->allocated) {
        return a->epoch == b->epoch;
    }
    const tetra_label_channel* const received = a->allocated ? b : a;
    const tetra_label_channel* const numbered = a->allocated ? a : b;
    if (received->epoch != tetra_confirm_carrier_epoch(state)) {
        return 0;
    }
    uint16_t carrier = 0;
    if (!tetra_calls_carrier(opts, state, &carrier)) {
        return unknown;
    }
    return carrier == numbered->carrier;
}

/* @p c's event label is valid on the channel the PDU being decoded came on; a carrier whose number is not known is no
 * allocated channel's. */
static int
calls_label_here(const dsd_opts* opts, const dsd_state* state, const tetra_state_t* ts, const tetra_call_t* c) {
    const tetra_label_channel here = label_channel_here(state, ts);
    return label_channels_meet(opts, state, &c->label_chan, &here, 0);
}

/* Label @p value goes to @p holder (NULL: a party no record holds) on channel @p ch (NULL: one the decoder cannot tell,
 * which binds none). A label stands for one party on its channel, so no record of another party than @p party keeps
 * that number on a channel that may be the same one (all of them, for @p ch NULL); on another channel it is another
 * label and stays. */
static void
label_bind(const dsd_opts* opts, const dsd_state* state, tetra_state_t* ts, tetra_call_t* holder, uint32_t party,
           uint16_t value, const tetra_label_channel* ch) {
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const o = &ts->calls.call[i];
        if (o != holder && (o->flags & TETRA_CALL_F_EVENT_LABEL) != 0u && o->event_label == value
            && !call_has_party(o, party) && (!ch || label_channels_meet(opts, state, &o->label_chan, ch, 1))) {
            label_drop(o);
        }
    }
    if (!holder) {
        return;
    }
    if (!ch) {
        label_drop(holder);
        return;
    }
    holder->event_label = value;
    holder->label_chan = *ch;
    holder->flags = (uint8_t)(holder->flags | TETRA_CALL_F_EVENT_LABEL);
}

/* The PDU of @p sdu was delivered (a fragmented TM-SDU at its MAC-END, clause 23.4.1.2.3.1 b; an encrypted PDU as far
 * as its clear address goes): an SSI or SMI + event label address (types 101, 111) names its party, whose call
 * @p holder is (NULL when no record holds it, or when the SSI is an encrypted short identity, which names no one,
 * clause 4.2.6 of EN 300 392-7). Label 0 withdraws the party's label. The all-ones label keeps the party's current
 * label valid after the channel change the PDU makes (clause 23.4.1.2.3.0): with an allocation the label moves to the
 * allocated channel, as an assignment there would bind it; without one no channel changes. Any other value is assigned
 * to the party on the channel it applies on. */
static void
calls_label_delivered(const dsd_opts* opts, const dsd_state* state, tetra_state_t* ts, tetra_call_t* holder,
                      const tetra_mac_sdu* sdu) {
    const tetra_mac_address* const addr = &sdu->addr;
    if (!sdu->has_addr || !address_assigns_event_label(addr)) {
        return;
    }
    if (addr->event_label == TETRA_EVENT_LABEL_WITHDRAW) {
        if (holder) {
            label_drop(holder);
        }
        return;
    }
    const int keep = addr->event_label == TETRA_EVENT_LABEL_KEEP;
    if (keep && (!sdu->has_alloc || !holder || (holder->flags & TETRA_CALL_F_EVENT_LABEL) == 0u)) {
        return;
    }
    uint32_t party = 0;
    if (!address_ssi(addr, &party)) {
        party = 0u;
    }
    tetra_label_channel ch;
    const int known = label_channel_of(state, ts, sdu, &ch);
    label_bind(opts, state, ts, holder, party, keep ? holder->event_label : addr->event_label, known ? &ch : NULL);
}

/* The call a party of which @p addr names: by its called, calling or talking SSI, or, an address that is an event
 * label (type 010), by the label assigned to one of them on this channel (calls_label_here()); an address that assigns
 * a label names its party by the SSI alone. The most recently heard of several. */
static tetra_call_t*
calls_by_address(const dsd_opts* opts, const dsd_state* state, tetra_state_t* ts, const tetra_mac_address* addr) {
    uint32_t ssi = 0;
    if (!address_ssi(addr, &ssi)) {
        ssi = 0u;
    }
    const int by_label = addr->type == TETRA_MAC_ADDR_EVENT_LABEL && addr->event_label != TETRA_EVENT_LABEL_WITHDRAW
                         && addr->event_label != TETRA_EVENT_LABEL_KEEP;
    tetra_call_t* best = NULL;
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase == TETRA_CALL_FREE) {
            continue;
        }
        const int label_match = by_label && (c->flags & TETRA_CALL_F_EVENT_LABEL) != 0u
                                && c->event_label == addr->event_label && calls_label_here(opts, state, ts, c);
        if ((call_has_party(c, ssi) || label_match) && (!best || c->seen_m > best->seen_m)) {
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
    return ts ? calls_find_const(ts, (uint16_t)(call_id & 0x3FFFu)) : NULL;
}

const tetra_call_t*
tetra_calls_followed(const dsd_state* state) {
    const int id = followed_id(state);
    return id >= 0 ? tetra_calls_peek(state, (uint16_t)id) : NULL;
}

/* The carrier whose downlink frequency under the SYSINFO band and offset is exactly @p freq_hz. */
static int
carrier_of_frequency(const tetra_state_t* ts, long freq_hz, uint16_t* carrier) {
    const long first = tetra_carrier_to_dl_hz(0u, ts->freq_band, ts->freq_offset);
    if (first <= 0L || freq_hz < first || (freq_hz - first) % 25000L != 0L || (freq_hz - first) / 25000L > 4095L) {
        return 0;
    }
    *carrier = (uint16_t)((freq_hz - first) / 25000L);
    return 1;
}

int
tetra_calls_carrier(const dsd_opts* opts, const dsd_state* state, uint16_t* carrier) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || !carrier || ts->dmo || ts->sm.pending_request_id != 0U) {
        return 0;
    }
    tetra_sm_grant g;
    if (tetra_sm_followed(state, &g)) {
        *carrier = g.carrier;
        return 1;
    }
    return carrier_of_frequency(ts, tetra_sm_receiver_freq(opts, state), carrier);
}

/* Air interface encryption a key stream here undoes: an encrypted mode, not the reserved one, under a usable key. */
static int
air_decryptable(const dsd_state* state, uint8_t air_enc) {
    return tetra_enc_mode_encrypted(air_enc) && tetra_air_key_usable(state);
}

/* Speech of @p c the listener cannot decrypt: end-to-end encrypted, or air encrypted without a key stream for it. */
static int
call_undecryptable(const dsd_state* state, const tetra_call_t* c) {
    return c->e2ee || (c->air_enc != TETRA_ENC_MODE_NONE && !air_decryptable(state, c->air_enc));
}

static uint32_t
call_source(const tetra_call_t* c) {
    return c->talker_ssi != 0u ? c->talker_ssi : c->calling_ssi;
}

/* An individual call, which has two parties (EN 300 392-2 clause 14.5.2), as its D-SETUP's basic service says. */
static int
call_individual(const tetra_call_t* c) {
    return (c->flags & TETRA_CALL_F_SETUP_SEEN) != 0u && !basic_is_group(c->basic_service);
}

uint32_t
tetra_calls_target(const tetra_call_t* c) {
    if (!c) {
        return 0u;
    }
    if (call_individual(c) && c->calling_ssi != 0u && call_source(c) == c->called_ssi) {
        return c->calling_ssi;
    }
    return c->called_ssi;
}

/* The source canonical slot 0 names for @p c: the transmitting party, and for an individual call the calling party
 * while none is named, so that with the target (tetra_calls_target()) it names both parties. */
static uint32_t
calls_source(const tetra_call_t* c) {
    return call_individual(c) ? call_source(c) : c->talker_ssi;
}

/* ---- Canonical calls ------------------------------------------------------------------------------------------- */

/* The TETRA synchronisation burst type the decoder is on: every canonical TETRA call carries one. */
static int
calls_protocol(const dsd_state* state) {
    if (DSD_SYNC_IS_TETRA(state->synctype)) {
        return state->synctype;
    }
    return DSD_SYNC_IS_TETRA(state->lastsynctype) ? state->lastsynctype : DSD_SYNC_TETRA_NDB_POS;
}

static dsd_call_kind
call_kind(const tetra_call_t* c) {
    if ((c->flags & TETRA_CALL_F_SETUP_SEEN) == 0u) {
        return DSD_CALL_KIND_VOICE;
    }
    if (basic_is_data(c->basic_service)) {
        return DSD_CALL_KIND_DATA;
    }
    return basic_is_group(c->basic_service) ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
}

/* End-to-end encryption (@p sealed) is never decrypted here; air interface encryption is when a key stream can be
 * made. */
static dsd_call_crypto_state
crypto_state(const dsd_state* state, int sealed, uint8_t air_enc) {
    if (sealed) {
        return DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
    }
    if (air_enc == TETRA_ENC_MODE_NONE) {
        return DSD_CALL_CRYPTO_CLEAR;
    }
    return air_decryptable(state, air_enc) ? DSD_CALL_CRYPTO_DECRYPTABLE : DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
}

/* End what canonical slot 0 carries for TETRA, when it is still an active TETRA call: an ended one keeps the reason
 * the engine or a terminator gave it. */
static void
calls_end_published(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, dsd_call_end_reason reason) {
    if (ts->calls.published == TETRA_PUB_NONE) {
        return;
    }
    ts->calls.published = TETRA_PUB_NONE;
    ts->calls.ends++;
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
        && DSD_SYNC_IS_TETRA(call.protocol) && dsd_call_state_end_ex(state, 0U, 0.0, reason) > 0 && opts) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

/* 1 while canonical slot 0 carries @p pub (call @p call_id) as an active TETRA call. */
static int
calls_carrying(const dsd_state* state, const tetra_state_t* ts, uint8_t pub, uint16_t call_id) {
    if (ts->calls.published != pub || ts->calls.published_call_id != call_id) {
        return 0;
    }
    dsd_call_snapshot call;
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
           && DSD_SYNC_IS_TETRA(call.protocol);
}

/* Classify the call canonical slot 0 carries as @p crypto; audio is permitted for clear and decryptable calls. */
static void
calls_classify(dsd_state* state, dsd_call_crypto_state crypto) {
    dsd_call_crypto_update update;
    DSD_MEMSET(&update, 0, sizeof update);
    update.classification = crypto;
    update.audio_permitted = (uint8_t)(crypto == DSD_CALL_CRYPTO_CLEAR || crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    (void)dsd_call_state_update_crypto(state, 0U, &update);
}

/* The call canonical slot 0 carries is of crypto class @p crypto now, which DSD_CALL_CRYPTO_UNKNOWN leaves as it was. */
static void
calls_reclassify(dsd_opts* opts, dsd_state* state, dsd_call_crypto_state crypto) {
    dsd_call_snapshot call;
    if (crypto == DSD_CALL_CRYPTO_UNKNOWN || (dsd_call_state_get(state, 0U, &call) > 0 && call.crypto == crypto)) {
        return;
    }
    calls_classify(state, crypto);
    if (opts) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

/* Observe @p o on canonical slot 0 as @p pub (call @p call_id) with crypto class @p crypto, which
 * DSD_CALL_CRYPTO_UNKNOWN leaves as it was. Another TETRA call there ends first. */
static void
calls_publish(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const dsd_call_observation* o,
              dsd_call_crypto_state crypto, uint8_t pub, uint16_t call_id) {
    if (ts->calls.published != pub || ts->calls.published_call_id != call_id) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    (void)dsd_call_state_observe(state, o, DSD_CALL_BOUNDARY_CONTINUE);
    if (crypto != DSD_CALL_CRYPTO_UNKNOWN) {
        calls_classify(state, crypto);
    }
    ts->calls.published = pub;
    ts->calls.published_call_id = call_id;
    if (opts) {
        dsd_event_sync_slot(opts, state, 0U);
    }
}

/* Observe @p c on canonical slot 0, heard on traffic channel @p chan, and its crypto class. */
static void
calls_publish_call(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_call_t* c,
                   const tetra_chan_alloc* chan) {
    dsd_call_observation o;
    DSD_MEMSET(&o, 0, sizeof o);
    o.protocol = calls_protocol(state);
    o.kind = call_kind(c);
    o.ota_target_id = tetra_calls_target(c);
    o.policy_target_id = o.ota_target_id;
    o.ota_source_id = calls_source(c);
    o.channel = chan->carrier;
    o.frequency_hz = chan->freq_hz;
    o.service_options = c->basic_service;
    o.priority = c->priority;
    o.has_service_metadata = (uint8_t)((c->flags & TETRA_CALL_F_SETUP_SEEN) != 0u);
    calls_publish(opts, state, ts, &o, crypto_state(state, c->e2ee, c->air_enc), TETRA_PUB_CALL, c->call_id);
}

/* The followed call, once its tune has settled and while a transmission is not known to have ceased, publishes on
 * its traffic channel. */
static void
calls_publish_followed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    tetra_sm_grant g;
    if (!tetra_sm_followed(state, &g) || ts->sm.pending_request_id != 0U) {
        return;
    }
    const tetra_call_t* const c = calls_find(ts, g.call_id);
    if (!c || c->phase == TETRA_CALL_CEASED) {
        return;
    }
    tetra_chan_alloc chan;
    DSD_MEMSET(&chan, 0, sizeof chan);
    chan.freq_hz = g.freq_hz;
    chan.carrier = g.carrier;
    chan.timeslots = g.timeslots;
    calls_publish_call(opts, state, ts, c, &chan);
}

/* Canonical slot 0 shows @p c as its record is: when @p c is the call slot 0 carries, a change of its parties, service,
 * traffic channel or protection is observed as it is made, not at the next PDU or speech that happens to publish the
 * call. The followed call is observed on its followed channel once its tune settled (calls_publish_followed()), any
 * other carried call on its record's channel. */
static void
calls_sync_published(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_call_t* c) {
    if (!c || !calls_carrying(state, ts, TETRA_PUB_CALL, c->call_id)) {
        return;
    }
    if (followed_id(state) == (int)c->call_id) {
        calls_publish_followed(opts, state, ts);
    } else {
        calls_publish_call(opts, state, ts, c, &c->alloc);
    }
}

/* An over-the-air end of call @p call_id (D-TX CEASED, D-RELEASE, D-DISCONNECT) ends its canonical call. */
static void
calls_end_call(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, uint16_t call_id) {
    if (ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == call_id) {
        calls_end_published(opts, state, ts, DSD_CALL_END_TERMINATOR);
    }
}

/* A data or control notice of TETRA traffic in the event history, @p text (may be NULL) its message. */
static void
calls_emit_notice(dsd_opts* opts, dsd_state* state, int control, uint32_t src, uint32_t dst, const char* notice,
                  const char* text) {
    if (!opts || !state || !notice) {
        return;
    }
    const dsd_call_observation o = dsd_call_observation_data(calls_protocol(state), 0U, src, dst);
    if (text && text[0] != '\0') {
        dsd_event_stage_text(state, 0U, text);
    }
    (void)dsd_event_emit_data_notice_classified(opts, state, 0U, &o,
                                                control ? DSD_EVENT_CATEGORY_CONTROL : DSD_EVENT_CATEGORY_DATA, notice);
}

/* ---- Following ------------------------------------------------------------------------------------------------- */

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

/* The followed call @p c is encrypted beyond the listener's keys while encrypted calls are not followed (its basic
 * service or encryption control, an encrypted allocation to its party, or its speech): it is locked out and left, as a
 * grant for it would have been refused (call_check_followed_crypto()), and no reallocation of it is followed. */
static int
call_followed_excluded(const dsd_opts* opts, const dsd_state* state, const tetra_call_t* c) {
    return opts && opts->trunk_enable == 1 && opts->trunk_tune_enc_calls == 0 && !basic_is_data(c->basic_service)
           && call_undecryptable(state, c) && c->called_ssi != 0u;
}

/* Whether @p c, which the policy allows, takes the receiver from the call it follows: a call of the held talkgroup
 * over one that is not, else a policy preemption (dsd_tg_policy_should_preempt()). */
static int
call_takes_over(const dsd_opts* opts, dsd_state* state, const tetra_call_t* c, const dsd_tg_policy_decision* d,
                const tetra_sm_grant* followed) {
    const tetra_call_t* const cur = tetra_calls_followed(state);
    const double now_m = dsd_decode_now_mono_s();
    dsd_tg_policy_decision cur_d = {0};
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
 * followed call's own allocation moves it. When the allocation took the followed channel (@p takes_followed,
 * calls_take_channel()), the call followed there holds no channel for @p c to take over, so the policy alone decides.
 * Nothing is handed over while a tune or return is in flight, answered during the burst or not (the burst's entry
 * settled what the backend had answered before it, tetra_burst_enter() in tetra.c; one this burst started is in
 * flight): the state machine takes no grant then, and the follow the grant was judged against is the one the whole
 * burst sees. */
static void
call_consider(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_call_t* c, int takes_followed) {
    if (!opts || opts->trunk_enable != 1 || !alloc_followable(&c->alloc)) {
        return;
    }
    if (ts->sm.pending_request_id != 0U) {
        call_log_skip(ts, c, "a tune is in flight");
        return;
    }
    const tetra_sm_grant grant = {c->alloc.freq_hz, c->alloc.carrier, c->alloc.timeslots, c->call_id};
    tetra_sm_grant followed;
    const int tuned = tetra_sm_followed(state, &followed);
    if (followed_id(state) == (int)c->call_id) {
        /* Its own reallocation moves it, unless it is excluded for its encryption: its PDU asked to leave it
         * (call_after_pdu()), and a return refused or deferred keeps the receiver where it is, never on a new channel. */
        if (!call_followed_excluded(opts, state, c)) {
            (void)tetra_sm_on_grant(opts, state, &grant);
        }
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
    if (tuned && !takes_followed && !call_takes_over(opts, state, c, &d, &followed)) {
        call_log_skip(ts, c, "another call is followed");
        return;
    }
    if (tetra_sm_on_grant(opts, state, &grant) == 1) {
        const dsd_tg_policy_call_route route = call_route(c, 0);
        (void)dsd_tg_policy_note_active_call(state, &route, &d, dsd_decode_now_mono_s());
        calls_publish_followed(opts, state, ts);
    }
}

/* ---- Traffic channels ------------------------------------------------------------------------------------------ */

/* The downlink of a traffic channel carries one call's traffic at a time: the AACH names it by its downlink traffic
 * usage marker, and an MS whose call's downlink carries another's leaves the channel (EN 300 392-2 clauses 23.5.5,
 * 23.5.6.1.1 c)). The uplink and the downlink of a timeslot may be allocated to two calls (clause 23.5.5), so only the
 * downlink, the direction the decoder receives, is held by one call here. */

/* Allocations @p a and @p b assign one downlink: the downlink in both (table 21.87, up/downlink assigned 01 or 11), a
 * timeslot in common and the same carrier, of the same frequency where both are known (an extended carrier number may
 * name another band). */
static int
alloc_downlinks_meet(const tetra_chan_alloc* a, const tetra_chan_alloc* b) {
    return (a->up_down & b->up_down & 1u) != 0u && (a->timeslots & b->timeslots) != 0u && a->carrier == b->carrier
           && (a->freq_hz == 0L || b->freq_hz == 0L || a->freq_hz == b->freq_hz);
}

/* The channel the state machine follows, as an allocation of its downlink. */
static int
followed_channel(const dsd_state* state, tetra_chan_alloc* out) {
    tetra_sm_grant g;
    if (!tetra_sm_followed(state, &g)) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->freq_hz = g.freq_hz;
    out->carrier = g.carrier;
    out->timeslots = g.timeslots;
    out->up_down = 1u;
    return 1;
}

/* @p o's traffic channel was allocated to another call, so @p o holds none until an allocation gives it one again. Its
 * canonical call ends, what the channel carries being the other call's from here, the heard speech is not its, and the
 * state machine follows its identifier no more, in what a tune in flight would put back too
 * (tetra_sm_retire_call()). */
static void
call_lose_channel(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* o) {
    DSD_MEMSET(&o->alloc, 0, sizeof o->alloc);
    o->alloc_m = 0.0;
    if (ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == o->call_id) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    calls_forget_heard_call(ts, o->call_id);
    tetra_sm_retire_call(state, o->call_id);
}

/* @p c's allocation is its traffic channel now: every other record whose allocation assigns the same downlink loses
 * its channel (call_lose_channel()). Returns 1 when the follow loses its channel too, @p c not being the followed call:
 * the followed call's record was one of them, or the followed channel is in the allocation (a record that holds no
 * channel there any more, or a follow without a call record, tetra_sm_retire_call()). */
static int
calls_take_channel(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c) {
    const int followed = followed_id(state);
    tetra_chan_alloc f;
    int taken = followed != (int)c->call_id && followed_channel(state, &f) && alloc_downlinks_meet(&f, &c->alloc);
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const o = &ts->calls.call[i];
        if (o != c && o->phase != TETRA_CALL_FREE && alloc_downlinks_meet(&o->alloc, &c->alloc)) {
            taken |= (int)o->call_id == followed;
            call_lose_channel(opts, state, ts, o);
        }
    }
    return taken;
}

/* The followed channel was allocated to @p c, which the state machine does not follow (the policy blocks it, or the
 * allocation came with no call PDU): the call followed there is over, as a "go to the MCCH" allocation of it says, so
 * the state machine lets go of it and returns to the control channel. Where it cannot (the return refused or deferred,
 * a tune in flight), it follows the channel without a call (tetra_sm_retire_call()): no speech there plays as the
 * followed call's or renews the hangtime, and the hangtime or the grant timeout gives up following. */
static void
calls_leave_taken_channel(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_call_t* c) {
    tetra_sm_grant g;
    if (!tetra_sm_followed(state, &g) || followed_id(state) == (int)c->call_id) {
        return;
    }
    if (g.call_id != c->call_id && ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == g.call_id) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    if (opts) {
        tetra_sm_on_release(opts, state, g.call_id);
    }
    tetra_sm_retire_call(state, g.call_id);
}

/* @p alloc is @p c's traffic channel now, and no other call's (calls_take_channel()). A "go to the MCCH" allocation
 * (timeslot assigned 0000, table 21.87), which names no channel the decoder can place, takes none and ends the followed
 * call; any other may be followed when @p may_follow, and when it took the followed channel and @p c is not followed,
 * the state machine lets go of the channel (calls_leave_taken_channel()). The follow it reads is the one the burst's
 * entry settled (tetra_burst_enter() in tetra.c), the same for every PDU of the burst. "Go to the MCCH" leaves the
 * call off its channel: the followed call is released, and the call is retired either way (tetra_sm_retire_call(),
 * as calls_leave_taken_channel() does): where the release cannot be made (a tune in flight for the rest of the burst,
 * even the call's own, or the return refused or deferred) the state machine follows the channel without the call,
 * and a tune in flight that fails puts the channel back without it; after a release that was made it changes
 * nothing, and a return in flight that fails comes back without the call too. */
static void
call_attach(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_chan_alloc* alloc,
            int may_follow) {
    const uint16_t id = c->call_id;
    if (alloc->timeslots == 0u) {
        DSD_MEMSET(&c->alloc, 0, sizeof c->alloc);
        c->alloc_m = 0.0;
        if (followed_id(state) == (int)id && opts) {
            tetra_sm_on_release(opts, state, id);
        }
        tetra_sm_retire_call(state, id);
        return;
    }
    c->alloc = *alloc;
    c->alloc_m = dsd_decode_now_mono_s();
    c->air_enc = TETRA_ENC_MODE_NONE;
    const int takes_followed = calls_take_channel(opts, state, ts, c);
    if (may_follow) {
        call_consider(opts, state, ts, c, takes_followed);
    }
    if (takes_followed) {
        calls_leave_taken_channel(opts, state, ts, c);
    }
}

/* ---- CMCE PDUs ------------------------------------------------------------------------------------------------- */

/* The network of a party the downlink names by its SSI: a calling party of type 2 (a TSI) names it in its extension
 * (@p ext, when @p has_ext), and any other SSI is a party of the current network (EN 300 392-2 clause 14.2.3, tables
 * 14.43, 14.44); TETRA_CALL_NET_UNKNOWN while the decoder knows none. */
static uint32_t
calls_party_net(const tetra_state_t* ts, int has_ext, uint32_t ext) {
    if (has_ext) {
        return ext & 0xFFFFFFu;
    }
    return ts->net_known ? (((uint32_t)ts->mcc << 14) | ts->mnc) : TETRA_CALL_NET_UNKNOWN;
}

/* The D-SETUP names another calling party than @p c's: another SSI, or the same SSI of another network
 * (@p calling_net, calls_party_net()). A calling party not known on either side is no change. */
static int
setup_names_another_caller(const tetra_call_t* c, const tetra_call_event* ev, uint32_t calling_net) {
    if ((ev->has & TETRA_CALL_HAS_CALLING) == 0u || ev->calling_ssi == 0u || c->calling_ssi == 0u) {
        return 0;
    }
    if (ev->calling_ssi != c->calling_ssi) {
        return 1;
    }
    return calling_net != TETRA_CALL_NET_UNKNOWN && c->calling_net != TETRA_CALL_NET_UNKNOWN
           && calling_net != c->calling_net;
}

/* A D-SETUP for a known call identifier that names another called party, another calling party or another kind of
 * call is a new call that reuses the identifier of one whose release went unheard: a call keeps its parties and its
 * kind, and a late-entry D-SETUP repeats them. */
static int
setup_reuses_id(const tetra_call_t* c, const tetra_call_event* ev, int has_called, uint32_t called,
                uint32_t calling_net) {
    if ((c->flags & TETRA_CALL_F_SETUP_SEEN) == 0u) {
        return 0;
    }
    const uint8_t kind_bits = 0xECu; /* circuit mode type and communication type */
    return (has_called && c->called_ssi != 0u && c->called_ssi != called)
           || setup_names_another_caller(c, ev, calling_net)
           || ((c->basic_service ^ ev->basic_service) & kind_bits) != 0u;
}

/* @p c ends: its canonical call ends with @p reason, its record goes, and when it is the followed call the state
 * machine lets go of it (tetra_sm_on_release()). Where it cannot -- the return to the control channel refused or
 * deferred, a tune in flight, or a return in flight that would put the call back if it failed -- the state machine
 * follows the channel but no call record from here (calls_drop()), so a call given the identifier later is never
 * taken for the followed one. A release, and a call whose release went unheard, end this way. */
static void
call_end(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, dsd_call_end_reason reason) {
    const uint16_t id = c->call_id;
    const int followed = followed_id(state) == (int)id;
    if (ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == id) {
        calls_end_published(opts, state, ts, reason);
    }
    /* The record goes first, so what the release publishes shows none of it; its identifier is retired after the
     * release, which may already have let go of it (calls_drop() in two steps). */
    calls_free(c);
    if (followed && opts) {
        tetra_sm_on_release(opts, state, id);
    }
    tetra_sm_retire_call(state, id);
    calls_forget_heard_call(ts, id);
}

/* The record a D-SETUP describes. */
static tetra_call_t*
call_setup(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev,
           const tetra_mac_sdu* sdu) {
    uint32_t called = 0;
    const int has_called = sdu_ssi(sdu, &called);
    const uint32_t calling_net = calls_party_net(ts, (ev->has & TETRA_CALL_HAS_CALLING_EXT) != 0u, ev->calling_ext);
    if (c && setup_reuses_id(c, ev, has_called, called, calling_net)) {
        /* The call whose identifier the D-SETUP reuses ended with its release unheard. */
        call_end(opts, state, ts, c, DSD_CALL_END_EXPLICIT);
        c = NULL;
    }
    if (!c) {
        c = calls_new(opts, state, ts, ev->call_id);
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
        /* The record's calling party, or one it did not know: a known network is kept until another is named. */
        if (c->calling_ssi != ev->calling_ssi || calling_net != TETRA_CALL_NET_UNKNOWN) {
            c->calling_net = calling_net;
        }
        c->calling_ssi = ev->calling_ssi;
    }
    /* A late-entry D-SETUP repeats the calling party; it names the talker only while none is known. */
    if (c->talker_ssi == 0u && (ev->has & TETRA_CALL_HAS_GRANT) != 0u && grant_is_transmission(ev->grant)) {
        c->talker_ssi = c->calling_ssi;
    }
    return c;
}

/* D-INFO and D-CALL RESTORE may give the call a new identifier. A record already holding it is a call whose release
 * went unheard: it ends as a D-SETUP reusing its identifier ends it, the followed call let go of with it. */
static void
call_rename(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev) {
    if ((ev->has & TETRA_CALL_HAS_NEW_ID) == 0u) {
        return;
    }
    const uint16_t id = (uint16_t)(ev->new_call_id & 0x3FFFu);
    tetra_call_t* const other = calls_find(ts, id);
    if (other && other != c) {
        call_end(opts, state, ts, other, DSD_CALL_END_EXPLICIT);
    }
    /* The canonical call, the heard speech and the speech in progress go on under the new identifier. */
    if (ts->calls.published == TETRA_PUB_CALL && ts->calls.published_call_id == c->call_id) {
        ts->calls.published_call_id = id;
    }
    if (ts->calls.heard_known && ts->calls.heard_call_id == c->call_id) {
        ts->calls.heard_call_id = id;
    }
    tetra_voice_call_renamed(&ts->voice, c->call_id, id);
    c->call_id = id;
}

static void
call_connected(tetra_call_t* c) {
    if (c->phase < TETRA_CALL_CONNECTED) {
        c->phase = TETRA_CALL_CONNECTED;
    }
}

/* The transmitting party a grant names: the one the PDU names, else the addressed party when it is the one granted;
 * 0 when it names none. */
static uint32_t
grant_talker(const tetra_call_event* ev, const tetra_mac_sdu* sdu) {
    uint32_t ssi = 0;
    if ((ev->has & TETRA_CALL_HAS_TALKER) != 0u) {
        return ev->talker_ssi;
    }
    return (ev->grant == 0u && sdu_ssi(sdu, &ssi)) ? ssi : 0u;
}

/* D-TX GRANTED and D-TX INTERRUPT: a transmission granted (to the addressed party, or to the transmitting party the PDU
 * names) starts; their encryption control says whether it is end-to-end encrypted (table 14.18). A grant is the SwMI's
 * floor decision, never sent unsolicited (EN 300 392-2 clause 14.5.1.2.1), so one granted after D-TX CEASED, or one
 * that names no talker where one was known, is the next transmission. After D-TX CEASED the speech of the last one (the
 * trail that follows it, whose talker is no longer known) is not its speech (tetra_voice_transmission_started()); the
 * canonical call of a talker no longer known ends, so the next transmission does not go on under that talker, and its
 * speech starts over with it. */
static void
call_transmission(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev,
                  const tetra_mac_sdu* sdu) {
    if ((ev->has & TETRA_CALL_HAS_ENCRYPTION) != 0u) {
        c->e2ee = (uint8_t)(ev->encryption != 0u);
    }
    if ((ev->has & TETRA_CALL_HAS_GRANT) == 0u || !grant_is_transmission(ev->grant)) {
        return;
    }
    const uint32_t before = c->talker_ssi;
    const int after_cease = c->phase == TETRA_CALL_CEASED;
    c->phase = TETRA_CALL_TX_GRANTED;
    c->talker_ssi = grant_talker(ev, sdu);
    if (after_cease) {
        tetra_voice_transmission_started(&ts->voice, c->call_id);
    }
    /* Ending the call starts its speech over too (tetra_calls_t::ends). */
    if (before != 0u && c->talker_ssi == 0u && ts->calls.published == TETRA_PUB_CALL
        && ts->calls.published_call_id == c->call_id) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
}

/* D-CONNECT goes to the calling party (clause 14.7.1.4): its address is the caller, a party of the current network, who
 * talks when it is granted. */
static void
call_connect(const tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev, const tetra_mac_sdu* sdu) {
    uint32_t ssi = 0;
    call_connected(c);
    if (!sdu_ssi(sdu, &ssi)) {
        return;
    }
    if (c->calling_ssi == 0u) {
        c->calling_ssi = ssi;
        c->calling_net = calls_party_net(ts, 0, 0u);
    }
    if (c->talker_ssi == 0u && (ev->has & TETRA_CALL_HAS_GRANT) != 0u && ev->grant == 0u) {
        c->talker_ssi = ssi;
    }
}

static void
call_apply(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_call_t* c, const tetra_call_event* ev,
           const tetra_mac_sdu* sdu) {
    if ((ev->has & TETRA_CALL_HAS_BASIC) != 0u && ev->kind != TETRA_CALL_EV_SETUP) {
        c->basic_service = ev->basic_service;
        c->e2ee = (uint8_t)basic_e2ee(ev->basic_service);
    }
    if ((ev->has & TETRA_CALL_HAS_TIMEOUT) != 0u) {
        c->timeout = ev->timeout;
    }
    switch ((tetra_call_event_kind)ev->kind) {
        case TETRA_CALL_EV_CONNECT: call_connect(ts, c, ev, sdu); break;
        case TETRA_CALL_EV_CONNECT_ACK: call_connected(c); break;
        case TETRA_CALL_EV_INFO: call_rename(opts, state, ts, c, ev); break;
        case TETRA_CALL_EV_RESTORE:
            call_rename(opts, state, ts, c, ev);
            call_connected(c);
            break;
        case TETRA_CALL_EV_TX_GRANTED:
        case TETRA_CALL_EV_TX_INTERRUPT: call_transmission(opts, state, ts, c, ev, sdu); break;
        case TETRA_CALL_EV_TX_CEASED:
            c->phase = TETRA_CALL_CEASED;
            c->talker_ssi = 0u;
            break;
        default: break;
    }
}

/* A usage marker (address type 110) the delivered PDU assigned to a party of @p c (EN 300 392-2 clause 23.5.5). */
static void
call_bind_usage_marker(tetra_call_t* c, const tetra_mac_address* addr) {
    if (addr->type == TETRA_MAC_ADDR_SSI_USAGE && !addr->esi) {
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
 * the channel it is on, so its release still ends it. The rename is no grant: the state machine takes the new identifier
 * as it is (tetra_sm_on_call_renamed()), not through the admission of a grant, which the first block decoded on a new
 * traffic carrier would fail, leaving the state machine on an identifier no record holds. */
static void
call_follow_renamed(dsd_state* state, uint16_t old_id, uint16_t new_id) {
    if (old_id != new_id) {
        tetra_sm_on_call_renamed(state, old_id, new_id);
    }
}

/* The followed call turned out encrypted beyond the listener's keys (call_followed_excluded()) while encrypted calls
 * are not followed: it is locked out and left, as a grant for it would have been refused. */
static void
call_check_followed_crypto(dsd_opts* opts, dsd_state* state, const tetra_call_t* c) {
    if (!call_followed_excluded(opts, state, c)) {
        return;
    }
    call_note_lockout(opts, state, c);
    tetra_sm_on_release(opts, state, c->call_id);
}

/* Not trunking, @p c is the call whose speech is being heard. */
static int
call_is_heard(const dsd_opts* opts, const tetra_state_t* ts, const tetra_call_t* c) {
    return (!opts || opts->trunk_enable != 1) && ts->calls.heard && ts->calls.heard_known
           && ts->calls.heard_call_id == c->call_id;
}

/* What a PDU of @p c means beyond its record. The followed call is on this carrier (tetra_confirm), may have turned
 * undecryptable, and its canonical call carries what the PDU said; so does the heard call's, until its transmission
 * ceased. */
static void
call_after_pdu(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_call_t* c) {
    if (followed_id(state) == (int)c->call_id) {
        tetra_confirm_note_followed_call(state);
        call_check_followed_crypto(opts, state, c);
        if (tetra_confirm_carrier_proven(state)) {
            calls_publish_followed(opts, state, ts);
        }
        return;
    }
    if (call_is_heard(opts, ts, c) && c->phase != TETRA_CALL_CEASED && tetra_confirm_carrier_proven(state)) {
        calls_publish_call(opts, state, ts, c, &c->alloc);
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
        c = call_setup(opts, state, ts, c, ev, sdu);
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
        /* A release ends the record it names and its canonical call; the followed call's goes back to the control
         * channel. */
        call_end(opts, state, ts, c, DSD_CALL_END_TERMINATOR);
        return;
    }
    const uint16_t old_id = c->call_id;
    call_apply(opts, state, ts, c, ev, sdu);
    call_follow_renamed(state, old_id, c->call_id);
    c->seen_m = dsd_decode_now_mono_s();
    if (ev->kind == TETRA_CALL_EV_TX_CEASED) {
        calls_end_call(opts, state, ts, c->call_id);
    }
    call_after_pdu(opts, state, ts, c);
    if (alloc) {
        call_attach(opts, state, ts, c, &sdu->alloc, 1);
    }
    /* What the PDU and its allocation changed reaches the canonical call carrying it now, on a carrier not proven yet
     * too: the call was published before, and the PDU passed its CRC. */
    calls_sync_published(opts, state, ts, c);
}

/* ---- MAC addresses and allocations ----------------------------------------------------------------------------- */

/* An allocation no call PDU of the TM-SDU took goes to the call of the party the PDU addressed, and never tunes. */
static void
calls_attach_addressed(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, tetra_mac_sdu* sdu) {
    if (!sdu->has_alloc || sdu->alloc_taken || sdu->alloc_unread) {
        return;
    }
    sdu->alloc_taken = 1;
    tetra_call_t* const c = sdu->has_addr ? calls_by_address(opts, state, ts, &sdu->addr) : NULL;
    if (!c) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CALL] allocation carrier=%u slots=0x%X names no tracked call; not followed\n",
                          (unsigned)sdu->alloc.carrier, (unsigned)sdu->alloc.timeslots);
        return;
    }
    call_attach(opts, state, ts, c, &sdu->alloc, 0);
    calls_sync_published(opts, state, ts, c);
}

void
tetra_calls_on_allocation(dsd_opts* opts, dsd_state* state, tetra_mac_sdu* sdu) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !sdu) {
        return;
    }
    calls_attach_addressed(opts, state, ts, sdu);
    tetra_call_t* const c = sdu->has_addr ? calls_by_address(opts, state, ts, &sdu->addr) : NULL;
    calls_label_delivered(opts, state, ts, c, sdu);
    if (c) {
        call_bind_usage_marker(c, &sdu->addr);
    }
}

void
tetra_calls_on_addressed(dsd_opts* opts, dsd_state* state, const tetra_mac_address* addr, uint8_t enc_mode,
                         int alloc_present, uint8_t tn) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !addr) {
        return;
    }
    const uint8_t evidence = (uint8_t)(1u + (enc_mode & 3u));
    ts->calls.slot_enc[0] = evidence;
    if (tn <= 4u) {
        ts->calls.slot_enc[tn] = evidence;
    }
    tetra_call_t* const c = calls_by_address(opts, state, ts, addr);
    if (!c) {
        return;
    }
    const int followed = followed_id(state) == (int)c->call_id;
    if (followed) {
        tetra_confirm_note_followed_call(state);
    }
    if (enc_mode == TETRA_ENC_MODE_NONE || !alloc_present || c->air_enc == (enc_mode & 3u)) {
        return;
    }
    /* The allocation is ciphertext (EN 300 392-7 clause 6.7.1.2): the channel stays as known, the call's traffic is now
     * air encrypted, which its canonical call shows from here, and which may leave a followed call undecryptable. */
    c->air_enc = (uint8_t)(enc_mode & 3u);
    if (followed) {
        call_check_followed_crypto(opts, state, c);
    }
    calls_sync_published(opts, state, ts, c);
}

/* ---- Direct mode (EN 300 396-3) ------------------------------------------------------------------------------- */

enum { DMO_PDU_OTHER = 0, DMO_PDU_CALL, DMO_PDU_END, DMO_PDU_SDS };

/* What a DM-MAC message does to the call on the channel (EN 300 396-3 clause 9.3.19, EN 300 396-5 clause 14.6.1). A
 * gateway message counts only once its subtype's elements were read. */
static int
dmo_pdu_kind(const tetra_dmo_sync_h_t* h) {
    switch (h->msg_type) {
        case TETRA_DMO_MSG_SETUP:
        case TETRA_DMO_MSG_SETUP_PRES:
        case TETRA_DMO_MSG_CONNECT_ACK:
        case TETRA_DMO_MSG_OCCUPIED: return DMO_PDU_CALL;
        case TETRA_DMO_MSG_TX_CEASED:
        case TETRA_DMO_MSG_RELEASE:
        case TETRA_DMO_MSG_DISCONNECT: return DMO_PDU_END;
        case TETRA_DMO_MSG_SDS_UDATA:
        case TETRA_DMO_MSG_SDS_DATA: return DMO_PDU_SDS;
        case TETRA_DMO_MSG_GATEWAY:
            if (!h->md_ok) {
                return DMO_PDU_OTHER;
            }
            if (h->e[TETRA_DMO_E_GW_SUBTYPE] == TETRA_DMO_GW_SETUP) {
                return DMO_PDU_CALL;
            }
            return h->e[TETRA_DMO_E_GW_SUBTYPE] == TETRA_DMO_GW_RELEASE ? DMO_PDU_END : DMO_PDU_OTHER;
        default: return DMO_PDU_OTHER;
    }
}

static dsd_call_kind
dmo_call_kind(const tetra_dmo_call_t* d) {
    if (d->circuit_known && d->circuit_mode >= 1u && d->circuit_mode <= 7u) {
        return DSD_CALL_KIND_DATA;
    }
    if (!d->group_known) {
        return DSD_CALL_KIND_VOICE;
    }
    return d->group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
}

/* The call's protection from the DMAC-SYNC that set it up (tetra_dmo_call_crypt()): there is no direct mode decryption
 * here, so anything encrypted is encrypted for the listener. */
static dsd_call_crypto_state
dmo_crypto_state(const dsd_state* state) {
    switch (tetra_dmo_call_crypt(state)) {
        case TETRA_DMO_CRYPT_CLEAR: return DSD_CALL_CRYPTO_CLEAR;
        case TETRA_DMO_CRYPT_AIR:
        case TETRA_DMO_CRYPT_E2E: return DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
        case TETRA_DMO_CRYPT_UNKNOWN:
        default: return DSD_CALL_CRYPTO_UNKNOWN;
    }
}

static void
dmo_publish(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    const tetra_dmo_call_t* const d = &ts->calls.dmo;
    dsd_call_observation o;
    DSD_MEMSET(&o, 0, sizeof o);
    o.protocol = calls_protocol(state);
    o.kind = dmo_call_kind(d);
    o.ota_target_id = d->dst;
    o.policy_target_id = d->dst;
    o.ota_source_id = d->src;
    calls_publish(opts, state, ts, &o, dmo_crypto_state(state), TETRA_PUB_DMO, 0U);
}

/* Forget who the call on the channel was; the short data notice memory stays. */
static void
dmo_call_forget(tetra_dmo_call_t* d) {
    d->src = 0u;
    d->dst = 0u;
    d->active = 0;
    d->in_tx = 0;
    d->addressed = 0;
    d->group = 0;
    d->group_known = 0;
    d->circuit_mode = 0;
    d->circuit_known = 0;
}

/* The call a DM-SETUP, DM-SETUP PRES, DM-CONNECT ACK, DM-OCCUPIED or DM-GSETUP describes: its source and destination
 * (the destination hidden by DM-2-B), its circuit mode type from the message dependent elements and its call type from
 * the DM-SDU, each when the security class left it clear. */
static void
dmo_call_from(tetra_dmo_call_t* d, const tetra_dmo_sync_h_t* h) {
    if (!d->active || d->src != h->src || (!h->dst_encrypted && d->dst != h->dst)) {
        dmo_call_forget(d);
    }
    d->active = 1;
    d->addressed = 1;
    d->src = h->src;
    d->dst = h->dst_encrypted ? 0u : h->dst;
    if (h->md_ok) {
        d->circuit_mode = (uint8_t)h->e[TETRA_DMO_E_CIRCUIT_MODE];
        d->circuit_known = 1;
    }
    if (h->md_ok && h->sdu_state == TETRA_DMO_SDU_CLEAR) {
        d->group = (uint8_t)(h->e[TETRA_DMO_E_CALL_TYPE] != 0u);
        d->group_known = 1;
    }
}

/* The message @p h carries, with its destination as published (@p dst). */
static tetra_dmo_sds_key
dmo_sds_key_of(const tetra_dmo_sync_h_t* h, uint32_t dst) {
    tetra_dmo_sds_key k;
    DSD_MEMSET(&k, 0, sizeof k);
    k.value = h->sds_value;
    k.digest = h->sdu_digest;
    k.src = h->src;
    k.dst = dst;
    k.length = (uint16_t)h->e[TETRA_DMO_E_SDS_LENGTH];
    k.sdu_bits = h->sdu_bits;
    k.type = h->msg_type;
    k.sdti = (uint8_t)h->e[TETRA_DMO_E_SDTI];
    return k;
}

static int
dmo_sds_key_equal(const tetra_dmo_sds_key* a, const tetra_dmo_sds_key* b) {
    return a->value == b->value && a->digest == b->digest && a->src == b->src && a->dst == b->dst
           && a->length == b->length && a->sdu_bits == b->sdu_bits && a->type == b->type && a->sdti == b->sdti;
}

/* Short data of the channel as a data notice (EN 300 396-3 clause 9.7); the DMAC-SYNC repeats it, so a repeat of the
 * last within 10 s is not noted again: the same message, its short data type, length and whole DM-SDU included, from
 * the same source to the same destination. Short data whose FCS fails is discarded (clause 6.3.2) before either: the
 * channel CRC does not stand in for it, and a corrupted copy kept as the last one noted would hide the good one. */
static void
dmo_sds_notice(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_dmo_sync_h_t* h) {
    if (h->fcs_state == TETRA_DMO_FCS_FAILED) {
        return;
    }
    tetra_dmo_call_t* const d = &ts->calls.dmo;
    const double now_m = dsd_decode_now_mono_s();
    const uint32_t dst = h->dst_encrypted ? 0u : h->dst;
    const tetra_dmo_sds_key key = dmo_sds_key_of(h, dst);
    if (dmo_sds_key_equal(&d->sds, &key) && now_m - d->sds_m < 10.0) {
        return;
    }
    d->sds = key;
    d->sds_m = now_m;
    char desc[160];
    char notice[224];
    (void)tetra_dmo_describe_elems(h, desc, (int)sizeof desc);
    DSD_SNPRINTF(notice, sizeof notice, "DMO SDS SRC: %u; TGT: %u; %s", (unsigned)h->src, (unsigned)dst, desc);
    calls_emit_notice(opts, state, 0, h->src, dst, notice, NULL);
}

/* The direct mode call canonical slot 0 shows ends when the channel's traffic is another transmission's now
 * (tetra_state_t::dmo_tx_seq): a transmission keeps its source and security class (EN 300 396-3 clauses 8.2.4,
 * 8.5.3), so what describes another is another call, which an identity the two calls do not both know could not tell
 * apart. A call its set-up announced before any transmission began (DM-SETUP PRES, whose DM-CONNECT ACK from the same
 * calling party starts the traffic, clause 6.2; DM-GSETUP) takes the first transmission of @p src, the source of the
 * PDU that describes it now (0 when none is known), as its own. 1 when the call on the channel is the transmission in
 * progress's. */
static int
dmo_call_of_transmission(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, uint32_t src) {
    tetra_dmo_call_t* const d = &ts->calls.dmo;
    if (d->tx_seq == ts->dmo_tx_seq) {
        return 1;
    }
    if (!d->in_tx && d->active && src != 0u && d->src == src) {
        d->tx_seq = ts->dmo_tx_seq;
        return 1;
    }
    if (ts->calls.published == TETRA_PUB_DMO) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    dmo_call_forget(d);
    d->tx_seq = ts->dmo_tx_seq;
    return 0;
}

void
tetra_calls_dmo_pdu(dsd_opts* opts, dsd_state* state, int sync_enc_state, const tetra_dmo_sync_h_t* h) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !h) {
        return;
    }
    tetra_dmo_call_t* const d = &ts->calls.dmo;
    if (!h->has_addrs) {
        /* DM-2-C encrypts the addresses and the message type too: an encrypted transmission, of no one known. */
        if (sync_enc_state == TETRA_DMO_AIE_DM2C) {
            (void)dmo_call_of_transmission(opts, state, ts, 0u);
            dmo_call_forget(d);
            d->active = 1;
            d->in_tx = (uint8_t)(ts->dmo_tx_crypt != (uint8_t)TETRA_DMO_CRYPT_UNKNOWN);
            d->seen_m = dsd_decode_now_mono_s();
            dmo_publish(opts, state, ts);
        }
        return;
    }
    switch (dmo_pdu_kind(h)) {
        case DMO_PDU_CALL:
            (void)dmo_call_of_transmission(opts, state, ts, h->src);
            dmo_call_from(d, h);
            d->in_tx = (uint8_t)(d->in_tx || ts->dmo_tx_crypt != (uint8_t)TETRA_DMO_CRYPT_UNKNOWN);
            d->seen_m = dsd_decode_now_mono_s();
            dmo_publish(opts, state, ts);
            break;
        case DMO_PDU_END:
            d->active = 0;
            if (ts->calls.published == TETRA_PUB_DMO) {
                calls_end_published(opts, state, ts, DSD_CALL_END_TERMINATOR);
            }
            break;
        case DMO_PDU_SDS: dmo_sds_notice(opts, state, ts, h); break;
        default: break;
    }
}

void
tetra_calls_dmo_protection(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts && calls_carrying(state, ts, TETRA_PUB_DMO, 0U) && dmo_call_of_transmission(opts, state, ts, 0u)) {
        calls_reclassify(opts, state, dmo_crypto_state(state));
    }
}

void
tetra_calls_dmo_forget(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    if (ts->calls.published == TETRA_PUB_DMO) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    DSD_MEMSET(&ts->calls.dmo, 0, sizeof ts->calls.dmo);
}

/* ---- Speech ---------------------------------------------------------------------------------------------------- */

const tetra_call_t*
tetra_calls_on_timeslot(const dsd_opts* opts, const dsd_state* state, uint8_t tn) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    const long freq_hz = tetra_sm_receiver_freq(opts, state);
    const uint8_t bit = tetra_slot_bit(tn);
    if (!ts || bit == 0u || freq_hz <= 0L) {
        return NULL;
    }
    const tetra_call_t* best = NULL;
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        const tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && c->alloc.freq_hz == freq_hz && (c->alloc.timeslots & bit) != 0u
            && (!best || c->alloc_m > best->alloc_m)) {
            best = c;
        }
    }
    return best;
}

/* Speech of a call record: the followed call may have turned undecryptable; a transmission that has not ceased
 * publishes. 1 when canonical slot 0 carries the call. */
static int
speech_of_record(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_speech* sp, double now_m) {
    tetra_call_t* const c = calls_find(ts, sp->call_id);
    if (!c) {
        return 0;
    }
    c->seen_m = now_m;
    if (sp->followed) {
        call_check_followed_crypto(opts, state, c);
        if (followed_id(state) != (int)c->call_id) {
            return 0;
        }
    }
    /* Speech that trails D-TX CEASED belongs to the transmission that ended. */
    if (c->phase == TETRA_CALL_CEASED) {
        return 0;
    }
    if (!calls_carrying(state, ts, TETRA_PUB_CALL, c->call_id)) {
        if (sp->followed) {
            calls_publish_followed(opts, state, ts);
        } else {
            calls_publish_call(opts, state, ts, c, &c->alloc);
        }
    } else {
        /* A cipher key loaded, or the network's KSG learnt, since: no PDU of the call says so, its speech does. */
        calls_reclassify(opts, state, crypto_state(state, c->e2ee, c->air_enc));
    }
    return calls_carrying(state, ts, TETRA_PUB_CALL, c->call_id);
}

/* Speech of the direct mode call a DM-SETUP or DM-OCCUPIED set up keeps it seen. */
static int
speech_of_dmo(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, double now_m) {
    tetra_dmo_call_t* const d = &ts->calls.dmo;
    if (!d->active) {
        return 0;
    }
    d->seen_m = now_m;
    if (!calls_carrying(state, ts, TETRA_PUB_DMO, 0U)) {
        dmo_publish(opts, state, ts);
    }
    return calls_carrying(state, ts, TETRA_PUB_DMO, 0U);
}

/* Speech no call record names publishes a call without identities, on a carrier a CRC-16 block proved: a speech frame's
 * CRC is too short to vouch for the carrier on its own. Its timeslot is all that names it: each timeslot is a traffic
 * channel of its own (EN 300 392-2 clause 9.4.2.2), so the speech of another timeslot is another call, which ends the
 * last one's (calls_publish()) rather than taking its epoch, media and crypto history. */
static int
speech_anonymous(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, const tetra_speech* sp) {
    if (!tetra_confirm_carrier_proven(state)) {
        return 0;
    }
    const dsd_call_crypto_state crypto = crypto_state(state, sp->sealed, sp->air_enc);
    if (!calls_carrying(state, ts, TETRA_PUB_ANON, sp->tn)) {
        dsd_call_observation o;
        DSD_MEMSET(&o, 0, sizeof o);
        o.protocol = calls_protocol(state);
        o.kind = DSD_CALL_KIND_VOICE;
        calls_publish(opts, state, ts, &o, crypto, TETRA_PUB_ANON, sp->tn);
    } else {
        /* Its timeslot's signalling encryption, which classifies it, may have changed since. */
        calls_reclassify(opts, state, crypto);
    }
    return calls_carrying(state, ts, TETRA_PUB_ANON, sp->tn);
}

/* The canonical call @p sp's speech publishes as (calls_publish_call(), dmo_publish(), speech_anonymous()): its kind
 * (@p private_call for an individual call), target and source. */
static void
speech_identity(const tetra_state_t* ts, const tetra_speech* sp, int* private_call, uint32_t* target,
                uint32_t* source) {
    *private_call = 0;
    *target = 0u;
    *source = 0u;
    if (sp->kind == TETRA_SPEECH_CALL) {
        const tetra_call_t* const c = calls_find_const(ts, sp->call_id);
        if (c) {
            *private_call = call_kind(c) == DSD_CALL_KIND_PRIVATE_VOICE;
            *target = tetra_calls_target(c);
            *source = calls_source(c);
        }
    } else if (sp->kind == TETRA_SPEECH_DMO && ts->calls.dmo.active) {
        const tetra_dmo_call_t* const d = &ts->calls.dmo;
        *private_call = dmo_call_kind(d) == DSD_CALL_KIND_PRIVATE_VOICE;
        *target = d->dst;
        *source = d->src;
    }
}

int
tetra_calls_speech_muted(const dsd_opts* opts, const dsd_state* state, const tetra_speech* sp) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || !sp || !opts) {
        return 0;
    }
    int private_call = 0;
    uint32_t target = 0u;
    uint32_t source = 0u;
    speech_identity(ts, sp, &private_call, &target, &source);
    int muted = 0;
    (void)dsd_audio_group_gate_call(opts, state, private_call, target, source, 0, &muted);
    return muted;
}

/* Not trunking, canonical slot 0 shows @p sp's speech: its timeslot is the heard one, and holds the receiver while its
 * speech plays (tetra_voice_hold()). Speech that does not play renews no hold. Another timeslot becomes the heard one
 * only once the last one's speech stopped, or with speech that plays and so holds it, so no live hold carries over. */
static void
calls_hear(tetra_calls_t* calls, const tetra_speech* sp, double now_m) {
    calls->heard = 1;
    calls->heard_tn = sp->tn;
    calls->heard_m = now_m;
    calls->heard_known = (uint8_t)(sp->kind == TETRA_SPEECH_CALL);
    calls->heard_call_id = sp->kind == TETRA_SPEECH_CALL ? sp->call_id : 0U;
    if (sp->audible) {
        calls->held = 1;
        calls->held_m = now_m;
    }
}

void
tetra_calls_on_speech(dsd_opts* opts, dsd_state* state, const tetra_speech* sp) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !sp) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    if (!sp->followed && sp->kind != TETRA_SPEECH_DMO) {
        calls_hear(&ts->calls, sp, now_m);
    }
    int carried = 0;
    switch (sp->kind) {
        case TETRA_SPEECH_CALL: carried = speech_of_record(opts, state, ts, sp, now_m); break;
        case TETRA_SPEECH_DMO: carried = speech_of_dmo(opts, state, ts, now_m); break;
        default: carried = speech_anonymous(opts, state, ts, sp); break;
    }
    if (carried && dsd_call_state_update_media(state, 0U, 1, 0.0) > 0 && opts) {
        dsd_event_sync_slot(opts, state, 0U);
    }
    if (sp->followed) {
        tetra_sm_note_voice(state, sp->tn);
    } else {
        dsd_mark_vc_sync(state);
    }
}

/* ---- Housekeeping ---------------------------------------------------------------------------------------------- */

void
tetra_calls_followed_moved(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    const tetra_call_t* const c = tetra_calls_followed(state);
    if (c && calls_carrying(state, ts, TETRA_PUB_CALL, c->call_id)) {
        calls_sync_published(opts, state, ts, c);
    } else if (ts->calls.published == TETRA_PUB_CALL || ts->calls.published == TETRA_PUB_ANON) {
        /* The receiver left the call followed before, for another: the state machine leaving a call ends it. */
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
}

void
tetra_calls_unfollowed(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    if (ts->calls.published == TETRA_PUB_CALL || ts->calls.published == TETRA_PUB_ANON) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    (void)dsd_tg_policy_clear_active_call(state, -1);
}

uint32_t
tetra_calls_addressed_ssi(const tetra_mac_sdu* sdu) {
    uint32_t ssi = 0;
    return sdu_ssi(sdu, &ssi) ? ssi : 0u;
}

void
tetra_calls_notice(dsd_opts* opts, dsd_state* state, int control, uint32_t src, uint32_t dst, const char* notice,
                   const char* text) {
    if (state && tetra_confirm_carrier_proven(state)) {
        calls_emit_notice(opts, state, control, src, dst, notice, text);
    }
}

/* The heard speech stopped with no terminator heard: its call ends, unless the state machine follows it now, and so
 * does the speech in progress, whether or not anything of it was published (a carrier no block proved yet publishes
 * no anonymous call): speech heard after the gap is another transmission's. */
static void
calls_tick_heard(dsd_opts* opts, dsd_state* state, tetra_state_t* ts, double now_m, int followed) {
    if (!ts->calls.heard || now_m - ts->calls.heard_m < TETRA_CALL_SPEECH_END_S) {
        return;
    }
    ts->calls.heard = 0;
    ts->calls.held = 0;
    tetra_voice_reset(state);
    const int heard_call = ts->calls.published == TETRA_PUB_CALL && ts->calls.heard_known
                           && ts->calls.published_call_id == ts->calls.heard_call_id
                           && followed != (int)ts->calls.heard_call_id;
    if (ts->calls.published == TETRA_PUB_ANON || heard_call) {
        calls_end_published(opts, state, ts, DSD_CALL_END_SYNC_LOSS);
    }
}

void
tetra_calls_tick(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    const double now_m = dsd_decode_now_mono_s();
    if (ts->calls.dmo.active && now_m - ts->calls.dmo.seen_m >= TETRA_CALL_DMO_IDLE_S) {
        /* The call is over, and so is its transmission's colour code and protection. */
        ts->calls.dmo.active = 0;
        tetra_dmo_transmission_over(state);
        if (ts->calls.published == TETRA_PUB_DMO) {
            calls_end_published(opts, state, ts, DSD_CALL_END_SYNC_LOSS);
        }
    }
    const int followed = followed_id(state);
    calls_tick_heard(opts, state, ts, now_m, followed);
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        if (c->phase != TETRA_CALL_FREE && (int)c->call_id != followed && now_m - c->seen_m >= TETRA_CALL_IDLE_S) {
            TETRA_LOG_VERBOSE(ts, 2, "[TETRA CALL] call=%u not heard from for %.0f s; dropped\n", (unsigned)c->call_id,
                              TETRA_CALL_IDLE_S);
            calls_drop(opts, state, ts, c);
        }
    }
}

/* What the speech of the trunked carrier left: its timeslots' signalling encryption and the heard timeslot. */
static void
calls_forget_carrier_speech(tetra_state_t* ts) {
    DSD_MEMSET(ts->calls.slot_enc, 0, sizeof ts->calls.slot_enc);
    ts->calls.heard = 0;
    ts->calls.held = 0;
    ts->calls.heard_known = 0;
}

void
tetra_calls_forget_carrier(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        calls_forget_carrier_speech(ts);
        /* The next carrier's direct mode call is set up by its own DMAC-SYNC (the engine ends the canonical calls). */
        DSD_MEMSET(&ts->calls.dmo, 0, sizeof ts->calls.dmo);
    }
}

void
tetra_calls_forget_trunked_carrier(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    /* The followed call is the state machine's to end; the heard call and anonymous speech were the last carrier's. */
    const int heard_call =
        ts->calls.published == TETRA_PUB_CALL && (int)ts->calls.published_call_id != followed_id(state);
    if (ts->calls.published == TETRA_PUB_ANON || heard_call) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    calls_forget_carrier_speech(ts);
}

void
tetra_calls_forget_cell(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        tetra_call_t* const c = &ts->calls.call[i];
        DSD_MEMSET(&c->alloc, 0, sizeof c->alloc);
        c->alloc_m = 0.0;
        label_drop(c);
        c->usage_marker = 0U;
        c->flags = (uint8_t)(c->flags & ~TETRA_CALL_F_USAGE_MARKER);
    }
}

void
tetra_calls_forget_calls(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    if (ts->calls.published == TETRA_PUB_CALL || ts->calls.published == TETRA_PUB_ANON) {
        calls_end_published(opts, state, ts, DSD_CALL_END_EXPLICIT);
    }
    for (size_t i = 0; i < TETRA_CALLS_MAX; i++) {
        calls_drop(opts, state, ts, &ts->calls.call[i]);
    }
    ts->calls.heard = 0;
    ts->calls.held = 0;
    ts->calls.heard_known = 0;
    ts->calls.heard_call_id = 0U;
    DSD_MEMSET(ts->calls.slot_enc, 0, sizeof ts->calls.slot_enc);
    ts->calls.la_known = 0;
    /* The speech in progress was one of theirs. */
    ts->calls.ends++;
}

void
tetra_calls_note_location_area(dsd_opts* opts, dsd_state* state, uint16_t la) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    if (ts->calls.la_known && ts->calls.la != la) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CALL] location area %u after %u: its calls are not the last area's\n",
                          (unsigned)la, (unsigned)ts->calls.la);
        tetra_calls_forget_calls(opts, state);
    }
    ts->calls.la = la;
    ts->calls.la_known = 1;
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
