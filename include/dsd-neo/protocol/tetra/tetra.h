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
void tetra_tdma_advance_ndb(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_H
