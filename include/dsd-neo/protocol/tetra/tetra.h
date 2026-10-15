// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_PROTOCOL_TETRA_H
#define DSD_NEO_PROTOCOL_TETRA_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode the normal burst (processTetraFrame()) or synchronisation burst (processTetraSBFrame()) the frame sync found.
 * Each returns 1 when the burst decoded a block whose CRC passed: a BSCH, SCH/HD, SCH/F, STCH or BNCH block, direct-mode
 * or uplink signalling, or a speech frame. A training sequence alone proves nothing. */
int processTetraFrame(dsd_opts* opts, dsd_state* state);
int processTetraSBFrame(dsd_opts* opts, dsd_state* state);

/* Run the BSCH-set TDMA clock to the slot of the normal downlink burst being decoded; call it straight after the
 * burst's training sequence. */
void tetra_tdma_advance_ndb(dsd_state* state);

/* Copy what the frontends show of the TETRA decoder state into dsd_state::tetra_pub (core/tetra_publication.h).
 * Runs at the end of every burst and on every trunking state change; NULL is ignored. */
void tetra_publish(dsd_state* state);

/* Forget the CRC evidence gathered on the carrier on air (src/protocol/tetra/tetra_confirm.c), declared here so the
 * engine can clear it with the carrier's other evidence. */
void tetra_confirm_reset(dsd_state* state);

/* Forget the carrier the receiver was on, keeping the TETRA state allocated: its CRC evidence, the partial TM-SDUs, the
 * TDMA clock, the speech in progress (tetra_voice_reset()), its timeslots' speech context, and direct mode with the
 * call on its channel, its DM colour code and protection (tetra_dmo_forget_carrier()): the next carrier is trunked until
 * its own DSB says otherwise. The engine calls it at every carrier boundary and no-carrier pass, and the trunking state
 * machine when a retune completes. A session without TETRA state is left without one; NULL is ignored. */
void tetra_forget_carrier(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_H
