// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA Mobility Management (MM) downlink PDU decoder.
 *
 * MM PDUs follow protocol discriminator 001 (EN 300 392-2 table 18.87). The decoder covers the downlink PDUs of
 * EN 300 392-2 clause 16 (location update accept, command, reject and proceeding, attach/detach group identity and
 * its acknowledgement, D-MM STATUS, MM PDU/FUNCTION NOT SUPPORTED) and the MM-carried security PDUs of
 * EN 300 392-7 annex A (D-AUTHENTICATION, D-OTAR, D-CK CHANGE DEMAND, D-DISABLE and D-ENABLE). A PDU updates the
 * decoder state only when all of it, optional elements included, was received.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_MM_H
#define DSD_NEO_PROTOCOL_TETRA_MM_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Downlink MM PDU types (EN 300 392-2 table 16.75, 4 bits). */
#define TETRA_MM_D_OTAR                         0
#define TETRA_MM_D_AUTHENTICATION               1
#define TETRA_MM_D_CK_CHANGE_DEMAND             2
#define TETRA_MM_D_DISABLE                      3
#define TETRA_MM_D_ENABLE                       4
#define TETRA_MM_D_LOCATION_UPDATING_ACCEPT     5
#define TETRA_MM_D_LOCATION_UPDATING_COMMAND    6
#define TETRA_MM_D_LOCATION_UPDATING_REJECT     7
#define TETRA_MM_RESERVED_8                     8
#define TETRA_MM_D_LOCATION_UPDATING_PROCEEDING 9
#define TETRA_MM_D_ATTACH_DETACH_GROUP          10
#define TETRA_MM_D_ATTACH_DETACH_GROUP_ACK      11
#define TETRA_MM_D_MM_STATUS                    12
#define TETRA_MM_RESERVED_13                    13
#define TETRA_MM_RESERVED_14                    14
#define TETRA_MM_FUNCTION_NOT_SUPPORTED         15

/**
 * @brief Downlink carrier frequency in Hz of a TETRA carrier number.
 *
 * EN 300 392-2 clause 21.4.4.1: base frequency + carrier x 25 kHz + offset, where the 2-bit offset code selects
 * 0, +6.25, -6.25 or +12.5 kHz. ETSI TS 100 392-15 table 1 defines the base frequency of frequency band values 1 to 9
 * as the value x 100 MHz.
 *
 * @param main_carrier 12-bit carrier number (SYSINFO main carrier or channel allocation carrier number).
 * @param freq_band    4-bit frequency band.
 * @param freq_offset  2-bit offset code.
 * @return The frequency, or 0 when the carrier has none: frequency band 0 (a cell without a main carrier, table 21.65
 *         note 3), a frequency band without a base frequency (10 to 15), or a field outside its width.
 */
long tetra_carrier_to_dl_hz(uint32_t main_carrier, uint32_t freq_band, uint32_t freq_offset);

/**
 * @brief Decode one downlink MM PDU and update the TETRA decoder state.
 *
 * @param bits  One byte per bit (0 or 1), MSB first, the 4-bit PDU type at bit 0.
 * @param nbits Number of bits in the PDU.
 * @param cc    Colour code, for the log prefix.
 * @param opts  Decoder options, for the event history notices of D-ENABLE and D-DISABLE; may be NULL. The diagnostics
 *              gates come from the TETRA state.
 * @param state Decoder state; nothing happens when it is NULL.
 */
void tetra_mm_dispatch(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state);

/** ETSI label of a 6-bit status downlink value (EN 300 392-2 table 16.92). */
static inline const char*
tetra_mm_status_name(uint8_t code) {
    if (code >= 16u && code <= 31u) {
        return "DMO gateway"; /* EN 300 396-5 annex B. */
    }
    if (code >= 32u && code <= 47u) {
        return "network specific";
    }
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
