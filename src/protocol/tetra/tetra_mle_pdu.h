// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA MLE protocol PDUs, CMCE short data PDUs and SN-PDU headers as decoded (EN 300 392-2 clauses 14, 18,
 * 28 and 29).
 *
 * tetra_mle_decode() reads one downlink MLE protocol PDU (protocol discriminator 101, table 18.87) into a
 * tetra_mle_pdu, tetra_sds_decode() one D-STATUS or D-SDS-DATA (protocol discriminator 010) into a tetra_sds_pdu, and
 * tetra_sndcp_decode() the header of one SN-PDU (protocol discriminator 100). The MLE dispatcher decodes through them
 * and acts only on a PDU that decoded completely. Fields a PDU does not carry read zero. Module-private.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MLE_PDU_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MLE_PDU_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Network broadcast elements, clause 18.5. */

/** Neighbour cell information for CA (table 18.64). */
typedef struct {
    uint16_t main_carrier;  /**< main carrier number */
    uint8_t cell_id;        /**< cell identifier CA */
    uint8_t reselect_types; /**< cell reselection types supported */
} tetra_mle_ca_neighbor;

/** Channel class (table 18.41). */
typedef struct {
    uint32_t characteristics; /**< channel characteristics, table 18.40 */
    uint8_t id;               /**< channel class identifier, table 18.42 */
    uint8_t bs_power;         /**< BS transmit power relative to the main carrier */
} tetra_mle_channel_class;

/** Neighbour cell channel class for CA or DA (tables 18.62 and 18.63). */
typedef struct {
    tetra_mle_channel_class channel_class;
    uint8_t cell_id;
} tetra_mle_neighbor_channel_class;

/** Irregular channel detail (table 18.96). */
typedef struct {
    uint16_t carrier;           /**< carrier number */
    uint16_t carrier_extension; /**< carrier number extension, when extension_valid */
    uint8_t channel_id;
    uint8_t extension_valid;
} tetra_mle_irregular_channel;

/** Neighbour cell irregular channel detail for CA or DA (tables 18.66 and 18.67). */
typedef struct {
    tetra_mle_irregular_channel channel;
    uint8_t cell_id;
} tetra_mle_neighbor_irregular_channel;

/**
 * The channel-class and irregular-channel lists that D-NWRK-BROADCAST EXTENSION (table 18.4) and
 * D-NWRK-BROADCAST-DA (table 18.3) both carry. Each count is the decoded list size; its present flag tells an
 * advertised empty list from an omitted one. The array sizes are the largest counts the count widths can carry.
 */
typedef struct {
    tetra_mle_channel_class serving_class[15];
    tetra_mle_neighbor_channel_class neighbor_class[31];
    tetra_mle_irregular_channel serving_irregular[31];
    tetra_mle_neighbor_irregular_channel neighbor_irregular[63];
    uint8_t serving_classes_present;
    uint8_t serving_class_count;
    uint8_t neighbor_classes_present;
    uint8_t neighbor_class_count;
    uint8_t serving_irregular_present;
    uint8_t serving_irregular_count;
    uint8_t neighbor_irregular_present;
    uint8_t neighbor_irregular_count;
} tetra_mle_channel_lists;

/** Neighbour cell information for DA (table 18.65). */
typedef struct {
    uint16_t main_carrier;      /**< main carrier number */
    uint16_t optional_mask;     /**< bit i set: the i-th type-2 element of table 18.65 was present */
    uint16_t carrier_extension; /**< main carrier number extension (type-2 element 0) */
    uint16_t la;                /**< location area (type-2 element 3) */
    uint8_t cell_id;            /**< cell identifier DA */
    uint8_t load_known;         /**< cell load known; load is valid when set */
    uint8_t load;               /**< cell load DA */
} tetra_mle_da_neighbor;

/** D-NWRK-BROADCAST-DA (table 18.3). Each *_valid / *_present flag reports its type-2 element. */
typedef struct {
    uint64_t network_time; /**< TETRA network time, table 18.100 */
    tetra_mle_channel_lists lists;
    tetra_mle_da_neighbor neighbor[7];
    tetra_mle_ca_neighbor local_ca; /**< neighbour cell information for the local CA cell */
    uint16_t reselect;              /**< cell re-select parameters */
    uint8_t cell_id_valid;
    uint8_t cell_id; /**< cell identifier DA */
    uint8_t reselect_valid;
    uint8_t load_valid;
    uint8_t load; /**< cell load DA */
    uint8_t network_time_valid;
    uint8_t local_ca_valid;
    uint8_t neighbor_list_present;
    uint8_t neighbor_count;
} tetra_mle_da_broadcast;

/** Channel classes and channels to remove (tables 18.90 to 18.92). */
typedef struct {
    uint8_t class_count;
    uint8_t class_id[15];
    uint8_t channel_count;
    uint8_t channel_id[31];
} tetra_mle_channel_removal;

/** Removal data for a CA or DA cell (tables 18.90 and 18.91); remove_cell=1 removes the cell and carries no lists. */
typedef struct {
    tetra_mle_channel_removal channels;
    uint8_t cell_id;
    uint8_t remove_cell;
} tetra_mle_cell_removal;

/** D-NWRK-BROADCAST REMOVE (table 18.5). */
typedef struct {
    tetra_mle_cell_removal ca[31];
    tetra_mle_cell_removal da[255];
    tetra_mle_channel_removal serving; /**< removal data for the serving cell (table 18.92) */
    uint8_t ca_present;
    uint8_t ca_count;
    uint8_t da_present;
    uint8_t da_count;
    uint8_t serving_present;
} tetra_mle_broadcast_removal;

/** D-NWRK-BROADCAST (table 18.2). */
typedef struct {
    uint64_t network_time; /**< TETRA network time (table 18.100), when network_time_valid */
    tetra_mle_ca_neighbor neighbor[7];
    uint16_t reselect; /**< cell re-select parameters */
    uint8_t load;      /**< cell load CA */
    uint8_t network_time_valid;
    uint8_t neighbor_count;       /**< number of CA neighbour cells, when neighbor_count_valid */
    uint8_t neighbor_count_valid; /**< the PDU carried the number of CA neighbour cells */
} tetra_mle_nwrk_broadcast;

/** D-CALL RESTORE (EN 300 392-2 table 14.6), the CMCE SDU a D-RESTORE-ACK carries. */
typedef struct {
    uint16_t call_id;
    uint16_t new_call_id; /**< when new_call_id_present */
    uint8_t new_call_id_present;
    uint8_t timeout; /**< call time-out (table 14.50), when timeout_present */
    uint8_t timeout_present;
    uint8_t grant;      /**< transmission grant */
    uint8_t permission; /**< transmission request permission */
    uint8_t reset;      /**< reset call time-out timer */
} tetra_cmce_call_restore;

/** D-RESTORE-ACK (table 18.8). */
typedef struct {
    tetra_cmce_call_restore call; /**< the D-CALL RESTORE it carries */
    uint8_t call_present;         /**< its SDU was a D-CALL RESTORE: an incomplete PDU failed inside that */
} tetra_mle_restore_ack;

/** D-CHANNEL RESPONSE (table 18.10). */
typedef struct {
    uint8_t type;        /**< channel response type */
    uint8_t reason;      /**< reason for rejection */
    uint8_t retry_delay; /**< retry delay */
} tetra_mle_channel_response;

/** One decoded downlink MLE protocol PDU; @c type (and @c extension for an extended PDU) selects what it holds. */
typedef struct {
    union {
        tetra_mle_nwrk_broadcast nwrk_broadcast;     /**< TETRA_MLE_D_NWRK_BROADCAST */
        tetra_mle_channel_lists ext;                 /**< TETRA_MLE_D_NWRK_BCAST_EXT */
        tetra_mle_restore_ack restore_ack;           /**< TETRA_MLE_D_RESTORE_ACK */
        tetra_mle_channel_response channel_response; /**< TETRA_MLE_D_CHANNEL_RESPONSE */
        tetra_mle_da_broadcast da;                   /**< extension TETRA_MLE_D_NWRK_BROADCAST_DA */
        tetra_mle_broadcast_removal remove;          /**< extension TETRA_MLE_D_NWRK_BCAST_REMOVE */
    };

    int sdu_pos;             /**< D-NEW-CELL and D-PREPARE-FAIL: the bit of the TM-SDU where their MM SDU starts */
    uint8_t type;            /**< PDU type (table 18.85, TETRA_MLE_* in <dsd-neo/protocol/tetra/tetra_mle.h>) */
    uint8_t extension;       /**< PDU type extension of TETRA_MLE_EXTENDED_PDU (table 18.86) */
    uint8_t channel_command; /**< D-NEW-CELL: channel command valid */
    uint8_t fail_cause;      /**< D-PREPARE-FAIL and D-RESTORE-FAIL: fail cause */
} tetra_mle_pdu;

/**
 * @brief Decode one downlink MLE protocol PDU without acting on it.
 *
 * @param bits  TM-SDU, one bit per byte (0 or 1), MSB first, starting at the protocol discriminator.
 * @param nbits Number of bits in @p bits.
 * @param out   Receives the PDU; zeroed first.
 * @return 1 when the TM-SDU is an MLE protocol PDU this decoder reads and every element it declares was inside
 *         @p nbits and well formed (for D-RESTORE-ACK, its D-CALL RESTORE too); 0 otherwise. The MM SDU after a
 *         D-NEW-CELL or D-PREPARE-FAIL is not part of the PDU (tetra_mm_decode() reads it).
 */
int tetra_mle_decode(const uint8_t* bits, int nbits, tetra_mle_pdu* out);

/* CMCE short data, clauses 14.7.1.10, 14.7.1.11 and 29. */

/** A calling or transmitting party (tables 14.43 to 14.45): type 1 carries an SSI, type 2 an SSI and its 24-bit
 * extension (MCC and MNC); types 0 and 3 are reserved and carry neither. */
typedef struct {
    uint32_t ssi;
    uint32_t extension;
    uint8_t type;
    uint8_t type_valid;
    uint8_t ssi_valid;
    uint8_t extension_valid;
} tetra_cmce_party;

/** D-STATUS (table 14.14). */
typedef struct {
    tetra_cmce_party calling;
    uint16_t status;              /**< pre-coded status */
    uint8_t short_report;         /**< the status is an SDS-TL SDS-SHORT REPORT (table 29.13: it starts 011111) */
    uint8_t short_report_type;    /**< short report type, when short_report */
    uint8_t short_report_msg_ref; /**< message reference, when short_report */
} tetra_cmce_status;

/** Decoded SDS text: UTF-8 with C0 and C1 controls replaced by spaces, at most 255 bytes plus the terminator, never cut
 * inside a character. */
typedef struct {
    char text[256];
    uint16_t len;
    uint8_t unicode; /**< text coding scheme 26, UCS-2 with the UTF-16BE extension (table 29.29) */
    uint8_t full;    /**< a character did not fit and ended the text */
} tetra_sds_text;

/* SDS-TL message types, table 29.20; TETRA_SDS_TL_NONE for user data without an SDS-TL header. */
#define TETRA_SDS_TL_TRANSFER 0u
#define TETRA_SDS_TL_REPORT   1u
#define TETRA_SDS_TL_ACK      2u
#define TETRA_SDS_TL_NONE     0xFFu

/** SDS-TL header (tables 29.11, 29.12 and 29.14) with its storage/forward control fields (table 29.18 forward address
 * type: 0 SNA, 1 SSI, 2 TSI, 3 external number, 7 none). */
typedef struct {
    uint32_t forward_ssi;
    uint32_t forward_extension;
    char forward_external[25]; /**< external subscriber number digits (table 14.59) */
    uint8_t message_type;      /**< TETRA_SDS_TL_* */
    uint8_t status;            /**< delivery status (table 29.16), SDS-REPORT and SDS-ACK */
    uint8_t reference;         /**< message reference (table 29.19) */
    uint8_t storage_forward;
    uint8_t validity_period;
    uint8_t forward_type;
    uint8_t forward_valid;
    uint8_t forward_sna;
} tetra_sds_tl;

/** One part of a concatenated message (tables 29.60, 29.62 and 29.63). */
typedef struct {
    int data_pos;           /**< the part's user data: bit of the TM-SDU where it starts */
    int data_bits;          /**< and its length */
    int payload_pid;        /**< protocol identifier of the whole message, carried by part 1 only; -1 in the others */
    uint16_t reference;     /**< concatenated message reference, its extension included */
    uint8_t total;          /**< parts in the message */
    uint8_t sequence;       /**< this part's number, 1..total */
    uint8_t with_transport; /**< protocol identifier 140 (SDS-TL) rather than 12 */
} tetra_sds_concat_part;

/** D-SDS-DATA (table 14.13): its user data decoded as far as one PDU goes. A concatenated message's text exists only
 * once every part arrived, so a part leaves @c text empty. */
typedef struct {
    tetra_sds_text text;          /**< the text of a text message (protocol identifiers 2, 9, 130 and 137) */
    tetra_sds_tl tl;              /**< SDS-TL header; message_type TETRA_SDS_TL_NONE without one */
    tetra_sds_concat_part concat; /**< when concat_present */
    tetra_cmce_party calling;
    int data_bits;       /**< user data length */
    uint16_t short_data; /**< user defined data-1, short data type identifier 0 */
    uint8_t sdti;        /**< short data type identifier (table 14.13) */
    uint8_t pid;         /**< protocol identifier (table 29.21) of user defined data-4, when pid_present */
    uint8_t pid_present;
    uint8_t concat_present; /**< the user data is a concatenation part */
} tetra_sds_data;

/** One decoded D-STATUS or D-SDS-DATA; @c type (TETRA_CMCE_D_STATUS or TETRA_CMCE_D_SDS_DATA) selects the member. */
typedef struct {
    union {
        tetra_cmce_status status;
        tetra_sds_data data;
    };

    uint8_t type; /**< CMCE PDU type (table 14.66) */
} tetra_sds_pdu;

/**
 * @brief Decode one D-STATUS or D-SDS-DATA without acting on it.
 *
 * @param bits  TM-SDU, one bit per byte (0 or 1), MSB first, starting at the protocol discriminator.
 * @param nbits Number of bits in @p bits.
 * @param out   Receives the PDU; zeroed first.
 * @return 1 when the TM-SDU is a D-STATUS or D-SDS-DATA, every element it declares was inside @p nbits, and its user
 *         data is well formed as far as this decoder reads it (text, SDS-TL header, concatenation control); 0
 *         otherwise. Concatenated parts are only decoded here: reassembling them is the dispatcher's.
 */
int tetra_sds_decode(const uint8_t* bits, int nbits, tetra_sds_pdu* out);

/** The header of an SN-PDU (EN 300 392-2 clause 28.4.4). */
typedef struct {
    uint16_t nbits;   /**< bits of the SN-PDU, after the protocol discriminator */
    uint8_t pdu_type; /**< SN PDU type (table 28.121) */
    uint8_t nsapi;    /**< NSAPI for the PDU types that carry one; 0 (a reserved value) for the others */
} tetra_sndcp_pdu;

/**
 * @brief Decode the header of one SN-PDU without acting on it.
 *
 * @param bits  TM-SDU, one bit per byte (0 or 1), MSB first, starting at the protocol discriminator.
 * @param nbits Number of bits in @p bits.
 * @param out   Receives the header; zeroed first.
 * @return 1 when the TM-SDU is an SN-PDU whose SN PDU type, and NSAPI where its type carries one, were inside @p nbits;
 *         0 otherwise.
 */
int tetra_sndcp_decode(const uint8_t* bits, int nbits, tetra_sndcp_pdu* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MLE_PDU_H_ */
