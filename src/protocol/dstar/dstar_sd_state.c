// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dstar_sd_state.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdlib.h>

_Static_assert((int)DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA < (int)DSD_STATE_EXT_MAX,
               "the D-STAR slow-data extension slot must fit the state's extension table");

dstar_sd_state*
dstar_sd_st(dsd_state* state) {
    if (!state) {
        return NULL;
    }
    dstar_sd_state* sd = DSD_STATE_EXT_GET_AS(dstar_sd_state, state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA);
    if (sd) {
        return sd;
    }
    sd = (dstar_sd_state*)calloc(1, sizeof(*sd));
    if (!sd) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA, sd, free) != 0) {
        free(sd);
        return NULL;
    }
    return sd;
}

void
dstar_sd_state_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    dstar_sd_state* sd = DSD_STATE_EXT_GET_AS(dstar_sd_state, state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA);
    if (sd) {
        DSD_MEMSET(sd, 0, sizeof(*sd));
    }
}
