// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The scope and label lookups position admission (src/core/gps/position_feed.c) makes, for tests that compile
 * dsd_gps.c and position_feed.c from an explicit source list without the alias and talkgroup modules: every reception
 * has no alias scope and every source no label.
 */

#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/state_fwd.h"

int
dsd_alias_scope_from_rxid(const struct dsd_received_identity* rx, dsd_alias_scope* out) {
    (void)rx;
    if (out != NULL) {
        DSD_MEMSET(out, 0, sizeof(*out));
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
