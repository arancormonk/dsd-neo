// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 unit activity rows (see p25_unit_activity.h). Sources: TIA-102.AABC-D §2.3.15 Table 2.3.15-1 (extended function
 * class $00), §2.3.29 (status), §2.3.30 Table 2.3.30-1 (unit addresses), §6.2.5 / §6.2.14 (deny / queued), §6.2.8
 * (group affiliation response, GAV), §6.2.21 (unit registration response, RV), §6.2.22 (deregistration ack);
 * TIA-102.BBAD-A for the Phase 2 forms; TIA-102.AABF-D §7.3.19 / §7.3.31 (link control message update).
 */

#include "p25_unit_activity.h"

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>

#include <stdint.h>

enum {
    P25_UA_FIRST_SPECIAL = 0xFFFFFC, /* FNE use, System Default, Registration Default, ALL units */
    P25_UA_ADDRESS_MASK = 0xFFFFFF,
    P25_UA_EXT_ACK = 0x80,
    P25_UA_EXT_CHECK = 0x00,
    P25_UA_EXT_DETACH = 0x7D,
    P25_UA_EXT_UNINHIBIT = 0x7E,
    P25_UA_EXT_INHIBIT = 0x7F,
};

/* Registration Value / Group Affiliation Value (TIA-102.AABC-D §6.2.21.1, §6.2.8.1): 0 accept, 1 fail, 2 deny,
   3 refused. */
static uint8_t
p25_ua_rv_result(int value) {
    switch (value & 3) {
        case 0: return DSD_UA_RESULT_ACCEPTED;
        case 1: return DSD_UA_RESULT_FAILED;
        case 2: return DSD_UA_RESULT_DENIED;
        default: return DSD_UA_RESULT_REFUSED;
    }
}

/* A unit address as an identity: 0 names nobody, the special addresses are infrastructure, the rest radios. */
static dsd_ua_identity
p25_ua_identity(uint32_t id) {
    dsd_ua_identity out;
    DSD_MEMSET(&out, 0, sizeof out);
    id &= P25_UA_ADDRESS_MASK;
    if (id == 0U) {
        return out;
    }
    out.id = id;
    out.valid = 1U;
    out.subtype = id >= P25_UA_FIRST_SPECIAL ? DSD_UA_ID_INFRASTRUCTURE : DSD_UA_ID_RADIO;
    return out;
}

/* A radio of a known home system (an SUID). The home goes only on a radio and only when the message named it. */
static dsd_ua_identity
p25_ua_identity_home(uint32_t id, const p25_home_id* home) {
    dsd_ua_identity out = p25_ua_identity(id);
    if (out.valid && out.subtype == DSD_UA_ID_RADIO && home && home->valid) {
        out.home_kind = DSD_UA_SCOPE_P25;
        out.home_a = home->wacn;
        out.home_b = home->sys;
        out.home_valid = 1U;
    }
    return out;
}

static dsd_ua_identity
p25_ua_unit_identity(const p25_ua_unit* u) {
    if (!u) {
        dsd_ua_identity none;
        DSD_MEMSET(&none, 0, sizeof none);
        return none;
    }
    return p25_ua_identity_home(u->id, &u->home);
}

/* 1 when the row may be pushed: a state and a message whose CRC passed. */
static int
p25_ua_origin_ok(const p25_ua_origin* o) {
    return o && o->state && o->crc_ok;
}

static void
p25_ua_row_init(const p25_ua_origin* o, dsd_unit_activity* row, uint8_t kind, uint8_t role, uint8_t result) {
    DSD_MEMSET(row, 0, sizeof *row);
    row->protocol = o->phase2 ? DSD_UA_PROTO_P25P2 : DSD_UA_PROTO_P25P1;
    row->kind = kind;
    row->role = role;
    row->result = result;
    row->slot = (uint8_t)(o->slot & 1U);
    row->freq_hz = (int64_t)dsd_opts_tuned_freq_hz(o->opts);
}

static void
p25_ua_push(const p25_ua_origin* o, dsd_unit_activity* row) {
    if (!row->subject.valid) {
        return; /* a row names whom it is about */
    }
    dsd_unit_activity_freeze_name(o->state, row);
    (void)dsd_unit_activity_push(o->state, row);
}

static void
p25_ua_set_group(dsd_unit_activity* row, uint32_t group) {
    if (group != 0U) {
        row->group = group;
        row->group_valid = 1U;
    }
}

static void
p25_ua_set_value(dsd_unit_activity* row, uint32_t value) {
    row->value = value;
    row->value_valid = 1U;
}

/* 1 when @p home is the serving system's: its WACN and System ID the receiver's, or, for a home whose WACN is not
   known yet (the abbreviated registration response before the site's WACN), its System ID. */
static int
p25_ua_home_is_serving(const dsd_state* state, const p25_home_id* home) {
    if (home->sys != (uint16_t)state->p2_sysid) {
        return 0;
    }
    return !home->valid || home->wacn == (uint32_t)state->p2_wacn;
}

/* The assigned address, kept when it says more than the home identity does. */
static void
p25_ua_set_assigned(const dsd_state* state, dsd_unit_activity* row, const p25_home_id* home, uint32_t assigned) {
    if (assigned == 0U || (assigned == home->id && p25_ua_home_is_serving(state, home))) {
        return;
    }
    row->assigned = p25_ua_identity(assigned);
}

/* The subject from @p u: its ID with its home, and its WUID on the sending system as the assigned identity. */
static void
p25_ua_set_subject_unit(const dsd_state* state, dsd_unit_activity* row, const p25_ua_unit* u) {
    row->subject = p25_ua_unit_identity(u);
    if (u && u->local != 0U && row->subject.valid) {
        if (u->home.valid) {
            p25_ua_set_assigned(state, row, &u->home, u->local);
        } else if (u->local != u->id) {
            row->assigned = p25_ua_identity(u->local);
        }
    }
}

void
p25_ua_registration_response(const p25_ua_origin* o, int rv, const p25_home_id* home, uint32_t assigned) {
    if (!p25_ua_origin_ok(o) || !home) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, p25_ua_rv_result(rv));
    row.subject = p25_ua_identity_home(home->id, home);
    if ((rv & 3) == 0) {
        p25_ua_set_assigned(o->state, &row, home, assigned);
    }
    p25_ua_push(o, &row);
}

void
p25_ua_location_registration_response(const p25_ua_origin* o, int rv, uint32_t target, uint16_t group) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, p25_ua_rv_result(rv));
    row.subject = p25_ua_identity(target);
    p25_ua_set_group(&row, group);
    p25_ua_push(o, &row);
}

void
p25_ua_deregistration_ack(const p25_ua_origin* o, const p25_home_id* home, uint32_t assigned) {
    if (!p25_ua_origin_ok(o) || !home) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED);
    row.subject = p25_ua_identity_home(home->id, home);
    p25_ua_set_assigned(o->state, &row, home, assigned);
    p25_ua_push(o, &row);
}

void
p25_ua_registration_command(const p25_ua_origin* o, uint32_t target) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    row.subject = p25_ua_identity(target);
    p25_ua_push(o, &row);
}

void
p25_ua_group_affiliation_response(const p25_ua_origin* o, int gav, uint32_t target, uint16_t group) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, p25_ua_rv_result(gav));
    row.subject = p25_ua_identity(target);
    p25_ua_set_group(&row, group);
    p25_ua_push(o, &row);
}

void
p25_ua_deny_queued(const p25_ua_origin* o, const p25_ua_response* r) {
    if (!p25_ua_origin_ok(o) || !r) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, r->is_deny ? DSD_UA_DENY : DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE,
                    r->is_deny ? DSD_UA_RESULT_DENIED : DSD_UA_RESULT_QUEUED);
    row.subject = p25_ua_identity(r->target);
    p25_ua_set_value(&row, r->service & 0x3FU);
    row.reason_set = r->is_deny ? DSD_UA_REASON_P25_DENY : DSD_UA_REASON_P25_QUEUED;
    row.reason_code = r->reason;
    if (r->has_info) {
        row.value2 = r->info & P25_UA_ADDRESS_MASK;
        row.value2_valid = 1U;
    }
    p25_ua_push(o, &row);
}

/* The kind of class $00 operand @p command (the ACK bit clear); 0 for a reserved operand. */
static uint8_t
p25_ua_extended_function_kind(uint8_t command) {
    switch (command) {
        case P25_UA_EXT_CHECK: return DSD_UA_RADIO_CHECK;
        case P25_UA_EXT_DETACH: return DSD_UA_DETACH;
        case P25_UA_EXT_UNINHIBIT: return DSD_UA_UNINHIBIT;
        case P25_UA_EXT_INHIBIT: return DSD_UA_INHIBIT;
        default: return 0U;
    }
}

void
p25_ua_extended_function(const p25_ua_origin* o, const p25_ua_ext_function* f) {
    if (!p25_ua_origin_ok(o) || !f || f->class_id != 0U) {
        return;
    }
    const int ack = (f->operand & P25_UA_EXT_ACK) != 0;
    const uint8_t kind = p25_ua_extended_function_kind((uint8_t)(f->operand & ~P25_UA_EXT_ACK));
    if (kind == 0U) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, kind, ack ? DSD_UA_ROLE_ACK : DSD_UA_ROLE_COMMAND,
                    ack ? DSD_UA_RESULT_ACKNOWLEDGED : DSD_UA_RESULT_NA);
    /* Table 2.3.15-1: a command's argument is the Source Address, an acknowledgement's the radio acknowledging. */
    const dsd_ua_identity argument = p25_ua_identity_home(f->argument, f->argument_home);
    if (ack) {
        row.subject = argument;
        row.actor = p25_ua_identity(f->target);
    } else {
        row.subject = p25_ua_identity(f->target);
        row.actor = argument;
    }
    p25_ua_push(o, &row);
}

void
p25_ua_radio_unit_monitor(const p25_ua_origin* o, uint32_t target, const p25_ua_unit* source, int talkgroup_mode,
                          uint16_t group) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    row.subject = p25_ua_identity(target);
    if (talkgroup_mode) {
        p25_ua_set_group(&row, group);
    } else {
        row.actor = p25_ua_unit_identity(source);
    }
    p25_ua_push(o, &row);
}

/* A report row: the reporting source is the subject and the target the recipient (plan G13). */
static void
p25_ua_report(const p25_ua_origin* o, uint8_t kind, uint32_t value, const p25_ua_unit* target,
              const p25_ua_unit* source) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, kind, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA);
    p25_ua_set_subject_unit(o->state, &row, source);
    row.recipient = p25_ua_unit_identity(target);
    p25_ua_set_value(&row, value);
    p25_ua_push(o, &row);
}

void
p25_ua_status_update(const p25_ua_origin* o, uint16_t status, const p25_ua_unit* target, const p25_ua_unit* source) {
    p25_ua_report(o, DSD_UA_STATUS, status, target, source);
}

void
p25_ua_message_update(const p25_ua_origin* o, uint16_t message, const p25_ua_unit* target, const p25_ua_unit* source) {
    p25_ua_report(o, DSD_UA_MESSAGE, message, target, source);
}

/* A row about @p target that @p source started. */
static void
p25_ua_directed(const p25_ua_origin* o, uint8_t kind, uint8_t role, const p25_ua_unit* target,
                const p25_ua_unit* source, dsd_unit_activity* row) {
    p25_ua_row_init(o, row, kind, role, DSD_UA_RESULT_NA);
    p25_ua_set_subject_unit(o->state, row, target);
    row->actor = p25_ua_unit_identity(source);
}

void
p25_ua_status_query(const p25_ua_origin* o, const p25_ua_unit* target, const p25_ua_unit* source) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_directed(o, DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, target, source, &row);
    p25_ua_push(o, &row);
}

void
p25_ua_call_alert(const p25_ua_origin* o, const p25_ua_unit* target, const p25_ua_unit* source) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_directed(o, DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, target, source, &row);
    p25_ua_push(o, &row);
}

void
p25_ua_answer_request(const p25_ua_origin* o, uint32_t target, const p25_ua_unit* source, int has_service,
                      uint8_t service) {
    if (!p25_ua_origin_ok(o)) {
        return;
    }
    dsd_unit_activity row;
    const p25_ua_unit paged = {target, {0U, 0U, 0U, 0U}, 0U};
    p25_ua_directed(o, DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, &paged, source, &row);
    if (has_service) {
        p25_ua_set_value(&row, service);
    }
    p25_ua_push(o, &row);
}

void
p25_ua_authentication_command(const p25_ua_origin* o, const p25_ua_unit* target) {
    if (!p25_ua_origin_ok(o) || !target) {
        return;
    }
    dsd_unit_activity row;
    p25_ua_row_init(o, &row, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA);
    row.subject = p25_ua_unit_identity(target);
    p25_ua_push(o, &row);
}
