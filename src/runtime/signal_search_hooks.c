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

/* The visit tag: the decoder sets and clears it, and a publish reads it on whichever thread closes a recording (the
   decoder, the watchdog, a command), so the pair is read and written whole under its own lock. Nothing calls out while
   holding it. */
static dsd_mutex_t visit_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
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

static void
visit_lock(void) {
    if (atomic_load(&visit_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&visit_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&visit_mu);
            atomic_store(&visit_mu_state, 2);
        } else {
            while (atomic_load(&visit_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    dsd_mutex_lock(&visit_mu);
}

void
dsd_signal_search_visit_set(uint64_t session_id, uint64_t hit_id) {
    const int named = session_id != 0 && hit_id != 0;
    visit_lock();
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
    visit_lock();
    const uint64_t session = visit_session;
    const uint64_t hit = visit_hit;
    dsd_mutex_unlock(&visit_mu);
    if (session_id) {
        *session_id = session;
    }
    if (hit_id) {
        *hit_id = hit;
    }
    return session != 0;
}
