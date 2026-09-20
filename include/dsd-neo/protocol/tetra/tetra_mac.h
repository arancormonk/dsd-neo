// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MAC PDU parser – SCH-HD (Half-Capacity Downlink Signaling Channel)
 * ETSI EN 300 392-2 §21.4 / §21.5
 *
 * MAC PDU type field (2 bits, MSB-first at bit 0 of the decoded block):
 *   00 (0) – MAC-RESOURCE   : resource grant / channel allocation
 *   01 (1) – MAC-FRAG/END   : fragmented PDU continuation or end
 *   10 (2) – MAC-BROADCAST  : system information (SYSINFO / ACCESS-DEFINE)
 *   11 (3) – MAC-SUPPL      : supplementary (D-BLCK, etc.)
 *
 * MAC-BROADCAST sub-types (2-bit broadcast_type after MAC PDU type):
 *   00 (0) – SYSINFO         : RF + network identity parameters
 *   01 (1) – ACCESS-DEFINE   : random access parameters
 *
 * All fields are big-endian (MSB-first) in the decoded bit array.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MAC_H
#define DSD_NEO_PROTOCOL_TETRA_MAC_H

#include <stdint.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * MAC PDU type constants (2-bit field, ETSI EN 300 392-2 §21.4.1)
 * ----------------------------------------------------------------------- */
#define TETRA_MAC_TYPE_RESOURCE   0   /* MAC-RESOURCE */
#define TETRA_MAC_TYPE_FRAG_END   1   /* MAC-FRAG / MAC-END variants */
#define TETRA_MAC_TYPE_BROADCAST  2   /* MAC-BROADCAST */
#define TETRA_MAC_TYPE_SUPPL      3   /* MAC-SUPPLEMENTARY */

/* MAC-BROADCAST sub-types (2-bit broadcast_type, ETSI §21.5.1) */
#define TETRA_MAC_BC_SYSINFO      0   /* SYSINFO (RF + MLE network params)          */
#define TETRA_MAC_BC_ACCESS_DEF   1   /* ACCESS-DEFINE                               */
#define TETRA_MAC_BC_RESTORE      2   /* D-RESTORE-ACK / D-RESTORE-FAIL (MLE)       */
#define TETRA_MAC_BC_NWRK_BCAST   3   /* D-NWRK-BROADCAST / D-NWRK-BCAST-EXT (MLE) */

/* FRAG/END sub-types (1-bit, ETSI §21.4.4.2) */
#define TETRA_MAC_FRAGE_FRAG      0   /* MAC-FRAG – fragment continuation */
#define TETRA_MAC_FRAGE_END       1   /* MAC-END  – last fragment */

/* Address types within MAC-RESOURCE (3-bit addr_type, ETSI Table 21.49) */
#define TETRA_MAC_ADDR_NULL        0
#define TETRA_MAC_ADDR_SSI         1   /* Short Subscriber Identity (24 bits) */
#define TETRA_MAC_ADDR_EVENT_LABEL 2   /* Event Label (10 bits) */
#define TETRA_MAC_ADDR_USSI        3   /* Unacknowledged SSI (24 bits) */
#define TETRA_MAC_ADDR_SMI         4   /* Short Management Identity (24 bits) */
#define TETRA_MAC_ADDR_SSI_EVENT   5   /* SSI + Event Label */
#define TETRA_MAC_ADDR_SSI_USAGE   6   /* SSI + Usage Marker */
#define TETRA_MAC_ADDR_SMI_EVENT   7   /* SMI + Event Label */

/* Encryption mode values (2-bit, within MAC-RESOURCE) */
#define TETRA_ENC_MODE_NONE        0   /* No encryption */
#define TETRA_ENC_MODE_ON          1   /* Encrypted */
#define TETRA_ENC_MODE_ON_AUTH     2   /* Encrypted + authenticated */
#define TETRA_ENC_MODE_RSVD        3   /* Reserved */

/* -----------------------------------------------------------------------
 * tetra_mac_parse_schd()
 *
 * Parse a decoded SCH-HD bit array as a TETRA MAC PDU and print a
 * human-readable summary to stderr.  Recognized PDU types also update
 * the TETRA-specific fields in @state:
 *
 *   MAC-BROADCAST/SYSINFO      → tetra_sysinfo_known, tetra_la,
 *                                 tetra_bs_service_det, tetra_subscr_class
 *   MAC-BROADCAST/NWRK-BCAST   → tetra_nwrk_bcast_known and MLE
 *                                 cell-reselection/load state
 *   MAC-BROADCAST/RESTORE       → log only (D-RESTORE-ACK/RESPONSE)
 *   MAC-RESOURCE               → tetra_ssi_valid, tetra_active_ssi, tetra_enc_mode
 *
 * @bits   – decoded bit array (one byte per bit, value 0 or 1), MSB-first
 * @nbits  – number of valid bits in the array
 * @cc     – Colour Code extracted from the NDB CB field
 * @opts   – dsd_opts (used for opts->payload / opts->errorbars verbosity)
 * @state  – dsd_state to update; may be NULL (state update is then skipped)
 * ----------------------------------------------------------------------- */
void tetra_mac_parse_schd(const uint8_t *bits, int nbits,
                          int cc, const dsd_opts *opts, dsd_state *state);

/* Calculate the 32-bit LLC FCS over a TL-SDU bit array according to
 * EN 300 392-2 Annex C. Bits are supplied and returned MSB-first. */
uint32_t tetra_llc_fcs32(const uint8_t *bits, int nbits);

/* Return a short string for a MAC PDU type code (for logging). */
const char *tetra_mac_type_name(int pdu_type);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_MAC_H */
