// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DMR acknowledgement reason classes and C_AHOY classification (see dmr_reason.h). The reason texts are the core's
 * (<dsd-neo/core/unit_activity_reason.h>), which the prints and the unit activity log share.
 */

#include "dmr_reason.h"

#include <dsd-neo/core/bit_packing.h>
#include <stddef.h>
#include <stdint.h>

dmr_tier3_reason_class
dmr_tier3_reason_class_of(uint8_t reason) {
    return (dmr_tier3_reason_class)((reason >> 6) & 3U);
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
