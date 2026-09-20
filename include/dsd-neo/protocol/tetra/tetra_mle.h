// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MLE (Mobile Link Entity) dispatch + CMCE call-control parser.
 * ETSI EN 300 392-2 §21.6 (D-MLE) and Chapter 14 (CMCE).
 *
 * Every TM-SDU starts with the 3-bit protocol discriminator from table
 * 18.87. When it selects the MLE protocol, a 3-bit MLE PDU type follows.
 * For CMCE, MM and SNDCP, the selected protocol PDU follows immediately.
 *
 * Individual CMCE parsers document their ETSI table layouts in the source.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MLE_H
#define DSD_NEO_PROTOCOL_TETRA_MLE_H

#include <stdint.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Downlink MLE PDU type constants (3-bit, table 18.85).
 * ----------------------------------------------------------------------- */
#define TETRA_MLE_D_NEW_CELL          0
#define TETRA_MLE_D_PREPARE_FAIL      1
#define TETRA_MLE_D_NWRK_BROADCAST    2
#define TETRA_MLE_D_NWRK_BCAST_EXT    3
#define TETRA_MLE_D_RESTORE_ACK        4
#define TETRA_MLE_D_RESTORE_FAIL       5
#define TETRA_MLE_D_CHANNEL_RESPONSE   6
#define TETRA_MLE_EXTENDED_PDU         7

/* Downlink extended-PDU type extension (4-bit, table 18.86). */
#define TETRA_MLE_D_NWRK_BROADCAST_DA  0
#define TETRA_MLE_D_NWRK_BCAST_REMOVE  1

/* -----------------------------------------------------------------------
 * Protocol discriminator (3-bit, table 18.87).
 * ----------------------------------------------------------------------- */
#define TETRA_MLE_PD_MM               1
#define TETRA_MLE_PD_CMCE             2
#define TETRA_MLE_PD_SNDCP            4
#define TETRA_MLE_PD_MLE              5

/* -----------------------------------------------------------------------
 * CMCE downlink PDU type constants (5-bit, ETSI EN 300 392-2 Table 14.66)
 * ----------------------------------------------------------------------- */
#define TETRA_CMCE_D_ALERT            0
#define TETRA_CMCE_D_CALL_PROCEEDING  1
#define TETRA_CMCE_D_CONNECT          2   /* Call connected → call_active = 1 */
#define TETRA_CMCE_D_CONNECT_ACK      3
#define TETRA_CMCE_D_DISCONNECT       4   /* Call ending   → call_active = 0 */
#define TETRA_CMCE_D_INFO             5
#define TETRA_CMCE_D_RELEASE          6   /* Call released → call_active = 0 */
#define TETRA_CMCE_D_SETUP            7   /* Incoming call setup              */
#define TETRA_CMCE_D_STATUS           8
#define TETRA_CMCE_D_TX_CEASED        9   /* PTT released  → tx_granted_valid=0 */
#define TETRA_CMCE_D_TX_CONTINUE     10
#define TETRA_CMCE_D_TX_GRANTED      11   /* PTT grant     → tx_granted_ssi     */
#define TETRA_CMCE_D_TX_WAIT         12
#define TETRA_CMCE_D_TX_INTERRUPT    13
#define TETRA_CMCE_D_CALL_RESTORE    14
#define TETRA_CMCE_D_SDS_DATA        15
#define TETRA_CMCE_D_FACILITY        16
#define TETRA_CMCE_FUNCTION_NOT_SUPPORTED 31

/* D-SETUP call_type field values (3-bit, ETSI §14.7.3.2 Table 14.33) */
#define TETRA_CMCE_CALL_TYPE_GROUP        0   /* Basic group call        */
#define TETRA_CMCE_CALL_TYPE_UNAACK_GRP   1   /* Unacknowledged group    */
#define TETRA_CMCE_CALL_TYPE_ACKNOWLEDGED 2   /* Acknowledged call       */
#define TETRA_CMCE_CALL_TYPE_SDS          3   /* Short data service      */
#define TETRA_CMCE_CALL_TYPE_PSTN         4   /* PSTN/PABX private call  */
#define TETRA_CMCE_CALL_TYPE_ISDN         5   /* ISDN private call        */
#define TETRA_CMCE_CALL_TYPE_SDM          6   /* Semi-duplex migration   */

/* -----------------------------------------------------------------------
 * tetra_mle_dispatch()
 *
 * Parse the MLE TM-SDU that follows the address field in a MAC-RESOURCE
 * PDU and dispatch to CMCE / MM decoders as appropriate.
 *
 * Updates @state:
 *   MLE D-NWRK-BROADCAST → tetra_nwrk_bcast_known and cell-reselection state
 *   MLE D-NWRK-BROADCAST-DA / D-NWRK-BCAST-EXT / D-NWRK-BROADCAST REMOVE /
 *       D-RESTORE-ACK / D-RESTORE-FAIL
 *                        → broadcast, removal, restored-call, or failure state
 *   CMCE D-ALERT / D-CALL-PROCEEDING
 *                    → tetra_call_active=1
 *   CMCE D-SETUP     → tetra_calling_ssi, tetra_gssi, tetra_call_id,
 *                       tetra_call_type, tetra_call_active=1 and canonical
 *                       call-state attribution
 *   CMCE D-CONNECT   → tetra_call_active=1
 *   CMCE D-STATUS    → tetra_sds_status, tetra_sds_src
 *   CMCE D-RELEASE /
 *   CMCE D-DISCONNECT → tetra_call_active=0
 *   CMCE D-TX-GRANTED → tetra_tx_granted_ssi, tetra_tx_granted_valid=1
 *   CMCE D-TX-CEASED  → tetra_tx_granted_valid=0
 *   CMCE D-TX-CONTINUE / D-TX-INTERRUPT / D-TX-WAIT
 *                    → floor-control event state
 *   CMCE D-INFO      → call identifier, reset and poll state
 *   CMCE D-FACILITY  → first supplementary-service type
 *
 * @bits   – one byte per bit (value 0 or 1), MSB-first, TM-SDU payload
 * @nbits  – number of valid bits starting at @bits[0]
 * @cc     – Colour Code (for diagnostic log prefix)
 * @opts   – dsd_opts (errorbars / payload verbosity flags)
 * @state  – dsd_state to update; may be NULL
 * ----------------------------------------------------------------------- */
void tetra_mle_dispatch(const uint8_t *bits, int nbits,
                        int cc, const dsd_opts *opts, dsd_state *state);

/** Clear bounded in-flight concatenated SDS reassembly contexts. */
void tetra_sds_concat_reset(void);

/* -----------------------------------------------------------------------
 * tetra_enc_mode_name()  (Phase 33)
 *
 * Return a human-readable string for the 2-bit encryption mode field.
 * ----------------------------------------------------------------------- */
static inline const char *
tetra_enc_mode_name(uint8_t mode)
{
    switch (mode & 0x03u) {
    case 0: return "none";
    case 1: return "on";
    case 2: return "on+auth";
    default: return "rsvd";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_MLE_H */
