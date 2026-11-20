// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The alias lookups position admission (src/core/gps/position_feed.c) uses, for tests that compile protocol decoders
 * directly and link the real admission without the core library: no reception has an alias scope and no source has a
 * label. Admission is otherwise the real one (verdicts, receive policy, ranges, feed-only scopes, stamps).
 */

#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stddef.h>
#include <stdint.h>

struct dsd_received_identity;

int
dsd_alias_scope_from_rxid(const struct dsd_received_identity* rx, dsd_alias_scope* out) {
    (void)rx;
    if (out != NULL) {
        *out = (dsd_alias_scope){0};
    }
    return 0;
}

int
dsd_source_label_lookup_scoped(const dsd_state* state, const dsd_alias_scope* scope, uint32_t id, char* mode,
                               size_t mode_sz, char* name, size_t name_sz) {
    (void)state;
    (void)scope;
    (void)id;
    if (mode != NULL && mode_sz > 0U) {
        mode[0] = '\0';
    }
    if (name != NULL && name_sz > 0U) {
        name[0] = '\0';
    }
    return 0;
}

int
dsd_tg_policy_lookup_learned_label(const dsd_state* state, const dsd_alias_scope* scope, uint32_t id, char* name,
                                   size_t name_sz) {
    (void)state;
    (void)scope;
    (void)id;
    if (name != NULL && name_sz > 0U) {
        name[0] = '\0';
    }
    return 0;
}
