// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The verified position feed (<dsd-neo/core/position.h>): admission, the reception origin, the run's ring and
 * the repeat rule.
 *
 * The ring keeps seq-ordered records: the record with seq s sits at index (s - 1) % DSD_POSITION_FEED_CAPACITY, so
 * the newest seq alone says which records are held. The repeat rule remembers, per track, the last kept record and
 * when it was kept (decode clock); a full table forgets the least recently kept track (a feed-wide counter, so a
 * clock that goes back never decides which track is forgotten).
 */
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/nonce.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

_Static_assert((int)DSD_STATE_EXT_CORE_POSITION_FEED < (int)DSD_STATE_EXT_MAX,
               "the position feed slot is in the table");
_Static_assert((int)DSD_POS_SCOPE_P25 == (int)DSD_ALIAS_SCOPE_P25 && (int)DSD_POS_SCOPE_DMR == (int)DSD_ALIAS_SCOPE_DMR
                   && (int)DSD_POS_SCOPE_NXDN == (int)DSD_ALIAS_SCOPE_NXDN
                   && (int)DSD_POS_SCOPE_TETRA == (int)DSD_ALIAS_SCOPE_TETRA
                   && (int)DSD_POS_SCOPE_P25_NAC == (int)DSD_ALIAS_SCOPE_P25_NAC
                   && (int)DSD_ALIAS_SCOPE_LAST == (int)DSD_POS_SCOPE_P25_NAC,
               "the position scope kinds 1..5 are the alias scope kinds");
_Static_assert((int)DSD_POS_SCOPE_FEED_EDACS > (int)DSD_ALIAS_SCOPE_LAST, "feed-only kinds never name an alias scope");
_Static_assert(sizeof(dsd_position_fix) <= 256U, "a position record stays small: the ring holds 1,024");
_Static_assert(DSD_SYNC_TETRA_SB_NEG < (int)DSD_POSITION_PROTOCOL_NONE, "every sync type fits the protocol byte");

#define POS_LAT_MAX_E7 900000000
#define POS_LON_MAX_E7 1800000000

typedef struct {
    dsd_position_fix last; /* the track's last kept record */
    uint64_t kept_ms;      /* dsd_decode_now_mono_ms() when it was kept */
    uint64_t touch;        /* the feed's touch counter when it was kept: the least is forgotten first */
} pos_track;

typedef struct {
    uint64_t stream_id;  /* never 0 */
    uint64_t newest_seq; /* 0 while empty */
    uint64_t touch;      /* bumped by every keep */
    uint32_t track_count;
    dsd_position_fix entries[DSD_POSITION_FEED_CAPACITY];
    pos_track tracks[DSD_POSITION_FEED_TRACKS];
} pos_feed;

static dsd_atomic_u64 g_next_stream = {1U};
static dsd_atomic_u64 g_rev = {0U};
static dsd_atomic_u64 g_proc_instance = {0U};

/* ---- Record ---- */

void
dsd_position_fix_init(dsd_position_fix* fix) {
    if (!fix) {
        return;
    }
    DSD_MEMSET(fix, 0, sizeof(*fix));
    fix->alt_dm = DSD_POSITION_ALT_UNKNOWN;
    fix->err_m = DSD_POSITION_ERR_UNKNOWN;
    fix->speed_dkmh = DSD_POSITION_SPEED_UNKNOWN;
    fix->heading_ddeg = DSD_POSITION_HEADING_UNKNOWN;
    fix->age_class = DSD_POSITION_AGE_CLASS_NONE;
    fix->sats = DSD_POSITION_SATS_NONE;
    fix->protocol = DSD_POSITION_PROTOCOL_NONE;
}

uint64_t
dsd_position_proc_instance(void) {
    const uint64_t held = dsd_atomic_u64_load_acquire(&g_proc_instance);
    if (held != 0U) {
        return held;
    }
    uint64_t drawn = 0U;
    dsd_nonce_fill(&drawn, sizeof drawn);
    if (drawn == 0U) {
        drawn = 1U;
    }
    uint64_t expected = 0U;
    /* The first draw stored wins; a racing caller returns the winner's. */
    if (dsd_atomic_u64_compare_exchange_relaxed(&g_proc_instance, &expected, drawn)) {
        return drawn;
    }
    return expected;
}

uint64_t
dsd_position_last_rev(void) {
    return dsd_atomic_u64_load_acquire(&g_rev);
}

/* ---- Scopes and origin ---- */

static int
pos_scope_equal(const dsd_position_scope* a, const dsd_position_scope* b) {
    return a->kind == b->kind && a->a == b->a && a->b == b->b;
}

/* The alias scope @p s names: 1 for kinds 1..5, 0 (with the invalid scope) for none and the feed-only kinds. */
static int
pos_scope_to_alias(const dsd_position_scope* s, dsd_alias_scope* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (s->kind == DSD_POS_SCOPE_NONE || s->kind > DSD_ALIAS_SCOPE_LAST) {
        return 0;
    }
    out->kind = s->kind;
    out->a = s->a;
    out->b = s->b;
    out->valid = 1U;
    return 1;
}

/* The feed-only scope of a reception that has no alias scope (D7): EDACS site and system, else the control channel
 * the identity holds, else the tuned frequency. 0 (and the none scope) when there is none. */
static int
pos_scope_feed_only(const dsd_received_identity* rx, uint32_t tuned_hz, dsd_position_scope* out) {
    const uint32_t edacs = DSD_RXID_EDACS_SITE | DSD_RXID_EDACS_SYSTEM;
    if (rx->family == DSD_RXID_FAMILY_EDACS && (rx->valid & edacs) != 0U) {
        out->kind = DSD_POS_SCOPE_FEED_EDACS;
        out->a = (rx->valid & DSD_RXID_EDACS_SITE) ? (0x10000U | rx->edacs_site) : 0U;
        out->b = (rx->valid & DSD_RXID_EDACS_SYSTEM) ? (0x10000U | rx->edacs_system) : 0U;
        return 1;
    }
    if ((rx->valid & DSD_RXID_CC_FREQ) != 0U && rx->cc_freq_hz != 0U) {
        out->kind = DSD_POS_SCOPE_FEED_CC_FREQ;
        out->a = rx->cc_freq_hz;
        return 1;
    }
    if (tuned_hz != 0U) {
        out->kind = DSD_POS_SCOPE_FEED_TUNED_FREQ;
        out->a = tuned_hz;
        return 1;
    }
    return 0;
}

int
dsd_position_scope_from_rxid(const struct dsd_received_identity* rx, uint32_t tuned_hz, dsd_position_scope* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!rx) {
        return 0;
    }
    dsd_alias_scope alias;
    if (dsd_alias_scope_from_rxid(rx, &alias)) {
        out->kind = alias.kind;
        out->a = alias.a;
        out->b = alias.b;
        return 1;
    }
    return pos_scope_feed_only(rx, tuned_hz, out);
}

/* The reception generation: the received identity's epoch low, the trunk tuning generation high. */
static uint64_t
pos_generation(const dsd_state* state) {
    return ((dsd_trunk_tuning_generation() & 0xFFFFFFFFU) << 32) | (uint64_t)state->rx_id.epoch;
}

void
dsd_position_origin_capture(const dsd_opts* opts, const dsd_state* state, dsd_position_origin* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!state) {
        return;
    }
    const uint32_t tuned_hz = dsd_opts_tuned_freq_hz(opts);
    (void)dsd_position_scope_from_rxid(&state->rx_id, tuned_hz, &out->scope);
    out->freq_hz = (int64_t)tuned_hz;
    out->gen = pos_generation(state);
    out->valid = 1U;
}

int
dsd_position_origin_matches(const dsd_state* state, const dsd_position_origin* origin) {
    return state && origin && origin->valid && origin->gen == pos_generation(state);
}

/* ---- Tracks ---- */

/* The protocol family a track belongs to (dsd_sync_protocol_family()): P25 Phase 1 and 2, or DMR base and mobile
 * bursts, name one track. */
static int
pos_protocol_family(uint8_t protocol) {
    return dsd_sync_protocol_family(protocol);
}

int
dsd_position_protocol_family(int protocol) {
    if (protocol < 0 || protocol >= (int)DSD_POSITION_PROTOCOL_NONE) {
        return -1;
    }
    return pos_protocol_family((uint8_t)protocol);
}

/* M17 and D-STAR name their sources by callsign and carry no system identity. */
static int
pos_protocol_is_callsign(uint8_t protocol) {
    const int family = pos_protocol_family(protocol);
    return family == 5 || family == 6;
}

static int
pos_anonymous(const dsd_position_fix* f) {
    if ((f->flags & (DSD_POS_FLAG_SOURCE_HASHED | DSD_POS_FLAG_SOURCE_UNVERIFIED)) != 0U) {
        return 1;
    }
    return f->source_id == 0U && f->source_text[0] == '\0';
}

int
dsd_position_same_track(const dsd_position_fix* a, const dsd_position_fix* b) {
    if (!a || !b || pos_anonymous(a) || pos_anonymous(b)) {
        return 0;
    }
    return pos_protocol_family(a->protocol) == pos_protocol_family(b->protocol)
           && pos_scope_equal(&a->sender_scope, &b->sender_scope) && a->identity_freq_hz == b->identity_freq_hz
           && a->source_id == b->source_id && strncmp(a->source_text, b->source_text, sizeof(a->source_text)) == 0;
}

/* Every field a reader shows of a report (spec 2.3 step 8): equal on all of them is a repeat. */
static int
pos_observable_equal(const dsd_position_fix* a, const dsd_position_fix* b) {
    return a->kind == b->kind && a->lat_e7 == b->lat_e7 && a->lon_e7 == b->lon_e7 && a->alt_dm == b->alt_dm
           && a->speed_dkmh == b->speed_dkmh && a->heading_ddeg == b->heading_ddeg && a->err_m == b->err_m
           && a->age_class == b->age_class && a->sats == b->sats && a->target_id == b->target_id && a->flags == b->flags
           && a->fix_time_s == b->fix_time_s;
}

/* ---- Admission steps ---- */

static int
pos_well_formed(const dsd_position_fix* f) {
    return f->kind >= (uint8_t)DSD_POS_DMR_LRRP && f->kind <= (uint8_t)DSD_POS_KIND_LAST
           && f->protocol != DSD_POSITION_PROTOCOL_NONE && f->slot <= 1U;
}

/* Step 2: without checked provenance (or with a hashed source) the source, callsign, target and group flag go; the
 * report format stays. Admission owns SOURCE_UNVERIFIED; a hashed source stays marked hashed instead. */
static void
pos_clear_identity(dsd_position_fix* r, int source_verified) {
    const int hashed = (r->flags & DSD_POS_FLAG_SOURCE_HASHED) != 0U;
    r->flags = (uint16_t)(r->flags & ~(unsigned)DSD_POS_FLAG_SOURCE_UNVERIFIED);
    if (source_verified == 1 && !hashed) {
        return;
    }
    r->source_id = 0U;
    r->target_id = 0U;
    DSD_MEMSET(r->source_text, 0, sizeof(r->source_text));
    r->flags = (uint16_t)(r->flags & ~(unsigned)DSD_POS_FLAG_TARGET_GROUP);
    /* The sender's own network is attribution too: without provenance the report names only where it was heard. */
    DSD_MEMSET(&r->sender_scope, 0, sizeof(r->sender_scope));
    if (!hashed) {
        r->flags = (uint16_t)(r->flags | (unsigned)DSD_POS_FLAG_SOURCE_UNVERIFIED);
    }
}

/* Step 4: the generic value checks. Out of range or exactly (0,0) refuses; an out-of-range heading becomes unknown. */
static int
pos_values_ok(dsd_position_fix* r) {
    if (r->lat_e7 < -POS_LAT_MAX_E7 || r->lat_e7 > POS_LAT_MAX_E7 || r->lon_e7 < -POS_LON_MAX_E7
        || r->lon_e7 > POS_LON_MAX_E7) {
        return 0;
    }
    if (r->lat_e7 == 0 && r->lon_e7 == 0) {
        return 0;
    }
    if (r->heading_ddeg > DSD_POSITION_HEADING_MAX) {
        r->heading_ddeg = DSD_POSITION_HEADING_UNKNOWN;
    }
    r->flags = (uint16_t)(r->flags & (unsigned)DSD_POS_FLAG_ALL);
    /* Text is kept only as whole UTF-8 characters, NUL-terminated. */
    const size_t text_len = strnlen(r->source_text, sizeof(r->source_text) - 1U);
    const size_t keep = dsd_utf8_whole_prefix_len(r->source_text, text_len, sizeof(r->source_text) - 1U);
    DSD_MEMSET(r->source_text + keep, 0, sizeof(r->source_text) - keep);
    return 1;
}

/* Step 5, the label: never for an anonymous source. A local sender (its sender scope equals the reception scope, of
 * whatever kind: an alias scope, a feed-only scope or none) is named as call rows name it, by the scoped lookup in the
 * reception's alias scope (the radio list first, then the group list; an invalid scope reads the rows that name a
 * radio everywhere). A foreign sender (its own network differs from the reception's) skips the radio list and the
 * unscoped rows, which name local radios: only a label learned in its own scope names it, and none when that scope is
 * no alias scope. */
static void
pos_freeze_label(const dsd_state* state, dsd_position_fix* r) {
    DSD_MEMSET(r->label, 0, sizeof(r->label));
    if (pos_anonymous(r) || r->source_id == 0U) {
        return;
    }
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    char mode[8];
    int found = 0;
    dsd_alias_scope scope;
    if (pos_scope_equal(&r->sender_scope, &r->reception_scope)) {
        (void)pos_scope_to_alias(&r->reception_scope, &scope);
        found = dsd_source_label_lookup_scoped(state, &scope, r->source_id, mode, sizeof mode, name, sizeof name);
    } else if (pos_scope_to_alias(&r->sender_scope, &scope)) {
        found = dsd_tg_policy_lookup_learned_label(state, &scope, r->source_id, name, sizeof name);
    }
    if (!found) {
        return;
    }
    const size_t len = dsd_utf8_whole_prefix_len(name, strlen(name), sizeof(r->label) - 1U);
    DSD_MEMCPY(r->label, name, len);
}

/* Whether a producer set the sender's own network: an alias scope kind (1..5). */
static int
pos_sender_scope_named(const dsd_position_scope* s) {
    return s->kind != DSD_POS_SCOPE_NONE && s->kind <= DSD_ALIAS_SCOPE_LAST;
}

/* Step 5: the scopes of the reception the report came from, the identity discriminator and the label. */
static void
pos_freeze(const dsd_state* state, dsd_position_fix* r, const dsd_position_origin* origin) {
    r->freq_hz = origin->freq_hz;
    r->origin_gen = origin->gen;
    if (pos_protocol_is_callsign(r->protocol)) {
        DSD_MEMSET(&r->reception_scope, 0, sizeof(r->reception_scope));
        DSD_MEMSET(&r->sender_scope, 0, sizeof(r->sender_scope));
    } else {
        r->reception_scope = origin->scope;
        /* Only a producer-set alias scope names the sender's own network; none, a feed-only or an undefined kind (and
         * a NONE carrying values) is unknown: the sender is taken as heard here. */
        if (!pos_sender_scope_named(&r->sender_scope)) {
            r->sender_scope = r->reception_scope;
        }
    }
    DSD_MEMSET(r->reception_scope.pad, 0, sizeof(r->reception_scope.pad));
    DSD_MEMSET(r->sender_scope.pad, 0, sizeof(r->sender_scope.pad));
    r->identity_freq_hz = r->sender_scope.kind == DSD_POS_SCOPE_P25_NAC ? r->freq_hz : 0;
    pos_freeze_label(state, r);
}

/* Step 6. */
static void
pos_stamp(dsd_position_fix* r) {
    r->stream_id = 0U;
    r->seq = 0U;
    r->rev = dsd_atomic_u64_fetch_add_release(&g_rev, 1U) + 1U;
    r->proc_instance = dsd_position_proc_instance();
    r->rx_time_s = (int64_t)dsd_decode_time();
    r->rx_clock = (uint8_t)dsd_decode_clock_source();
}

/* ---- Feed ---- */

static pos_track*
pos_track_find(pos_feed* f, const dsd_position_fix* r) {
    for (uint32_t i = 0; i < f->track_count; i++) {
        if (dsd_position_same_track(&f->tracks[i].last, r)) {
            return &f->tracks[i];
        }
    }
    return NULL;
}

/* A slot for a new track: a free one, else the least recently kept track's. */
static pos_track*
pos_track_take(pos_feed* f) {
    if (f->track_count < (uint32_t)DSD_POSITION_FEED_TRACKS) {
        return &f->tracks[f->track_count++];
    }
    pos_track* oldest = &f->tracks[0];
    for (uint32_t i = 1; i < f->track_count; i++) {
        if (f->tracks[i].touch < oldest->touch) {
            oldest = &f->tracks[i];
        }
    }
    return oldest;
}

/* Step 8. A repeat needs the clock not to have gone back past the kept record (as the unit activity merge). With
 * unsigned readings a backward step already wraps now_ms - kept_ms past the window; the explicit guard keeps the rule
 * if this ever moves to signed arithmetic. The ring is written here, so its slot is read through the mutable state. */
static dsd_position_admit_result
pos_feed_keep(dsd_state* state, dsd_position_fix* r) {
    pos_feed* f = (pos_feed*)state->state_ext[DSD_STATE_EXT_CORE_POSITION_FEED];
    if (!f) {
        return DSD_POSITION_ADMITTED_NOT_KEPT;
    }
    const uint64_t now_ms = dsd_decode_now_mono_ms();
    pos_track* t = pos_anonymous(r) ? NULL : pos_track_find(f, r);
    if (t && pos_observable_equal(&t->last, r) && now_ms >= t->kept_ms
        && now_ms - t->kept_ms < (uint64_t)DSD_POSITION_REPEAT_MS) {
        return DSD_POSITION_ADMITTED_NOT_KEPT;
    }
    r->stream_id = f->stream_id;
    r->seq = ++f->newest_seq;
    f->entries[(r->seq - 1U) % (uint64_t)DSD_POSITION_FEED_CAPACITY] = *r;
    if (!pos_anonymous(r)) {
        if (!t) {
            t = pos_track_take(f);
        }
        t->last = *r;
        t->kept_ms = now_ms;
        t->touch = ++f->touch;
    }
    return DSD_POSITION_ADMITTED_KEPT;
}

dsd_position_admit_result
dsd_position_admit(const dsd_opts* opts, dsd_state* state, dsd_position_fix* fix, int pos_verified, int source_verified,
                   const dsd_position_origin* origin) {
    if (!state || !fix || pos_verified != 1 || !pos_well_formed(fix)) {
        return DSD_POSITION_REFUSED;
    }
    if (!dsd_receive_allows(opts, state, fix->protocol, fix->slot)) {
        return DSD_POSITION_REFUSED;
    }
    dsd_position_fix rec = *fix;
    pos_clear_identity(&rec, source_verified);
    dsd_position_origin here;
    if (origin) {
        if (!dsd_position_origin_matches(state, origin)) {
            return DSD_POSITION_REFUSED;
        }
        here = *origin;
    } else {
        dsd_position_origin_capture(opts, state, &here);
    }
    if (!pos_values_ok(&rec)) {
        return DSD_POSITION_REFUSED;
    }
    pos_freeze(state, &rec, &here);
    pos_stamp(&rec);
    const dsd_position_admit_result result = pos_feed_keep(state, &rec);
    *fix = rec;
    return result;
}

/* ---- Ring lifetime and read ---- */

static void
pos_feed_cleanup(void* p) {
    free(p);
}

int
dsd_position_feed_start(dsd_state* state) {
    if (!state) {
        return -1;
    }
    pos_feed* f = DSD_STATE_EXT_GET_AS(pos_feed, state, DSD_STATE_EXT_CORE_POSITION_FEED);
    if (f) {
        DSD_MEMSET(f, 0, sizeof(*f));
    } else {
        f = (pos_feed*)calloc(1, sizeof(*f));
        if (!f) {
            return -1;
        }
        if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_POSITION_FEED, f, pos_feed_cleanup) != 0) {
            free(f);
            return -1;
        }
    }
    do {
        f->stream_id = dsd_atomic_u64_fetch_add_relaxed(&g_next_stream, 1U);
    } while (f->stream_id == 0U);
    /* The process instance is drawn here, at run start, so no report pays for the draw on the decode path. */
    (void)dsd_position_proc_instance();
    return 0;
}

uint64_t
dsd_position_feed_stream_id(const dsd_state* state) {
    const pos_feed* f = (const pos_feed*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_POSITION_FEED);
    return f ? f->stream_id : 0U;
}

int
dsd_position_feed_copy_since(const dsd_state* state, uint64_t after_seq, dsd_position_fix* out, size_t cap,
                             dsd_position_feed_meta* meta) {
    if (!state || !meta || (!out && cap > 0U)) {
        return -1;
    }
    DSD_MEMSET(meta, 0, sizeof(*meta));
    const pos_feed* f = (const pos_feed*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_POSITION_FEED);
    if (!f) {
        return 0;
    }
    meta->stream_id = f->stream_id;
    meta->last_seq = after_seq;
    if (f->newest_seq == 0U) {
        return 0;
    }
    const uint64_t capacity = (uint64_t)DSD_POSITION_FEED_CAPACITY;
    meta->newest_seq = f->newest_seq;
    meta->oldest_seq = f->newest_seq > capacity ? f->newest_seq - capacity + 1U : 1U;
    if (after_seq >= f->newest_seq) {
        return 0;
    }
    uint64_t start = after_seq + 1U;
    if (start < meta->oldest_seq) {
        meta->lost = 1U;
        start = meta->oldest_seq;
    }
    const uint64_t available = f->newest_seq - start + 1U;
    const uint64_t n = available < (uint64_t)cap ? available : (uint64_t)cap;
    for (uint64_t i = 0; i < n; i++) {
        out[i] = f->entries[(start + i - 1U) % capacity];
    }
    if (n > 0U) {
        meta->last_seq = start + n - 1U;
    }
    meta->more = n < available ? 1U : 0U;
    return (int)n;
}
