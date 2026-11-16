// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The app-control unit activity view: labels that never claim an outcome, every kind / role / result label,
 * filter membership, reason, scope and target text, pagination and loss over the ring, and names that follow each
 * row's own system across an A -> B -> A trunk-scan hop.
 *
 * All IDs are made up (WACN 0xBEE00, SYS 0x123 / 0x456, RID 42 / 1234567).
 */
#include <assert.h>
#include <ctype.h>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WACN_A = 0xBEE00,
    SYS_A = 0x123,
    SYS_B = 0x456,
    RID = 42,
};

static const uint64_t T0_NS = UINT64_C(1700000000000000000);

static dsd_state*
new_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

static void
tune_p25(dsd_state* st, uint16_t sysid) {
    dsd_rxid_reset(st);
    dsd_rxid_note_p25_network(st, WACN_A, sysid);
}

static dsd_unit_activity
mk(uint8_t protocol, uint8_t kind, uint8_t role, uint8_t result, uint32_t subject) {
    dsd_unit_activity r;
    DSD_MEMSET(&r, 0, sizeof(r));
    r.protocol = protocol;
    r.kind = kind;
    r.role = role;
    r.result = result;
    r.subject.id = subject;
    r.subject.subtype = DSD_UA_ID_RADIO;
    r.subject.valid = 1U;
    return r;
}

static void
set_identity(dsd_ua_identity* id, uint32_t value, uint8_t subtype) {
    DSD_MEMSET(id, 0, sizeof(*id));
    id->id = value;
    id->subtype = subtype;
    id->valid = 1U;
}

/* Push @p r and read back the one row it made or merged into. */
static dsd_app_unit_activity_row
view_of(dsd_state* st, dsd_app_unit_activity_cursor* cursor, const dsd_unit_activity* r) {
    dsd_app_unit_activity_row out[2];
    dsd_app_unit_activity_meta meta;
    assert(dsd_unit_activity_push(st, r) >= 0);
    const int n = dsd_app_unit_activity_rows(st, cursor, out, 2, &meta);
    assert(n == 1);
    assert(meta.held == 1U && meta.more == 0U);
    return out[0];
}

/* Read every row after @p cursor; returns the count. */
static int
read_all(const dsd_state* st, dsd_app_unit_activity_cursor* cursor, dsd_app_unit_activity_row* out, int cap,
         dsd_app_unit_activity_meta* meta_any) {
    int total = 0;
    DSD_MEMSET(meta_any, 0, sizeof(*meta_any));
    for (;;) {
        dsd_app_unit_activity_meta meta;
        const int room = cap - total < 7 ? cap - total : 7;
        const int n = dsd_app_unit_activity_rows(st, cursor, out + total, room, &meta);
        assert(n >= 0);
        meta_any->held |= meta.held;
        meta_any->lost |= meta.lost;
        meta_any->reset |= meta.reset;
        total += n;
        if (!meta.more || total >= cap) {
            return total;
        }
    }
}

static const dsd_app_unit_activity_row*
find_seq(const dsd_app_unit_activity_row* rows, int n, uint64_t seq) {
    for (int i = 0; i < n; i++) {
        if (rows[i].seq == seq) {
            return &rows[i];
        }
    }
    /* A missing row fails the test here, so no caller reads through a null row. */
    DSD_FPRINTF(stderr, "no row with seq %llu\n", (unsigned long long)seq);
    abort();
}

/* ---- Labels ---- */

typedef struct {
    uint8_t protocol, kind, role, result, stage;
    const char* label;
} label_case;

static const label_case k_labels[] = {
    {DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 0, "Registration accepted"},
    {DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, 0, "Registration failed"},
    {DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 0, "Registration denied"},
    {DSD_UA_PROTO_P25P2, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 0, "Registration refused"},
    {DSD_UA_PROTO_NXDN, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_UNKNOWN, 0, "Registration response"},
    {DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0,
     "Registration acknowledged"},
    {DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 0, "Registration refused"},
    {DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 0, "Registration denied"},
    {DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0,
     "Talkgroup subscription list requested"},
    {DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Registration reported"},
    {DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Registration requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 0,
     "Location registration accepted"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, 0,
     "Location registration failed"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 0,
     "Location registration denied"},
    {DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 0,
     "Location registration refused"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Deregistered"},
    {DSD_UA_PROTO_NXDN, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_UNKNOWN, 0, "Logout response"},
    {DSD_UA_PROTO_NXDN, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 0, "Logout response"},
    {DSD_UA_PROTO_DMR, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Deregistration reported"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 0, "Joined talkgroup"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 0,
     "Talkgroup join denied"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, 0,
     "Talkgroup join failed"},
    {DSD_UA_PROTO_NXDN, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 0,
     "Talkgroup join refused"},
    {DSD_UA_PROTO_DMR, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 0,
     "Talkgroup subscription response"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 0, "Denied"},
    {DSD_UA_PROTO_P25P1, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED, 0, "Queued"},
    {DSD_UA_PROTO_DMR, DSD_UA_WAITING, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Waiting for further signalling"},
    {DSD_UA_PROTO_P25P1, DSD_UA_RADIO_CHECK, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Radio check"},
    {DSD_UA_PROTO_P25P1, DSD_UA_RADIO_CHECK, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0,
     "Radio check acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Inhibit sent"},
    {DSD_UA_PROTO_P25P1, DSD_UA_INHIBIT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Inhibit acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_UNINHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Uninhibit sent"},
    {DSD_UA_PROTO_P25P1, DSD_UA_UNINHIBIT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Uninhibit acknowledged"},
    {DSD_UA_PROTO_DMR, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Stun sent"},
    {DSD_UA_PROTO_DMR, DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Revive sent"},
    {DSD_UA_PROTO_DMR, DSD_UA_KILL, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Kill sent"},
    {DSD_UA_PROTO_NXDN, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Stun sent"},
    {DSD_UA_PROTO_NXDN, DSD_UA_STUN, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Remote-control response"},
    {DSD_UA_PROTO_NXDN, DSD_UA_REVIVE, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_UNKNOWN, 0, "Remote-control response"},
    {DSD_UA_PROTO_NXDN, DSD_UA_KILL, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, 0, "Remote-control response"},
    {DSD_UA_PROTO_TETRA, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_INTENT, "Disable command"},
    {DSD_UA_PROTO_TETRA, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated disable command"},
    {DSD_UA_PROTO_TETRA, DSD_UA_KILL, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_INTENT,
     "Permanent disable command"},
    {DSD_UA_PROTO_TETRA, DSD_UA_KILL, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated permanent disable command"},
    {DSD_UA_PROTO_TETRA, DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_INTENT, "Enable command"},
    {DSD_UA_PROTO_TETRA, DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_STAGE_AUTHENTICATED,
     "Authenticated enable command"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DETACH, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Detach sent"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DETACH, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Detach acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Monitor requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Status"},
    {DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Status requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Message"},
    {DSD_UA_PROTO_P25P1, DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Call alert"},
    {DSD_UA_PROTO_P25P2, DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Answer request"},
    {DSD_UA_PROTO_P25P1, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Authentication"},
    {DSD_UA_PROTO_DMR, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Authentication"},
    {DSD_UA_PROTO_P25P1, DSD_UA_AUTHENTICATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Authentication response"},
    {DSD_UA_PROTO_P25P1, DSD_UA_CALL_ALERT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Call alert acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0, "Deregistration requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_DETACH, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Detach response"},
    {DSD_UA_PROTO_P25P1, DSD_UA_INHIBIT, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Inhibit response"},
    {DSD_UA_PROTO_DMR, DSD_UA_KILL, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Kill acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0,
     "Location registration acknowledged"},
    {DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0,
     "Location registration reported"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0,
     "Location registration requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_UNKNOWN, 0,
     "Location registration response"},
    {DSD_UA_PROTO_P25P1, DSD_UA_MESSAGE, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Message acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Monitor acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_RADIO_CHECK, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Radio check response"},
    {DSD_UA_PROTO_DMR, DSD_UA_REVIVE, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Revive acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Status acknowledged"},
    {DSD_UA_PROTO_DMR, DSD_UA_STUN, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 0, "Stun acknowledged"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 0, "Talkgroup affiliation"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0,
     "Talkgroup affiliation requested"},
    {DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0,
     "Talkgroup join response"},
    {DSD_UA_PROTO_P25P1, DSD_UA_UNINHIBIT, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 0, "Uninhibit response"},
};

static void
test_every_kind_role_result_label(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    for (size_t i = 0; i < sizeof(k_labels) / sizeof(k_labels[0]); i++) {
        const label_case* c = &k_labels[i];
        dsd_unit_activity r = mk(c->protocol, c->kind, c->role, c->result, 1000U + (uint32_t)i);
        r.stage = c->stage;
        const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
        if (strcmp(row.kind_label, c->label) != 0) {
            DSD_FPRINTF(stderr, "case %zu: label '%s', want '%s'\n", i, row.kind_label, c->label);
            assert(0);
        }
    }
    free_state(st);
}

/* @p label, lowercased, contains @p word. */
static int
label_has(const char* label, const char* word) {
    char lower[64];
    size_t i = 0;
    for (; label[i] != '\0' && i + 1U < sizeof lower; i++) {
        lower[i] = (char)tolower((unsigned char)label[i]);
    }
    lower[i] = '\0';
    return strstr(lower, word) != NULL;
}

/* 1 when test_every_kind_role_result_label() asserts @p label exactly. */
static int
label_is_asserted(const char* label) {
    for (size_t i = 0; i < sizeof(k_labels) / sizeof(k_labels[0]); i++) {
        if (strcmp(k_labels[i].label, label) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Every combination of protocol, kind, role, result and stage has a label of its own kind that the exact-label test
 * covers (so a new rule label needs its case), and no command,
 * acknowledgement or response label says the radio was killed, stunned, disabled or inhibited. */
static void
test_labels_never_claim_outcome(void) {
    static const char* const outcomes[] = {"killed",  "stunned",  "disabled", "inhibited", "revived",
                                           "enabled", "detached", "done",     "complete"};
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    uint32_t subject = 1U;
    for (int p = DSD_UA_PROTO_P25P1; p <= DSD_UA_PROTO_TETRA; p++) {
        for (int k = DSD_UA_REGISTRATION; k <= DSD_UA_AUTHENTICATION; k++) {
            for (int role = DSD_UA_ROLE_COMMAND; role <= DSD_UA_ROLE_REPORT; role++) {
                for (int res = DSD_UA_RESULT_NA; res <= DSD_UA_RESULT_UNKNOWN; res++) {
                    for (int stage = DSD_UA_STAGE_NONE; stage <= DSD_UA_STAGE_AUTHENTICATED; stage++) {
                        dsd_unit_activity r = mk((uint8_t)p, (uint8_t)k, (uint8_t)role, (uint8_t)res, subject++);
                        r.stage = (uint8_t)stage;
                        const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
                        assert(row.kind_label[0] != '\0');
                        assert(strcmp(row.kind_label, "Unit activity") != 0);
                        if (!label_is_asserted(row.kind_label)) {
                            DSD_FPRINTF(stderr, "label '%s' has no exact case in k_labels\n", row.kind_label);
                            assert(0);
                        }
                        assert(row.filters != 0U);
                        assert(strcmp(row.protocol_text, "") != 0);
                        if (role == DSD_UA_ROLE_REPORT) {
                            continue;
                        }
                        for (size_t w = 0; w < sizeof(outcomes) / sizeof(outcomes[0]); w++) {
                            if (label_has(row.kind_label, outcomes[w])) {
                                DSD_FPRINTF(stderr, "label '%s' claims '%s'\n", row.kind_label, outcomes[w]);
                                assert(0);
                            }
                        }
                    }
                }
            }
        }
    }
    free_state(st);
}

/* ---- Filters and tone ---- */

static void
test_filter_membership(void) {
    struct {
        uint8_t kind, role, result;
        uint32_t filters;
    } const cases[] = {
        {DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, DSD_UA_FILTER_LOGINS},
        {DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, DSD_UA_FILTER_LOGINS | DSD_UA_FILTER_REFUSED},
        {DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED,
         DSD_UA_FILTER_LOGINS | DSD_UA_FILTER_REFUSED},
        {DSD_UA_DEREGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, DSD_UA_FILTER_LOGINS},
        {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, DSD_UA_FILTER_TALKGROUPS},
        {DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED,
         DSD_UA_FILTER_TALKGROUPS | DSD_UA_FILTER_REFUSED},
        {DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, DSD_UA_FILTER_REFUSED},
        {DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED, DSD_UA_FILTER_REFUSED},
        {DSD_UA_WAITING, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, DSD_UA_FILTER_REFUSED},
        {DSD_UA_RADIO_CHECK, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_INHIBIT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_UNINHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_REVIVE, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_KILL, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_FAILED, DSD_UA_FILTER_COMMANDS | DSD_UA_FILTER_REFUSED},
        {DSD_UA_DETACH, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_RADIO_MONITOR, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_COMMANDS},
        {DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, DSD_UA_FILTER_OTHER},
        {DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, DSD_UA_FILTER_OTHER},
        {DSD_UA_CALL_ALERT, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, DSD_UA_FILTER_OTHER},
        {DSD_UA_ANSWER_REQUEST, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, DSD_UA_FILTER_OTHER},
        {DSD_UA_AUTHENTICATION, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, DSD_UA_FILTER_OTHER},
    };

    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_P25P1, cases[i].kind, cases[i].role, cases[i].result, 2000U + (uint32_t)i);
        const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
        if (row.filters != cases[i].filters) {
            DSD_FPRINTF(stderr, "case %zu: filters 0x%X, want 0x%X\n", i, row.filters, cases[i].filters);
            assert(0);
        }
    }
    free_state(st);
}

static void
test_warning_tone(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    uint32_t subject = 3000U;
    for (int k = DSD_UA_REGISTRATION; k <= DSD_UA_AUTHENTICATION; k++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_TETRA, (uint8_t)k, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, subject++);
        const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
        const int warn = k == DSD_UA_INHIBIT || k == DSD_UA_KILL;
        assert(row.tone == (warn ? DSD_UA_TONE_WARNING : DSD_UA_TONE_NORMAL));
    }
    free_state(st);
}

/* ---- Texts ---- */

static void
test_reason_text(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_unit_activity r = mk(DSD_UA_PROTO_P25P1, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 1234567U);
    r.reason_set = DSD_UA_REASON_P25_DENY;
    r.reason_code = 0x20U;
    r.value = 0x00U;
    r.value_valid = 1U;
    dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.kind_label, "Denied") == 0);
    assert(strcmp(row.reason_text, "Target Unit Not Valid (0x20)") == 0);
    assert(strcmp(row.target_text, "service 0x00") == 0);

    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED, 1234567U);
    r.reason_set = DSD_UA_REASON_P25_QUEUED;
    r.reason_code = 0x40U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.reason_text, "Channel Resources Unavailable (0x40)") == 0);

    r = mk(DSD_UA_PROTO_DMR, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 1234567U);
    r.reason_set = DSD_UA_REASON_DMR_TIER3;
    r.reason_code = 0x2FU;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.reason_text, "Talkgroup not allowed (0x2F)") == 0);

    r.reason_code = 0x3AU; /* no Table 7.43 entry */
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.reason_text, "Reason 0x3A") == 0);

    r = mk(DSD_UA_PROTO_NXDN, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 1234U);
    r.reason_set = DSD_UA_REASON_NXDN_MM;
    r.reason_code = 0x08U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.reason_text, "Registration is refused (0x08)") == 0);

    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 1234U);
    r.reason_set = DSD_UA_REASON_TETRA_LU_REJECT;
    r.reason_code = 3U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.reason_text, "LA not allowed (0x03)") == 0);

    r.reason_set = DSD_UA_REASON_NONE;
    row = view_of(st, &cursor, &r);
    assert(row.reason_text[0] == '\0');
    free_state(st);
}

static dsd_ua_scope
scope_of(uint8_t kind, uint32_t a, uint32_t b, uint32_t c) {
    dsd_ua_scope s;
    DSD_MEMSET(&s, 0, sizeof(s));
    s.kind = kind;
    s.valid = 1U;
    s.a = a;
    s.b = b;
    s.c = c;
    return s;
}

static void
test_scope_and_protocol_text(void) {
    struct {
        uint8_t protocol;
        dsd_ua_scope scope;
        const char* scope_text;
        const char* protocol_text;
    } const cases[] = {
        {DSD_UA_PROTO_P25P1, scope_of(DSD_UA_SCOPE_P25, WACN_A, SYS_A, DSD_UA_SCOPE_SITE_KNOWN | 0x0102U),
         "WACN BEE00 SYS 123 RFSS 1 SITE 2", "P25 Phase 1"},
        {DSD_UA_PROTO_P25P2, scope_of(DSD_UA_SCOPE_P25, WACN_A, SYS_B, 0U), "WACN BEE00 SYS 456", "P25 Phase 2"},
        {DSD_UA_PROTO_P25P1, scope_of(DSD_UA_SCOPE_P25_NAC, 0x2A3U, 0U, 0U), "NAC 2A3", "P25 Phase 1"},
        {DSD_UA_PROTO_DMR,
         scope_of(DSD_UA_SCOPE_DMR, (DSD_RXID_DMR_TRUNK_TIER3 << 8) | 1U, 17U, DSD_UA_SCOPE_SITE_KNOWN | 5U),
         "Net 17 Site 5", "DMR"},
        {DSD_UA_PROTO_DMR, scope_of(DSD_UA_SCOPE_DMR, (DSD_RXID_DMR_TRUNK_CONNECT_PLUS << 8), 9U, 0U), "Net 9", "DMR"},
        {DSD_UA_PROTO_DMR,
         scope_of(DSD_UA_SCOPE_DMR, (DSD_RXID_DMR_TRUNK_CAPACITY_PLUS << 8) | 3U, 4U, DSD_UA_SCOPE_SITE_KNOWN | 4U),
         "Site 4 CC 3", "DMR"},
        {DSD_UA_PROTO_NXDN, scope_of(DSD_UA_SCOPE_NXDN, 0x101U, 77U, DSD_UA_SCOPE_SITE_KNOWN | 6U), "SYS 77 SITE 6",
         "NXDN"},
        {DSD_UA_PROTO_TETRA, scope_of(DSD_UA_SCOPE_TETRA, 901U, 9999U, DSD_UA_SCOPE_SITE_KNOWN | 12U),
         "MCC 901 MNC 9999 LA 12", "TETRA"},
        {DSD_UA_PROTO_TETRA, scope_of(DSD_UA_SCOPE_TETRA, 901U, 9999U, 0U), "MCC 901 MNC 9999", "TETRA"},
    };

    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        dsd_unit_activity r = mk(cases[i].protocol, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED,
                                 4000U + (uint32_t)i);
        r.scope = cases[i].scope;
        const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
        if (strcmp(row.scope_text, cases[i].scope_text) != 0
            || strcmp(row.protocol_text, cases[i].protocol_text) != 0) {
            DSD_FPRINTF(stderr, "case %zu: '%s' / '%s'\n", i, row.scope_text, row.protocol_text);
            assert(0);
        }
    }
    /* No system named yet (rx_id empty): no scope text. */
    const dsd_unit_activity r = mk(DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 5U);
    assert(view_of(st, &cursor, &r).scope_text[0] == '\0');
    free_state(st);
}

static void
test_subject_and_target_text(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};

    /* A P25 registration of home ID 1234567 assigned WUID 100. */
    dsd_unit_activity r =
        mk(DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 1234567U);
    set_identity(&r.assigned, 100U, DSD_UA_ID_RADIO);
    dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_text, "1234567") == 0);
    assert(strcmp(row.target_text, "as 100") == 0);

    /* A location registration with its talkgroup. */
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 77U);
    r.group = 100U;
    r.group_valid = 1U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "TG 100") == 0);

    /* A DMR kill sent through the KILLI gateway: the gateway is the requester, never the affected radio. */
    r = mk(DSD_UA_PROTO_DMR, DSD_UA_KILL, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 1234567U);
    set_identity(&r.actor, 0xFFFECFU, DSD_UA_ID_INFRASTRUCTURE);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_text, "1234567") == 0);
    assert(strcmp(row.target_text, "from System 0xFFFECF") == 0);
    assert(row.tone == DSD_UA_TONE_WARNING);

    /* A P25 inhibit acknowledged, with the requester in the actor. */
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_INHIBIT, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 1234567U);
    set_identity(&r.actor, 55U, DSD_UA_ID_RADIO);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.kind_label, "Inhibit acknowledged") == 0);
    assert(strcmp(row.target_text, "from 55") == 0);

    /* DMR talkgroup subscription responses: a mask, never invented talkgroup IDs. */
    r = mk(DSD_UA_PROTO_DMR, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_NA, 1234567U);
    r.value = 0U;
    r.value_valid = 1U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "all rejected") == 0);
    r.value = 0x5U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "accepted mask 0x5") == 0);

    /* A TETRA location update accepted as an ITSI attach, with its assigned SSI. */
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 2001U);
    r.value = 3U;
    r.value_valid = 1U;
    set_identity(&r.assigned, 3001U, DSD_UA_ID_RADIO);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "ITSI attach, as 3001") == 0);

    /* Identity kinds that are never radios. */
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0U);
    set_identity(&r.subject, 0xABCU, DSD_UA_ID_TEI_PARTIAL);
    DSD_SNPRINTF(r.name_frozen, sizeof r.name_frozen, "%s", "never shown");
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_text, "Equipment 0xABC") == 0);
    assert(row.subject_name[0] == '\0');
    set_identity(&r.subject, 77U, DSD_UA_ID_UNKNOWN);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_text, "Address 77") == 0);
    assert(row.subject_name[0] == '\0');
    set_identity(&r.subject, 500U, DSD_UA_ID_GROUP);
    DSD_MEMSET(r.name_frozen, 0, sizeof r.name_frozen);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_text, "TG 500") == 0);
    free_state(st);
}

/* Rows that differ only in value2 (a keyed field) read differently: each value2 is shown where it has a meaning. */
static void
test_value2_tells_rows_apart(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};

    /* Motorola ARS device (PDU type 0x00) and user (0x05) registrations of one radio. */
    dsd_unit_activity r = mk(DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 1234U);
    r.value2 = 0x00U;
    r.value2_valid = 1U;
    dsd_app_unit_activity_row device = view_of(st, &cursor, &r);
    r.value2 = 0x05U;
    dsd_app_unit_activity_row user = view_of(st, &cursor, &r);
    assert(strcmp(device.target_text, user.target_text) != 0);
    assert(strcmp(device.target_text, "device") == 0);
    assert(strcmp(user.target_text, "user") == 0);
    r = mk(DSD_UA_PROTO_DMR, DSD_UA_DEREGISTRATION, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 1234U);
    r.value2 = 0x06U;
    r.value2_valid = 1U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "user") == 0);

    /* Two P25 denies differing only in the Additional Information (the request's target). */
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 1234567U);
    r.value = 0x04U;
    r.value_valid = 1U;
    r.value2 = 2345U;
    r.value2_valid = 1U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "service 0x04, for 2345") == 0);
    r.value2 = 2346U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "service 0x04, for 2346") == 0);

    /* NXDN: a visitor's group, a remote control command's parameter. */
    r = mk(DSD_UA_PROTO_NXDN, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 1001U);
    r.group = 200U;
    r.group_valid = 1U;
    r.value2 = 400U;
    r.value2_valid = 1U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "TG 200, visitor TG 400") == 0);
    r = mk(DSD_UA_PROTO_NXDN, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 2002U);
    r.value2 = 0x8000U;
    r.value2_valid = 1U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "parameter 0x8000") == 0);

    /* TETRA: a rejected location update's type; an equipment command's whole TEI. */
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 2001U);
    r.value2 = 3U;
    r.value2_valid = 1U;
    assert(strcmp(view_of(st, &cursor, &r).target_text, "update type 3") == 0);
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_STUN, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 0U);
    set_identity(&r.subject, 0x12345678U, DSD_UA_ID_TEI_PARTIAL);
    r.value2 = 0x0ABCDEF0U;
    r.value2_valid = 1U;
    dsd_app_unit_activity_row tei = view_of(st, &cursor, &r);
    assert(strcmp(tei.subject_text, "Equipment 0xABCDEF012345678") == 0);
    assert(tei.target_text[0] == '\0');
    free_state(st);
}

/* A status report is attributed to the unit that sent it: the source is the subject, the target its recipient. */
static void
test_status_report_subject_is_source(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_unit_activity r = mk(DSD_UA_PROTO_P25P1, DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 2345U);
    set_identity(&r.recipient, 6789U, DSD_UA_ID_RADIO);
    r.value = 0xA10BU; /* User Status A1, Unit Status 0B */
    r.value_valid = 1U;
    const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.kind_label, "Status") == 0);
    assert(strcmp(row.subject_text, "2345") == 0);
    assert(strcmp(row.target_text, "status A1/0B, to 6789") == 0);
    free_state(st);
}

/* A P25 short data message reads in hex, as the P25 prints show it. */
static void
test_p25_message_value_hex(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_unit_activity r = mk(DSD_UA_PROTO_P25P2, DSD_UA_MESSAGE, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 2345U);
    set_identity(&r.recipient, 6789U, DSD_UA_ID_RADIO);
    r.value = 0x00ABU;
    r.value_valid = 1U;
    const dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.kind_label, "Message") == 0);
    assert(strcmp(row.target_text, "message 00AB, to 6789") == 0);
    free_state(st);
}

/* Received names reach the row as plain UTF-8: a control character turns into a space, a character the producer cut
 * short is dropped. */
static void
test_name_text_is_clean(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_unit_activity r = mk(DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 9U);
    DSD_MEMCPY(r.name_frozen, "A\tB\xC3\xA9", 6);
    dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_name, "A B\xC3\xA9") == 0);
    r.subject.id = 10U;
    DSD_MEMSET(r.name_frozen, 0, sizeof r.name_frozen);
    DSD_MEMCPY(r.name_frozen, "Ab\xE2\x82", 4); /* a three-byte character cut after two */
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_name, "Ab") == 0);
    /* Not UTF-8 (an overlong '/', a surrogate, a code point above U+10FFFF) is dropped; a C1 control is a space. */
    r.subject.id = 11U;
    DSD_MEMSET(r.name_frozen, 0, sizeof r.name_frozen);
    DSD_MEMCPY(r.name_frozen,
               "a\xC0\xAF"
               "b\xED\xA0\x80"
               "c\xF4\x90\x80\x80"
               "d\xC2\x85"
               "e",
               17);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.subject_name, "abcd e") == 0);
    free_state(st);
}

/* ---- Target text that would not fit ---- */

static int
add_group_label(dsd_state* st, uint32_t tg, const char* name) {
    dsd_tg_policy_entry entry;
    if (dsd_tg_policy_make_exact_entry(tg, "A", name, DSD_TG_POLICY_SOURCE_IMPORTED, &entry) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_exact(st, &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static int
ends_with(const char* text, const char* tail) {
    const size_t n = strlen(text);
    const size_t m = strlen(tail);
    return n >= m && strcmp(text + n - m, tail) == 0;
}

/* target_text never cuts an ID or a value: the longest TETRA accept type keeps its assigned SSI, a long talkgroup
 * label is shortened (with "…") before an identity is touched, and when even that is not enough a whole part is left
 * out and the text ends ", …". */
static void
test_target_text_never_cuts_a_part(void) {
    /* A 49-byte label, as long as a list row holds. */
    static const char k_label49[] = "Group Alpha Talkgroup Label Long Text Number 0001";
    _Static_assert(sizeof(k_label49) == DSD_SOURCE_ALIAS_NAME_MAX, "the longest label a row holds");
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    assert(add_group_label(st, 100U, k_label49) == 0);
    assert(add_group_label(st, 4294967295U, k_label49) == 0);

    /* TETRA accept type 5 (the longest, 60 bytes) with an assigned SSI. */
    dsd_unit_activity r =
        mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 2001U);
    r.value = 5U;
    r.value_valid = 1U;
    set_identity(&r.assigned, 1234567U, DSD_UA_ID_RADIO);
    dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "Migrating or service restoration migrating location updating, as 1234567") == 0);

    /* A long label with a requester: both whole. */
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 2002U);
    r.group = 100U;
    r.group_valid = 1U;
    set_identity(&r.actor, 1234567U, DSD_UA_ID_RADIO);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "TG 100 Group Alpha Talkgroup Label Long Text Number 0001, from 1234567") == 0);

    /* Every part at its longest: the label gives way first, every ID stays whole. */
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 2003U);
    r.group = 100U;
    r.group_valid = 1U;
    r.value = 5U;
    r.value_valid = 1U;
    set_identity(&r.assigned, 4294967295U, DSD_UA_ID_RADIO);
    set_identity(&r.recipient, 4294967295U, DSD_UA_ID_RADIO);
    set_identity(&r.actor, 0xFFFFFFFFU, DSD_UA_ID_INFRASTRUCTURE);
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text,
                  "TG 100 G\xE2\x80\xA6, Migrating or service restoration migrating location updating, as 4294967295, "
                  "to 4294967295, from System 0xFFFFFFFF")
           == 0);
    assert(strlen(row.target_text) < sizeof row.target_text);

    /* With the longest talkgroup number too, not even the shortened label fits: the label goes, then the last part,
     * and the text says so. */
    r.group = 4294967295U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text,
                  "TG 4294967295, Migrating or service restoration migrating location updating, as 4294967295, "
                  "to 4294967295, \xE2\x80\xA6")
           == 0);
    assert(ends_with(row.target_text, ", \xE2\x80\xA6"));

    /* An accept type the 3-bit field cannot hold is shown raw, never as a real type. */
    r = mk(DSD_UA_PROTO_TETRA, DSD_UA_LOCATION_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 2004U);
    r.value = 13U;
    r.value_valid = 1U;
    row = view_of(st, &cursor, &r);
    assert(strcmp(row.target_text, "accept type 13") == 0);
    free_state(st);
}

/* ---- Headline ---- */

static void
test_headline(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    char headline[DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE];

    dsd_unit_activity r = mk(DSD_UA_PROTO_P25P1, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 1U);
    r.reason_set = DSD_UA_REASON_P25_DENY;
    r.reason_code = 0x20U;
    dsd_app_unit_activity_row row = view_of(st, &cursor, &r);
    dsd_app_unit_activity_headline(&row, headline, sizeof headline);
    assert(strcmp(headline, "Denied: Target Unit Not Valid (0x20)") == 0);

    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_QUEUED, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_QUEUED, 2U);
    row = view_of(st, &cursor, &r); /* no reason */
    dsd_app_unit_activity_headline(&row, headline, sizeof headline);
    assert(strcmp(headline, "Queued") == 0);

    /* Another kind keeps its reason on its own line. */
    r = mk(DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_REFUSED, 3U);
    r.reason_set = DSD_UA_REASON_DMR_TIER3;
    r.reason_code = 0x2AU;
    row = view_of(st, &cursor, &r);
    dsd_app_unit_activity_headline(&row, headline, sizeof headline);
    assert(strcmp(headline, "Registration refused") == 0);

    /* The longest label and reason fit the documented size. */
    r = mk(DSD_UA_PROTO_DMR, DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 4U);
    r.reason_set = DSD_UA_REASON_DMR_TIER3;
    r.reason_code = 0x65U;
    row = view_of(st, &cursor, &r);
    dsd_app_unit_activity_headline(&row, headline, sizeof headline);
    assert(strcmp(headline, "Denied: Registration with talkgroup subscription / attachment (0x65)") == 0);
    /* Cut at a character boundary, never past cap. */
    dsd_app_unit_activity_headline(&row, headline, 9U);
    assert(strcmp(headline, "Denied: ") == 0);
    dsd_app_unit_activity_headline(NULL, headline, sizeof headline);
    assert(headline[0] == '\0');
    dsd_app_unit_activity_headline(&row, NULL, 4U);
    free_state(st);
}

/* ---- Reading ---- */

static void
test_pagination_and_merge(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    for (uint32_t i = 0; i < 10U; i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 100U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_row out[4];
    dsd_app_unit_activity_meta meta;
    const int want[] = {3, 3, 3, 1};
    uint64_t seq = 1U;
    for (int page = 0; page < 4; page++) {
        const int n = dsd_app_unit_activity_rows(st, &cursor, out, 3, &meta);
        assert(n == want[page]);
        assert(meta.held == 1U);
        assert(meta.more == (page < 3 ? 1U : 0U));
        assert(meta.reset == (page == 0 ? 1U : 0U));
        assert(meta.lost == 0U);
        for (int i = 0; i < n; i++) {
            assert(out[i].seq == seq);
            seq++;
            assert(out[i].repeat == 1U);
        }
    }
    assert(dsd_app_unit_activity_rows(st, &cursor, out, 3, &meta) == 0 && meta.more == 0U);

    /* The third row repeats: it comes back alone, with its seq, a new revision and its repeat count. */
    const dsd_unit_activity again =
        mk(DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 102U);
    assert(dsd_unit_activity_push(st, &again) == 0);
    assert(dsd_app_unit_activity_rows(st, &cursor, out, 3, &meta) == 1);
    assert(out[0].seq == 3U && out[0].repeat == 2U && out[0].rev == 11U);

    /* cap 0 reads nothing and keeps the cursor. */
    const dsd_app_unit_activity_cursor before = cursor;
    assert(dsd_unit_activity_push(st, &again) == 0);
    assert(dsd_app_unit_activity_rows(st, &cursor, NULL, 0, &meta) == 0);
    assert(meta.more == 1U && cursor.rev == before.rev && cursor.instance == before.instance);
    free_state(st);
}

/* A reader that comes back after the ring wrapped is told once, and gets the newest rows without a duplicate. */
static void
test_reader_lost_after_wrap(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    dsd_app_unit_activity_row* rows =
        (dsd_app_unit_activity_row*)calloc(DSD_UNIT_ACTIVITY_CAPACITY + 8, sizeof(dsd_app_unit_activity_row));
    assert(rows != NULL);
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;

    /* No log yet: nothing, and the cursor stays. */
    assert(dsd_app_unit_activity_rows(st, &cursor, rows, 4, &meta) == 0);
    assert(meta.held == 0U && cursor.instance == 0U && cursor.rev == 0U);

    /* First attach without a wrap: a new log, nothing lost. */
    for (uint32_t i = 0; i < 4U; i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 10U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    assert(read_all(st, &cursor, rows, DSD_UNIT_ACTIVITY_CAPACITY + 8, &meta) == 4);
    assert(meta.reset == 1U && meta.lost == 0U);

    /* Away while the ring wraps past the unread rows. */
    const uint32_t pushes = DSD_UNIT_ACTIVITY_CAPACITY + 5U;
    for (uint32_t i = 0; i < pushes; i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, 100000U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    const int n = read_all(st, &cursor, rows, DSD_UNIT_ACTIVITY_CAPACITY + 8, &meta);
    assert(n == DSD_UNIT_ACTIVITY_CAPACITY);
    assert(meta.lost == 1U && meta.reset == 0U);
    for (int i = 1; i < n; i++) {
        assert(rows[i].seq == rows[i - 1].seq + 1U);
    }
    assert(rows[n - 1].seq == 4U + pushes);
    /* Told once. */
    assert(read_all(st, &cursor, rows, 4, &meta) == 0);
    assert(meta.lost == 0U);
    free(rows);
    free_state(st);
}

/* One read larger than the view's internal page (16 records): rows stay in order across the seam, the cursor ends on
 * the last one, and a loss is reported for the read that crossed it. */
static void
test_read_across_pages(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    for (uint32_t i = 0; i < 40U; i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 500U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    dsd_app_unit_activity_row* rows =
        (dsd_app_unit_activity_row*)calloc(DSD_UNIT_ACTIVITY_CAPACITY, sizeof(dsd_app_unit_activity_row));
    assert(rows != NULL);
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;
    assert(dsd_app_unit_activity_rows(st, &cursor, rows, 64, &meta) == 40);
    assert(meta.reset == 1U && meta.lost == 0U && meta.more == 0U);
    for (int i = 0; i < 40; i++) {
        assert(rows[i].seq == (uint64_t)i + 1U);
        assert(strcmp(rows[i].subject_text, "") != 0);
    }
    assert(cursor.rev == 40U);

    /* Wrap past the cursor, then read it all in one call of 37 pages' worth. */
    for (uint32_t i = 0; i < DSD_UNIT_ACTIVITY_CAPACITY + 20U; i++) {
        const dsd_unit_activity r =
            mk(DSD_UA_PROTO_DMR, DSD_UA_REGISTRATION, DSD_UA_ROLE_ACK, DSD_UA_RESULT_ACKNOWLEDGED, 9000U + i);
        assert(dsd_unit_activity_push(st, &r) == 1);
    }
    const int n = dsd_app_unit_activity_rows(st, &cursor, rows, DSD_UNIT_ACTIVITY_CAPACITY, &meta);
    assert(n == DSD_UNIT_ACTIVITY_CAPACITY);
    assert(meta.lost == 1U && meta.reset == 0U && meta.more == 0U);
    for (int i = 1; i < n; i++) {
        assert(rows[i].seq == rows[i - 1].seq + 1U);
    }
    assert(rows[n - 1].seq == 40U + DSD_UNIT_ACTIVITY_CAPACITY + 20U);
    assert(cursor.rev == rows[n - 1].rev);
    assert(dsd_app_unit_activity_rows(st, &cursor, rows, 4, &meta) == 0 && meta.lost == 0U);
    free(rows);
    free_state(st);
}

static void
test_bad_arguments(void) {
    dsd_state* st = new_state();
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_row row;
    dsd_app_unit_activity_meta meta;
    assert(dsd_app_unit_activity_rows(NULL, &cursor, &row, 1, &meta) == -1);
    assert(dsd_app_unit_activity_rows(st, NULL, &row, 1, &meta) == -1);
    assert(dsd_app_unit_activity_rows(st, &cursor, &row, 1, NULL) == -1);
    assert(dsd_app_unit_activity_rows(st, &cursor, &row, -1, &meta) == -1);
    assert(dsd_app_unit_activity_rows(st, &cursor, NULL, 1, &meta) == -1);
    assert(dsd_app_unit_activity_name_key(NULL) == 0U);
    assert(dsd_app_unit_activity_name_key(st) != 0U);
    free_state(st);
}

/* ---- Names ---- */

/* A frontend's copy of @p live: the ring, the group and radio lists, and the reception. */
static dsd_state*
snapshot_of(const dsd_state* live) {
    dsd_state* copy = new_state();
    copy->rx_id = live->rx_id;
    assert(dsd_unit_activity_copy_snapshot(copy, live) == 0);
    assert(dsd_tg_policy_copy_snapshot(copy, live) == 0);
    assert(dsd_source_alias_copy_snapshot(copy, live) == 0);
    return copy;
}

/* Every row of @p st, read from a fresh cursor (as a frontend reads again when the name key changes). */
static int
rows_of(const dsd_state* st, dsd_app_unit_activity_row* out, int cap) {
    dsd_app_unit_activity_cursor cursor = {0, 0};
    dsd_app_unit_activity_meta meta;
    return read_all(st, &cursor, out, cap, &meta);
}

static dsd_unit_activity
rid_row(const char* frozen) {
    dsd_unit_activity r =
        mk(DSD_UA_PROTO_P25P1, DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, RID);
    r.group = 100U;
    r.group_valid = 1U;
    DSD_SNPRINTF(r.name_frozen, sizeof r.name_frozen, "%s", frozen);
    return r;
}

/* Trunk scan A -> B -> A: both systems use RID 42 with their own learned aliases. Each row shows its own system's
 * name, through a frontend snapshot, and the name key moves with the scope. */
static void
test_names_follow_row_scope(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    dsd_app_unit_activity_row rows[8];

    tune_p25(st, SYS_A);
    assert(dsd_alias_learn_radio(NULL, st, NULL, RID, "Alice", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    dsd_unit_activity r = rid_row("Alice");
    assert(dsd_unit_activity_push(st, &r) == 1); /* seq 1, scope A */
    const uint64_t key_a = dsd_app_unit_activity_name_key(st);

    tune_p25(st, SYS_B);
    assert(dsd_alias_learn_radio(NULL, st, NULL, RID, "Bob", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    r = rid_row("Bob");
    assert(dsd_unit_activity_push(st, &r) == 1); /* seq 2, scope B */
    const uint64_t key_b = dsd_app_unit_activity_name_key(st);
    assert(key_b != key_a);

    /* On B: B's row is Bob from the live list, A's row keeps what was heard on A. */
    dsd_state* snap = snapshot_of(st);
    assert(rows_of(snap, rows, 8) == 2);
    assert(strcmp(find_seq(rows, 2, 1U)->subject_name, "Alice") == 0);
    assert(strcmp(find_seq(rows, 2, 2U)->subject_name, "Bob") == 0);
    assert(strcmp(find_seq(rows, 2, 1U)->scope_text, "WACN BEE00 SYS 123") == 0);
    assert(strcmp(find_seq(rows, 2, 2U)->scope_text, "WACN BEE00 SYS 456") == 0);
    free_state(snap);

    /* Back on A: the same answers. */
    tune_p25(st, SYS_A);
    assert(dsd_app_unit_activity_name_key(st) != key_b); /* the scope moved: a frontend reads its rows again */
    r = rid_row("Alice");
    r.group = 101U;
    assert(dsd_unit_activity_push(st, &r) == 1); /* seq 3, scope A */
    snap = snapshot_of(st);
    assert(rows_of(snap, rows, 8) == 3);
    assert(strcmp(find_seq(rows, 3, 1U)->subject_name, "Alice") == 0);
    assert(strcmp(find_seq(rows, 3, 2U)->subject_name, "Bob") == 0);
    assert(strcmp(find_seq(rows, 3, 3U)->subject_name, "Alice") == 0);
    free_state(snap);

    /* A's own radio list (a trunk-scan target's list, no scope of its own) names A's radio 42; it never reaches B's
     * row, which keeps what B's producer stored. */
    dsd_source_alias_store* radios = dsd_source_alias_store_create();
    dsd_source_alias_entry desk = {.id_start = RID, .id_end = RID};
    DSD_SNPRINTF(desk.name, sizeof desk.name, "%s", "Desk A");
    assert(radios && dsd_source_alias_store_append(radios, &desk) == 0);
    dsd_source_alias_install(st, radios);
    snap = snapshot_of(st);
    assert(rows_of(snap, rows, 8) == 3);
    assert(strcmp(find_seq(rows, 3, 1U)->subject_name, "Desk A") == 0);
    assert(strcmp(find_seq(rows, 3, 2U)->subject_name, "Bob") == 0);
    assert(strcmp(find_seq(rows, 3, 3U)->subject_name, "Desk A") == 0);
    free_state(snap);
    free_state(st);
}

/* A late alias renames the rows of its own system, never another system's rows with the same ID. */
static void
test_late_alias_renames_same_scope_only(void) {
    dsd_state* st = new_state();
    dsd_decode_clock_use_test(T0_NS);
    dsd_app_unit_activity_row rows[8];

    tune_p25(st, SYS_B);
    dsd_unit_activity r = rid_row("");
    assert(dsd_unit_activity_push(st, &r) == 1); /* seq 1, scope B, unnamed */
    tune_p25(st, SYS_A);
    r = rid_row("");
    assert(dsd_unit_activity_push(st, &r) == 1); /* seq 2, scope A, unnamed */
    assert(rows_of(st, rows, 8) == 2);
    assert(rows[0].subject_name[0] == '\0' && rows[1].subject_name[0] == '\0');

    const uint64_t before = dsd_app_unit_activity_name_key(st);
    assert(dsd_alias_learn_radio(NULL, st, NULL, RID, "Alice", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    assert(dsd_app_unit_activity_name_key(st) != before);
    assert(rows_of(st, rows, 8) == 2);
    assert(find_seq(rows, 2, 1U)->subject_name[0] == '\0');            /* B's radio 42 is not Alice */
    assert(strcmp(find_seq(rows, 2, 2U)->subject_name, "Alice") == 0); /* A's is */

    /* A rename on A follows; B's row still has no name. */
    assert(dsd_alias_learn_radio(NULL, st, NULL, RID, "Alicia", NULL) == DSD_TG_POLICY_LEARN_REPLACED);
    assert(rows_of(st, rows, 8) == 2);
    assert(strcmp(find_seq(rows, 2, 2U)->subject_name, "Alicia") == 0);
    assert(find_seq(rows, 2, 1U)->subject_name[0] == '\0');

    /* A registration names the radio by its assigned (serving) ID first. */
    assert(dsd_alias_learn_radio(NULL, st, NULL, 100U, "Serving", NULL) == DSD_TG_POLICY_LEARN_ADDED);
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, RID);
    set_identity(&r.assigned, 100U, DSD_UA_ID_RADIO);
    assert(dsd_unit_activity_push(st, &r) == 1);
    assert(rows_of(st, rows, 8) == 3);
    assert(strcmp(find_seq(rows, 3, 3U)->subject_name, "Serving") == 0);

    /* A roaming radio's home ID (another system's) is never named from this system's lists; its ID of this system
     * is. Unassigned, it keeps what its producer stored. */
    r = mk(DSD_UA_PROTO_P25P1, DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, RID);
    r.subject.home_valid = 1U;
    r.subject.home_kind = DSD_UA_SCOPE_P25;
    r.subject.home_a = WACN_A;
    r.subject.home_b = SYS_B;
    assert(dsd_unit_activity_push(st, &r) == 1);
    r.subject.home_b = SYS_A; /* its home is this system: its own ID is named */
    assert(dsd_unit_activity_push(st, &r) == 1);
    assert(rows_of(st, rows, 8) == 5);
    assert(find_seq(rows, 5, 4U)->subject_name[0] == '\0');
    assert(strcmp(find_seq(rows, 5, 5U)->subject_name, "Alicia") == 0);
    free_state(st);
}

int
main(void) {
    test_every_kind_role_result_label();
    test_labels_never_claim_outcome();
    test_filter_membership();
    test_warning_tone();
    test_reason_text();
    test_scope_and_protocol_text();
    test_subject_and_target_text();
    test_value2_tells_rows_apart();
    test_status_report_subject_is_source();
    test_p25_message_value_hex();
    test_name_text_is_clean();
    test_pagination_and_merge();
    test_reader_lost_after_wrap();
    test_read_across_pages();
    test_target_text_never_cuts_a_part();
    test_headline();
    test_bad_arguments();
    test_names_follow_row_scope();
    test_late_alias_renames_same_scope_only();
    printf("APP_CONTROL_UNIT_ACTIVITY: OK\n");
    return 0;
}
