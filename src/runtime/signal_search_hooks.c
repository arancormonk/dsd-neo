// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
static dsd_signal_search_evidence_fn evidence_fn = NULL;
static dsd_signal_search_active_fn active_fn = NULL;
static dsd_signal_search_site_fn site_fn = NULL;
static dsd_signal_search_call_fn call_fn = NULL;

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
