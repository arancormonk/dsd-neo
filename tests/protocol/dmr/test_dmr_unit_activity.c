// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR unit activity rows, driven through the CSBK dispatcher dmr_cspdu() and the compressed UDP data path
 * dmr_udp_comp_pdu(): every row of the DMR table (TS 102 361-4 Tables 6.9, 6.11, 6.17, 6.19, 6.23, 6.27, 7.23,
 * 7.41-7.45, A.8; MOTOTRBO ARS 3.4), FID 0x00 only, CRC-checked CSBKs only, gateways as the actor and never the
 * subject, and C_AHOY commands as commands.
 */

#include <assert.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

enum {
    REGI = 0xFFFEC6,
    TSI = 0xFFFECA,
    STUNI = 0xFFFECC,
    KILLI = 0xFFFECF,
    TATTSI = 0xFFFED7,
    RADIO = 1234567,
    PEER = 7654321,
};

static int g_failures;
static uint64_t g_instance;
static uint64_t g_rev;

static void
expect_true(int cond, const char* tag) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_failures++;
    }
}

/* The rows revised since the last call (a frontend's cursor). */
static int
read_new(const dsd_state* state, dsd_unit_activity* out, int cap) {
    dsd_unit_activity_read_meta meta;
    const int n = dsd_unit_activity_read(state, g_instance, g_rev, out, cap, &meta);
    if (meta.held) {
        g_instance = meta.instance;
        g_rev = meta.last_rev;
    }
    return n;
}

/* Exactly one new row: its copy in @p out. */
static int
one_row(const dsd_state* state, dsd_unit_activity* out, const char* tag) {
    dsd_unit_activity rows[8];
    const int n = read_new(state, rows, 8);
    if (n != 1) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d new rows, want 1\n", tag, n);
        g_failures++;
        return 0;
    }
    *out = rows[0];
    return 1;
}

static void
expect_no_row(const dsd_state* state, const char* tag) {
    dsd_unit_activity rows[8];
    const int n = read_new(state, rows, 8);
    if (n != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d new rows, want none (kind %u)\n", tag, n, (unsigned)rows[0].kind);
        g_failures++;
    }
}

static void
expect_identity(const dsd_ua_identity* id, uint32_t want_id, uint8_t want_subtype, const char* tag) {
    if (!id->valid || id->id != want_id || id->subtype != want_subtype) {
        DSD_FPRINTF(stderr, "FAIL: %s: identity valid %u id %u subtype %u, want id %u subtype %u\n", tag,
                    (unsigned)id->valid, (unsigned)id->id, (unsigned)id->subtype, (unsigned)want_id,
                    (unsigned)want_subtype);
        g_failures++;
    }
}

static void
expect_kind(const dsd_unit_activity* r, uint8_t kind, uint8_t role, uint8_t result, const char* tag) {
    if (r->protocol != DSD_UA_PROTO_DMR || r->kind != kind || r->role != role || r->result != result) {
        DSD_FPRINTF(stderr, "FAIL: %s: protocol %u kind %u role %u result %u, want DMR kind %u role %u result %u\n",
                    tag, (unsigned)r->protocol, (unsigned)r->kind, (unsigned)r->role, (unsigned)r->result,
                    (unsigned)kind, (unsigned)role, (unsigned)result);
        g_failures++;
    }
}

static void
bytes_to_bits(const uint8_t* bytes, size_t count, uint8_t* bits) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - (i % 8U))) & 1U);
    }
}

/* One CSBK through the dispatcher with the given CRC verdict; @p ten holds its octets 0-9. */
static void
run_csbk_crc(dsd_opts* opts, dsd_state* state, const uint8_t ten[10], uint32_t crc_correct) {
    uint8_t bytes[24];
    uint8_t bits[sizeof bytes * 8U];
    DSD_MEMSET(bytes, 0, sizeof bytes);
    DSD_MEMCPY(bytes, ten, 10);
    bytes_to_bits(bytes, sizeof bytes, bits);
    dsd_test_quiet_stderr cap;
    assert(dsd_test_quiet_stderr_begin(&cap) == 0);
    dmr_cspdu(opts, state, bits, bytes, crc_correct, 0);
    assert(dsd_test_quiet_stderr_end(&cap) == 0);
}

static void
run_csbk(dsd_opts* opts, dsd_state* state, const uint8_t ten[10]) {
    run_csbk_crc(opts, state, ten, 1U);
}

/* C_ACKD (opcode 32, FID @p fid): Response_Info bits 16-22, Reason bits 23-30, target 32-55, source 56-79. */
static void
run_ackd_fid(dsd_opts* opts, dsd_state* state, uint8_t fid, uint8_t response_info, uint8_t reason, uint32_t target,
             uint32_t source) {
    uint8_t pdu[10];
    pdu[0] = 0xA0U;
    pdu[1] = fid;
    pdu[2] = (uint8_t)(((response_info & 0x7FU) << 1) | (reason >> 7));
    pdu[3] = (uint8_t)((reason << 1) & 0xFEU);
    pdu[4] = (uint8_t)(target >> 16);
    pdu[5] = (uint8_t)(target >> 8);
    pdu[6] = (uint8_t)target;
    pdu[7] = (uint8_t)(source >> 16);
    pdu[8] = (uint8_t)(source >> 8);
    pdu[9] = (uint8_t)source;
    run_csbk(opts, state, pdu);
}

static void
run_ackd(dsd_opts* opts, dsd_state* state, uint8_t response_info, uint8_t reason, uint32_t target, uint32_t source) {
    run_ackd_fid(opts, state, 0x00U, response_info, reason, target, source);
}

/* C_AHOY (opcode 28): Service_Kind_Flag bit 23, G/I bit 25, Service_Kind bits 28-31. */
static void
run_ahoy(dsd_opts* opts, dsd_state* state, uint8_t fid, uint8_t flag, uint8_t gi, uint8_t kind, uint32_t target,
         uint32_t source) {
    uint8_t pdu[10] = {0x9CU, fid};
    pdu[2] = flag & 1U;
    pdu[3] = (uint8_t)(((gi & 1U) << 6) | (kind & 0x0FU));
    pdu[4] = (uint8_t)(target >> 16);
    pdu[5] = (uint8_t)(target >> 8);
    pdu[6] = (uint8_t)target;
    pdu[7] = (uint8_t)(source >> 16);
    pdu[8] = (uint8_t)(source >> 8);
    pdu[9] = (uint8_t)source;
    run_csbk(opts, state, pdu);
}

static void
test_registration_acks(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    /* Reg_Accepted from REGI: acknowledged, never "accepted" (it answers a deregistration too), no reason. */
    run_ackd(opts, state, 0x05U, 0x62U, RADIO, REGI);
    if (one_row(state, &r, "0x62 row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, "0x62 kind");
        expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "0x62 subject is the MS");
        expect_identity(&r.actor, REGI, DSD_UA_ID_INFRASTRUCTURE, "0x62 REGI is the actor");
        expect_true(r.reason_set == DSD_UA_REASON_NONE, "0x62 sets no reason");
        expect_true(!r.value_valid, "0x62 power save offset is no value");
    }
    /* 0x62 from another source than REGI is not a registration answer. */
    run_ackd(opts, state, 0x00U, 0x62U, RADIO, PEER);
    expect_no_row(state, "0x62 not from REGI");
    /* Other acknowledgements add nothing. */
    run_ackd(opts, state, 0x00U, 0x60U, RADIO, REGI);
    expect_no_row(state, "0x60 message accepted");

    run_ackd(opts, state, 0x00U, 0x2AU, RADIO, REGI);
    if (one_row(state, &r, "0x2A row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, "0x2A refused");
        expect_true(r.reason_set == DSD_UA_REASON_DMR_TIER3 && r.reason_code == 0x2AU, "0x2A reason");
        expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "0x2A subject");
        expect_identity(&r.actor, REGI, DSD_UA_ID_INFRASTRUCTURE, "0x2A actor");
    }
    run_ackd(opts, state, 0x00U, 0x2BU, RADIO, REGI);
    if (one_row(state, &r, "0x2B row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, "0x2B denied");
        expect_true(r.reason_code == 0x2BU, "0x2B reason");
    }
    /* Reg_Refused not from REGI is an ordinary refusal; the destination is kept, of unknown kind. */
    run_ackd(opts, state, 0x00U, 0x2AU, RADIO, PEER);
    if (one_row(state, &r, "0x2A other source row")) {
        expect_kind(&r, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, "0x2A not from REGI is a deny");
        expect_identity(&r.recipient, PEER, DSD_UA_ID_UNKNOWN, "destination kept as the recipient");
        expect_true(!r.actor.valid, "a destination is not the actor");
    }
}

static void
test_subscription_masks(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    run_ackd(opts, state, 0x00U, 0x65U, RADIO, REGI);
    if (one_row(state, &r, "0x65 zero mask row")) {
        expect_kind(&r, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, "zero mask refused");
        expect_true(r.value_valid && r.value == 0U, "zero mask value");
        expect_true(!r.group_valid, "no talkgroup invented");
        expect_true(r.reason_set == DSD_UA_REASON_DMR_TIER3 && r.reason_code == 0x65U, "0x65 reason");
    }
    run_ackd(opts, state, 0x05U, 0x65U, RADIO, REGI);
    if (one_row(state, &r, "0x65 mixed mask row")) {
        expect_kind(&r, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, "mixed mask");
        expect_true(r.value_valid && r.value == 0x05U, "mixed mask value");
        expect_true(!r.group_valid, "mixed mask invents no talkgroup");
    }
}

static void
test_refusal_classes(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    /* NACK class 0x27 (network busy), G/I 0, destination PEER. */
    run_ackd(opts, state, 0x00U, 0x27U, RADIO, PEER);
    if (one_row(state, &r, "nack row")) {
        expect_kind(&r, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, "nack is a deny");
        expect_true(r.reason_set == DSD_UA_REASON_DMR_TIER3 && r.reason_code == 0x27U, "nack reason");
        expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "nack subject");
    }
    /* G/I 1 (Response_Info bit 16): the target address is a talkgroup (Table 7.41). */
    run_ackd(opts, state, 0x40U, 0x27U, 100U, PEER);
    if (one_row(state, &r, "group-addressed nack row")) {
        expect_identity(&r.subject, 100U, DSD_UA_ID_GROUP, "group-addressed subject is a talkgroup");
    }
    /* QACK and WACK. */
    run_ackd(opts, state, 0x00U, 0xA0U, RADIO, PEER);
    if (one_row(state, &r, "qack row")) {
        expect_kind(&r, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED, "qack is queued");
        expect_true(r.reason_code == 0xA0U, "qack reason");
    }
    run_ackd(opts, state, 0x00U, 0xE0U, RADIO, REGI);
    if (one_row(state, &r, "wack row")) {
        expect_kind(&r, DSD_UA_WAITING, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, "wack is waiting, not queued");
        expect_identity(&r.actor, REGI, DSD_UA_ID_INFRASTRUCTURE, "wack gateway is the actor");
    }
    /* A refusal from a gateway names it as the actor. */
    run_ackd(opts, state, 0x00U, 0x23U, RADIO, STUNI);
    if (one_row(state, &r, "gateway nack row")) {
        expect_identity(&r.actor, STUNI, DSD_UA_ID_INFRASTRUCTURE, "gateway actor");
        expect_true(!r.recipient.valid, "gateway is no recipient");
    }
}

static void
test_dmr_ahoy_gateway_not_subject(dsd_opts* opts, dsd_state* state) {
    static const struct {
        uint8_t flag;
        uint32_t gateway;
        uint8_t kind;
        const char* tag;
    } cases[] = {
        {0U, STUNI, DSD_UA_STUN, "stun"},
        {1U, STUNI, DSD_UA_REVIVE, "revive"},
        {0U, KILLI, DSD_UA_KILL, "kill"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_unit_activity r;
        run_ahoy(opts, state, 0x00U, cases[i].flag, 0U, 13U, RADIO, cases[i].gateway);
        if (one_row(state, &r, cases[i].tag)) {
            expect_kind(&r, cases[i].kind, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, cases[i].tag);
            expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "the target is the subject");
            expect_identity(&r.actor, cases[i].gateway, DSD_UA_ID_INFRASTRUCTURE, "the gateway is the actor");
            expect_true(r.subject.id != cases[i].gateway, "the gateway is never the subject");
        }
    }
    /* A stun of a talkgroup, DGNA and kill with flag 1 are no command this log names. */
    run_ahoy(opts, state, 0x00U, 0U, 1U, 13U, 100U, STUNI);
    expect_no_row(state, "stun of a talkgroup");
    run_ahoy(opts, state, 0x00U, 0U, 0U, 13U, RADIO, 0xFFFED6U);
    expect_no_row(state, "dgna");
    run_ahoy(opts, state, 0x00U, 1U, 0U, 13U, RADIO, KILLI);
    expect_no_row(state, "kill flag 1");
}

static void
test_ahoy_kind14(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    run_ahoy(opts, state, 0x00U, 0U, 0U, 14U, RADIO, TSI);
    if (one_row(state, &r, "radio check row")) {
        expect_kind(&r, DSD_UA_RADIO_CHECK, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, "radio check");
        expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "radio check subject");
        expect_identity(&r.actor, TSI, DSD_UA_ID_INFRASTRUCTURE, "radio check gateway");
    }
    run_ahoy(opts, state, 0x00U, 0U, 1U, 14U, 100U, TSI);
    if (one_row(state, &r, "talkgroup radio check row")) {
        expect_identity(&r.subject, 100U, DSD_UA_ID_GROUP, "talkgroup radio check subject");
    }
    run_ahoy(opts, state, 0x00U, 0U, 0U, 14U, RADIO, 0x123456U);
    uint64_t auth_seq = 0;
    if (one_row(state, &r, "authentication row")) {
        expect_kind(&r, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, "authentication");
        expect_identity(&r.subject, RADIO, DSD_UA_ID_RADIO, "authentication subject");
        expect_true(!r.actor.valid && !r.recipient.valid && !r.assigned.valid, "the challenge is never an identity");
        expect_true(!r.value_valid && !r.value2_valid, "the challenge is no value");
        auth_seq = r.seq;
    }
    /* Two challenges to one radio are one row: the challenge is not part of the row (the merge revises it). */
    run_ahoy(opts, state, 0x00U, 0U, 0U, 14U, RADIO, 0x654321U);
    if (one_row(state, &r, "second challenge")) {
        expect_true(r.seq == auth_seq && r.repeat == 2U, "a second challenge merges into the row");
    }

    run_ahoy(opts, state, 0x00U, 1U, 0U, 14U, RADIO, TATTSI);
    if (one_row(state, &r, "subscription request row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, "subscription list request");
        expect_identity(&r.actor, TATTSI, DSD_UA_ID_INFRASTRUCTURE, "subscription gateway");
    }
    /* A voice call poll (Service_Kind 0) is no unit event. */
    run_ahoy(opts, state, 0x00U, 0U, 0U, 0U, RADIO, PEER);
    expect_no_row(state, "voice poll");
}

static void
test_fid_and_crc_gates(dsd_opts* opts, dsd_state* state) {
    /* Vendor FIDs reuse opcodes 28 and 32 for their own PDUs. */
    run_ackd_fid(opts, state, 0x10U, 0x00U, 0x62U, RADIO, REGI);
    expect_no_row(state, "FID 0x10 op 32");
    run_ackd_fid(opts, state, 0x68U, 0x00U, 0x2BU, RADIO, REGI);
    expect_no_row(state, "FID 0x68 op 32");
    run_ahoy(opts, state, 0x06U, 0U, 0U, 13U, RADIO, STUNI);
    expect_no_row(state, "FID 0x06 op 28");

    /* A CSBK whose CRC failed is not dispatched; one -F accepted as RAS is dispatched but logs nothing. */
    uint8_t pdu[10] = {0xA0U, 0x00U, 0x00U, 0xC4U, 0x00U, 0x00U, 0x7BU, 0xFFU, 0xFEU, 0xC6U};
    run_csbk_crc(opts, state, pdu, 0U);
    expect_no_row(state, "failed CRC");
    opts->dmr_crc_relaxed_default = 1;
    run_csbk_crc(opts, state, pdu, 0U);
    expect_no_row(state, "failed CRC under -F");
    state->event_crc_invalid[0] = 1U;
    run_csbk(opts, state, pdu);
    expect_no_row(state, "RAS-accepted CSBK");
    run_ahoy(opts, state, 0x00U, 0U, 0U, 13U, RADIO, KILLI);
    expect_no_row(state, "RAS-accepted C_AHOY");
    state->event_crc_invalid[0] = 0U;
    opts->dmr_crc_relaxed_default = 0;
}

static void
test_repeat_merges(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    run_ackd(opts, state, 0x00U, 0x2BU, PEER, REGI);
    run_ackd(opts, state, 0x00U, 0x2BU, PEER, REGI);
    if (one_row(state, &r, "repeat")) {
        expect_true(r.repeat == 2U, "a repeated answer is one row");
    }
}

/* A compressed-UDP ARS datagram (TS 102 361-3 Table 7.14) from radio @p source, as test_dmr_udp_comp_header.c sends
   one: extended headers with UDP port 4005 both ways, then the ARS message. */
static void
run_ars(dsd_opts* opts, dsd_state* state, const uint8_t* ars, size_t ars_len, uint32_t source) {
    uint8_t pdu[64] = {0x00U, 0x05U, 0x01U, 0x00U, 0x00U, 0x0FU, 0xA5U, 0x0FU, 0xA5U};
    assert(ars_len + 9U <= sizeof pdu);
    DSD_MEMCPY(pdu + 9, ars, ars_len);
    state->currentslot = 0;
    state->dmr_lrrp_source[0] = source;
    state->dmr_lrrp_target[0] = 5678;
    dsd_test_quiet_stderr cap;
    assert(dsd_test_quiet_stderr_begin(&cap) == 0);
    dmr_udp_comp_pdu(opts, state, (uint16_t)(ars_len + 9U), pdu);
    assert(dsd_test_quiet_stderr_end(&cap) == 0);
}

static void
test_ars_rows(dsd_opts* opts, dsd_state* state) {
    /* MOTOTRBO ARS 2.3.1: an initial device registration of radio 11. */
    static const uint8_t reg[] = {0x00U, 0x07U, 0xF0U, 0x20U, 0x02U, 0x31U, 0x31U, 0x00U, 0x00U};
    static const uint8_t dereg[] = {0x00U, 0x01U, 0x31U};
    static const uint8_t ack[] = {0x00U, 0x02U, 0xBFU, 0x01U};
    dsd_unit_activity r;
    const int saved = state->lastsynctype;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    run_ars(opts, state, reg, sizeof reg, 1234U);
    if (one_row(state, &r, "ars registration row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, "ars registration");
        expect_identity(&r.subject, 1234U, DSD_UA_ID_RADIO, "ars subject is the sending radio");
    }
    run_ars(opts, state, dereg, sizeof dereg, 1234U);
    if (one_row(state, &r, "ars deregistration row")) {
        expect_kind(&r, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, "ars deregistration");
    }
    run_ars(opts, state, ack, sizeof ack, 1234U);
    expect_no_row(state, "ars ack");
    /* A user registration of the same radio is another report than its device registration. */
    static const uint8_t user_reg[] = {0x00U, 0x10U, 0xF5U, 0x00U, 0x02U, 0x31U, 0x31U, 0x09U, '9',
                                       '9',   '9',   '9',   '9',   '9',   '9',   '9',   '9',   0x00U};
    run_ars(opts, state, user_reg, sizeof user_reg, 1234U);
    if (one_row(state, &r, "ars user registration row")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, "ars user registration");
        expect_true(r.value2_valid && r.value2 == 5U, "a user registration is its own row");
    }
    /* A data PDU whose CRC failed (accepted under -F) logs nothing. */
    state->event_crc_invalid[0] = 1U;
    run_ars(opts, state, dereg, sizeof dereg, 2345U);
    expect_no_row(state, "ars with a failed CRC");
    state->event_crc_invalid[0] = 0U;
    /* The same path carries P25 data: no DMR row from it. */
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    run_ars(opts, state, dereg, sizeof dereg, 3456U);
    expect_no_row(state, "ars on P25");
    state->lastsynctype = saved;
}

/* The row names its subject radio as the current reception names it (dsd_unit_activity_freeze_name()); a gateway, a
   talkgroup subject and a radio no list names stay unnamed. */
static void
test_naming(dsd_opts* opts, dsd_state* state) {
    dsd_unit_activity r;
    (void)dsd_alias_learn_radio(NULL, state, NULL, 2468024U, "ENGINE 7", NULL);
    (void)dsd_alias_learn_radio(NULL, state, NULL, 300U, "NOT A GROUP NAME", NULL);
    run_ackd(opts, state, 0x00U, 0x62U, 2468024U, REGI);
    if (one_row(state, &r, "named registration")) {
        expect_true(strcmp(r.name_frozen, "ENGINE 7") == 0, "the subject radio is named at push");
    }
    run_ackd(opts, state, 0x40U, 0x27U, 300U, PEER);
    if (one_row(state, &r, "group-addressed deny")) {
        expect_true(r.name_frozen[0] == '\0', "a talkgroup subject is never named as a radio");
    }
    run_ahoy(opts, state, 0x00U, 0U, 0U, 13U, 1357913U, STUNI);
    if (one_row(state, &r, "unnamed radio")) {
        expect_true(r.name_frozen[0] == '\0', "a radio no list names stays unnamed, the gateway is not named");
    }
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    state->event_history_s = (Event_History_I*)calloc(2, sizeof(Event_History_I));
    assert(state->event_history_s != NULL);
    for (int i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }

    test_registration_acks(opts, state);
    test_subscription_masks(opts, state);
    test_refusal_classes(opts, state);
    test_dmr_ahoy_gateway_not_subject(opts, state);
    test_ahoy_kind14(opts, state);
    test_fid_and_crc_gates(opts, state);
    test_repeat_merges(opts, state);
    test_ars_rows(opts, state);
    test_naming(opts, state);

    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DMR_UNIT_ACTIVITY: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "DMR_UNIT_ACTIVITY: PASS\n");
    return 0;
}
