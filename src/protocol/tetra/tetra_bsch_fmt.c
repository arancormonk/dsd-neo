// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA BSCH network-identity display formatter.  Phase 31.
 */

#include <dsd-neo/protocol/tetra/tetra_bsch_fmt.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <string.h>

void
tetra_bsch_fmt_net(const dsd_state *state, char *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (!state || !state->tetra_net_known) {
        snprintf(buf, len, "MCC:? MNC:? CC:?");
        return;
    }

    snprintf(buf, len, "MCC:%u MNC:%u CC:%u",
             (unsigned)state->tetra_mcc,
             (unsigned)state->tetra_mnc,
             (unsigned)state->tetra_colour);
}
