// SPDX-License-Identifier: GPL-3.0-or-later

#include "tetra_state.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <stdlib.h>

/* The state holds operator air keys, so it is wiped before it is freed. */
static void
tetra_state_free(void* ptr) {
    if (ptr) {
        DSD_SECURE_ZERO(ptr, sizeof(tetra_state_t));
        free(ptr);
    }
}

tetra_state_t*
tetra_st(dsd_state* state) {
    if (!state) {
        return NULL;
    }
    tetra_state_t* ts = DSD_STATE_EXT_GET_AS(tetra_state_t, state, DSD_STATE_EXT_PROTO_TETRA);
    if (ts) {
        return ts;
    }
    ts = (tetra_state_t*)calloc(1, sizeof(*ts));
    if (!ts) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_TETRA, ts, tetra_state_free) != 0) {
        free(ts);
        return NULL;
    }
    return ts;
}

const tetra_state_t*
tetra_st_peek(const dsd_state* state) {
    /* What a session with no TETRA state reads: every field at its zero default. */
    static const tetra_state_t k_empty;
    if (!state) {
        return NULL;
    }
    const tetra_state_t* ts = (const tetra_state_t*)dsd_state_ext_get_const(state, DSD_STATE_EXT_PROTO_TETRA);
    return ts ? ts : &k_empty;
}

void
tetra_state_reset(dsd_state* state) {
    if (!state) {
        return;
    }
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_TETRA, NULL, NULL);
}
