// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN Type-C unit activity messages (see nxdn_unit_activity.h). Layouts from NXDN TS 1-A v1.3: Fig 6.4-45 REG_RESP,
 * Fig 6.4-47 REG_C_RESP, Fig 6.4-50 GRP_REG_RESP, Fig 6.4-20 REM_CON_REQ, Fig 6.4-21 REM_CON_RESP; Fig 6.5-1 / Table
 * 6.5-1 (Location ID), Fig 6.5-2 / 6.5-3 (Registration Option, Group Registration Option), Tables 6.5-2 / 6.5-3 (Unit
 * and Group ID values), Fig 6.5-14 (Cause), Tables 6.5-22 / 6.5-23 (Control Command and Parameter), Tables 6.5-27 /
 * 6.5-29 (Cause MM / SS). The roles follow TS 1-C clause 13.5 and Fig 15.21-1 (a REM_CON_RESP always carries the
 * controlled unit as its source and the requester as its destination). Every octet n starts at element 8n, MSB first;
 * octet 0 holds F1, F2 and the message type.
 */

#include "nxdn_unit_activity.h"

#include <dsd-neo/core/nxdn_channel_access.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity_site.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Mandatory lengths in elements, and with the optional visitor fields. */
enum {
    NXDN_UA_REG_RESP_BITS = 72U,
    NXDN_UA_REG_RESP_VISITOR_BITS = 104U,
    NXDN_UA_REG_C_RESP_BITS = 56U,
    NXDN_UA_GRP_REG_RESP_BITS = 56U,
    NXDN_UA_GRP_REG_RESP_VISITOR_BITS = 72U,
    NXDN_UA_REM_CON_BITS = 72U,
};

/* Table 6.5-22 Control Command. */
enum {
    NXDN_UA_CMD_STUN = 0,
    NXDN_UA_CMD_REVIVAL = 1,
    NXDN_UA_CMD_KILL = 2,
    NXDN_UA_CMD_REMOTE_MONITOR = 4,
};

static uint32_t
nxdn_ua_bits(const uint8_t* e, size_t pos, size_t count) {
    uint32_t v = 0U;
    for (size_t i = 0; i < count; i++) {
        v = (v << 1) | (uint32_t)(e[pos + i] & 1U);
    }
    return v;
}

static uint32_t
nxdn_ua_octets(const uint8_t* e, size_t octet, size_t count) {
    return nxdn_ua_bits(e, octet * 8U, count * 8U);
}

/* The Type-C control channel path: a CAC, or a composite control channel's frames once they read as Type-C. */
static int
nxdn_ua_type_c_path(const dsd_state* state) {
    return state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_CONTROL
           || (state->nxdn_frame_channel == (uint8_t)NXDN_FRAME_CHANNEL_COMPOSITE && dsd_nxdn_type_c(state));
}

static int
nxdn_ua_accept(const dsd_state* state, const uint8_t* elements, size_t elements_bits, size_t min_bits,
               const char* name) {
    if (state == NULL || elements == NULL || !nxdn_ua_type_c_path(state)) {
        return 0;
    }
    if (elements_bits < min_bits) {
        DSD_FPRINTF(stderr, " %s Too Short (%zu bits); ", name, elements_bits);
        return 0;
    }
    return 1;
}

/* Table 6.5-2: 0 is the null Unit ID, FFF0-FFFF the TC, PSTN and special IDs; the rest are standard unit IDs. */
static dsd_ua_identity
nxdn_ua_unit(uint32_t id) {
    dsd_ua_identity out;
    DSD_MEMSET(&out, 0, sizeof out);
    if (id != 0U) {
        out.id = id;
        out.valid = 1;
        out.subtype = id >= 0xFFF0U ? DSD_UA_ID_INFRASTRUCTURE : DSD_UA_ID_RADIO;
    }
    return out;
}

/* Table 6.5-3: 0 is the null Group ID. */
static dsd_ua_identity
nxdn_ua_group_identity(uint32_t id) {
    dsd_ua_identity out;
    DSD_MEMSET(&out, 0, sizeof out);
    if (id != 0U) {
        out.id = id;
        out.valid = 1;
        out.subtype = DSD_UA_ID_GROUP;
    }
    return out;
}

/* The subscriber's home: a Location ID without its Site Code (19 elements from octet 1 bit 2), category bits first,
   then the system code of the category's width (Table 6.5-1: Global 10, Regional 14, Local 17 bits), in the scope
   encoding of a Type-C reception (dsd_ua_scope: a = 0x100 | category 0 Global / 1 Regional / 2 Local, b = system). */
static void
nxdn_ua_set_home(dsd_ua_identity* id, const uint8_t* e) {
    static const uint8_t k_width[3] = {10U, 17U, 14U}; /* category bits 00, 01, 10 */
    static const uint8_t k_category[3] = {0U, 2U, 1U}; /* the received identity's numbering */
    const uint32_t category_bits = nxdn_ua_bits(e, 13U, 2U);
    if (category_bits > 2U) {
        return; /* 11 is reserved */
    }
    const uint32_t sys = nxdn_ua_bits(e, 15U, k_width[category_bits]);
    const uint8_t category = k_category[category_bits];
    /* Site 1 is a valid site code in every category: only the system code is checked here. */
    if (!dsd_nxdn_location_valid(category, sys, 1U)) {
        return;
    }
    id->home_kind = DSD_UA_SCOPE_NXDN;
    id->home_a = 0x100U | category;
    id->home_b = sys;
    id->home_valid = 1;
}

/* Cause (MM) (Table 6.5-27): the registration's result. Location accepted with the group failed or refused (0x04,
   0x05) is a registration accepted; the reason says the rest. */
static uint8_t
nxdn_ua_mm_result(uint8_t cause, int group_result) {
    switch (cause) {
        case 0x01U: return DSD_UA_RESULT_ACCEPTED;
        case 0x04U: return group_result ? DSD_UA_RESULT_FAILED : DSD_UA_RESULT_ACCEPTED;
        case 0x05U: return group_result ? DSD_UA_RESULT_REFUSED : DSD_UA_RESULT_ACCEPTED;
        case 0x06U: return DSD_UA_RESULT_FAILED;
        case 0x08U: return DSD_UA_RESULT_REFUSED;
        default: break;
    }
    /* Types 101 resource unavailable, 110 service unavailable, 111 procedure error. */
    return (cause >= 0x50U && cause <= 0x7FU) ? DSD_UA_RESULT_FAILED : DSD_UA_RESULT_UNKNOWN;
}

/* Cause (SS) (Table 6.5-29) of a remote-control response: what the answer states, never that the command took
   effect. ACK (receive / send success) is an acknowledgement, NACK a refusal, 0x10 accepted, 0x15 no response from the
   called unit a failure, the other normal-class values a denial and the resource, service and procedure classes a
   failure. */
static uint8_t
nxdn_ua_ss_result(uint8_t cause) {
    const uint8_t type = (uint8_t)((cause >> 4) & 7U);
    if (cause == 0x01U || cause == 0x02U) {
        return DSD_UA_RESULT_ACKNOWLEDGED;
    }
    if (cause >= 0x08U && cause <= 0x0AU) {
        return DSD_UA_RESULT_REFUSED;
    }
    if (cause == 0x10U) {
        return DSD_UA_RESULT_ACCEPTED;
    }
    if (cause == 0x15U || type >= 5U) {
        return DSD_UA_RESULT_FAILED;
    }
    return (type == 1U || type == 2U) ? DSD_UA_RESULT_DENIED : DSD_UA_RESULT_UNKNOWN;
}

static dsd_unit_activity
nxdn_ua_row(const dsd_opts* opts, uint8_t kind, uint8_t role) {
    dsd_unit_activity row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.protocol = DSD_UA_PROTO_NXDN;
    row.kind = kind;
    row.role = role;
    row.freq_hz = (int64_t)dsd_opts_tuned_freq_hz(opts);
    return row;
}

/* The subject's name as the receiver knows it now (the core's one naming rule: a visitor by its assigned Visitor Unit
   ID, a radio by its own ID only at home), and the push. */
static void
nxdn_ua_push(dsd_state* state, dsd_unit_activity* row) {
    dsd_unit_activity_freeze_name(state, row);
    (void)dsd_unit_activity_push(state, row);
}

static void
nxdn_ua_print_cause(uint8_t reason_set, uint8_t cause) {
    const char* text = dsd_ua_reason_text(reason_set, cause);
    if (text != NULL) {
        DSD_FPRINTF(stderr, "Cause: %02X (%s); ", cause, text);
    } else {
        DSD_FPRINTF(stderr, "Cause: %02X; ", cause);
    }
}

static void
nxdn_ua_reg_resp(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_ua_accept(state, elements, elements_bits, NXDN_UA_REG_RESP_BITS, "REG_RESP")) {
        return;
    }
    const uint8_t visitor = (uint8_t)elements[9]; /* Registration Option H/V */
    const uint32_t unit = nxdn_ua_octets(elements, 4U, 2U);
    const uint32_t group = nxdn_ua_octets(elements, 6U, 2U);
    const uint8_t cause = (uint8_t)(nxdn_ua_octets(elements, 8U, 1U) & 0x7FU);
    dsd_unit_activity row = nxdn_ua_row(opts, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE);
    row.result = nxdn_ua_mm_result(cause, 0);
    row.subject = nxdn_ua_unit(unit);
    nxdn_ua_set_home(&row.subject, elements);
    row.group = group;
    row.group_valid = (uint8_t)(group != 0U);
    row.reason_set = DSD_UA_REASON_NXDN_MM;
    row.reason_code = cause;
    DSD_FPRINTF(stderr, " - Unit: %u; Group: %u; ", (unsigned)unit, (unsigned)group);
    if ((visitor & 1U) && elements_bits >= NXDN_UA_REG_RESP_VISITOR_BITS) {
        const uint32_t visitor_unit = nxdn_ua_octets(elements, 9U, 2U);
        const uint32_t visitor_group = nxdn_ua_octets(elements, 11U, 2U);
        row.assigned = nxdn_ua_unit(visitor_unit);
        row.value2 = visitor_group;
        row.value2_valid = (uint8_t)(visitor_group != 0U);
        DSD_FPRINTF(stderr, "Visitor Unit: %u; Visitor Group: %u; ", (unsigned)visitor_unit, (unsigned)visitor_group);
    }
    nxdn_ua_print_cause(DSD_UA_REASON_NXDN_MM, cause);
    if (row.subject.valid) {
        nxdn_ua_push(state, &row);
    }
}

static void
nxdn_ua_reg_c_resp(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_ua_accept(state, elements, elements_bits, NXDN_UA_REG_C_RESP_BITS, "REG_C_RESP")) {
        return;
    }
    const uint32_t unit = nxdn_ua_octets(elements, 4U, 2U);
    const uint8_t cause = (uint8_t)(nxdn_ua_octets(elements, 6U, 1U) & 0x7FU);
    /* The standard names no "clear accepted" value (0x01 reads "Registration is accepted"), and no second source says
       which cause a successful clear carries: a success stays UNKNOWN, a refusal or failure is what its class says,
       and the row is a logout response whatever the cause. */
    dsd_unit_activity row = nxdn_ua_row(opts, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_RESPONSE);
    const uint8_t result = nxdn_ua_mm_result(cause, 0);
    row.result = result == DSD_UA_RESULT_ACCEPTED ? (uint8_t)DSD_UA_RESULT_UNKNOWN : result;
    row.subject = nxdn_ua_unit(unit);
    nxdn_ua_set_home(&row.subject, elements);
    row.reason_set = DSD_UA_REASON_NXDN_MM;
    row.reason_code = cause;
    DSD_FPRINTF(stderr, " - Unit: %u; ", (unsigned)unit);
    nxdn_ua_print_cause(DSD_UA_REASON_NXDN_MM, cause);
    if (row.subject.valid) {
        nxdn_ua_push(state, &row);
    }
}

static void
nxdn_ua_grp_reg_resp(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_ua_accept(state, elements, elements_bits, NXDN_UA_GRP_REG_RESP_BITS, "GRP_REG_RESP")) {
        return;
    }
    const uint8_t visitor = (uint8_t)elements[9]; /* Group Registration Option H/V */
    const uint32_t unit = nxdn_ua_octets(elements, 2U, 2U);
    const uint32_t group = nxdn_ua_octets(elements, 4U, 2U);
    const uint8_t cause = (uint8_t)(nxdn_ua_octets(elements, 6U, 1U) & 0x7FU);
    dsd_unit_activity row = nxdn_ua_row(opts, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE);
    row.result = nxdn_ua_mm_result(cause, 1);
    row.subject = nxdn_ua_unit(unit);
    row.group = group;
    row.group_valid = (uint8_t)(group != 0U);
    row.reason_set = DSD_UA_REASON_NXDN_MM;
    row.reason_code = cause;
    DSD_FPRINTF(stderr, " - Unit: %u; Group: %u; ", (unsigned)unit, (unsigned)group);
    if ((visitor & 1U) && elements_bits >= NXDN_UA_GRP_REG_RESP_VISITOR_BITS) {
        const uint32_t visitor_group = nxdn_ua_octets(elements, 7U, 2U);
        row.value2 = visitor_group;
        row.value2_valid = (uint8_t)(visitor_group != 0U);
        DSD_FPRINTF(stderr, "Visitor Group: %u; ", (unsigned)visitor_group);
    }
    nxdn_ua_print_cause(DSD_UA_REASON_NXDN_MM, cause);
    if (row.subject.valid) {
        nxdn_ua_push(state, &row);
    }
}

/* Table 6.5-22: the command a Control Command names (0 for a reserved one); Remote Monitor only when allowed. */
static uint8_t
nxdn_ua_command_kind(uint32_t command, int allow_monitor) {
    switch (command) {
        case NXDN_UA_CMD_STUN: return DSD_UA_STUN;
        case NXDN_UA_CMD_REVIVAL: return DSD_UA_REVIVE;
        case NXDN_UA_CMD_KILL: return DSD_UA_KILL;
        case NXDN_UA_CMD_REMOTE_MONITOR: return allow_monitor ? (uint8_t)DSD_UA_RADIO_MONITOR : 0U;
        default: return 0U;
    }
}

static const char*
nxdn_ua_command_name(uint32_t command) {
    switch (command) {
        case NXDN_UA_CMD_STUN: return "Stun";
        case NXDN_UA_CMD_REVIVAL: return "Revival";
        case NXDN_UA_CMD_KILL: return "Kill";
        case NXDN_UA_CMD_REMOTE_MONITOR: return "Remote Monitor";
        default: return "Reserved";
    }
}

static void
nxdn_ua_rem_con_req(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_ua_accept(state, elements, elements_bits, NXDN_UA_REM_CON_BITS, "REM_CON_REQ")) {
        return;
    }
    const uint8_t unit_addressed = (uint8_t)(elements[16] & 1U); /* G/U: 1 a Unit ID in octets 5-6 */
    const uint32_t command = nxdn_ua_bits(elements, 19U, 5U);
    const uint32_t source = nxdn_ua_octets(elements, 3U, 2U);
    const uint32_t destination = nxdn_ua_octets(elements, 5U, 2U);
    const uint32_t parameter = nxdn_ua_octets(elements, 7U, 2U);
    DSD_FPRINTF(stderr, " - %s; Source: %u; %s: %u; Parameter: %04X; ", nxdn_ua_command_name(command), (unsigned)source,
                unit_addressed ? "Unit" : "Group", (unsigned)destination, (unsigned)parameter);
    /* Remote Monitor addresses a unit only (Table 6.5-22). */
    const uint8_t kind = nxdn_ua_command_kind(command, unit_addressed);
    if (kind == 0U) {
        return;
    }
    dsd_unit_activity row = nxdn_ua_row(opts, kind, DSD_UA_ROLE_COMMAND);
    row.result = DSD_UA_RESULT_NA;
    row.subject = unit_addressed ? nxdn_ua_unit(destination) : nxdn_ua_group_identity(destination);
    row.actor = nxdn_ua_unit(source);
    /* The command argument (Table 6.5-23: stun mode, monitor mode and duration) tells two commands apart. */
    row.value2 = parameter;
    row.value2_valid = 1;
    if (row.subject.valid) {
        nxdn_ua_push(state, &row);
    }
}

static void
nxdn_ua_rem_con_resp(const dsd_opts* opts, dsd_state* state, const uint8_t* elements, size_t elements_bits) {
    if (!nxdn_ua_accept(state, elements, elements_bits, NXDN_UA_REM_CON_BITS, "REM_CON_RESP")) {
        return;
    }
    const uint32_t command = nxdn_ua_bits(elements, 19U, 5U);
    const uint32_t source = nxdn_ua_octets(elements, 3U, 2U);
    const uint32_t destination = nxdn_ua_octets(elements, 5U, 2U);
    const uint8_t cause = (uint8_t)(nxdn_ua_octets(elements, 8U, 1U) & 0x7FU);
    DSD_FPRINTF(stderr, " - %s; Source: %u; Destination: %u; ", nxdn_ua_command_name(command), (unsigned)source,
                (unsigned)destination);
    nxdn_ua_print_cause(DSD_UA_REASON_NXDN_SS, cause);
    /* A response to a remote monitor has no row: the log names it only as requested. */
    const uint8_t kind = nxdn_ua_command_kind(command, 0);
    if (kind == 0U) {
        return;
    }
    dsd_unit_activity row = nxdn_ua_row(opts, kind, DSD_UA_ROLE_RESPONSE);
    row.result = nxdn_ua_ss_result(cause);
    row.subject = nxdn_ua_unit(source);
    row.recipient = nxdn_ua_unit(destination);
    row.reason_set = DSD_UA_REASON_NXDN_SS;
    row.reason_code = cause;
    if (row.subject.valid) {
        nxdn_ua_push(state, &row);
    }
}

int
nxdn_ua_dispatch(const dsd_opts* opts, dsd_state* state, uint8_t message_type, const uint8_t* elements,
                 size_t elements_bits) {
    switch (message_type) {
        case 0x20U: nxdn_ua_reg_resp(opts, state, elements, elements_bits); return 1;
        case 0x22U: nxdn_ua_reg_c_resp(opts, state, elements, elements_bits); return 1;
        case 0x24U: nxdn_ua_grp_reg_resp(opts, state, elements, elements_bits); return 1;
        case 0x34U: nxdn_ua_rem_con_req(opts, state, elements, elements_bits); return 1;
        case 0x35U: nxdn_ua_rem_con_resp(opts, state, elements, elements_bits); return 1;
        default: return 0;
    }
}
