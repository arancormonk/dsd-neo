// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The verified position feed (issue #800): admission gates, identity clearing, value checks, scopes and the
 * identity discriminator, labels for local and foreign senders, the repeat rule, the ring and its stream, the reception
 * origin, and the process-wide revision.
 *
 * Every coordinate, ID and system number here is made up.
 */
#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(DSD_STATE_EXT_CORE_POSITION_FEED == 17, "the position feed owns state_ext slot 17");
_Static_assert(DSD_POSITION_FEED_CAPACITY == 1024, "the ring holds 1,024 records");
_Static_assert(DSD_POSITION_FEED_TRACKS == 256, "the repeat rule remembers 256 tracks");
_Static_assert(DSD_POSITION_REPEAT_MS == 30000U, "the repeat window is 30 s");

/* A decode clock reading in whole ms past an arbitrary epoch second, so the wall and mono readings agree. */
static const uint64_t T0_MS = UINT64_C(1700000000000);

static void
clock_at_ms(uint64_t ms) {
    dsd_decode_clock_use_test(ms * UINT64_C(1000000));
}

static dsd_state*
new_state(void) {
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    assert(st != NULL);
    return st;
}

static dsd_opts*
new_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    assert(opts != NULL);
    return opts;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

/* A verified DMR LRRP report from made-up radio 1234 to talkgroup 99 at 22.5 N 45.0 E. */
static dsd_position_fix
base_fix(void) {
    dsd_position_fix f;
    dsd_position_fix_init(&f);
    f.kind = DSD_POS_DMR_LRRP;
    f.protocol = DSD_SYNC_DMR_BS_DATA_POS;
    f.slot = 0U;
    f.lat_e7 = 225000000;
    f.lon_e7 = 450000000;
    f.alt_dm = 1234;
    f.err_m = 10U;
    f.speed_dkmh = 500U;
    f.heading_ddeg = 900U;
    f.source_id = 1234U;
    f.target_id = 99U;
    f.flags = DSD_POS_FLAG_TARGET_GROUP;
    f.fix_time_s = INT64_C(1700000000);
    return f;
}

static void
set_tetra_net(dsd_state* st, uint16_t mcc, uint16_t mnc) {
    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_TETRA;
    st->rx_id.valid = DSD_RXID_TETRA_NET;
    st->rx_id.tetra_mcc = mcc;
    st->rx_id.tetra_mnc = mnc;
}

static void
set_p25_nac(dsd_state* st, uint16_t nac) {
    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_P25;
    st->rx_id.valid = DSD_RXID_ACCESS;
    st->rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    st->rx_id.access = nac;
}

static void
tune_rtl(dsd_opts* opts, uint32_t hz) {
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rtlsdr_center_freq = hz;
}

static void
test_init_sentinels(void) {
    dsd_position_fix f;
    DSD_MEMSET(&f, 0x5A, sizeof(f));
    dsd_position_fix_init(&f);
    assert(f.alt_dm == DSD_POSITION_ALT_UNKNOWN && f.alt_dm == INT32_MIN);
    assert(f.err_m == 0U && f.speed_dkmh == UINT16_MAX && f.heading_ddeg == UINT16_MAX);
    assert(f.age_class == 0xFFU && f.sats == 0xFFU && f.protocol == DSD_POSITION_PROTOCOL_NONE);
    assert(f.kind == 0U && f.flags == 0U && f.fix_time_s == 0 && f.source_id == 0U && f.target_id == 0U);
    assert(f.stream_id == 0U && f.seq == 0U && f.rev == 0U && f.proc_instance == 0U);
    assert(f.source_text[0] == '\0' && f.label[0] == '\0' && f.reception_scope.kind == 0U);
    dsd_position_fix_init(NULL);
}

static void
test_gates(void) {
    dsd_state* st = new_state();
    dsd_opts* opts = new_opts();
    clock_at_ms(T0_MS);
    dsd_position_fix f = base_fix();
    const dsd_position_fix before = f;

    /* The producer's own check must have passed, exactly 1. */
    assert(dsd_position_admit(opts, st, &f, 0, 1, NULL) == DSD_POSITION_REFUSED);
    assert(dsd_position_admit(opts, st, &f, 2, 1, NULL) == DSD_POSITION_REFUSED);
    assert(memcmp(&f, &before, sizeof f) == 0); /* a refused report is left as it was */
    assert(dsd_position_admit(opts, NULL, &f, 1, 1, NULL) == DSD_POSITION_REFUSED);
    assert(dsd_position_admit(opts, st, NULL, 1, 1, NULL) == DSD_POSITION_REFUSED);

    /* A report with no format or no protocol is a producer bug: refused. */
    dsd_position_fix bad = base_fix();
    bad.kind = 0U;
    assert(dsd_position_admit(opts, st, &bad, 1, 1, NULL) == DSD_POSITION_REFUSED);
    bad = base_fix();
    bad.kind = DSD_POS_KIND_LAST + 1U;
    assert(dsd_position_admit(opts, st, &bad, 1, 1, NULL) == DSD_POSITION_REFUSED);
    bad = base_fix();
    bad.protocol = DSD_POSITION_PROTOCOL_NONE;
    assert(dsd_position_admit(opts, st, &bad, 1, 1, NULL) == DSD_POSITION_REFUSED);

    /* The receive policy: a P25 report while the NAC filter is on and no NAC is verified yet is not admitted. */
    dsd_position_fix p25 = base_fix();
    p25.protocol = DSD_SYNC_P25P1_POS;
    opts->receive.filters[DSD_RECEIVE_NAC].enabled = 1;
    assert(dsd_position_admit(opts, st, &p25, 1, 1, NULL) == DSD_POSITION_REFUSED);
    opts->receive.filters[DSD_RECEIVE_NAC].enabled = 0;
    assert(dsd_position_admit(opts, st, &p25, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);

    /* No ring here: admitted, stamped, never kept. */
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.stream_id == 0U && f.seq == 0U && f.rev != 0U && f.proc_instance != 0U);
    free(opts);
    free_state(st);
}

static void
test_identity_clearing(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);

    /* Unverified: source, callsign, target and the group flag go; the format and the other flags stay. */
    dsd_position_fix f = base_fix();
    f.kind = DSD_POS_P25_APX;
    f.flags |= DSD_POS_FLAG_FORMAT_PROVISIONAL;
    DSD_SNPRINTF(f.source_text, sizeof f.source_text, "%s", "AB1CDE");
    assert(dsd_position_admit(NULL, st, &f, 1, 0, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.kind == DSD_POS_P25_APX);
    assert(f.source_id == 0U && f.target_id == 0U && f.source_text[0] == '\0');
    assert((f.flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);
    assert(f.flags == (DSD_POS_FLAG_SOURCE_UNVERIFIED | DSD_POS_FLAG_FORMAT_PROVISIONAL));

    /* Hashed: the identity goes too, and the report stays marked hashed rather than unverified. */
    f = base_fix();
    f.kind = DSD_POS_LIP_USBD;
    f.flags = DSD_POS_FLAG_SOURCE_HASHED;
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.kind == DSD_POS_LIP_USBD && f.source_id == 0U && f.target_id == 0U);
    assert(f.flags == DSD_POS_FLAG_SOURCE_HASHED);

    /* Verified: kept, and admission alone says whether a source is unverified. */
    f = base_fix();
    f.flags |= DSD_POS_FLAG_SOURCE_UNVERIFIED | 0x8000U;
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.source_id == 1234U && f.target_id == 99U && f.flags == DSD_POS_FLAG_TARGET_GROUP);
    free_state(st);
}

static dsd_position_admit_result
admit_at(dsd_state* st, int32_t lat_e7, int32_t lon_e7) {
    dsd_position_fix f = base_fix();
    f.lat_e7 = lat_e7;
    f.lon_e7 = lon_e7;
    return dsd_position_admit(NULL, st, &f, 1, 1, NULL);
}

static void
test_values(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    const dsd_position_admit_result ok = DSD_POSITION_ADMITTED_NOT_KEPT;
    assert(admit_at(st, 0, 0) == DSD_POSITION_REFUSED);
    assert(admit_at(st, 0, 200000000) == ok); /* on the equator */
    assert(admit_at(st, 100000000, 0) == ok); /* on the prime meridian */
    assert(admit_at(st, 900000000, 1800000000) == ok);
    assert(admit_at(st, -900000000, -1800000000) == ok);
    assert(admit_at(st, 900000001, 450000000) == DSD_POSITION_REFUSED);
    assert(admit_at(st, -900000001, 450000000) == DSD_POSITION_REFUSED);
    assert(admit_at(st, 225000000, 1800000001) == DSD_POSITION_REFUSED);
    assert(admit_at(st, 225000000, -1800000001) == DSD_POSITION_REFUSED);
    assert(admit_at(st, INT32_MIN, INT32_MIN) == DSD_POSITION_REFUSED);

    /* A heading past 359.9 degrees is unknown, never folded into range. */
    dsd_position_fix f = base_fix();
    f.heading_ddeg = 3600U;
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == ok);
    assert(f.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN);
    f = base_fix();
    f.heading_ddeg = 3599U;
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == ok && f.heading_ddeg == 3599U);
    f = base_fix();
    f.heading_ddeg = DSD_POSITION_HEADING_UNKNOWN;
    f.speed_dkmh = DSD_POSITION_SPEED_UNKNOWN;
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == ok);
    assert(f.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN && f.speed_dkmh == DSD_POSITION_SPEED_UNKNOWN);

    /* A callsign that fills its field keeps only whole characters and its NUL. */
    f = base_fix();
    DSD_MEMSET(f.source_text, 'A', sizeof f.source_text);
    assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == ok);
    assert(strlen(f.source_text) == sizeof f.source_text - 1U);
    free_state(st);
}

static void
test_scopes(void) {
    dsd_state* st = new_state();
    dsd_opts* opts = new_opts();
    clock_at_ms(T0_MS);

    /* A conventional P25 NAC scope is not unique: the tuned frequency joins the identity. */
    set_p25_nac(st, 0x2A3U);
    tune_rtl(opts, 450000000U);
    dsd_position_fix f = base_fix();
    f.protocol = DSD_SYNC_P25P1_POS;
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_P25_NAC && f.reception_scope.a == 0x2A3U);
    assert(f.sender_scope.kind == DSD_POS_SCOPE_P25_NAC && f.sender_scope.a == 0x2A3U);
    assert(f.freq_hz == 450000000 && f.identity_freq_hz == 450000000);

    /* A network status names the system: nothing joins it. */
    st->rx_id.valid |= DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID;
    st->rx_id.p25_wacn = 0xBEE00U;
    st->rx_id.p25_sysid = 0x123U;
    f = base_fix();
    f.protocol = DSD_SYNC_P25P1_POS;
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.sender_scope.kind == DSD_POS_SCOPE_P25 && f.sender_scope.a == 0xBEE00U && f.sender_scope.b == 0x123U);
    assert(f.freq_hz == 450000000 && f.identity_freq_hz == 0);

    /* No alias scope: the control channel the identity holds, else the tuned frequency, else none. */
    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_DMR;
    st->rx_id.valid = DSD_RXID_CC_FREQ;
    st->rx_id.cc_freq_hz = 451000000U;
    f = base_fix();
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_FEED_CC_FREQ && f.reception_scope.a == 451000000U);
    assert(f.identity_freq_hz == 0);
    st->rx_id.valid = 0U;
    f = base_fix();
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_FEED_TUNED_FREQ && f.reception_scope.a == 450000000U);
    opts->audio_in_type = 0;
    f = base_fix();
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_NONE && f.freq_hz == 0);

    /* EDACS: its site and system. */
    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_EDACS;
    st->rx_id.valid = DSD_RXID_EDACS_SITE;
    st->rx_id.edacs_site = 7U;
    f = base_fix();
    f.protocol = DSD_SYNC_EDACS_POS;
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_FEED_EDACS && f.reception_scope.a == (0x10000U | 7U)
           && f.reception_scope.b == 0U);

    /* M17 and D-STAR name sources by callsign: no scope, whatever the receiver holds or the producer set. */
    set_tetra_net(st, 100U, 20U);
    tune_rtl(opts, 430000000U);
    f = base_fix();
    f.kind = DSD_POS_M17_GNSS;
    f.protocol = DSD_SYNC_M17_LSF_POS;
    f.source_id = 0U;
    f.sender_scope.kind = DSD_POS_SCOPE_TETRA;
    DSD_SNPRINTF(f.source_text, sizeof f.source_text, "%s", "AB1CDE");
    assert(dsd_position_admit(opts, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.reception_scope.kind == DSD_POS_SCOPE_NONE && f.sender_scope.kind == DSD_POS_SCOPE_NONE);
    assert(f.freq_hz == 430000000 && strcmp(f.source_text, "AB1CDE") == 0);
    free(opts);
    free_state(st);
}

/* A sender scope the producer did not name as an alias scope is unknown: the sender is taken as heard here. */
static void
test_unnamed_sender_scope(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    set_tetra_net(st, 100U, 20U);
    const dsd_position_scope given[] = {
        {0x99U, {0U, 0U, 0U}, 7U, 0U},                         /* an undefined kind */
        {DSD_POS_SCOPE_NONE, {0U, 0U, 0U}, 0U, 30U},           /* NONE carrying a value */
        {DSD_POS_SCOPE_FEED_TUNED_FREQ, {0U, 0U, 0U}, 1U, 0U}, /* a feed-only kind names no network */
    };
    for (size_t i = 0; i < sizeof given / sizeof given[0]; i++) {
        dsd_position_fix f = base_fix();
        f.protocol = DSD_SYNC_TETRA_NDB_POS;
        f.sender_scope = given[i];
        assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
        assert(f.sender_scope.kind == DSD_POS_SCOPE_TETRA && f.sender_scope.a == 100U && f.sender_scope.b == 20U);
    }
    free_state(st);
}

/* One SSI in the local network and in a foreign one, with a populated local radio list (spec 2.3 step 5). */
static void
test_labels_local_and_foreign(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    set_tetra_net(st, 100U, 20U);

    dsd_source_alias_store* radios = dsd_source_alias_store_create();
    assert(radios != NULL);
    dsd_source_alias_entry radio;
    DSD_MEMSET(&radio, 0, sizeof radio);
    radio.id_start = radio.id_end = 4242U;
    DSD_SNPRINTF(radio.name, sizeof radio.name, "%s", "Local Radio");
    assert(dsd_source_alias_store_append(radios, &radio) == 0);
    dsd_source_alias_install(st, radios);

    const dsd_alias_scope foreign = {DSD_ALIAS_SCOPE_TETRA, 200U, 30U, 1U};
    assert(dsd_alias_learn_radio(NULL, st, &foreign, 4242U, "Foreign Radio", NULL) == DSD_TG_POLICY_LEARN_ADDED);

    /* A local sender: the scoped lookup, the radio list first. */
    dsd_position_fix f = base_fix();
    f.kind = DSD_POS_TETRA_LIP;
    f.protocol = DSD_SYNC_TETRA_NDB_POS;
    f.source_id = 4242U;
    dsd_position_fix local = f;
    assert(dsd_position_admit(NULL, st, &local, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(strcmp(local.label, "Local Radio") == 0);
    assert(local.sender_scope.kind == DSD_POS_SCOPE_TETRA && local.sender_scope.a == 100U);

    /* The same number from a foreign network: only the label learned in that network names it. */
    dsd_position_fix away = f;
    away.sender_scope.kind = DSD_POS_SCOPE_TETRA;
    away.sender_scope.a = 200U;
    away.sender_scope.b = 30U;
    assert(dsd_position_admit(NULL, st, &away, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(strcmp(away.label, "Foreign Radio") == 0);
    assert(away.reception_scope.a == 100U && away.sender_scope.a == 200U);
    assert(!dsd_position_same_track(&local, &away));

    /* A third network nobody learned: the local radio list never names it. */
    dsd_position_fix third = f;
    third.sender_scope.kind = DSD_POS_SCOPE_TETRA;
    third.sender_scope.a = 300U;
    third.sender_scope.b = 40U;
    assert(dsd_position_admit(NULL, st, &third, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(third.label[0] == '\0');

    /* An unverified source has no label, and its unchecked sender network is dropped. */
    dsd_position_fix unverified = away;
    assert(dsd_position_admit(NULL, st, &unverified, 1, 0, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(unverified.label[0] == '\0' && unverified.sender_scope.a == 100U);

    /* A reception that names no system (a conventional channel, a replay): its own senders are local and named as
     * call rows name them, the radio list included, whether the scope is feed-only or none. */
    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    dsd_opts* opts = new_opts();
    tune_rtl(opts, 450000000U);
    dsd_position_fix feed_only = f;
    assert(dsd_position_admit(opts, st, &feed_only, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(feed_only.reception_scope.kind == DSD_POS_SCOPE_FEED_TUNED_FREQ);
    assert(strcmp(feed_only.label, "Local Radio") == 0);
    dsd_position_fix no_scope = f;
    assert(dsd_position_admit(NULL, st, &no_scope, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(no_scope.reception_scope.kind == DSD_POS_SCOPE_NONE && strcmp(no_scope.label, "Local Radio") == 0);

    /* The same number sent from an explicit foreign network over that reception: never the local radio list. */
    dsd_position_fix foreign_unlearned = f;
    foreign_unlearned.sender_scope.kind = DSD_POS_SCOPE_TETRA;
    foreign_unlearned.sender_scope.a = 300U;
    foreign_unlearned.sender_scope.b = 40U;
    assert(dsd_position_admit(opts, st, &foreign_unlearned, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(foreign_unlearned.label[0] == '\0');
    dsd_position_fix foreign_learned = away;
    foreign_learned.label[0] = '\0';
    assert(dsd_position_admit(opts, st, &foreign_learned, 1, 1, NULL) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(strcmp(foreign_learned.label, "Foreign Radio") == 0);
    free(opts);
    free_state(st);
}

/* ---- Feed ---- */

static dsd_position_admit_result
admit_fix(dsd_state* st, dsd_position_fix* f) {
    return dsd_position_admit(NULL, st, f, 1, 1, NULL);
}

static void
test_repeats(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    clock_at_ms(T0_MS);
    dsd_position_fix a = base_fix();
    assert(admit_fix(st, &a) == DSD_POSITION_ADMITTED_KEPT && a.seq == 1U);
    assert(a.stream_id == dsd_position_feed_stream_id(st) && a.stream_id != 0U);

    /* A stationary radio: its repeats inside 30 s are admitted (history, -L) but not kept. */
    clock_at_ms(T0_MS + 10000U);
    dsd_position_fix again = base_fix();
    assert(admit_fix(st, &again) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(again.seq == 0U && again.stream_id == 0U && again.rev > a.rev);
    clock_at_ms(T0_MS + 29999U);
    again = base_fix();
    assert(admit_fix(st, &again) == DSD_POSITION_ADMITTED_NOT_KEPT);
    clock_at_ms(T0_MS + 30000U);
    again = base_fix();
    assert(admit_fix(st, &again) == DSD_POSITION_ADMITTED_KEPT && again.seq == 2U);

    /* A -> B -> A: the repeat is judged against the last kept report only. */
    clock_at_ms(T0_MS + 31000U);
    dsd_position_fix b = base_fix();
    b.lat_e7 = 100000000;
    b.lon_e7 = 200000000;
    assert(admit_fix(st, &b) == DSD_POSITION_ADMITTED_KEPT && b.seq == 3U);
    clock_at_ms(T0_MS + 32000U);
    again = base_fix();
    assert(admit_fix(st, &again) == DSD_POSITION_ADMITTED_KEPT && again.seq == 4U);

    /* The clock went back past the kept report: never a repeat. */
    clock_at_ms(T0_MS + 20000U);
    again = base_fix();
    assert(admit_fix(st, &again) == DSD_POSITION_ADMITTED_KEPT && again.seq == 5U);

    /* Another radio, or the same number in another system, is another track. */
    clock_at_ms(T0_MS + 20001U);
    dsd_position_fix other = base_fix();
    other.source_id = 1235U;
    assert(admit_fix(st, &other) == DSD_POSITION_ADMITTED_KEPT);
    other = base_fix();
    other.protocol = DSD_SYNC_P25P1_POS;
    assert(admit_fix(st, &other) == DSD_POSITION_ADMITTED_KEPT);
    other = base_fix();
    other.sender_scope.kind = DSD_POS_SCOPE_DMR;
    other.sender_scope.a = 5U;
    assert(admit_fix(st, &other) == DSD_POSITION_ADMITTED_KEPT);
    /* ... but a burst of the other DMR polarity, or the other slot, is the same radio. */
    other = base_fix();
    other.protocol = DSD_SYNC_DMR_MS_DATA;
    other.slot = 1U;
    assert(admit_fix(st, &other) == DSD_POSITION_ADMITTED_NOT_KEPT);
    free_state(st);
}

static void
test_anonymous_never_fold(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    clock_at_ms(T0_MS);
    for (int i = 0; i < 3; i++) {
        dsd_position_fix f = base_fix();
        assert(dsd_position_admit(NULL, st, &f, 1, 0, NULL) == DSD_POSITION_ADMITTED_KEPT);
        f = base_fix();
        f.flags |= DSD_POS_FLAG_SOURCE_HASHED;
        assert(dsd_position_admit(NULL, st, &f, 1, 1, NULL) == DSD_POSITION_ADMITTED_KEPT);
        f = base_fix();
        f.source_id = 0U;
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT);
    }
    dsd_position_fix x = base_fix();
    dsd_position_fix y = base_fix();
    x.flags |= DSD_POS_FLAG_SOURCE_UNVERIFIED;
    assert(!dsd_position_same_track(&x, &y) && !dsd_position_same_track(NULL, &y));
    free_state(st);
}

typedef void (*fix_mutator)(dsd_position_fix* f);

#define MUTATOR(name, body)                                                                                            \
    static void name(dsd_position_fix* f) { body; }
MUTATOR(m_kind, f->kind = DSD_POS_DMR_LOCN)
MUTATOR(m_lat, f->lat_e7 += 1)
MUTATOR(m_lon, f->lon_e7 += 1)
MUTATOR(m_alt, f->alt_dm = DSD_POSITION_ALT_UNKNOWN)
MUTATOR(m_speed, f->speed_dkmh = 501U)
MUTATOR(m_heading, f->heading_ddeg = 901U)
MUTATOR(m_err, f->err_m = 11U)
MUTATOR(m_age, f->age_class = 1U)
MUTATOR(m_sats, f->sats = 6U)
MUTATOR(m_target, f->target_id = 98U)
MUTATOR(m_flags, f->flags |= DSD_POS_FLAG_EMERGENCY)
MUTATOR(m_fix_time, f->fix_time_s += 1)

static void
test_every_observable_field_breaks_a_repeat(void) {
    static const struct {
        const char* name;
        fix_mutator mutate;
    } cases[] = {
        {"kind", m_kind},   {"lat", m_lat},         {"lon", m_lon},     {"alt", m_alt},
        {"speed", m_speed}, {"heading", m_heading}, {"err", m_err},     {"age class", m_age},
        {"sats", m_sats},   {"target", m_target},   {"flags", m_flags}, {"fix time", m_fix_time},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_state* st = new_state();
        assert(dsd_position_feed_start(st) == 0);
        clock_at_ms(T0_MS);
        dsd_position_fix f = base_fix();
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT);
        clock_at_ms(T0_MS + 1000U);
        f = base_fix();
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_NOT_KEPT);
        f = base_fix();
        cases[i].mutate(&f);
        if (admit_fix(st, &f) != DSD_POSITION_ADMITTED_KEPT) {
            DSD_FPRINTF(stderr, "FAIL: a change of %s was folded as a repeat\n", cases[i].name);
            exit(1);
        }
        free_state(st);
    }
}

static void
test_track_table_forgets_the_oldest(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    clock_at_ms(T0_MS);
    for (uint32_t i = 0; i <= (uint32_t)DSD_POSITION_FEED_TRACKS; i++) {
        dsd_position_fix f = base_fix();
        f.source_id = 1000U + i;
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT);
    }
    /* Radio 1000 was the least recently kept when the 257th track arrived: forgotten, so its report is kept again. */
    dsd_position_fix first = base_fix();
    first.source_id = 1000U;
    assert(admit_fix(st, &first) == DSD_POSITION_ADMITTED_KEPT);
    /* Radio 1002 is still remembered (1001 made room for 1000). */
    dsd_position_fix third = base_fix();
    third.source_id = 1002U;
    assert(admit_fix(st, &third) == DSD_POSITION_ADMITTED_NOT_KEPT);
    free_state(st);
}

static dsd_position_fix g_page[DSD_POSITION_FEED_CAPACITY];

/* A started ring past its capacity: @p total distinct reports, all kept. */
static dsd_state*
wrapped_ring(uint64_t total) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    clock_at_ms(T0_MS);
    for (uint64_t i = 0; i < total; i++) {
        dsd_position_fix f = base_fix();
        f.lat_e7 = 100000000 + (int32_t)i;
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT && f.seq == i + 1U);
    }
    return st;
}

static void
test_ring_wrap_and_read(void) {
    const uint64_t total = (uint64_t)DSD_POSITION_FEED_CAPACITY + 6U;
    dsd_state* st = wrapped_ring(total);
    dsd_position_feed_meta meta;
    /* Seqs 1..6 were overwritten: a reader from the start gets a gap, then the oldest held first. */
    int n = dsd_position_feed_copy_since(st, 0U, g_page, 10U, &meta);
    assert(n == 10 && meta.lost == 1U && meta.more == 1U);
    assert(meta.oldest_seq == 7U && meta.newest_seq == total && meta.last_seq == 16U);
    assert(meta.stream_id == dsd_position_feed_stream_id(st));
    for (int i = 0; i < n; i++) {
        assert(g_page[i].seq == 7U + (uint64_t)i && g_page[i].lat_e7 == 100000000 + 6 + i);
    }
    /* The next page from the cursor: no gap. */
    n = dsd_position_feed_copy_since(st, meta.last_seq, g_page, DSD_POSITION_FEED_CAPACITY, &meta);
    assert(n == (int)(total - 16U) && meta.lost == 0U && meta.more == 0U && meta.last_seq == total);
    assert(g_page[n - 1].seq == total);
    /* Caught up. */
    n = dsd_position_feed_copy_since(st, total, g_page, 4U, &meta);
    assert(n == 0 && meta.last_seq == total && meta.more == 0U && meta.lost == 0U);
    free_state(st);
}

static void
test_read_edges(void) {
    const uint64_t total = (uint64_t)DSD_POSITION_FEED_CAPACITY + 6U;
    dsd_state* st = wrapped_ring(total);
    dsd_position_feed_meta meta;
    /* No room: nothing copied, one more waiting. */
    const int n = dsd_position_feed_copy_since(st, total - 1U, NULL, 0U, &meta);
    assert(n == 0 && meta.more == 1U && meta.last_seq == total - 1U);
    /* The read seam: arguments. */
    assert(dsd_position_feed_copy_since(st, 0U, g_page, 1U, NULL) == -1);
    assert(dsd_position_feed_copy_since(st, 0U, NULL, 1U, &meta) == -1);
    assert(dsd_position_feed_copy_since(NULL, 0U, g_page, 1U, &meta) == -1);
    free_state(st);
}

static void
test_stream_restart(void) {
    dsd_state* st = new_state();
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t first = dsd_position_feed_stream_id(st);
    clock_at_ms(T0_MS);
    dsd_position_fix f = base_fix();
    assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT && f.seq == 1U && f.stream_id == first);

    /* A new run on the same state: a new stream, seq from 1, no remembered track. */
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t second = dsd_position_feed_stream_id(st);
    assert(second != 0U && second != first);
    dsd_position_feed_meta meta;
    assert(dsd_position_feed_copy_since(st, 0U, g_page, 4U, &meta) == 0);
    assert(meta.stream_id == second && meta.newest_seq == 0U && meta.oldest_seq == 0U);
    f = base_fix();
    assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_KEPT && f.seq == 1U && f.stream_id == second);

    /* The run ends (the extensions are freed); the next run's start allocates a fresh stream. */
    dsd_state_ext_free_all(st);
    assert(dsd_position_feed_stream_id(st) == 0U);
    assert(dsd_position_feed_start(st) == 0);
    const uint64_t third = dsd_position_feed_stream_id(st);
    assert(third != 0U && third != second && third != first);
    assert(dsd_position_feed_start(NULL) == -1);
    free_state(st);
}

static void
test_no_ring_is_a_no_op(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    for (int i = 0; i < 3; i++) {
        dsd_position_fix f = base_fix();
        assert(admit_fix(st, &f) == DSD_POSITION_ADMITTED_NOT_KEPT && f.seq == 0U && f.stream_id == 0U);
    }
    dsd_position_feed_meta meta;
    DSD_MEMSET(&meta, 0x5A, sizeof meta);
    assert(dsd_position_feed_copy_since(st, 7U, g_page, 4U, &meta) == 0);
    assert(meta.stream_id == 0U && meta.last_seq == 0U && meta.oldest_seq == 0U && meta.newest_seq == 0U);
    assert(meta.more == 0U && meta.lost == 0U);
    assert(dsd_position_feed_stream_id(st) == 0U && dsd_position_feed_stream_id(NULL) == 0U);
    assert(dsd_state_ext_get(st, DSD_STATE_EXT_CORE_POSITION_FEED) == NULL); /* admission never allocates */
    free_state(st);
}

static void
test_origin(void) {
    dsd_state* st = new_state();
    dsd_opts* opts = new_opts();
    clock_at_ms(T0_MS);
    set_p25_nac(st, 0x2A3U);
    tune_rtl(opts, 450000000U);
    dsd_position_origin origin;
    dsd_position_origin_capture(opts, st, &origin);
    assert(origin.valid == 1U && origin.freq_hz == 450000000 && origin.scope.kind == DSD_POS_SCOPE_P25_NAC);
    assert(dsd_position_origin_matches(st, &origin));

    /* The report keeps the reception it began on, even though the tuned frequency reads another one now. */
    tune_rtl(opts, 460000000U);
    dsd_position_fix f = base_fix();
    f.protocol = DSD_SYNC_P25P1_POS;
    assert(dsd_position_admit(opts, st, &f, 1, 1, &origin) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(f.freq_hz == 450000000 && f.identity_freq_hz == 450000000 && f.origin_gen == origin.gen);

    /* A retune between fragments refuses the completion. */
    dsd_trunk_tuning_generation_advance();
    assert(!dsd_position_origin_matches(st, &origin));
    f = base_fix();
    f.protocol = DSD_SYNC_P25P1_POS;
    const dsd_position_fix before = f;
    assert(dsd_position_admit(opts, st, &f, 1, 1, &origin) == DSD_POSITION_REFUSED);
    assert(memcmp(&f, &before, sizeof f) == 0);

    /* So does a received identity epoch change (another system, a target switch). */
    dsd_position_origin_capture(opts, st, &origin);
    st->rx_id.epoch++;
    f = base_fix();
    assert(dsd_position_admit(opts, st, &f, 1, 1, &origin) == DSD_POSITION_REFUSED);

    /* An origin never captured never matches. */
    dsd_position_origin zero;
    DSD_MEMSET(&zero, 0, sizeof zero);
    zero.gen = ((dsd_trunk_tuning_generation() & 0xFFFFFFFFU) << 32) | st->rx_id.epoch;
    assert(!dsd_position_origin_matches(st, &zero));
    f = base_fix();
    assert(dsd_position_admit(opts, st, &f, 1, 1, &zero) == DSD_POSITION_REFUSED);
    assert(!dsd_position_origin_matches(NULL, &origin) && !dsd_position_origin_matches(st, NULL));

    dsd_position_origin_capture(opts, NULL, &origin);
    assert(origin.valid == 0U);
    dsd_position_origin_capture(opts, st, NULL);
    free(opts);
    free_state(st);
}

static void
test_rev_and_stamps_across_a_backward_wall_step(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    dsd_position_fix one = base_fix();
    assert(admit_fix(st, &one) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(one.rx_time_s == (int64_t)(T0_MS / 1000U) && one.rx_clock == (uint8_t)DSD_DECODE_CLOCK_TEST);
    assert(one.rev == dsd_position_last_rev());

    /* The wall clock steps back an hour: the revision still goes up, the receive time follows the clock. */
    clock_at_ms(T0_MS - 3600000U);
    dsd_position_fix two = base_fix();
    assert(admit_fix(st, &two) == DSD_POSITION_ADMITTED_NOT_KEPT);
    assert(two.rev > one.rev && two.rx_time_s == one.rx_time_s - 3600);
    assert(two.proc_instance == one.proc_instance && one.proc_instance == dsd_position_proc_instance());
    assert(one.proc_instance != 0U);

    /* Refused reports take no revision. */
    const uint64_t rev = dsd_position_last_rev();
    assert(admit_at(st, 0, 0) == DSD_POSITION_REFUSED);
    assert(dsd_position_last_rev() == rev);
    free_state(st);
}

int
main(void) {
    test_init_sentinels();
    test_gates();
    test_identity_clearing();
    test_values();
    test_scopes();
    test_unnamed_sender_scope();
    test_labels_local_and_foreign();
    test_repeats();
    test_anonymous_never_fold();
    test_every_observable_field_breaks_a_repeat();
    test_track_table_forgets_the_oldest();
    test_ring_wrap_and_read();
    test_read_edges();
    test_stream_restart();
    test_no_ring_is_a_no_op();
    test_origin();
    test_rev_and_stamps_across_a_backward_wall_step();
    printf("core position feed: OK\n");
    return 0;
}
