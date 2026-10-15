// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA downlink MAC PDUs as decoded (EN 300 392-2 clauses 21.4.3.1, 21.4.4.1 and 21.4.4.3).
 *
 * tetra_mac_resource_decode() reads the header of a MAC-RESOURCE, and tetra_mac_broadcast_decode() a SYSINFO or
 * ACCESS-DEFINE, from the start of a pi/4-DQPSK MAC PDU without acting on it. tetra_mac_parse_schd() decodes through
 * them and acts only on what decoded completely. Fields a PDU does not carry read zero. Module-private.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MAC_PDU_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MAC_PDU_H_

#include <stdint.h>

#include "tetra_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** MAC-RESOURCE header (table 21.55) up to the channel allocation flag; pi/4-DQPSK has no immediate napping permission
 * flag and no multiple slot granting. */
typedef struct {
    tetra_mac_address addr; /**< address; type TETRA_MAC_ADDR_NULL for a Null PDU */
    int pdu_bits;           /**< PDU length in the block (clause 23.4.3.3); -1 for a reserved length indication */
    int body;               /**< first bit after the channel allocation flag */
    uint8_t fill;           /**< fill bit indication */
    uint8_t grant_pos;      /**< position of grant */
    uint8_t enc_mode;       /**< encryption mode, TETRA_ENC_MODE_* */
    uint8_t random_access;  /**< random access flag */
    uint8_t length_ind;     /**< length indication */
    uint8_t alloc_present;  /**< channel allocation flag */
} tetra_mac_resource;

/**
 * @brief Decode the header of the MAC-RESOURCE at the start of @p bits.
 *
 * @param bits  The MAC PDU and what follows it in its block, one bit per byte, MSB first, the MAC PDU type at bit 0.
 * @param nbits Bits left in the block.
 * @param out   Receives the header; zeroed first.
 * @return 1 for an addressed MAC-RESOURCE whose header, optional elements included, fits in its indicated length; 0
 *         for a Null PDU, a reserved length indication, a header longer than its PDU, or another MAC PDU type.
 */
int tetra_mac_resource_decode(const uint8_t* bits, int nbits, tetra_mac_resource* out);

/** SYSINFO (table 21.65) for pi/4-DQPSK, and the D-MLE-SYSINFO TM-SDU it carries (clause 18.4.2.2). */
typedef struct {
    uint32_t option;       /**< 20-bit optional field */
    uint16_t main_carrier; /**< main carrier number */
    uint16_t
        hyperframe_or_cck;     /**< hyperframe number; with cck_flag, the CCK identifier or static cipher key version */
    uint16_t la;               /**< location area, when mle_present */
    uint16_t subscriber_class; /**< subscriber class, when mle_present */
    uint16_t bs_service;       /**< BS service details (12 bits), when mle_present */
    uint8_t band;              /**< frequency band */
    uint8_t offset;            /**< offset code */
    uint8_t duplex_spacing;
    uint8_t reverse;          /**< reverse operation */
    uint8_t num_scch;         /**< number of common secondary control channels on the main carrier */
    uint8_t ms_txpwr_max;     /**< MS_TXPWR_MAX_CELL */
    uint8_t rxlev_access_min; /**< RXLEV_ACCESS_MIN */
    uint8_t access_parameter; /**< ACCESS_PARAMETER */
    uint8_t radio_dl_timeout; /**< RADIO_DOWNLINK_TIMEOUT */
    uint8_t cck_flag;         /**< hyperframe_or_cck holds a CCK identifier or static cipher key version */
    uint8_t option_flag; /**< optional field flag: even or odd multiframe, default access code, extended services */
    uint8_t mle_present; /**< the block carried the 42-bit D-MLE-SYSINFO */
} tetra_mac_sysinfo;

/** ACCESS-DEFINE (table 21.79). */
typedef struct {
    uint32_t gssi;               /**< with optional field flag 2 */
    uint16_t subscriber_class;   /**< with optional field flag 1 */
    uint8_t assigned;            /**< common or assigned control channel flag: 1 assigned */
    uint8_t access_code;         /**< 0..3 for access codes A..D */
    uint8_t imm;                 /**< IMM */
    uint8_t wait_time;           /**< WT */
    uint8_t num_ra;              /**< Nu, number of random access transmissions */
    uint8_t frame_length_factor; /**< frame-length factor */
    uint8_t timeslot_pointer;    /**< timeslot pointer */
    uint8_t min_priority;        /**< minimum PDU priority */
    uint8_t option_flag;         /**< optional field flag: 0 none, 1 subscriber class, 2 GSSI */
    uint8_t nbits;               /**< PDU length: 32, 48 or 56 bits */
} tetra_mac_access_define;

/** One decoded broadcast PDU; @c type (table 21.64, TETRA_MAC_BC_*) selects the member. */
typedef struct {
    union {
        tetra_mac_sysinfo sysinfo;             /**< TETRA_MAC_BC_SYSINFO */
        tetra_mac_access_define access_define; /**< TETRA_MAC_BC_ACCESS_DEF */
    };

    uint8_t type; /**< broadcast type */
} tetra_mac_broadcast;

/**
 * @brief Decode the SYSINFO or ACCESS-DEFINE at the start of @p bits.
 *
 * @param bits  The MAC PDU and what follows it in its block, one bit per byte, MSB first, the MAC PDU type at bit 0.
 * @param nbits Bits left in the block.
 * @param out   Receives the PDU; zeroed first. @c out->type is the broadcast type once its bits were there.
 * @return 1 for a SYSINFO whose MAC fields fit (the D-MLE-SYSINFO is optional, @c mle_present) or an ACCESS-DEFINE that
 *         fits; 0 otherwise, SYSINFO-DA and the reserved broadcast type included.
 */
int tetra_mac_broadcast_decode(const uint8_t* bits, int nbits, tetra_mac_broadcast* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MAC_PDU_H_ */
