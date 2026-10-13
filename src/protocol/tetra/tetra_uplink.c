// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/protocol/tetra/tetra.h>

#include <dsd-neo/protocol/tetra/tetra_fec.h>

#include <stdio.h>
#include <stdlib.h>

/* Own translation unit so a sync test can link this without the rest of the
 * NDB decoder. tetra_fec.o must follow this member in the archive. */
void
tetra_uplink_configure(dsd_state* state) {
    static int once = 0;
    const char* net;
    unsigned mcc = 0, mnc = 0;
    int colour = 0;
    if (once || !state) {
        return;
    }
    once = 1;

    net = getenv("TETRA_UPLINK");
    if (net && (net[0] == '1' || net[0] == 'y' || net[0] == 'Y')) {
        state->tetra_uplink = 1;
    }

    /* Uplink-only reception never sees a BSCH, so the colour code has to be
     * supplied. Decimal or 0x colour: TETRA_NET=86,28,0x19. A later BSCH
     * overwrites this. */
    net = getenv("TETRA_NET");
    if (!net || state->tetra_net_known || sscanf(net, "%u,%u,%i", &mcc, &mnc, &colour) != 3) {
        return;
    }
    state->tetra_mcc = (uint16_t)(mcc & 0x3FFu);
    state->tetra_mnc = (uint16_t)(mnc & 0x3FFFu);
    state->tetra_colour = (uint8_t)(colour & 0x3F);
    state->tetra_lfsr_seed = tetra_compute_scramb_seed(state->tetra_mcc, state->tetra_mnc, state->tetra_colour);
    state->tetra_net_known = 1;
}
