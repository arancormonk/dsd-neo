// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The unit activity ring: dedup merges, revision-order reads, loss and reset reporting, the incremental
 * snapshot copy and its allocation failure, and the reception scope a push takes from rx_id.
 */
#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(DSD_STATE_EXT_CORE_UNIT_ACTIVITY == 14, "the unit activity ring owns state_ext slot 14");
_Static_assert(sizeof(dsd_unit_activity) <= 192, "a row fits its 192-byte budget");

/* A decode clock reading in whole ms past an arbitrary 2023 epoch second, so the wall and mono readings agree. */
static const uint64_t T0_MS = UINT64_C(1700000000000);

static void
clock_at_ms(uint64_t ms) {
    dsd_decode_clock_test_set_ns(ms * UINT64_C(1000000));
}

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

/* A P25 registration response for made-up RID 1234567 (home WACN 0xBEE00 / SYS 0x123) assigned WUID 100. */
static dsd_unit_activity
base_row(void) {
    dsd_unit_activity row;
    DSD_MEMSET(&row, 0, sizeof(row));
    row.protocol = DSD_UA_PROTO_P25P1;
    row.kind = DSD_UA_REGISTRATION;
    row.role = DSD_UA_ROLE_RESPONSE;
    row.result = DSD_UA_RESULT_ACCEPTED;
    row.subject.id = 1234567U;
    row.subject.subtype = DSD_UA_ID_RADIO;
    row.subject.valid = 1U;
    row.subject.home_kind = DSD_UA_SCOPE_P25;
    row.subject.home_valid = 1U;
    row.subject.home_a = 0xBEE00U;
    row.subject.home_b = 0x123U;
    row.assigned.id = 100U;
    row.assigned.subtype = DSD_UA_ID_RADIO;
    row.assigned.valid = 1U;
    row.scope.kind = DSD_UA_SCOPE_P25;
    row.scope.valid = 1U;
    row.scope.a = 0xBEE00U;
    row.scope.b = 0x123U;
    row.scope.c = DSD_UA_SCOPE_SITE_KNOWN | 0x0101U;
    row.freq_hz = 851000000;
    return row;
}

static dsd_unit_activity
row_for(uint32_t rid) {
    dsd_unit_activity row = base_row();
    row.subject.id = rid;
    return row;
}

static int
push_rid(dsd_state* st, uint32_t rid) {
    const dsd_unit_activity row = row_for(rid);
    return dsd_unit_activity_push(st, &row);
}

/* Every row the ring holds after (instance, rev), read in pages of @p page; returns the count and the cursor. */
static int
read_all(const dsd_state* st, uint64_t* instance, uint64_t* rev, int page, dsd_unit_activity* out, int cap,
         uint8_t* lost_any) {
    int total = 0;
    dsd_unit_activity_read_meta meta;
    *lost_any = 0U;
    for (;;) {
        const int room = cap - total < page ? cap - total : page;
        const int n = dsd_unit_activity_read(st, *instance, *rev, out + total, room, &meta);
        assert(n >= 0);
        *lost_any |= meta.lost;
        *instance = meta.instance;
        *rev = meta.last_rev;
        total += n;
        if (!meta.more || total >= cap) {
            return total;
        }
    }
}

static void
test_dedup_anchored_window(void) {
    dsd_state* st = new_state();
    const dsd_unit_activity row = base_row();
    clock_at_ms(T0_MS);
    assert(dsd_unit_activity_push(st, &row) == 1);
    clock_at_ms(T0_MS + 1999U);
    assert(dsd_unit_activity_push(st, &row) == 0);

    dsd_unit_activity out[4];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 4, &meta) == 1);
    assert(out[0].seq == 1U && out[0].rev == 2U && out[0].repeat == 2U);
    assert(out[0].first_ms == T0_MS && out[0].last_ms == T0_MS + 1999U);

    /* The window is anchored at the row's first push: 2000 ms after it starts a row even though the last push was
       1 ms ago. */
    clock_at_ms(T0_MS + 2000U);
    assert(dsd_unit_activity_push(st, &row) == 1);
    assert(dsd_unit_activity_read(st, meta.instance, meta.last_rev, out, 4, &meta) == 1);
    assert(out[0].seq == 2U && out[0].rev == 3U && out[0].repeat == 1U && out[0].first_ms == T0_MS + 2000U);
    free_state(st);
}

/* A repeated deregistration acknowledgement whose producer no longer finds the assigned identity (the first one
 * removed the registration it came from) merges into the row and keeps that identity; another assigned identity, or
 * a registration without one, still starts a row. */
static void
test_dereg_repeat_without_assigned_merges(void) {
    dsd_state* st = new_state();
    dsd_unit_activity dereg = base_row();
    dereg.kind = DSD_UA_DEREGISTRATION;
    dereg.role = DSD_UA_ROLE_ACK;
    dereg.result = DSD_UA_RESULT_ACKNOWLEDGED;
    dsd_unit_activity repeat = dereg;
    DSD_MEMSET(&repeat.assigned, 0, sizeof(repeat.assigned));
    clock_at_ms(T0_MS);
    assert(dsd_unit_activity_push(st, &dereg) == 1);
    clock_at_ms(T0_MS + 300U);
    assert(dsd_unit_activity_push(st, &repeat) == 0);
    clock_at_ms(T0_MS + 600U);
    assert(dsd_unit_activity_push(st, &repeat) == 0);

    dsd_unit_activity out[4];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 4, &meta) == 1);
    assert(out[0].repeat == 3U && out[0].assigned.valid && out[0].assigned.id == 100U);

    dsd_unit_activity other = dereg;
    other.assigned.id = 101U;
    assert(dsd_unit_activity_push(st, &other) == 1);
    dsd_unit_activity reg = base_row();
    assert(dsd_unit_activity_push(st, &reg) == 1);
    DSD_MEMSET(&reg.assigned, 0, sizeof(reg.assigned));
    assert(dsd_unit_activity_push(st, &reg) == 1);
    free_state(st);
}

static void
test_backwards_time_never_merges(void) {
    dsd_state* st = new_state();
    const dsd_unit_activity row = base_row();
    clock_at_ms(T0_MS + 500U);
    assert(dsd_unit_activity_push(st, &row) == 1);
    clock_at_ms(T0_MS);
    assert(dsd_unit_activity_push(st, &row) == 1);
    free_state(st);

    /* Behind the row's last push, though still after its first: a new row, so last_ms never goes back. */
    st = new_state();
    clock_at_ms(T0_MS);
    assert(dsd_unit_activity_push(st, &row) == 1);
    clock_at_ms(T0_MS + 1000U);
    assert(dsd_unit_activity_push(st, &row) == 0);
    clock_at_ms(T0_MS + 500U);
    assert(dsd_unit_activity_push(st, &row) == 1);
    free_state(st);
}

static void
test_merge_bumps_repeat_last_and_rev(void) {
    dsd_state* st = new_state();
    dsd_unit_activity row = base_row();
    clock_at_ms(T0_MS + 100U);
    assert(dsd_unit_activity_push(st, &row) == 1);
    assert(push_rid(st, 7U) == 1);
    clock_at_ms(T0_MS + 900U);
    row.slot = 1U; /* not in the key: the row keeps its slot */
    DSD_SNPRINTF(row.name_frozen, sizeof(row.name_frozen), "%s", "Unit Seven");
    assert(dsd_unit_activity_push(st, &row) == 0);

    dsd_unit_activity out[4];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 2U, out, 4, &meta) == 2); /* reset: read from the start */
    assert(meta.reset == 1U && meta.held == 1U && meta.lost == 0U && meta.more == 0U && meta.last_rev == 3U);
    assert(out[0].seq == 2U && out[0].rev == 2U);
    assert(out[1].seq == 1U && out[1].rev == 3U && out[1].repeat == 2U);
    assert(out[1].first_ms == T0_MS + 100U && out[1].last_ms == T0_MS + 900U);
    assert(out[1].wall_s == (int64_t)(T0_MS / 1000U) && out[1].slot == 0U);
    assert(strcmp(out[1].name_frozen, "Unit Seven") == 0);

    /* An empty name on a later merge keeps the one held. */
    row.name_frozen[0] = '\0';
    assert(dsd_unit_activity_push(st, &row) == 0);
    assert(dsd_unit_activity_read(st, meta.instance, meta.last_rev, out, 4, &meta) == 1);
    assert(out[0].repeat == 3U && strcmp(out[0].name_frozen, "Unit Seven") == 0);
    free_state(st);
}

/* Repeat count saturates instead of wrapping. */
static void
test_repeat_saturates(void) {
    dsd_state* st = new_state();
    const dsd_unit_activity row = base_row();
    clock_at_ms(T0_MS);
    for (uint32_t i = 0; i < 70000U; i++) {
        assert(dsd_unit_activity_push(st, &row) == (i == 0U ? 1 : 0));
    }
    dsd_unit_activity out;
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, 1, &meta) == 1);
    assert(out.repeat == UINT16_MAX && out.rev == 70000U);
    free_state(st);
}

typedef void (*row_mutator)(dsd_unit_activity* row);

#define MUTATOR(name, body)                                                                                            \
    static void name(dsd_unit_activity* r) { body; }

/* One key field each (plan G8 + H2: changed assignment, identical denies for different groups, different status
   values, deny additional info, NXDN visitor group, TETRA accept type and assigned SSI, intent vs confirm stage). */
MUTATOR(m_protocol, r->protocol = DSD_UA_PROTO_P25P2)
MUTATOR(m_kind, r->kind = DSD_UA_LOCATION_REGISTRATION)
MUTATOR(m_role, r->role = DSD_UA_ROLE_ACK)
MUTATOR(m_result, r->result = DSD_UA_RESULT_DENIED)
MUTATOR(m_stage, r->stage = 1U)
MUTATOR(m_reason_set, r->reason_set = 2U)
MUTATOR(m_reason_code, r->reason_code = 0x5AU)
MUTATOR(m_subject_id, r->subject.id = 1234568U)
MUTATOR(m_subject_subtype, r->subject.subtype = DSD_UA_ID_GROUP)
MUTATOR(m_subject_valid, r->subject.valid = 0U)
MUTATOR(m_subject_home_a, r->subject.home_a = 0xBEE01U)
MUTATOR(m_subject_home_b, r->subject.home_b = 0x124U)
MUTATOR(m_subject_home_kind, r->subject.home_kind = DSD_UA_SCOPE_TETRA)
MUTATOR(m_subject_home_valid, r->subject.home_valid = 0U)
MUTATOR(m_assigned_id, r->assigned.id = 101U)
MUTATOR(m_assigned_valid, r->assigned.valid = 0U)
MUTATOR(m_actor, (r->actor.id = 9U, r->actor.subtype = DSD_UA_ID_INFRASTRUCTURE, r->actor.valid = 1U))
MUTATOR(m_recipient, (r->recipient.id = 9U, r->recipient.subtype = DSD_UA_ID_RADIO, r->recipient.valid = 1U))
MUTATOR(m_group, (r->group = 4001U, r->group_valid = 1U))
MUTATOR(m_value, (r->value = 3U, r->value_valid = 1U))
MUTATOR(m_value2, (r->value2 = 1U, r->value2_valid = 1U))
MUTATOR(m_scope_a, r->scope.a = 0xBEE01U)
MUTATOR(m_scope_b, r->scope.b = 0x124U)
MUTATOR(m_scope_c, r->scope.c = DSD_UA_SCOPE_SITE_KNOWN | 0x0102U)
MUTATOR(m_scope_kind, r->scope.kind = DSD_UA_SCOPE_P25_NAC)
MUTATOR(m_scope_category, r->scope.category = 1U)
MUTATOR(m_scope_invalid, r->scope.valid = 0U) /* rx_id is empty here: an unknown scope */
MUTATOR(m_scope_epoch, r->scope_epoch = 2U)
MUTATOR(m_freq, r->freq_hz = 851012500)

/* Fields outside the key, or held only while a valid flag is set: the pair merges. */
MUTATOR(n_slot, r->slot = 1U)
MUTATOR(n_name, DSD_SNPRINTF(r->name_frozen, sizeof(r->name_frozen), "%s", "Other"))
MUTATOR(n_stamps, (r->seq = 99U, r->rev = 99U, r->first_ms = 5U, r->last_ms = 6U, r->wall_s = 7, r->repeat = 8U))
MUTATOR(n_gated_value, r->value = 77U)
MUTATOR(n_gated_value2, r->value2 = 77U)
MUTATOR(n_gated_group, r->group = 77U)
MUTATOR(n_gated_actor, r->actor.id = 77U)
MUTATOR(n_gated_home, (r->assigned.home_a = 77U, r->assigned.home_kind = DSD_UA_SCOPE_P25))

static void
test_pairs_differing_in_one_key_field(void) {
    static const struct {
        const char* name;
        row_mutator mutate;
        int merges;
    } cases[] = {
        {"protocol", m_protocol, 0},
        {"kind", m_kind, 0},
        {"role", m_role, 0},
        {"result", m_result, 0},
        {"stage", m_stage, 0},
        {"reason_set", m_reason_set, 0},
        {"reason_code", m_reason_code, 0},
        {"subject id", m_subject_id, 0},
        {"subject subtype", m_subject_subtype, 0},
        {"subject valid", m_subject_valid, 0},
        {"subject home a", m_subject_home_a, 0},
        {"subject home b", m_subject_home_b, 0},
        {"subject home kind", m_subject_home_kind, 0},
        {"subject home valid", m_subject_home_valid, 0},
        {"assigned id", m_assigned_id, 0},
        {"assigned valid", m_assigned_valid, 0},
        {"actor", m_actor, 0},
        {"recipient", m_recipient, 0},
        {"group", m_group, 0},
        {"value", m_value, 0},
        {"value2", m_value2, 0},
        {"scope a", m_scope_a, 0},
        {"scope b", m_scope_b, 0},
        {"scope c", m_scope_c, 0},
        {"scope kind", m_scope_kind, 0},
        {"scope category", m_scope_category, 0},
        {"scope unknown", m_scope_invalid, 0},
        {"scope epoch", m_scope_epoch, 0},
        {"freq", m_freq, 0},
        {"slot", n_slot, 1},
        {"name", n_name, 1},
        {"stamps", n_stamps, 1},
        {"value without value_valid", n_gated_value, 1},
        {"value2 without value2_valid", n_gated_value2, 1},
        {"group without group_valid", n_gated_group, 1},
        {"actor without valid", n_gated_actor, 1},
        {"home without home_valid", n_gated_home, 1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_state* st = new_state();
        dsd_unit_activity a = base_row();
        dsd_unit_activity b = base_row();
        cases[i].mutate(&b);
        clock_at_ms(T0_MS);
        assert(dsd_unit_activity_push(st, &a) == 1);
        clock_at_ms(T0_MS + 10U);
        const int rc = dsd_unit_activity_push(st, &b);
        if (rc != (cases[i].merges ? 0 : 1)) {
            DSD_FPRINTF(stderr, "dedup key: %s gave %d, want %d\n", cases[i].name, rc, cases[i].merges ? 0 : 1);
            assert(0);
        }
        free_state(st);
    }
}

/* Only the most recently revised DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK rows are looked at. */
static void
test_dedup_lookback(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    assert(push_rid(st, 1U) == 1);
    for (uint32_t i = 0; i < (uint32_t)DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK - 1U; i++) {
        assert(push_rid(st, 100U + i) == 1);
    }
    assert(push_rid(st, 1U) == 0); /* 16th most recent: merged, now the newest */
    for (uint32_t i = 0; i < (uint32_t)DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK; i++) {
        assert(push_rid(st, 200U + i) == 1);
    }
    assert(push_rid(st, 1U) == 1); /* 17th: a new row */
    free_state(st);
}

/* G4: a capped read after an older row merged returns it after the newer rows, never skipping one. */
static void
test_revision_order_read_capped(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    assert(push_rid(st, 1U) == 1);
    assert(push_rid(st, 2U) == 1);
    assert(push_rid(st, 3U) == 1);

    dsd_unit_activity out;
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, 1, &meta) == 1);
    assert(out.subject.id == 1U && meta.more == 1U && meta.reset == 1U);
    uint64_t inst = meta.instance;
    uint64_t cur = meta.last_rev;

    clock_at_ms(T0_MS + 50U);
    assert(push_rid(st, 1U) == 0);

    const uint32_t want[] = {2U, 3U, 1U};
    for (size_t i = 0; i < 3U; i++) {
        assert(dsd_unit_activity_read(st, inst, cur, &out, 1, &meta) == 1);
        assert(out.subject.id == want[i] && meta.reset == 0U && meta.lost == 0U);
        assert(meta.more == (i < 2U ? 1U : 0U));
        inst = meta.instance;
        cur = meta.last_rev;
    }
    assert(out.repeat == 2U && out.rev == 4U && out.seq == 1U);
    assert(dsd_unit_activity_read(st, inst, cur, &out, 1, &meta) == 0);
    assert(meta.last_rev == cur && meta.more == 0U);

    /* All read, then the middle row merges: only it comes back. */
    clock_at_ms(T0_MS + 60U);
    assert(push_rid(st, 2U) == 0);
    assert(dsd_unit_activity_read(st, inst, cur, &out, 1, &meta) == 1);
    assert(out.subject.id == 2U && out.repeat == 2U && meta.more == 0U);

    /* cap 0 probes: nothing written, the cursor kept, more set. */
    assert(push_rid(st, 9U) == 1);
    assert(dsd_unit_activity_read(st, meta.instance, meta.last_rev, NULL, 0, &meta) == 0);
    assert(meta.more == 1U && meta.last_rev == 5U);
    free_state(st);
}

static void
test_first_attach_no_loss(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    for (uint32_t i = 0; i < 5U; i++) {
        assert(push_rid(st, 10U + i) == 1);
    }
    dsd_unit_activity out[8];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 8, &meta) == 5);
    assert(meta.reset == 1U && meta.lost == 0U && meta.more == 0U && meta.last_rev == 5U && meta.evicted_rev == 0U);
    for (uint32_t i = 0; i < 5U; i++) {
        assert(out[i].seq == i + 1U && out[i].rev == i + 1U && out[i].subject.id == 10U + i);
    }
    free_state(st);
}

static dsd_unit_activity g_rows[DSD_UNIT_ACTIVITY_CAPACITY + 64];

static void
fill(dsd_state* st, uint32_t first_rid, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        assert(push_rid(st, first_rid + i) == 1);
    }
}

static void
test_wrap_after_consume_no_loss(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    fill(st, 1U, DSD_UNIT_ACTIVITY_CAPACITY);
    uint64_t inst = 0U;
    uint64_t cur = 0U;
    uint8_t lost = 0U;
    assert(read_all(st, &inst, &cur, 100, g_rows, DSD_UNIT_ACTIVITY_CAPACITY + 64, &lost)
           == DSD_UNIT_ACTIVITY_CAPACITY);
    assert(lost == 0U && cur == (uint64_t)DSD_UNIT_ACTIVITY_CAPACITY);

    fill(st, 5000U, 10U); /* evicts revisions 1..10, all read */
    assert(read_all(st, &inst, &cur, 100, g_rows, DSD_UNIT_ACTIVITY_CAPACITY + 64, &lost) == 10);
    assert(lost == 0U);
    for (uint32_t i = 0; i < 10U; i++) {
        assert(g_rows[i].seq == (uint64_t)DSD_UNIT_ACTIVITY_CAPACITY + 1U + i && g_rows[i].subject.id == 5000U + i);
    }
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, inst, cur, NULL, 0, &meta) == 0);
    assert(meta.evicted_rev == 10U && meta.lost == 0U);
    free_state(st);
}

static void
test_wrap_of_unread_row_is_lost_once(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    fill(st, 1U, DSD_UNIT_ACTIVITY_CAPACITY);
    dsd_unit_activity out[8];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 8, &meta) == 8);
    assert(meta.lost == 0U && meta.more == 1U);
    const uint64_t inst = meta.instance;

    fill(st, 5000U, 20U); /* evicts revisions 1..20; 9..20 were not read */
    assert(dsd_unit_activity_read(st, inst, meta.last_rev, out, 5, &meta) == 5);
    assert(meta.lost == 1U && meta.reset == 0U && meta.evicted_rev == 20U);
    assert(out[0].rev == 21U && out[0].seq == 21U); /* the oldest row held is past every row lost */
    /* The loss is reported once: the next page starts past it. */
    assert(dsd_unit_activity_read(st, inst, meta.last_rev, out, 5, &meta) == 5);
    assert(meta.lost == 0U && out[0].rev == 26U);

    /* A first attach after a wrap missed rows too. */
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 1, &meta) == 1);
    assert(meta.reset == 1U && meta.lost == 1U && out[0].rev == 21U);
    free_state(st);
}

/* A full ring evicts its least recently revised row, not the first inserted: a row merged later outlives it. */
static void
test_evicts_least_recently_revised(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    fill(st, 1U, 16U);
    clock_at_ms(T0_MS + 10U);
    assert(push_rid(st, 1U) == 0);                    /* seq 1 now carries rev 17 */
    fill(st, 100U, DSD_UNIT_ACTIVITY_CAPACITY - 16U); /* full */
    fill(st, 9000U, 1U);                              /* evicts seq 2, the lowest revision */
    uint64_t inst = 0U;
    uint64_t cur = 0U;
    uint8_t lost = 0U;
    const int n = read_all(st, &inst, &cur, 64, g_rows, DSD_UNIT_ACTIVITY_CAPACITY + 64, &lost);
    assert(n == DSD_UNIT_ACTIVITY_CAPACITY && lost == 1U);
    int saw_seq1 = 0;
    for (int i = 0; i < n; i++) {
        assert(g_rows[i].seq != 2U);
        assert(i == 0 || g_rows[i].rev > g_rows[i - 1].rev);
        saw_seq1 |= g_rows[i].seq == 1U;
    }
    assert(saw_seq1);
    free_state(st);
}

static void
test_instance_change_resets(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    fill(st, 1U, 3U);
    dsd_unit_activity out[4];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 4, &meta) == 3);
    const uint64_t old_instance = meta.instance;
    const uint64_t old_rev = meta.last_rev;
    assert(old_instance != 0U);

    assert(dsd_state_ext_set(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL) == 0);
    assert(dsd_unit_activity_read(st, old_instance, old_rev, out, 4, &meta) == 0);
    assert(meta.held == 0U && meta.instance == old_instance && meta.last_rev == old_rev && meta.reset == 0U);

    fill(st, 50U, 1U);
    assert(dsd_unit_activity_read(st, old_instance, old_rev, out, 4, &meta) == 1);
    assert(meta.reset == 1U && meta.instance != old_instance && meta.instance != 0U && meta.lost == 0U);
    assert(out[0].seq == 1U && out[0].rev == 1U && out[0].subject.id == 50U);
    free_state(st);
}

static void
test_stamps_from_floored_decode_wall(void) {
    dsd_state* st = new_state();
    const dsd_unit_activity row = base_row();
    clock_at_ms(T0_MS + 999U);
    assert(dsd_unit_activity_push(st, &row) == 1);
    clock_at_ms(T0_MS + 1500U); /* the wall reading is now a second later: a merge keeps the first */
    assert(dsd_unit_activity_push(st, &row) == 0);
    dsd_unit_activity out;
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, 1, &meta) == 1);
    assert(out.wall_s == (int64_t)(T0_MS / 1000U));
    assert(out.first_ms == T0_MS + 999U && out.last_ms == T0_MS + 1500U);
    free_state(st);
}

static dsd_ua_scope
pushed_scope(dsd_state* st, int give_scope) {
    dsd_unit_activity row = base_row();
    if (!give_scope) {
        DSD_MEMSET(&row.scope, 0, sizeof(row.scope));
        row.scope.a = 5U; /* ignored: not valid */
    }
    assert(dsd_state_ext_set(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL) == 0);
    assert(dsd_unit_activity_push(st, &row) == 1);
    dsd_unit_activity out;
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, 1, &meta) == 1);
    return out.scope;
}

static void
assert_scope(const dsd_ua_scope* s, uint8_t kind, uint8_t category, uint32_t a, uint32_t b, uint32_t c) {
    assert(s->valid == 1U && s->kind == kind && s->category == category && s->a == a && s->b == b && s->c == c);
}

static void
test_scope_from_rx_id(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    dsd_ua_scope s = pushed_scope(st, 0);
    assert(s.valid == 0U && s.kind == 0U && s.a == 0U && s.b == 0U && s.c == 0U); /* nothing received */

    st->rx_id.family = DSD_RXID_FAMILY_P25;
    st->rx_id.valid = DSD_RXID_ACCESS;
    st->rx_id.access_kind = DSD_ACCESS_CODE_NAC;
    st->rx_id.access = 0x2A3U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_P25_NAC, 0U, 0x2A3U, 0U, 0U);

    st->rx_id.valid |= DSD_RXID_P25_WACN | DSD_RXID_P25_SYSID | DSD_RXID_P25_SITE;
    st->rx_id.p25_wacn = 0xBEE00U;
    st->rx_id.p25_sysid = 0x123U;
    st->rx_id.p25_rfss = 2U;
    st->rx_id.p25_site = 7U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_P25, 0U, 0xBEE00U, 0x123U, DSD_UA_SCOPE_SITE_KNOWN | 0x0207U);

    /* A scope the producer gave is kept as given. */
    s = pushed_scope(st, 1);
    assert_scope(&s, DSD_UA_SCOPE_P25, 0U, 0xBEE00U, 0x123U, DSD_UA_SCOPE_SITE_KNOWN | 0x0101U);

    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_DMR;
    st->rx_id.valid = DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE;
    st->rx_id.dmr_trunk = DSD_RXID_DMR_TRUNK_TIER3;
    st->rx_id.dmr_t3_model = 1U;
    st->rx_id.dmr_net = 12U;
    st->rx_id.dmr_site = 3U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_DMR, 0U, (DSD_RXID_DMR_TRUNK_TIER3 << 8) | 1U, 12U, DSD_UA_SCOPE_SITE_KNOWN | 3U);

    st->rx_id.valid = DSD_RXID_DMR_SITE | DSD_RXID_ACCESS;
    st->rx_id.dmr_trunk = DSD_RXID_DMR_TRUNK_CAPACITY_PLUS;
    st->rx_id.access_kind = DSD_ACCESS_CODE_COLOR_CODE;
    st->rx_id.access = 5U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_DMR, 0U, (DSD_RXID_DMR_TRUNK_CAPACITY_PLUS << 8) | 5U, 3U,
                 DSD_UA_SCOPE_SITE_KNOWN | 3U);

    st->rx_id.valid = DSD_RXID_ACCESS; /* Tier II: a colour code names no system */
    s = pushed_scope(st, 0);
    assert(s.valid == 0U && s.kind == 0U && s.a == 0U && s.c == 0U);

    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_NXDN;
    st->rx_id.valid = DSD_RXID_NXDN_LOCATION;
    st->rx_id.nxdn_category = 1U;
    st->rx_id.nxdn_sys = 321U;
    st->rx_id.nxdn_site = 9U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_NXDN, 1U, 0x101U, 321U, DSD_UA_SCOPE_SITE_KNOWN | 9U);

    st->rx_id.valid = DSD_RXID_NXDN_TYPED_SYSTEM;
    st->rx_id.nxdn_typed_sys = 0x12345U;
    st->rx_id.nxdn_typed_site = 4U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_NXDN, 0U, 0x200U, 0x12345U, DSD_UA_SCOPE_SITE_KNOWN | 4U);

    DSD_MEMSET(&st->rx_id, 0, sizeof(st->rx_id));
    st->rx_id.family = DSD_RXID_FAMILY_TETRA;
    st->rx_id.valid = DSD_RXID_TETRA_NET;
    st->rx_id.tetra_mcc = 901U;
    st->rx_id.tetra_mnc = 9999U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_TETRA, 0U, 901U, 9999U, 0U);
    st->rx_id.valid |= DSD_RXID_TETRA_LA;
    st->rx_id.tetra_la = 77U;
    s = pushed_scope(st, 0);
    assert_scope(&s, DSD_UA_SCOPE_TETRA, 0U, 901U, 9999U, DSD_UA_SCOPE_SITE_KNOWN | 77U);
    free_state(st);
}

static void
test_name_frozen_terminated_at_a_character(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    dsd_unit_activity row = base_row();
    DSD_MEMSET(row.name_frozen, 'a', sizeof(row.name_frozen)); /* no NUL: cut to 31 bytes */
    assert(dsd_unit_activity_push(st, &row) == 1);
    /* 30 ASCII bytes then a two-byte character in bytes 30-31: the character would be cut, so it goes. */
    dsd_unit_activity accented = row_for(2U);
    DSD_MEMSET(accented.name_frozen, 'b', 30U);
    accented.name_frozen[30] = (char)0xC3;
    accented.name_frozen[31] = (char)0xA9;
    assert(dsd_unit_activity_push(st, &accented) == 1);
    dsd_unit_activity out[2];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, out, 2, &meta) == 2);
    assert(strlen(out[0].name_frozen) == 31U && out[0].name_frozen[30] == 'a');
    assert(strlen(out[1].name_frozen) == 30U && out[1].name_frozen[29] == 'b');
    free_state(st);
}

static void
test_rejects_bad_rows_and_args(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    dsd_unit_activity row = base_row();
    row.protocol = 0U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.kind = (uint8_t)(DSD_UA_AUTHENTICATION + 1);
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.role = 0U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.result = (uint8_t)(DSD_UA_RESULT_UNKNOWN + 1);
    assert(dsd_unit_activity_push(st, &row) == -1);
    /* A valid identity, home or scope needs a value of its enum. */
    row = base_row();
    row.subject.subtype = 0U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.subject.subtype = (uint8_t)(DSD_UA_ID_UNKNOWN + 1);
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.assigned.subtype = 9U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.subject.home_kind = DSD_UA_SCOPE_NONE;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.subject.home_kind = (uint8_t)(DSD_UA_SCOPE_P25_NAC + 1);
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.actor.valid = 1U;
    row.actor.subtype = DSD_UA_ID_INFRASTRUCTURE;
    row.actor.home_valid = 1U;
    row.actor.home_kind = 7U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.recipient.valid = 1U;
    row.recipient.subtype = 0U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.scope.kind = DSD_UA_SCOPE_NONE;
    assert(dsd_unit_activity_push(st, &row) == -1);
    row = base_row();
    row.scope.kind = 7U;
    assert(dsd_unit_activity_push(st, &row) == -1);
    assert(dsd_state_ext_get(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL); /* still nothing allocated */
    /* Out-of-enum values in parts that are not valid are zeroed, not refused. */
    row = base_row();
    row.subject.home_valid = 0U;
    row.subject.home_kind = 7U;
    row.actor.subtype = 9U;
    row.scope.valid = 0U;
    row.scope.kind = 7U;
    assert(dsd_unit_activity_push(st, &row) == 1);
    dsd_unit_activity held;
    dsd_unit_activity_read_meta held_meta;
    assert(dsd_unit_activity_read(st, 0U, 0U, &held, 1, &held_meta) == 1);
    assert(held.subject.home_kind == 0U && held.actor.subtype == 0U && held.scope.kind == 0U);
    assert(dsd_state_ext_set(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL) == 0);

    row = base_row();
    assert(dsd_unit_activity_push(NULL, &row) == -1);
    assert(dsd_unit_activity_push(st, NULL) == -1);
    assert(dsd_state_ext_get(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL); /* nothing allocated */

    dsd_unit_activity_read_meta meta;
    dsd_unit_activity out;
    assert(dsd_unit_activity_read(NULL, 0U, 0U, &out, 1, &meta) == -1);
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, 1, NULL) == -1);
    assert(dsd_unit_activity_read(st, 0U, 0U, &out, -1, &meta) == -1);
    assert(dsd_unit_activity_read(st, 0U, 0U, NULL, 1, &meta) == -1);
    assert(dsd_unit_activity_copy_snapshot(NULL, st) == -1);
    assert(dsd_unit_activity_copy_snapshot(st, NULL) == -1);
    assert(dsd_unit_activity_copy_snapshot(st, st) == 0);
    free_state(st);
}

static void
test_push_allocation_failure(void) {
    dsd_state* st = new_state();
    clock_at_ms(T0_MS);
    dsd_unit_activity_test_alloc_fail_after(0);
    assert(push_rid(st, 1U) == -1);
    assert(dsd_state_ext_get(st, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL);
    dsd_unit_activity_test_alloc_fail_after(-1);
    assert(push_rid(st, 1U) == 1);
    free_state(st);
}

/* Read every row of @p st from the start, in revision order. */
static int
read_from_start(const dsd_state* st, dsd_unit_activity* out, int cap, dsd_unit_activity_read_meta* meta) {
    return dsd_unit_activity_read(st, 0U, 0U, out, cap, meta);
}

static dsd_unit_activity g_src_rows[DSD_UNIT_ACTIVITY_CAPACITY];
static dsd_unit_activity g_dst_rows[DSD_UNIT_ACTIVITY_CAPACITY];

static void
assert_copy_matches(const dsd_state* dst, const dsd_state* src) {
    dsd_unit_activity_read_meta sm;
    dsd_unit_activity_read_meta dm;
    const int ns = read_from_start(src, g_src_rows, DSD_UNIT_ACTIVITY_CAPACITY, &sm);
    const int nd = read_from_start(dst, g_dst_rows, DSD_UNIT_ACTIVITY_CAPACITY, &dm);
    assert(ns == nd && sm.instance == dm.instance && sm.last_rev == dm.last_rev && sm.evicted_rev == dm.evicted_rev);
    for (int i = 0; i < ns; i++) {
        assert(g_src_rows[i].seq == g_dst_rows[i].seq && g_src_rows[i].rev == g_dst_rows[i].rev);
        assert(g_src_rows[i].repeat == g_dst_rows[i].repeat && g_src_rows[i].last_ms == g_dst_rows[i].last_ms);
        assert(g_src_rows[i].subject.id == g_dst_rows[i].subject.id);
    }
}

static void
test_snapshot_copies_only_changed_rows(void) {
    dsd_state* src = new_state();
    dsd_state* dst = new_state();
    clock_at_ms(T0_MS);
    fill(src, 1U, 3U);

    uint64_t copied = dsd_unit_activity_test_rows_copied();
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_test_rows_copied() - copied == 3U);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY)
           != dsd_state_ext_get(src, DSD_STATE_EXT_CORE_UNIT_ACTIVITY));
    assert(dst->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] != NULL);
    assert_copy_matches(dst, src);

    /* A consumer of the copy reads two rows. */
    dsd_unit_activity out[8];
    dsd_unit_activity_read_meta meta;
    assert(dsd_unit_activity_read(dst, 0U, 0U, out, 2, &meta) == 2);
    const uint64_t inst = meta.instance;
    uint64_t cur = meta.last_rev;

    /* Same revision: nothing copied. */
    copied = dsd_unit_activity_test_rows_copied();
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_test_rows_copied() == copied);

    /* A merge of row 1 and two new rows: three rows copied. */
    clock_at_ms(T0_MS + 20U);
    assert(push_rid(src, 1U) == 0);
    fill(src, 10U, 2U);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_test_rows_copied() - copied == 3U);
    assert_copy_matches(dst, src);

    /* The consumer goes on from its cursor without a skip, in revision order: row 3 (rev 3), the merged row 1 (rev 4),
       then the new rows (revs 5 and 6). */
    const uint32_t want[] = {3U, 1U, 10U, 11U};
    assert(dsd_unit_activity_read(dst, inst, cur, out, 8, &meta) == 4);
    for (size_t i = 0; i < 4U; i++) {
        assert(out[i].subject.id == want[i]);
    }
    assert(meta.reset == 0U && meta.lost == 0U && meta.more == 0U);
    cur = meta.last_rev;

    /* The source wraps between copies: the copy follows slot for slot. */
    fill(src, 20000U, DSD_UNIT_ACTIVITY_CAPACITY + 100U);
    copied = dsd_unit_activity_test_rows_copied();
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_test_rows_copied() - copied == (uint64_t)DSD_UNIT_ACTIVITY_CAPACITY);
    assert_copy_matches(dst, src);
    assert(dsd_unit_activity_read(dst, inst, cur, out, 1, &meta) == 1);
    assert(meta.lost == 1U && meta.reset == 0U);

    free_state(dst);
    free_state(src);
}

static void
test_snapshot_instance_change_copies_all(void) {
    dsd_state* src = new_state();
    dsd_state* dst = new_state();
    clock_at_ms(T0_MS);
    fill(src, 1U, 5U);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    const void* dst_ring = dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY);

    /* A new decoder session: another ring, its revisions starting again. */
    assert(dsd_state_ext_set(src, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL) == 0);
    fill(src, 100U, 7U);
    const uint64_t copied = dsd_unit_activity_test_rows_copied();
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_test_rows_copied() - copied == 7U);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == dst_ring); /* storage reused */
    assert_copy_matches(dst, src);

    /* A source without a ring takes the copy's away. */
    assert(dsd_state_ext_set(src, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL) == 0);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL);
    free_state(dst);
    free_state(src);
}

static void
test_snapshot_allocation_failure_keeps_cursor(void) {
    dsd_state* src = new_state();
    dsd_state* dst = new_state();
    clock_at_ms(T0_MS);
    fill(src, 1U, 4U);
    dsd_unit_activity out[8];
    dsd_unit_activity_read_meta meta;

    dsd_unit_activity_test_alloc_fail_after(0);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == -1);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL);
    /* A reader of the copy holding a cursor of this instance reads nothing and keeps it. */
    dsd_unit_activity_read_meta src_meta;
    assert(dsd_unit_activity_read(src, 0U, 0U, out, 2, &src_meta) == 2);
    assert(dsd_unit_activity_read(dst, src_meta.instance, src_meta.last_rev, out, 8, &meta) == 0);
    assert(meta.held == 0U && meta.instance == src_meta.instance && meta.last_rev == src_meta.last_rev);
    assert(meta.reset == 0U && meta.lost == 0U);

    dsd_unit_activity_test_alloc_fail_after(-1);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_unit_activity_read(dst, meta.instance, meta.last_rev, out, 8, &meta) == 2);
    assert(out[0].subject.id == 3U && out[1].subject.id == 4U && meta.reset == 0U && meta.lost == 0U);
    free_state(dst);
    free_state(src);
}

/* A struct copy that left the destination pointing at the source's ring is detached, never freed through it. */
static void
test_snapshot_detaches_an_alias(void) {
    dsd_state* src = new_state();
    dsd_state* dst = new_state();
    clock_at_ms(T0_MS);
    fill(src, 1U, 2U);
    dst->state_ext[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = src->state_ext[DSD_STATE_EXT_CORE_UNIT_ACTIVITY];
    dst->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = src->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY];

    dsd_unit_activity_test_alloc_fail_after(0);
    assert(dsd_unit_activity_copy_snapshot(dst, src) == -1);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) == NULL);
    assert(dst->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] == NULL);
    dsd_unit_activity_test_alloc_fail_after(-1);

    dst->state_ext[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = src->state_ext[DSD_STATE_EXT_CORE_UNIT_ACTIVITY];
    dst->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = src->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY];
    assert(dsd_unit_activity_copy_snapshot(dst, src) == 0);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY) != NULL);
    assert(dsd_state_ext_get(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY)
           != dsd_state_ext_get(src, DSD_STATE_EXT_CORE_UNIT_ACTIVITY));
    assert_copy_matches(dst, src);
    free_state(dst); /* frees the copy only: the source still reads */
    dsd_unit_activity out[2];
    dsd_unit_activity_read_meta meta;
    assert(read_from_start(src, out, 2, &meta) == 2);
    free_state(src);
}

int
main(void) {
    dsd_decode_clock_use_test(T0_MS * UINT64_C(1000000));
    test_dedup_anchored_window();
    test_dereg_repeat_without_assigned_merges();
    test_backwards_time_never_merges();
    test_merge_bumps_repeat_last_and_rev();
    test_repeat_saturates();
    test_pairs_differing_in_one_key_field();
    test_dedup_lookback();
    test_revision_order_read_capped();
    test_first_attach_no_loss();
    test_wrap_after_consume_no_loss();
    test_wrap_of_unread_row_is_lost_once();
    test_evicts_least_recently_revised();
    test_instance_change_resets();
    test_stamps_from_floored_decode_wall();
    test_scope_from_rx_id();
    test_name_frozen_terminated_at_a_character();
    test_rejects_bad_rows_and_args();
    test_push_allocation_failure();
    test_snapshot_copies_only_changed_rows();
    test_snapshot_instance_change_copies_all();
    test_snapshot_allocation_failure_keeps_cursor();
    test_snapshot_detaches_an_alias();
    dsd_decode_clock_use_system();
    printf("CORE_UNIT_ACTIVITY: OK\n");
    return 0;
}
