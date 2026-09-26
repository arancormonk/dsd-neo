// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MM (Mobility Management) PDU dispatcher.
 * ETSI EN 300 392-2, Chapter 16.
 *
 * Phase 11: MM decoding and SYSINFO-based DL carrier frequency computation.
 */

#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define mm_bits_to_uint  tetra_bits_to_uint

/* ETSI carrier formula: band supplies a 100 MHz base, carrier spacing is
 * 25 kHz, and the two-bit offset selects 0, +6.25, -6.25, or +12.5 kHz. */

long tetra_carrier_to_dl_hz(uint32_t main_carrier,
                              uint32_t freq_band,
                              uint32_t freq_offset)
{
    if (freq_band >= 16 || main_carrier > 4095)
        return 0L;
    static const long offset_hz[4] = {0L, 6250L, -6250L, 12500L};
    return (long)freq_band * 100000000L
           + (long)main_carrier * 25000L
           + offset_hz[freq_offset & 3u];
}

/* Mandatory downlink MM fields from ETSI EN 300 392-2, tables 16.1-16.27. */

/* Validate and skip the Annex E M-bit chain of type 3/4 information elements. */
static int mm_skip_type34_tail(const uint8_t *bits, int nbits, int *off)
{
    while (*off < nbits) {
        uint8_t more = bits[(*off)++] & 1u;
        if (!more)
            return 1;
        if (*off + 15 > nbits)
            return 0;
        *off += 4; /* element identifier */
        uint32_t length = mm_bits_to_uint(bits, *off, 11);
        *off += 11;
        if (length == 0 || length > (uint32_t)(nbits - *off))
            return 0;
        *off += (int)length;
    }
    return 0; /* O=1 requires a terminating M-bit. */
}

static void mm_read_octets(const uint8_t *bits, int off, uint8_t *out,
                           unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        out[i] = (uint8_t)mm_bits_to_uint(bits, off + (int)i * 8, 8);
}

/* EN 300 392-7 V3.5.1 tables A.1-A.4. */
static void mm_parse_authentication(const uint8_t *bits, int nbits, int cc,
                                    dsd_state *state)
{
    int off = 4;
    uint8_t subtype, challenge[10] = {0}, seed[10] = {0};
    uint32_t response = 0;
    uint8_t mutual = 0, result = 0, reject_reason = 0;

    if (off + 2 > nbits) goto truncated;
    subtype = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;

    switch (subtype) {
    case 0: /* D-AUTHENTICATION DEMAND */
        if (off + 160 > nbits) goto truncated;
        mm_read_octets(bits, off, challenge, sizeof(challenge)); off += 80;
        mm_read_octets(bits, off, seed, sizeof(seed)); off += 80;
        break;
    case 1: /* D-AUTHENTICATION RESPONSE */
        if (off + 113 > nbits) goto truncated;
        mm_read_octets(bits, off, seed, sizeof(seed)); off += 80;
        response = mm_bits_to_uint(bits, off, 32); off += 32;
        mutual = bits[off++] & 1u;
        if (mutual) {
            if (off + 80 > nbits) goto truncated;
            mm_read_octets(bits, off, challenge, sizeof(challenge)); off += 80;
        }
        break;
    case 2: /* D-AUTHENTICATION RESULT */
        if (off + 2 > nbits) goto truncated;
        result = bits[off++] & 1u;
        mutual = bits[off++] & 1u;
        if (mutual) {
            if (off + 32 > nbits) goto truncated;
            response = mm_bits_to_uint(bits, off, 32); off += 32;
        }
        break;
    case 3: /* D-AUTHENTICATION REJECT has no optional tail. */
        if (off + 3 > nbits) goto truncated;
        reject_reason = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (reject_reason != 0) goto truncated; /* remaining values reserved */
        goto publish;
    }

    /* Demand, response and result can carry a proprietary Type 3 element. */
    if (off >= nbits) goto truncated;
    if ((bits[off++] & 1u) && !mm_skip_type34_tail(bits, nbits, &off))
        goto truncated;

publish:
    fprintf(stderr, "[TETRA MM D-AUTHENTICATION] CC=%d subtype=%u\n", cc, subtype);
    if (state) {
        state->tetra_mm_auth_subtype = subtype;
        memcpy(state->tetra_mm_auth_random_challenge, challenge, sizeof(challenge));
        memcpy(state->tetra_mm_auth_random_seed, seed, sizeof(seed));
        state->tetra_mm_auth_response = response;
        state->tetra_mm_auth_mutual = mutual;
        state->tetra_mm_auth_result = result;
        state->tetra_mm_auth_reject_reason = reject_reason;
        state->tetra_mm_auth_valid = 1;
    }
    return;

truncated:
    fprintf(stderr,
            "[TETRA MM D-AUTHENTICATION] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
}

static int mm_parse_otar_optional_tail(const uint8_t *bits, int nbits, int *off,
                                       uint8_t *address_valid, uint16_t *mcc,
                                       uint16_t *mnc)
{
    if (*off >= nbits)
        return 0;
    if (!(bits[(*off)++] & 1u))
        return 1;
    if (*off >= nbits)
        return 0;
    if (bits[(*off)++] & 1u) {
        if (*off + 24 > nbits)
            return 0;
        *mcc = (uint16_t)mm_bits_to_uint(bits, *off, 10); *off += 10;
        *mnc = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        *address_valid = 1;
    }
    return mm_skip_type34_tail(bits, nbits, off);
}

static int mm_valid_sck_subset(uint8_t grouping, uint8_t subset)
{
    static const uint8_t max_subsets[10] = {1, 2, 3, 4, 5, 6, 7, 10, 15, 30};
    return grouping <= 9 && subset >= 1 && subset <= max_subsets[grouping];
}

typedef struct {
    uint16_t id;
    uint8_t key_type;
    uint8_t sealed[15];
    uint8_t la_type;
    uint8_t la_count;
    uint16_t la[15];
    uint16_t la_mask;
    uint16_t la_selector;
    uint16_t la_low;
    uint16_t la_high;
    uint8_t future;
    uint8_t future_sealed[15];
} mm_otar_cck_fields;

/* Tables A.42/A.43 CCK information, shared by CCK Provide and NEWCELL. */
static int mm_parse_otar_cck_information(const uint8_t *bits, int nbits,
                                         int *off, mm_otar_cck_fields *cck)
{
    if (*off + 139 > nbits)
        return 0;
    cck->id = (uint16_t)mm_bits_to_uint(bits, *off, 16); *off += 16;
    cck->key_type = bits[(*off)++] & 1u;
    mm_read_octets(bits, *off, cck->sealed, sizeof(cck->sealed)); *off += 120;
    cck->la_type = (uint8_t)mm_bits_to_uint(bits, *off, 2); *off += 2;
    switch (cck->la_type) {
    case 0:
        break;
    case 1:
        if (*off + 4 > nbits)
            return 0;
        cck->la_count = (uint8_t)mm_bits_to_uint(bits, *off, 4); *off += 4;
        if (cck->la_count == 0 || *off + 14 * cck->la_count > nbits)
            return 0;
        for (uint8_t i = 0; i < cck->la_count; i++) {
            cck->la[i] = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        }
        break;
    case 2:
        if (*off + 28 > nbits)
            return 0;
        cck->la_mask = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        cck->la_selector = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        break;
    case 3:
        if (*off + 28 > nbits)
            return 0;
        cck->la_low = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        cck->la_high = (uint16_t)mm_bits_to_uint(bits, *off, 14); *off += 14;
        if (cck->la_high <= cck->la_low)
            return 0;
        break;
    }
    if (*off >= nbits)
        return 0;
    cck->future = bits[(*off)++] & 1u;
    if (cck->key_type && cck->future)
        return 0;
    if (cck->future) {
        if (*off + 120 > nbits)
            return 0;
        mm_read_octets(bits, *off, cck->future_sealed,
                       sizeof(cck->future_sealed));
        *off += 120;
    }
    return 1;
}

static void mm_clear_otar_snapshot(dsd_state *state)
{
    state->tetra_mm_otar_cck_provision = 0;
    state->tetra_mm_otar_cck_id = 0;
    state->tetra_mm_otar_cck_key_type = 0;
    memset(state->tetra_mm_otar_cck_sealed, 0, sizeof(state->tetra_mm_otar_cck_sealed));
    state->tetra_mm_otar_cck_la_type = 0;
    state->tetra_mm_otar_cck_la_count = 0;
    memset(state->tetra_mm_otar_cck_la, 0, sizeof(state->tetra_mm_otar_cck_la));
    state->tetra_mm_otar_cck_la_mask = 0;
    state->tetra_mm_otar_cck_la_selector = 0;
    state->tetra_mm_otar_cck_la_low = 0;
    state->tetra_mm_otar_cck_la_high = 0;
    state->tetra_mm_otar_cck_future = 0;
    memset(state->tetra_mm_otar_cck_future_sealed, 0,
           sizeof(state->tetra_mm_otar_cck_future_sealed));
    state->tetra_mm_otar_ack = 0;
    state->tetra_mm_otar_explicit_response = 0;
    state->tetra_mm_otar_max_response_timer = 0;
    state->tetra_mm_otar_session_key = 0;
    memset(state->tetra_mm_otar_random_seed, 0, sizeof(state->tetra_mm_otar_random_seed));
    state->tetra_mm_otar_gsko_version = 0;
    state->tetra_mm_otar_key_count = 0;
    memset(state->tetra_mm_otar_sck_number, 0, sizeof(state->tetra_mm_otar_sck_number));
    memset(state->tetra_mm_otar_sck_version, 0, sizeof(state->tetra_mm_otar_sck_version));
    memset(state->tetra_mm_otar_sck_use, 0, sizeof(state->tetra_mm_otar_sck_use));
    memset(state->tetra_mm_otar_sck_sealed, 0, sizeof(state->tetra_mm_otar_sck_sealed));
    memset(state->tetra_mm_otar_gck_number, 0, sizeof(state->tetra_mm_otar_gck_number));
    memset(state->tetra_mm_otar_gck_version, 0, sizeof(state->tetra_mm_otar_gck_version));
    memset(state->tetra_mm_otar_gck_sealed, 0, sizeof(state->tetra_mm_otar_gck_sealed));
    state->tetra_mm_otar_group_association = 0;
    memset(state->tetra_mm_otar_gck_reject_group_association, 0,
           sizeof(state->tetra_mm_otar_gck_reject_group_association));
    memset(state->tetra_mm_otar_gck_reject_gssi, 0,
           sizeof(state->tetra_mm_otar_gck_reject_gssi));
    state->tetra_mm_otar_key_association_type = 0;
    state->tetra_mm_otar_sck_select = 0;
    state->tetra_mm_otar_sck_subset_grouping = 0;
    state->tetra_mm_otar_gck_select = 0;
    state->tetra_mm_otar_group_count = 0;
    state->tetra_mm_otar_group_is_range = 0;
    state->tetra_mm_otar_group_value_count = 0;
    memset(state->tetra_mm_otar_group_gssi, 0,
           sizeof(state->tetra_mm_otar_group_gssi));
    memset(state->tetra_mm_otar_reject_reason, 0,
           sizeof(state->tetra_mm_otar_reject_reason));
    state->tetra_mm_otar_ksg = 0;
    state->tetra_mm_otar_retry_interval = 0;
    state->tetra_mm_otar_address_valid = 0;
    state->tetra_mm_otar_mcc = 0;
    state->tetra_mm_otar_mnc = 0;
    memset(state->tetra_mm_otar_gsko_sealed, 0, sizeof(state->tetra_mm_otar_gsko_sealed));
    state->tetra_mm_otar_gssi = 0;
    state->tetra_mm_otar_dck_forwarding_result = 0;
    state->tetra_mm_otar_key_status_type = 0;
    state->tetra_mm_otar_key_status_sck_number = 0;
    state->tetra_mm_otar_key_status_sck_grouping = 0;
    state->tetra_mm_otar_key_status_sck_subset = 0;
    state->tetra_mm_otar_key_status_gck_number = 0;
    state->tetra_mm_otar_key_delete_type = 0;
    state->tetra_mm_otar_key_delete_sck_count = 0;
    memset(state->tetra_mm_otar_key_delete_sck_number, 0,
           sizeof(state->tetra_mm_otar_key_delete_sck_number));
    state->tetra_mm_otar_key_delete_sck_grouping = 0;
    state->tetra_mm_otar_key_delete_sck_subset = 0;
    state->tetra_mm_otar_key_delete_gck_count = 0;
    memset(state->tetra_mm_otar_key_delete_gck_number, 0,
           sizeof(state->tetra_mm_otar_key_delete_gck_number));
    state->tetra_mm_otar_dm_sck_ack = 0;
    state->tetra_mm_otar_dm_sck_count = 0;
    state->tetra_mm_otar_dm_sck_grouping = 0;
    state->tetra_mm_otar_dm_sck_subset = 0;
    state->tetra_mm_otar_dm_sck_vn = 0;
    memset(state->tetra_mm_otar_dm_sck_number, 0,
           sizeof(state->tetra_mm_otar_dm_sck_number));
    memset(state->tetra_mm_otar_dm_sck_version, 0,
           sizeof(state->tetra_mm_otar_dm_sck_version));
    state->tetra_mm_otar_dm_sck_time_type = 0;
    state->tetra_mm_otar_dm_sck_slot = 0;
    state->tetra_mm_otar_dm_sck_frame = 0;
    state->tetra_mm_otar_dm_sck_multiframe = 0;
    state->tetra_mm_otar_dm_sck_hyperframe = 0;
    state->tetra_mm_otar_dm_sck_network_time = 0;
}

/* EN 300 392-7 V3.5.1 tables A.9, A.12, A.15, A.16, A.19, A.20, A.23,
 * A.24, A.27d, A.27f, A.27h, A.30 and A.30a. Sealed keys are opaque values; a passive monitor
 * records but never decrypts them. */
static void mm_parse_otar(const uint8_t *bits, int nbits, int cc,
                          dsd_state *state)
{
    int off = 4;
    uint8_t subtype, provision = 0, dck_forwarding_result = 0;
    mm_otar_cck_fields cck = {0};
    uint8_t ack = 0, explicit_response = 0, session_key = 0, random_seed[10] = {0};
    uint16_t max_response_timer = 0, gsko_version = 0;
    uint8_t key_count = 0, sck_number[7] = {0}, sck_use[7] = {0};
    uint16_t sck_version[7] = {0};
    uint8_t sck_sealed[7][15] = {{0}}, reject_reason[7] = {0};
    uint16_t gck_number[7] = {0}, gck_version[7] = {0};
    uint8_t gck_sealed[7][15] = {{0}}, group_association = 0;
    uint8_t gck_reject_group_association[7] = {0};
    uint32_t gck_reject_gssi[7] = {0};
    uint8_t key_association_type = 0, sck_select = 0, sck_subset_grouping = 0;
    uint32_t gck_select = 0, group_gssi[30] = {0};
    uint8_t group_count = 0, group_is_range = 0, group_value_count = 0;
    uint8_t ksg = 0, retry = 0, address_valid = 0, gsko_sealed[15] = {0};
    uint16_t mcc = 0, mnc = 0;
    uint32_t gssi = 0;
    uint8_t key_status_type = 0, key_status_sck_number = 0;
    uint8_t key_status_sck_grouping = 0, key_status_sck_subset = 0;
    uint16_t key_status_gck_number = 0;
    uint8_t key_delete_type = 0, key_delete_sck_count = 0;
    uint8_t key_delete_sck_number[31] = {0};
    uint8_t key_delete_sck_grouping = 0, key_delete_sck_subset = 0;
    uint8_t key_delete_gck_count = 0;
    uint16_t key_delete_gck_number[15] = {0};
    uint8_t dm_sck_ack = 0, dm_sck_count = 0, dm_sck_grouping = 0;
    uint8_t dm_sck_subset = 0, dm_sck_number[15] = {0};
    uint16_t dm_sck_vn = 0, dm_sck_version[15] = {0};
    uint8_t dm_sck_time_type = 0, dm_sck_slot = 0, dm_sck_frame = 0;
    uint8_t dm_sck_multiframe = 0;
    uint16_t dm_sck_hyperframe = 0;
    uint64_t dm_sck_network_time = 0;

    if (off + 4 > nbits) goto truncated;
    subtype = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;

    switch (subtype) {
    case 0: /* Table A.9 D-OTAR CCK Provide. */
        if (off + 1 > nbits) goto truncated;
        provision = bits[off++] & 1u;
        if (provision && !mm_parse_otar_cck_information(bits, nbits, &off, &cck))
            goto truncated;
        if (off >= nbits) goto truncated;
        if ((bits[off++] & 1u) && !mm_skip_type34_tail(bits, nbits, &off))
            goto truncated;
        break;

    case 7: /* Table A.30 D-OTAR NEWCELL. */
        if (off + 2 > nbits) goto truncated;
        dck_forwarding_result = bits[off++] & 1u;
        provision = bits[off++] & 1u;
        if (provision && !mm_parse_otar_cck_information(bits, nbits, &off, &cck))
            goto truncated;
        if (off >= nbits) goto truncated;
        if ((bits[off++] & 1u) && !mm_skip_type34_tail(bits, nbits, &off))
            goto truncated;
        break;

    case 2: /* Table A.16 D-OTAR SCK Provide. */
        if (off + 22 > nbits) goto truncated;
        ack = bits[off++] & 1u;
        explicit_response = bits[off++] & 1u;
        if (!ack && explicit_response) goto truncated; /* reserved bit must be zero */
        max_response_timer = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        session_key = bits[off++] & 1u;
        if (session_key) {
            if (off + 16 > nbits) goto truncated;
            gsko_version = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        } else {
            if (off + 80 > nbits) goto truncated;
            mm_read_octets(bits, off, random_seed, sizeof(random_seed)); off += 80;
        }
        if (off + 3 > nbits) goto truncated;
        key_count = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (off + 143 * key_count > nbits) goto truncated;
        for (uint8_t i = 0; i < key_count; i++) {
            sck_number[i] = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            sck_version[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            sck_use[i] = bits[off++] & 1u;
            if (bits[off++] & 1u) goto truncated;
            mm_read_octets(bits, off, sck_sealed[i], 15); off += 120;
        }
        if (off + 7 > nbits) goto truncated;
        ksg = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
        if (ksg >= 4 && ksg <= 7) goto truncated;
        retry = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 3: /* Table A.19 D-OTAR SCK Reject. */
        if (off + 6 > nbits) goto truncated;
        key_count = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (off + 8 * key_count + 3 > nbits) goto truncated;
        for (uint8_t i = 0; i < key_count; i++) {
            reject_reason[i] = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
            if (reject_reason[i] > 3) goto truncated;
            sck_number[i] = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
        }
        retry = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 4: /* Table A.12 D-OTAR GCK Provide. */
        if (off + 22 > nbits) goto truncated;
        ack = bits[off++] & 1u;
        explicit_response = bits[off++] & 1u;
        if (!ack && explicit_response) goto truncated; /* reserved bit must be zero */
        max_response_timer = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        session_key = bits[off++] & 1u;
        if (session_key) {
            if (off + 16 > nbits) goto truncated;
            gsko_version = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        } else {
            if (off + 80 > nbits) goto truncated;
            mm_read_octets(bits, off, random_seed, sizeof(random_seed)); off += 80;
        }
        if (off + 3 > nbits) goto truncated;
        key_count = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (off + 152 * key_count > nbits) goto truncated;
        for (uint8_t i = 0; i < key_count; i++) {
            gck_number[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            gck_version[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            mm_read_octets(bits, off, gck_sealed[i], 15); off += 120;
        }
        if (off + 8 > nbits) goto truncated;
        ksg = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
        if (ksg >= 4 && ksg <= 7) goto truncated;
        group_association = bits[off++] & 1u;
        if (group_association) {
            for (uint8_t i = 1; i < key_count; i++) {
                if (gck_number[i] != gck_number[0]) goto truncated;
                for (uint8_t j = 0; j < i; j++) {
                    if (gck_version[i] == gck_version[j]) goto truncated;
                }
            }
            if (off + 24 > nbits) goto truncated;
            gssi = mm_bits_to_uint(bits, off, 24); off += 24;
        }
        retry = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 5: /* Table A.15 D-OTAR GCK Reject. */
        if (off + 3 > nbits) goto truncated;
        key_count = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (key_count == 0) goto truncated; /* Table A.79d reserves zero. */
        for (uint8_t i = 0; i < key_count; i++) {
            if (off + 20 > nbits) goto truncated;
            reject_reason[i] = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
            if (reject_reason[i] > 3) goto truncated;
            gck_reject_group_association[i] = bits[off++] & 1u;
            if (gck_reject_group_association[i]) {
                if (off + 24 > nbits) goto truncated;
                gck_reject_gssi[i] = mm_bits_to_uint(bits, off, 24); off += 24;
            } else {
                gck_number[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            }
        }
        if (off + 3 > nbits) goto truncated;
        retry = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 6: /* Table A.24 D-OTAR KEY ASSOCIATE Demand. */
        if (off + 19 > nbits) goto truncated;
        ack = bits[off++] & 1u;
        explicit_response = bits[off++] & 1u;
        if (!ack && explicit_response) goto truncated;
        max_response_timer = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        key_association_type = bits[off++] & 1u;
        if (key_association_type) {
            if (off + 17 > nbits) goto truncated;
            gck_select = mm_bits_to_uint(bits, off, 17); off += 17;
            if (gck_select > 0x10000u) goto truncated;
        } else {
            if (off + 10 > nbits) goto truncated;
            sck_select = (uint8_t)mm_bits_to_uint(bits, off, 6); off += 6;
            sck_subset_grouping = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            if (sck_select > 32 || sck_subset_grouping > 9) goto truncated;
        }
        if (off + 5 > nbits) goto truncated;
        group_count = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
        if (group_count == 0) goto truncated;
        group_is_range = (uint8_t)(group_count == 31);
        group_value_count = group_is_range ? 2 : group_count;
        if (off + 24 * group_value_count > nbits) goto truncated;
        for (uint8_t i = 0; i < group_value_count; i++) {
            group_gssi[i] = mm_bits_to_uint(bits, off, 24); off += 24;
        }
        if (group_is_range && group_gssi[1] <= group_gssi[0]) goto truncated;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 8: /* Table A.20 D-OTAR GSKO Provide. */
        if (off + 240 > nbits) goto truncated;
        mm_read_octets(bits, off, random_seed, sizeof(random_seed)); off += 80;
        gsko_version = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        mm_read_octets(bits, off, gsko_sealed, sizeof(gsko_sealed)); off += 120;
        gssi = mm_bits_to_uint(bits, off, 24); off += 24;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 9: /* Table A.23 D-OTAR GSKO Reject. */
        if (off + 30 > nbits) goto truncated;
        reject_reason[0] = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (reject_reason[0] > 3) goto truncated;
        gssi = mm_bits_to_uint(bits, off, 24); off += 24;
        retry = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 10: /* Table A.27f D-OTAR KEY DELETE Demand. */
        if (off + 3 > nbits) goto truncated;
        key_delete_type = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        switch (key_delete_type) {
        case 0: /* Individual SCKs. */
        case 1: /* Members of a KAG. */
            if (off + 5 > nbits) goto truncated;
            key_delete_sck_count = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            if (off + 5 * key_delete_sck_count > nbits) goto truncated;
            for (uint8_t i = 0; i < key_delete_sck_count; i++) {
                key_delete_sck_number[i] =
                    (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            }
            break;
        case 2: /* SCK subset. */
            if (off + 9 > nbits) goto truncated;
            key_delete_sck_grouping =
                (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            key_delete_sck_subset =
                (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            if (!mm_valid_sck_subset(key_delete_sck_grouping,
                                     key_delete_sck_subset))
                goto truncated;
            break;
        case 3: /* All SCKs. */
        case 5: /* All GCKs. */
        case 6: /* GSKO. */
            break;
        case 4: /* Individual GCKs. */
            if (off + 4 > nbits) goto truncated;
            key_delete_gck_count =
                (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            if (off + 16 * key_delete_gck_count > nbits) goto truncated;
            for (uint8_t i = 0; i < key_delete_gck_count; i++) {
                key_delete_gck_number[i] =
                    (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            }
            break;
        default: /* Key-delete extension is defined for the uplink result. */
            goto truncated;
        }
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 11: { /* Table A.27h D-OTAR KEY STATUS Demand. */
        if (off + 19 > nbits) goto truncated;
        ack = bits[off++] & 1u;
        explicit_response = bits[off++] & 1u;
        if (!ack && explicit_response) goto truncated;
        max_response_timer = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        key_status_type = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        switch (key_status_type) {
        case 0: /* Individual SCK. */
            if (off + 5 > nbits) goto truncated;
            key_status_sck_number = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            break;
        case 1: /* SCK subset. */
            if (off + 9 > nbits) goto truncated;
            key_status_sck_grouping = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            key_status_sck_subset = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            if (!mm_valid_sck_subset(key_status_sck_grouping,
                                     key_status_sck_subset))
                goto truncated;
            break;
        case 2: /* All SCKs. */
        case 4: /* All GCKs. */
        case 5: /* GSKO. */
            break;
        case 3: /* Individual GCK. */
            if (off + 16 > nbits) goto truncated;
            key_status_gck_number = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            break;
        default: /* Reject and reserved are response-only/invalid. */
            goto truncated;
        }
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;
    }

    case 12: /* Table A.30a D-OTAR CMG GTSI Provide. */
        if (off + 24 > nbits) goto truncated;
        gssi = mm_bits_to_uint(bits, off, 24); off += 24;
        if (!mm_parse_otar_optional_tail(bits, nbits, &off,
                                         &address_valid, &mcc, &mnc))
            goto truncated;
        break;

    case 13: /* Table A.27d D-DM-SCK ACTIVATE DEMAND. */
        if (off + 5 > nbits) goto truncated;
        dm_sck_ack = bits[off++] & 1u;
        dm_sck_count = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
        if (dm_sck_count == 0) {
            if (off + 25 > nbits) goto truncated;
            dm_sck_grouping = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            dm_sck_subset = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            dm_sck_vn = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            if (!mm_valid_sck_subset(dm_sck_grouping, dm_sck_subset))
                goto truncated;
        } else {
            if (off + 21 * dm_sck_count > nbits) goto truncated;
            for (uint8_t i = 0; i < dm_sck_count; i++) {
                dm_sck_number[i] = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
                dm_sck_version[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            }
        }
        if (off + 2 > nbits) goto truncated;
        dm_sck_time_type = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
        if (dm_sck_time_type == 0) {
            if (off + 29 > nbits) goto truncated;
            dm_sck_slot = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
            dm_sck_frame = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            dm_sck_multiframe = (uint8_t)mm_bits_to_uint(bits, off, 6); off += 6;
            dm_sck_hyperframe = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        } else if (dm_sck_time_type == 1) {
            if (off + 48 > nbits) goto truncated;
            dm_sck_network_time = ((uint64_t)mm_bits_to_uint(bits, off, 16) << 32)
                                | mm_bits_to_uint(bits, off + 16, 32);
            off += 48;
        }
        /* Unlike other D-OTAR messages, the DMO network MNI is mandatory. */
        if (off + 24 > nbits) goto truncated;
        mcc = (uint16_t)mm_bits_to_uint(bits, off, 10); off += 10;
        mnc = (uint16_t)mm_bits_to_uint(bits, off, 14); off += 14;
        address_valid = 1;
        break;

    default:
        fprintf(stderr, "[TETRA MM D-OTAR] CC=%d unsupported subtype=%u\n",
                cc, subtype);
        return;
    }

    fprintf(stderr, "[TETRA MM D-OTAR] CC=%d subtype=%u keys=%u\n",
            cc, subtype, key_count);
    if (state) {
        mm_clear_otar_snapshot(state);
        state->tetra_mm_otar_subtype = subtype;
        state->tetra_mm_otar_cck_provision = provision;
        state->tetra_mm_otar_cck_id = cck.id;
        state->tetra_mm_otar_cck_key_type = cck.key_type;
        memcpy(state->tetra_mm_otar_cck_sealed, cck.sealed, sizeof(cck.sealed));
        state->tetra_mm_otar_cck_la_type = cck.la_type;
        state->tetra_mm_otar_cck_la_count = cck.la_count;
        memcpy(state->tetra_mm_otar_cck_la, cck.la, sizeof(cck.la));
        state->tetra_mm_otar_cck_la_mask = cck.la_mask;
        state->tetra_mm_otar_cck_la_selector = cck.la_selector;
        state->tetra_mm_otar_cck_la_low = cck.la_low;
        state->tetra_mm_otar_cck_la_high = cck.la_high;
        state->tetra_mm_otar_cck_future = cck.future;
        memcpy(state->tetra_mm_otar_cck_future_sealed, cck.future_sealed,
               sizeof(cck.future_sealed));
        state->tetra_mm_otar_ack = ack;
        state->tetra_mm_otar_explicit_response = explicit_response;
        state->tetra_mm_otar_max_response_timer = max_response_timer;
        state->tetra_mm_otar_session_key = session_key;
        memcpy(state->tetra_mm_otar_random_seed, random_seed, sizeof(random_seed));
        state->tetra_mm_otar_gsko_version = gsko_version;
        state->tetra_mm_otar_key_count = key_count;
        memcpy(state->tetra_mm_otar_sck_number, sck_number, sizeof(sck_number));
        memcpy(state->tetra_mm_otar_sck_version, sck_version, sizeof(sck_version));
        memcpy(state->tetra_mm_otar_sck_use, sck_use, sizeof(sck_use));
        memcpy(state->tetra_mm_otar_sck_sealed, sck_sealed, sizeof(sck_sealed));
        memcpy(state->tetra_mm_otar_gck_number, gck_number, sizeof(gck_number));
        memcpy(state->tetra_mm_otar_gck_version, gck_version, sizeof(gck_version));
        memcpy(state->tetra_mm_otar_gck_sealed, gck_sealed, sizeof(gck_sealed));
        state->tetra_mm_otar_group_association = group_association;
        memcpy(state->tetra_mm_otar_gck_reject_group_association,
               gck_reject_group_association, sizeof(gck_reject_group_association));
        memcpy(state->tetra_mm_otar_gck_reject_gssi, gck_reject_gssi,
               sizeof(gck_reject_gssi));
        state->tetra_mm_otar_key_association_type = key_association_type;
        state->tetra_mm_otar_sck_select = sck_select;
        state->tetra_mm_otar_sck_subset_grouping = sck_subset_grouping;
        state->tetra_mm_otar_gck_select = gck_select;
        state->tetra_mm_otar_group_count = group_count;
        state->tetra_mm_otar_group_is_range = group_is_range;
        state->tetra_mm_otar_group_value_count = group_value_count;
        memcpy(state->tetra_mm_otar_group_gssi, group_gssi, sizeof(group_gssi));
        memcpy(state->tetra_mm_otar_reject_reason, reject_reason, sizeof(reject_reason));
        state->tetra_mm_otar_ksg = ksg;
        state->tetra_mm_otar_retry_interval = retry;
        state->tetra_mm_otar_address_valid = address_valid;
        state->tetra_mm_otar_mcc = mcc;
        state->tetra_mm_otar_mnc = mnc;
        memcpy(state->tetra_mm_otar_gsko_sealed, gsko_sealed, sizeof(gsko_sealed));
        state->tetra_mm_otar_gssi = gssi;
        state->tetra_mm_otar_dck_forwarding_result = dck_forwarding_result;
        state->tetra_mm_otar_key_status_type = key_status_type;
        state->tetra_mm_otar_key_status_sck_number = key_status_sck_number;
        state->tetra_mm_otar_key_status_sck_grouping = key_status_sck_grouping;
        state->tetra_mm_otar_key_status_sck_subset = key_status_sck_subset;
        state->tetra_mm_otar_key_status_gck_number = key_status_gck_number;
        state->tetra_mm_otar_key_delete_type = key_delete_type;
        state->tetra_mm_otar_key_delete_sck_count = key_delete_sck_count;
        memcpy(state->tetra_mm_otar_key_delete_sck_number,
               key_delete_sck_number, sizeof(key_delete_sck_number));
        state->tetra_mm_otar_key_delete_sck_grouping = key_delete_sck_grouping;
        state->tetra_mm_otar_key_delete_sck_subset = key_delete_sck_subset;
        state->tetra_mm_otar_key_delete_gck_count = key_delete_gck_count;
        memcpy(state->tetra_mm_otar_key_delete_gck_number,
               key_delete_gck_number, sizeof(key_delete_gck_number));
        state->tetra_mm_otar_dm_sck_ack = dm_sck_ack;
        state->tetra_mm_otar_dm_sck_count = dm_sck_count;
        state->tetra_mm_otar_dm_sck_grouping = dm_sck_grouping;
        state->tetra_mm_otar_dm_sck_subset = dm_sck_subset;
        state->tetra_mm_otar_dm_sck_vn = dm_sck_vn;
        memcpy(state->tetra_mm_otar_dm_sck_number, dm_sck_number,
               sizeof(dm_sck_number));
        memcpy(state->tetra_mm_otar_dm_sck_version, dm_sck_version,
               sizeof(dm_sck_version));
        state->tetra_mm_otar_dm_sck_time_type = dm_sck_time_type;
        state->tetra_mm_otar_dm_sck_slot = dm_sck_slot;
        state->tetra_mm_otar_dm_sck_frame = dm_sck_frame;
        state->tetra_mm_otar_dm_sck_multiframe = dm_sck_multiframe;
        state->tetra_mm_otar_dm_sck_hyperframe = dm_sck_hyperframe;
        state->tetra_mm_otar_dm_sck_network_time = dm_sck_network_time;
        state->tetra_mm_otar_valid = 1;
    }
    return;

truncated:
    fprintf(stderr, "[TETRA MM D-OTAR] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
}

/* EN 300 392-7 V3.5.1 table A.26.  Decode all conditional and repeated
 * fields into locals so a malformed security command cannot replace the
 * last complete key-change snapshot. */
static void mm_parse_ck_change_demand(const uint8_t *bits, int nbits, int cc,
                                      dsd_state *state)
{
    int off = 4;
    uint8_t ack, security_class, key_type;
    uint8_t sck_use = 0, sck_count = 0, sck_grouping = 0, sck_subset = 0;
    uint16_t sck_vn = 0, cck_id = 0, gck_vn = 0;
    uint8_t sck_number[15] = {0};
    uint16_t sck_version[15] = {0};
    uint8_t gck_count = 0;
    uint16_t gck_number[15] = {0}, gck_version[15] = {0};
    uint8_t time_type, slot = 0, frame = 0, multiframe = 0;
    uint16_t hyperframe = 0;
    uint64_t network_time = 0;

    if (off + 6 > nbits) goto truncated;
    ack = bits[off++] & 1u;
    security_class = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
    key_type = (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
    if (key_type > 5) goto truncated; /* 6 and 7 are reserved. */

    switch (key_type) {
    case 0: /* SCK */
        if (off + 5 > nbits) goto truncated;
        sck_use = bits[off++] & 1u;
        sck_count = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
        if (sck_count == 0) {
            /* A zero count selects a subset only for DMO SCK use. */
            if (!sck_use || off + 25 > nbits) goto truncated;
            sck_grouping = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
            sck_subset = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
            sck_vn = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        } else {
            if (off + 21 * sck_count > nbits) goto truncated;
            for (uint8_t i = 0; i < sck_count; i++) {
                sck_number[i] = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
                sck_version[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            }
        }
        break;
    case 1: /* CCK */
        if (off + 16 > nbits) goto truncated;
        cck_id = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        break;
    case 2: /* GCK */
        if (off + 4 > nbits) goto truncated;
        gck_count = (uint8_t)mm_bits_to_uint(bits, off, 4); off += 4;
        if (gck_count == 0 || off + 32 * gck_count > nbits) goto truncated;
        for (uint8_t i = 0; i < gck_count; i++) {
            gck_number[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
            gck_version[i] = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        }
        break;
    case 3: /* Class 3 CCK and GCK activation */
        if (off + 32 > nbits) goto truncated;
        cck_id = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        gck_vn = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        break;
    case 4: /* All GCKs */
        if (off + 16 > nbits) goto truncated;
        gck_vn = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
        break;
    case 5: /* No cipher key */
        break;
    }

    if (off + 2 > nbits) goto truncated;
    time_type = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
    if (time_type == 0) {
        if (off + 29 > nbits) goto truncated;
        slot = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
        frame = (uint8_t)mm_bits_to_uint(bits, off, 5); off += 5;
        multiframe = (uint8_t)mm_bits_to_uint(bits, off, 6); off += 6;
        hyperframe = (uint16_t)mm_bits_to_uint(bits, off, 16); off += 16;
    } else if (time_type == 1) {
        if (off + 48 > nbits) goto truncated;
        network_time = ((uint64_t)mm_bits_to_uint(bits, off, 16) << 32)
                     | mm_bits_to_uint(bits, off + 16, 32);
        off += 48;
    }

    fprintf(stderr, "[TETRA MM D-CK-CHANGE] CC=%d key_type=%u time_type=%u\n",
            cc, key_type, time_type);
    if (state) {
        state->tetra_mm_ck_change_ack = ack;
        state->tetra_mm_ck_change_security_class = security_class;
        state->tetra_mm_ck_change_key_type = key_type;
        state->tetra_mm_ck_change_sck_use = sck_use;
        state->tetra_mm_ck_change_sck_count = sck_count;
        state->tetra_mm_ck_change_sck_grouping = sck_grouping;
        state->tetra_mm_ck_change_sck_subset = sck_subset;
        state->tetra_mm_ck_change_sck_vn = sck_vn;
        memcpy(state->tetra_mm_ck_change_sck_number, sck_number, sizeof(sck_number));
        memcpy(state->tetra_mm_ck_change_sck_version, sck_version, sizeof(sck_version));
        state->tetra_mm_ck_change_cck_id = cck_id;
        state->tetra_mm_ck_change_gck_count = gck_count;
        memcpy(state->tetra_mm_ck_change_gck_number, gck_number, sizeof(gck_number));
        memcpy(state->tetra_mm_ck_change_gck_version, gck_version, sizeof(gck_version));
        state->tetra_mm_ck_change_gck_vn = gck_vn;
        state->tetra_mm_ck_change_time_type = time_type;
        state->tetra_mm_ck_change_slot = slot;
        state->tetra_mm_ck_change_frame = frame;
        state->tetra_mm_ck_change_multiframe = multiframe;
        state->tetra_mm_ck_change_hyperframe = hyperframe;
        state->tetra_mm_ck_change_network_time = network_time;
        state->tetra_mm_ck_change_valid = 1;
    }
    return;

truncated:
    fprintf(stderr, "[TETRA MM D-CK-CHANGE] CC=%d (invalid/truncated: %d bits)\n",
            cc, nbits);
}

/* EN 300 392-7 tables A.31/A.32.  These messages can change whether the
 * receiver accepts normal services, so validate every conditional identity,
 * the complete optional authentication challenge, and the proprietary tail
 * before changing the public state. */
static void mm_parse_enable_disable(const uint8_t *bits, int nbits, int cc,
                                    int is_enable, dsd_state *state)
{
    int off = 4;
    uint8_t intent, permanent = 0, equipment, subscription;
    uint64_t tei = 0;
    uint16_t mcc = 0, mnc = 0;
    uint32_t ssi = 0;
    uint8_t auth_valid = 0, auth_challenge[20] = {0};

    if (off >= nbits) goto truncated;
    intent = bits[off++] & 1u;
    if (!is_enable) {
        if (off >= nbits) goto truncated;
        permanent = bits[off++] & 1u;
    }
    if (off >= nbits) goto truncated;
    equipment = bits[off++] & 1u;
    if (equipment) {
        if (off + 60 > nbits) goto truncated;
        tei = ((uint64_t)mm_bits_to_uint(bits, off, 28) << 32)
            | mm_bits_to_uint(bits, off + 28, 32);
        off += 60;
    }
    if (off >= nbits) goto truncated;
    subscription = bits[off++] & 1u;
    if (subscription) {
        if (off + 48 > nbits) goto truncated;
        mcc = (uint16_t)mm_bits_to_uint(bits, off, 10); off += 10;
        mnc = (uint16_t)mm_bits_to_uint(bits, off, 14); off += 14;
        ssi = mm_bits_to_uint(bits, off, 24); off += 24;
    }

    if (off >= nbits) goto truncated;
    if (bits[off++] & 1u) {
        /* Authentication challenge is the sole Type-2 element. */
        if (off >= nbits) goto truncated;
        if (bits[off++] & 1u) {
            if (off + 160 > nbits) goto truncated;
            for (unsigned byte = 0; byte < sizeof(auth_challenge); byte++)
                auth_challenge[byte] =
                    (uint8_t)mm_bits_to_uint(bits, off + (int)byte * 8, 8);
            off += 160;
            auth_valid = 1;
        }
        if (!mm_skip_type34_tail(bits, nbits, &off)) goto truncated;
    }

    fprintf(stderr, "[TETRA MM D-%s] CC=%d intent=%u equipment=%u subscription=%u%s\n",
            is_enable ? "ENABLE" : "DISABLE", cc, intent, equipment,
            subscription, !is_enable && permanent ? " permanent" : "");
    if (state) {
        state->tetra_ms_enabled = (uint8_t)is_enable;
        state->tetra_mm_enable_disable_valid = 1;
        state->tetra_mm_enable_disable_is_enable = (uint8_t)is_enable;
        state->tetra_mm_enable_disable_intent = intent;
        state->tetra_mm_disable_permanent = permanent;
        state->tetra_mm_enable_disable_equipment = equipment;
        state->tetra_mm_enable_disable_tei = tei;
        state->tetra_mm_enable_disable_subscription = subscription;
        state->tetra_mm_enable_disable_mcc = mcc;
        state->tetra_mm_enable_disable_mnc = mnc;
        state->tetra_mm_enable_disable_ssi = ssi;
        state->tetra_mm_enable_disable_auth_valid = auth_valid;
        memcpy(state->tetra_mm_enable_disable_auth_challenge, auth_challenge,
               sizeof(auth_challenge));
    }
    return;

truncated:
    fprintf(stderr, "[TETRA MM D-%s] CC=%d (invalid/truncated: %d bits)\n",
            is_enable ? "ENABLE" : "DISABLE", cc, nbits);
}

static void mm_parse_lu_accept(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 8) {
        fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t type = (uint8_t)mm_bits_to_uint(bits, 4, 3);
    int off = 7;
    uint32_t ssi = 0;
    uint16_t mcc = 0, mnc = 0, subscriber_class = 0, energy_saving = 0;
    uint8_t scch = 0;
    uint8_t ssi_valid = 0, address_valid = 0, subscriber_valid = 0;
    uint8_t energy_valid = 0, scch_valid = 0;

    uint8_t optional = bits[off++] & 1u;
    if (optional) {
#define MM_ACCEPT_TYPE2(width, body) do { \
            if (off >= nbits) return; \
            uint8_t present = bits[off++] & 1u; \
            if (present) { \
                if (off + (width) > nbits) return; \
                body; \
                off += (width); \
            } \
        } while (0)
        MM_ACCEPT_TYPE2(24, ssi = mm_bits_to_uint(bits, off, 24); ssi_valid = 1);
        MM_ACCEPT_TYPE2(24, mcc = (uint16_t)mm_bits_to_uint(bits, off, 10);
                            mnc = (uint16_t)mm_bits_to_uint(bits, off + 10, 14);
                            address_valid = 1);
        MM_ACCEPT_TYPE2(16, subscriber_class = (uint16_t)mm_bits_to_uint(bits, off, 16);
                            subscriber_valid = 1);
        MM_ACCEPT_TYPE2(14, energy_saving = (uint16_t)mm_bits_to_uint(bits, off, 14);
                            energy_valid = 1);
        MM_ACCEPT_TYPE2(6, scch = (uint8_t)mm_bits_to_uint(bits, off, 6); scch_valid = 1);
#undef MM_ACCEPT_TYPE2
        if (!mm_skip_type34_tail(bits, nbits, &off)) {
            fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d (truncated optional tail)\n", cc);
            return;
        }
    }
    fprintf(stderr, "[TETRA MM D-LU-ACCEPT] CC=%d type=%u\n", cc, type);
    if (state) {
        state->tetra_mm_lu_accept_type = type;
        state->tetra_mm_lu_accept_ssi = ssi;
        state->tetra_mm_lu_accept_ssi_valid = ssi_valid;
        state->tetra_mm_lu_accept_mcc = mcc;
        state->tetra_mm_lu_accept_mnc = mnc;
        state->tetra_mm_lu_accept_address_ext_valid = address_valid;
        state->tetra_mm_lu_accept_subscriber_class = subscriber_class;
        state->tetra_mm_lu_accept_subscriber_class_valid = subscriber_valid;
        state->tetra_mm_lu_accept_energy_saving = energy_saving;
        state->tetra_mm_lu_accept_energy_saving_valid = energy_valid;
        state->tetra_mm_lu_accept_scch = scch;
        state->tetra_mm_lu_accept_scch_valid = scch_valid;
        state->tetra_mm_lu_accept_valid = 1;
    }
}

static void mm_parse_lu_command(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 6) {
        fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t report = bits[4] & 1u;
    uint8_t cipher = bits[5] & 1u;
    uint16_t params = 0;
    uint16_t mcc = 0, mnc = 0;
    uint8_t address_ext_valid = 0;
    int off = 6;
    if (cipher) {
        if (nbits < 16) {
            fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d (truncated cipher parameters)\n", cc);
            return;
        }
        params = (uint16_t)mm_bits_to_uint(bits, 6, 10);
        off = 16;
    }
    if (off >= nbits) {
        fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d (missing O-bit)\n", cc);
        return;
    }
    uint8_t optional = bits[off++] & 1u;
    if (optional) {
        if (off >= nbits)
            return;
        uint8_t address_ext_present = bits[off++] & 1u;
        if (address_ext_present) {
            if (off + 24 > nbits)
                return;
            mcc = (uint16_t)mm_bits_to_uint(bits, off, 10); off += 10;
            mnc = (uint16_t)mm_bits_to_uint(bits, off, 14); off += 14;
            address_ext_valid = 1;
        }
        if (!mm_skip_type34_tail(bits, nbits, &off)) {
            fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d (truncated optional tail)\n", cc);
            return;
        }
    }
    fprintf(stderr, "[TETRA MM D-LU-COMMAND] CC=%d group_report=%u cipher=%u\n", cc, report, cipher);
    if (state) {
        state->tetra_mm_lu_command_group_report = report;
        state->tetra_mm_lu_command_cipher_control = cipher;
        state->tetra_mm_lu_command_cipher_params = params;
        state->tetra_mm_lu_command_mcc = mcc;
        state->tetra_mm_lu_command_mnc = mnc;
        state->tetra_mm_lu_command_address_ext_valid = address_ext_valid;
        state->tetra_mm_lu_command_valid = 1;
    }
}

static void mm_parse_lu_reject(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 13) {
        fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t update_type = (uint8_t)mm_bits_to_uint(bits, 4, 3);
    uint8_t cause = (uint8_t)mm_bits_to_uint(bits, 7, 5);
    uint8_t cipher = bits[12] & 1u;
    uint16_t params = 0;
    uint16_t mcc = 0, mnc = 0;
    uint8_t address_valid = 0;
    int off = 13;
    if (cipher && nbits < 23) {
        fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d (truncated cipher parameters)\n", cc);
        return;
    }
    if (cipher) {
        params = (uint16_t)mm_bits_to_uint(bits, 13, 10);
        off = 23;
    }
    if (off >= nbits) {
        fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d (missing O-bit)\n", cc);
        return;
    }
    uint8_t optional = bits[off++] & 1u;
    if (optional) {
        if (off >= nbits)
            return;
        uint8_t address_present = bits[off++] & 1u;
        if (address_present) {
            if (off + 24 > nbits)
                return;
            mcc = (uint16_t)mm_bits_to_uint(bits, off, 10); off += 10;
            mnc = (uint16_t)mm_bits_to_uint(bits, off, 14); off += 14;
            address_valid = 1;
        }
        if (!mm_skip_type34_tail(bits, nbits, &off)) {
            fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d (truncated optional tail)\n", cc);
            return;
        }
    }
    fprintf(stderr, "[TETRA MM D-LU-REJECT] CC=%d type=%u cause=%u cipher=%u\n",
            cc, update_type, cause, cipher);
    if (state) {
        state->tetra_mm_lu_reject_type = update_type;
        state->tetra_mm_lu_reject_cause = cause;
        state->tetra_mm_lu_reject_cipher_control = cipher;
        state->tetra_mm_lu_reject_cipher_params = params;
        state->tetra_mm_lu_reject_mcc = mcc;
        state->tetra_mm_lu_reject_mnc = mnc;
        state->tetra_mm_lu_reject_address_ext_valid = address_valid;
        state->tetra_mm_lu_reject_valid = 1;
    }
}

static void mm_parse_lu_proceeding(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 53) {
        fprintf(stderr, "[TETRA MM D-LU-PROCEEDING] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint32_t ssi = mm_bits_to_uint(bits, 4, 24);
    uint32_t mni = mm_bits_to_uint(bits, 28, 24);
    int off = 52;
    uint8_t optional = bits[off++] & 1u;
    if (optional && !mm_skip_type34_tail(bits, nbits, &off)) {
        fprintf(stderr, "[TETRA MM D-LU-PROCEEDING] CC=%d (truncated optional tail)\n", cc);
        return;
    }
    fprintf(stderr, "[TETRA MM D-LU-PROCEEDING] CC=%d SSI=%u MNI=%u\n", cc, ssi, mni);
    if (state) {
        state->tetra_mm_lu_proceeding_ssi = ssi;
        state->tetra_mm_lu_proceeding_mni = mni;
        state->tetra_mm_lu_proceeding_valid = 1;
    }
}

typedef struct {
    uint8_t count;
    uint8_t action[63];
    uint8_t attachment_lifetime[63];
    uint8_t class_of_usage[63];
    uint8_t detachment_reason[63];
    uint8_t address_type[63];
    uint32_t gssi[63];
    uint32_t extension[63];
    uint32_t vgssi[63];
} mm_group_snapshot;

/* Table 16.54 Group Identity Downlink collection. */
static int mm_parse_group_downlink_value(const uint8_t *bits, int nbits,
                                         mm_group_snapshot *snapshot)
{
    if (!bits || !snapshot || nbits < 6)
        return 0;
    int off = 0;
    uint8_t count = (uint8_t)mm_bits_to_uint(bits, off, 6); off += 6;
    if ((unsigned)snapshot->count + count > 63u)
        return 0;

    for (uint8_t i = 0; i < count; i++) {
        if (off >= nbits)
            return 0;
        unsigned dst = snapshot->count++;
        uint8_t action = bits[off++] & 1u;
        snapshot->action[dst] = action;
        if (!action) {
            if (off + 5 > nbits)
                return 0;
            snapshot->attachment_lifetime[dst] =
                (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
            snapshot->class_of_usage[dst] =
                (uint8_t)mm_bits_to_uint(bits, off, 3); off += 3;
        } else {
            if (off + 2 > nbits)
                return 0;
            snapshot->detachment_reason[dst] =
                (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
        }

        if (off + 2 > nbits)
            return 0;
        uint8_t address_type = (uint8_t)mm_bits_to_uint(bits, off, 2); off += 2;
        snapshot->address_type[dst] = address_type;
        if (address_type == 0 || address_type == 1 || address_type == 3) {
            if (off + 24 > nbits)
                return 0;
            snapshot->gssi[dst] = mm_bits_to_uint(bits, off, 24); off += 24;
        }
        if (address_type == 1 || address_type == 3) {
            if (off + 24 > nbits)
                return 0;
            snapshot->extension[dst] = mm_bits_to_uint(bits, off, 24); off += 24;
        }
        if (address_type == 2 || address_type == 3) {
            if (off + 24 > nbits)
                return 0;
            snapshot->vgssi[dst] = mm_bits_to_uint(bits, off, 24); off += 24;
        }
    }
    return off == nbits;
}

/* D-ATTACH/DETACH GROUP IDENTITY Annex E M-chain.  Type 4 identifier 7 is
 * Group Identity Downlink (table 16.95); other bounded elements are skipped. */
static int mm_parse_group_identity_tail(const uint8_t *bits, int nbits, int *off,
                                        mm_group_snapshot *snapshot)
{
    if (!bits || !off || !snapshot)
        return 0;
    for (;;) {
        if (*off >= nbits)
            return 0;
        uint8_t more = bits[(*off)++] & 1u;
        if (!more)
            return 1;
        if (*off + 15 > nbits)
            return 0;
        uint8_t identifier = (uint8_t)mm_bits_to_uint(bits, *off, 4); *off += 4;
        uint32_t length = mm_bits_to_uint(bits, *off, 11); *off += 11;
        if (length == 0 || length > (uint32_t)(nbits - *off))
            return 0;
        if (identifier == 7
            && !mm_parse_group_downlink_value(bits + *off, (int)length, snapshot))
            return 0;
        *off += (int)length;
    }
}

static void mm_parse_group_identity(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 8) {
        fprintf(stderr, "[TETRA MM D-GROUP-IDENTITY] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t report = bits[4] & 1u;
    uint8_t ack_request = bits[5] & 1u;
    uint8_t mode = bits[6] & 1u;
    int off = 7;
    mm_group_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    uint8_t optional = bits[off++] & 1u;
    if (optional && !mm_parse_group_identity_tail(bits, nbits, &off, &snapshot)) {
        fprintf(stderr, "[TETRA MM D-GROUP-IDENTITY] CC=%d (truncated optional tail)\n", cc);
        return;
    }
    if (state) {
        state->tetra_mm_group_identity_report = report;
        state->tetra_mm_group_identity_ack_request = ack_request;
        state->tetra_mm_group_identity_attach_detach_mode = mode;
        state->tetra_mm_group_entry_count = snapshot.count;
        memcpy(state->tetra_mm_group_entry_action, snapshot.action, sizeof(snapshot.action));
        memcpy(state->tetra_mm_group_entry_attachment_lifetime,
               snapshot.attachment_lifetime, sizeof(snapshot.attachment_lifetime));
        memcpy(state->tetra_mm_group_entry_class_of_usage,
               snapshot.class_of_usage, sizeof(snapshot.class_of_usage));
        memcpy(state->tetra_mm_group_entry_detachment_reason,
               snapshot.detachment_reason, sizeof(snapshot.detachment_reason));
        memcpy(state->tetra_mm_group_entry_address_type,
               snapshot.address_type, sizeof(snapshot.address_type));
        memcpy(state->tetra_mm_group_entry_gssi, snapshot.gssi, sizeof(snapshot.gssi));
        memcpy(state->tetra_mm_group_entry_extension, snapshot.extension,
               sizeof(snapshot.extension));
        memcpy(state->tetra_mm_group_entry_vgssi, snapshot.vgssi, sizeof(snapshot.vgssi));
        /* Keep the historic public fields synchronized for API compatibility. */
        state->tetra_mm_class_of_grp = report;
        state->tetra_mm_addr_type = ack_request;
        state->tetra_mm_detach_flag = mode;
        state->tetra_mm_group_identity_valid = 1;
    }
}

static void mm_parse_group_ack(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 7) {
        fprintf(stderr, "[TETRA MM D-GROUP-ACK] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t result = bits[4] & 1u;
    int off = 6; /* skip the mandatory reserved bit at bit 5 */
    uint8_t optional = bits[off++] & 1u;
    if (optional && !mm_skip_type34_tail(bits, nbits, &off)) {
        fprintf(stderr, "[TETRA MM D-GROUP-ACK] CC=%d (truncated optional tail)\n", cc);
        return;
    }
    if (state) {
        state->tetra_mm_group_ack_result = result;
        state->tetra_mm_group_ack_valid = 1;
    }
}

static void mm_parse_status(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 10) {
        fprintf(stderr, "[TETRA MM D-MM-STATUS] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    uint8_t status = (uint8_t)mm_bits_to_uint(bits, 4, 6);
    if (state) {
        state->tetra_mm_status_code = status;
        state->tetra_mm_status_valid = 1;
    }
}

static void mm_parse_fns(const uint8_t *bits, int nbits, int cc, dsd_state *state)
{
    if (nbits < 8) {
        fprintf(stderr, "[TETRA MM FUNCTION-NOT-SUPPORTED] CC=%d (too short: %d bits)\n", cc, nbits);
        return;
    }
    if (state) {
        state->tetra_mm_fns_pdu_type = (uint8_t)mm_bits_to_uint(bits, 4, 4);
        state->tetra_mm_fns_valid = 1;
    }
}

/* Public entry point: 4-bit downlink type, ETSI Table 16.75. */
void tetra_mm_dispatch(const uint8_t *bits, int nbits,
                       int cc, const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 4) {
        return;
    }

    uint32_t pdu_type = mm_bits_to_uint(bits, 0, 4);

    switch (pdu_type) {

    case TETRA_MM_D_OTAR:
        mm_parse_otar(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_AUTHENTICATION:
        mm_parse_authentication(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_CK_CHANGE_DEMAND:
        mm_parse_ck_change_demand(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_DISABLE:
        mm_parse_enable_disable(bits, nbits, cc, 0, state);
        break;

    case TETRA_MM_D_ENABLE:
        mm_parse_enable_disable(bits, nbits, cc, 1, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_ACCEPT:
        mm_parse_lu_accept(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_COMMAND:
        mm_parse_lu_command(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_REJECT:
        mm_parse_lu_reject(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_LOCATION_UPDATING_PROCEEDING:
        mm_parse_lu_proceeding(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_ATTACH_DETACH_GROUP:
        mm_parse_group_identity(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_ATTACH_DETACH_GROUP_ACK:
        mm_parse_group_ack(bits, nbits, cc, state);
        break;

    case TETRA_MM_D_MM_STATUS:
        mm_parse_status(bits, nbits, cc, state);
        break;

    case TETRA_MM_FUNCTION_NOT_SUPPORTED:
        mm_parse_fns(bits, nbits, cc, state);
        break;

    default:
        if (opts && opts->errorbars) {
            fprintf(stderr, "[TETRA MM] CC=%d  pdu_type=%u  (%d bits)\n",
                    cc, pdu_type, nbits);
        }
        break;
    }
}
