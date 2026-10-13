// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA MLE routing and the downlink CMCE, SDS and SNDCP decoders (EN 300 392-2 clauses 14, 18, 28 and 29).
 *
 * Every TM-SDU starts with the 3-bit protocol discriminator of table 18.87. MLE protocol PDUs follow it with the 3-bit
 * PDU type of table 18.85; CMCE, MM and SNDCP PDUs start right after it (Annex E.1.1).
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MLE_H
#define DSD_NEO_PROTOCOL_TETRA_MLE_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Downlink MLE PDU types, table 18.85. */
#define TETRA_MLE_D_NEW_CELL              0
#define TETRA_MLE_D_PREPARE_FAIL          1
#define TETRA_MLE_D_NWRK_BROADCAST        2
#define TETRA_MLE_D_NWRK_BCAST_EXT        3
#define TETRA_MLE_D_RESTORE_ACK           4
#define TETRA_MLE_D_RESTORE_FAIL          5
#define TETRA_MLE_D_CHANNEL_RESPONSE      6
#define TETRA_MLE_EXTENDED_PDU            7

/* Downlink PDU type extensions of the extended MLE PDU, table 18.86. */
#define TETRA_MLE_D_NWRK_BROADCAST_DA     0
#define TETRA_MLE_D_NWRK_BCAST_REMOVE     1

/* Protocol discriminators, table 18.87. */
#define TETRA_MLE_PD_MM                   1
#define TETRA_MLE_PD_CMCE                 2
#define TETRA_MLE_PD_SNDCP                4
#define TETRA_MLE_PD_MLE                  5

/* Downlink CMCE PDU types, table 14.66. */
#define TETRA_CMCE_D_ALERT                0
#define TETRA_CMCE_D_CALL_PROCEEDING      1
#define TETRA_CMCE_D_CONNECT              2
#define TETRA_CMCE_D_CONNECT_ACK          3
#define TETRA_CMCE_D_DISCONNECT           4
#define TETRA_CMCE_D_INFO                 5
#define TETRA_CMCE_D_RELEASE              6
#define TETRA_CMCE_D_SETUP                7
#define TETRA_CMCE_D_STATUS               8
#define TETRA_CMCE_D_TX_CEASED            9
#define TETRA_CMCE_D_TX_CONTINUE          10
#define TETRA_CMCE_D_TX_GRANTED           11
#define TETRA_CMCE_D_TX_WAIT              12
#define TETRA_CMCE_D_TX_INTERRUPT         13
#define TETRA_CMCE_D_CALL_RESTORE         14
#define TETRA_CMCE_D_SDS_DATA             15
#define TETRA_CMCE_D_FACILITY             16
#define TETRA_CMCE_FUNCTION_NOT_SUPPORTED 31

/**
 * @brief Decode one downlink TM-SDU and publish what it carries to the TETRA state of @p state.
 *
 * Routes by protocol discriminator: MM PDUs go to tetra_mm_dispatch(), CMCE and SNDCP PDUs and the MLE protocol PDUs
 * (D-NWRK-BROADCAST and its extension, DA and remove forms, D-NEW-CELL, D-PREPARE-FAIL, D-RESTORE-ACK,
 * D-RESTORE-FAIL, D-CHANNEL RESPONSE) are decoded here. A PDU is published only when every element it declares
 * fitted in @p nbits; a truncated or malformed PDU leaves the state as it was. One summary line per decoded
 * signalling PDU goes to stderr; truncation notes need -Z and call-tracking narration -v.
 *
 * @param bits  TM-SDU, one bit per byte (0 or 1), MSB first, starting at the protocol discriminator.
 * @param nbits Number of valid bits in @p bits.
 * Call PDUs update the call records of the TETRA state by call identifier. Decoded this way, without the MAC PDU that
 * carried it, a TM-SDU names no addressed party and brings no channel allocation; the MAC passes both.
 *
 * @param cc    Colour code, for the log lines.
 * @param opts  Options; handed on to the MM decoder, the call records and the trunking state machine. May be NULL.
 * @param state Decoder state; nothing is decoded when it is NULL.
 */
void tetra_mle_dispatch(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_MLE_H */
