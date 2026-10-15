// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA Mobility Management downlink PDUs as decoded (EN 300 392-2 clause 16, EN 300 392-7 annex A).
 *
 * tetra_mm_decode() reads one whole PDU, optional elements included, into a tetra_mm_pdu. tetra_mm_dispatch() decodes
 * through it and acts only on a PDU that decoded completely: a truncated or malformed PDU acts on nothing. Sealed keys
 * are kept as received; a passive monitor never unwraps them. Fields a PDU does not carry read zero.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MM_PDU_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MM_PDU_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Address extension: the MCC and MNC of a TSI (EN 300 392-2 table 16.28). */
typedef struct {
    uint16_t mcc;
    uint16_t mnc;
    uint8_t valid; /**< the element was present */
} tetra_mm_address_t;

/** Time a key change takes effect (EN 300 392-7 table A.111). */
typedef struct {
    uint64_t network_time; /**< time type 1: EN 300 392-2 clause 18.5.24 network time */
    uint16_t hyperframe;   /**< time type 0: absolute IV */
    uint8_t type;          /**< 0 absolute IV, 1 network time, 2 next multiframe, 3 currently in use */
    uint8_t slot;
    uint8_t frame;
    uint8_t multiframe;
} tetra_mm_time_t;

/** SCKs a key change activates (EN 300 392-7 tables A.26 and A.27d): count 0 selects one SCK subset. */
typedef struct {
    uint16_t subset_vn;   /**< SCK-VN of the subset */
    uint16_t version[15]; /**< SCK-VN of each SCK */
    uint8_t count;
    uint8_t grouping; /**< SCK subset grouping type (table A.100a) */
    uint8_t subset;   /**< SCK subset number (table A.100b) */
    uint8_t number[15];
} tetra_mm_sck_set_t;

/** CCK information with its location area information (EN 300 392-7 tables A.42 and A.43). */
typedef struct {
    uint16_t id;
    uint16_t la[15];
    uint16_t la_mask;
    uint16_t la_selector;
    uint16_t la_low;
    uint16_t la_high;
    uint8_t key_type; /**< 0 current, 1 future */
    uint8_t sealed[15];
    uint8_t la_type; /**< 0 all, 1 list, 2 mask and selector, 3 range */
    uint8_t la_count;
    uint8_t future;
    uint8_t future_sealed[15];
} tetra_mm_cck_info_t;

/** D-AUTHENTICATION demand, response, result or reject (EN 300 392-7 tables A.1 to A.4). */
typedef struct {
    uint32_t response;            /**< RES2 */
    uint8_t subtype;              /**< table A.40 */
    uint8_t random_challenge[10]; /**< RAND1 */
    uint8_t random_seed[10];      /**< RS */
    uint8_t mutual;
    uint8_t result;
    uint8_t reject_reason;
} tetra_mm_auth_t;

/** D-OTAR (EN 300 392-7 tables A.9, A.12, A.15, A.16, A.19, A.20, A.23, A.24, A.27d, A.27f, A.27h, A.30 and A.30a):
 * the union of the fields of every downlink sub-type; fields the sub-type does not carry are zero. */
typedef struct {
    tetra_mm_cck_info_t cck;
    tetra_mm_address_t address;
    tetra_mm_sck_set_t dm_sck;
    tetra_mm_time_t dm_sck_time;
    uint32_t gssi;
    uint32_t gck_select;
    uint32_t group_gssi[30];
    uint32_t gck_reject_gssi[7];
    uint16_t max_response_timer;
    uint16_t gsko_version;
    uint16_t sck_version[7];
    uint16_t gck_number[7];
    uint16_t gck_version[7];
    uint16_t key_status_gck_number;
    uint16_t key_delete_gck_number[15];
    uint8_t subtype; /**< table A.85 */
    uint8_t cck_provision;
    uint8_t dck_forwarding_result;
    uint8_t ack;
    uint8_t explicit_response;
    uint8_t session_key;
    uint8_t random_seed[10];
    uint8_t key_count;
    uint8_t sck_number[7];
    uint8_t sck_use[7];
    uint8_t sck_sealed[7][15];
    uint8_t gck_sealed[7][15];
    uint8_t reject_reason[7];
    uint8_t group_association;
    uint8_t gck_reject_group_association[7];
    uint8_t key_association_type;
    uint8_t sck_select;
    uint8_t sck_subset_grouping;
    uint8_t group_count;
    uint8_t group_is_range;
    uint8_t group_value_count;
    uint8_t ksg;
    uint8_t retry_interval;
    uint8_t gsko_sealed[15];
    uint8_t key_status_type;
    uint8_t key_status_sck_number;
    uint8_t key_status_sck_grouping;
    uint8_t key_status_sck_subset;
    uint8_t key_delete_type;
    uint8_t key_delete_sck_count;
    uint8_t key_delete_sck_number[31];
    uint8_t key_delete_sck_grouping;
    uint8_t key_delete_sck_subset;
    uint8_t key_delete_gck_count;
    uint8_t dm_sck_ack;
} tetra_mm_otar_t;

/** D-CK CHANGE DEMAND (EN 300 392-7 table A.26). */
typedef struct {
    tetra_mm_sck_set_t sck;
    tetra_mm_time_t time;
    uint16_t cck_id;
    uint16_t gck_vn;
    uint16_t gck_number[15];
    uint16_t gck_version[15];
    uint8_t ack;
    uint8_t security_class;
    uint8_t key_type; /**< key change type (table A.71) */
    uint8_t sck_use;
    uint8_t gck_count;
} tetra_mm_ck_change_t;

/** D-ENABLE or D-DISABLE (EN 300 392-7 tables A.31 and A.32). */
typedef struct {
    uint64_t tei;
    uint32_t ssi;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t is_enable;
    uint8_t intent; /**< 0 intent, 1 confirm */
    uint8_t permanent;
    uint8_t equipment;
    uint8_t subscription;
    uint8_t auth_valid;
    uint8_t auth_challenge[20]; /**< RAND1 then RS (table A.37) */
} tetra_mm_enable_disable_t;

/** D-LOCATION UPDATE ACCEPT (EN 300 392-2 table 16.11) type 1 and type 2 elements. */
typedef struct {
    uint32_t ssi;
    tetra_mm_address_t address;
    uint16_t subscriber_class;
    uint16_t energy_saving;
    uint8_t type;
    uint8_t ssi_valid;
    uint8_t subscriber_class_valid;
    uint8_t energy_saving_valid;
    uint8_t scch;
    uint8_t scch_valid;
} tetra_mm_lu_accept_t;

/** D-LOCATION UPDATE COMMAND (EN 300 392-2 table 16.12). */
typedef struct {
    tetra_mm_address_t address;
    uint16_t cipher_params;
    uint8_t group_report;
    uint8_t cipher_control;
} tetra_mm_lu_command_t;

/** D-LOCATION UPDATE REJECT (EN 300 392-2 table 16.13). */
typedef struct {
    tetra_mm_address_t address;
    uint16_t cipher_params;
    uint8_t type;
    uint8_t cause;
    uint8_t cipher_control;
} tetra_mm_lu_reject_t;

/** D-LOCATION UPDATE PROCEEDING (EN 300 392-2 table 16.14). */
typedef struct {
    uint32_t ssi;
    tetra_mm_address_t address;
} tetra_mm_lu_proceeding_t;

/** One group identity of a group identity downlink element (EN 300 392-2 table 16.54). */
typedef struct {
    uint32_t gssi;
    uint32_t vgssi;
    tetra_mm_address_t address; /**< present with a GTSI (address types 1 and 3) */
    uint8_t action;             /**< 0 attachment, 1 detachment */
    uint8_t attachment_lifetime;
    uint8_t class_of_usage;
    uint8_t detachment_reason;
    uint8_t address_type; /**< 0 GSSI, 1 GTSI, 2 (V)GSSI, 3 GTSI and (V)GSSI */
} tetra_mm_group_entry_t;

/** D-ATTACH/DETACH GROUP IDENTITY (EN 300 392-2 table 16.1): the group identities of every group identity downlink
 * element in its type 3/4 chain. */
typedef struct {
    tetra_mm_group_entry_t entry[63];
    uint8_t count;
    uint8_t report;      /**< group identity report */
    uint8_t ack_request; /**< group identity acknowledgement request */
    uint8_t detach_all;  /**< group identity attach/detach mode: 1 detaches every attached group before these apply */
} tetra_mm_group_identity_t;

/** D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT (EN 300 392-2 table 16.2). */
typedef struct {
    uint8_t rejected; /**< group identity accept/reject (table 16.44): 1 at least one was rejected */
} tetra_mm_group_ack_t;

/** D-MM STATUS (EN 300 392-2 table 16.3). */
typedef struct {
    uint8_t status; /**< status downlink (table 16.92) */
} tetra_mm_status_t;

/** MM PDU/FUNCTION NOT SUPPORTED (EN 300 392-2 table 16.27). */
typedef struct {
    uint8_t pdu_type; /**< the PDU type the MS does not support */
} tetra_mm_fns_t;

/** One decoded downlink MM PDU; @c type selects the member that holds it. */
typedef struct {
    union {
        tetra_mm_otar_t otar;                     /**< TETRA_MM_D_OTAR */
        tetra_mm_auth_t auth;                     /**< TETRA_MM_D_AUTHENTICATION */
        tetra_mm_ck_change_t ck_change;           /**< TETRA_MM_D_CK_CHANGE_DEMAND */
        tetra_mm_enable_disable_t enable_disable; /**< TETRA_MM_D_DISABLE and TETRA_MM_D_ENABLE */
        tetra_mm_lu_accept_t lu_accept;           /**< TETRA_MM_D_LOCATION_UPDATING_ACCEPT */
        tetra_mm_lu_command_t lu_command;         /**< TETRA_MM_D_LOCATION_UPDATING_COMMAND */
        tetra_mm_lu_reject_t lu_reject;           /**< TETRA_MM_D_LOCATION_UPDATING_REJECT */
        tetra_mm_lu_proceeding_t lu_proceeding;   /**< TETRA_MM_D_LOCATION_UPDATING_PROCEEDING */
        tetra_mm_group_identity_t group_identity; /**< TETRA_MM_D_ATTACH_DETACH_GROUP */
        tetra_mm_group_ack_t group_ack;           /**< TETRA_MM_D_ATTACH_DETACH_GROUP_ACK */
        tetra_mm_status_t status;                 /**< TETRA_MM_D_MM_STATUS */
        tetra_mm_fns_t fns;                       /**< TETRA_MM_FUNCTION_NOT_SUPPORTED */
    };

    uint8_t type; /**< PDU type (EN 300 392-2 table 16.75, TETRA_MM_* in <dsd-neo/protocol/tetra/tetra_mm.h>) */
} tetra_mm_pdu;

/**
 * @brief Decode one downlink MM PDU without acting on it.
 *
 * @param bits  One byte per bit (0 or 1), MSB first, the 4-bit PDU type at bit 0.
 * @param nbits Number of bits in the PDU.
 * @param out   Receives the PDU; zeroed first, so elements the PDU does not carry read zero. @c out->type is the PDU
 *              type once its 4 bits were there.
 * @return 1 when the whole PDU, optional elements included, was inside @p nbits and well formed; 0 when it was
 *         truncated or malformed, or is a reserved PDU type or a D-OTAR sub-type without a downlink layout.
 */
int tetra_mm_decode(const uint8_t* bits, int nbits, tetra_mm_pdu* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_MM_PDU_H_ */
