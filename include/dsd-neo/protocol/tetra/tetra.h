// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_H
#define DSD_NEO_PROTOCOL_TETRA_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void processTetraFrame(dsd_opts* opts, dsd_state* state);
void processTetraSBFrame(dsd_opts* opts, dsd_state* state);

/* Run the BSCH-set TDMA clock to the slot of the normal downlink burst being decoded; call it straight after the
 * burst's training sequence. */
void tetra_tdma_advance_ndb(dsd_state* state);

/* Copy what the frontends show of the TETRA decoder state into dsd_state::tetra_pub (core/tetra_publication.h).
 * Runs at the end of every burst; NULL is ignored. */
void tetra_publish(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_H
