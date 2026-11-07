// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
static dsd_signal_search_evidence_fn evidence_fn = NULL;
static dsd_signal_search_active_fn active_fn = NULL;
static dsd_signal_search_site_fn site_fn = NULL;
static dsd_signal_search_call_fn call_fn = NULL;
static dsd_signal_search_p25p2_codes_fn p25p2_codes_fn = NULL;
static dsd_signal_search_p25p2_code_fn p25p2_code_fn = NULL;

/* The visit tag: the decoder sets and clears it, and a publish reads it on whichever thread closes a recording (the
   decoder, the watchdog, a command), so the pair is read and written whole under its own lock. Nothing calls out while
   holding it. */
static dsd_mutex_t visit_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready, -1 = failed. */
static atomic_int visit_mu_state;
static uint64_t visit_session, visit_hit;

void
dsd_signal_search_active_set(dsd_signal_search_active_fn fn) {
    active_fn = fn;
}

int
dsd_signal_search_is_running(const dsd_state* state) {
    return active_fn ? active_fn(state) : 0;
}

void
dsd_signal_search_evidence_set(dsd_signal_search_evidence_fn fn) {
    evidence_fn = fn;
}

void
dsd_signal_search_note_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                const char* access) {
    dsd_signal_search_note_evidence_at(state, protocol, -1, -1, target, source, access);
}

void
dsd_signal_search_note_evidence_at(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                                   uint32_t source, const char* access) {
    if (evidence_fn) {
        evidence_fn(state, protocol, sync, slot, target, source, access);
    }
}

void
dsd_signal_search_site_set(dsd_signal_search_site_fn fn) {
    site_fn = fn;
}

void
dsd_signal_search_note_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    if (site_fn && note) {
        site_fn(state, note);
    }
}

void
dsd_signal_search_call_set(dsd_signal_search_call_fn fn) {
    call_fn = fn;
}

void
dsd_signal_search_note_call(const dsd_state* state, uint8_t slot) {
    if (call_fn) {
        call_fn(state, slot);
    }
}

void
dsd_signal_search_p25p2_codes_set(dsd_signal_search_p25p2_codes_fn fn) {
    p25p2_codes_fn = fn;
}

size_t
dsd_signal_search_p25p2_codes(const dsd_state* state, uint64_t* out, size_t cap) {
    if (!p25p2_codes_fn || !out || cap == 0) {
        return 0;
    }
    const size_t n = p25p2_codes_fn(state, out, cap);
    return n < cap ? n : cap;
}

void
dsd_signal_search_p25p2_code_set(dsd_signal_search_p25p2_code_fn fn) {
    p25p2_code_fn = fn;
}

void
dsd_signal_search_note_p25p2_code(const dsd_state* state, uint64_t code, int source) {
    if (p25p2_code_fn) {
        p25p2_code_fn(state, code, source);
    }
}

/* Whether the lock is usable. A lock that failed to initialise stays unusable: nothing is set through it, and the tag
   reads as clear, so recordings are published untagged rather than under a pair that might be torn. */
static int
visit_mu_ready(void) {
    int state = atomic_load(&visit_mu_state);
    if (state == 2 || state == -1) {
        return state == 2;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&visit_mu_state, &expected, 1)) {
        const int ready = dsd_mutex_init(&visit_mu) == 0;
        atomic_store(&visit_mu_state, ready ? 2 : -1);
        return ready;
    }
    while ((state = atomic_load(&visit_mu_state)) == 1) {
        dsd_thread_yield();
    }
    return state == 2;
}

static int
visit_lock(void) {
    return visit_mu_ready() && dsd_mutex_lock(&visit_mu) == 0;
}

void
dsd_signal_search_visit_set(uint64_t session_id, uint64_t hit_id) {
    const int named = session_id != 0 && hit_id != 0;
    if (!visit_lock()) {
        return;
    }
    visit_session = named ? session_id : 0;
    visit_hit = named ? hit_id : 0;
    dsd_mutex_unlock(&visit_mu);
}

void
dsd_signal_search_visit_clear(void) {
    dsd_signal_search_visit_set(0, 0);
}

int
dsd_signal_search_visit_get(uint64_t* session_id, uint64_t* hit_id) {
    uint64_t session = 0, hit = 0;
    if (visit_lock()) {
        session = visit_session;
        hit = visit_hit;
        dsd_mutex_unlock(&visit_mu);
    }
    if (session_id) {
        *session_id = session;
    }
    if (hit_id) {
        *hit_id = hit;
    }
    return session != 0;
}
