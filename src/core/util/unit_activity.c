// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The unit activity ring (<dsd-neo/core/unit_activity.h>): fixed row storage with a revision-ordered list, the
 * push with its dedup merge, the revision-ordered read and the incremental snapshot copy.
 *
 * Rows never move: a slot keeps its row until the row is evicted, and the slot then takes the new row. A doubly linked
 * list through the slots orders them by revision (oldest first); a push or merge moves its row to the list's end. The
 * least recently revised row is the one evicted, so every row held is newer than every row evicted, and a snapshot
 * mirrors the source slot for slot: it copies the rows revised since its own revision and the links.
 */
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert((int)DSD_STATE_EXT_CORE_UNIT_ACTIVITY < (int)DSD_STATE_EXT_MAX,
               "the unit activity slot is in the table");
_Static_assert(DSD_UNIT_ACTIVITY_CAPACITY < 0xFFFF, "slot links are uint16_t, 0xFFFF marking none");

#define UA_NONE 0xFFFFU

typedef struct dsd_unit_activity_ring {
    uint64_t instance;        /* drawn per ring (never 0); a snapshot copy holds its source's */
    uint64_t rev;             /* the latest revision stamped, 0 before the first push */
    uint64_t seq;             /* the latest seq assigned */
    uint64_t evicted_rev_max; /* the revision of the latest row evicted (the highest ever, as eviction goes by rev) */
    uint32_t count;           /* slots in use: 0 .. count-1 */
    uint16_t oldest, newest;  /* revision list ends, UA_NONE while empty */
    uint16_t prev[DSD_UNIT_ACTIVITY_CAPACITY]; /* toward the older revision */
    uint16_t next[DSD_UNIT_ACTIVITY_CAPACITY]; /* toward the newer revision */
    dsd_unit_activity rows[DSD_UNIT_ACTIVITY_CAPACITY];
} dsd_unit_activity_ring;

static dsd_atomic_u64 g_next_instance = {1u};

#ifdef DSD_NEO_TEST_HOOKS
static long g_alloc_remaining = -1;
static uint64_t g_rows_copied = 0;

void
dsd_unit_activity_test_alloc_fail_after(long successful_allocations) {
    g_alloc_remaining = successful_allocations;
}

uint64_t
dsd_unit_activity_test_rows_copied(void) {
    return g_rows_copied;
}

#define UA_COUNT_COPIED(n) (g_rows_copied += (uint64_t)(n))
#else
#define UA_COUNT_COPIED(n) ((void)(n))
#endif

static dsd_unit_activity_ring*
ua_ring_alloc(void) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_alloc_remaining == 0) {
        return NULL;
    }
    if (g_alloc_remaining > 0) {
        --g_alloc_remaining;
    }
#endif
    dsd_unit_activity_ring* r = calloc(1, sizeof(*r));
    if (r) {
        r->oldest = r->newest = UA_NONE;
    }
    return r;
}

static void
ua_ring_cleanup(void* r) {
    free(r);
}

/* Allocate a ring and hand it to @p state's slot (its cleanup frees it with the state). NULL on failure. */
static dsd_unit_activity_ring*
ua_ring_install(dsd_state* state) {
    dsd_unit_activity_ring* r = ua_ring_alloc();
    if (!r) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, r, ua_ring_cleanup) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

static const dsd_unit_activity_ring*
ua_ring_get_const(const dsd_state* state) {
    return (const dsd_unit_activity_ring*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_UNIT_ACTIVITY);
}

static void
ua_list_unlink(dsd_unit_activity_ring* r, uint16_t i) {
    const uint16_t p = r->prev[i];
    const uint16_t n = r->next[i];
    if (p != UA_NONE) {
        r->next[p] = n;
    } else {
        r->oldest = n;
    }
    if (n != UA_NONE) {
        r->prev[n] = p;
    } else {
        r->newest = p;
    }
    r->prev[i] = r->next[i] = UA_NONE;
}

static void
ua_list_append(dsd_unit_activity_ring* r, uint16_t i) {
    r->prev[i] = r->newest;
    r->next[i] = UA_NONE;
    if (r->newest != UA_NONE) {
        r->next[r->newest] = i;
    } else {
        r->oldest = i;
    }
    r->newest = i;
}

/* ---- Normalisation ---- */

static void
ua_identity_normalize(const dsd_ua_identity* in, dsd_ua_identity* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!in->valid) {
        return;
    }
    out->id = in->id;
    out->subtype = in->subtype;
    out->valid = 1U;
    if (in->home_valid) {
        out->home_kind = in->home_kind;
        out->home_valid = 1U;
        out->home_a = in->home_a;
        out->home_b = in->home_b;
    }
}

static void
ua_scope_normalize(const dsd_ua_scope* in, dsd_ua_scope* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!in->valid) {
        return;
    }
    out->kind = in->kind;
    out->category = in->category;
    out->valid = 1U;
    out->a = in->a;
    out->b = in->b;
    out->c = in->c;
}

/* The learned-alias scope kinds are the reception scope kinds (<dsd-neo/core/unit_activity.h>, dsd_ua_scope): one
 * mapping, dsd_alias_scope_from_rxid(), names the system of both. */
_Static_assert((int)DSD_UA_SCOPE_NONE == (int)DSD_ALIAS_SCOPE_NONE, "scope kinds agree");
_Static_assert((int)DSD_UA_SCOPE_P25 == (int)DSD_ALIAS_SCOPE_P25, "scope kinds agree");
_Static_assert((int)DSD_UA_SCOPE_DMR == (int)DSD_ALIAS_SCOPE_DMR, "scope kinds agree");
_Static_assert((int)DSD_UA_SCOPE_NXDN == (int)DSD_ALIAS_SCOPE_NXDN, "scope kinds agree");
_Static_assert((int)DSD_UA_SCOPE_TETRA == (int)DSD_ALIAS_SCOPE_TETRA, "scope kinds agree");
_Static_assert((int)DSD_UA_SCOPE_P25_NAC == (int)DSD_ALIAS_SCOPE_P25_NAC, "scope kinds agree");

/* What a reception scope adds to the alias scope of the same reception: the site (c) and the NXDN Type-C category.
 * P25 by NAC has no site. */
static void
ua_scope_add_site(const dsd_received_identity* rx, dsd_ua_scope* out) {
    switch (out->kind) {
        case DSD_UA_SCOPE_P25:
            if (rx->valid & DSD_RXID_P25_SITE) {
                out->c = DSD_UA_SCOPE_SITE_KNOWN | ((uint32_t)rx->p25_rfss << 8) | rx->p25_site;
            }
            break;
        case DSD_UA_SCOPE_DMR:
            if (rx->valid & DSD_RXID_DMR_SITE) {
                out->c = DSD_UA_SCOPE_SITE_KNOWN | rx->dmr_site;
            }
            break;
        case DSD_UA_SCOPE_NXDN:
            if (out->a & 0x100U) { /* Type-C location: a = 0x100 | category */
                out->category = rx->nxdn_category;
                out->c = DSD_UA_SCOPE_SITE_KNOWN | rx->nxdn_site;
            } else { /* Type-D SRV_INFO system */
                out->c = DSD_UA_SCOPE_SITE_KNOWN | rx->nxdn_typed_site;
            }
            break;
        case DSD_UA_SCOPE_TETRA:
            if (rx->valid & DSD_RXID_TETRA_LA) {
                out->c = DSD_UA_SCOPE_SITE_KNOWN | rx->tetra_la;
            }
            break;
        default: break;
    }
}

/* The reception scope of @p rx in @p out: the learned-alias scope of the reception (kind, a, b) plus its site; valid
 * only when the record names a system. */
static void
ua_scope_from_rxid(const dsd_received_identity* rx, dsd_ua_scope* out) {
    dsd_alias_scope alias;
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!dsd_alias_scope_from_rxid(rx, &alias)) {
        return;
    }
    out->kind = alias.kind;
    out->a = alias.a;
    out->b = alias.b;
    ua_scope_add_site(rx, out);
    out->valid = 1U;
}

/* Copy @p in (any bytes) into zeroed @p out as a NUL-terminated UTF-8 string: up to its NUL, whole characters only
 * (dsd_utf8_whole_prefix_len()), cut to the field when it fills it. */
static void
ua_name_copy(char* out, size_t out_sz, const char* in, size_t in_sz) {
    const char* nul = memchr(in, '\0', in_sz);
    const size_t len = dsd_utf8_whole_prefix_len(in, nul ? (size_t)(nul - in) : in_sz, out_sz - 1U);
    DSD_MEMCPY(out, in, len);
}

static int
ua_scope_kind_valid(uint8_t kind) {
    return kind >= DSD_UA_SCOPE_P25 && kind <= DSD_UA_SCOPE_P25_NAC;
}

/* A valid identity names its subtype, and a valid home its system kind; an invalid one is zeroed, whatever it holds. */
static int
ua_identity_enums_valid(const dsd_ua_identity* id) {
    if (!id->valid) {
        return 1;
    }
    return id->subtype >= DSD_UA_ID_RADIO && id->subtype <= DSD_UA_ID_UNKNOWN
           && (!id->home_valid || ua_scope_kind_valid(id->home_kind));
}

static int
ua_enums_valid(const dsd_unit_activity* row) {
    return row->protocol >= DSD_UA_PROTO_P25P1 && row->protocol <= DSD_UA_PROTO_TETRA
           && row->kind >= DSD_UA_REGISTRATION && row->kind <= DSD_UA_AUTHENTICATION && row->role >= DSD_UA_ROLE_COMMAND
           && row->role <= DSD_UA_ROLE_REPORT && row->result <= DSD_UA_RESULT_UNKNOWN;
}

static int
ua_row_enums_valid(const dsd_unit_activity* row) {
    return ua_enums_valid(row) && ua_identity_enums_valid(&row->subject) && ua_identity_enums_valid(&row->actor)
           && ua_identity_enums_valid(&row->assigned) && ua_identity_enums_valid(&row->recipient)
           && (!row->scope.valid || ua_scope_kind_valid(row->scope.kind));
}

/* The row as the ring keeps it, in zeroed @p out (no padding byte left set): invalid parts zeroed, the name
 * terminated, the scope from rx_id when the producer gave none. Stamps are left for the push. */
static int
ua_normalize(const dsd_state* state, const dsd_unit_activity* in, dsd_unit_activity* out) {
    if (!ua_row_enums_valid(in)) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->protocol = in->protocol;
    out->kind = in->kind;
    out->role = in->role;
    out->result = in->result;
    out->stage = in->stage;
    out->reason_set = in->reason_set;
    out->reason_code = in->reason_code;
    out->slot = in->slot;
    out->freq_hz = in->freq_hz;
    ua_identity_normalize(&in->subject, &out->subject);
    out->subject_nonlocal = (in->subject.valid && in->subject_nonlocal) ? 1U : 0U;
    ua_identity_normalize(&in->actor, &out->actor);
    ua_identity_normalize(&in->assigned, &out->assigned);
    ua_identity_normalize(&in->recipient, &out->recipient);
    out->group_valid = in->group_valid ? 1U : 0U;
    out->group = in->group_valid ? in->group : 0U;
    out->value_valid = in->value_valid ? 1U : 0U;
    out->value = in->value_valid ? in->value : 0U;
    out->value2_valid = in->value2_valid ? 1U : 0U;
    out->value2 = in->value2_valid ? in->value2 : 0U;
    if (in->scope.valid) {
        ua_scope_normalize(&in->scope, &out->scope);
    } else {
        ua_scope_from_rxid(&state->rx_id, &out->scope);
    }
    ua_name_copy(out->name_frozen, sizeof(out->name_frozen), in->name_frozen, sizeof(in->name_frozen));
    return 0;
}

/* ---- Merge key ---- */

static int
ua_identity_equal(const dsd_ua_identity* a, const dsd_ua_identity* b) {
    return a->valid == b->valid && a->id == b->id && a->subtype == b->subtype && a->home_valid == b->home_valid
           && a->home_kind == b->home_kind && a->home_a == b->home_a && a->home_b == b->home_b;
}

static int
ua_scope_equal(const dsd_ua_scope* a, const dsd_ua_scope* b) {
    return a->valid == b->valid && a->kind == b->kind && a->category == b->category && a->a == b->a && a->b == b->b
           && a->c == b->c;
}

static int
ua_header_equal(const dsd_unit_activity* a, const dsd_unit_activity* b) {
    return a->protocol == b->protocol && a->kind == b->kind && a->role == b->role && a->result == b->result
           && a->stage == b->stage && a->reason_set == b->reason_set && a->reason_code == b->reason_code;
}

static int
ua_payload_equal(const dsd_unit_activity* a, const dsd_unit_activity* b) {
    return a->group_valid == b->group_valid && a->group == b->group && a->value_valid == b->value_valid
           && a->value == b->value && a->value2_valid == b->value2_valid && a->value2 == b->value2;
}

static int
ua_reception_equal(const dsd_unit_activity* a, const dsd_unit_activity* b) {
    return ua_scope_equal(&a->scope, &b->scope) && a->freq_hz == b->freq_hz;
}

/* A deregistration's assigned identity is not in its message: the producer reads it from the registration the first
 * acknowledgement removes, so a repeat that finds none is the same deregistration and keeps the held identity. */
static int
ua_assigned_equal(const dsd_unit_activity* held, const dsd_unit_activity* rec) {
    if (rec->kind == DSD_UA_DEREGISTRATION && !rec->assigned.valid) {
        return 1;
    }
    return ua_identity_equal(&held->assigned, &rec->assigned);
}

/* Every field the row shows but the stamps, repeat, slot and the name (dsd_unit_activity_push()). */
static int
ua_key_equal(const dsd_unit_activity* held, const dsd_unit_activity* rec) {
    return ua_header_equal(held, rec) && ua_payload_equal(held, rec) && ua_reception_equal(held, rec)
           && ua_identity_equal(&held->subject, &rec->subject) && held->subject_nonlocal == rec->subject_nonlocal
           && ua_identity_equal(&held->actor, &rec->actor) && ua_assigned_equal(held, rec)
           && ua_identity_equal(&held->recipient, &rec->recipient);
}

/* The row @p rec merges into: the most recently revised of the lookback rows with its key, when the clock has not gone
 * back past that row's last push and its anchored window is still open. UA_NONE: insert. */
static uint16_t
ua_find_merge(const dsd_unit_activity_ring* r, const dsd_unit_activity* rec, uint64_t now_ms) {
    uint16_t i = r->newest;
    for (int n = 0; i != UA_NONE && n < DSD_UNIT_ACTIVITY_DEDUP_LOOKBACK; n++, i = r->prev[i]) {
        const dsd_unit_activity* held = &r->rows[i];
        if (!ua_key_equal(held, rec)) {
            continue;
        }
        if (now_ms >= held->last_ms && now_ms - held->first_ms < (uint64_t)DSD_UNIT_ACTIVITY_DEDUP_MS) {
            return i;
        }
        return UA_NONE;
    }
    return UA_NONE;
}

/* ---- Push ---- */

static void
ua_merge(dsd_unit_activity_ring* r, uint16_t i, const dsd_unit_activity* rec, uint64_t now_ms) {
    dsd_unit_activity* held = &r->rows[i];
    if (held->repeat < UINT16_MAX) {
        held->repeat++;
    }
    held->last_ms = now_ms;
    /* name_frozen is the latest pushed name: a merge that resolved one replaces the held one. */
    if (rec->name_frozen[0] != '\0') {
        DSD_MEMCPY(held->name_frozen, rec->name_frozen, sizeof(held->name_frozen));
    }
    held->rev = ++r->rev;
    ua_list_unlink(r, i);
    ua_list_append(r, i);
}

static void
ua_insert(dsd_unit_activity_ring* r, dsd_unit_activity* rec, uint64_t now_ms) {
    uint16_t slot;
    if (r->count < (uint32_t)DSD_UNIT_ACTIVITY_CAPACITY) {
        slot = (uint16_t)r->count++;
    } else {
        slot = r->oldest;
        r->evicted_rev_max = r->rows[slot].rev;
        ua_list_unlink(r, slot);
    }
    rec->seq = ++r->seq;
    rec->rev = ++r->rev;
    rec->first_ms = rec->last_ms = now_ms;
    /* The floored decode wall reading, as the history rows are stamped (never dsd_decode_time()). */
    rec->wall_s = (int64_t)floor(dsd_decode_now_realtime_s());
    rec->repeat = 1U;
    r->rows[slot] = *rec;
    ua_list_append(r, slot);
}

int
dsd_unit_activity_push(dsd_state* state, const dsd_unit_activity* row) {
    if (!state || !row) {
        return -1;
    }
    dsd_unit_activity rec;
    if (ua_normalize(state, row, &rec) != 0) {
        return -1;
    }
    dsd_unit_activity_ring* r = DSD_STATE_EXT_GET_AS(dsd_unit_activity_ring, state, DSD_STATE_EXT_CORE_UNIT_ACTIVITY);
    if (!r) {
        r = ua_ring_install(state);
        if (!r) {
            return -1;
        }
        do {
            r->instance = dsd_atomic_u64_fetch_add_relaxed(&g_next_instance, 1u);
        } while (!r->instance);
    }
    const uint64_t now_ms = dsd_decode_now_mono_ms();
    const uint16_t into = ua_find_merge(r, &rec, now_ms);
    if (into != UA_NONE) {
        ua_merge(r, into, &rec, now_ms);
        return 0;
    }
    ua_insert(r, &rec, now_ms);
    return 1;
}

/* ---- Read ---- */

/* The oldest-revised row revised after @p cursor, UA_NONE when there is none: a walk back from the newest. */
static uint16_t
ua_first_after(const dsd_unit_activity_ring* r, uint64_t cursor) {
    uint16_t first = UA_NONE;
    for (uint16_t i = r->newest; i != UA_NONE && r->rows[i].rev > cursor; i = r->prev[i]) {
        first = i;
    }
    return first;
}

int
dsd_unit_activity_read(const dsd_state* state, uint64_t instance, uint64_t after_rev, dsd_unit_activity* out, int cap,
                       dsd_unit_activity_read_meta* meta) {
    if (!state || !meta || cap < 0 || (!out && cap > 0)) {
        return -1;
    }
    DSD_MEMSET(meta, 0, sizeof(*meta));
    meta->instance = instance;
    meta->last_rev = after_rev;
    const dsd_unit_activity_ring* r = ua_ring_get_const(state);
    if (!r) {
        return 0;
    }
    meta->held = 1U;
    meta->evicted_rev = r->evicted_rev_max;
    uint64_t cursor = after_rev;
    if (instance != r->instance) {
        meta->reset = 1U;
        cursor = 0U;
        meta->instance = r->instance;
        meta->last_rev = 0U;
    }
    if (cursor > r->rev) {
        return 0;
    }
    meta->lost = r->evicted_rev_max > cursor ? 1U : 0U;
    int n = 0;
    uint16_t i = ua_first_after(r, cursor);
    for (; i != UA_NONE && n < cap; i = r->next[i]) {
        out[n++] = r->rows[i];
        meta->last_rev = r->rows[i].rev;
    }
    meta->more = i != UA_NONE ? 1U : 0U;
    return n;
}

/* ---- Snapshot copy ---- */

/* Copy into @p d, slot for slot, the rows of @p s revised after @p d's revision (every row when @p full). */
static void
ua_copy_rows(dsd_unit_activity_ring* d, const dsd_unit_activity_ring* s, int full) {
    if (full) {
        if (s->count) {
            DSD_MEMCPY(d->rows, s->rows, (size_t)s->count * sizeof(s->rows[0]));
        }
        UA_COUNT_COPIED(s->count);
        return;
    }
    uint32_t copied = 0U;
    for (uint16_t i = s->newest; i != UA_NONE && s->rows[i].rev > d->rev; i = s->prev[i]) {
        d->rows[i] = s->rows[i];
        copied++;
    }
    UA_COUNT_COPIED(copied);
}

int
dsd_unit_activity_copy_snapshot(dsd_state* dst, const dsd_state* src) {
    if (!dst || !src) {
        return -1;
    }
    if (dst == src) {
        return 0;
    }
    const dsd_unit_activity_ring* s = ua_ring_get_const(src);
    dsd_unit_activity_ring* d = DSD_STATE_EXT_GET_AS(dsd_unit_activity_ring, dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY);
    if (d && d == s) {
        /* A struct copy left @p dst pointing at the source's ring: detach it, never free it. */
        dst->state_ext[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = NULL;
        dst->state_ext_cleanup[DSD_STATE_EXT_CORE_UNIT_ACTIVITY] = NULL;
        d = NULL;
    }
    if (!s) {
        return dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_UNIT_ACTIVITY, NULL, NULL);
    }
    if (!d) {
        d = ua_ring_install(dst);
        if (!d) {
            return -1;
        }
    }
    if (d->instance == s->instance && d->rev == s->rev) {
        return 0;
    }
    ua_copy_rows(d, s, d->instance != s->instance || d->rev > s->rev);
    DSD_MEMCPY(d->prev, s->prev, sizeof(d->prev));
    DSD_MEMCPY(d->next, s->next, sizeof(d->next));
    d->instance = s->instance;
    d->rev = s->rev;
    d->seq = s->seq;
    d->evicted_rev_max = s->evicted_rev_max;
    d->count = s->count;
    d->oldest = s->oldest;
    d->newest = s->newest;
    return 0;
}

/* ---- Naming at push (producers) ---- */

int
dsd_unit_activity_identity_at_home(const dsd_state* state, const dsd_ua_identity* id) {
    if (state == NULL || id == NULL || !id->valid) {
        return 0;
    }
    if (!id->home_valid) {
        return 1;
    }
    dsd_alias_scope here;
    (void)dsd_alias_scope_current(state, &here);
    /* dsd_ua_scope_kind and dsd_alias_scope_kind agree value for value (the static asserts above). */
    return here.valid && here.kind == id->home_kind && here.a == id->home_a && here.b == id->home_b;
}

/* A radio's name in the current reception's scope; 1 when found. */
static int
ua_freeze_radio(const dsd_state* state, const dsd_ua_identity* id, char* name, size_t cap) {
    char mode[8];
    return id->valid && id->subtype == DSD_UA_ID_RADIO
           && dsd_source_label_lookup_scoped(state, NULL, id->id, mode, sizeof mode, name, cap);
}

/* The name the view gives a live row (ua_subject_name()), in the same order: the assigned identity first; when it has
 * none, the subject radio while it is at home and not nonlocal, or a group subject's talkgroup label. */
static int
ua_freeze_lookup(const dsd_state* state, const dsd_unit_activity* row, char* name, size_t cap) {
    if (ua_freeze_radio(state, &row->assigned, name, cap)) {
        return 1;
    }
    if (!row->subject.valid || row->subject_nonlocal) {
        return 0;
    }
    if (row->subject.subtype == DSD_UA_ID_GROUP) {
        return dsd_tg_policy_lookup_group_label(state, row->subject.id, NULL, 0U, name, cap);
    }
    return dsd_unit_activity_identity_at_home(state, &row->subject) && ua_freeze_radio(state, &row->subject, name, cap);
}

void
dsd_unit_activity_freeze_name(const dsd_state* state, dsd_unit_activity* row) {
    if (state == NULL || row == NULL) {
        return;
    }
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    if (!ua_freeze_lookup(state, row, name, sizeof name)) {
        return;
    }
    /* A name that fills the field is copied unterminated; the push cuts it at a character boundary. */
    const size_t len = strlen(name);
    DSD_MEMCPY(row->name_frozen, name, len < sizeof row->name_frozen ? len + 1U : sizeof row->name_frozen);
}
