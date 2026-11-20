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

/* What the last TETRA burst proved about the channel (issue #890), strongest first among what its blocks reached:
 *
 * - DMO_SPEECH: direct mode speech whose CRC passed and that re-encodes like speech (EN 300 395-2 clauses 5.5, 5.6).
 * - DMO_SERVICE: a direct mode PDU of a call or of short data whose CRC passed and that was read in the clear: DM-SETUP,
 *   DM-SETUP PRES, DM-CONNECT ACK, DM-OCCUPIED, DM-GSETUP, or DM-SDS DATA / UDATA whose FCS did not fail, in a DSB's
 *   SCH/H or a DNB's DMAC-DATA (EN 300 396-3 clause 9.4).
 * - DMO_OPAQUE: a DMAC-SYNC whose SCH/H passed its CRC under DM-2-C, which encrypts its message type (EN 300 396-3
 *   clause 8.5.3): an encrypted direct mode transmission, or a reservation, that no one can tell apart.
 * - TMO: a trunked mode block whose CRC passed: a BSCH (the home cell's or another's), a downlink MAC block, an uplink
 *   SCH/HU or MAC block, or trunked speech.
 * - PROFILE: a CRC passed, but on nothing of the above: a DSB of which only SCH/S decoded (its SCH/H failed, or only the
 *   soft decision decoded it and SCH/H went by unread), a presence signal, or a direct mode PDU that sets up no call
 *   (DM-RESERVED and the other reservation, end and request signalling).
 * - NONE: no CRC passed, or the burst was read across a carrier boundary or cut by an input interruption.
 *
 * A burst is one or the other mode, so the order only ranks the blocks of one burst. */
typedef enum {
    DSD_TETRA_BURST_PROOF_NONE = 0,
    DSD_TETRA_BURST_PROOF_PROFILE,
    DSD_TETRA_BURST_PROOF_TMO,
    DSD_TETRA_BURST_PROOF_DMO_OPAQUE,
    DSD_TETRA_BURST_PROOF_DMO_SERVICE,
    DSD_TETRA_BURST_PROOF_DMO_SPEECH,
} dsd_tetra_burst_proof;

/* What the last burst processTetraFrame() or processTetraSBFrame() closed proved (dsd_tetra_burst_proof), NONE after a
 * carrier reset or for a session without TETRA state. Decoder thread. */
dsd_tetra_burst_proof tetra_burst_proof(const dsd_state* state);

/* 1 for a proof of checked content on the channel, which a -Y row holds on (DSD_CONTENT_EVIDENCE_PROVEN): TMO and every
 * DMO proof; 0 for PROFILE and NONE. */
int tetra_burst_proof_is_content(dsd_tetra_burst_proof proof);

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
 * TDMA clock, the speech in progress (keeping the transmission it belonged to, so speech of the same transmission after
 * the gap counts as the same), its timeslots' speech context, and direct mode with the
 * call on its channel, its DM colour code and protection (tetra_dmo_forget_carrier()): the next carrier is trunked until
 * its own DSB says otherwise. The engine calls it at every carrier boundary and no-carrier pass, and the trunking state
 * machine when a retune completes. A session without TETRA state is left without one; NULL is ignored. */
void tetra_forget_carrier(dsd_state* state);

/* Start a scan's visit to a TETRA row cold (issue #890): the engine calls it at every landing of a scan session that
 * holds the TETRA state (dsd_state::tetra_ext_scan_held), a TETRA row after another TETRA row included, and when the
 * session lets the state go. In this order it clears what the last visit's bursts proved, the D-MM STATUS, the trunked
 * calls kept with their canonical calls, the TM-SDUs and short data being rebuilt, the network (its cell and SYSINFO,
 * then MCC, MNC, colour code and scrambling seed, net_known last: tetra_bsch_forget_network()), the carrier as
 * tetra_forget_carrier() does (DM colour code and direct mode included), the trunking state machine's staged and
 * in-flight work (tetra_sm_init()), the carrier's polarity with the frame sync's burst capture, and the frontends'
 * publication. It keeps the state allocated with the speech decoder's storage, the operator air keys (which live in
 * dsd_state) and the KSG evidence kept per network (tetra_state_t::air_ksg), which only that network's own identity
 * reaches again; a --tetra-net preset seeds the identity again at the next burst. Never creates TETRA state; NULL
 * @p state is ignored, and @p opts may be NULL. */
void tetra_scan_visit_reset(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif // DSD_NEO_PROTOCOL_TETRA_H
