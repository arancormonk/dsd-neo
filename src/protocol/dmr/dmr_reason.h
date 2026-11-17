// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What DMR acknowledgement reasons and C_AHOY polls mean: the class of a Tier III C_ACKD reason, and the
 * C_AHOY service kinds a control channel listener can tell apart.
 *
 * The reason texts live in the core's one table set (<dsd-neo/core/unit_activity_reason.h>: DSD_UA_REASON_DMR_TIER3
 * and DSD_UA_REASON_DMR_TIER2_NACK, kept apart because Tier II, ETSI TS 102 361-2 Table 7.13, and Tier III, ETSI TS
 * 102 361-4 Tables 7.42-7.45, give 0x21 different meanings), which the CSBK prints and the unit activity log share.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_REASON_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_REASON_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A Tier III Reason's class, its two top bits (TS 102 361-4 clause 7.2.8.0: t t d a a a a a).
 */
typedef enum {
    DMR_TIER3_REASON_NACK = 0, /**< tt 00: refused (C_NACKD / C_NACKU). */
    DMR_TIER3_REASON_ACK = 1,  /**< tt 01: accepted. */
    DMR_TIER3_REASON_QACK = 2, /**< tt 10: queued. */
    DMR_TIER3_REASON_WACK = 3, /**< tt 11: wait, more signalling follows. */
} dmr_tier3_reason_class;

/** @brief The class of a Tier III C_ACKD / C_ACKU / P_ACKD / P_ACKU Reason (its ACK type bits). */
dmr_tier3_reason_class dmr_tier3_reason_class_of(uint8_t reason);

/**
 * @brief What a C_AHOY of Service_Kind 13 or 14 asks of its target, told from its whole field combination.
 */
typedef enum {
    DMR_AHOY_OTHER_SERVICE = 0,    /**< Another Service_Kind: a call set-up or service poll. */
    DMR_AHOY_STUN = 1,             /**< Kind 13 from STUNI, flag 0, individual target (Table 6.19). */
    DMR_AHOY_REVIVE = 2,           /**< Kind 13 from STUNI, flag 1, individual target (Table 6.19). */
    DMR_AHOY_KILL = 3,             /**< Kind 13 from KILLI, flag 0, individual target (Table 6.23). */
    DMR_AHOY_RADIO_CHECK = 4,      /**< Kind 14 from TSI, flag 0: unsolicited radio check (Table 6.27). */
    DMR_AHOY_AUTHENTICATION = 5,   /**< Kind 14, flag 0, individual target, a challenge value as the source
                                        (Table 6.17). The source field is the challenge, never an identity. */
    DMR_AHOY_SUBSCRIPTION_REQ = 6, /**< Kind 14 from TATTSI, flag 1, individual target: the TSCC asks for the
                                        talkgroup subscription / attachment list (Table 6.11). */
    DMR_AHOY_UNRECOGNISED = 7,     /**< Kind 13 or 14 with a combination none of the above has (for example DGNA
                                        or supplementary data, or a stun addressed to a talkgroup). */
} dmr_ahoy_kind;

/** @brief The C_AHOY fields the classification reads (TS 102 361-4 Table 7.22). */
typedef struct {
    uint8_t service_kind;      /**< Bits 28-31. */
    uint8_t service_kind_flag; /**< Bit 23. */
    uint8_t group;             /**< G/I, bit 25: 1 when the target is a talkgroup. */
    uint32_t target;           /**< Bits 32-55. */
    uint32_t source;           /**< Bits 56-79: source, gateway or authentication challenge. */
} dmr_ahoy_fields;

/** @brief Read the dmr_ahoy_fields of an 80-bit C_AHOY CSBK given as one bit per element, MSB first. */
void dmr_ahoy_fields_from_bits(const uint8_t* cs_pdu_bits, dmr_ahoy_fields* out);

/** @brief Classify a C_AHOY; returns a dmr_ahoy_kind. */
int dmr_ahoy_classify(const dmr_ahoy_fields* fields);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_REASON_H_ */
