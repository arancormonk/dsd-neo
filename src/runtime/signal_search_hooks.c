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
    if (evidence_fn) {
        evidence_fn(state, protocol, target, source, access);
    }
}
