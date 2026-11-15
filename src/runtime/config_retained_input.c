// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The [input] of the config a session loaded, which a session running an I/Q replay saves in place of its own input
 * (issue #743): a replay has no INI form, and a save that wrote it as a file input left a config the next start
 * refuses. See <dsd-neo/runtime/config.h> for who sets, replaces and drops it.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "dsd-neo/core/state_fwd.h"

/* The [input] block of dsdneoUserConfig, has_input through input_warn_db_is_set. */
#define RETAINED_INPUT_BEGIN offsetof(dsdneoUserConfig, has_input)
#define RETAINED_INPUT_END                                                                                             \
    (offsetof(dsdneoUserConfig, input_warn_db_is_set) + sizeof(((dsdneoUserConfig*)0)->input_warn_db_is_set))

typedef struct {
    /* Which retain the copy came from, unique for the process, so a snapshot that holds it copies nothing again; a
       per-state count would repeat in the next session's state, which may land at the freed one's address. */
    uint64_t revision;
    unsigned char input[RETAINED_INPUT_END - RETAINED_INPUT_BEGIN];
} retained_input;

static dsd_atomic_u64 g_retained_input_revision = {0U};

static retained_input*
retained_input_of(const dsd_state* state) {
    return (retained_input*)dsd_state_ext_get(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT);
}

/* The state's copy, allocated empty when it has none; NULL when that allocation fails. */
static retained_input*
retained_input_slot(dsd_state* state) {
    retained_input* kept = retained_input_of(state);
    if (kept != NULL) {
        return kept;
    }
    kept = (retained_input*)calloc(1, sizeof *kept);
    if (kept != NULL) {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT, kept, free);
    }
    return kept;
}

int
dsd_user_config_retain_input(dsd_state* state, const dsdneoUserConfig* cfg) {
    if (state == NULL) {
        return -1;
    }
    if (cfg == NULL || !cfg->has_input) {
        dsd_user_config_forget_retained_input(state);
        return 0;
    }
    retained_input* kept = retained_input_slot(state);
    if (kept == NULL) {
        return -1;
    }
    DSD_MEMCPY(kept->input, (const unsigned char*)cfg + RETAINED_INPUT_BEGIN, sizeof kept->input);
    kept->revision = dsd_atomic_u64_fetch_add_relaxed(&g_retained_input_revision, 1U) + 1U;
    return 0;
}

void
dsd_user_config_forget_retained_input(dsd_state* state) {
    if (retained_input_of(state) != NULL) {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT, NULL, NULL);
    }
}

int
dsd_user_config_retained_input(const dsd_state* state, dsdneoUserConfig* cfg) {
    const retained_input* kept =
        (const retained_input*)dsd_state_ext_get_const(state, DSD_STATE_EXT_RUNTIME_CONFIG_INPUT);
    if (kept == NULL || cfg == NULL) {
        return 0;
    }
    DSD_MEMCPY((unsigned char*)cfg + RETAINED_INPUT_BEGIN, kept->input, sizeof kept->input);
    return 1;
}

void
dsd_user_config_copy_retained_input(dsd_state* dst, const dsd_state* src) {
    if (dst == NULL || dst == src) {
        return;
    }
    const retained_input* from = retained_input_of(src);
    if (from == NULL) {
        dsd_user_config_forget_retained_input(dst);
        return;
    }
    const retained_input* held = retained_input_of(dst);
    if (held != NULL && held->revision == from->revision) {
        return;
    }
    retained_input* to = retained_input_slot(dst);
    if (to != NULL) {
        DSD_MEMCPY(to, from, sizeof *to);
    }
}
