// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The unit activity view (<dsd-neo/app_control/unit_activity_view.h>): reads a snapshot's ring in pages and
 * resolves each record's labels, filters, reason, scope and names.
 */

#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/core/utf16.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Records read from the ring per page: a few KiB of stack. */
enum { UA_VIEW_PAGE = 16 };

/* Wildcards of a label rule. */
enum {
    UA_ANY_ROLE = 0,
    UA_ANY_PROTO = 0,
    UA_ANY_RESULT = 0xFF,
    UA_ANY_STAGE = 0xFF,
};

/* ---- Text ---- */

/* 1 when the sequence of @p len bytes at @p s is a C0 or C1 control character or DEL. */
static int
ua_is_control(const unsigned char* s, size_t len) {
    if (len == 1U) {
        return s[0] < 0x20U || s[0] == 0x7FU;
    }
    return len == 2U && s[0] == 0xC2U && s[1] <= 0x9FU; /* U+0080..U+009F */
}

/* Append @p src to @p dst (capacity @p cap, holding a string): whole UTF-8 characters only, cut at the last one that
 * fits, with each control character as a space and each byte that starts no well-formed character dropped. Returns
 * 1 when all of @p src went in, 0 when it was cut. */
static int
ua_text_append(char* dst, size_t cap, const char* src) {
    size_t out = strlen(dst);
    const unsigned char* s = (const unsigned char*)src;
    size_t left = strlen(src);
    int whole = 1;
    while (left > 0U) {
        const size_t len = dsd_utf8_sequence_len(s, left);
        if (len == 0U) {
            s++;
            left--;
            continue;
        }
        if (out + len >= cap) {
            whole = 0;
            break;
        }
        if (ua_is_control(s, len)) {
            dst[out++] = ' ';
        } else {
            DSD_MEMCPY(dst + out, s, len);
            out += len;
        }
        s += len;
        left -= len;
    }
    dst[out] = '\0';
    return whole;
}

static void
ua_text_set(char* dst, size_t cap, const char* src) {
    dst[0] = '\0';
    (void)ua_text_append(dst, cap, src);
}

/* ---- Labels ---- */

typedef struct {
    uint8_t kind, role, result, protocol, stage;
    const char* label;
} ua_label_rule;

/* The labels a headline adds the reason to (dsd_app_unit_activity_headline()). */
static const char k_label_denied[] = "Denied";
static const char k_label_queued[] = "Queued";

/* First match wins: protocol- and stage-specific rules before the general ones, a kind's catch-all last. Commands and
 * acknowledgements say the command was sent or acknowledged, never that it took effect. */
static const ua_label_rule k_label_rules[] = {
    {DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_DMR, UA_ANY_STAGE,
     "Talkgroup subscription list requested"},
    {DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Registration requested"},
    {DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Registration acknowledged"},
    {DSD_UA_REGISTRATION, DSD_UA_ROLE_REPORT, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Registration reported"},
    {DSD_UA_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_ACCEPTED, UA_ANY_PROTO, UA_ANY_STAGE, "Registration accepted"},
    {DSD_UA_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_FAILED, UA_ANY_PROTO, UA_ANY_STAGE, "Registration failed"},
    {DSD_UA_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_DENIED, UA_ANY_PROTO, UA_ANY_STAGE, "Registration denied"},
    {DSD_UA_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_REFUSED, UA_ANY_PROTO, UA_ANY_STAGE, "Registration refused"},
    {DSD_UA_REGISTRATION, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Registration response"},

    {DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration requested"},
    {DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration acknowledged"},
    {DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_REPORT, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration reported"},
    {DSD_UA_LOCATION_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_ACCEPTED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration accepted"},
    {DSD_UA_LOCATION_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_FAILED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration failed"},
    {DSD_UA_LOCATION_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_DENIED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration denied"},
    {DSD_UA_LOCATION_REGISTRATION, UA_ANY_ROLE, DSD_UA_RESULT_REFUSED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration refused"},
    {DSD_UA_LOCATION_REGISTRATION, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Location registration response"},

    /* A logout answer whose success value no second source confirms (NXDN REG_C_RESP) stays a "response". */
    {DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Deregistered"},
    {DSD_UA_DEREGISTRATION, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Deregistration requested"},
    {DSD_UA_DEREGISTRATION, DSD_UA_ROLE_REPORT, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Deregistration reported"},
    {DSD_UA_DEREGISTRATION, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Logout response"},

    /* DMR C_ACKD 0x65 carries an acceptance mask, never a talkgroup ID. */
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, UA_ANY_RESULT, DSD_UA_PROTO_DMR, UA_ANY_STAGE,
     "Talkgroup subscription response"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Talkgroup affiliation requested"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Joined talkgroup"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Talkgroup join failed"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Talkgroup join denied"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, UA_ANY_PROTO, UA_ANY_STAGE,
     "Talkgroup join refused"},
    {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE,
     "Talkgroup join response"},
    {DSD_UA_GROUP_AFFILIATION, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Talkgroup affiliation"},

    {DSD_UA_DENY, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, k_label_denied},
    {DSD_UA_QUEUED, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, k_label_queued},
    {DSD_UA_WAITING, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Waiting for further signalling"},

    {DSD_UA_RADIO_CHECK, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Radio check acknowledged"},
    {DSD_UA_RADIO_CHECK, DSD_UA_ROLE_RESPONSE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Radio check response"},
    {DSD_UA_RADIO_CHECK, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Radio check"},

    {DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Inhibit sent"},
    {DSD_UA_INHIBIT, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Inhibit acknowledged"},
    {DSD_UA_INHIBIT, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Inhibit response"},
    {DSD_UA_UNINHIBIT, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Uninhibit sent"},
    {DSD_UA_UNINHIBIT, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Uninhibit acknowledged"},
    {DSD_UA_UNINHIBIT, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Uninhibit response"},

    /* TETRA D-DISABLE temporary (stun), permanent (kill) and D-ENABLE (revive): commands in both stages. */
    {DSD_UA_STUN, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated disable command"},
    {DSD_UA_STUN, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, UA_ANY_STAGE, "Disable command"},
    {DSD_UA_STUN, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Stun sent"},
    {DSD_UA_STUN, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Stun acknowledged"},
    {DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated enable command"},
    {DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, UA_ANY_STAGE, "Enable command"},
    {DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Revive sent"},
    {DSD_UA_REVIVE, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Revive acknowledged"},
    {DSD_UA_KILL, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated permanent disable command"},
    {DSD_UA_KILL, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, DSD_UA_PROTO_TETRA, UA_ANY_STAGE, "Permanent disable command"},
    {DSD_UA_KILL, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Kill sent"},
    {DSD_UA_KILL, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Kill acknowledged"},
    /* NXDN REM_CON_RESP: the controller's answer or the forwarded target response, both before execution. */
    {DSD_UA_STUN, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Remote-control response"},
    {DSD_UA_REVIVE, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Remote-control response"},
    {DSD_UA_KILL, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Remote-control response"},

    {DSD_UA_DETACH, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Detach sent"},
    {DSD_UA_DETACH, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Detach acknowledged"},
    {DSD_UA_DETACH, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Detach response"},
    {DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Monitor acknowledged"},
    {DSD_UA_RADIO_MONITOR, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Monitor requested"},

    {DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Status requested"},
    {DSD_UA_STATUS, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Status acknowledged"},
    {DSD_UA_STATUS, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Status"},
    {DSD_UA_MESSAGE, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Message acknowledged"},
    {DSD_UA_MESSAGE, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Message"},
    {DSD_UA_CALL_ALERT, DSD_UA_ROLE_ACK, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Call alert acknowledged"},
    {DSD_UA_CALL_ALERT, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Call alert"},
    {DSD_UA_ANSWER_REQUEST, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Answer request"},
    {DSD_UA_AUTHENTICATION, DSD_UA_ROLE_RESPONSE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Authentication response"},
    {DSD_UA_AUTHENTICATION, UA_ANY_ROLE, UA_ANY_RESULT, UA_ANY_PROTO, UA_ANY_STAGE, "Authentication"},
};

static int
ua_rule_matches(const ua_label_rule* rule, const dsd_unit_activity* r) {
    return rule->kind == r->kind && (rule->role == UA_ANY_ROLE || rule->role == r->role)
           && (rule->result == UA_ANY_RESULT || rule->result == r->result)
           && (rule->protocol == UA_ANY_PROTO || rule->protocol == r->protocol)
           && (rule->stage == UA_ANY_STAGE || rule->stage == r->stage);
}

static const char*
ua_kind_label(const dsd_unit_activity* r) {
    for (size_t i = 0; i < sizeof(k_label_rules) / sizeof(k_label_rules[0]); i++) {
        if (ua_rule_matches(&k_label_rules[i], r)) {
            return k_label_rules[i].label;
        }
    }
    return "Unit activity"; /* a kind the ring refuses, kept for safety */
}

/* ---- Filters and tone ---- */

/* The filter of each kind (plan D4), indexed by dsd_ua_kind. */
static const uint8_t k_kind_filters[] = {
    [DSD_UA_REGISTRATION] = DSD_UA_FILTER_LOGINS,    [DSD_UA_LOCATION_REGISTRATION] = DSD_UA_FILTER_LOGINS,
    [DSD_UA_DEREGISTRATION] = DSD_UA_FILTER_LOGINS,  [DSD_UA_GROUP_AFFILIATION] = DSD_UA_FILTER_TALKGROUPS,
    [DSD_UA_DENY] = DSD_UA_FILTER_REFUSED,           [DSD_UA_QUEUED] = DSD_UA_FILTER_REFUSED,
    [DSD_UA_WAITING] = DSD_UA_FILTER_REFUSED,        [DSD_UA_RADIO_CHECK] = DSD_UA_FILTER_COMMANDS,
    [DSD_UA_INHIBIT] = DSD_UA_FILTER_COMMANDS,       [DSD_UA_UNINHIBIT] = DSD_UA_FILTER_COMMANDS,
    [DSD_UA_STUN] = DSD_UA_FILTER_COMMANDS,          [DSD_UA_REVIVE] = DSD_UA_FILTER_COMMANDS,
    [DSD_UA_KILL] = DSD_UA_FILTER_COMMANDS,          [DSD_UA_DETACH] = DSD_UA_FILTER_COMMANDS,
    [DSD_UA_RADIO_MONITOR] = DSD_UA_FILTER_COMMANDS, [DSD_UA_STATUS] = DSD_UA_FILTER_OTHER,
    [DSD_UA_MESSAGE] = DSD_UA_FILTER_OTHER,          [DSD_UA_CALL_ALERT] = DSD_UA_FILTER_OTHER,
    [DSD_UA_ANSWER_REQUEST] = DSD_UA_FILTER_OTHER,   [DSD_UA_AUTHENTICATION] = DSD_UA_FILTER_OTHER,
};

static uint32_t
ua_kind_filter(uint8_t kind) {
    const uint8_t f = kind < sizeof(k_kind_filters) ? k_kind_filters[kind] : 0U;
    return f ? f : (uint32_t)DSD_UA_FILTER_OTHER;
}

static uint32_t
ua_filters(const dsd_unit_activity* r) {
    uint32_t filters = ua_kind_filter(r->kind);
    if (r->result == DSD_UA_RESULT_REFUSED || r->result == DSD_UA_RESULT_DENIED || r->result == DSD_UA_RESULT_FAILED) {
        filters |= DSD_UA_FILTER_REFUSED;
    }
    return filters;
}

static uint8_t
ua_tone(const dsd_unit_activity* r) {
    return (r->kind == DSD_UA_INHIBIT || r->kind == DSD_UA_KILL) ? (uint8_t)DSD_UA_TONE_WARNING
                                                                 : (uint8_t)DSD_UA_TONE_NORMAL;
}

/* ---- Identities, values, reason, scope ---- */

static void
ua_identity_text(const dsd_ua_identity* id, char* out, size_t cap) {
    out[0] = '\0';
    if (!id->valid) {
        return;
    }
    switch (id->subtype) {
        case DSD_UA_ID_RADIO: DSD_SNPRINTF(out, cap, "%u", (unsigned)id->id); break;
        case DSD_UA_ID_GROUP: DSD_SNPRINTF(out, cap, "TG %u", (unsigned)id->id); break;
        case DSD_UA_ID_INFRASTRUCTURE: DSD_SNPRINTF(out, cap, "System 0x%X", (unsigned)id->id); break;
        case DSD_UA_ID_TEI_PARTIAL: DSD_SNPRINTF(out, cap, "Equipment 0x%X", (unsigned)id->id); break;
        default: DSD_SNPRINTF(out, cap, "Address %u", (unsigned)id->id); break;
    }
}

/* The learned-alias scope of a row's reception scope (the same kind, a and b). */
static void
ua_alias_scope_of(const dsd_ua_scope* s, dsd_alias_scope* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (s->valid) {
        out->kind = s->kind;
        out->a = s->a;
        out->b = s->b;
        out->valid = 1U;
    }
}

/* How a row is named: from the snapshot's lists in the row's scope (live) or from what its producer stored. */
typedef struct {
    const dsd_state* snapshot;
    dsd_alias_scope scope;
    int live;
} ua_naming;

static void
ua_naming_init(const dsd_state* snapshot, const dsd_unit_activity* r, ua_naming* n) {
    dsd_alias_scope current;
    n->snapshot = snapshot;
    ua_alias_scope_of(&r->scope, &n->scope);
    (void)dsd_alias_scope_current(snapshot, &current);
    n->live = dsd_alias_scope_equal(&n->scope, &current);
}

/* A radio's live name in the row's scope; 1 when found. */
static int
ua_radio_name(const ua_naming* n, const dsd_ua_identity* id, char* out, size_t cap) {
    if (!id->valid || id->subtype != DSD_UA_ID_RADIO) {
        return 0;
    }
    return dsd_source_label_lookup_scoped(n->snapshot, &n->scope, id->id, NULL, 0U, out, cap);
}

/* 1 when @p id is an ID of the row's own system: no home system given, or the home is the row's scope. A roaming
 * radio's home ID names another system's radio, so the lists of this one never name it. */
static int
ua_identity_is_local(const ua_naming* n, const dsd_ua_identity* id) {
    if (!id->home_valid) {
        return 1;
    }
    return n->scope.valid && id->home_kind == n->scope.kind && id->home_a == n->scope.a && id->home_b == n->scope.b;
}

static void
ua_subject_name(const ua_naming* n, const dsd_unit_activity* r, char* out, size_t cap) {
    char name[DSD_SOURCE_ALIAS_NAME_MAX + 16];
    name[0] = '\0';
    out[0] = '\0';
    if (!r->subject.valid || (r->subject.subtype != DSD_UA_ID_RADIO && r->subject.subtype != DSD_UA_ID_GROUP)) {
        return; /* infrastructure, equipment and unknown addresses are never named */
    }
    int found = 0;
    if (n->live && r->subject.subtype == DSD_UA_ID_RADIO) {
        /* The assigned (serving) ID names the radio first: a home identity is known there by it. A nonlocal subject
           (a TETRA USSI, a P25 source awaiting its extension) is another network's number: never named here. */
        found = ua_radio_name(n, &r->assigned, name, sizeof name)
                || (!r->subject_nonlocal && ua_identity_is_local(n, &r->subject)
                    && ua_radio_name(n, &r->subject, name, sizeof name));
    } else if (n->live) {
        found = dsd_tg_policy_lookup_group_label(n->snapshot, r->subject.id, NULL, 0U, name, sizeof name);
    }
    ua_text_set(out, cap, found ? name : r->name_frozen);
}

/* The "…" a shortened text ends with. */
static const char k_ellipsis[] = "\xE2\x80\xA6";

/* A P25 status or message value as the P25 prints show it: the 16-bit Status field's User Status (high octet) and Unit
 * Status (low octet) in hex (TIA-102.AABC-D §2.3.29), the 16-bit short data message in hex. 0 for another row. */
static int
ua_p25_value_part(const dsd_unit_activity* r, unsigned v, char* out, size_t cap) {
    if (r->protocol != DSD_UA_PROTO_P25P1 && r->protocol != DSD_UA_PROTO_P25P2) {
        return 0;
    }
    if (r->kind == DSD_UA_STATUS) {
        DSD_SNPRINTF(out, cap, "status %02X/%02X", (v >> 8) & 0xFFU, v & 0xFFU);
        return 1;
    }
    if (r->kind == DSD_UA_MESSAGE) {
        DSD_SNPRINTF(out, cap, "message %04X", v & 0xFFFFU);
        return 1;
    }
    return 0;
}

static void
ua_value_part(const dsd_unit_activity* r, char* out, size_t cap) {
    out[0] = '\0';
    if (!r->value_valid) {
        return;
    }
    const unsigned v = (unsigned)r->value;
    if (ua_p25_value_part(r, v, out, cap)) {
        return;
    }
    if (r->protocol == DSD_UA_PROTO_TETRA && r->kind == DSD_UA_LOCATION_REGISTRATION) {
        const char* type = dsd_ua_tetra_accept_type_text(r->value);
        if (type) {
            DSD_SNPRINTF(out, cap, "%s", type);
        } else {
            DSD_SNPRINTF(out, cap, "accept type %u", v);
        }
    } else if (r->protocol == DSD_UA_PROTO_DMR && r->kind == DSD_UA_GROUP_AFFILIATION) {
        if (v == 0U) {
            DSD_SNPRINTF(out, cap, "%s", "all rejected");
        } else {
            DSD_SNPRINTF(out, cap, "accepted mask 0x%X", v);
        }
    } else if (r->kind == DSD_UA_STATUS) {
        DSD_SNPRINTF(out, cap, "status %u", v);
    } else if (r->kind == DSD_UA_MESSAGE) {
        DSD_SNPRINTF(out, cap, "message %u", v);
    } else if (r->kind == DSD_UA_DENY || r->kind == DSD_UA_QUEUED || r->kind == DSD_UA_CALL_ALERT
               || r->kind == DSD_UA_ANSWER_REQUEST) {
        DSD_SNPRINTF(out, cap, "service 0x%02X", v);
    } else {
        DSD_SNPRINTF(out, cap, "value %u", v);
    }
}

/* Motorola ARS PDU types (dsd_unit_activity::value2 of a DMR ARS report): device and user (de)registration. */
enum {
    UA_ARS_DEVICE_REG = 0x00,
    UA_ARS_DEVICE_DEREG = 0x01,
    UA_ARS_USER_REG = 0x05,
    UA_ARS_USER_DEREG = 0x06,
};

static int
ua_is_command_kind(uint8_t kind) {
    return kind == DSD_UA_STUN || kind == DSD_UA_REVIVE || kind == DSD_UA_KILL || kind == DSD_UA_RADIO_MONITOR;
}

static void
ua_dmr_value2_part(const dsd_unit_activity* r, unsigned v, char* out, size_t cap) {
    if (r->role != DSD_UA_ROLE_REPORT || (r->kind != DSD_UA_REGISTRATION && r->kind != DSD_UA_DEREGISTRATION)) {
        return;
    }
    if (v == UA_ARS_DEVICE_REG || v == UA_ARS_DEVICE_DEREG) {
        DSD_SNPRINTF(out, cap, "%s", "device");
    } else if (v == UA_ARS_USER_REG || v == UA_ARS_USER_DEREG) {
        DSD_SNPRINTF(out, cap, "%s", "user");
    }
}

/* The second payload field where it means something to a reader (it is keyed, so two rows that differ only there read
 * apart): a DMR ARS report's device or user, a P25 deny or queued response's Additional Information, an NXDN visitor
 * group or remote control parameter, a TETRA rejected location update's type. A TETRA TEI's high bits go in the
 * subject text instead. */
static void
ua_value2_part(const dsd_unit_activity* r, char* out, size_t cap) {
    out[0] = '\0';
    if (!r->value2_valid) {
        return;
    }
    const unsigned v = (unsigned)r->value2;
    switch (r->protocol) {
        case DSD_UA_PROTO_DMR: ua_dmr_value2_part(r, v, out, cap); break;
        case DSD_UA_PROTO_P25P1:
        case DSD_UA_PROTO_P25P2:
            if (r->kind == DSD_UA_DENY || r->kind == DSD_UA_QUEUED) {
                DSD_SNPRINTF(out, cap, "for %u", v);
            }
            break;
        case DSD_UA_PROTO_NXDN:
            if (r->kind == DSD_UA_REGISTRATION || r->kind == DSD_UA_GROUP_AFFILIATION) {
                DSD_SNPRINTF(out, cap, "visitor TG %u", v);
            } else if (ua_is_command_kind(r->kind)) {
                DSD_SNPRINTF(out, cap, "parameter 0x%04X", v);
            }
            break;
        case DSD_UA_PROTO_TETRA:
            if (r->kind == DSD_UA_LOCATION_REGISTRATION) {
                DSD_SNPRINTF(out, cap, "update type %u", v);
            }
            break;
        default: break;
    }
}

/* The subject's text: its identity, with a TETRA TEI piece made whole again from its high bits (value2). */
static void
ua_subject_text(const dsd_unit_activity* r, char* out, size_t cap) {
    if (r->subject.valid && r->subject.subtype == DSD_UA_ID_TEI_PARTIAL && r->value2_valid) {
        DSD_SNPRINTF(out, cap, "Equipment 0x%X%08X", (unsigned)r->value2, (unsigned)r->subject.id);
        return;
    }
    ua_identity_text(&r->subject, out, cap);
}

/* An identity part with its lead word ("as 100", "to 2345", "from System 0xFFFECC"). */
static void
ua_identity_part(const char* lead, const dsd_ua_identity* id, char* out, size_t cap) {
    char text[48];
    out[0] = '\0';
    ua_identity_text(id, text, sizeof text);
    if (text[0] != '\0') {
        DSD_SNPRINTF(out, cap, "%s %s", lead, text);
    }
}

/* The parts of a target text after its group, in display order. */
enum { UA_TARGET_VALUE, UA_TARGET_VALUE2, UA_TARGET_AS, UA_TARGET_TO, UA_TARGET_FROM, UA_TARGET_PARTS };

enum { UA_TARGET_PART_SIZE = 80 };

/* Shorten @p label in place so that it takes at most @p room bytes: whole, or cut at a character boundary and ended
 * with "…", or (when not even one character and the "…" fit) nothing. */
static void
ua_label_fit(char* label, size_t room) {
    if (strlen(label) <= room) {
        return;
    }
    if (room < sizeof(k_ellipsis)) {
        label[0] = '\0';
        return;
    }
    char cut[DSD_SOURCE_ALIAS_NAME_MAX + 16];
    const size_t keep = room - (sizeof(k_ellipsis) - 1U);
    ua_text_set(cut, keep + 1U < sizeof cut ? keep + 1U : sizeof cut, label);
    size_t len = strlen(cut);
    while (len > 0U && cut[len - 1U] == ' ') {
        cut[--len] = '\0';
    }
    DSD_SNPRINTF(label, room + 1U, "%s%s", cut, len > 0U ? k_ellipsis : "");
}

/* Append @p part after a ", " when the text already holds one, only when it fits whole in @p limit bytes (the
 * terminator included). Returns 0 (nothing appended) when it does not. */
static int
ua_join_whole(char* out, size_t limit, const char* part) {
    const size_t sep = out[0] != '\0' ? 2U : 0U;
    if (strlen(out) + sep + strlen(part) >= limit) {
        return 0;
    }
    if (sep) {
        (void)ua_text_append(out, limit, ", ");
    }
    (void)ua_text_append(out, limit, part);
    return 1;
}

/* The row's talkgroup with, when the row's system is current, its label shortened to what the rest leaves room for. */
static void
ua_group_text(const ua_naming* n, const dsd_unit_activity* r, size_t room, char* out, size_t cap) {
    char label[DSD_SOURCE_ALIAS_NAME_MAX + 16];
    out[0] = '\0';
    if (!r->group_valid) {
        return;
    }
    DSD_SNPRINTF(out, cap, "TG %u", (unsigned)r->group);
    const size_t used = strlen(out) + 1U; /* the number and the space before the label */
    if (!n->live || room <= used
        || !dsd_tg_policy_lookup_group_label(n->snapshot, r->group, NULL, 0U, label, sizeof label)) {
        return;
    }
    ua_label_fit(label, room - used);
    if (label[0] != '\0') {
        (void)ua_text_append(out, cap, " ");
        (void)ua_text_append(out, cap, label);
    }
}

/* Every part goes in whole or not at all: a group label is shortened first ("…"), then a part that still does not fit
 * and those after it are left out and the text ends with ", …". An ID is never cut. */
static void
ua_target_text(const ua_naming* n, const dsd_unit_activity* r, char* out, size_t cap) {
    char parts[UA_TARGET_PARTS][UA_TARGET_PART_SIZE];
    char group[UA_TARGET_PART_SIZE];
    ua_value_part(r, parts[UA_TARGET_VALUE], sizeof parts[0]);
    ua_value2_part(r, parts[UA_TARGET_VALUE2], sizeof parts[0]);
    ua_identity_part("as", &r->assigned, parts[UA_TARGET_AS], sizeof parts[0]);
    ua_identity_part("to", &r->recipient, parts[UA_TARGET_TO], sizeof parts[0]);
    ua_identity_part("from", &r->actor, parts[UA_TARGET_FROM], sizeof parts[0]);
    size_t rest = 0U; /* the parts after the group, each with its ", " */
    for (int i = 0; i < UA_TARGET_PARTS; i++) {
        rest += parts[i][0] != '\0' ? strlen(parts[i]) + 2U : 0U;
    }
    const size_t room = cap - 1U > rest ? cap - 1U - rest : 0U;
    ua_group_text(n, r, room, group, sizeof group);
    int last = -1;
    for (int i = 0; i < UA_TARGET_PARTS; i++) {
        last = parts[i][0] != '\0' ? i : last;
    }
    out[0] = '\0';
    (void)ua_join_whole(out, cap, group);
    /* Room for ", …" is kept while a later part could still be left out. */
    const size_t reserve = 2U + sizeof(k_ellipsis) - 1U;
    for (int i = 0; i <= last; i++) {
        if (parts[i][0] != '\0' && !ua_join_whole(out, i < last ? cap - reserve : cap, parts[i])) {
            (void)ua_join_whole(out, cap, k_ellipsis);
            return;
        }
    }
}

static void
ua_reason_text(const dsd_unit_activity* r, char* out, size_t cap) {
    out[0] = '\0';
    if (r->reason_set == DSD_UA_REASON_NONE) {
        return;
    }
    const char* text = dsd_ua_reason_text(r->reason_set, r->reason_code);
    char buf[96];
    if (text) {
        DSD_SNPRINTF(buf, sizeof buf, "%s (0x%02X)", text, (unsigned)r->reason_code);
    } else {
        DSD_SNPRINTF(buf, sizeof buf, "Reason 0x%02X", (unsigned)r->reason_code);
    }
    ua_text_set(out, cap, buf);
}

static unsigned
ua_site(const dsd_ua_scope* s) {
    return (unsigned)(s->c & ~DSD_UA_SCOPE_SITE_KNOWN);
}

static int
ua_site_known(const dsd_ua_scope* s) {
    return (s->c & DSD_UA_SCOPE_SITE_KNOWN) != 0U;
}

static void
ua_scope_dmr_text(const dsd_ua_scope* s, char* out, size_t cap) {
    if ((s->a >> 8) == DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) {
        /* Capacity Plus sends no network number: the site number and colour code stand in (b, a & 0xFF). */
        DSD_SNPRINTF(out, cap, "Site %u CC %u", (unsigned)s->b, (unsigned)(s->a & 0xFFU));
    } else if (ua_site_known(s)) {
        DSD_SNPRINTF(out, cap, "Net %u Site %u", (unsigned)s->b, ua_site(s));
    } else {
        DSD_SNPRINTF(out, cap, "Net %u", (unsigned)s->b);
    }
}

static void
ua_scope_text(const dsd_ua_scope* s, char* out, size_t cap) {
    out[0] = '\0';
    if (!s->valid) {
        return;
    }
    const int site = ua_site_known(s);
    switch (s->kind) {
        case DSD_UA_SCOPE_P25:
            if (site) {
                DSD_SNPRINTF(out, cap, "WACN %05X SYS %03X RFSS %u SITE %u", (unsigned)s->a, (unsigned)s->b,
                             (ua_site(s) >> 8) & 0xFFU, ua_site(s) & 0xFFU);
            } else {
                DSD_SNPRINTF(out, cap, "WACN %05X SYS %03X", (unsigned)s->a, (unsigned)s->b);
            }
            break;
        case DSD_UA_SCOPE_P25_NAC: DSD_SNPRINTF(out, cap, "NAC %03X", (unsigned)s->a); break;
        case DSD_UA_SCOPE_DMR: ua_scope_dmr_text(s, out, cap); break;
        case DSD_UA_SCOPE_NXDN: DSD_SNPRINTF(out, cap, "SYS %u", (unsigned)s->b); break;
        case DSD_UA_SCOPE_TETRA: DSD_SNPRINTF(out, cap, "MCC %u MNC %u", (unsigned)s->a, (unsigned)s->b); break;
        default: break;
    }
    char tail[24];
    tail[0] = '\0';
    if (site && s->kind == DSD_UA_SCOPE_NXDN) {
        DSD_SNPRINTF(tail, sizeof tail, " SITE %u", ua_site(s));
    } else if (site && s->kind == DSD_UA_SCOPE_TETRA) {
        DSD_SNPRINTF(tail, sizeof tail, " LA %u", ua_site(s));
    }
    ua_text_append(out, cap, tail);
}

static const char*
ua_protocol_text(uint8_t protocol) {
    switch (protocol) {
        case DSD_UA_PROTO_P25P1: return "P25 Phase 1";
        case DSD_UA_PROTO_P25P2: return "P25 Phase 2";
        case DSD_UA_PROTO_DMR: return "DMR";
        case DSD_UA_PROTO_NXDN: return "NXDN";
        case DSD_UA_PROTO_TETRA: return "TETRA";
        default: return "";
    }
}

static void
ua_row_from(const dsd_state* snapshot, const dsd_unit_activity* r, dsd_app_unit_activity_row* out) {
    ua_naming naming;
    DSD_MEMSET(out, 0, sizeof(*out));
    ua_naming_init(snapshot, r, &naming);
    out->seq = r->seq;
    out->rev = r->rev;
    out->wall_s = r->wall_s;
    out->repeat = r->repeat;
    out->filters = ua_filters(r);
    out->tone = ua_tone(r);
    ua_text_set(out->kind_label, sizeof out->kind_label, ua_kind_label(r));
    ua_subject_text(r, out->subject_text, sizeof out->subject_text);
    ua_subject_name(&naming, r, out->subject_name, sizeof out->subject_name);
    ua_target_text(&naming, r, out->target_text, sizeof out->target_text);
    ua_reason_text(r, out->reason_text, sizeof out->reason_text);
    ua_scope_text(&r->scope, out->scope_text, sizeof out->scope_text);
    ua_text_set(out->protocol_text, sizeof out->protocol_text, ua_protocol_text(r->protocol));
}

/* ---- Reading ---- */

/* One page of the ring after @p cursor: the rows read, or -1 for a failed read. A read never returns more rows than it
   was given room for; one that did is refused rather than written past @p room. */
static int
ua_read_page(const dsd_state* snapshot, const dsd_app_unit_activity_cursor* cursor, dsd_unit_activity* page, int room,
             dsd_unit_activity_read_meta* rm) {
    const int n = dsd_unit_activity_read(snapshot, cursor->instance, cursor->rev, page, room, rm);
    return (n < 0 || n > room) ? -1 : n;
}

int
dsd_app_unit_activity_rows(const dsd_state* snapshot, dsd_app_unit_activity_cursor* cursor,
                           dsd_app_unit_activity_row* out, int cap, dsd_app_unit_activity_meta* meta) {
    if (!snapshot || !cursor || !meta || cap < 0 || (!out && cap > 0)) {
        return -1;
    }
    DSD_MEMSET(meta, 0, sizeof(*meta));
    dsd_unit_activity page[UA_VIEW_PAGE];
    int total = 0;
    for (;;) {
        const int room = cap - total < UA_VIEW_PAGE ? cap - total : UA_VIEW_PAGE;
        dsd_unit_activity_read_meta rm;
        const int n = ua_read_page(snapshot, cursor, page, room, &rm);
        if (n < 0) {
            return -1;
        }
        if (!rm.held) {
            return total;
        }
        meta->held = 1U;
        meta->lost |= rm.lost;
        meta->reset |= rm.reset;
        meta->more = rm.more;
        cursor->instance = rm.instance;
        cursor->rev = rm.last_rev;
        for (int i = 0; i < n; i++) {
            ua_row_from(snapshot, &page[i], &out[total + i]);
        }
        total += n;
        if (!rm.more || total >= cap) {
            return total;
        }
    }
}

uint64_t
dsd_app_unit_activity_name_key(const dsd_state* snapshot) {
    return dsd_alias_name_key(snapshot);
}

void
dsd_app_unit_activity_headline(const dsd_app_unit_activity_row* row, char* out, size_t cap) {
    if (!out || cap == 0U) {
        return;
    }
    out[0] = '\0';
    if (!row) {
        return;
    }
    (void)ua_text_append(out, cap, row->kind_label);
    const int with_reason =
        strcmp(row->kind_label, k_label_denied) == 0 || strcmp(row->kind_label, k_label_queued) == 0;
    if (with_reason && row->reason_text[0] != '\0') {
        (void)ua_text_append(out, cap, ": ");
        (void)ua_text_append(out, cap, row->reason_text);
    }
}
