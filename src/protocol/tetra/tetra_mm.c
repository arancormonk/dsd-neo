// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Mobility Management (MM) downlink PDU decoder: EN 300 392-2 clause 16, and the authentication, OTAR, cipher
 * key change and enable/disable PDUs of EN 300 392-7 annex A that share the MM protocol discriminator.
 *
 * Every parser reads its PDU through a bounded bit reader into a local snapshot, including the O-bit, P-bits and the
 * M-bit chain of EN 300 392-2 annex E.1.1, and replaces the published snapshot only when the whole PDU was there. A
 * truncated or malformed PDU leaves the previous snapshot in place.
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "tetra_log.h"
#include "tetra_state.h"

/* EN 300 392-2 clause 21.4.4.1: downlink main carrier frequency = base frequency + main carrier x 25 kHz + offset.
 * ETSI TS 100 392-15 table 1 gives frequency band values 1 to 9 the base frequency value x 100 MHz. Value 0 means the
 * cell has no main carrier (clause 21.4.4.1, table 21.65 note 3) and 10 to 15 have no base frequency. */
long
tetra_carrier_to_dl_hz(uint32_t main_carrier, uint32_t freq_band, uint32_t freq_offset) {
    static const long offset_hz[4] = {0L, 6250L, -6250L, 12500L};
    if (freq_band < 1u || freq_band > 9u || main_carrier > 4095u || freq_offset > 3u) {
        return 0L;
    }
    return (long)freq_band * 100000000L + (long)main_carrier * 25000L + offset_hz[freq_offset];
}

/* One MM PDU being decoded: the reader starts after the 4-bit PDU type. */
typedef struct {
    tetra_bit_reader r;
    dsd_state* state;
    tetra_state_t* ts;
    int cc;
} mm_pdu;

static uint8_t
mm_u8(tetra_bit_reader* r, int width) {
    return (uint8_t)tetra_bits_read(r, width);
}

static uint16_t
mm_u16(tetra_bit_reader* r, int width) {
    return (uint16_t)tetra_bits_read(r, width);
}

static void
mm_read_octets(tetra_bit_reader* r, uint8_t* out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        out[i] = mm_u8(r, 8);
    }
}

/* Address extension: a 10-bit MCC and a 14-bit MNC (EN 300 392-2 table 16.28). */
static void
mm_address_from(uint32_t value, tetra_mm_address_t* address) {
    address->mcc = (uint16_t)((value >> 14) & 0x3FFu);
    address->mnc = (uint16_t)(value & 0x3FFFu);
    address->valid = 1;
}

static void
mm_read_address(tetra_bit_reader* r, tetra_mm_address_t* address) {
    mm_address_from(tetra_bits_read(r, 24), address);
}

static void
mm_log_invalid(const mm_pdu* pdu, const char* name) {
    TETRA_LOG_PAYLOAD(pdu->ts, "[TETRA MM %s] CC=%d (invalid/truncated: %d bits)\n", name, pdu->cc, pdu->r.nbits);
}

/* The optional part of a PDU (EN 300 392-2 annex E.1.1): the O-bit; when it is set, a P-bit and, if that is set, a
 * value for each of the @p count type 2 elements, then the M-bit chain of type 3/4 elements. A PDU that defines no
 * optional element still carries the O-bit (annex E.1.1 note 4); one set there introduces type 3/4 elements a later
 * edition added. 1 when the whole optional part was inside the PDU. */
static int
mm_read_optional(tetra_bit_reader* r, const uint8_t* width, size_t count, uint32_t* value, uint8_t* present) {
    if (!tetra_bits_present(r)) {
        return tetra_bits_ok(r);
    }
    for (size_t i = 0; i < count; i++) {
        present[i] = (uint8_t)tetra_bits_present(r);
        if (present[i]) {
            value[i] = tetra_bits_read(r, width[i]);
        }
    }
    return tetra_bits_skip_type34(r);
}

/* Optional part of a PDU with no type 2 element. */
static int
mm_read_type34_tail(tetra_bit_reader* r) {
    return mm_read_optional(r, NULL, 0, NULL, NULL);
}

/* Optional part of a PDU whose only type 2 element is the address extension. */
static int
mm_read_address_tail(tetra_bit_reader* r, tetra_mm_address_t* address) {
    static const uint8_t width[1] = {24};
    uint32_t value[1] = {0};
    uint8_t present[1] = {0};
    if (!mm_read_optional(r, width, 1, value, present)) {
        return 0;
    }
    if (present[0]) {
        mm_address_from(value[0], address);
    }
    return 1;
}

/* EN 300 392-7 tables A.1 to A.4: D-AUTHENTICATION. */

/* Table A.1 DEMAND: RAND1, RS and a proprietary element. */
static int
mm_auth_demand(tetra_bit_reader* r, tetra_mm_auth_t* a) {
    mm_read_octets(r, a->random_challenge, sizeof a->random_challenge);
    mm_read_octets(r, a->random_seed, sizeof a->random_seed);
    return mm_read_type34_tail(r);
}

/* Table A.3 RESPONSE: RS, RES2, the mutual authentication flag, RAND1 when it is set, and a proprietary element. */
static int
mm_auth_response(tetra_bit_reader* r, tetra_mm_auth_t* a) {
    mm_read_octets(r, a->random_seed, sizeof a->random_seed);
    a->response = tetra_bits_read(r, 32);
    a->mutual = mm_u8(r, 1);
    if (a->mutual) {
        mm_read_octets(r, a->random_challenge, sizeof a->random_challenge);
    }
    return mm_read_type34_tail(r);
}

/* Table A.4 RESULT: R1, the mutual authentication flag, RES2 when it is set, and a proprietary element. */
static int
mm_auth_result(tetra_bit_reader* r, tetra_mm_auth_t* a) {
    a->result = mm_u8(r, 1);
    a->mutual = mm_u8(r, 1);
    if (a->mutual) {
        a->response = tetra_bits_read(r, 32);
    }
    return mm_read_type34_tail(r);
}

/* Table A.2 REJECT: the reject reason, of which table A.38 defines only 0 (authentication not supported). */
static int
mm_auth_reject(tetra_bit_reader* r, tetra_mm_auth_t* a) {
    a->reject_reason = mm_u8(r, 3);
    return a->reject_reason == 0;
}

static void
mm_parse_authentication(mm_pdu* pdu) {
    typedef int (*reader)(tetra_bit_reader*, tetra_mm_auth_t*);
    static const reader readers[4] = {mm_auth_demand, mm_auth_response, mm_auth_result, mm_auth_reject};
    static const char* const names[4] = {"DEMAND", "RESPONSE", "RESULT", "REJECT"}; /* table A.40 */
    tetra_mm_auth_t a = {0};
    a.subtype = mm_u8(&pdu->r, 2);
    if (!readers[a.subtype](&pdu->r, &a) || !tetra_bits_ok(&pdu->r)) {
        mm_log_invalid(pdu, "D-AUTHENTICATION");
        return;
    }
    const char* outcome = "";
    if (a.subtype == 2) {
        outcome = a.result ? " successful" : " failed"; /* Table A.39. */
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-AUTHENTICATION] CC=%d %s%s%s\n", pdu->cc, names[a.subtype], outcome,
                a.mutual ? " mutual" : "");
    a.valid = 1;
    pdu->ts->mm_auth = a;
}

/* EN 300 392-7 annex A.2 to A.5: D-OTAR. */

/* Table A.84b defines OTAR reject reasons 0 to 3. */
static int
mm_reject_reason_defined(uint8_t reason) {
    return reason <= 3u;
}

/* Table A.73 reserves KSG numbers 4 to 7; 8 to 15 are proprietary algorithms. */
static int
mm_ksg_defined(uint8_t ksg) {
    return ksg < 4u || ksg > 7u;
}

/* Table A.100a groupings 0 to 9 split the SCKs into this many subsets; table A.100b numbers them from 1. */
static int
mm_sck_subset_defined(uint8_t grouping, uint8_t subset) {
    static const uint8_t max_subsets[10] = {1, 2, 3, 4, 5, 6, 7, 10, 15, 30};
    return grouping <= 9u && subset >= 1u && subset <= max_subsets[grouping];
}

/* Table A.43 CCK location area information. */
static int
mm_read_cck_location(tetra_bit_reader* r, tetra_mm_cck_info_t* cck) {
    cck->la_type = mm_u8(r, 2);
    switch (cck->la_type) {
        case 1: /* Table A.76 list; table A.81 reserves an empty one. */
            cck->la_count = mm_u8(r, 4);
            for (uint8_t i = 0; i < cck->la_count; i++) {
                cck->la[i] = mm_u16(r, 14);
            }
            return cck->la_count != 0;
        case 2: /* Tables A.74 and A.75. */
            cck->la_mask = mm_u16(r, 14);
            cck->la_selector = mm_u16(r, 14);
            return 1;
        case 3: /* Table A.77: the high value is above the low value. */
            cck->la_low = mm_u16(r, 14);
            cck->la_high = mm_u16(r, 14);
            return cck->la_high > cck->la_low;
        default: return 1; /* All location areas. */
    }
}

/* Table A.42 CCK information, in CCK Provide and NEWCELL. */
static int
mm_read_cck_info(tetra_bit_reader* r, tetra_mm_cck_info_t* cck) {
    cck->id = mm_u16(r, 16);
    cck->key_type = mm_u8(r, 1);
    mm_read_octets(r, cck->sealed, sizeof cck->sealed);
    if (!mm_read_cck_location(r, cck)) {
        return 0;
    }
    cck->future = mm_u8(r, 1);
    if (cck->future) {
        mm_read_octets(r, cck->future_sealed, sizeof cck->future_sealed);
    }
    /* The future key flag is always false when the key type flag says future. */
    return !(cck->key_type && cck->future);
}

/* Table A.9 CCK Provide. */
static int
mm_otar_cck_provide(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->cck_provision = mm_u8(r, 1);
    if (o->cck_provision && !mm_read_cck_info(r, &o->cck)) {
        return 0;
    }
    return mm_read_type34_tail(r);
}

/* Table A.30 NEWCELL: the DCK forwarding result, then the CCK Provide body. */
static int
mm_otar_newcell(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->dck_forwarding_result = mm_u8(r, 1);
    return mm_otar_cck_provide(r, o);
}

/* Acknowledgement flag, explicit response and max response timer value (tables A.36, A.59a and A.77a). */
static void
mm_read_response_request(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->ack = mm_u8(r, 1);
    o->explicit_response = mm_u8(r, 1);
    o->max_response_timer = mm_u16(r, 16);
}

/* Tables A.12 and A.16: the response request, then the session key flag and the random seed (individual session
 * key) or GSKO-VN (group session key) it selects. */
static int
mm_read_provide_header(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_response_request(r, o);
    o->session_key = mm_u8(r, 1);
    if (o->session_key) {
        o->gsko_version = mm_u16(r, 16);
    } else {
        mm_read_octets(r, o->random_seed, sizeof o->random_seed);
    }
    /* Without an acknowledgement request the explicit response position is a reserved zero bit. */
    return o->ack || !o->explicit_response;
}

/* Table A.16 SCK Provide. */
static int
mm_otar_sck_provide(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    if (!mm_read_provide_header(r, o)) {
        return 0;
    }
    o->key_count = mm_u8(r, 3);
    for (uint8_t i = 0; i < o->key_count; i++) {
        /* Table A.95: SCKN, SCK-VN, SCK use, a reserved zero bit and the sealed SCK. */
        o->sck_number[i] = mm_u8(r, 5);
        o->sck_version[i] = mm_u16(r, 16);
        o->sck_use[i] = mm_u8(r, 1);
        if (mm_u8(r, 1) != 0) {
            return 0;
        }
        mm_read_octets(r, o->sck_sealed[i], sizeof o->sck_sealed[i]);
    }
    o->ksg = mm_u8(r, 4);
    o->retry_interval = mm_u8(r, 3);
    return mm_ksg_defined(o->ksg) && mm_read_address_tail(r, &o->address);
}

/* Table A.19 SCK Reject. */
static int
mm_otar_sck_reject(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->key_count = mm_u8(r, 3);
    for (uint8_t i = 0; i < o->key_count; i++) {
        /* Table A.99a SCK rejected. */
        o->reject_reason[i] = mm_u8(r, 3);
        o->sck_number[i] = mm_u8(r, 5);
        if (!mm_reject_reason_defined(o->reject_reason[i])) {
            return 0;
        }
    }
    o->retry_interval = mm_u8(r, 3);
    return mm_read_address_tail(r, &o->address);
}

/* Table A.12 note 4: GCKs provided for one GSSI all carry its GCKN and differ in GCK-VN. */
static int
mm_gck_group_consistent(const tetra_mm_otar_t* o) {
    for (uint8_t i = 1; i < o->key_count; i++) {
        if (o->gck_number[i] != o->gck_number[0]) {
            return 0;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (o->gck_version[i] == o->gck_version[j]) {
                return 0;
            }
        }
    }
    return 1;
}

/* Table A.12 GCK Provide. */
static int
mm_otar_gck_provide(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    if (!mm_read_provide_header(r, o)) {
        return 0;
    }
    o->key_count = mm_u8(r, 3);
    for (uint8_t i = 0; i < o->key_count; i++) {
        /* Table A.62 GCK key and identifier. */
        o->gck_number[i] = mm_u16(r, 16);
        o->gck_version[i] = mm_u16(r, 16);
        mm_read_octets(r, o->gck_sealed[i], sizeof o->gck_sealed[i]);
    }
    o->ksg = mm_u8(r, 4);
    o->group_association = mm_u8(r, 1);
    if (o->group_association) {
        o->gssi = tetra_bits_read(r, 24);
    }
    o->retry_interval = mm_u8(r, 3);
    if (!mm_ksg_defined(o->ksg) || (o->group_association && !mm_gck_group_consistent(o))) {
        return 0;
    }
    return mm_read_address_tail(r, &o->address);
}

/* Table A.15 GCK Reject. */
static int
mm_otar_gck_reject(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->key_count = mm_u8(r, 3);
    if (o->key_count == 0) {
        return 0; /* Table A.79d reserves zero. */
    }
    for (uint8_t i = 0; i < o->key_count; i++) {
        /* Table A.63b GCK rejected: the GCKN, or the GSSI when the group association says so. */
        o->reject_reason[i] = mm_u8(r, 3);
        o->gck_reject_group_association[i] = mm_u8(r, 1);
        if (o->gck_reject_group_association[i]) {
            o->gck_reject_gssi[i] = tetra_bits_read(r, 24);
        } else {
            o->gck_number[i] = mm_u16(r, 16);
        }
        if (!mm_reject_reason_defined(o->reject_reason[i])) {
            return 0;
        }
    }
    o->retry_interval = mm_u8(r, 3);
    return mm_read_address_tail(r, &o->address);
}

/* Table A.80 number of groups: 0 is reserved, 1 to 30 count the GSSIs, and 31 gives a range as its lowest and highest
 * GSSI (table A.24 note 1). */
static int
mm_read_group_list(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->group_count = mm_u8(r, 5);
    o->group_is_range = (uint8_t)(o->group_count == 31u);
    o->group_value_count = o->group_is_range ? 2u : o->group_count;
    for (uint8_t i = 0; i < o->group_value_count; i++) {
        o->group_gssi[i] = tetra_bits_read(r, 24);
    }
    if (o->group_count == 0) {
        return 0;
    }
    return !o->group_is_range || o->group_gssi[1] > o->group_gssi[0];
}

/* Table A.24 KEY ASSOCIATE demand. */
static int
mm_otar_key_associate(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_response_request(r, o);
    if (!o->ack && o->explicit_response) {
        return 0;
    }
    o->key_association_type = mm_u8(r, 1);
    if (o->key_association_type) {
        /* Table A.64: GCKN 0 to 65535, or 65536 for no GCKN; higher values are reserved. */
        o->gck_select = tetra_bits_read(r, 17);
        if (o->gck_select > 0x10000u) {
            return 0;
        }
    } else {
        /* Table A.100 SCK select number; table A.100a groupings 0 to 9. */
        o->sck_select = mm_u8(r, 6);
        o->sck_subset_grouping = mm_u8(r, 4);
        if (o->sck_select > 32u || o->sck_subset_grouping > 9u) {
            return 0;
        }
    }
    return mm_read_group_list(r, o) && mm_read_address_tail(r, &o->address);
}

/* Table A.20 GSKO Provide. */
static int
mm_otar_gsko_provide(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_octets(r, o->random_seed, sizeof o->random_seed);
    o->gsko_version = mm_u16(r, 16);
    mm_read_octets(r, o->gsko_sealed, sizeof o->gsko_sealed);
    o->gssi = tetra_bits_read(r, 24);
    return mm_read_address_tail(r, &o->address);
}

/* Table A.23 GSKO Reject. */
static int
mm_otar_gsko_reject(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->reject_reason[0] = mm_u8(r, 3);
    o->gssi = tetra_bits_read(r, 24);
    o->retry_interval = mm_u8(r, 3);
    return mm_reject_reason_defined(o->reject_reason[0]) && mm_read_address_tail(r, &o->address);
}

/* Table A.27f KEY DELETE demand, by key delete type (table A.71a). */
static int
mm_otar_key_delete(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->key_delete_type = mm_u8(r, 3);
    switch (o->key_delete_type) {
        case 0: /* Individual SCKs. */
        case 1: /* Members of a KAG. */
            o->key_delete_sck_count = mm_u8(r, 5);
            for (uint8_t i = 0; i < o->key_delete_sck_count; i++) {
                o->key_delete_sck_number[i] = mm_u8(r, 5);
            }
            break;
        case 2: /* SCK subset. */
            o->key_delete_sck_grouping = mm_u8(r, 4);
            o->key_delete_sck_subset = mm_u8(r, 5);
            if (!mm_sck_subset_defined(o->key_delete_sck_grouping, o->key_delete_sck_subset)) {
                return 0;
            }
            break;
        case 4: /* Individual GCKs: the PDU table gives the count 4 bits, although table A.79a lists 5. */
            o->key_delete_gck_count = mm_u8(r, 4);
            for (uint8_t i = 0; i < o->key_delete_gck_count; i++) {
                o->key_delete_gck_number[i] = mm_u16(r, 16);
            }
            break;
        case 7: return 0; /* The key delete extension belongs to the uplink result only. */
        default: break;   /* All SCKs, all GCKs, or the GSKO. */
    }
    return mm_read_address_tail(r, &o->address);
}

/* Table A.27h KEY STATUS demand, by key status type (table A.71b). */
static int
mm_otar_key_status(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_response_request(r, o);
    if (!o->ack && o->explicit_response) {
        return 0;
    }
    o->key_status_type = mm_u8(r, 3);
    switch (o->key_status_type) {
        case 0: /* Individual SCK. */ o->key_status_sck_number = mm_u8(r, 5); break;
        case 1: /* SCK subset. */
            o->key_status_sck_grouping = mm_u8(r, 4);
            o->key_status_sck_subset = mm_u8(r, 5);
            if (!mm_sck_subset_defined(o->key_status_sck_grouping, o->key_status_sck_subset)) {
                return 0;
            }
            break;
        case 3: /* Individual GCK. */ o->key_status_gck_number = mm_u16(r, 16); break;
        case 6: /* Reject is a response value. */
        case 7: /* Reserved. */ return 0;
        default: break; /* All SCKs, all GCKs, or the GSKO. */
    }
    return mm_read_address_tail(r, &o->address);
}

/* Table A.30a CMG GTSI PROVIDE. */
static int
mm_otar_cmg_gtsi(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->gssi = tetra_bits_read(r, 24);
    return mm_read_address_tail(r, &o->address);
}

/* Number of SCKs changed, then the subset it selects when zero or that many SCK data elements (tables A.26, A.27d,
 * A.82 and A.93). */
static int
mm_read_sck_set(tetra_bit_reader* r, tetra_mm_sck_set_t* set, int subset_allowed) {
    set->count = mm_u8(r, 4);
    if (set->count == 0) {
        set->grouping = mm_u8(r, 4);
        set->subset = mm_u8(r, 5);
        set->subset_vn = mm_u16(r, 16);
        return subset_allowed;
    }
    for (uint8_t i = 0; i < set->count; i++) {
        set->number[i] = mm_u8(r, 5);
        set->version[i] = mm_u16(r, 16);
    }
    return 1;
}

/* Time type and the time it selects (table A.111). */
static void
mm_read_time(tetra_bit_reader* r, tetra_mm_time_t* t) {
    t->type = mm_u8(r, 2);
    if (t->type == 0) {
        t->slot = mm_u8(r, 2);
        t->frame = mm_u8(r, 5);
        t->multiframe = mm_u8(r, 6);
        t->hyperframe = mm_u16(r, 16);
    } else if (t->type == 1) {
        t->network_time = tetra_bits_read64(r, 48);
    }
}

/* Table A.27d DM-SCK ACTIVATE DEMAND. The MNI of the DMO network is a mandatory type 1 element. */
static int
mm_otar_dm_sck_activate(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->dm_sck_ack = mm_u8(r, 1);
    if (!mm_read_sck_set(r, &o->dm_sck, 1)
        || (o->dm_sck.count == 0 && !mm_sck_subset_defined(o->dm_sck.grouping, o->dm_sck.subset))) {
        return 0;
    }
    mm_read_time(r, &o->dm_sck_time);
    mm_read_address(r, &o->address);
    return tetra_bits_ok(r);
}

typedef struct {
    const char* name;
    int (*parse)(tetra_bit_reader* r, tetra_mm_otar_t* o);
} mm_otar_subtype;

/* Table A.85 downlink OTAR sub-types. Sub-type 1 has no downlink PDU table; 14 and 15 are reserved. */
static const mm_otar_subtype k_otar_subtypes[16] = {
    {"CCK PROVIDE", mm_otar_cck_provide},
    {"CCK REJECT", NULL},
    {"SCK PROVIDE", mm_otar_sck_provide},
    {"SCK REJECT", mm_otar_sck_reject},
    {"GCK PROVIDE", mm_otar_gck_provide},
    {"GCK REJECT", mm_otar_gck_reject},
    {"KEY ASSOCIATE DEMAND", mm_otar_key_associate},
    {"NEWCELL", mm_otar_newcell},
    {"GSKO PROVIDE", mm_otar_gsko_provide},
    {"GSKO REJECT", mm_otar_gsko_reject},
    {"KEY DELETE DEMAND", mm_otar_key_delete},
    {"KEY STATUS DEMAND", mm_otar_key_status},
    {"CMG GTSI PROVIDE", mm_otar_cmg_gtsi},
    {"DM-SCK ACTIVATE DEMAND", mm_otar_dm_sck_activate},
    {"reserved", NULL},
    {"reserved", NULL},
};

/* Summary detail for SCK and GCK Provide and Reject: how many are carried, and the algorithm of a Provide. */
static void
mm_otar_detail(const tetra_mm_otar_t* o, char* out, size_t cap) {
    out[0] = '\0';
    if (o->subtype < 2u || o->subtype > 5u) {
        return;
    }
    const char* alg = "";
    if (o->subtype == 2u || o->subtype == 4u) {
        const char* const tea = tetra_air_ksg_name(o->ksg);
        alg = tea ? tea : "proprietary";
    }
    DSD_SNPRINTF(out, cap, " count=%u%s%s", (unsigned)o->key_count, alg[0] ? " alg=" : "", alg);
}

/* SCK and GCK Provide name the algorithm (table A.73) for the network in their address extension, or for the
 * serving network when they carry none (tables A.12 and A.16, address extension notes). */
static void
mm_otar_remember_ksg(const mm_pdu* pdu, const tetra_mm_otar_t* o) {
    if (o->subtype != 2u && o->subtype != 4u) {
        return;
    }
    if (o->address.valid) {
        (void)tetra_air_ksg_remember(pdu->state, o->address.mcc, o->address.mnc, o->ksg);
    } else if (pdu->ts->net_known) {
        (void)tetra_air_ksg_remember(pdu->state, pdu->ts->mcc, pdu->ts->mnc, o->ksg);
    }
}

static void
mm_parse_otar(mm_pdu* pdu) {
    tetra_mm_otar_t o;
    DSD_MEMSET(&o, 0, sizeof o);
    o.subtype = mm_u8(&pdu->r, 4);
    const mm_otar_subtype* const sub = &k_otar_subtypes[o.subtype];
    if (!tetra_bits_ok(&pdu->r)) {
        mm_log_invalid(pdu, "D-OTAR");
        return;
    }
    if (!sub->parse) {
        TETRA_LOG_PAYLOAD(pdu->ts, "[TETRA MM D-OTAR] CC=%d %s sub-type %u not decoded\n", pdu->cc, sub->name,
                          (unsigned)o.subtype);
        return;
    }
    if (!sub->parse(&pdu->r, &o) || !tetra_bits_ok(&pdu->r)) {
        mm_log_invalid(pdu, "D-OTAR");
        return;
    }
    char detail[48];
    mm_otar_detail(&o, detail, sizeof detail);
    DSD_FPRINTF(stderr, "[TETRA MM D-OTAR] CC=%d %s%s\n", pdu->cc, sub->name, detail);
    o.valid = 1;
    pdu->ts->mm_otar = o;
    mm_otar_remember_ksg(pdu, &o);
}

/* EN 300 392-7 table A.26: D-CK CHANGE DEMAND. */

/* Table A.26 GCK change: the number of GCKs changed (zero is reserved) and that many GCK data elements (table A.61). */
static int
mm_read_gck_change(tetra_bit_reader* r, tetra_mm_ck_change_t* c) {
    c->gck_count = mm_u8(r, 4);
    for (uint8_t i = 0; i < c->gck_count; i++) {
        c->gck_number[i] = mm_u16(r, 16);
        c->gck_version[i] = mm_u16(r, 16);
    }
    return c->gck_count != 0;
}

/* The elements that depend on the key change type (table A.71). An SCK subset is selected only for DMO use. */
static int
mm_read_change_target(tetra_bit_reader* r, tetra_mm_ck_change_t* c) {
    switch (c->key_type) {
        case 0: c->sck_use = mm_u8(r, 1); return mm_read_sck_set(r, &c->sck, c->sck_use);
        case 1: c->cck_id = mm_u16(r, 16); return 1;
        case 2: return mm_read_gck_change(r, c);
        case 3:
            c->cck_id = mm_u16(r, 16);
            c->gck_vn = mm_u16(r, 16);
            return 1;
        case 4: c->gck_vn = mm_u16(r, 16); return 1;
        case 5: return 1;
        default: return 0; /* 6 and 7 are reserved. */
    }
}

static void
mm_parse_ck_change(mm_pdu* pdu) {
    static const char* const targets[6] = {"SCK", "CCK", "GCK", "CCK and GCK", "all GCKs", "no cipher key"};
    static const char* const times[4] = {"at absolute IV", "at network time", "from next multiframe", "already in use"};
    tetra_bit_reader* const r = &pdu->r;
    tetra_mm_ck_change_t c;
    DSD_MEMSET(&c, 0, sizeof c);
    c.ack = mm_u8(r, 1);
    c.security_class = mm_u8(r, 2);
    c.key_type = mm_u8(r, 3);
    int ok = mm_read_change_target(r, &c);
    mm_read_time(r, &c.time);
    if (!ok || !tetra_bits_ok(r)) {
        mm_log_invalid(pdu, "D-CK-CHANGE");
        return;
    }
    const char* const target = targets[c.key_type];
    char transition[32] = "";
    if (c.security_class != 0) {
        /* Table A.45: values 1 to 3 move the cell to that security class. */
        DSD_SNPRINTF(transition, sizeof transition, " to security class %u", (unsigned)c.security_class);
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-CK-CHANGE] CC=%d %s %s%s\n", pdu->cc, target, times[c.time.type], transition);
    c.valid = 1;
    pdu->ts->mm_ck_change = c;
}

/* EN 300 392-7 tables A.31 and A.32: D-DISABLE and D-ENABLE. These change whether the addressed MS gives service, so
 * the whole PDU, conditional identities and authentication challenge included, has to be there. */

static int
mm_read_enable_disable(tetra_bit_reader* r, tetra_mm_enable_disable_t* e) {
    e->intent = mm_u8(r, 1);
    if (!e->is_enable) {
        e->permanent = mm_u8(r, 1); /* Table A.52 disabling type. */
    }
    e->equipment = mm_u8(r, 1);
    if (e->equipment) {
        e->tei = tetra_bits_read64(r, 60);
    }
    e->subscription = mm_u8(r, 1);
    if (e->subscription) {
        e->mcc = mm_u16(r, 10);
        e->mnc = mm_u16(r, 14);
        e->ssi = tetra_bits_read(r, 24);
    }
    if (!tetra_bits_present(r)) {
        return tetra_bits_ok(r);
    }
    /* The 160-bit authentication challenge (table A.37) is the one type 2 element. */
    if (tetra_bits_present(r)) {
        mm_read_octets(r, e->auth_challenge, sizeof e->auth_challenge);
        e->auth_valid = 1;
    }
    return tetra_bits_skip_type34(r);
}

static void
mm_parse_enable_disable(mm_pdu* pdu, uint8_t is_enable) {
    const char* const name = is_enable ? "D-ENABLE" : "D-DISABLE";
    tetra_mm_enable_disable_t e;
    DSD_MEMSET(&e, 0, sizeof e);
    e.is_enable = is_enable;
    if (!mm_read_enable_disable(&pdu->r, &e)) {
        mm_log_invalid(pdu, name);
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM %s] CC=%d intent=%u equipment=%u subscription=%u%s\n", name, pdu->cc,
                (unsigned)e.intent, (unsigned)e.equipment, (unsigned)e.subscription, e.permanent ? " permanent" : "");
    e.valid = 1;
    pdu->ts->mm_enable_disable = e;
}

static void
mm_parse_disable(mm_pdu* pdu) {
    mm_parse_enable_disable(pdu, 0);
}

static void
mm_parse_enable(mm_pdu* pdu) {
    mm_parse_enable_disable(pdu, 1);
}

/* EN 300 392-2 clause 16.9.2: location update and group identity PDUs. */

/* Table 16.11 D-LOCATION UPDATE ACCEPT: the location update accept type, then the type 2 SSI, address extension,
 * subscriber class, energy saving information and SCCH information; every type 3/4 element is skipped. */
static void
mm_parse_lu_accept(mm_pdu* pdu) {
    static const uint8_t width[5] = {24, 24, 16, 14, 6};
    uint32_t value[5] = {0};
    uint8_t present[5] = {0};
    tetra_mm_lu_accept_t a;
    DSD_MEMSET(&a, 0, sizeof a);
    a.type = mm_u8(&pdu->r, 3);
    if (!mm_read_optional(&pdu->r, width, 5, value, present)) {
        mm_log_invalid(pdu, "D-LU-ACCEPT");
        return;
    }
    a.ssi = value[0];
    a.ssi_valid = present[0];
    if (present[1]) {
        mm_address_from(value[1], &a.address);
    }
    a.subscriber_class = (uint16_t)value[2];
    a.subscriber_class_valid = present[2];
    a.energy_saving = (uint16_t)value[3];
    a.energy_saving_valid = present[3];
    a.scch = (uint8_t)value[4];
    a.scch_valid = present[4];
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d type=%u\n", pdu->cc, (unsigned)a.type);
    a.valid = 1;
    pdu->ts->mm_lu_accept = a;
}

/* Cipher control and the ciphering parameters it announces (EN 300 392-2 tables 16.12 and 16.13). */
static void
mm_read_cipher(tetra_bit_reader* r, uint8_t* control, uint16_t* params) {
    *control = mm_u8(r, 1);
    if (*control) {
        *params = mm_u16(r, 10);
    }
}

/* Table 16.12 D-LOCATION UPDATE COMMAND. */
static void
mm_parse_lu_command(mm_pdu* pdu) {
    tetra_mm_lu_command_t c;
    DSD_MEMSET(&c, 0, sizeof c);
    c.group_report = mm_u8(&pdu->r, 1);
    mm_read_cipher(&pdu->r, &c.cipher_control, &c.cipher_params);
    if (!mm_read_address_tail(&pdu->r, &c.address)) {
        mm_log_invalid(pdu, "D-LU-COMMAND");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-COMMAND] CC=%d group_report=%u cipher=%u\n", pdu->cc, (unsigned)c.group_report,
                (unsigned)c.cipher_control);
    c.valid = 1;
    pdu->ts->mm_lu_command = c;
}

/* Table 16.13 D-LOCATION UPDATE REJECT. */
static void
mm_parse_lu_reject(mm_pdu* pdu) {
    tetra_mm_lu_reject_t rej;
    DSD_MEMSET(&rej, 0, sizeof rej);
    rej.type = mm_u8(&pdu->r, 3);
    rej.cause = mm_u8(&pdu->r, 5);
    mm_read_cipher(&pdu->r, &rej.cipher_control, &rej.cipher_params);
    if (!mm_read_address_tail(&pdu->r, &rej.address)) {
        mm_log_invalid(pdu, "D-LU-REJECT");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-REJECT] CC=%d type=%u cause=%u cipher=%u\n", pdu->cc, (unsigned)rej.type,
                (unsigned)rej.cause, (unsigned)rej.cipher_control);
    rej.valid = 1;
    pdu->ts->mm_lu_reject = rej;
}

/* Table 16.14 D-LOCATION UPDATE PROCEEDING: the (V)ASSI and the MNI are both type 1 elements. */
static void
mm_parse_lu_proceeding(mm_pdu* pdu) {
    tetra_mm_lu_proceeding_t p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.ssi = tetra_bits_read(&pdu->r, 24);
    mm_read_address(&pdu->r, &p.address);
    if (!mm_read_type34_tail(&pdu->r)) {
        mm_log_invalid(pdu, "D-LU-PROCEEDING");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-PROCEEDING] CC=%d SSI=%u MCC=%u MNC=%u\n", pdu->cc, (unsigned)p.ssi,
                (unsigned)p.address.mcc, (unsigned)p.address.mnc);
    p.valid = 1;
    pdu->ts->mm_lu_proceeding = p;
}

typedef struct {
    tetra_mm_group_entry_t entry[63];
    uint8_t count;
} mm_group_entries;

/* One group identity of table 16.54: attachment (lifetime and class of usage) or detachment (reason), then the
 * address type and the GSSI, address extension and (V)GSSI it selects. */
static void
mm_read_group_entry(tetra_bit_reader* r, tetra_mm_group_entry_t* e) {
    e->action = mm_u8(r, 1);
    if (e->action == 0) {
        e->attachment_lifetime = mm_u8(r, 2);
        e->class_of_usage = mm_u8(r, 3);
    } else {
        e->detachment_reason = mm_u8(r, 2);
    }
    e->address_type = mm_u8(r, 2);
    if (e->address_type != 2u) {
        e->gssi = tetra_bits_read(r, 24);
    }
    if (e->address_type & 1u) {
        mm_read_address(r, &e->address);
    }
    if (e->address_type >= 2u) {
        e->vgssi = tetra_bits_read(r, 24);
    }
}

/* Type 4 group identity downlink element: a 6-bit number of repeated elements, then that many group identities,
 * filling exactly @p length bits. */
static int
mm_read_group_downlink(const tetra_bit_reader* r, int length, mm_group_entries* entries) {
    tetra_bit_reader element = tetra_bits_reader(r->bits, r->pos + length, r->pos);
    const uint8_t count = mm_u8(&element, 6);
    if ((unsigned)entries->count + count > 63u) {
        return 0;
    }
    for (uint8_t i = 0; i < count; i++) {
        mm_read_group_entry(&element, &entries->entry[entries->count++]);
    }
    return tetra_bits_ok(&element) && tetra_bits_left(&element) == 0;
}

/* The M-bit chain of D-ATTACH/DETACH GROUP IDENTITY (table 16.1): group identity downlink elements (identifier 7,
 * table 16.95) are decoded, the other type 3/4 elements skipped. */
static int
mm_read_group_identity_chain(tetra_bit_reader* r, mm_group_entries* entries) {
    while (tetra_bits_read(r, 1) != 0) {
        const uint32_t identifier = tetra_bits_read(r, 4);
        const int length = (int)tetra_bits_read(r, 11);
        if (!tetra_bits_ok(r) || length == 0 || length > tetra_bits_left(r)) {
            return 0;
        }
        if (identifier == 7u && !mm_read_group_downlink(r, length, entries)) {
            return 0;
        }
        (void)tetra_bits_skip(r, length);
    }
    return tetra_bits_ok(r);
}

/* Table 16.1 D-ATTACH/DETACH GROUP IDENTITY. */
static void
mm_parse_group_identity(mm_pdu* pdu) {
    tetra_bit_reader* const r = &pdu->r;
    mm_group_entries entries;
    DSD_MEMSET(&entries, 0, sizeof entries);
    const uint8_t report = mm_u8(r, 1);
    const uint8_t ack_request = mm_u8(r, 1);
    const uint8_t mode = mm_u8(r, 1);
    const int optional = tetra_bits_present(r);
    if (!tetra_bits_ok(r) || (optional && !mm_read_group_identity_chain(r, &entries))) {
        mm_log_invalid(pdu, "D-GROUP-IDENTITY");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-GROUP-IDENTITY] CC=%d report=%u ack_request=%u detach_all=%u groups=%u\n", pdu->cc,
                (unsigned)report, (unsigned)ack_request, (unsigned)mode, (unsigned)entries.count);
    tetra_state_t* const ts = pdu->ts;
    ts->mm_group_identity_report = report;
    ts->mm_group_identity_ack_request = ack_request;
    ts->mm_group_identity_attach_detach_mode = mode;
    ts->mm_group_entry_count = entries.count;
    DSD_MEMCPY(ts->mm_group_entry, entries.entry, sizeof ts->mm_group_entry);
    ts->mm_addr_type = ack_request; /* Shown by tetra_channel_info.c. */
    ts->mm_group_identity_valid = 1;
}

/* Table 16.2 D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT: accept/reject (table 16.44), a reserved bit, and type
 * 3/4 elements that are skipped. */
static void
mm_parse_group_ack(mm_pdu* pdu) {
    const uint8_t rejected = mm_u8(&pdu->r, 1);
    (void)tetra_bits_skip(&pdu->r, 1);
    if (!mm_read_type34_tail(&pdu->r)) {
        mm_log_invalid(pdu, "D-GROUP-ACK");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-GROUP-ACK] CC=%d %s\n", pdu->cc,
                rejected ? "at least one rejected" : "all accepted");
    pdu->ts->mm_group_ack_result = rejected;
    pdu->ts->mm_group_ack_valid = 1;
}

/* Table 16.3 D-MM STATUS: the 6-bit status downlink. */
static void
mm_parse_status(mm_pdu* pdu) {
    const uint8_t status = mm_u8(&pdu->r, 6);
    if (!tetra_bits_ok(&pdu->r)) {
        mm_log_invalid(pdu, "D-MM-STATUS");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-MM-STATUS] CC=%d status=%u (%s)\n", pdu->cc, (unsigned)status,
                tetra_mm_status_name(status));
    pdu->ts->mm_status_code = status;
    pdu->ts->mm_status_valid = 1;
}

/* Table 16.27 MM PDU/FUNCTION NOT SUPPORTED: the not-supported PDU type. */
static void
mm_parse_function_not_supported(mm_pdu* pdu) {
    const uint8_t type = mm_u8(&pdu->r, 4);
    if (!tetra_bits_ok(&pdu->r)) {
        mm_log_invalid(pdu, "FUNCTION-NOT-SUPPORTED");
        return;
    }
    DSD_FPRINTF(stderr, "[TETRA MM FUNCTION-NOT-SUPPORTED] CC=%d pdu_type=%u\n", pdu->cc, (unsigned)type);
    pdu->ts->mm_fns_pdu_type = type;
    pdu->ts->mm_fns_valid = 1;
}

/* EN 300 392-2 table 16.75 downlink PDU types; 8, 13 and 14 are reserved. */
static void (*const k_mm_parsers[16])(mm_pdu*) = {
    mm_parse_otar,                   /* 0 D-OTAR */
    mm_parse_authentication,         /* 1 D-AUTHENTICATION */
    mm_parse_ck_change,              /* 2 D-CK CHANGE DEMAND */
    mm_parse_disable,                /* 3 D-DISABLE */
    mm_parse_enable,                 /* 4 D-ENABLE */
    mm_parse_lu_accept,              /* 5 D-LOCATION UPDATE ACCEPT */
    mm_parse_lu_command,             /* 6 D-LOCATION UPDATE COMMAND */
    mm_parse_lu_reject,              /* 7 D-LOCATION UPDATE REJECT */
    NULL,                            /* 8 reserved */
    mm_parse_lu_proceeding,          /* 9 D-LOCATION UPDATE PROCEEDING */
    mm_parse_group_identity,         /* 10 D-ATTACH/DETACH GROUP IDENTITY */
    mm_parse_group_ack,              /* 11 D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT */
    mm_parse_status,                 /* 12 D-MM STATUS */
    NULL,                            /* 13 reserved */
    NULL,                            /* 14 reserved */
    mm_parse_function_not_supported, /* 15 MM PDU/FUNCTION NOT SUPPORTED */
};

void
tetra_mm_dispatch(const uint8_t* bits, int nbits, int cc, const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !bits) {
        return;
    }
    mm_pdu pdu = {tetra_bits_reader(bits, nbits, 0), state, ts, cc};
    const uint8_t type = mm_u8(&pdu.r, 4);
    if (!tetra_bits_ok(&pdu.r)) {
        return;
    }
    if (k_mm_parsers[type]) {
        k_mm_parsers[type](&pdu);
    } else {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MM] CC=%d reserved PDU type %u (%d bits)\n", cc, (unsigned)type, nbits);
    }
}
