// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA BSCH network-identity display formatter.  Phase 31.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_bsch_fmt.h>

#include <stdio.h>
#include <string.h>
#include "tetra_state.h"

void
tetra_bsch_fmt_net(const dsd_state* state, char* buf, size_t len) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!buf || len == 0) {
        return;
    }

    if (!ts || !ts->net_known) {
        snprintf(buf, len, "MCC:? MNC:? CC:?");
        return;
    }

    snprintf(buf, len, "MCC:%u MNC:%u CC:%u", (unsigned)ts->mcc, (unsigned)ts->mnc, (unsigned)ts->colour);
}
