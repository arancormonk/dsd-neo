// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MM (Mobility Management) PDU dispatcher.
 * ETSI EN 300 392-2, Chapter 16.
 *
 * MM PDUs follow Protocol Discriminator 001 directly (table 18.87).
 *
 * ETSI EN 300 392-2 Table 16.75 defines a 4-bit PDU type field. This module
 * decodes the mandatory fields used by a passive monitor and publishes state
 * only after the complete mandatory portion has arrived.
 *
 * The passive-monitor state covers enable/disable, location-update accept,
 * command, reject and proceeding, group identity/acknowledgement, D-MM-STATUS,
 * MM PDU/FUNCTION NOT SUPPORTED, and the EN 300 392-7 D-OTAR CCK/SCK/GCK/GSKO
 * provision and rejection messages, KEY ASSOCIATE/KEY DELETE/KEY STATUS and
 * DM-SCK ACTIVATE demands, OTAR NEWCELL, CMG GTSI provision, D-AUTHENTICATION, D-CK CHANGE,
 * D-DISABLE, and D-ENABLE.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MM_H
#define DSD_NEO_PROTOCOL_TETRA_MM_H

#include <stdint.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * MM PDU type constants — downlink, ETSI EN 300 392-2 Table 16.75 (4 bits).
 * ----------------------------------------------------------------------- */
#define TETRA_MM_D_OTAR                        0
#define TETRA_MM_D_AUTHENTICATION              1
#define TETRA_MM_D_CK_CHANGE_DEMAND            2
#define TETRA_MM_D_DISABLE                     3
#define TETRA_MM_D_ENABLE                      4
#define TETRA_MM_D_LOCATION_UPDATING_ACCEPT    5
#define TETRA_MM_D_LOCATION_UPDATING_COMMAND   6
#define TETRA_MM_D_LOCATION_UPDATING_REJECT    7
#define TETRA_MM_RESERVED_8                     8
#define TETRA_MM_D_LOCATION_UPDATING_PROCEEDING 9
#define TETRA_MM_D_ATTACH_DETACH_GROUP         10
#define TETRA_MM_D_ATTACH_DETACH_GROUP_ACK     11
#define TETRA_MM_D_MM_STATUS                   12
#define TETRA_MM_RESERVED_13                   13
#define TETRA_MM_RESERVED_14                   14
#define TETRA_MM_FUNCTION_NOT_SUPPORTED        15

/* -----------------------------------------------------------------------
 * Carrier frequency computation helper.
 *
 * tetra_carrier_to_dl_hz()
 *
 * Convert the SYSINFO `main_carrier` (12-bit, 0–4095) + `freq_band` (4-bit)
 * + `freq_offset` (2-bit) into an absolute DL frequency in Hz.
 *
 * TETRA DL frequency formula (ETSI EN 300 392-2 Annex A):
 *   DL (Hz) = freq_band × 100000000 + main_carrier × 25000
 *             + decoded frequency offset
 *
 * Returns 0 when either encoded field is outside its specified width.
 * ----------------------------------------------------------------------- */
long tetra_carrier_to_dl_hz(uint32_t main_carrier,
                             uint32_t freq_band,
                             uint32_t freq_offset);

/* -----------------------------------------------------------------------
 * tetra_mm_dispatch()
 *
 * Parse a TETRA MM PDU (bit array, one byte per bit, MSB-first) and update
 * dsd_state with any extracted mobility-management information.
 *
 * @bits   – one byte per bit, value 0 or 1, MSB-first, MM PDU at bit 0
 * @nbits  – number of valid bits
 * @cc     – Colour Code (for log prefix)
 * @opts   – dsd_opts verbosity flags
 * @state  – dsd_state to update; may be NULL
 * ----------------------------------------------------------------------- */
void tetra_mm_dispatch(const uint8_t *bits, int nbits,
                       int cc, const dsd_opts *opts, dsd_state *state);

/* -----------------------------------------------------------------------
 * tetra_mm_status_name()  (Phase 34)
 *
 * Return the ETSI label for the 6-bit Status Downlink value (Table 16.92).
 * ----------------------------------------------------------------------- */
static inline const char *
tetra_mm_status_name(uint8_t code)
{
    switch (code) {
    case 1: return "change energy saving request";
    case 2: return "change energy saving response";
    case 3: return "dual watch response";
    case 4: return "terminate dual watch response";
    case 5: return "change dual watch request";
    case 7: return "MS frequency bands request";
    case 8: return "periodic distance reporting";
    default: return "reserved";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_MM_H */
