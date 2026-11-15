// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR acknowledgement reasons and C_AHOY classification (see dmr_reason.h).
 */

#include "dmr_reason.h"

#include <dsd-neo/core/bit_packing.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t value;
    const char* text;
} dmr_reason_entry;

/* TS 102 361-4 V1.12.1 clause 7.2.8: Table 7.42 (C_ACK), Table 7.43 (C_NACK), Table 7.44 (C_QACK), Table 7.45
   (C_WACK). Values the MS sends, or the TS mirrors for it, have the direction bit clear. */
static const dmr_reason_entry k_tier3_reasons[] = {
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

/* TS 102 361-2 V2.5.1 clause 7.2.3, Table 7.13: the only Reason Code NACK_Rsp defines. */
static const dmr_reason_entry k_tier2_nack_reasons[] = {
    {0x21U, "Radio does not support this service or feature"},
};

static const char*
dmr_reason_lookup(const dmr_reason_entry* table, size_t count, uint8_t value) {
    for (size_t i = 0; i < count; i++) {
        if (table[i].value == value) {
            return table[i].text;
        }
    }
    return NULL;
}

dmr_tier3_reason_class
dmr_tier3_reason_class_of(uint8_t reason) {
    return (dmr_tier3_reason_class)((reason >> 6) & 3U);
}

const char*
dmr_tier3_reason_text(uint8_t reason) {
    return dmr_reason_lookup(k_tier3_reasons, sizeof k_tier3_reasons / sizeof k_tier3_reasons[0], reason);
}

const char*
dmr_tier2_nack_reason_text(uint8_t reason) {
    return dmr_reason_lookup(k_tier2_nack_reasons, sizeof k_tier2_nack_reasons / sizeof k_tier2_nack_reasons[0],
                             reason);
}

/* TS 102 361-4 Table A.8 gateway identifiers the C_AHOY classification needs. */
enum {
    DMR_GW_TSI = 0xFFFECA,
    DMR_GW_STUNI = 0xFFFECC,
    DMR_GW_KILLI = 0xFFFECF,
    DMR_GW_TATTSI = 0xFFFED7,
    /* The highest authentication challenge value (Table 6.17: 000000 to FFFCDF). */
    DMR_AHOY_CHALLENGE_MAX = 0xFFFCDF,
};

void
dmr_ahoy_fields_from_bits(const uint8_t* cs_pdu_bits, dmr_ahoy_fields* out) {
    if (cs_pdu_bits == NULL || out == NULL) {
        return;
    }
    out->service_kind_flag = cs_pdu_bits[23];
    out->group = cs_pdu_bits[25];
    out->service_kind = (uint8_t)convert_bits_into_output(&cs_pdu_bits[28], 4);
    out->target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    out->source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);
}

/* Service_Kind 13 (Table 7.50 note: the gateway says which supplementary service). Stun, revive and kill address one
   radio (G/I 0, Tables 6.19 / 6.23); kill has no flag-1 form. */
static dmr_ahoy_kind
dmr_ahoy_classify_supplementary(const dmr_ahoy_fields* f) {
    if (f->group != 0U) {
        return DMR_AHOY_UNRECOGNISED;
    }
    if (f->source == DMR_GW_STUNI) {
        return f->service_kind_flag ? DMR_AHOY_REVIVE : DMR_AHOY_STUN;
    }
    if (f->source == DMR_GW_KILLI && f->service_kind_flag == 0U) {
        return DMR_AHOY_KILL;
    }
    return DMR_AHOY_UNRECOGNISED;
}

/* Service_Kind 14: registration / authentication / MS radio check (Table 7.50). Flag 1 is the talkgroup subscription
   list request from TATTSI (Table 6.11); flag 0 from TSI is an unsolicited radio check of a radio or a talkgroup
   (Table 6.27); flag 0 with a challenge value in the source field is an authentication challenge to one radio (Table
   6.17). */
static dmr_ahoy_kind
dmr_ahoy_classify_registration(const dmr_ahoy_fields* f) {
    if (f->service_kind_flag != 0U) {
        return (f->source == DMR_GW_TATTSI && f->group == 0U) ? DMR_AHOY_SUBSCRIPTION_REQ : DMR_AHOY_UNRECOGNISED;
    }
    if (f->source == DMR_GW_TSI) {
        return DMR_AHOY_RADIO_CHECK;
    }
    if (f->source <= DMR_AHOY_CHALLENGE_MAX && f->group == 0U) {
        return DMR_AHOY_AUTHENTICATION;
    }
    return DMR_AHOY_UNRECOGNISED;
}

int
dmr_ahoy_classify(const dmr_ahoy_fields* fields) {
    if (fields == NULL) {
        return DMR_AHOY_UNRECOGNISED;
    }
    if (fields->service_kind == 13U) {
        return dmr_ahoy_classify_supplementary(fields);
    }
    if (fields->service_kind == 14U) {
        return dmr_ahoy_classify_registration(fields);
    }
    return DMR_AHOY_OTHER_SERVICE;
}
