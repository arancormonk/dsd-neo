// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit tests for tetra_mm_decode() and tetra_mm_dispatch() (EN 300 392-2 clause 16, EN 300 392-7 annex A) and
 * tetra_carrier_to_dl_hz() (EN 300 392-2 clause 21.4.4.1).
 *
 * Bit layout convention: one byte per bit, value 0 or 1, MSB-first.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mm_pdu.h"
#include "tetra_state.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                                        \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* dsd_state is very large — always heap-allocate to avoid stack overflow */
static dsd_state*
alloc_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

static void
free_state(dsd_state* state) {
    dsd_state_ext_free_all(state);
    free(state);
}

/* pack an n-bit value v into bits[] starting at offset off (MSB first) */
static void
pack_bits(uint8_t* bits, int off, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        bits[off + (n - 1 - i)] = (uint8_t)((v >> i) & 1u);
    }
}

/* EN 300 392-2 clause 21.4.4.1 with the TS 100 392-15 table 1 base frequencies, and the offset codes
 * 00 = 0, 01 = +6.25 kHz, 10 = -6.25 kHz, 11 = +12.5 kHz. */
static void
test_carrier_to_dl_hz(void) {
    long hz = tetra_carrier_to_dl_hz(50, 4, 0);
    CHECK(hz == 401250000L, "band 4, carrier=50, offset=0 → 401 250 000");

    /* band 4 with offset=2 */
    hz = tetra_carrier_to_dl_hz(50, 4, 2);
    CHECK(hz == 401243750L, "band 4, carrier=50, offset code 2 (-6.25 kHz)");

    hz = tetra_carrier_to_dl_hz(50, 4, 3);
    CHECK(hz == 401262500L, "band 4, carrier=50, offset code 3 (+12.5 kHz)");

    hz = tetra_carrier_to_dl_hz(0, 8, 0);
    CHECK(hz == 800000000L, "band 8, carrier=0, offset=0 → 800 000 000");

    /* Public real-air trace: band 3, carrier 3748, offset code 3. */
    hz = tetra_carrier_to_dl_hz(3748, 3, 3);
    CHECK(hz == 393712500L, "real-air SYSINFO → 393 712 500");

    hz = tetra_carrier_to_dl_hz(4095, 9, 3);
    CHECK(hz == 1002387500L, "band 9, highest carrier and offset");
    hz = tetra_carrier_to_dl_hz(0, 1, 0);
    CHECK(hz == 100000000L, "band 1 is the lowest band with a base frequency");

    /* Frequency band 0 is a cell without a main carrier: the decoder must not invent one (a channel allocation for
     * carrier 400 on band 0 used to come out as 10 MHz). */
    CHECK(tetra_carrier_to_dl_hz(400, 0, 0) == 0L, "band 0 has no downlink frequency");
    CHECK(tetra_carrier_to_dl_hz(100, 0, 2) == 0L, "band 0 with an offset has no downlink frequency");
    for (uint32_t band = 10; band <= 15; band++) {
        CHECK(tetra_carrier_to_dl_hz(100, band, 0) == 0L, "bands 10 to 15 have no base frequency");
    }
    CHECK(tetra_carrier_to_dl_hz(0, 16, 0) == 0L, "out-of-range band → 0");
    CHECK(tetra_carrier_to_dl_hz(4096, 3, 0) == 0L, "out-of-range carrier → 0");
    CHECK(tetra_carrier_to_dl_hz(100, 4, 4) == 0L, "out-of-range offset code → 0");
}

static void
test_standard_mm_pdus(void) {
    uint8_t bits[320] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    tetra_mm_pdu pdu;
    int ok = 0;

    /* A bare PDU type is not a valid EN 300 392-7 D-DISABLE command. */
    pack_bits(bits, 0, TETRA_MM_D_DISABLE, 4);
    ok = tetra_mm_decode(bits, 4, &pdu);
    CHECK(!ok, "truncated D-DISABLE is incomplete");

    /* Table A.31: permanent equipment and subscription disable with a
     * complete 160-bit authentication challenge. */
    DSD_MEMSET(bits, 0, sizeof bits);
    int eo = 0;
    pack_bits(bits, eo, TETRA_MM_D_DISABLE, 4);
    eo += 4;
    pack_bits(bits, eo, 1, 1);
    eo++; /* confirm */
    pack_bits(bits, eo, 1, 1);
    eo++; /* permanent */
    pack_bits(bits, eo, 1, 1);
    eo++; /* equipment */
    pack_bits(bits, eo, 0x0ABCDEFA, 28);
    eo += 28;
    pack_bits(bits, eo, 0x12345678, 32);
    eo += 32;
    pack_bits(bits, eo, 1, 1);
    eo++; /* subscription */
    pack_bits(bits, eo, 460, 10);
    eo += 10;
    pack_bits(bits, eo, 4242, 14);
    eo += 14;
    pack_bits(bits, eo, 0x654321, 24);
    eo += 24;
    pack_bits(bits, eo, 1, 1);
    eo++; /* O-bit */
    pack_bits(bits, eo, 1, 1);
    eo++; /* authentication P-bit */
    for (int i = 0; i < 20; i++) {
        pack_bits(bits, eo, (uint32_t)(0x80 + i), 8);
        eo += 8;
    }
    pack_bits(bits, eo, 0, 1);
    eo++; /* terminating proprietary M-bit */
    ok = tetra_mm_decode(bits, eo, &pdu);
    CHECK(ok, "complete D-DISABLE decodes");
    CHECK(!pdu.enable_disable.is_enable && pdu.enable_disable.intent && pdu.enable_disable.permanent,
          "D-DISABLE command flags decoded");
    CHECK(pdu.enable_disable.equipment && pdu.enable_disable.tei == UINT64_C(0x0ABCDEFA12345678),
          "D-DISABLE 60-bit TEI decoded");
    CHECK(pdu.enable_disable.subscription && pdu.enable_disable.mcc == 460 && pdu.enable_disable.mnc == 4242
              && pdu.enable_disable.ssi == 0x654321,
          "D-DISABLE subscription identity decoded");
    CHECK(pdu.enable_disable.auth_valid && pdu.enable_disable.auth_challenge[0] == 0x80
              && pdu.enable_disable.auth_challenge[19] == 0x93,
          "D-DISABLE authentication challenge decoded");

    /* A missing M-bit leaves the PDU incomplete. */
    ok = tetra_mm_decode(bits, eo - 1, &pdu);
    CHECK(!ok, "truncated D-DISABLE is incomplete");

    /* Minimal table A.32 D-ENABLE, with neither conditional identity. */
    DSD_MEMSET(bits, 0, sizeof bits);
    eo = 0;
    pack_bits(bits, eo, TETRA_MM_D_ENABLE, 4);
    eo += 4;
    pack_bits(bits, eo, 0, 1);
    eo++; /* intent */
    pack_bits(bits, eo, 0, 1);
    eo++; /* equipment absent */
    pack_bits(bits, eo, 0, 1);
    eo++; /* subscription absent */
    pack_bits(bits, eo, 0, 1);
    eo++; /* O-bit */
    ok = tetra_mm_decode(bits, eo, &pdu);
    CHECK(ok && pdu.enable_disable.is_enable, "complete D-ENABLE decodes");
    CHECK(!pdu.enable_disable.equipment && !pdu.enable_disable.subscription && !pdu.enable_disable.auth_valid
              && pdu.enable_disable.tei == 0 && pdu.enable_disable.ssi == 0,
          "D-ENABLE clears absent conditional fields");

    /* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.10. */
    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    pack_bits(bits, 4, 6, 3);
    pack_bits(bits, 7, 0, 1); /* no optional elements */
    ok = tetra_mm_decode(bits, 8, &pdu);
    CHECK(ok == 1 && pdu.lu_accept.type == 6, "LU accept mandatory type");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    pack_bits(bits, 4, 2, 3);
    pack_bits(bits, 7, 1, 1); /* O-bit */
    int ao = 8;
    pack_bits(bits, ao, 1, 1);
    ao += 1;
    pack_bits(bits, ao, 0x123456, 24);
    ao += 24;
    pack_bits(bits, ao, 1, 1);
    ao += 1;
    pack_bits(bits, ao, 460, 10);
    ao += 10;
    pack_bits(bits, ao, 4242, 14);
    ao += 14;
    pack_bits(bits, ao, 1, 1);
    ao += 1;
    pack_bits(bits, ao, 0xBEEF, 16);
    ao += 16;
    pack_bits(bits, ao, 0, 1);
    ao += 1; /* no energy saving */
    pack_bits(bits, ao, 1, 1);
    ao += 1;
    pack_bits(bits, ao, 0x2D, 6);
    ao += 6;
    pack_bits(bits, ao, 0, 1);
    ao += 1; /* terminating M-bit */
    ok = tetra_mm_decode(bits, ao, &pdu);
    CHECK(ok == 1 && pdu.lu_accept.type == 2, "LU accept with Type 2 elements");
    CHECK(pdu.lu_accept.ssi_valid == 1 && pdu.lu_accept.ssi == 0x123456, "LU accept SSI");
    CHECK(pdu.lu_accept.address.valid == 1 && pdu.lu_accept.address.mcc == 460 && pdu.lu_accept.address.mnc == 4242,
          "LU accept address extension");
    CHECK(pdu.lu_accept.subscriber_class_valid == 1 && pdu.lu_accept.subscriber_class == 0xBEEF,
          "LU accept subscriber class");
    CHECK(pdu.lu_accept.energy_saving_valid == 0, "LU accept absent energy saving");
    CHECK(pdu.lu_accept.scch_valid == 1 && pdu.lu_accept.scch == 0x2D, "LU accept SCCH");
    ok = tetra_mm_decode(bits, ao - 1, &pdu);
    CHECK(ok == 0, "truncated LU accept is incomplete");

    /* Table 16.12: type(4), group-report(1), cipher-control(1), params(10). */
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_COMMAND, 4);
    pack_bits(bits, 4, 1, 1);
    pack_bits(bits, 5, 1, 1);
    pack_bits(bits, 6, 0x2AA, 10);
    pack_bits(bits, 16, 1, 1); /* O-bit */
    pack_bits(bits, 17, 1, 1); /* Address Extension P-bit */
    pack_bits(bits, 18, 460, 10);
    pack_bits(bits, 28, 4242, 14);
    pack_bits(bits, 42, 0, 1); /* terminating M-bit */
    ok = tetra_mm_decode(bits, 43, &pdu);
    CHECK(ok == 1, "LU command decodes");
    CHECK(pdu.lu_command.group_report == 1, "LU command group report");
    CHECK(pdu.lu_command.cipher_control == 1, "LU command cipher control");
    CHECK(pdu.lu_command.cipher_params == 0x2AA, "LU command cipher parameters");
    CHECK(pdu.lu_command.address.valid == 1, "LU command address extension");
    CHECK(pdu.lu_command.address.mcc == 460 && pdu.lu_command.address.mnc == 4242, "LU command MCC/MNC");

    /* A declared cipher parameter block must be complete before publication. */
    ok = tetra_mm_decode(bits, 42, &pdu);
    CHECK(ok == 0, "truncated LU command is incomplete");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_REJECT, 4);
    pack_bits(bits, 4, 3, 3);  /* location update type */
    pack_bits(bits, 7, 17, 5); /* reject cause */
    pack_bits(bits, 12, 1, 1); /* cipher on */
    pack_bits(bits, 13, 0x155, 10);
    pack_bits(bits, 23, 1, 1); /* O-bit */
    pack_bits(bits, 24, 1, 1); /* Address Extension P-bit */
    pack_bits(bits, 25, 460, 10);
    pack_bits(bits, 35, 4242, 14);
    pack_bits(bits, 49, 0, 1); /* terminating M-bit */
    ok = tetra_mm_decode(bits, 50, &pdu);
    CHECK(ok == 1, "LU reject decodes");
    CHECK(pdu.lu_reject.cause == 17, "LU reject uses 5-bit cause");
    CHECK(pdu.lu_reject.type == 3, "LU reject update type");
    CHECK(pdu.lu_reject.cipher_control == 1, "LU reject cipher control");
    CHECK(pdu.lu_reject.cipher_params == 0x155, "LU reject cipher parameters");
    CHECK(pdu.lu_reject.address.valid == 1 && pdu.lu_reject.address.mcc == 460 && pdu.lu_reject.address.mnc == 4242,
          "LU reject address extension");
    ok = tetra_mm_decode(bits, 49, &pdu);
    CHECK(ok == 0, "truncated LU reject is incomplete");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_PROCEEDING, 4);
    pack_bits(bits, 4, 0x123456, 24);
    pack_bits(bits, 28, 460, 10);  /* MCC */
    pack_bits(bits, 38, 4242, 14); /* MNC */
    pack_bits(bits, 52, 0, 1);     /* O-bit */
    ok = tetra_mm_decode(bits, 53, &pdu);
    CHECK(ok == 1, "LU proceeding decodes");
    CHECK(pdu.lu_proceeding.ssi == 0x123456, "LU proceeding SSI");
    CHECK(pdu.lu_proceeding.address.valid && pdu.lu_proceeding.address.mcc == 460
              && pdu.lu_proceeding.address.mnc == 4242,
          "LU proceeding MNI");
    ok = tetra_mm_decode(bits, 52, &pdu);
    CHECK(ok == 0, "truncated LU proceeding is incomplete");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    pack_bits(bits, 4, 1, 1); /* report */
    pack_bits(bits, 5, 1, 1); /* acknowledgement request */
    pack_bits(bits, 6, 0, 1); /* attachment mode */
    pack_bits(bits, 7, 0, 1); /* O-bit */
    ok = tetra_mm_decode(bits, 8, &pdu);
    CHECK(ok == 1, "group identity mandatory fields");
    CHECK(pdu.group_identity.report == 1 && pdu.group_identity.ack_request == 1 && pdu.group_identity.detach_all == 0,
          "group identity report and acknowledgement request");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP_ACK, 4);
    pack_bits(bits, 4, 1, 1);
    pack_bits(bits, 5, 0, 1);
    pack_bits(bits, 6, 0, 1); /* O-bit */
    ok = tetra_mm_decode(bits, 7, &pdu);
    CHECK(ok == 1 && pdu.group_ack.rejected == 1, "group identity acknowledgement result");
    ok = tetra_mm_decode(bits, 6, &pdu);
    CHECK(ok == 0, "group ACK missing O-bit is incomplete");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_MM_STATUS, 4);
    pack_bits(bits, 4, 0x2D, 6);
    ok = tetra_mm_decode(bits, 10, &pdu);
    CHECK(ok && pdu.status.status == 0x2D, "D-MM-STATUS uses 6-bit status downlink");
    tetra_mm_dispatch(bits, 10, 1, opts, state);
    CHECK(tetra_st(state)->mm_status_valid == 1 && tetra_st(state)->mm_status_code == 0x2D,
          "a complete D-MM-STATUS is the status the frontends show");
    ok = tetra_mm_decode(bits, 9, &pdu);
    CHECK(!ok, "truncated status is incomplete");
    tetra_st(state)->mm_status_code = 3;
    tetra_mm_dispatch(bits, 9, 1, opts, state);
    CHECK(tetra_st(state)->mm_status_code == 3 && tetra_st(state)->mm_status_valid == 1,
          "truncated status leaves the status the frontends show");

    DSD_MEMSET(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_FUNCTION_NOT_SUPPORTED, 4);
    pack_bits(bits, 4, TETRA_MM_D_ENABLE, 4);
    ok = tetra_mm_decode(bits, 8, &pdu);
    CHECK(ok == 0, "function-not-supported without its O-bit is incomplete");
    ok = tetra_mm_decode(bits, 9, &pdu); /* table 16.27 with the O-bit clear */
    CHECK(ok == 1 && pdu.fns.pdu_type == TETRA_MM_D_ENABLE, "function-not-supported rejected type");

    free_state(state);
    free(opts);
}

/* Expand a frozen MSB-first hexadecimal PDU into the decoder's one-byte-per-bit
 * representation. These vectors deliberately do not use pack_bits(): their
 * field boundaries remain independent of the test-side message construction
 * used by the exhaustive table cases below. */
static unsigned
hex_digit_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10u;
    }
    return ch - 'a' + 10u;
}

static void
unpack_hex_bits(const char* hex, uint8_t* bits, int nbits) {
    for (int bit = 0; bit < nbits; bit++) {
        unsigned value = hex_digit_value((unsigned char)hex[bit / 4]);
        bits[bit] = (uint8_t)((value >> (3 - (bit & 3))) & 1u);
    }
}

/* Frozen EN 300 392-7 V3.5.1 security PDUs. The exact bit counts exclude the
 * zero padding in the final hexadecimal octet. They independently pin the
 * outer PDU type and all conditional-field offsets for tables A.4, A.26 and
 * A.31 instead of rebuilding those offsets with pack_bits(). */
static void
test_security_golden_pdus(void) {
    uint8_t bits[192] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;

    /* Table A.4: successful mutual D-AUTHENTICATION RESULT, RES2=CAFEBABE,
     * no proprietary element (41 significant bits). */
    unpack_hex_bits("1BCAFEBABE00", bits, 41);
    ok = tetra_mm_decode(bits, 41, &pdu);
    CHECK(ok && pdu.auth.subtype == 2 && pdu.auth.result == 1 && pdu.auth.mutual == 1
              && pdu.auth.response == UINT32_C(0xCAFEBABE),
          "golden table A.4 authentication result offsets");

    /* Table A.26: acknowledgement, transition to class 3, combined CCK/GCK,
     * CCK-id BEEF, GCK-VN 1234, IV 2/17/42/CDEF, and the O-bit (74 bits). */
    DSD_MEMSET(bits, 0, sizeof(bits));
    unpack_hex_bits("2EEFBBC48D0A3566F780", bits, 74);
    ok = tetra_mm_decode(bits, 74, &pdu);
    CHECK(ok && pdu.ck_change.ack == 1 && pdu.ck_change.security_class == 3 && pdu.ck_change.key_type == 3
              && pdu.ck_change.cck_id == 0xBEEF && pdu.ck_change.gck_vn == 0x1234,
          "golden table A.26 key selection offsets");
    CHECK(pdu.ck_change.time.type == 0 && pdu.ck_change.time.slot == 2 && pdu.ck_change.time.frame == 17
              && pdu.ck_change.time.multiframe == 42 && pdu.ck_change.time.hyperframe == 0xCDEF,
          "golden table A.26 activation-time offsets");

    /* Table A.31: confirmed permanent equipment/subscription disable, TEI
     * 0ABCDEFA12345678, identity 460/4242/654321, no optional tail (117 bits). */
    DSD_MEMSET(bits, 0, sizeof(bits));
    unpack_hex_bits("3F579BDF42468ACF17310926543210", bits, 117);
    ok = tetra_mm_decode(bits, 117, &pdu);
    CHECK(ok && !pdu.enable_disable.is_enable && pdu.enable_disable.intent && pdu.enable_disable.permanent,
          "golden table A.31 command offsets");
    CHECK(pdu.enable_disable.equipment && pdu.enable_disable.tei == UINT64_C(0x0ABCDEFA12345678)
              && pdu.enable_disable.subscription && pdu.enable_disable.mcc == 460 && pdu.enable_disable.mnc == 4242
              && pdu.enable_disable.ssi == 0x654321 && !pdu.enable_disable.auth_valid,
          "golden table A.31 conditional identity offsets");

    /* Table A.30a: D-OTAR CMG GTSI provision for GSSI 654321 in foreign
     * network 460/4242, including the Type-2 address and terminal M-bit
     * (59 significant bits). */
    DSD_MEMSET(bits, 0, sizeof(bits));
    unpack_hex_bits("0C654321DCC42480", bits, 59);
    ok = tetra_mm_decode(bits, 59, &pdu);
    CHECK(ok && pdu.otar.subtype == 12 && pdu.otar.gssi == 0x654321 && pdu.otar.address.valid
              && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "golden table A.30a D-OTAR address offsets");
}

/* ETSI EN 300 392-7 V3.5.1 tables A.1-A.4. */
static void
test_authentication_pdus(void) {
    uint8_t bits[320] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* D-AUTHENTICATION DEMAND: full 80-bit RAND1 and 80-bit RS. */
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4);
    off += 4;
    pack_bits(bits, off, 0, 2);
    off += 2;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x10 + i), 8);
        off += 8;
    }
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 1);
    off++; /* no proprietary element */
    CHECK(off == 167, "table A.1 demand body is 167 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.auth.subtype == 0, "authentication demand decodes");
    CHECK(pdu.auth.random_challenge[0] == 0x10 && pdu.auth.random_challenge[9] == 0x19
              && pdu.auth.random_seed[0] == 0xA0 && pdu.auth.random_seed[9] == 0xA9,
          "complete 80-bit challenge and seed decoded");

    /* D-AUTHENTICATION RESPONSE with mutual RAND1. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4);
    off += 4;
    pack_bits(bits, off, 1, 2);
    off += 2;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x20 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0xDEADBEEF, 32);
    off += 32;
    pack_bits(bits, off, 1, 1);
    off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x30 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 200, "table A.3 mutual response body is 200 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.auth.subtype == 1 && pdu.auth.response == UINT32_C(0xDEADBEEF) && pdu.auth.mutual == 1,
          "mutual authentication response decoded");
    CHECK(pdu.auth.random_seed[9] == 0x29 && pdu.auth.random_challenge[9] == 0x39,
          "response seed and conditional challenge decoded");

    /* Removing any part of conditional RAND1 leaves the response incomplete. */
    pack_bits(bits, 86, 0xAAAAAAAA, 32);
    ok = tetra_mm_decode(bits, off - 2, &pdu);
    CHECK(!ok, "truncated mutual response is incomplete");

    /* D-AUTHENTICATION RESULT with conditional RES2. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4);
    off += 4;
    pack_bits(bits, off, 2, 2);
    off += 2;
    pack_bits(bits, off, 1, 1);
    off++; /* authentication success */
    pack_bits(bits, off, 1, 1);
    off++; /* mutual */
    pack_bits(bits, off, 0xCAFEBABE, 32);
    off += 32;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.auth.subtype == 2 && pdu.auth.result == 1 && pdu.auth.mutual == 1
              && pdu.auth.response == UINT32_C(0xCAFEBABE),
          "authentication result and RES2 decoded");
    CHECK(pdu.auth.random_seed[0] == 0 && pdu.auth.random_challenge[0] == 0,
          "result clears fields absent from its subtype");

    /* D-AUTHENTICATION REJECT permits only reason zero in this revision. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_AUTHENTICATION, 4);
    pack_bits(bits, 4, 3, 2);
    pack_bits(bits, 6, 0, 3);
    ok = tetra_mm_decode(bits, 9, &pdu);
    CHECK(!ok, "rejection without its O-bit is incomplete");
    ok = tetra_mm_decode(bits, 10, &pdu); /* O-bit clear (EN 300 392-2 annex E.1.1 note 4) */
    CHECK(ok && pdu.auth.subtype == 3 && pdu.auth.reject_reason == 0, "authentication-not-supported rejection decoded");
    pack_bits(bits, 6, 5, 3); /* reserved reason */
    ok = tetra_mm_decode(bits, 10, &pdu);
    CHECK(!ok, "reserved authentication reject reason is rejected");

    /* A bounded proprietary Type 3 value needs its terminal M-bit. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4);
    off += 4;
    pack_bits(bits, off, 0, 2);
    off += 2;
    for (int i = 0; i < 20; i++) {
        pack_bits(bits, off, (uint32_t)(0x40 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 1, 1);
    off++; /* O-bit */
    pack_bits(bits, off, 1, 1);
    off++; /* M-bit */
    pack_bits(bits, off, 9, 4);
    off += 4;
    pack_bits(bits, off, 8, 11);
    off += 11;
    pack_bits(bits, off, 0x5A, 8);
    off += 8;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.auth.subtype == 0 && pdu.auth.random_challenge[0] == 0x40,
          "authentication demand with proprietary tail accepted");
    pack_bits(bits, 6, 0x60, 8);
    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "missing proprietary terminal M-bit is incomplete");
}

/* ETSI EN 300 392-7 V3.5.1 tables A.9 and A.42-A.43. */
static void
test_otar_cck_provide(void) {
    uint8_t bits[640] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Current CCK for an explicit list of two location areas, plus future CCK. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 0, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x10 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 1, 2);
    off += 2;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 0x123, 14);
    off += 14;
    pack_bits(bits, off, 0x234, 14);
    off += 14;
    pack_bits(bits, off, 1, 1);
    off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x80 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 302, "table A.9 list and future-key body is 302 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 0 && pdu.otar.cck_provision, "CCK Provide decodes");
    CHECK(pdu.otar.cck.id == 0xCAFE && pdu.otar.cck.key_type == 0 && pdu.otar.cck.sealed[0] == 0x10
              && pdu.otar.cck.sealed[14] == 0x1E,
          "CCK identifier, type and full sealed key decoded");
    CHECK(pdu.otar.cck.la_type == 1 && pdu.otar.cck.la_count == 2 && pdu.otar.cck.la[0] == 0x123
              && pdu.otar.cck.la[1] == 0x234,
          "CCK location-area list decoded");
    CHECK(pdu.otar.cck.future && pdu.otar.cck.future_sealed[0] == 0x80 && pdu.otar.cck.future_sealed[14] == 0x8E,
          "future sealed CCK decoded");

    pack_bits(bits, 9, 0xBEEF, 16);
    ok = tetra_mm_decode(bits, off - 2, &pdu);
    CHECK(!ok, "truncated future CCK is incomplete");

    /* Mask/selector form clears fields that only occur in other forms. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 0, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x1357, 16);
    off += 16;
    pack_bits(bits, off, 1, 1);
    off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x30 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 2, 2);
    off += 2;
    pack_bits(bits, off, 0x3F00, 14);
    off += 14;
    pack_bits(bits, off, 0x1200, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.cck.key_type == 1 && pdu.otar.cck.la_type == 2 && pdu.otar.cck.la_mask == 0x3F00
              && pdu.otar.cck.la_selector == 0x1200,
          "future CCK mask and selector decoded");
    CHECK(pdu.otar.cck.la_count == 0 && !pdu.otar.cck.future, "mask form clears list and absent future-key fields");
    pack_bits(bits, 9, 0x7777, 16);
    pack_bits(bits, 176, 1, 1); /* forbidden for a future CCK */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "future CCK cannot carry a second future sealed key");

    /* Range form requires the high value to be greater than the low value. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 0, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x2468, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    off += 120;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 100, 14);
    off += 14;
    pack_bits(bits, off, 200, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.cck.la_type == 3 && pdu.otar.cck.la_low == 100 && pdu.otar.cck.la_high == 200,
          "CCK location-area range decoded");
    pack_bits(bits, 162, 50, 14);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "invalid CCK location-area range is rejected");

    /* Type zero applies the CCK to all location areas. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 0, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x369C, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    off += 120;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.cck.id == 0x369C && pdu.otar.cck.la_type == 0, "CCK applies to all location areas");

    /* No-provision form contains only its generic optional-element bit. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_OTAR, 4);
    pack_bits(bits, 4, 0, 4);
    pack_bits(bits, 8, 0, 1);
    pack_bits(bits, 9, 0, 1);
    ok = tetra_mm_decode(bits, 10, &pdu);
    CHECK(ok && !pdu.otar.cck_provision && pdu.otar.cck.id == 0 && pdu.otar.cck.la_type == 0,
          "CCK no-provision form clears conditional fields");
}

/* Tables A.16/A.19 and A.20/A.23: SCK and GSKO transfer paths. */
static void
test_otar_sck_and_gsko(void) {
    uint8_t bits[1400] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Two group-session-encrypted SCKs with an explicit response and MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 45, 16);
    off += 16;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x2345, 16);
    off += 16;
    pack_bits(bits, off, 2, 3);
    off += 3;
    for (int key = 0; key < 2; key++) {
        pack_bits(bits, off, (uint32_t)(3 + key), 5);
        off += 5;
        pack_bits(bits, off, (uint32_t)(0x1000 + key), 16);
        off += 16;
        pack_bits(bits, off, (uint32_t)key, 1);
        off++;
        pack_bits(bits, off, 0, 1);
        off++;
        for (int i = 0; i < 15; i++) {
            pack_bits(bits, off, (uint32_t)(0x20 + key * 0x20 + i), 8);
            off += 8;
        }
    }
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 5, 3);
    off += 3;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 366, "table A.16 two-SCK body is 366 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 2 && pdu.otar.ack && pdu.otar.explicit_response
              && pdu.otar.max_response_timer == 45,
          "SCK Provide response controls decoded");
    CHECK(pdu.otar.session_key == 1 && pdu.otar.gsko_version == 0x2345 && pdu.otar.key_count == 2,
          "SCK Provide group-session fields decoded");
    CHECK(pdu.otar.sck_number[0] == 3 && pdu.otar.sck_version[1] == 0x1001 && pdu.otar.sck_use[1] == 1
              && pdu.otar.sck_sealed[0][0] == 0x20 && pdu.otar.sck_sealed[1][14] == 0x4E,
          "both complete SCK descriptors decoded");
    CHECK(pdu.otar.ksg == 2 && pdu.otar.retry_interval == 5 && pdu.otar.address.valid && pdu.otar.address.mcc == 460
              && pdu.otar.address.mnc == 4242,
          "SCK Provide KSG, retry and address decoded");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(tetra_air_ksg_lookup(state, 460, 4242) == 2, "SCK Provide KSG remembered for the network");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated SCK optional tail is incomplete");
    pack_bits(bits, 68, 1, 1);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "non-zero SCK reserved bit is rejected");
    pack_bits(bits, 68, 0, 1);
    pack_bits(bits, 332, 5, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "SCK Provide with a reserved KSG number is rejected");

    /* Individual-session form carries the 80-bit random seed. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 8, 4);
    off += 4; /* proprietary KSG range */
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 118, "table A.16 individual-session zero-key body is 118 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 2 && !pdu.otar.session_key && pdu.otar.random_seed[0] == 0xA0
              && pdu.otar.random_seed[9] == 0xA9 && pdu.otar.key_count == 0 && pdu.otar.ksg == 8,
          "individual-session SCK seed and proprietary KSG decoded");

    /* Each rejected SCK has its own valid reject reason. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 3, 4);
    off += 4;
    pack_bits(bits, off, 2, 3);
    off += 3;
    pack_bits(bits, off, 1, 3);
    off += 3;
    pack_bits(bits, off, 7, 5);
    off += 5;
    pack_bits(bits, off, 3, 3);
    off += 3;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 6, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 31, "table A.19 two-SCK reject body is 31 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 3 && pdu.otar.key_count == 2 && pdu.otar.reject_reason[0] == 1
              && pdu.otar.sck_number[0] == 7 && pdu.otar.reject_reason[1] == 3 && pdu.otar.sck_number[1] == 31
              && pdu.otar.retry_interval == 6,
          "SCK Reject list decoded");
    pack_bits(bits, 11, 7, 3);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved SCK reject reason is rejected");

    /* GSKO Provide retains the complete seed, key, version and group. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 8, 4);
    off += 4;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x60 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0xABCD, 16);
    off += 16;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x90 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0x654321, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 8 && pdu.otar.random_seed[0] == 0x60 && pdu.otar.random_seed[9] == 0x69
              && pdu.otar.gsko_version == 0xABCD && pdu.otar.gsko_sealed[14] == 0x9E && pdu.otar.gssi == 0x654321,
          "GSKO Provide decoded");
    CHECK(pdu.otar.key_count == 0 && pdu.otar.cck.id == 0, "GSKO Provide leaves the other sub-types' fields zero");

    /* GSKO Reject is the fixed reject reason, group and retry tuple. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 9, 4);
    off += 4;
    pack_bits(bits, off, 2, 3);
    off += 3;
    pack_bits(bits, off, 0x123456, 24);
    off += 24;
    pack_bits(bits, off, 4, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 9 && pdu.otar.reject_reason[0] == 2 && pdu.otar.gssi == 0x123456
              && pdu.otar.retry_interval == 4,
          "GSKO Reject decoded");

    free_state(state);
    free(opts);
}

/* Tables A.12/A.15 and A.62/A.63b: GCK provision and rejection paths. */
static void
test_otar_gck(void) {
    uint8_t bits[1600] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Two group-session-encrypted GCKs associated with one GSSI and MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 4, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 91, 16);
    off += 16;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x3456, 16);
    off += 16;
    pack_bits(bits, off, 2, 3);
    off += 3;
    for (int key = 0; key < 2; key++) {
        pack_bits(bits, off, 0x1200, 16);
        off += 16;
        pack_bits(bits, off, (uint32_t)(0x4500 + key), 16);
        off += 16;
        for (int i = 0; i < 15; i++) {
            pack_bits(bits, off, (uint32_t)(0x30 + key * 0x20 + i), 8);
            off += 8;
        }
    }
    pack_bits(bits, off, 3, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x654321, 24);
    off += 24;
    pack_bits(bits, off, 5, 3);
    off += 3;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 409, "table A.12 two-GCK body is 409 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 4 && pdu.otar.ack && pdu.otar.explicit_response
              && pdu.otar.max_response_timer == 91,
          "GCK Provide response controls decoded");
    CHECK(pdu.otar.session_key && pdu.otar.gsko_version == 0x3456 && pdu.otar.key_count == 2,
          "GCK Provide group-session fields decoded");
    CHECK(pdu.otar.gck_number[0] == 0x1200 && pdu.otar.gck_version[1] == 0x4501 && pdu.otar.gck_sealed[0][0] == 0x30
              && pdu.otar.gck_sealed[1][14] == 0x5E,
          "both complete GCK descriptors decoded");
    CHECK(pdu.otar.ksg == 3 && pdu.otar.group_association == 1 && pdu.otar.gssi == 0x654321
              && pdu.otar.retry_interval == 5 && pdu.otar.address.valid && pdu.otar.address.mcc == 460
              && pdu.otar.address.mnc == 4242,
          "GCK Provide association, retry and address decoded");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated GCK optional tail is incomplete");
    pack_bits(bits, 8, 0, 1);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "non-zero GCK reserved response bit is rejected");
    pack_bits(bits, 8, 1, 1);
    pack_bits(bits, 350, 6, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "GCK Provide with a reserved KSG number is rejected");
    pack_bits(bits, 350, 3, 4);
    pack_bits(bits, 198, 0x1201, 16);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "GSSI association rejects mixed GCK numbers");
    pack_bits(bits, 198, 0x1200, 16);
    pack_bits(bits, 214, 0x4500, 16);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "GSSI association rejects duplicate GCK versions");

    /* Individual-session form permits zero keys and association by GCKN. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 4, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 8, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 119, "table A.12 individual-session zero-key body is 119 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 4 && !pdu.otar.session_key && pdu.otar.random_seed[0] == 0xA0
              && pdu.otar.random_seed[9] == 0xA9 && pdu.otar.key_count == 0 && pdu.otar.ksg == 8
              && !pdu.otar.group_association && pdu.otar.gssi == 0,
          "individual-session zero-key GCK fields decoded");

    /* Rejected GCKs may be identified independently by GCKN or GSSI. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 5, 4);
    off += 4;
    pack_bits(bits, off, 2, 3);
    off += 3;
    pack_bits(bits, off, 1, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 3, 3);
    off += 3;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x123456, 24);
    off += 24;
    pack_bits(bits, off, 6, 3);
    off += 3;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 63, "table A.15 mixed two-GCK reject body is 63 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 5 && pdu.otar.key_count == 2 && pdu.otar.reject_reason[0] == 1
              && !pdu.otar.gck_reject_group_association[0] && pdu.otar.gck_number[0] == 0xCAFE,
          "GCK Reject entry identified by GCKN decoded");
    CHECK(pdu.otar.reject_reason[1] == 3 && pdu.otar.gck_reject_group_association[1]
              && pdu.otar.gck_reject_gssi[1] == 0x123456 && pdu.otar.retry_interval == 6,
          "GCK Reject entry identified by GSSI decoded");

    pack_bits(bits, 11, 7, 3);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved GCK reject reason is rejected");
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_OTAR, 4);
    pack_bits(bits, 4, 5, 4);
    pack_bits(bits, 8, 0, 3);
    ok = tetra_mm_decode(bits, 15, &pdu);
    CHECK(!ok, "reserved zero GCK reject count is rejected");
}

/* Table A.24: SCK/GCK association and disassociation for GSSI lists/ranges. */
static void
test_otar_key_associate(void) {
    uint8_t bits[1000] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Associate an SCK subset with three groups in a foreign network. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 6, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 73, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 4, 6);
    off += 6;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 3, 5);
    off += 5;
    pack_bits(bits, off, 0x100001, 24);
    off += 24;
    pack_bits(bits, off, 0x100002, 24);
    off += 24;
    pack_bits(bits, off, 0x100003, 24);
    off += 24;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 141, "table A.24 three-group SCK association is 141 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 6 && pdu.otar.ack && pdu.otar.explicit_response
              && pdu.otar.max_response_timer == 73,
          "key-associate response controls decoded");
    CHECK(!pdu.otar.key_association_type && pdu.otar.sck_select == 4 && pdu.otar.sck_subset_grouping == 2,
          "SCK selection and subset grouping decoded");
    CHECK(pdu.otar.group_count == 3 && !pdu.otar.group_is_range && pdu.otar.group_value_count == 3
              && pdu.otar.group_gssi[0] == 0x100001 && pdu.otar.group_gssi[2] == 0x100003,
          "key-associate GSSI list decoded");
    CHECK(pdu.otar.address.valid && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "key-associate address extension decoded");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated key-associate tail is incomplete");
    /* Table A.24 makes the explicit response a type 1 element that only an acknowledgement request gives meaning
     * (note 3), so a demand without the request is still a demand. */
    pack_bits(bits, 8, 0, 1);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && !pdu.otar.ack && pdu.otar.explicit_response,
          "explicit response without acknowledgement is a valid key-associate demand");
    pack_bits(bits, 8, 1, 1);
    pack_bits(bits, 27, 34, 6);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved SCK selection is rejected");
    pack_bits(bits, 27, 4, 6);
    pack_bits(bits, 33, 10, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "non-demand SCK grouping value is rejected");
    pack_bits(bits, 33, 2, 4);
    pack_bits(bits, 37, 0, 5);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved zero group count is rejected");

    /* Disassociate GCKs from an ordered range, returning the groups to CCK. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 6, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 16);
    off += 16;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x10000, 17);
    off += 17;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 1000, 24);
    off += 24;
    pack_bits(bits, off, 2000, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 98, "table A.24 GCK range disassociation is 98 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 6 && pdu.otar.key_association_type && pdu.otar.gck_select == 0x10000,
          "GCK disassociation selection decoded");
    CHECK(pdu.otar.group_count == 31 && pdu.otar.group_is_range && pdu.otar.group_value_count == 2
              && pdu.otar.group_gssi[0] == 1000 && pdu.otar.group_gssi[1] == 2000,
          "ordered GSSI range decoded");
    CHECK(pdu.otar.sck_select == 0 && !pdu.otar.address.valid,
          "GCK association leaves the other sub-types' fields zero");

    pack_bits(bits, 27, 0x10001, 17);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved GCK selection is rejected");
    pack_bits(bits, 27, 0x10000, 17);
    pack_bits(bits, 73, 999, 24);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reversed GSSI range is rejected");
}

/* Table A.30: DCK forwarding result with optional CCK information. */
static void
test_otar_newcell(void) {
    uint8_t bits[640] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 7, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x70 + i), 8);
        off += 8;
    }
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 151, "table A.30 current-CCK NEWCELL is 151 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 7 && pdu.otar.dck_forwarding_result && pdu.otar.cck_provision,
          "NEWCELL forwarding and CCK flags decoded");
    CHECK(pdu.otar.cck.id == 0xCAFE && pdu.otar.cck.key_type == 0 && pdu.otar.cck.sealed[0] == 0x70
              && pdu.otar.cck.sealed[14] == 0x7E && pdu.otar.cck.la_type == 0,
          "NEWCELL complete CCK information decoded");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated NEWCELL is incomplete");
    pack_bits(bits, 26, 1, 1);
    pack_bits(bits, 149, 1, 1);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "future CCK cannot include another future key in NEWCELL");

    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 7, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 11, "table A.30 no-CCK NEWCELL is 11 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 7 && !pdu.otar.dck_forwarding_result && !pdu.otar.cck_provision
              && pdu.otar.cck.id == 0,
          "NEWCELL without CCK clears conditional fields");
}

/* Table A.30a: crypto-management group address provision. */
static void
test_otar_cmg_gtsi(void) {
    uint8_t bits[128] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 12, 4);
    off += 4;
    pack_bits(bits, off, 0x654321, 24);
    off += 24;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 59, "table A.30a CMG GTSI with address is 59 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 12 && pdu.otar.gssi == 0x654321, "CMG GTSI decoded");
    CHECK(pdu.otar.address.valid && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "CMG GTSI foreign-network address decoded");
    CHECK(!pdu.otar.cck_provision && pdu.otar.group_count == 0, "CMG GTSI leaves the other sub-types' fields zero");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated CMG GTSI tail is incomplete");
}

/* Table A.27f: delete selected, grouped, subset, or complete key classes. */
static void
test_otar_key_delete(void) {
    uint8_t bits[256] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Delete three KAG members in a foreign network. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 10, 4);
    off += 4;
    pack_bits(bits, off, 1, 3);
    off += 3;
    pack_bits(bits, off, 3, 5);
    off += 5;
    pack_bits(bits, off, 0, 5);
    off += 5;
    pack_bits(bits, off, 15, 5);
    off += 5;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 58, "table A.27f KAG deletion with address is 58 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 10 && pdu.otar.key_delete_type == 1 && pdu.otar.key_delete_sck_count == 3,
          "KAG deletion type and count decoded");
    CHECK(pdu.otar.key_delete_sck_number[0] == 0 && pdu.otar.key_delete_sck_number[1] == 15
              && pdu.otar.key_delete_sck_number[2] == 31,
          "all KAG SCK identifiers decoded");
    CHECK(pdu.otar.address.valid && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "key-delete address extension decoded");

    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "truncated key-delete optional tail is incomplete");

    /* Delete one validated SCK subset. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 10, 4);
    off += 4;
    pack_bits(bits, off, 2, 3);
    off += 3;
    pack_bits(bits, off, 8, 4);
    off += 4;
    pack_bits(bits, off, 15, 5);
    off += 5;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 21, "table A.27f subset deletion is 21 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.key_delete_type == 2 && pdu.otar.key_delete_sck_grouping == 8
              && pdu.otar.key_delete_sck_subset == 15 && pdu.otar.key_delete_sck_count == 0,
          "SCK subset deletion decoded with an empty list");

    pack_bits(bits, 15, 16, 5);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "out-of-group delete subset is rejected");
    pack_bits(bits, 15, 15, 5);
    pack_bits(bits, 11, 10, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved delete grouping is rejected");

    /* Delete two named GCKs. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 10, 4);
    off += 4;
    pack_bits(bits, off, 4, 3);
    off += 3;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 0x1234, 16);
    off += 16;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 48, "table A.27f two-GCK deletion is 48 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.key_delete_type == 4 && pdu.otar.key_delete_gck_count == 2
              && pdu.otar.key_delete_gck_number[0] == 0x1234 && pdu.otar.key_delete_gck_number[1] == 0xCAFE,
          "individual GCK deletion list decoded");
    CHECK(pdu.otar.key_delete_sck_grouping == 0, "GCK deletion leaves the SCK subset fields zero");

    /* All-SCK, all-GCK, and GSKO deletions have no conditional body. */
    const uint8_t whole_types[] = {3, 5, 6};
    for (unsigned i = 0; i < sizeof(whole_types); i++) {
        DSD_MEMSET(bits, 0, sizeof(bits));
        off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
        off += 4;
        pack_bits(bits, off, 10, 4);
        off += 4;
        pack_bits(bits, off, whole_types[i], 3);
        off += 3;
        pack_bits(bits, off, 0, 1);
        off++;
        CHECK(off == 12, "table A.27f whole-class deletion is 12 bits");
        ok = tetra_mm_decode(bits, off, &pdu);
        CHECK(ok && pdu.otar.key_delete_type == whole_types[i] && pdu.otar.key_delete_gck_count == 0,
              "whole key-class deletion decoded");
    }

    pack_bits(bits, 8, 7, 3);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "downlink key-delete extension type is rejected");
}

/* Table A.27h: request the status of individual or grouped traffic keys. */
static void
test_otar_key_status_demand(void) {
    uint8_t bits[128] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Request the last valid subset in grouping 7, with a foreign MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 11, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x1234, 16);
    off += 16;
    pack_bits(bits, off, 1, 3);
    off += 3;
    pack_bits(bits, off, 7, 4);
    off += 4;
    pack_bits(bits, off, 10, 5);
    off += 5;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 65, "table A.27h subset request with address is 65 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 11 && pdu.otar.ack && pdu.otar.explicit_response
              && pdu.otar.max_response_timer == 0x1234,
          "key-status response controls decoded");
    CHECK(pdu.otar.key_status_type == 1 && pdu.otar.key_status_sck_grouping == 7
              && pdu.otar.key_status_sck_subset == 10,
          "key-status SCK subset decoded");
    CHECK(pdu.otar.address.valid && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "key-status address extension decoded");

    pack_bits(bits, 33, 11, 5);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "out-of-group SCK subset is rejected");
    pack_bits(bits, 33, 10, 5);
    pack_bits(bits, 29, 10, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved SCK grouping is rejected");
    pack_bits(bits, 29, 7, 4);
    pack_bits(bits, 8, 0, 1);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.key_status_type == 1 && !pdu.otar.ack && pdu.otar.explicit_response,
          "key-status explicit response without acknowledgement is a valid demand (table A.27h note 1)");

    /* Individual SCK and GCK identifiers use their complete encoded ranges. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 11, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 16);
    off += 16;
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 35, "table A.27h individual SCK request is 35 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.key_status_type == 0 && pdu.otar.key_status_sck_number == 31 && !pdu.otar.address.valid,
          "individual SCK status request decoded");

    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 11, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 9, 16);
    off += 16;
    pack_bits(bits, off, 3, 3);
    off += 3;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 46, "table A.27h individual GCK request is 46 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.key_status_type == 3 && pdu.otar.key_status_gck_number == 0xCAFE
              && pdu.otar.key_status_sck_number == 0,
          "individual GCK status request decoded with no SCK selection");

    /* Aggregate SCK, aggregate GCK and GSKO requests have no identifier. */
    for (uint8_t type = 2; type <= 5; type += (uint8_t)(type == 2 ? 2 : 1)) {
        DSD_MEMSET(bits, 0, sizeof(bits));
        off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
        off += 4;
        pack_bits(bits, off, 11, 4);
        off += 4;
        pack_bits(bits, off, 0, 1);
        off++;
        pack_bits(bits, off, 0, 1);
        off++;
        pack_bits(bits, off, 0, 16);
        off += 16;
        pack_bits(bits, off, type, 3);
        off += 3;
        pack_bits(bits, off, 0, 1);
        off++;
        CHECK(off == 30, "table A.27h aggregate request is 30 bits");
        ok = tetra_mm_decode(bits, off, &pdu);
        CHECK(ok && pdu.otar.key_status_type == type && pdu.otar.key_status_gck_number == 0,
              "aggregate key-status request decoded");
    }

    pack_bits(bits, 26, 6, 3);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "response-only reject type is rejected");
    pack_bits(bits, 26, 7, 3);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved key-status type is rejected");
}

/* Table A.27d: activate DMO SCK subsets or individual keys for another MNI. */
static void
test_otar_dm_sck_activate(void) {
    uint8_t bits[256] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Activate a complete subset at an absolute IV. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 13, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0, 4);
    off += 4;
    pack_bits(bits, off, 4, 4);
    off += 4;
    pack_bits(bits, off, 5, 5);
    off += 5;
    pack_bits(bits, off, 0xBEEF, 16);
    off += 16;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 17, 5);
    off += 5;
    pack_bits(bits, off, 45, 6);
    off += 6;
    pack_bits(bits, off, 0x2468, 16);
    off += 16;
    pack_bits(bits, off, 460, 10);
    off += 10;
    pack_bits(bits, off, 4242, 14);
    off += 14;
    off++; /* O-bit: no optional element (EN 300 392-2 annex E.1.1 note 4) */
    CHECK(off == 94, "table A.27d subset absolute-IV demand is 94 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 13 && pdu.otar.dm_sck_ack && pdu.otar.dm_sck.count == 0,
          "DM-SCK subset demand and acknowledgement decoded");
    CHECK(pdu.otar.dm_sck.grouping == 4 && pdu.otar.dm_sck.subset == 5 && pdu.otar.dm_sck.subset_vn == 0xBEEF,
          "DM-SCK subset identity decoded");
    CHECK(pdu.otar.dm_sck_time.type == 0 && pdu.otar.dm_sck_time.slot == 3 && pdu.otar.dm_sck_time.frame == 17
              && pdu.otar.dm_sck_time.multiframe == 45 && pdu.otar.dm_sck_time.hyperframe == 0x2468,
          "DM-SCK absolute activation time decoded");
    CHECK(pdu.otar.address.valid && pdu.otar.address.mcc == 460 && pdu.otar.address.mnc == 4242,
          "mandatory DMO network MNI decoded");

    ok = tetra_mm_decode(bits, off - 2, &pdu);
    CHECK(!ok, "truncated mandatory DMO MNI is incomplete");
    pack_bits(bits, 53, 0x1111, 16);
    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "DM-SCK demand without its O-bit is incomplete");
    pack_bits(bits, 53, 0x2468, 16);
    pack_bits(bits, 17, 6, 5);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "out-of-group DM-SCK subset is rejected");
    pack_bits(bits, 17, 5, 5);
    pack_bits(bits, 13, 10, 4);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved DM-SCK grouping is rejected");

    /* Activate two named SCKs at a 48-bit network time. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 13, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 3, 5);
    off += 5;
    pack_bits(bits, off, 0x1234, 16);
    off += 16;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 0xABCD, 16);
    off += 16;
    pack_bits(bits, off, 1, 2);
    off += 2;
    pack_bits(bits, off, 0x5678, 16);
    off += 16;
    pack_bits(bits, off, 0x9ABCDEF0, 32);
    off += 32;
    pack_bits(bits, off, 302, 10);
    off += 10;
    pack_bits(bits, off, 100, 14);
    off += 14;
    off++; /* O-bit */
    CHECK(off == 130, "table A.27d two-SCK network-time demand is 130 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.dm_sck.count == 2 && pdu.otar.dm_sck.number[0] == 3 && pdu.otar.dm_sck.version[0] == 0x1234
              && pdu.otar.dm_sck.number[1] == 31 && pdu.otar.dm_sck.version[1] == 0xABCD,
          "repeated DM-SCK descriptors decoded");
    CHECK(pdu.otar.dm_sck_time.type == 1 && pdu.otar.dm_sck_time.network_time == UINT64_C(0x56789ABCDEF0)
              && pdu.otar.address.mcc == 302 && pdu.otar.address.mnc == 100,
          "DM-SCK network time and DMO network decoded");
    CHECK(pdu.otar.dm_sck.grouping == 0 && pdu.otar.dm_sck.subset_vn == 0,
          "individual DM-SCK demand leaves the subset fields zero");

    /* Immediate and already-active demands carry no additional time value. */
    for (uint8_t time_type = 2; time_type <= 3; time_type++) {
        DSD_MEMSET(bits, 0, sizeof(bits));
        off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
        off += 4;
        pack_bits(bits, off, 13, 4);
        off += 4;
        pack_bits(bits, off, 0, 1);
        off++;
        pack_bits(bits, off, 1, 4);
        off += 4;
        pack_bits(bits, off, 7, 5);
        off += 5;
        pack_bits(bits, off, 0x7000 + time_type, 16);
        off += 16;
        pack_bits(bits, off, time_type, 2);
        off += 2;
        pack_bits(bits, off, 1, 10);
        off += 10;
        pack_bits(bits, off, 2, 14);
        off += 14;
        off++; /* O-bit */
        CHECK(off == 61, "table A.27d implicit-time demand is 61 bits");
        ok = tetra_mm_decode(bits, off, &pdu);
        CHECK(ok && pdu.otar.dm_sck_time.type == time_type && pdu.otar.dm_sck_time.network_time == 0,
              "DM-SCK implicit activation time decoded");
    }
}

/* ETSI EN 300 392-7 V3.5.1 table A.26. */
static void
test_ck_change_demand(void) {
    uint8_t bits[320] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    /* Two individually named TMO SCKs, activated immediately. */
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++; /* acknowledgement */
    pack_bits(bits, off, 3, 2);
    off += 2; /* transition to class 3 */
    pack_bits(bits, off, 0, 3);
    off += 3; /* SCK */
    pack_bits(bits, off, 0, 1);
    off++; /* TMO */
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 3, 5);
    off += 5;
    pack_bits(bits, off, 0xABCD, 16);
    off += 16;
    pack_bits(bits, off, 31, 5);
    off += 5;
    pack_bits(bits, off, 0x1234, 16);
    off += 16;
    pack_bits(bits, off, 2, 2);
    off += 2; /* immediate */
    off++;    /* O-bit: no optional element (EN 300 392-2 annex E.1.1 note 4) */
    CHECK(off == 60, "table A.26 two-SCK body is 60 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.ack, "SCK change decodes with its acknowledgement");
    CHECK(pdu.ck_change.security_class == 3 && pdu.ck_change.key_type == 0 && pdu.ck_change.sck.count == 2,
          "SCK change mandatory fields");
    CHECK(pdu.ck_change.sck.number[0] == 3 && pdu.ck_change.sck.version[0] == 0xABCD
              && pdu.ck_change.sck.number[1] == 31 && pdu.ck_change.sck.version[1] == 0x1234,
          "both repeated SCK descriptors decoded");
    CHECK(pdu.ck_change.time.type == 2, "immediate SCK activation decoded");

    /* The final bit of Time Type and the O-bit are mandatory; truncation is atomic. */
    pack_bits(bits, 15, 7, 5);
    ok = tetra_mm_decode(bits, off - 2, &pdu);
    CHECK(!ok, "truncated SCK demand is incomplete");
    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "SCK demand without its O-bit is incomplete");

    /* Zero SCK count selects a DMO subset and carries its common version. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 0, 3);
    off += 3;
    pack_bits(bits, off, 1, 1);
    off++; /* DMO */
    pack_bits(bits, off, 0, 4);
    off += 4; /* subset */
    pack_bits(bits, off, 9, 4);
    off += 4;
    pack_bits(bits, off, 17, 5);
    off += 5;
    pack_bits(bits, off, 0xBEEF, 16);
    off += 16;
    pack_bits(bits, off, 3, 2);
    off += 2; /* currently in use */
    off++;    /* O-bit */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.sck_use == 1 && pdu.ck_change.sck.count == 0 && pdu.ck_change.sck.grouping == 9
              && pdu.ck_change.sck.subset == 17 && pdu.ck_change.sck.subset_vn == 0xBEEF,
          "DMO SCK subset fields decoded");

    /* Two GCK descriptors followed by a 48-bit network time. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 2, 3);
    off += 3; /* GCK */
    pack_bits(bits, off, 2, 4);
    off += 4;
    pack_bits(bits, off, 0x0102, 16);
    off += 16;
    pack_bits(bits, off, 0x0304, 16);
    off += 16;
    pack_bits(bits, off, 0xA0B0, 16);
    off += 16;
    pack_bits(bits, off, 0xC0D0, 16);
    off += 16;
    pack_bits(bits, off, 1, 2);
    off += 2; /* network time */
    pack_bits(bits, off, 0x1234, 16);
    off += 16;
    pack_bits(bits, off, 0x56789ABC, 32);
    off += 32;
    off++; /* O-bit */
    CHECK(off == 129, "table A.26 two-GCK network-time body is 129 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.key_type == 2 && pdu.ck_change.gck_count == 2, "GCK change and repeat count decoded");
    CHECK(pdu.ck_change.gck_number[0] == 0x0102 && pdu.ck_change.gck_version[0] == 0x0304
              && pdu.ck_change.gck_number[1] == 0xA0B0 && pdu.ck_change.gck_version[1] == 0xC0D0,
          "both repeated GCK descriptors decoded");
    CHECK(pdu.ck_change.time.network_time == UINT64_C(0x123456789ABC), "48-bit key-change network time decoded");

    /* Combined Class 3 activation at an absolute IV. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 3, 3);
    off += 3;
    pack_bits(bits, off, 0xCAFE, 16);
    off += 16;
    pack_bits(bits, off, 0x1357, 16);
    off += 16;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 17, 5);
    off += 5;
    pack_bits(bits, off, 45, 6);
    off += 6;
    pack_bits(bits, off, 0x2468, 16);
    off += 16;
    off++; /* O-bit */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.key_type == 3 && pdu.ck_change.cck_id == 0xCAFE && pdu.ck_change.gck_vn == 0x1357,
          "combined Class 3 CCK/GCK fields decoded");
    CHECK(pdu.ck_change.time.type == 0 && pdu.ck_change.time.slot == 3 && pdu.ck_change.time.frame == 17
              && pdu.ck_change.time.multiframe == 45 && pdu.ck_change.time.hyperframe == 0x2468,
          "absolute IV fields decoded");

    /* Reserved key-change types and a TMO zero-SCK count are invalid. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    pack_bits(bits, 7, 6, 3);
    ok = tetra_mm_decode(bits, 10, &pdu);
    CHECK(!ok, "reserved key-change type is rejected");
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    pack_bits(bits, 7, 0, 3);
    pack_bits(bits, 10, 0, 1); /* TMO */
    pack_bits(bits, 11, 0, 4); /* invalid subset count */
    pack_bits(bits, 15, 2, 2);
    ok = tetra_mm_decode(bits, 18, &pdu);
    CHECK(!ok, "TMO zero-SCK count is rejected");

    /* Remaining table A.71 key types have their own conditional widths. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 1, 3);
    off += 3; /* CCK */
    pack_bits(bits, off, 0x9876, 16);
    off += 16;
    pack_bits(bits, off, 3, 2);
    off += 2;
    off++; /* O-bit */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.key_type == 1 && pdu.ck_change.cck_id == 0x9876, "individual CCK change decoded");
    CHECK(pdu.ck_change.sck.count == 0 && pdu.ck_change.gck_count == 0,
          "CCK change leaves the repeated-key lists zero");

    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 0, 2);
    off += 2;
    pack_bits(bits, off, 4, 3);
    off += 3; /* all GCKs */
    pack_bits(bits, off, 0x7777, 16);
    off += 16;
    pack_bits(bits, off, 2, 2);
    off += 2;
    off++; /* O-bit */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.key_type == 4 && pdu.ck_change.gck_vn == 0x7777, "all-GCK version change decoded");

    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 1, 2);
    off += 2;
    pack_bits(bits, off, 5, 3);
    off += 3; /* no cipher key */
    pack_bits(bits, off, 3, 2);
    off += 2;
    off++; /* O-bit */
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.key_type == 5 && pdu.ck_change.security_class == 1 && pdu.ck_change.time.type == 3,
          "no-cipher security-class change decoded");
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.11.  The example carries
 * Subscriber Class followed by New Registered Area and Group Identity
 * Location Accept Type 4 elements.  Their nested contents are opaque to the
 * current state model, but every standardized length and terminating M-bit
 * must be present before the visible LU Accept state is replaced. */
static void
test_annex_e11_lu_accept_type4_chain(void) {
    uint8_t bits[256] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    off += 4;
    pack_bits(bits, off, 5, 3);
    off += 3; /* accept type: any */
    pack_bits(bits, off, 1, 1);
    off++; /* O-bit */
    pack_bits(bits, off, 0, 1);
    off++; /* no SSI */
    pack_bits(bits, off, 0, 1);
    off++; /* no address extension */
    pack_bits(bits, off, 1, 1);
    off++; /* Subscriber Class present */
    pack_bits(bits, off, 0xA55A, 16);
    off += 16;
    pack_bits(bits, off, 0, 1);
    off++; /* no energy saving */
    pack_bits(bits, off, 0, 1);
    off++; /* no SCCH information */

    pack_bits(bits, off, 1, 1);
    off++; /* first outer M-bit */
    pack_bits(bits, off, 2, 4);
    off += 4; /* New Registered Area */
    pack_bits(bits, off, 42, 11);
    off += 11;
    pack_bits(bits, off, 2, 6);
    off += 6; /* two repeated LA entries */
    pack_bits(bits, off, 3, 3);
    off += 3; /* first LA timer */
    pack_bits(bits, off, 0x1234, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++; /* first LA O-bit */
    pack_bits(bits, off, 6, 3);
    off += 3; /* second LA timer */
    pack_bits(bits, off, 0x2345, 14);
    off += 14;
    pack_bits(bits, off, 0, 1);
    off++; /* second LA O-bit */

    pack_bits(bits, off, 1, 1);
    off++; /* another outer element */
    pack_bits(bits, off, 5, 4);
    off += 4; /* Group Identity Location Accept */
    pack_bits(bits, off, 55, 11);
    off += 11;
    pack_bits(bits, off, 1, 1);
    off++; /* group identity accepted */
    pack_bits(bits, off, 0, 1);
    off++; /* reserved */
    pack_bits(bits, off, 1, 1);
    off++; /* nested O-bit */
    pack_bits(bits, off, 1, 1);
    off++; /* nested M-bit */
    pack_bits(bits, off, 7, 4);
    off += 4; /* Group Identity Downlink */
    pack_bits(bits, off, 35, 11);
    off += 11;
    pack_bits(bits, off, 1, 6);
    off += 6; /* one repeated identity */
    pack_bits(bits, off, 1, 1);
    off++; /* detachment */
    pack_bits(bits, off, 2, 2);
    off += 2; /* detachment reason: any */
    pack_bits(bits, off, 0, 2);
    off += 2; /* address type GSSI */
    pack_bits(bits, off, 0x456789, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++; /* nested terminal M-bit */
    pack_bits(bits, off, 0, 1);
    off++; /* outer terminal M-bit */
    CHECK(off == 159, "Annex E.11 MM body is exactly 159 bits");

    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok == 1 && pdu.lu_accept.type == 5, "Annex E.11 LU Accept accepted");
    CHECK(pdu.lu_accept.subscriber_class_valid == 1 && pdu.lu_accept.subscriber_class == 0xA55A,
          "Annex E.11 Subscriber Class decoded");

    /* Change visible fields, then stop inside the second outer Type 4 value.
     * The previous complete message must remain visible. */
    pack_bits(bits, 4, 2, 3);
    pack_bits(bits, 11, 0xBEEF, 16);
    ok = tetra_mm_decode(bits, 157, &pdu);
    CHECK(!ok, "truncated Annex E.11 nested value is incomplete");

    /* Both values are complete at bit 158; the outer terminal M-bit remains
     * mandatory for the complete D-LOCATION UPDATE ACCEPT envelope. */
    ok = tetra_mm_decode(bits, 158, &pdu);
    CHECK(!ok, "missing Annex E.11 outer M-bit is incomplete");
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.9. */
static void
test_annex_e9_group_identity_downlink(void) {
    uint8_t bits[256] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    off += 4;
    pack_bits(bits, off, 0, 1);
    off++; /* no group identity report */
    pack_bits(bits, off, 1, 1);
    off++; /* acknowledgement requested */
    pack_bits(bits, off, 0, 1);
    off++; /* attach/detach mode: any */
    pack_bits(bits, off, 1, 1);
    off++; /* O-bit */
    pack_bits(bits, off, 1, 1);
    off++; /* M-bit */
    pack_bits(bits, off, 7, 4);
    off += 4; /* Group Identity Downlink */
    pack_bits(bits, off, 67, 11);
    off += 11;
    pack_bits(bits, off, 2, 6);
    off += 6; /* two repeated entries */
    pack_bits(bits, off, 0, 1);
    off++; /* first: attachment */
    pack_bits(bits, off, 2, 2);
    off += 2; /* attachment lifetime */
    pack_bits(bits, off, 5, 3);
    off += 3; /* class of usage */
    pack_bits(bits, off, 0, 2);
    off += 2; /* GSSI address */
    pack_bits(bits, off, 0x123456, 24);
    off += 24;
    pack_bits(bits, off, 1, 1);
    off++; /* second: detachment */
    pack_bits(bits, off, 3, 2);
    off += 2; /* detachment reason */
    pack_bits(bits, off, 0, 2);
    off += 2; /* GSSI address */
    pack_bits(bits, off, 0x654321, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++; /* terminal M-bit */
    CHECK(off == 92, "Annex E.9 MM body is exactly 92 bits");

    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok == 1 && pdu.group_identity.count == 2, "Annex E.9 two-entry Group Identity Downlink accepted");
    CHECK(pdu.group_identity.entry[0].action == 0 && pdu.group_identity.entry[0].attachment_lifetime == 2
              && pdu.group_identity.entry[0].class_of_usage == 5 && pdu.group_identity.entry[0].gssi == 0x123456,
          "Annex E.9 attachment fields decoded");
    CHECK(pdu.group_identity.entry[1].action == 1 && pdu.group_identity.entry[1].detachment_reason == 3
              && pdu.group_identity.entry[1].gssi == 0x654321,
          "Annex E.9 detachment fields decoded");

    /* A complete 67-bit collection still needs the outer terminal M-bit. */
    pack_bits(bits, 38, 0x234567, 24);
    ok = tetra_mm_decode(bits, off - 1, &pdu);
    CHECK(!ok, "missing Annex E.9 terminal M-bit is incomplete");

    /* Table 16.54 conditional address forms: GTSI, (V)GSSI, and
     * GTSI-(V)GSSI. */
    DSD_MEMSET(bits, 0, sizeof(bits));
    off = 0;
    pack_bits(bits, off, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    off += 4;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 7, 4);
    off += 4;
    pack_bits(bits, off, 171, 11);
    off += 11;
    pack_bits(bits, off, 3, 6);
    off += 6;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 1, 2);
    off += 2;
    pack_bits(bits, off, 2, 3);
    off += 3;
    pack_bits(bits, off, 1, 2);
    off += 2;
    pack_bits(bits, off, 0x111111, 24);
    off += 24;
    pack_bits(bits, off, 0x222222, 24);
    off += 24;
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 2, 2);
    off += 2;
    pack_bits(bits, off, 2, 2);
    off += 2;
    pack_bits(bits, off, 0x333333, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 7, 3);
    off += 3;
    pack_bits(bits, off, 3, 2);
    off += 2;
    pack_bits(bits, off, 0x444444, 24);
    off += 24;
    pack_bits(bits, off, 0x555555, 24);
    off += 24;
    pack_bits(bits, off, 0x666666, 24);
    off += 24;
    pack_bits(bits, off, 0, 1);
    off++;
    CHECK(off == 196, "table 16.54 conditional-address body is 196 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.group_identity.count == 3, "three conditional group-address forms accepted");
    CHECK(pdu.group_identity.entry[0].address_type == 1 && pdu.group_identity.entry[0].gssi == 0x111111
              && pdu.group_identity.entry[0].address.valid
              && pdu.group_identity.entry[0].address.mcc == (0x222222 >> 14)
              && pdu.group_identity.entry[0].address.mnc == (0x222222 & 0x3FFF),
          "GTSI group address decoded");
    CHECK(pdu.group_identity.entry[1].address_type == 2 && pdu.group_identity.entry[1].vgssi == 0x333333,
          "visitor GSSI decoded");
    CHECK(pdu.group_identity.entry[2].address_type == 3 && pdu.group_identity.entry[2].gssi == 0x444444
              && pdu.group_identity.entry[2].address.mcc == (0x555555 >> 14)
              && pdu.group_identity.entry[2].address.mnc == (0x555555 & 0x3FFF)
              && pdu.group_identity.entry[2].vgssi == 0x666666,
          "GTSI with visitor GSSI decoded");

    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    pack_bits(bits, 4, 1, 1);
    pack_bits(bits, 5, 0, 1);
    pack_bits(bits, 6, 0, 1);
    pack_bits(bits, 7, 0, 1); /* no optional entries */
    ok = tetra_mm_decode(bits, 8, &pdu);
    CHECK(ok && pdu.group_identity.count == 0 && pdu.group_identity.entry[0].gssi == 0,
          "group message without entries decodes none");
}

/* Table A.100 gives SCK select number 33 to "SCKN dissociated"; only 34 to 63 are reserved. */
static void
test_otar_key_associate_dissociation(void) {
    uint8_t bits[96] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_OTAR, 4);
    off += 4;
    pack_bits(bits, off, 6, 4);
    off += 4; /* KEY ASSOCIATE demand */
    off += 2; /* no acknowledgement, no explicit response */
    off += 16;
    off += 1; /* SCK association */
    pack_bits(bits, off, 33, 6);
    off += 6; /* SCKN dissociated */
    off += 4; /* grouping 0 */
    pack_bits(bits, off, 1, 5);
    off += 5;
    pack_bits(bits, off, 0x100001, 24);
    off += 24;
    off++; /* O-bit */
    CHECK(off == 67, "table A.24 single-group dissociation is 67 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.otar.subtype == 6 && pdu.otar.sck_select == 33 && pdu.otar.group_gssi[0] == 0x100001,
          "SCK dissociation demand accepted");

    pack_bits(bits, 27, 34, 6);
    pack_bits(bits, 42, 0x100002, 24);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "reserved SCK select 34 is rejected");
}

/* Table A.26 with table A.100b note 1: a DMO subset number cannot exceed the subsets of its grouping. */
static void
test_ck_change_subset_range(void) {
    uint8_t bits[64] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    off += 4;
    off += 1; /* no acknowledgement */
    off += 2; /* no security class change */
    off += 3; /* SCK */
    pack_bits(bits, off, 1, 1);
    off++;    /* DMO */
    off += 4; /* subset */
    off += 4; /* grouping 0: one subset of 30 keys */
    pack_bits(bits, off, 1, 5);
    off += 5;
    pack_bits(bits, off, 0x4321, 16);
    off += 16;
    pack_bits(bits, off, 2, 2);
    off += 2; /* immediate */
    off++;    /* O-bit */
    CHECK(off == 43, "table A.26 DMO subset body is 43 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.ck_change.sck.subset == 1 && pdu.ck_change.sck.subset_vn == 0x4321,
          "subset 1 of grouping 0 accepted");

    pack_bits(bits, 19, 2, 5);
    pack_bits(bits, 24, 0x1111, 16);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "subset 2 of a one-subset grouping is rejected");
    pack_bits(bits, 15, 10, 4);
    pack_bits(bits, 19, 1, 5);
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "grouping 10 is not a demand grouping");
}

/* EN 300 392-2 annex E.1.1 reserves zero repeated elements in a type 4 element. */
static void
test_group_downlink_empty_element(void) {
    uint8_t bits[64] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    off += 4;
    off += 3; /* report, acknowledgement request, mode */
    pack_bits(bits, off, 1, 1);
    off++; /* O-bit */
    pack_bits(bits, off, 1, 1);
    off++; /* M-bit */
    pack_bits(bits, off, 7, 4);
    off += 4; /* group identity downlink */
    pack_bits(bits, off, 6, 11);
    off += 11;
    off += 6; /* zero repeated elements */
    off++;    /* terminal M-bit */
    CHECK(off == 31, "empty group identity downlink body is 31 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(!ok, "group identity downlink with no elements is rejected");
}

/* EN 300 392-2 tables 16.4 to 16.10: a D-MM STATUS sub-PDU is published only when it is complete. */
static void
test_mm_status_sub_pdus(void) {
    uint8_t bits[64] = {0};
    dsd_state* state = alloc_state();
    dsd_opts* opts = alloc_opts();

    /* Table 16.4: energy saving information, then the O-bit. */
    pack_bits(bits, 0, TETRA_MM_D_MM_STATUS, 4);
    pack_bits(bits, 4, 1, 6);
    pack_bits(bits, 10, 0x1ABC, 14);
    tetra_mm_dispatch(bits, 24, 1, opts, state);
    CHECK(!tetra_st(state)->mm_status_valid, "energy saving request without its O-bit is incomplete");
    tetra_mm_dispatch(bits, 16, 1, opts, state);
    CHECK(!tetra_st(state)->mm_status_valid, "energy saving request cut inside its information is incomplete");
    tetra_mm_dispatch(bits, 25, 1, opts, state);
    CHECK(tetra_st(state)->mm_status_valid && tetra_st(state)->mm_status_code == 1,
          "complete energy saving request published");

    /* Table 16.7: reserved octet, then the energy saving and SCCH type 2 elements. */
    DSD_MEMSET(bits, 0, sizeof bits);
    int off = 0;
    pack_bits(bits, off, TETRA_MM_D_MM_STATUS, 4);
    off += 4;
    pack_bits(bits, off, 4, 6);
    off += 6;
    off += 8; /* reserved */
    pack_bits(bits, off, 1, 1);
    off++; /* O-bit */
    pack_bits(bits, off, 1, 1);
    off++; /* energy saving information present */
    pack_bits(bits, off, 0x0123, 14);
    off += 14;
    off++; /* no SCCH information */
    off++; /* terminal M-bit */
    CHECK(off == 36, "table 16.7 body with energy saving information is 36 bits");
    tetra_mm_dispatch(bits, off - 3, 1, opts, state);
    CHECK(tetra_st(state)->mm_status_code == 1,
          "terminating dual watch response cut inside a type 2 value is incomplete");
    tetra_mm_dispatch(bits, off, 1, opts, state);
    CHECK(tetra_st(state)->mm_status_code == 4, "complete terminating dual watch response published");

    /* Table 16.92 ranges outside the sub-PDUs decoded here. */
    CHECK(strcmp(tetra_mm_status_name(20), "DMO gateway") == 0, "status 16 to 31 is a DMO gateway function");
    CHECK(strcmp(tetra_mm_status_name(45), "network specific") == 0, "status 32 to 47 is network specific");
    CHECK(strcmp(tetra_mm_status_name(50), "reserved") == 0, "status 48 to 63 is reserved");

    free_state(state);
    free(opts);
}

/* Table 16.11: the 14-bit energy saving information type 2 element. */
static void
test_lu_accept_energy_saving(void) {
    uint8_t bits[32] = {0};
    tetra_mm_pdu pdu;
    int ok = 0;
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    off += 4;
    pack_bits(bits, off, 3, 3);
    off += 3;
    pack_bits(bits, off, 1, 1);
    off++;    /* O-bit */
    off += 3; /* no SSI, address extension or subscriber class */
    pack_bits(bits, off, 1, 1);
    off++;
    pack_bits(bits, off, 0x2ABC, 14);
    off += 14;
    off++; /* no SCCH information */
    off++; /* terminal M-bit */
    CHECK(off == 28, "LU accept with energy saving information is 28 bits");
    ok = tetra_mm_decode(bits, off, &pdu);
    CHECK(ok && pdu.lu_accept.energy_saving_valid && pdu.lu_accept.energy_saving == 0x2ABC && !pdu.lu_accept.scch_valid,
          "LU accept energy saving information decoded");
}

int
main(void) {
    test_carrier_to_dl_hz();
    test_security_golden_pdus();
    test_standard_mm_pdus();
    test_otar_cck_provide();
    test_otar_sck_and_gsko();
    test_otar_gck();
    test_otar_key_associate();
    test_otar_newcell();
    test_otar_cmg_gtsi();
    test_otar_key_delete();
    test_otar_key_status_demand();
    test_otar_dm_sck_activate();
    test_authentication_pdus();
    test_ck_change_demand();
    test_annex_e11_lu_accept_type4_chain();
    test_annex_e9_group_identity_downlink();
    test_otar_key_associate_dissociation();
    test_ck_change_subset_range();
    test_group_downlink_empty_element();
    test_mm_status_sub_pdus();
    test_lu_accept_energy_saving();

    if (g_failures != 0) {
        printf("FAIL  tetra_mm_dispatch: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS  tetra_mm_dispatch: all checks passed\n");
    return 0;
}
