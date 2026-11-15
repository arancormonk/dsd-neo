// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR unit activity rows (see dmr_unit_activity.h). Sources: ETSI TS 102 361-4 V1.12.1 Table 7.23 (C_ACKD layout),
 * Table 7.41 (Response_Info: the acceptance pattern of 0x65, G/I for the other reasons), clause 7.2.8 Tables 7.42-7.45
 * (the reason classes), Table 6.9 / clause 6.4.4 (REGI answers a registration), clause 6.4.6 (a deregistration is
 * answered with the same 0x62), Tables 6.11 / 6.17 / 6.19 / 6.23 / 6.27 (the C_AHOY commands) and Table A.8 (gateway
 * addresses); the MOTOTRBO ARS specification v02.00 section 3.4 (ARS PDU types).
 */

#include "dmr_unit_activity.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>

#include <stdint.h>
#include "dmr_reason.h"

enum {
    DMR_UA_REGI = 0xFFFEC6,
    /* Table A.8: the gateway identifiers PSTNI (FFFEC0) to TATTSI (FFFED7), and ALLMSIDL / ALLMSIDZ / ALLMSID. */
    DMR_UA_GATEWAY_FIRST = 0xFFFEC0,
    DMR_UA_GATEWAY_LAST = 0xFFFED7,
    DMR_UA_ALL_MS_FIRST = 0xFFFFFD,
    DMR_UA_ADDRESS_MAX = 0xFFFFFF,
    DMR_UA_REASON_REG_ACCEPTED = 0x62,
    DMR_UA_REASON_REG_SUBSCRIPTION = 0x65,
    DMR_UA_REASON_REG_REFUSED = 0x2A,
    DMR_UA_REASON_REG_DENIED = 0x2B,
    /* MOTOTRBO ARS spec 3.4: device registration, device deregistration, user registration, user deregistration. */
    DMR_UA_ARS_DEV_REG = 0x00,
    DMR_UA_ARS_DEV_DEREG = 0x01,
    DMR_UA_ARS_USER_REG = 0x05,
    DMR_UA_ARS_USER_DEREG = 0x06,
};

static int
dmr_ua_is_gateway(uint32_t id) {
    return (id >= DMR_UA_GATEWAY_FIRST && id <= DMR_UA_GATEWAY_LAST)
           || (id >= DMR_UA_ALL_MS_FIRST && id <= DMR_UA_ADDRESS_MAX);
}

/* An address field as an identity: a gateway is infrastructure, else a talkgroup when @p group, else a radio. 0 names
 * nobody. */
static dsd_ua_identity
dmr_ua_identity(uint32_t id, uint8_t group) {
    dsd_ua_identity out;
    DSD_MEMSET(&out, 0, sizeof out);
    if (id == 0U) {
        return out;
    }
    out.id = id;
    out.valid = 1;
    if (dmr_ua_is_gateway(id)) {
        out.subtype = DSD_UA_ID_INFRASTRUCTURE;
    } else {
        out.subtype = group ? DSD_UA_ID_GROUP : DSD_UA_ID_RADIO;
    }
    return out;
}

static dsd_unit_activity
dmr_ua_row(const dsd_opts* opts, const dsd_state* state, uint8_t kind, uint8_t role, uint8_t result) {
    dsd_unit_activity row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.protocol = DSD_UA_PROTO_DMR;
    row.kind = kind;
    row.role = role;
    row.result = result;
    row.slot = (uint8_t)(state->currentslot & 1);
    row.freq_hz = (int64_t)dsd_opts_tuned_freq_hz(opts);
    return row;
}

/* The subject's name as the receiver knows it now (the core's one naming rule), and the push. */
static void
dmr_ua_push(dsd_state* state, dsd_unit_activity* row) {
    dsd_unit_activity_freeze_name(state, row);
    (void)dsd_unit_activity_push(state, row);
}

/* Tables 7.42 / 6.9: what REGI acknowledges. 0 for an acknowledgement this log does not show. */
static int
dmr_ua_ackd_accept_row(const dmr_ua_ackd_fields* f, dsd_unit_activity* row) {
    if (f->source != DMR_UA_REGI) {
        return 0;
    }
    if (f->reason == DMR_UA_REASON_REG_ACCEPTED) {
        /* A registration and a deregistration are both answered with 0x62 (clause 6.4.6): the row says acknowledged
           and carries no reason, whose text would say accepted. */
        row->kind = DSD_UA_REGISTRATION;
        row->role = DSD_UA_ROLE_ACK;
        row->result = DSD_UA_RESULT_ACKNOWLEDGED;
    } else if (f->reason == DMR_UA_REASON_REG_SUBSCRIPTION) {
        /* Response_Info is the index pattern of the accepted talkgroups (Table 7.41); it names no talkgroup. */
        const uint32_t mask = (uint32_t)(f->response_info & 0x7FU);
        row->kind = DSD_UA_GROUP_AFFILIATION;
        row->role = DSD_UA_ROLE_RESPONSE;
        row->result = mask != 0U ? DSD_UA_RESULT_ACCEPTED : DSD_UA_RESULT_REFUSED;
        row->value = mask;
        row->value_valid = 1;
        row->reason_set = DSD_UA_REASON_DMR_TIER3;
        row->reason_code = f->reason;
    } else {
        return 0;
    }
    row->subject = dmr_ua_identity(f->target, 0U);
    row->actor = dmr_ua_identity(f->source, 0U);
    return 1;
}

/* A refusal, queued or wait answer (Tables 7.43-7.45). Response_Info bit 16 is G/I: 1 when the target address is a
   talkgroup (Table 7.41). The additional information is the request's destination, of unknown kind, or a gateway. */
static void
dmr_ua_ackd_other_row(const dmr_ua_ackd_fields* f, dmr_tier3_reason_class cls, dsd_unit_activity* row) {
    const uint8_t group = (uint8_t)((f->response_info >> 6) & 1U);
    row->role = DSD_UA_ROLE_RESPONSE;
    if (cls == DMR_TIER3_REASON_QACK) {
        row->kind = DSD_UA_QUEUED;
        row->result = DSD_UA_RESULT_QUEUED;
    } else if (cls == DMR_TIER3_REASON_WACK) {
        row->kind = DSD_UA_WAITING;
        row->result = DSD_UA_RESULT_NA;
    } else if (f->source == DMR_UA_REGI && f->reason == DMR_UA_REASON_REG_REFUSED) {
        row->kind = DSD_UA_REGISTRATION;
        row->result = DSD_UA_RESULT_REFUSED;
    } else if (f->source == DMR_UA_REGI && f->reason == DMR_UA_REASON_REG_DENIED) {
        row->kind = DSD_UA_REGISTRATION;
        row->result = DSD_UA_RESULT_DENIED;
    } else {
        row->kind = DSD_UA_DENY;
        row->result = DSD_UA_RESULT_DENIED;
    }
    row->reason_set = DSD_UA_REASON_DMR_TIER3;
    row->reason_code = f->reason;
    row->subject = dmr_ua_identity(f->target, group);
    if (dmr_ua_is_gateway(f->source)) {
        row->actor = dmr_ua_identity(f->source, 0U);
    } else if (f->source != 0U) {
        row->recipient.id = f->source;
        row->recipient.subtype = DSD_UA_ID_UNKNOWN;
        row->recipient.valid = 1;
    }
}

void
dmr_ua_c_ackd(const dsd_opts* opts, dsd_state* state, const dmr_ua_ackd_fields* fields) {
    if (state == NULL || fields == NULL || fields->target == 0U) {
        return;
    }
    dsd_unit_activity row = dmr_ua_row(opts, state, 0U, 0U, 0U);
    const dmr_tier3_reason_class cls = dmr_tier3_reason_class_of(fields->reason);
    if (cls == DMR_TIER3_REASON_ACK) {
        if (!dmr_ua_ackd_accept_row(fields, &row)) {
            return;
        }
    } else {
        dmr_ua_ackd_other_row(fields, cls, &row);
    }
    dmr_ua_push(state, &row);
}

void
dmr_ua_c_ahoy(const dsd_opts* opts, dsd_state* state, int kind, uint8_t group, uint32_t target, uint32_t source) {
    if (state == NULL || target == 0U) {
        return;
    }
    dsd_unit_activity row = dmr_ua_row(opts, state, 0U, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    switch (kind) {
        case DMR_AHOY_STUN: row.kind = DSD_UA_STUN; break;
        case DMR_AHOY_REVIVE: row.kind = DSD_UA_REVIVE; break;
        case DMR_AHOY_KILL: row.kind = DSD_UA_KILL; break;
        case DMR_AHOY_RADIO_CHECK: row.kind = DSD_UA_RADIO_CHECK; break;
        case DMR_AHOY_AUTHENTICATION: row.kind = DSD_UA_AUTHENTICATION; break;
        case DMR_AHOY_SUBSCRIPTION_REQ: row.kind = DSD_UA_REGISTRATION; break;
        default: return;
    }
    row.subject = dmr_ua_identity(target, group);
    /* The source field of an authentication challenge is the challenge (Table 6.17), never an identity; every other
       kind names its gateway there (STUNI, KILLI, TSI, TATTSI). */
    if (kind != DMR_AHOY_AUTHENTICATION) {
        row.actor = dmr_ua_identity(source, 0U);
    }
    dmr_ua_push(state, &row);
}

void
dmr_ua_ars(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t pdu_type, uint32_t source) {
    if (state == NULL || source == 0U) {
        return;
    }
    uint8_t kind;
    switch (pdu_type) {
        case DMR_UA_ARS_DEV_REG:
        case DMR_UA_ARS_USER_REG: kind = DSD_UA_REGISTRATION; break;
        case DMR_UA_ARS_DEV_DEREG:
        case DMR_UA_ARS_USER_DEREG: kind = DSD_UA_DEREGISTRATION; break;
        default: return;
    }
    dsd_unit_activity row = dmr_ua_row(opts, state, kind, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    row.slot = (uint8_t)(slot & 1U);
    row.subject = dmr_ua_identity(source, 0U);
    /* A device and a user registration of one radio are two reports. */
    row.value2 = pdu_type;
    row.value2_valid = 1;
    dmr_ua_push(state, &row);
}
