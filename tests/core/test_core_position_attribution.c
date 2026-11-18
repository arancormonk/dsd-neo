// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The attribution rule (issue #800, <dsd-neo/core/position_attribution.h>) as a matrix: evidence learned for one call
 * is granted to a report only on that call (epoch, or transmission for a producer bound to it), of the report's
 * protocol class, under the reception it was learned under, while the call still names its source (and its target for
 * the target). Also the fixed-network source rule admission applies to every producer
 * (dsd_position_source_names_radio()). IDs are made up.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_fail;

static void
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_fail = 1;
    }
}

#define SOURCE 3001U
#define TARGET 100U

static dsd_state*
new_state(void) {
    dsd_state* st = (dsd_state*)calloc(1U, sizeof(*st));
    if (!st) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        exit(2);
    }
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

/* Open slot 0's call of @p protocol from SOURCE to TARGET (a new epoch). */
static void
open_call(dsd_state* st, int protocol, uint64_t source, uint64_t target) {
    dsd_call_observation obs = dsd_call_observation_data(protocol, 0U, source, target);
    obs.kind = DSD_CALL_KIND_GROUP_VOICE;
    obs.policy_target_id = target;
    if (dsd_call_state_observe(st, &obs, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        DSD_FPRINTF(stderr, "call did not open\n");
        exit(2);
    }
}

/* The protocol classes: P25 Phase 1 and 2 apart, DMR base and mobile bursts one class. */
static void
test_classes(void) {
    expect_true("p25p1 pos/neg", dsd_position_attribution_class(DSD_SYNC_P25P1_POS)
                                     == dsd_position_attribution_class(DSD_SYNC_P25P1_NEG));
    expect_true("p25p2 pos/neg", dsd_position_attribution_class(DSD_SYNC_P25P2_POS)
                                     == dsd_position_attribution_class(DSD_SYNC_P25P2_NEG));
    expect_true("p25 phases apart", dsd_position_attribution_class(DSD_SYNC_P25P1_POS)
                                        != dsd_position_attribution_class(DSD_SYNC_P25P2_POS));
    expect_true("dmr bs/ms one class", dsd_position_attribution_class(DSD_SYNC_DMR_BS_VOICE_POS)
                                           == dsd_position_attribution_class(DSD_SYNC_DMR_MS_DATA));
    expect_true("dmr is not p25", dsd_position_attribution_class(DSD_SYNC_DMR_BS_VOICE_POS)
                                      != dsd_position_attribution_class(DSD_SYNC_P25P1_POS));
}

typedef enum {
    CASE_SAME = 0,         /* nothing changed: granted */
    CASE_CALL_CLOSED,      /* the call ended */
    CASE_NEW_EPOCH,        /* another call (a new talker's) on the slot */
    CASE_OTHER_CLASS_CALL, /* the slot's call is of another protocol class */
    CASE_REPORT_CLASS,     /* the report is of another protocol class */
    CASE_ACCESS_CHANGE,    /* another access code (colour code, NAC) on the carrier */
    CASE_SYSTEM_CHANGE,    /* another system named on the carrier */
    CASE_ACCESS_REPEAT,    /* the same access code noted again: granted */
    CASE_TARGET_CHANGE,    /* the call no longer names the target: the source only */
    CASE_COUNT,
} attribution_case;

static const char* const k_case_names[CASE_COUNT] = {
    "same",          "call closed",   "new epoch",     "other class call", "report class",
    "access change", "system change", "access repeat", "target change",
};

/* One producer's protocol: the call's, the report's, and the binding it learns with. */
typedef struct {
    const char* name;
    int protocol;
    int other_class;
    dsd_rxid_family family;
    uint8_t access_kind;
    dsd_position_attribution_bind bind;
} producer;

static void
apply_case(dsd_state* st, const producer* p, attribution_case c) {
    switch (c) {
        case CASE_CALL_CLOSED: (void)dsd_call_state_end(st, 0U, 0.0); break;
        case CASE_NEW_EPOCH: open_call(st, p->protocol, SOURCE + 1U, TARGET); break;
        case CASE_ACCESS_CHANGE: dsd_rxid_note_access(st, p->family, p->access_kind, 2U); break;
        case CASE_SYSTEM_CHANGE:
            if (p->family == DSD_RXID_FAMILY_P25) {
                dsd_rxid_note_p25_network(st, 0xBEE00U, 0x1A4U);
            } else {
                dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 4U, 0U, NULL);
            }
            break;
        case CASE_ACCESS_REPEAT: dsd_rxid_note_access(st, p->family, p->access_kind, 1U); break;
        default: break;
    }
}

/* A state on the producer's reception (access code 1, a system or site), with slot 0's call open. */
static dsd_state*
matrix_state(const producer* p) {
    dsd_state* st = new_state();
    dsd_rxid_note_access(st, p->family, p->access_kind, 1U);
    if (p->family == DSD_RXID_FAMILY_P25) {
        dsd_rxid_note_p25_network(st, 0xBEE00U, 0x1A3U);
    } else {
        dsd_rxid_note_dmr_site(st, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS, 0, 0U, 3U, 0U, NULL);
    }
    open_call(st, p->protocol, SOURCE, TARGET);
    return st;
}

/* What @p c grants: the source (and the target unless the call no longer names it), or nothing. */
static void
matrix_expect(const producer* p, attribution_case c, int ok, const dsd_position_attribution* granted) {
    const int want = c == CASE_SAME || c == CASE_ACCESS_REPEAT || c == CASE_TARGET_CHANGE;
    const int names_target = c != CASE_TARGET_CHANGE;
    char tag[160];
    DSD_SNPRINTF(tag, sizeof tag, "%s %s: %s", p->name, k_case_names[c], want ? "granted" : "refused");
    expect_true(tag, ok == want && granted->source == (want ? SOURCE : 0U));
    DSD_SNPRINTF(tag, sizeof tag, "%s %s: target", p->name, k_case_names[c]);
    expect_true(tag,
                !ok || (granted->target == (names_target ? TARGET : 0U) && granted->group == (uint8_t)names_target));
}

static void
test_matrix(const producer* p) {
    for (int c = 0; c < (int)CASE_COUNT; c++) {
        dsd_state* st = matrix_state(p);
        dsd_position_attribution_call call;
        dsd_position_attribution evidence;
        dsd_position_attribution_clear(&evidence);
        char tag[160];
        DSD_SNPRINTF(tag, sizeof tag, "%s %s: learned", p->name, k_case_names[c]);
        expect_true(tag, dsd_position_attribution_call_from_slot(st, 0U, &call) == 1
                             && dsd_position_attribution_learn(st, &call, SOURCE, TARGET, 1, p->bind, &evidence) == 1);

        apply_case(st, p, (attribution_case)c);
        (void)dsd_position_attribution_call_from_slot(st, 0U, &call);
        // The same call, seen as another protocol class's or as naming another target.
        if (c == CASE_OTHER_CLASS_CALL) {
            call.protocol = p->other_class;
        } else if (c == CASE_TARGET_CHANGE) {
            call.target = TARGET + 1U;
        }
        const int report = c == CASE_REPORT_CLASS ? p->other_class : p->protocol;
        dsd_position_attribution granted;
        const int ok = dsd_position_attribution_grant(st, &evidence, &call, report, &granted);
        matrix_expect(p, (attribution_case)c, ok, &granted);
        free_state(st);
    }
}

/* Learning needs an open call that holds the source; the target only while the call holds it. */
static void
test_learn(void) {
    dsd_state* st = new_state();
    dsd_position_attribution_call call;
    dsd_position_attribution evidence;
    dsd_position_attribution_clear(&evidence);
    expect_true("no call: not learned",
                dsd_position_attribution_call_from_slot(st, 0U, &call) == 0
                    && dsd_position_attribution_learn(st, &call, SOURCE, TARGET, 1, DSD_POSITION_ATTRIBUTION_BIND_EPOCH,
                                                      &evidence)
                           == 0);
    open_call(st, DSD_SYNC_P25P1_POS, SOURCE, TARGET);
    (void)dsd_position_attribution_call_from_slot(st, 0U, &call);
    expect_true("another source: not learned",
                dsd_position_attribution_learn(st, &call, SOURCE + 1U, TARGET, 1, DSD_POSITION_ATTRIBUTION_BIND_EPOCH,
                                               &evidence)
                        == 0
                    && evidence.call_epoch == 0U);
    expect_true("no source: not learned",
                dsd_position_attribution_learn(st, &call, 0U, TARGET, 1, DSD_POSITION_ATTRIBUTION_BIND_EPOCH, &evidence)
                    == 0);
    expect_true("another target: the source alone",
                dsd_position_attribution_learn(st, &call, SOURCE, TARGET + 1U, 1, DSD_POSITION_ATTRIBUTION_BIND_EPOCH,
                                               &evidence)
                        == 1
                    && evidence.source == SOURCE && evidence.target == 0U && evidence.group == 0U);
    free_state(st);
}

/* A DMR producer binds to the transmission: a fade the call layer heals into a new epoch of the same transmission
 * keeps the evidence; an epoch-bound producer's does not survive it. */
static void
test_transmission_binding(void) {
    for (int bind = 0; bind <= 1; bind++) {
        dsd_state* st = new_state();
        dsd_decode_clock_use_test(UINT64_C(1000000000));
        open_call(st, DSD_SYNC_DMR_BS_VOICE_POS, SOURCE, TARGET);
        dsd_position_attribution_call call;
        dsd_position_attribution evidence;
        (void)dsd_position_attribution_call_from_slot(st, 0U, &call);
        expect_true("binding: learned", dsd_position_attribution_learn(st, &call, SOURCE, TARGET, 1,
                                                                       (dsd_position_attribution_bind)bind, &evidence)
                                            == 1);
        // The same transmission, a later epoch: what the canonical layer's heal gives.
        call.epoch += 1U;
        dsd_position_attribution granted;
        const int ok = dsd_position_attribution_grant(st, &evidence, &call, DSD_SYNC_DMR_BS_VOICE_POS, &granted);
        expect_true(bind ? "transmission binding survives a healed epoch" : "epoch binding ends with its epoch",
                    ok == bind);
        // Another transmission is never the evidence's.
        call.transmission += 1U;
        expect_true("another transmission refused",
                    dsd_position_attribution_grant(st, &evidence, &call, DSD_SYNC_DMR_BS_VOICE_POS, &granted) == 0);
        dsd_decode_clock_use_system();
        free_state(st);
    }
}

/* The terminator's closing call stands for the call it ended. */
static void
test_closing_call(void) {
    dsd_state* st = new_state();
    open_call(st, DSD_SYNC_P25P1_POS, SOURCE, TARGET);
    dsd_position_attribution_call call;
    dsd_position_attribution evidence;
    (void)dsd_position_attribution_call_from_slot(st, 0U, &call);
    (void)dsd_position_attribution_learn(st, &call, SOURCE, TARGET, 1, DSD_POSITION_ATTRIBUTION_BIND_EPOCH, &evidence);
    dsd_event_closing_call closing;
    expect_true("closing: captured", dsd_event_capture_closing_call(st, 0U, &closing) == 1);
    (void)dsd_call_state_end(st, 0U, 0.0);
    expect_true("closing: the slot's call is closed", dsd_position_attribution_call_from_slot(st, 0U, &call) == 0);
    expect_true("closing: the captured call is open",
                dsd_position_attribution_call_from_closing(&closing, &call) == 1 && call.target == TARGET);
    dsd_position_attribution granted;
    expect_true("closing: granted",
                dsd_position_attribution_grant(st, &evidence, &call, DSD_SYNC_P25P1_POS, &granted) == 1
                    && granted.source == SOURCE && granted.target == TARGET);
    dsd_rxid_note_access(st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_access(st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x294U);
    expect_true("closing: another NAC refused",
                dsd_position_attribution_grant(st, &evidence, &call, DSD_SYNC_P25P1_POS, &granted) == 0);
    closing.valid = 0U;
    expect_true("closing: an invalid capture is closed",
                dsd_position_attribution_call_from_closing(&closing, &call) == 0);
    free_state(st);
}

/* A P25 special address ($FFFFFC-$FFFFFF) names no radio; DMR and every other protocol keep the ID. Admission clears
 * it alone, the destination kept. */
static void
test_fixed_network_source(void) {
    expect_true("p25 FNE use", dsd_position_source_names_radio(DSD_SYNC_P25P1_POS, 0xFFFFFCU) == 0);
    expect_true("p25 FNE", dsd_position_source_names_radio(DSD_SYNC_P25P1_POS, 0xFFFFFDU) == 0);
    expect_true("p25 registration default", dsd_position_source_names_radio(DSD_SYNC_P25P2_POS, 0xFFFFFEU) == 0);
    expect_true("p25 all units", dsd_position_source_names_radio(DSD_SYNC_P25P2_POS, 0xFFFFFFU) == 0);
    expect_true("p25 none", dsd_position_source_names_radio(DSD_SYNC_P25P1_POS, 0U) == 0);
    expect_true("p25 radio", dsd_position_source_names_radio(DSD_SYNC_P25P1_POS, 0xFFFFFBU) == 1);
    expect_true("p25 subscriber table",
                p25_source_id_is_subscriber(0xFFFFFBU) && !p25_source_id_is_subscriber(0xFFFFFCU)
                    && !p25_source_id_is_subscriber(0xFFFFFEU) && !p25_source_id_is_subscriber(0U));
    expect_true("dmr keeps it", dsd_position_source_names_radio(DSD_SYNC_DMR_BS_DATA_POS, 0xFFFFFDU) == 1);
    expect_true("nxdn keeps it", dsd_position_source_names_radio(DSD_SYNC_NXDN_POS, 0xFFFFFFU) == 1);

    dsd_state* st = new_state();
    static const int protocols[] = {DSD_SYNC_P25P1_POS, DSD_SYNC_DMR_BS_DATA_POS};
    for (size_t i = 0; i < sizeof protocols / sizeof protocols[0]; i++) {
        dsd_position_fix fix;
        dsd_position_fix_init(&fix);
        fix.kind = DSD_POS_DMR_LRRP;
        fix.protocol = (uint8_t)protocols[i];
        fix.lat_e7 = 225000000;
        fix.lon_e7 = 450000000;
        fix.source_id = 0xFFFFFDU;
        fix.target_id = 222U;
        const int p25 = DSD_SYNC_IS_P25(protocols[i]);
        expect_true(p25 ? "p25 FNE admitted" : "dmr admitted",
                    dsd_position_admit(NULL, st, &fix, 1, 1, NULL) != DSD_POSITION_REFUSED);
        expect_true(p25 ? "p25 FNE: anonymous, destination kept" : "dmr: source kept",
                    fix.source_id == (p25 ? 0U : 0xFFFFFDU) && fix.target_id == 222U
                        && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
    }
    free_state(st);
}

static dsd_position_reception
reception(uint8_t kind, uint32_t a, uint32_t b, int64_t freq_hz, uint64_t rx_gen, uint8_t epoch_known) {
    dsd_position_reception r;
    DSD_MEMSET(&r, 0, sizeof r);
    r.scope.kind = kind;
    r.scope.a = a;
    r.scope.b = b;
    r.identity_freq_hz = freq_hz;
    r.rx_gen = rx_gen;
    r.epoch_known = epoch_known;
    return r;
}

/* The reception rule (dsd_position_same_reception(), fail closed): known and equal scopes match across generations;
 * within one generation an unknown scope or a refinement matches; anything else -- an unknown side across generations,
 * two families, two tuned frequencies, two values of one kind -- does not. Symmetric. */
static void
test_reception_rule(void) {
    const uint8_t nac = DSD_POS_SCOPE_P25_NAC;
    const uint8_t net = DSD_POS_SCOPE_P25;
    const uint8_t tuned = DSD_POS_SCOPE_FEED_TUNED_FREQ;
    const uint8_t none = DSD_POS_SCOPE_NONE;
    static const char* const tags[] = {
        "equal known scopes, other generations",
        "equal NAC, other discriminator",
        "unknown side, other generations",
        "unknown generation",
        "both unknown, same generation",
        "one unknown, same generation",
        "NAC refined to WACN/SYS",
        "tuned refined to NAC",
        "tuned frequency moved",
        "NAC discriminator moved",
        "two NACs",
        "two families",
        "two networks",
        "EDACS site then system",
        "EDACS two sites",
    };

    const struct {
        dsd_position_reception a;
        dsd_position_reception b;
        int same;
    } cases[] = {
        {reception(net, 0xA1234U, 0x111U, 0, 1U, 1U), reception(net, 0xA1234U, 0x111U, 0, 2U, 1U), 1},
        {reception(nac, 0x293U, 0U, 450000000, 1U, 1U), reception(nac, 0x293U, 0U, 451000000, 2U, 1U), 0},
        {reception(net, 0xA1234U, 0x111U, 0, 1U, 1U), reception(none, 0U, 0U, 0, 2U, 1U), 0},
        {reception(none, 0U, 0U, 0, 1U, 0U), reception(none, 0U, 0U, 0, 1U, 1U), 0},
        {reception(none, 0U, 0U, 0, 7U, 1U), reception(none, 0U, 0U, 0, 7U, 1U), 1},
        {reception(net, 0xA1234U, 0x111U, 0, 7U, 1U), reception(none, 0U, 0U, 0, 7U, 1U), 1},
        {reception(nac, 0x293U, 0U, 450000000, 7U, 1U), reception(net, 0xA1234U, 0x111U, 0, 7U, 1U), 1},
        {reception(tuned, 450000000U, 0U, 0, 7U, 1U), reception(nac, 0x293U, 0U, 450000000, 7U, 1U), 1},
        {reception(tuned, 450000000U, 0U, 0, 7U, 1U), reception(tuned, 451000000U, 0U, 0, 7U, 1U), 0},
        {reception(tuned, 450000000U, 0U, 0, 7U, 1U), reception(nac, 0x293U, 0U, 451000000, 7U, 1U), 0},
        {reception(nac, 0x293U, 0U, 450000000, 7U, 1U), reception(nac, 0x294U, 0U, 450000000, 7U, 1U), 0},
        {reception(net, 0xA1234U, 0x111U, 0, 7U, 1U), reception(DSD_POS_SCOPE_DMR, 17U, 0U, 0, 7U, 1U), 0},
        {reception(net, 0xA1234U, 0x111U, 0, 7U, 1U), reception(net, 0xA1234U, 0x112U, 0, 7U, 1U), 0},
        {reception(DSD_POS_SCOPE_FEED_EDACS, 0x10001U, 0U, 0, 7U, 1U),
         reception(DSD_POS_SCOPE_FEED_EDACS, 0x10001U, 0x10002U, 0, 7U, 1U), 1},
        {reception(DSD_POS_SCOPE_FEED_EDACS, 0x10001U, 0U, 0, 7U, 1U),
         reception(DSD_POS_SCOPE_FEED_EDACS, 0x10003U, 0U, 0, 7U, 1U), 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        expect_true(tags[i], dsd_position_same_reception(&cases[i].a, &cases[i].b) == cases[i].same
                                 && dsd_position_same_reception(&cases[i].b, &cases[i].a) == cases[i].same);
    }
    expect_true("NULL", dsd_position_same_reception(NULL, &cases[0].a) == 0);

    /* An admitted report's reception: its scope, the NAC discriminator and its origin's epoch. */
    dsd_state* st = new_state();
    st->rx_id.family = DSD_RXID_FAMILY_P25;
    st->rx_id.valid = DSD_RXID_ACCESS;
    st->rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    st->rx_id.access = 0x293U;
    st->rx_id.epoch = 9U;
    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    fix.kind = DSD_POS_P25_HARRIS;
    fix.protocol = DSD_SYNC_P25P1_POS;
    fix.lat_e7 = 225000000;
    fix.lon_e7 = 450000000;
    dsd_position_reception r;
    dsd_position_fix_reception(&fix, &r);
    expect_true("unadmitted report: unknown generation", r.epoch_known == 0U);
    expect_true("report admitted", dsd_position_admit(NULL, st, &fix, 1, 1, NULL) != DSD_POSITION_REFUSED);
    dsd_position_fix_reception(&fix, &r);
    expect_true("report reception", r.scope.kind == nac && r.scope.a == 0x293U && (uint32_t)r.rx_gen == 9U
                                        && r.epoch_known == 1U && r.identity_freq_hz == fix.freq_hz);
    free_state(st);
}

int
main(void) {
    static const producer producers[] = {
        {"dmr embedded", DSD_SYNC_DMR_BS_VOICE_POS, DSD_SYNC_P25P1_POS, DSD_RXID_FAMILY_DMR,
         (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, DSD_POSITION_ATTRIBUTION_BIND_TRANSMISSION},
        {"p25p1 lc", DSD_SYNC_P25P1_POS, DSD_SYNC_P25P2_POS, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC,
         DSD_POSITION_ATTRIBUTION_BIND_EPOCH},
        {"p25p2 mac", DSD_SYNC_P25P2_POS, DSD_SYNC_P25P1_POS, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC,
         DSD_POSITION_ATTRIBUTION_BIND_EPOCH},
    };
    test_classes();
    for (size_t i = 0; i < sizeof producers / sizeof producers[0]; i++) {
        test_matrix(&producers[i]);
    }
    test_learn();
    test_transmission_binding();
    test_closing_call();
    test_fixed_network_source();
    test_reception_rule();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "CORE_POSITION_ATTRIBUTION: OK\n");
    return 0;
}
