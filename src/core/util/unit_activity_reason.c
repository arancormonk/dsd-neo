// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The reason tables of <dsd-neo/core/unit_activity_reason.h>: data only.
 */

#include <dsd-neo/core/unit_activity_reason.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t code;
    const char* text;
} ua_reason_entry;

typedef struct {
    const ua_reason_entry* entries;
    size_t count;
} ua_reason_table;

#define UA_TABLE(t) {(t), sizeof(t) / sizeof((t)[0])}

/* P25 Deny Response reasons (TIA-102.AABC Annex B), in the words dsd-neo's P25 prints use. */
static const ua_reason_entry k_p25_deny[] = {
    {0x10U, "Requesting Unit Not Valid"},
    {0x11U, "Requesting Unit Not Authorized"},
    {0x20U, "Target Unit Not Valid"},
    {0x21U, "Target Unit Not Authorized"},
    {0x2FU, "Target Unit Refused Call"},
    {0x30U, "Target Group Not Valid"},
    {0x31U, "Target Group Not Authorized"},
    {0x40U, "Invalid Dialing"},
    {0x41U, "Telephone Number Not Authorized"},
    {0x42U, "PSTN Not Valid"},
    {0x50U, "Call Timeout"},
    {0x51U, "Landline Terminated Call"},
    {0x52U, "Subscriber Unit Terminated Call"},
    {0x5FU, "Call Preempted"},
    {0x60U, "Site Access Denial"},
    {0x67U, "PTT Collide"},
    {0x77U, "PTT Bonk"},
    {0xF0U, "Call Options Not Valid For Service"},
    {0xF1U, "Protection Service Option Not Valid"},
    {0xF2U, "Duplex Service Option Not Valid"},
    {0xF3U, "Circuit/Packet Mode Option Not Valid"},
    {0xFFU, "System Does Not Support Service"},
};

/* P25 Queued Response reasons (TIA-102.AABC Annex C). */
static const ua_reason_entry k_p25_queued[] = {
    {0x10U, "Requesting Unit Busy Other Service"}, {0x20U, "Target Unit Busy Other Service"},
    {0x2FU, "Target Unit Queued This Call"},       {0x30U, "Target Group Currently Active"},
    {0x40U, "Channel Resources Unavailable"},      {0x41U, "Telephone Resources Unavailable"},
    {0x42U, "Data Resources Unavailable"},         {0x50U, "Superseding Service Currently Active"},
};

/* DMR Tier III reasons, TS 102 361-4 V1.12.1 clause 7.2.8: Table 7.42 (C_ACK), Table 7.43 (C_NACK), Table 7.44
 * (C_QACK), Table 7.45 (C_WACK), and MS_Duplex_Not_Supported 0x16 from the clause 7.2.8.2 text. Values the MS sends,
 * or the TS mirrors for it, have the direction bit clear. */
static const ua_reason_entry k_dmr_tier3[] = {
    /* C_ACK, sent by the TS */
    {0x60U, "Message accepted"},
    {0x61U, "Placed in store and forward"},
    {0x62U, "Registration accepted"},
    {0x63U, "Accepted for the status polling service"},
    {0x64U, "Authentication response"},
    {0x65U, "Registration with talkgroup subscription / attachment"},
    /* C_ACK, sent by the MS or mirrored by the TS */
    {0x44U, "Accepted by the radio"},
    {0x45U, "Radio will call back"},
    {0x46U, "Radio alerting"},
    {0x47U, "Accepted for the status polling service (radio)"},
    {0x48U, "Authentication response (radio)"},
    /* C_NACK, refused by the network */
    {0x20U, "Service not supported by the network"},
    {0x21U, "Not authorised for this user (permanent)"},
    {0x22U, "Not authorised for this user (temporary)"},
    {0x23U, "Service not available at this time"},
    {0x24U, "Called party not in contact (not registered)"},
    {0x25U, "Called party not in contact (registered)"},
    {0x26U, "Called radio has diverted its calls"},
    {0x27U, "Network busy"},
    {0x28U, "Network not ready"},
    {0x29U, "Call cancel refused"},
    {0x2AU, "Registration refused"},
    {0x2BU, "Registration denied"},
    {0x2CU, "IP connection failed"},
    {0x2DU, "Radio not registered"},
    {0x2EU, "Called party busy"},
    {0x2FU, "Talkgroup not allowed"},
    {0x30U, "CRC error in the UDT upload"},
    {0x31U, "No resources for a duplex call"},
    {0x3FU, "Refused, reason unknown"},
    /* C_NACK, refused by the MS (may be forwarded by the TS) */
    {0x00U, "Radio does not support this service"},
    {0x11U, "Line not supported by the called party"},
    {0x12U, "Called party call stack full"},
    {0x13U, "Called party equipment busy"},
    {0x14U, "Refused by the called party"},
    {0x15U, "Refused, custom reason"},
    {0x16U, "Radio does not support duplex"},
    {0x1FU, "Refused by the radio, reason unknown"},
    /* C_QACK */
    {0xA0U, "Queued for a resource"},
    {0xA1U, "Queued, called party busy"},
    /* C_WACK */
    {0xE0U, "Wait for further signalling"},
};

/* DMR Tier II NACK_Rsp, TS 102 361-2 V2.5.1 clause 7.2.3, Table 7.13: the only Reason Code it defines. */
static const ua_reason_entry k_dmr_tier2_nack[] = {
    {0x21U, "Radio does not support this service or feature"},
};

/* NXDN Cause (MM), TS 1-A v1.3 Table 6.5-27 (Type << 4 | Indication). */
static const ua_reason_entry k_nxdn_mm[] = {
    {0x01U, "Registration is accepted"},
    {0x04U, "Location is accepted, but Group is failed"},
    {0x05U, "Location is accepted, but Group is refused"},
    {0x06U, "Registration is failed"},
    {0x08U, "Registration is refused"},
    {0x51U, "Network failure"},
    {0x52U, "Temporary failure"},
    {0x53U, "Equipment congestion"},
    {0x5FU, "Other class for resource unavailable"},
    {0x60U, "Request for unavailable service"},
    {0x6FU, "Other class for service unavailable or unsupported"},
    {0x70U, "Lack of mandatory information elements"},
    {0x71U, "Undefined information element or Invalid contents"},
    {0x7FU, "Other class for procedure error"},
};

/* NXDN Cause (SS), TS 1-A v1.3 Table 6.5-29. */
static const ua_reason_entry k_nxdn_ss[] = {
    {0x01U, "ACK (Receive success)"},
    {0x02U, "ACK (Send success)"},
    {0x08U, "NACK (Request for full retry)"},
    {0x09U, "NACK (Memory Full)"},
    {0x0AU, "NACK (Abort)"},
    {0x10U, "Accepted normal"},
    {0x11U, "Called group is not permitted for the service"},
    {0x12U, "Calling SU is not permitted for the service"},
    {0x13U, "Called SU is not permitted for the service"},
    {0x14U, "Called SU has not done the registration yet"},
    {0x15U, "No response from the called SU"},
    {0x16U, "Incoming call rejection for a called SU"},
    {0x18U, "Called SU is in busy"},
    {0x19U, "Called group is in busy"},
    {0x1AU, "Calling SU is in busy"},
    {0x1CU, "SU has not been registered yet"},
    {0x1DU, "Group has not been registered yet"},
    {0x20U, "Calling SU is not permitted for use"},
    {0x21U, "Called SU is not permitted for use"},
    {0x22U, "Group is not permitted for use"},
    {0x50U, "Channel unavailable"},
    {0x51U, "Network failure"},
    {0x52U, "Temporary failure"},
    {0x53U, "Equipment congestion"},
    {0x5FU, "Other class for resource unavailable"},
    {0x60U, "Request for unavailable service"},
    {0x61U, "Request for unsupported service"},
    {0x6FU, "Other class for service unavailable or unsupported"},
    {0x70U, "Lack of mandatory information elements"},
    {0x71U, "Undefined information element or Invalid contents"},
    {0x7FU, "Other class for procedure error"},
};

/* TETRA reject cause, EN 300 392-2 V3.8.1 Table 16.81 (0, 17 and 22 to 31 are reserved). */
static const ua_reason_entry k_tetra_lu_reject[] = {
    {1U, "ITSI/ATSI unknown"},
    {2U, "Illegal MS"},
    {3U, "LA not allowed"},
    {4U, "LA unknown"},
    {5U, "Network failure"},
    {6U, "Congestion"},
    {7U, "Forward registration failure"},
    {8U, "Service not subscribed"},
    {9U, "Mandatory element error"},
    {10U, "Message consistency error"},
    {11U, "Roaming not supported"},
    {12U, "Migration not supported"},
    {13U, "No cipher KSG"},
    {14U, "Identified cipher KSG not supported"},
    {15U, "Requested cipher key type not available"},
    {16U, "Identified cipher key not available"},
    {18U, "Ciphering required"},
    {19U, "Authentication failure"},
    {20U, "Use of CA cell not permitted"},
    {21U, "Use of DA cell not permitted"},
};

static const char*
ua_reason_find(ua_reason_table table, uint16_t code) {
    if (code > 0xFFU) {
        return NULL;
    }
    for (size_t i = 0; i < table.count; i++) {
        if (table.entries[i].code == code) {
            return table.entries[i].text;
        }
    }
    return NULL;
}

/* A P25 code no entry names reads as its range: Reserved up to @p reserved_max, User/System Defined above. */
static const char*
ua_reason_p25(ua_reason_table table, uint16_t code, uint8_t reserved_max) {
    if (code > 0xFFU) {
        return NULL;
    }
    const char* text = ua_reason_find(table, code);
    if (text) {
        return text;
    }
    return code <= reserved_max ? "Reserved" : "User/System Defined";
}

const char*
dsd_ua_reason_text(uint8_t reason_set, uint16_t code) {
    static const ua_reason_table p25_deny = UA_TABLE(k_p25_deny);
    static const ua_reason_table p25_queued = UA_TABLE(k_p25_queued);
    static const ua_reason_table dmr_tier3 = UA_TABLE(k_dmr_tier3);
    static const ua_reason_table dmr_tier2_nack = UA_TABLE(k_dmr_tier2_nack);
    static const ua_reason_table nxdn_mm = UA_TABLE(k_nxdn_mm);
    static const ua_reason_table nxdn_ss = UA_TABLE(k_nxdn_ss);
    static const ua_reason_table tetra_lu_reject = UA_TABLE(k_tetra_lu_reject);
    switch (reason_set) {
        case DSD_UA_REASON_P25_DENY: return ua_reason_p25(p25_deny, code, 0x5EU);
        case DSD_UA_REASON_P25_QUEUED: return ua_reason_p25(p25_queued, code, 0x7FU);
        case DSD_UA_REASON_DMR_TIER3: return ua_reason_find(dmr_tier3, code);
        case DSD_UA_REASON_DMR_TIER2_NACK: return ua_reason_find(dmr_tier2_nack, code);
        case DSD_UA_REASON_NXDN_MM: return ua_reason_find(nxdn_mm, code);
        case DSD_UA_REASON_NXDN_SS: return ua_reason_find(nxdn_ss, code);
        case DSD_UA_REASON_TETRA_LU_REJECT: return ua_reason_find(tetra_lu_reject, code);
        default: return NULL;
    }
}
