// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Hosts that deliberately stub talkgroup policy do not load row group files.
 * Production ownership and import are exercised by CORE_SCAN_PROFILE. */
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stddef.h>

dsd_tg_policy_store*
dsd_tg_policy_retain(const dsd_state* state) {
    (void)state;
    return NULL;
}

void
dsd_tg_policy_release(dsd_tg_policy_store* store) {
    (void)store;
}

void
dsd_tg_policy_install(dsd_state* state, dsd_tg_policy_store* store) {
    (void)state;
    (void)store;
}

void
dsd_tg_policy_restore(dsd_state* state, dsd_tg_policy_store* store) {
    (void)state;
    (void)store;
}

int
dsd_tg_policy_load(const char* path, dsd_tg_policy_store** out) {
    (void)path;
    (void)out;
    return -1;
}
