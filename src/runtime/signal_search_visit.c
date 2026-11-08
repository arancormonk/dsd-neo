// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The Signal search visit tag (issue #809). It has its own file so that its lock-failure test can build it with the lock
   calls redirected without building a second, different copy of the hook table in signal_search_hooks.c. */
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>

/* The visit tag: the decoder sets and clears it, and a publish reads it on whichever thread closes a recording (the
   decoder, the watchdog, a command), so the pair is read and written whole under its own lock. Nothing calls out while
   holding it. */
static dsd_mutex_t visit_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready, -1 = failed. */
static atomic_int visit_mu_state;
static uint64_t visit_session, visit_hit;

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
