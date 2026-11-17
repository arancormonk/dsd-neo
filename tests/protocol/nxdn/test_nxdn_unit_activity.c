// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN Type-C unit activity rows (NXDN TS 1-A v1.3 Figs 6.4-20 / 6.4-21 / 6.4-45 / 6.4-47 / 6.4-50, Tables 6.5-22,
 * 6.5-27, 6.5-29), driven through a CAC (nxdn_handle_cac(), which dispatches a CRC-checked CAC's elements) and the
 * element dispatch NXDN_Elements_Content_decode() for lengths a CAC cannot carry: minimum lengths, optional visitor
 * fields, the group / unit flag, remote-control roles, the Type-C path only (no Type-D or traffic channel rows) and
 * no row from a CAC whose CRC failed.
 */

#include <assert.h>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_internal.h"
#include "test_support.h"

enum { CAC_ELEMENTS = 147 };

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
    if (out->protocol != DSD_UA_PROTO_NXDN) {
        DSD_FPRINTF(stderr, "FAIL: %s: protocol %u\n", tag, (unsigned)out->protocol);
        g_failures++;
    }
    return 1;
}

static void
expect_no_row(const dsd_state* state, const char* tag) {
    dsd_unit_activity rows[8];
    const int n = read_new(state, rows, 8);
    if (n != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: %d new rows, want none\n", tag, n);
        g_failures++;
    }
}

static void
expect_kind(const dsd_unit_activity* r, uint8_t kind, uint8_t role, uint8_t result, const char* tag) {
    if (r->kind != kind || r->role != role || r->result != result) {
        DSD_FPRINTF(stderr, "FAIL: %s: kind %u role %u result %u, want kind %u role %u result %u\n", tag,
                    (unsigned)r->kind, (unsigned)r->role, (unsigned)r->result, (unsigned)kind, (unsigned)role,
                    (unsigned)result);
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

/* A message as elements: octet n at element 8n, MSB first. */
static void
put_bits(uint8_t* e, size_t pos, uint32_t value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        e[pos + i] = (uint8_t)((value >> (count - 1U - i)) & 1U);
    }
}

/* Octets 1-3 of REG_RESP / REG_C_RESP: Registration Option (Em, H/V, 3 spare) and the 19-bit Location ID without the
   Site Code, here category 01 (Local) and system code @p sys (17 bits). */
static void
put_reg_option_location(uint8_t* e, uint8_t visitor, uint32_t sys) {
    put_bits(e, 8, 0U, 1);
    put_bits(e, 9, visitor, 1);
    put_bits(e, 13, 1U, 2);
    put_bits(e, 15, sys, 17);
}

static void
build_reg_resp(uint8_t* e, uint8_t visitor, uint32_t unit, uint32_t group, uint8_t cause, uint32_t v_unit,
               uint32_t v_group) {
    DSD_MEMSET(e, 0, CAC_ELEMENTS);
    put_bits(e, 2, 0x20U, 6);
    put_reg_option_location(e, visitor, 77U);
    put_bits(e, 32, unit, 16);
    put_bits(e, 48, group, 16);
    put_bits(e, 64, cause, 8);
    put_bits(e, 72, v_unit, 16);
    put_bits(e, 88, v_group, 16);
}

static void
build_reg_c_resp(uint8_t* e, uint32_t unit, uint8_t cause) {
    DSD_MEMSET(e, 0, CAC_ELEMENTS);
    put_bits(e, 2, 0x22U, 6);
    put_reg_option_location(e, 0U, 77U);
    put_bits(e, 32, unit, 16);
    put_bits(e, 48, cause, 8);
}

static void
build_grp_reg_resp(uint8_t* e, uint8_t visitor, uint32_t unit, uint32_t group, uint8_t cause, uint32_t v_group) {
    DSD_MEMSET(e, 0, CAC_ELEMENTS);
    put_bits(e, 2, 0x24U, 6);
    put_bits(e, 9, visitor, 1);
    put_bits(e, 16, unit, 16);
    put_bits(e, 32, group, 16);
    put_bits(e, 48, cause, 8);
    put_bits(e, 56, v_group, 16);
}

static void
build_rem_con(uint8_t* e, uint8_t type, uint8_t unit_addressed, uint8_t command, uint32_t source, uint32_t dest,
              uint32_t last16) {
    DSD_MEMSET(e, 0, CAC_ELEMENTS);
    put_bits(e, 2, type, 6);
    put_bits(e, 16, unit_addressed, 1);
    put_bits(e, 19, command, 5);
    put_bits(e, 24, source, 16);
    put_bits(e, 40, dest, 16);
    put_bits(e, 56, last16, 16);
}

/* One CAC whose CRC check gave @p crc (0 passed), with the message @p e. */
static void
run_cac_crc(dsd_opts* opts, dsd_state* state, const uint8_t* e, uint16_t crc) {
    uint8_t trellis[8 + CAC_ELEMENTS];
    uint8_t m_data[32];
    DSD_MEMSET(trellis, 0, sizeof trellis);
    DSD_MEMSET(m_data, 0, sizeof m_data);
    DSD_MEMCPY(trellis + 8, e, CAC_ELEMENTS);
    state->nxdn_frame_channel = (uint8_t)NXDN_FRAME_CHANNEL_CONTROL;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "nxdn_unit_activity") == 0);
    nxdn_handle_cac(opts, state, trellis, m_data, crc);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
}

static void
run_cac(dsd_opts* opts, dsd_state* state, const uint8_t* e) {
    run_cac_crc(opts, state, e, 0U);
}

/* The element dispatch alone, on frame channel @p channel, with @p bits elements. */
static void
run_elements(dsd_opts* opts, dsd_state* state, const uint8_t* e, size_t bits, uint8_t channel) {
    state->nxdn_frame_channel = channel;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "nxdn_unit_activity_elements") == 0);
    NXDN_Elements_Content_decode(opts, state, e, bits);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
}

static void
test_reg_resp(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    build_reg_resp(e, 0U, 1001U, 200U, 0x01U, 0U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "reg_resp accepted")) {
        expect_kind(&r, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, "reg_resp accepted kind");
        expect_identity(&r.subject, 1001U, DSD_UA_ID_RADIO, "reg_resp subject is the destination unit");
        expect_true(r.subject.home_valid && r.subject.home_kind == DSD_UA_SCOPE_NXDN && r.subject.home_a == 0x102U
                        && r.subject.home_b == 77U,
                    "reg_resp home is the subscriber's Local system 77, apart from the receiving site");
        expect_true(r.group_valid && r.group == 200U, "reg_resp group");
        expect_true(!r.assigned.valid && !r.value2_valid, "no visitor fields at home");
        expect_true(r.reason_set == DSD_UA_REASON_NXDN_MM && r.reason_code == 0x01U, "reg_resp cause");
    }

    static const struct {
        uint8_t cause;
        uint8_t result;
    } causes[] = {
        {0x08U, DSD_UA_RESULT_REFUSED}, {0x06U, DSD_UA_RESULT_FAILED},  {0x05U, DSD_UA_RESULT_ACCEPTED},
        {0x53U, DSD_UA_RESULT_FAILED},  {0x30U, DSD_UA_RESULT_UNKNOWN},
    };

    for (size_t i = 0; i < sizeof causes / sizeof causes[0]; i++) {
        build_reg_resp(e, 0U, 1001U, 200U, causes[i].cause, 0U, 0U);
        run_cac(opts, state, e);
        if (one_row(state, &r, "reg_resp cause row")) {
            expect_true(r.result == causes[i].result && r.reason_code == causes[i].cause, "reg_resp cause result");
        }
    }
}

static void
test_reg_resp_visitor(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    build_reg_resp(e, 1U, 1002U, 200U, 0x01U, 3001U, 400U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "visitor reg_resp")) {
        expect_identity(&r.subject, 1002U, DSD_UA_ID_RADIO, "visitor subject is the home unit ID");
        expect_identity(&r.assigned, 3001U, DSD_UA_ID_RADIO, "visitor unit ID is the assigned identity");
        expect_true(r.value2_valid && r.value2 == 400U, "visitor group ID kept");
    }
    /* Pairs that differ in one field are two rows: the visitor group, the group. */
    build_reg_resp(e, 1U, 1002U, 200U, 0x01U, 3001U, 401U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "other visitor group")) {
        expect_true(r.value2 == 401U, "other visitor group is another row");
    }
    build_reg_resp(e, 1U, 1002U, 201U, 0x01U, 3001U, 401U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "other group")) {
        expect_true(r.group == 201U, "other group is another row");
    }
    /* H/V set but the message ends after its mandatory part: no visitor field is read. */
    run_elements(opts, state, e, 72U, (uint8_t)NXDN_FRAME_CHANNEL_CONTROL);
    if (one_row(state, &r, "visitor fields absent")) {
        expect_true(!r.assigned.valid && !r.value2_valid, "absent visitor fields are not read");
    }
    /* One element short of the mandatory part: no row. */
    run_elements(opts, state, e, 71U, (uint8_t)NXDN_FRAME_CHANNEL_CONTROL);
    expect_no_row(state, "reg_resp too short");
    /* H/V clear: the octets after the cause are not visitor fields. */
    build_reg_resp(e, 0U, 1003U, 200U, 0x01U, 3001U, 400U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "home reg_resp with trailing octets")) {
        expect_true(!r.assigned.valid && !r.value2_valid, "H/V clear reads no visitor fields");
    }
}

static void
test_reg_c_and_group(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    build_reg_c_resp(e, 1004U, 0x01U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "reg_c_resp")) {
        expect_true(r.kind == DSD_UA_DEREGISTRATION && r.role == DSD_UA_ROLE_RESPONSE, "reg_c_resp is a response");
        expect_true(r.result == DSD_UA_RESULT_UNKNOWN, "an unconfirmed clear success is not stored as accepted");
        expect_identity(&r.subject, 1004U, DSD_UA_ID_RADIO, "reg_c_resp subject");
        expect_true(r.subject.home_valid && r.subject.home_b == 77U, "reg_c_resp home");
        expect_true(r.reason_set == DSD_UA_REASON_NXDN_MM && r.reason_code == 0x01U, "reg_c_resp cause");
    }
    build_reg_c_resp(e, 1004U, 0x08U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "reg_c_resp refused")) {
        expect_true(r.result == DSD_UA_RESULT_REFUSED, "a refused clear is refused");
    }
    build_grp_reg_resp(e, 0U, 1005U, 300U, 0x01U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "grp_reg_resp")) {
        expect_kind(&r, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, "grp_reg_resp kind");
        expect_identity(&r.subject, 1005U, DSD_UA_ID_RADIO, "grp_reg_resp subject");
        expect_true(r.group_valid && r.group == 300U, "grp_reg_resp group");
        expect_true(!r.subject.home_valid, "grp_reg_resp names no home");
    }
    build_grp_reg_resp(e, 0U, 1005U, 300U, 0x05U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "grp_reg_resp refused")) {
        expect_true(r.result == DSD_UA_RESULT_REFUSED, "group refused");
    }
    build_grp_reg_resp(e, 0U, 1005U, 300U, 0x04U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "grp_reg_resp failed")) {
        expect_true(r.result == DSD_UA_RESULT_FAILED, "group failed");
    }
    build_grp_reg_resp(e, 1U, 1006U, 300U, 0x01U, 500U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "grp_reg_resp visitor")) {
        expect_true(r.value2_valid && r.value2 == 500U, "visitor group of a group registration");
    }
    run_elements(opts, state, e, 56U, (uint8_t)NXDN_FRAME_CHANNEL_CONTROL);
    if (one_row(state, &r, "grp_reg_resp without the visitor field")) {
        expect_true(!r.value2_valid, "a 56-element group registration has no visitor group");
    }
    run_elements(opts, state, e, 55U, (uint8_t)NXDN_FRAME_CHANNEL_CONTROL);
    expect_no_row(state, "grp_reg_resp too short");
}

static void
test_rem_con_req(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;

    static const struct {
        uint8_t command;
        uint8_t kind;
    } commands[] = {{0U, DSD_UA_STUN}, {1U, DSD_UA_REVIVE}, {2U, DSD_UA_KILL}, {4U, DSD_UA_RADIO_MONITOR}};

    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        build_rem_con(e, 0x34U, 1U, commands[i].command, 1001U, 2002U, 0x8000U);
        run_cac(opts, state, e);
        if (one_row(state, &r, "rem_con_req unit")) {
            expect_kind(&r, commands[i].kind, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, "rem_con_req command");
            expect_identity(&r.subject, 2002U, DSD_UA_ID_RADIO, "the destination unit is the subject");
            expect_identity(&r.actor, 1001U, DSD_UA_ID_RADIO, "the source is the actor");
            expect_true(r.value2_valid && r.value2 == 0x8000U, "the control parameter is keyed");
        }
    }
    /* G/U 0: a group. */
    build_rem_con(e, 0x34U, 0U, 0U, 1001U, 300U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "rem_con_req group")) {
        expect_identity(&r.subject, 300U, DSD_UA_ID_GROUP, "a group-addressed stun names a group");
    }
    /* Another Control Parameter (Tx and Rx inhibit vs Tx inhibit) is another command. */
    build_rem_con(e, 0x34U, 1U, 0U, 1001U, 2002U, 0x0000U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "rem_con_req other parameter")) {
        expect_true(r.value2 == 0U, "the other parameter is a row of its own");
    }
    /* The controller's own Unit ID (TRS TC ID) is infrastructure. */
    build_rem_con(e, 0x34U, 1U, 2U, 0xFFF0U, 2003U, 0U);
    run_cac(opts, state, e);
    if (one_row(state, &r, "rem_con_req from the controller")) {
        expect_identity(&r.actor, 0xFFF0U, DSD_UA_ID_INFRASTRUCTURE, "TC ID actor");
    }
    /* Remote monitor addresses a unit only; reserved commands name nothing. */
    build_rem_con(e, 0x34U, 0U, 4U, 1001U, 300U, 0U);
    run_cac(opts, state, e);
    expect_no_row(state, "remote monitor of a group");
    build_rem_con(e, 0x34U, 1U, 3U, 1001U, 2002U, 0U);
    run_cac(opts, state, e);
    expect_no_row(state, "reserved command");
    run_elements(opts, state, e, 71U, (uint8_t)NXDN_FRAME_CHANNEL_CONTROL);
    expect_no_row(state, "rem_con_req too short");
}

/* TS 1-C Fig 15.21-1: every REM_CON_RESP carries the controlled unit [Bu] as its source and the requester [Au] as its
   destination; the controller's answer (send success), the target's (receive success) and the timeout (no response)
   differ only in Cause (SS). */
static void
test_rem_con_resp(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;

    static const struct {
        uint8_t cause;
        uint8_t result;
        const char* tag;
    } answers[] = {
        {0x02U, DSD_UA_RESULT_ACKNOWLEDGED, "controller answer"},
        {0x01U, DSD_UA_RESULT_ACKNOWLEDGED, "forwarded target response"},
        {0x15U, DSD_UA_RESULT_FAILED, "timeout"},
        {0x08U, DSD_UA_RESULT_REFUSED, "refused"},
    };

    for (size_t i = 0; i < sizeof answers / sizeof answers[0]; i++) {
        build_rem_con(e, 0x35U, 1U, 0U, 2002U, 1001U, answers[i].cause);
        run_cac(opts, state, e);
        if (one_row(state, &r, answers[i].tag)) {
            expect_kind(&r, DSD_UA_STUN, DSD_UA_ROLE_RESPONSE, answers[i].result, answers[i].tag);
            expect_identity(&r.subject, 2002U, DSD_UA_ID_RADIO, "the controlled unit is the subject");
            expect_identity(&r.recipient, 1001U, DSD_UA_ID_RADIO, "the requester is the recipient");
            expect_true(!r.actor.valid, "no actor on a response");
            expect_true(r.reason_set == DSD_UA_REASON_NXDN_SS && r.reason_code == answers[i].cause, "cause");
        }
    }
    build_rem_con(e, 0x35U, 1U, 4U, 2002U, 1001U, 0x01U);
    run_cac(opts, state, e);
    expect_no_row(state, "remote monitor response");
}

static void
test_channel_gates(dsd_opts* opts, dsd_state* state) {
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    build_reg_resp(e, 0U, 1010U, 200U, 0x01U, 0U, 0U);
    run_cac_crc(opts, state, e, 1U);
    expect_no_row(state, "a CAC whose CRC failed");
    /* Type-D (IDAS) uses the same type codes with other layouts (TS 1-E 6.4), and a traffic channel is not the Type-C
       control path. */
    run_elements(opts, state, e, CAC_ELEMENTS, (uint8_t)NXDN_FRAME_CHANNEL_TYPE_D);
    expect_no_row(state, "Type-D not parsed");
    build_rem_con(e, 0x34U, 1U, 0U, 1001U, 2002U, 0U);
    run_elements(opts, state, e, CAC_ELEMENTS, (uint8_t)NXDN_FRAME_CHANNEL_TRAFFIC);
    expect_no_row(state, "traffic channel remote control");
    /* A composite control channel only once its frames read as Type-C. */
    dsd_nxdn_forget_carrier_system(state);
    run_elements(opts, state, e, CAC_ELEMENTS, (uint8_t)NXDN_FRAME_CHANNEL_COMPOSITE);
    expect_no_row(state, "composite channel before Type-C");
    dsd_nxdn_note_type_c(state);
    run_elements(opts, state, e, CAC_ELEMENTS, (uint8_t)NXDN_FRAME_CHANNEL_COMPOSITE);
    (void)one_row(state, &r, "composite channel after Type-C");
}

/* Names at push (dsd_unit_activity_freeze_name()): a radio at home by its own ID, a visitor by its Visitor Unit ID, a
   roaming radio's home ID never (it may be another local radio's). The reception is Local system 77 (category 2 in
   the received identity's numbering, the location ID's category bits 01). */
static void
test_naming(dsd_opts* opts) {
    dsd_state* st = calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    g_instance = 0;
    g_rev = 0;
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    dsd_rxid_note_nxdn_location(st, 2U, 77U, 3U);
    (void)dsd_alias_learn_radio(NULL, st, NULL, 1001U, "HOME UNIT", NULL);
    (void)dsd_alias_learn_radio(NULL, st, NULL, 3001U, "VISITOR", NULL);
    build_reg_resp(e, 0U, 1001U, 200U, 0x01U, 0U, 0U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "home radio")) {
        expect_true(strcmp(r.name_frozen, "HOME UNIT") == 0, "a radio at home is named by its own ID");
    }
    build_reg_resp(e, 1U, 1001U, 200U, 0x01U, 3001U, 400U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "visitor")) {
        expect_true(strcmp(r.name_frozen, "VISITOR") == 0, "a visitor is named by its assigned Visitor Unit ID");
    }
    /* The subscriber's home is system 78, not the reception's 77: its home ID is not named. */
    build_reg_resp(e, 0U, 1001U, 200U, 0x01U, 0U, 0U);
    put_bits(e, 15, 78U, 17);
    run_cac(opts, st, e);
    if (one_row(st, &r, "roaming radio")) {
        expect_true(r.subject.home_valid && r.subject.home_b == 78U, "home system 78");
        expect_true(r.name_frozen[0] == '\0', "a radio away from home is not named by its home ID");
    }
    /* A group-addressed stun keeps its talkgroup's label, so the row still shows it once another system is heard. */
    dsd_tg_policy_entry fire;
    expect_true(dsd_tg_policy_make_exact_entry(300U, "A", "Fire", DSD_TG_POLICY_SOURCE_IMPORTED, &fire) == 0,
                "a group label row");
    (void)dsd_tg_policy_upsert_exact(st, &fire, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
    build_rem_con(e, 0x34U, 0U, 0U, 1001U, 300U, 0U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "group stun")) {
        expect_true(r.subject.subtype == DSD_UA_ID_GROUP && strcmp(r.name_frozen, "Fire") == 0,
                    "a group subject is stored with its talkgroup label");
    }
    dsd_state_ext_free_all(st);
    free(st);
}

/* An inter-system remote control message (CC Option I/I set, octet 1 bit 6, Table 6.5-7): its Location ID Option
   (octet 9 bits 7-3, Table 6.5-12: 10 the source's, 01 the destination's) and the Location ID after it, category bit
   first and without the site code (Fig 6.4-20 / 6.4-21, Fig 6.5-1), name the system the related unit belongs to. */
static void
set_inter_system(uint8_t* e, uint32_t related, uint32_t local_system) {
    put_bits(e, 9, 1U, 1);
    put_bits(e, 72, related, 2);
    put_bits(e, 77, 1U, 2);            /* category bits 01: Local */
    put_bits(e, 79, local_system, 17); /* a Local System Code is 17 bits */
}

/* A remote control response heard on system 77 from unit 2002 of system 78 is not the local unit 2002: no local name,
   and the same response from unit 2002 of system 79 is another row. A request whose destination is another system's
   unit is not named either; one of system 77's own units is. */
static void
test_rem_con_inter_system(dsd_opts* opts) {
    dsd_state* st = calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    g_instance = 0;
    g_rev = 0;
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    dsd_rxid_note_nxdn_location(st, 2U, 77U, 3U);
    (void)dsd_alias_learn_radio(NULL, st, NULL, 2002U, "LOCAL 2002", NULL);

    build_rem_con(e, 0x35U, 1U, 0U, 2002U, 1001U, 0x01U);
    set_inter_system(e, 2U, 78U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system response from system 78")) {
        expect_true(r.subject.home_valid && r.subject.home_b == 78U, "the source's Location ID is its home");
        expect_true(r.name_frozen[0] == '\0', "another system's unit 2002 is not named from the local lists");
    }
    build_rem_con(e, 0x35U, 1U, 0U, 2002U, 1001U, 0x01U);
    set_inter_system(e, 2U, 79U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system response from system 79")) {
        expect_true(r.repeat == 1U && r.subject.home_b == 79U, "a unit of another system is another row");
    }
    build_rem_con(e, 0x35U, 1U, 0U, 2002U, 1001U, 0x01U);
    set_inter_system(e, 2U, 77U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system response from the home system")) {
        expect_true(strcmp(r.name_frozen, "LOCAL 2002") == 0, "a unit of the reception's own system is named");
    }
    build_rem_con(e, 0x34U, 1U, 0U, 1001U, 2002U, 0x8000U);
    set_inter_system(e, 1U, 78U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system request to system 78")) {
        expect_true(r.subject.home_valid && r.subject.home_b == 78U && !r.actor.home_valid,
                    "the destination's Location ID is the subject's home");
        expect_true(r.name_frozen[0] == '\0', "another system's destination is not named from the local lists");
    }
    /* Inter-system with a reserved Location ID Option: which unit is foreign is unknown, so the subject is not named. */
    build_rem_con(e, 0x35U, 1U, 0U, 2002U, 1001U, 0x01U);
    set_inter_system(e, 3U, 78U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system, reserved option")) {
        expect_true(r.subject_nonlocal && r.name_frozen[0] == '\0', "an unqualified inter-system subject is nonlocal");
    }
    dsd_state_ext_free_all(st);
    free(st);
}

/* The newest row's subject name as every frontend shows it (the app-control view over the live ring). */
static void
view_newest_name(const dsd_state* state, char* out, size_t cap) {
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;
    dsd_app_unit_activity_row rows[16];
    const int n = dsd_app_unit_activity_rows(state, &cursor, rows, 16, &meta);
    uint64_t newest = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (rows[i].seq > newest) {
            newest = rows[i].seq;
            DSD_SNPRINTF(out, cap, "%s", rows[i].subject_name);
        }
    }
}

/* The newest row's target text as every frontend shows it (the app-control view over the live ring). */
static void
view_newest_target(const dsd_state* state, char* out, size_t cap) {
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;
    dsd_app_unit_activity_row rows[16];
    const int n = dsd_app_unit_activity_rows(state, &cursor, rows, 16, &meta);
    uint64_t newest = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (rows[i].seq > newest) {
            newest = rows[i].seq;
            DSD_SNPRINTF(out, cap, "%s", rows[i].target_text);
        }
    }
}

/* A visitor's registration names its home GroupID (TS 1-C §15.2): a talkgroup of the visitor's home system, which the
   serving system's list never labels (system 77's TG 300 "Local Fire" is another group). A unit at home is labelled
   as before. */
static void
test_visitor_home_group_not_labelled(dsd_opts* opts) {
    dsd_state* st = calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    g_instance = 0;
    g_rev = 0;
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    char target[160];
    dsd_rxid_note_nxdn_location(st, 2U, 77U, 3U);
    dsd_tg_policy_entry fire;
    expect_true(dsd_tg_policy_make_exact_entry(300U, "A", "Local Fire", DSD_TG_POLICY_SOURCE_IMPORTED, &fire) == 0,
                "a group label row");
    (void)dsd_tg_policy_upsert_exact(st, &fire, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);

    build_reg_resp(e, 1U, 1001U, 300U, 0x01U, 3001U, 400U);
    put_bits(e, 15, 88U, 17); /* the visitor's home: system 88 */
    run_cac(opts, st, e);
    (void)one_row(st, &r, "visitor registration");
    view_newest_target(st, target, sizeof target);
    expect_true(strcmp(target, "TG 300, visitor TG 400, as 3001") == 0, "a visitor's home group is not labelled here");

    build_grp_reg_resp(e, 1U, 1001U, 300U, 0x01U, 400U);
    run_cac(opts, st, e);
    (void)one_row(st, &r, "visitor group registration");
    view_newest_target(st, target, sizeof target);
    expect_true(strcmp(target, "TG 300, visitor TG 400") == 0, "a visitor's home group join is not labelled here");

    build_reg_resp(e, 0U, 1001U, 300U, 0x01U, 0U, 0U);
    run_cac(opts, st, e);
    (void)one_row(st, &r, "home registration");
    view_newest_target(st, target, sizeof target);
    expect_true(strcmp(target, "TG 300 Local Fire") == 0, "a unit at home has its group labelled");
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "last target text: '%s'\n", target);
    }
    dsd_state_ext_free_all(st);
    free(st);
}

/* A group stun between systems whose Location ID relates to the destination: a Location ID qualifies a unit, never a
   group, so the group is another system's, unqualified. System 77's TG 300 "Local Fire" never names it, at push or
   in the view; the same stun within the system is named. */
static void
test_inter_system_group_subject_not_labelled(dsd_opts* opts) {
    dsd_state* st = calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    g_instance = 0;
    g_rev = 0;
    uint8_t e[CAC_ELEMENTS];
    dsd_unit_activity r;
    char name[96];
    dsd_rxid_note_nxdn_location(st, 2U, 77U, 3U);
    dsd_tg_policy_entry fire;
    expect_true(dsd_tg_policy_make_exact_entry(300U, "A", "Local Fire", DSD_TG_POLICY_SOURCE_IMPORTED, &fire) == 0,
                "a group label row");
    (void)dsd_tg_policy_upsert_exact(st, &fire, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);

    build_rem_con(e, 0x34U, 0U, 0U, 1001U, 300U, 0U);
    set_inter_system(e, 1U, 78U);
    run_cac(opts, st, e);
    if (one_row(st, &r, "inter-system group stun")) {
        expect_true(r.subject.subtype == DSD_UA_ID_GROUP && r.subject_nonlocal && r.name_frozen[0] == '\0',
                    "an unqualified inter-system group is nonlocal and stored unnamed");
    }
    view_newest_name(st, name, sizeof name);
    expect_true(name[0] == '\0', "the view does not label an inter-system group from the local list");

    build_rem_con(e, 0x34U, 0U, 0U, 1001U, 300U, 1U);
    run_cac(opts, st, e);
    (void)one_row(st, &r, "group stun within the system");
    view_newest_name(st, name, sizeof name);
    expect_true(strcmp(name, "Local Fire") == 0, "a group of the reception's own system is labelled");
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "last name: '%s'\n", name);
    }
    dsd_state_ext_free_all(st);
    free(st);
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    test_reg_resp(opts, state);
    test_reg_resp_visitor(opts, state);
    test_reg_c_and_group(opts, state);
    test_rem_con_req(opts, state);
    test_rem_con_resp(opts, state);
    test_channel_gates(opts, state);
    test_naming(opts);
    test_rem_con_inter_system(opts);
    test_visitor_home_group_not_labelled(opts);
    test_inter_system_group_subject_not_labelled(opts);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "NXDN_UNIT_ACTIVITY: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "NXDN_UNIT_ACTIVITY: PASS\n");
    return 0;
}
