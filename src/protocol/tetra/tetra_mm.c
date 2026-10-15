// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Mobility Management (MM) downlink PDU decoder: EN 300 392-2 clause 16, and the authentication, OTAR, cipher
 * key change and enable/disable PDUs of EN 300 392-7 annex A that share the MM protocol discriminator.
 *
 * tetra_mm_decode() reads a PDU through a bounded bit reader into a tetra_mm_pdu, including the O-bit, P-bits and the
 * M-bit chain of EN 300 392-2 annex E.1.1. tetra_mm_dispatch() decodes through it and prints, notes or remembers what
 * the PDU carries only when the whole PDU was there: a truncated or malformed PDU acts on nothing.
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
#include "tetra_calls.h"
#include "tetra_log.h"
#include "tetra_mm_pdu.h"
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

/* What acting on a complete MM PDU needs. */
typedef struct {
    dsd_opts* opts; /* may be NULL */
    dsd_state* state;
    tetra_state_t* ts;
    int cc;
    int nbits; /* bits in the PDU, for the truncation notes */
} mm_ctx;

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
mm_log_invalid(const mm_ctx* ctx, const char* name) {
    TETRA_LOG_PAYLOAD(ctx->ts, "[TETRA MM %s] CC=%d (invalid/truncated: %d bits)\n", name, ctx->cc, ctx->nbits);
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

/* Table A.2 REJECT: the reject reason, of which table A.38 defines only 0 (authentication not supported), then the
 * O-bit that a PDU without optional elements still carries (EN 300 392-2 annex E.1.1 note 4). */
static int
mm_auth_reject(tetra_bit_reader* r, tetra_mm_auth_t* a) {
    a->reject_reason = mm_u8(r, 3);
    return a->reject_reason == 0 && mm_read_type34_tail(r);
}

static int
mm_decode_authentication(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    typedef int (*reader)(tetra_bit_reader*, tetra_mm_auth_t*);
    static const reader readers[4] = {mm_auth_demand, mm_auth_response, mm_auth_result, mm_auth_reject};
    tetra_mm_auth_t* const a = &pdu->auth;
    a->subtype = mm_u8(r, 2);
    return readers[a->subtype](r, a);
}

static void
mm_report_authentication(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    static const char* const names[4] = {"DEMAND", "RESPONSE", "RESULT", "REJECT"}; /* table A.40 */
    const tetra_mm_auth_t* const a = &pdu->auth;
    const char* outcome = "";
    if (a->subtype == 2) {
        outcome = a->result ? " successful" : " failed"; /* Table A.39. */
    }
    DSD_FPRINTF(stderr, "[TETRA MM D-AUTHENTICATION] CC=%d %s%s%s\n", ctx->cc, names[a->subtype & 3u], outcome,
                a->mutual ? " mutual" : "");
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

/* Table A.24 KEY ASSOCIATE demand. The explicit response is a type 1 element here, meaningful only with an
 * acknowledgement request (note 3). */
static int
mm_otar_key_associate(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_response_request(r, o);
    o->key_association_type = mm_u8(r, 1);
    if (o->key_association_type) {
        /* Table A.64: GCKN 0 to 65535, or 65536 for no GCKN; higher values are reserved. */
        o->gck_select = tetra_bits_read(r, 17);
        if (o->gck_select > 0x10000u) {
            return 0;
        }
    } else {
        /* Table A.100: SCKN, 32 for no SCKN and 33 for dissociation; table A.100a groupings 0 to 9. */
        o->sck_select = mm_u8(r, 6);
        o->sck_subset_grouping = mm_u8(r, 4);
        if (o->sck_select > 33u || o->sck_subset_grouping > 9u) {
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

/* Table A.27h KEY STATUS demand, by key status type (table A.71b). The explicit response is a type 1 element here,
 * meaningful only with an acknowledgement request (note 1). */
static int
mm_otar_key_status(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    mm_read_response_request(r, o);
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
        return subset_allowed && mm_sck_subset_defined(set->grouping, set->subset);
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

/* Table A.27d DM-SCK ACTIVATE DEMAND. The MNI of the DMO network is a mandatory type 1 element; the O-bit follows
 * it (EN 300 392-2 annex E.1.1 note 4). */
static int
mm_otar_dm_sck_activate(tetra_bit_reader* r, tetra_mm_otar_t* o) {
    o->dm_sck_ack = mm_u8(r, 1);
    if (!mm_read_sck_set(r, &o->dm_sck, 1)) {
        return 0;
    }
    mm_read_time(r, &o->dm_sck_time);
    mm_read_address(r, &o->address);
    return mm_read_type34_tail(r);
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
mm_otar_remember_ksg(const mm_ctx* ctx, const tetra_mm_otar_t* o) {
    if (o->subtype != 2u && o->subtype != 4u) {
        return;
    }
    if (o->address.valid) {
        (void)tetra_air_ksg_remember(ctx->state, o->address.mcc, o->address.mnc, o->ksg);
    } else if (ctx->ts->net_known) {
        (void)tetra_air_ksg_remember(ctx->state, ctx->ts->mcc, ctx->ts->mnc, o->ksg);
    }
}

/* 1 when the D-OTAR sub-type has a downlink layout this decoder reads. */
static int
mm_otar_subtype_decoded(uint8_t subtype) {
    return k_otar_subtypes[subtype & 15u].parse != NULL;
}

static int
mm_decode_otar(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_otar_t* const o = &pdu->otar;
    o->subtype = mm_u8(r, 4);
    const mm_otar_subtype* const sub = &k_otar_subtypes[o->subtype];
    return tetra_bits_ok(r) && sub->parse && sub->parse(r, o);
}

static void
mm_report_otar(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    const tetra_mm_otar_t* const o = &pdu->otar;
    char detail[48];
    mm_otar_detail(o, detail, sizeof detail);
    DSD_FPRINTF(stderr, "[TETRA MM D-OTAR] CC=%d %s%s\n", ctx->cc, k_otar_subtypes[o->subtype & 15u].name, detail);
    mm_otar_remember_ksg(ctx, o);
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

/* Table A.26 has no optional element, so the PDU ends with the O-bit (EN 300 392-2 annex E.1.1 note 4). */
static int
mm_decode_ck_change(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_ck_change_t* const c = &pdu->ck_change;
    c->ack = mm_u8(r, 1);
    c->security_class = mm_u8(r, 2);
    c->key_type = mm_u8(r, 3);
    const int ok = mm_read_change_target(r, c);
    mm_read_time(r, &c->time);
    return ok && mm_read_type34_tail(r);
}

static void
mm_report_ck_change(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    static const char* const targets[6] = {"SCK", "CCK", "GCK", "CCK and GCK", "all GCKs", "no cipher key"};
    static const char* const times[4] = {"at absolute IV", "at network time", "from next multiframe", "already in use"};
    const tetra_mm_ck_change_t* const c = &pdu->ck_change;
    char transition[32] = "";
    if (c->security_class != 0) {
        /* Table A.45: values 1 to 3 move the cell to that security class. */
        DSD_SNPRINTF(transition, sizeof transition, " to security class %u", (unsigned)c->security_class);
    }
    const char* const target = targets[c->key_type % 6u];
    DSD_FPRINTF(stderr, "[TETRA MM D-CK-CHANGE] CC=%d %s %s%s\n", ctx->cc, target, times[c->time.type & 3u],
                transition);
}

/* EN 300 392-7 tables A.31 and A.32: D-DISABLE and D-ENABLE. These change whether the addressed MS gives service, so
 * the whole PDU, conditional identities and authentication challenge included, has to be there. */

/* Enabling or disabling a terminal or a subscription is worth a line in the event history (tetra_calls_notice()): the
 * SSI and TEI it names, whether it is an intent or the confirmation, and a permanent disable. */
static void
mm_notice_enable_disable(const mm_ctx* ctx, const tetra_mm_enable_disable_t* e, const char* name) {
    char notice[128];
    int n = DSD_SNPRINTF(notice, sizeof notice, "%s %s;", name, e->intent ? "CONFIRM" : "INTENT");
    if (n > 0 && (size_t)n < sizeof notice && e->subscription) {
        n += DSD_SNPRINTF(notice + n, sizeof notice - (size_t)n, " SSI: %u;", (unsigned)e->ssi);
    }
    if (n > 0 && (size_t)n < sizeof notice && e->equipment) {
        n += DSD_SNPRINTF(notice + n, sizeof notice - (size_t)n, " TEI: %015llX;", (unsigned long long)e->tei);
    }
    if (n > 0 && (size_t)n < sizeof notice && e->permanent) {
        DSD_SNPRINTF(notice + n, sizeof notice - (size_t)n, " Permanent;");
    }
    tetra_calls_notice(ctx->opts, ctx->state, 1, 0U, e->subscription ? e->ssi : 0U, notice, NULL);
}

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

static int
mm_decode_enable_disable(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    pdu->enable_disable.is_enable = (uint8_t)(pdu->type == TETRA_MM_D_ENABLE);
    return mm_read_enable_disable(r, &pdu->enable_disable);
}

static void
mm_report_enable_disable(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    const tetra_mm_enable_disable_t* const e = &pdu->enable_disable;
    const char* const name = e->is_enable ? "D-ENABLE" : "D-DISABLE";
    DSD_FPRINTF(stderr, "[TETRA MM %s] CC=%d intent=%u equipment=%u subscription=%u%s\n", name, ctx->cc,
                (unsigned)e->intent, (unsigned)e->equipment, (unsigned)e->subscription,
                e->permanent ? " permanent" : "");
    mm_notice_enable_disable(ctx, e, name);
}

/* EN 300 392-2 clause 16.9.2: location update and group identity PDUs. */

/* Table 16.11 D-LOCATION UPDATE ACCEPT: the location update accept type, then the type 2 SSI, address extension,
 * subscriber class, energy saving information and SCCH information; every type 3/4 element is skipped. */
static int
mm_decode_lu_accept(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    static const uint8_t width[5] = {24, 24, 16, 14, 6};
    uint32_t value[5] = {0};
    uint8_t present[5] = {0};
    tetra_mm_lu_accept_t* const a = &pdu->lu_accept;
    a->type = mm_u8(r, 3);
    if (!mm_read_optional(r, width, 5, value, present)) {
        return 0;
    }
    a->ssi = value[0];
    a->ssi_valid = present[0];
    if (present[1]) {
        mm_address_from(value[1], &a->address);
    }
    a->subscriber_class = (uint16_t)value[2];
    a->subscriber_class_valid = present[2];
    a->energy_saving = (uint16_t)value[3];
    a->energy_saving_valid = present[3];
    a->scch = (uint8_t)value[4];
    a->scch_valid = present[4];
    return 1;
}

static void
mm_report_lu_accept(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d type=%u\n", ctx->cc, (unsigned)pdu->lu_accept.type);
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
static int
mm_decode_lu_command(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_lu_command_t* const c = &pdu->lu_command;
    c->group_report = mm_u8(r, 1);
    mm_read_cipher(r, &c->cipher_control, &c->cipher_params);
    return mm_read_address_tail(r, &c->address);
}

static void
mm_report_lu_command(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-COMMAND] CC=%d group_report=%u cipher=%u\n", ctx->cc,
                (unsigned)pdu->lu_command.group_report, (unsigned)pdu->lu_command.cipher_control);
}

/* Table 16.13 D-LOCATION UPDATE REJECT. */
static int
mm_decode_lu_reject(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_lu_reject_t* const rej = &pdu->lu_reject;
    rej->type = mm_u8(r, 3);
    rej->cause = mm_u8(r, 5);
    mm_read_cipher(r, &rej->cipher_control, &rej->cipher_params);
    return mm_read_address_tail(r, &rej->address);
}

static void
mm_report_lu_reject(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    const tetra_mm_lu_reject_t* const rej = &pdu->lu_reject;
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-REJECT] CC=%d type=%u cause=%u cipher=%u\n", ctx->cc, (unsigned)rej->type,
                (unsigned)rej->cause, (unsigned)rej->cipher_control);
}

/* Table 16.14 D-LOCATION UPDATE PROCEEDING: the (V)ASSI and the MNI are both type 1 elements. */
static int
mm_decode_lu_proceeding(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_lu_proceeding_t* const p = &pdu->lu_proceeding;
    p->ssi = tetra_bits_read(r, 24);
    mm_read_address(r, &p->address);
    return mm_read_type34_tail(r);
}

static void
mm_report_lu_proceeding(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    const tetra_mm_lu_proceeding_t* const p = &pdu->lu_proceeding;
    DSD_FPRINTF(stderr, "[TETRA MM D-LU-PROCEEDING] CC=%d SSI=%u MCC=%u MNC=%u\n", ctx->cc, (unsigned)p->ssi,
                (unsigned)p->address.mcc, (unsigned)p->address.mnc);
}

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

/* Type 4 group identity downlink element: a 6-bit number of repeated elements (1 to 63; annex E.1.1 reserves 0),
 * then that many group identities, filling exactly @p length bits. */
static int
mm_read_group_downlink(const tetra_bit_reader* r, int length, tetra_mm_group_identity_t* g) {
    tetra_bit_reader element = tetra_bits_reader(r->bits, r->pos + length, r->pos);
    const uint8_t count = mm_u8(&element, 6);
    if (count == 0 || (unsigned)g->count + count > 63u) {
        return 0;
    }
    for (uint8_t i = 0; i < count; i++) {
        mm_read_group_entry(&element, &g->entry[g->count++]);
    }
    return tetra_bits_ok(&element) && tetra_bits_left(&element) == 0;
}

/* The M-bit chain of D-ATTACH/DETACH GROUP IDENTITY (table 16.1): group identity downlink elements (identifier 7,
 * table 16.95) are decoded, the other type 3/4 elements skipped. */
static int
mm_read_group_identity_chain(tetra_bit_reader* r, tetra_mm_group_identity_t* g) {
    while (tetra_bits_read(r, 1) != 0) {
        const uint32_t identifier = tetra_bits_read(r, 4);
        const int length = (int)tetra_bits_read(r, 11);
        if (!tetra_bits_ok(r) || length == 0 || length > tetra_bits_left(r)) {
            return 0;
        }
        if (identifier == 7u && !mm_read_group_downlink(r, length, g)) {
            return 0;
        }
        (void)tetra_bits_skip(r, length);
    }
    return tetra_bits_ok(r);
}

/* Table 16.1 D-ATTACH/DETACH GROUP IDENTITY. */
static int
mm_decode_group_identity(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    tetra_mm_group_identity_t* const g = &pdu->group_identity;
    g->report = mm_u8(r, 1);
    g->ack_request = mm_u8(r, 1);
    g->detach_all = mm_u8(r, 1);
    const int optional = tetra_bits_present(r);
    return tetra_bits_ok(r) && (!optional || mm_read_group_identity_chain(r, g));
}

static void
mm_report_group_identity(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    const tetra_mm_group_identity_t* const g = &pdu->group_identity;
    DSD_FPRINTF(stderr, "[TETRA MM D-GROUP-IDENTITY] CC=%d report=%u ack_request=%u detach_all=%u groups=%u\n", ctx->cc,
                (unsigned)g->report, (unsigned)g->ack_request, (unsigned)g->detach_all, (unsigned)g->count);
}

/* Table 16.2 D-ATTACH/DETACH GROUP IDENTITY ACKNOWLEDGEMENT: accept/reject (table 16.44), a reserved bit, and type
 * 3/4 elements that are skipped. */
static int
mm_decode_group_ack(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    pdu->group_ack.rejected = mm_u8(r, 1);
    (void)tetra_bits_skip(r, 1);
    return mm_read_type34_tail(r);
}

static void
mm_report_group_ack(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    DSD_FPRINTF(stderr, "[TETRA MM D-GROUP-ACK] CC=%d %s\n", ctx->cc,
                pdu->group_ack.rejected ? "at least one rejected" : "all accepted");
}

/* Status downlink dependent information (tables 16.4 to 16.10): the type 1 bits after the status downlink, then the
 * widths of the type 2 elements; each sub-PDU ends with an optional proprietary element. */
typedef struct {
    uint8_t status;
    uint8_t type1_bits;
    uint8_t type2_count;
    uint8_t type2_width[2];
} mm_status_layout;

static const mm_status_layout k_status_layouts[] = {
    {1, 14, 0, {0, 0}}, /* Change of energy saving mode request: energy saving information. */
    {2, 14, 0, {0, 0}}, /* Change of energy saving mode response. */
    {3, 25, 1, {6, 0}}, /* Dual watch mode response: energy saving, result, reserved; SCCH information. */
    {4, 8, 2, {14, 6}}, /* Terminating dual watch mode response: reserved; energy saving and SCCH information. */
    {5, 25, 1, {6, 0}}, /* Change of dual watch mode request: energy saving, reason, reserved; SCCH information. */
    {7, 0, 0, {0, 0}},  /* MS frequency bands request. */
    {8, 8, 0, {0, 0}},  /* Distance reporting request: timer and validity. */
};

/* Table 16.3 D-MM STATUS. The sub-PDUs of tables 16.4 to 16.10 are checked for completeness; DMO gateway (16 to 31,
 * EN 300 396-5), network specific (32 to 47) and reserved values have no layout here, so only the status counts. */
static int
mm_decode_status(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    const uint8_t status = mm_u8(r, 6);
    pdu->status.status = status;
    int ok = tetra_bits_ok(r);
    for (size_t i = 0; ok && i < sizeof k_status_layouts / sizeof k_status_layouts[0]; i++) {
        const mm_status_layout* const layout = &k_status_layouts[i];
        if (layout->status == status) {
            uint32_t value[2] = {0};
            uint8_t present[2] = {0};
            (void)tetra_bits_skip(r, layout->type1_bits);
            ok = mm_read_optional(r, layout->type2_width, layout->type2_count, value, present);
        }
    }
    return ok;
}

/* The status the frontends show (tetra_publish()). */
static void
mm_report_status(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    DSD_FPRINTF(stderr, "[TETRA MM D-MM-STATUS] CC=%d status=%u (%s)\n", ctx->cc, (unsigned)pdu->status.status,
                tetra_mm_status_name(pdu->status.status));
    ctx->ts->mm_status_code = pdu->status.status;
    ctx->ts->mm_status_valid = 1;
}

/* Table 16.27 MM PDU/FUNCTION NOT SUPPORTED: the not-supported PDU type and the O-bit. The type 2 sub-PDU type that
 * may follow has a length that depends on the rejected PDU, so nothing after the O-bit is read. */
static int
mm_decode_function_not_supported(tetra_bit_reader* r, tetra_mm_pdu* pdu) {
    pdu->fns.pdu_type = mm_u8(r, 4);
    return tetra_bits_skip(r, 1);
}

static void
mm_report_function_not_supported(const mm_ctx* ctx, const tetra_mm_pdu* pdu) {
    DSD_FPRINTF(stderr, "[TETRA MM FUNCTION-NOT-SUPPORTED] CC=%d pdu_type=%u\n", ctx->cc, (unsigned)pdu->fns.pdu_type);
}

/* How each downlink PDU type of EN 300 392-2 table 16.75 is decoded and acted on; 8, 13 and 14 are reserved. */
typedef struct {
    const char* name; /* in the summary line and the truncation note */
    int (*decode)(tetra_bit_reader* r, tetra_mm_pdu* pdu);
    void (*report)(const mm_ctx* ctx, const tetra_mm_pdu* pdu);
} mm_kind;

static const mm_kind k_mm_kinds[16] = {
    {"D-OTAR", mm_decode_otar, mm_report_otar},
    {"D-AUTHENTICATION", mm_decode_authentication, mm_report_authentication},
    {"D-CK-CHANGE", mm_decode_ck_change, mm_report_ck_change},
    {"D-DISABLE", mm_decode_enable_disable, mm_report_enable_disable},
    {"D-ENABLE", mm_decode_enable_disable, mm_report_enable_disable},
    {"D-LU-ACCEPT", mm_decode_lu_accept, mm_report_lu_accept},
    {"D-LU-COMMAND", mm_decode_lu_command, mm_report_lu_command},
    {"D-LU-REJECT", mm_decode_lu_reject, mm_report_lu_reject},
    {NULL, NULL, NULL},
    {"D-LU-PROCEEDING", mm_decode_lu_proceeding, mm_report_lu_proceeding},
    {"D-GROUP-IDENTITY", mm_decode_group_identity, mm_report_group_identity},
    {"D-GROUP-ACK", mm_decode_group_ack, mm_report_group_ack},
    {"D-MM-STATUS", mm_decode_status, mm_report_status},
    {NULL, NULL, NULL},
    {NULL, NULL, NULL},
    {"FUNCTION-NOT-SUPPORTED", mm_decode_function_not_supported, mm_report_function_not_supported},
};

int
tetra_mm_decode(const uint8_t* bits, int nbits, tetra_mm_pdu* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    out->type = mm_u8(&r, 4);
    const mm_kind* const kind = &k_mm_kinds[out->type & 15u];
    return tetra_bits_ok(&r) && kind->decode && kind->decode(&r, out) && tetra_bits_ok(&r);
}

void
tetra_mm_dispatch(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !bits || nbits < 4) {
        return;
    }
    tetra_mm_pdu pdu;
    const int complete = tetra_mm_decode(bits, nbits, &pdu);
    const mm_kind* const kind = &k_mm_kinds[pdu.type & 15u];
    const mm_ctx ctx = {opts, state, ts, cc, nbits};
    if (!kind->decode) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MM] CC=%d reserved PDU type %u (%d bits)\n", cc, (unsigned)pdu.type, nbits);
    } else if (complete) {
        kind->report(&ctx, &pdu);
    } else if (pdu.type == TETRA_MM_D_OTAR && nbits >= 8 && !mm_otar_subtype_decoded(pdu.otar.subtype)) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MM D-OTAR] CC=%d %s sub-type %u not decoded\n", cc,
                          k_otar_subtypes[pdu.otar.subtype & 15u].name, (unsigned)pdu.otar.subtype);
    } else {
        mm_log_invalid(&ctx, kind->name);
    }
}
