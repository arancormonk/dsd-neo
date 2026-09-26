// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_H
#define DSD_NEO_PROTOCOL_TETRA_H

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

void processTetraFrame(dsd_opts* opts, dsd_state* state);
void processTetraSBFrame(dsd_opts* opts, dsd_state* state);

/* Advance a BSCH-synchronised TDMA timestamp to the next normal burst. */
static inline void tetra_tdma_advance_ndb(dsd_state* state)
{
    if (!state || !state->tetra_tdma_valid ||
        state->tetra_tn < 1 || state->tetra_tn > 4)
        return;

    if (state->tetra_tn < 4) {
        state->tetra_tn++;
        return;
    }

    state->tetra_tn = 1;
    state->tetra_fn = (uint8_t)((state->tetra_fn + 1u) % 18u);
    if (state->tetra_fn == 0)
        state->tetra_mn = (uint8_t)((state->tetra_mn + 1u) % 60u);
}

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_H
