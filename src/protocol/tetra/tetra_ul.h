// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA trunked mode uplink C-plane reader: the algorithm a radio's clear registration proposes.
 *
 * The uplink is the radios' side. Its MAC, LLC and MM PDUs are not the downlink's (EN 300 392-2 clauses 21.4.2 and
 * 16.9.3: uplink MM PDU type 2 is U-LOCATION UPDATE DEMAND where the downlink's is D-CK CHANGE DEMAND), so the downlink
 * dispatchers never see an uplink block. This reader is entered only from the control uplink burst's SCH/HU and the
 * normal uplink burst's SCH/F, after their CRC passed, and reads only as far as it needs to:
 *
 * - the first MAC PDU of the block, a MAC-ACCESS (table 21.41) or a MAC-DATA (table 21.46), whose encrypted flag is
 *   the uplink's air encryption state, Null PDUs included (clause 21.4.2.1 note 1); its end is the lesser of its
 *   length indication and the block (clause 23.4.3.3), and its fill bits come off (clause 23.4.3.2);
 * - of a clear TM-SDU that is whole or a fragment start, the basic link LLC header (table 21.1; a whole FCS variant's
 *   FCS is checked, a fragment start's cannot be), the MLE protocol discriminator, and an MM PDU's type;
 * - of a U-LOCATION UPDATE DEMAND (table 16.18), its leading type 1 elements: location update type, request to append
 *   LA, cipher control and, when that is on, the ciphering parameters (EN 300 392-7 table A.46), whose KSG number is
 *   what the MS proposes to the SwMI (EN 300 392-7 clause 6.6.2.1).
 *
 * Nothing else is read: no associated PDU after the first, no later fragment (MAC-END-HU, MAC-FRAG, MAC-END), no
 * MAC-U-BLCK, no stealing channel half slot, no encrypted TM-SDU and no other MM, CMCE or SDS PDU.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UL_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UL_H_

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The uplink logical channel of a CRC-passed block. */
typedef enum {
    TETRA_UL_SCH_HU, /**< a control uplink burst's subslot, 92 type 1 bits */
    TETRA_UL_SCH_F,  /**< a normal uplink burst's full slot, 268 type 1 bits */
} tetra_ul_channel;

/** The uplink's air encryption (tetra_state_t::ul_enc). */
enum {
    TETRA_UL_ENC_UNKNOWN = 0,
    TETRA_UL_ENC_CLEAR = 1,
    TETRA_UL_ENC_ENCRYPTED = 2,
};

/** The first MAC PDU of an uplink block. */
typedef enum {
    TETRA_UL_PDU_UNREAD = 0,    /**< reserved, malformed or too short a header */
    TETRA_UL_PDU_ACCESS,        /**< MAC-ACCESS (SCH/HU) */
    TETRA_UL_PDU_DATA,          /**< MAC-DATA (SCH/F) */
    TETRA_UL_PDU_END_HU,        /**< MAC-END-HU, a last fragment (SCH/HU) */
    TETRA_UL_PDU_FRAG_END,      /**< MAC-FRAG or MAC-END, a later fragment (SCH/F) */
    TETRA_UL_PDU_SUPPLEMENTARY, /**< MAC-U-BLCK or a reserved supplementary PDU (SCH/F) */
} tetra_ul_pdu_kind;

/** What tetra_ul_read() read of a block. Fields it did not reach read zero. */
typedef struct {
    uint32_t addr;          /**< the MAC address, of @c addr_type */
    uint16_t cipher_params; /**< the ciphering parameters (EN 300 392-7 table A.46), when @c cipher_control */
    uint8_t kind;           /**< tetra_ul_pdu_kind */
    uint8_t header_ok;      /**< a MAC-ACCESS or MAC-DATA header read whole: @c encrypted is the PDU's */
    uint8_t encrypted;      /**< its encrypted flag */
    uint8_t addr_type;      /**< 0 SSI, 1 event label, 2 USSI, 3 SMI (table 21.41) */
    uint8_t null_pdu;       /**< a Null PDU: nothing follows its header */
    uint8_t fragment;       /**< the TM-SDU is the start of a fragmented one */
    uint8_t mm;             /**< the TM-SDU reached an MM PDU: @c mm_type is its uplink PDU type (table 16.75) */
    uint8_t mm_type;
    uint8_t lu_demand;      /**< a U-LOCATION UPDATE DEMAND whose leading type 1 elements were read */
    uint8_t lu_type;        /**< its location update type */
    uint8_t cipher_control; /**< its cipher control: the ciphering parameters follow */
} tetra_ul_reading;

/**
 * @brief Read the first MAC PDU of a CRC-passed uplink block of channel @p ch, @p nbits type 1 bits (one per byte).
 * @return 1 when a MAC-ACCESS or MAC-DATA header was read whole (@c header_ok), 0 otherwise.
 */
int tetra_ul_read(tetra_ul_channel ch, const uint8_t* bits, int nbits, tetra_ul_reading* out);

/**
 * @brief Act on a CRC-passed uplink block of the carrier with colour code @p cc: the uplink's air encryption state,
 * one summary line, and the KSG a clear U-LOCATION UPDATE DEMAND proposes noted for the serving network
 * (tetra_air_ksg_request()), never the network of its address extension, which is the MS's own MNI.
 */
void tetra_ul_block(dsd_state* state, tetra_ul_channel ch, const uint8_t* bits, int nbits, int cc);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_UL_H_ */
