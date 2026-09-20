// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Unit tests for tetra_mm_dispatch() and tetra_carrier_to_dl_hz().
 * Phase 11: MM decoder + SYSINFO DL carrier frequency computation.
 *
 * Bit layout convention: one byte per bit, value 0 or 1, MSB-first.
 */

#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Minimal helpers
 * ----------------------------------------------------------------------- */
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
        g_failures++; \
    } \
} while (0)

/* dsd_state is very large — always heap-allocate to avoid stack overflow */
static dsd_state *alloc_state(void)
{
    return (dsd_state *)calloc(1, sizeof(dsd_state));
}

static dsd_opts *alloc_opts(void)
{
    return (dsd_opts *)calloc(1, sizeof(dsd_opts));
}

/* pack an n-bit value v into bits[] starting at offset off (MSB first) */
static void pack_bits(uint8_t *bits, int off, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        bits[off + (n - 1 - i)] = (uint8_t)((v >> i) & 1u);
    }
}

/* -----------------------------------------------------------------------
 * test_carrier_to_dl_hz
 *
 * Verify the formula and the ETSI offset-code mapping:
 * 00=0, 01=+6.25 kHz, 10=-6.25 kHz, 11=+12.5 kHz.
 * ----------------------------------------------------------------------- */
static void test_carrier_to_dl_hz(void)
{
    /* Each band number supplies a 100 MHz base. */
    long hz = tetra_carrier_to_dl_hz(100, 0, 0);
    CHECK(hz == 2500000L,
          "band 0, carrier=100, offset=0 → 2 500 000");

    hz = tetra_carrier_to_dl_hz(50, 4, 0);
    CHECK(hz == 401250000L,
          "band 4, carrier=50, offset=0 → 401 250 000");

    /* band 4 with offset=2 */
    hz = tetra_carrier_to_dl_hz(50, 4, 2);
    CHECK(hz == 401243750L,
          "band 4, carrier=50, offset code 2 (-6.25 kHz)");

    hz = tetra_carrier_to_dl_hz(50, 4, 3);
    CHECK(hz == 401262500L,
          "band 4, carrier=50, offset code 3 (+12.5 kHz)");

    hz = tetra_carrier_to_dl_hz(0, 8, 0);
    CHECK(hz == 800000000L, "band 8, carrier=0, offset=0 → 800 000 000");

    /* Public real-air trace: band 3, carrier 3748, offset code 3. */
    hz = tetra_carrier_to_dl_hz(3748, 3, 3);
    CHECK(hz == 393712500L, "real-air SYSINFO → 393 712 500");

    hz = tetra_carrier_to_dl_hz(100, 15, 0);
    CHECK(hz == 1502500000L, "band 15, carrier=100 → 1 502 500 000");

    CHECK(tetra_carrier_to_dl_hz(0, 16, 0) == 0L, "out-of-range band → 0");
    CHECK(tetra_carrier_to_dl_hz(4096, 3, 0) == 0L, "out-of-range carrier → 0");

    fprintf(stderr, "[test_carrier_to_dl_hz] done\n");
}

static void test_standard_mm_pdus(void)
{
    uint8_t bits[320] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();

    /* A bare PDU type is not a valid EN 300 392-7 D-DISABLE command. */
    pack_bits(bits, 0, TETRA_MM_D_DISABLE, 4);
    state->tetra_ms_enabled = 1;
    tetra_mm_dispatch(bits, 4, 1, opts, state);
    CHECK(state->tetra_ms_enabled == 1 && !state->tetra_mm_enable_disable_valid,
          "truncated D-DISABLE cannot change state");

    /* Table A.31: permanent equipment and subscription disable with a
     * complete 160-bit authentication challenge. */
    memset(bits, 0, sizeof bits);
    int eo = 0;
    pack_bits(bits, eo, TETRA_MM_D_DISABLE, 4); eo += 4;
    pack_bits(bits, eo, 1, 1); eo++; /* confirm */
    pack_bits(bits, eo, 1, 1); eo++; /* permanent */
    pack_bits(bits, eo, 1, 1); eo++; /* equipment */
    pack_bits(bits, eo, 0x0ABCDEFA, 28); eo += 28;
    pack_bits(bits, eo, 0x12345678, 32); eo += 32;
    pack_bits(bits, eo, 1, 1); eo++; /* subscription */
    pack_bits(bits, eo, 460, 10); eo += 10;
    pack_bits(bits, eo, 4242, 14); eo += 14;
    pack_bits(bits, eo, 0x654321, 24); eo += 24;
    pack_bits(bits, eo, 1, 1); eo++; /* O-bit */
    pack_bits(bits, eo, 1, 1); eo++; /* authentication P-bit */
    for (int i = 0; i < 20; i++) {
        pack_bits(bits, eo, (uint32_t)(0x80 + i), 8); eo += 8;
    }
    pack_bits(bits, eo, 0, 1); eo++; /* terminating proprietary M-bit */
    tetra_mm_dispatch(bits, eo, 1, opts, state);
    CHECK(state->tetra_ms_enabled == 0 && state->tetra_mm_enable_disable_valid,
          "complete D-DISABLE changes state");
    CHECK(!state->tetra_mm_enable_disable_is_enable
          && state->tetra_mm_enable_disable_intent
          && state->tetra_mm_disable_permanent,
          "D-DISABLE command flags retained");
    CHECK(state->tetra_mm_enable_disable_equipment
          && state->tetra_mm_enable_disable_tei == UINT64_C(0x0ABCDEFA12345678),
          "D-DISABLE 60-bit TEI retained");
    CHECK(state->tetra_mm_enable_disable_subscription
          && state->tetra_mm_enable_disable_mcc == 460
          && state->tetra_mm_enable_disable_mnc == 4242
          && state->tetra_mm_enable_disable_ssi == 0x654321,
          "D-DISABLE subscription identity retained");
    CHECK(state->tetra_mm_enable_disable_auth_valid
          && state->tetra_mm_enable_disable_auth_challenge[0] == 0x80
          && state->tetra_mm_enable_disable_auth_challenge[19] == 0x93,
          "D-DISABLE authentication challenge retained");

    /* A missing M-bit must preserve the last complete snapshot and enabled state. */
    state->tetra_ms_enabled = 1;
    state->tetra_mm_enable_disable_ssi = 0x777777;
    tetra_mm_dispatch(bits, eo - 1, 1, opts, state);
    CHECK(state->tetra_ms_enabled == 1
          && state->tetra_mm_enable_disable_ssi == 0x777777,
          "truncated D-DISABLE is atomic");

    /* Minimal table A.32 D-ENABLE, with neither conditional identity. */
    memset(bits, 0, sizeof bits);
    eo = 0;
    pack_bits(bits, eo, TETRA_MM_D_ENABLE, 4); eo += 4;
    pack_bits(bits, eo, 0, 1); eo++; /* intent */
    pack_bits(bits, eo, 0, 1); eo++; /* equipment absent */
    pack_bits(bits, eo, 0, 1); eo++; /* subscription absent */
    pack_bits(bits, eo, 0, 1); eo++; /* O-bit */
    tetra_mm_dispatch(bits, eo, 1, opts, state);
    CHECK(state->tetra_ms_enabled == 1 && state->tetra_mm_enable_disable_is_enable,
          "complete D-ENABLE changes state");
    CHECK(!state->tetra_mm_enable_disable_equipment
          && !state->tetra_mm_enable_disable_subscription
          && !state->tetra_mm_enable_disable_auth_valid
          && state->tetra_mm_enable_disable_tei == 0
          && state->tetra_mm_enable_disable_ssi == 0,
          "D-ENABLE clears absent conditional fields");

    /* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.10. */
    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    pack_bits(bits, 4, 6, 3);
    pack_bits(bits, 7, 0, 1); /* no optional elements */
    tetra_mm_dispatch(bits, 8, 1, opts, state);
    CHECK(state->tetra_mm_lu_accept_valid == 1 && state->tetra_mm_lu_accept_type == 6,
          "LU accept mandatory type");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4);
    pack_bits(bits, 4, 2, 3);
    pack_bits(bits, 7, 1, 1); /* O-bit */
    int ao = 8;
    pack_bits(bits, ao, 1, 1); ao += 1; pack_bits(bits, ao, 0x123456, 24); ao += 24;
    pack_bits(bits, ao, 1, 1); ao += 1; pack_bits(bits, ao, 460, 10); ao += 10;
    pack_bits(bits, ao, 4242, 14); ao += 14;
    pack_bits(bits, ao, 1, 1); ao += 1; pack_bits(bits, ao, 0xBEEF, 16); ao += 16;
    pack_bits(bits, ao, 0, 1); ao += 1; /* no energy saving */
    pack_bits(bits, ao, 1, 1); ao += 1; pack_bits(bits, ao, 0x2D, 6); ao += 6;
    pack_bits(bits, ao, 0, 1); ao += 1; /* terminating M-bit */
    tetra_mm_dispatch(bits, ao, 1, opts, state);
    CHECK(state->tetra_mm_lu_accept_valid == 1 && state->tetra_mm_lu_accept_type == 2,
          "LU accept with Type 2 elements");
    CHECK(state->tetra_mm_lu_accept_ssi_valid == 1
          && state->tetra_mm_lu_accept_ssi == 0x123456, "LU accept SSI");
    CHECK(state->tetra_mm_lu_accept_address_ext_valid == 1
          && state->tetra_mm_lu_accept_mcc == 460
          && state->tetra_mm_lu_accept_mnc == 4242, "LU accept address extension");
    CHECK(state->tetra_mm_lu_accept_subscriber_class_valid == 1
          && state->tetra_mm_lu_accept_subscriber_class == 0xBEEF, "LU accept subscriber class");
    CHECK(state->tetra_mm_lu_accept_energy_saving_valid == 0, "LU accept absent energy saving");
    CHECK(state->tetra_mm_lu_accept_scch_valid == 1
          && state->tetra_mm_lu_accept_scch == 0x2D, "LU accept SCCH");
    state->tetra_mm_lu_accept_valid = 0;
    tetra_mm_dispatch(bits, ao - 1, 1, opts, state);
    CHECK(state->tetra_mm_lu_accept_valid == 0, "truncated LU accept stays invalid");

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
    tetra_mm_dispatch(bits, 43, 1, opts, state);
    CHECK(state->tetra_mm_lu_command_valid == 1, "LU command becomes valid");
    CHECK(state->tetra_mm_lu_command_group_report == 1, "LU command group report");
    CHECK(state->tetra_mm_lu_command_cipher_control == 1, "LU command cipher control");
    CHECK(state->tetra_mm_lu_command_cipher_params == 0x2AA, "LU command cipher parameters");
    CHECK(state->tetra_mm_lu_command_address_ext_valid == 1, "LU command address extension");
    CHECK(state->tetra_mm_lu_command_mcc == 460 && state->tetra_mm_lu_command_mnc == 4242,
          "LU command MCC/MNC");

    /* A declared cipher parameter block must be complete before publication. */
    state->tetra_mm_lu_command_valid = 0;
    tetra_mm_dispatch(bits, 42, 1, opts, state);
    CHECK(state->tetra_mm_lu_command_valid == 0, "truncated LU command stays invalid");

    memset(bits, 0, sizeof bits);
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
    tetra_mm_dispatch(bits, 50, 1, opts, state);
    CHECK(state->tetra_mm_lu_reject_valid == 1, "LU reject becomes valid");
    CHECK(state->tetra_mm_lu_reject_cause == 17, "LU reject uses 5-bit cause");
    CHECK(state->tetra_mm_lu_reject_type == 3, "LU reject update type");
    CHECK(state->tetra_mm_lu_reject_cipher_control == 1, "LU reject cipher control");
    CHECK(state->tetra_mm_lu_reject_cipher_params == 0x155, "LU reject cipher parameters");
    CHECK(state->tetra_mm_lu_reject_address_ext_valid == 1
          && state->tetra_mm_lu_reject_mcc == 460
          && state->tetra_mm_lu_reject_mnc == 4242, "LU reject address extension");
    state->tetra_mm_lu_reject_valid = 0;
    tetra_mm_dispatch(bits, 49, 1, opts, state);
    CHECK(state->tetra_mm_lu_reject_valid == 0, "truncated LU reject stays invalid");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_LOCATION_UPDATING_PROCEEDING, 4);
    pack_bits(bits, 4, 0x123456, 24);
    pack_bits(bits, 28, 0xABCDEF, 24);
    pack_bits(bits, 52, 0, 1); /* O-bit */
    tetra_mm_dispatch(bits, 53, 1, opts, state);
    CHECK(state->tetra_mm_lu_proceeding_valid == 1, "LU proceeding becomes valid");
    CHECK(state->tetra_mm_lu_proceeding_ssi == 0x123456, "LU proceeding SSI");
    CHECK(state->tetra_mm_lu_proceeding_mni == 0xABCDEF, "LU proceeding MNI");
    state->tetra_mm_lu_proceeding_valid = 0;
    tetra_mm_dispatch(bits, 52, 1, opts, state);
    CHECK(state->tetra_mm_lu_proceeding_valid == 0, "truncated LU proceeding stays invalid");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    pack_bits(bits, 4, 1, 1); /* report */
    pack_bits(bits, 5, 1, 1); /* acknowledgement request */
    pack_bits(bits, 6, 0, 1); /* attachment mode */
    pack_bits(bits, 7, 0, 1); /* O-bit */
    tetra_mm_dispatch(bits, 8, 1, opts, state);
    CHECK(state->tetra_mm_group_identity_valid == 1, "group identity mandatory fields");
    CHECK(state->tetra_mm_group_identity_report == 1
          && state->tetra_mm_group_identity_ack_request == 1
          && state->tetra_mm_group_identity_attach_detach_mode == 0,
          "group identity report and acknowledgement request");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP_ACK, 4);
    pack_bits(bits, 4, 1, 1);
    pack_bits(bits, 5, 0, 1);
    pack_bits(bits, 6, 0, 1); /* O-bit */
    tetra_mm_dispatch(bits, 7, 1, opts, state);
    CHECK(state->tetra_mm_group_ack_valid == 1 && state->tetra_mm_group_ack_result == 1,
          "group identity acknowledgement result");
    state->tetra_mm_group_ack_valid = 0;
    tetra_mm_dispatch(bits, 6, 1, opts, state);
    CHECK(state->tetra_mm_group_ack_valid == 0, "group ACK missing O-bit stays invalid");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_D_MM_STATUS, 4);
    pack_bits(bits, 4, 0x2D, 6);
    tetra_mm_dispatch(bits, 10, 1, opts, state);
    CHECK(state->tetra_mm_status_code == 0x2D, "D-MM-STATUS uses 6-bit status downlink");
    CHECK(state->tetra_mm_status_valid == 1, "D-MM-STATUS valid flag");
    state->tetra_mm_status_code = 3;
    state->tetra_mm_status_valid = 1;
    tetra_mm_dispatch(bits, 9, 1, opts, state);
    CHECK(state->tetra_mm_status_code == 3 && state->tetra_mm_status_valid == 1,
          "truncated status preserves prior state");

    memset(bits, 0, sizeof bits);
    pack_bits(bits, 0, TETRA_MM_FUNCTION_NOT_SUPPORTED, 4);
    pack_bits(bits, 4, TETRA_MM_D_ENABLE, 4);
    tetra_mm_dispatch(bits, 8, 1, opts, state);
    CHECK(state->tetra_mm_fns_valid == 1 && state->tetra_mm_fns_pdu_type == TETRA_MM_D_ENABLE,
          "function-not-supported rejected type");

    free(state);
    free(opts);
}

/* Expand a frozen MSB-first hexadecimal PDU into the decoder's one-byte-per-bit
 * representation. These vectors deliberately do not use pack_bits(): their
 * field boundaries remain independent of the test-side message construction
 * used by the exhaustive table cases below. */
static void unpack_hex_bits(const char *hex, uint8_t *bits, int nbits)
{
    for (int bit = 0; bit < nbits; bit++) {
        unsigned char ch = (unsigned char)hex[bit / 4];
        unsigned value = ch >= '0' && ch <= '9' ? ch - '0'
                       : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                       : ch - 'a' + 10;
        bits[bit] = (uint8_t)((value >> (3 - (bit & 3))) & 1u);
    }
}

/* Frozen EN 300 392-7 V3.5.1 security PDUs. The exact bit counts exclude the
 * zero padding in the final hexadecimal octet. They independently pin the
 * outer PDU type and all conditional-field offsets for tables A.4, A.26 and
 * A.31 instead of rebuilding those offsets with pack_bits(). */
static void test_security_golden_pdus(void)
{
    uint8_t bits[192] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();

    /* Table A.4: successful mutual D-AUTHENTICATION RESULT, RES2=CAFEBABE,
     * no proprietary element (41 significant bits). */
    unpack_hex_bits("1BCAFEBABE00", bits, 41);
    tetra_mm_dispatch(bits, 41, 9, opts, state);
    CHECK(state->tetra_mm_auth_valid && state->tetra_mm_auth_subtype == 2
          && state->tetra_mm_auth_result == 1
          && state->tetra_mm_auth_mutual == 1
          && state->tetra_mm_auth_response == UINT32_C(0xCAFEBABE),
          "golden table A.4 authentication result offsets");

    /* Table A.26: acknowledgement, transition to class 3, combined CCK/GCK,
     * CCK-id BEEF, GCK-VN 1234, and IV 2/17/42/CDEF (73 bits). */
    memset(bits, 0, sizeof(bits));
    unpack_hex_bits("2EEFBBC48D0A3566F780", bits, 73);
    tetra_mm_dispatch(bits, 73, 9, opts, state);
    CHECK(state->tetra_mm_ck_change_valid
          && state->tetra_mm_ck_change_ack == 1
          && state->tetra_mm_ck_change_security_class == 3
          && state->tetra_mm_ck_change_key_type == 3
          && state->tetra_mm_ck_change_cck_id == 0xBEEF
          && state->tetra_mm_ck_change_gck_vn == 0x1234,
          "golden table A.26 key selection offsets");
    CHECK(state->tetra_mm_ck_change_time_type == 0
          && state->tetra_mm_ck_change_slot == 2
          && state->tetra_mm_ck_change_frame == 17
          && state->tetra_mm_ck_change_multiframe == 42
          && state->tetra_mm_ck_change_hyperframe == 0xCDEF,
          "golden table A.26 activation-time offsets");

    /* Table A.31: confirmed permanent equipment/subscription disable, TEI
     * 0ABCDEFA12345678, identity 460/4242/654321, no optional tail (117 bits). */
    memset(bits, 0, sizeof(bits));
    state->tetra_ms_enabled = 1;
    unpack_hex_bits("3F579BDF42468ACF17310926543210", bits, 117);
    tetra_mm_dispatch(bits, 117, 9, opts, state);
    CHECK(state->tetra_mm_enable_disable_valid && !state->tetra_ms_enabled
          && !state->tetra_mm_enable_disable_is_enable
          && state->tetra_mm_enable_disable_intent
          && state->tetra_mm_disable_permanent,
          "golden table A.31 command offsets");
    CHECK(state->tetra_mm_enable_disable_equipment
          && state->tetra_mm_enable_disable_tei == UINT64_C(0x0ABCDEFA12345678)
          && state->tetra_mm_enable_disable_subscription
          && state->tetra_mm_enable_disable_mcc == 460
          && state->tetra_mm_enable_disable_mnc == 4242
          && state->tetra_mm_enable_disable_ssi == 0x654321
          && !state->tetra_mm_enable_disable_auth_valid,
          "golden table A.31 conditional identity offsets");

    /* Table A.30a: D-OTAR CMG GTSI provision for GSSI 654321 in foreign
     * network 460/4242, including the Type-2 address and terminal M-bit
     * (59 significant bits). */
    memset(bits, 0, sizeof(bits));
    unpack_hex_bits("0C654321DCC42480", bits, 59);
    tetra_mm_dispatch(bits, 59, 9, opts, state);
    CHECK(state->tetra_mm_otar_valid && state->tetra_mm_otar_subtype == 12
          && state->tetra_mm_otar_gssi == 0x654321
          && state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "golden table A.30a D-OTAR address offsets");

    free(state);
    free(opts);
}

/* ETSI EN 300 392-7 V3.5.1 tables A.1-A.4. */
static void test_authentication_pdus(void)
{
    uint8_t bits[320] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* D-AUTHENTICATION DEMAND: full 80-bit RAND1 and 80-bit RS. */
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4); off += 4;
    pack_bits(bits, off, 0, 2); off += 2;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x10 + i), 8); off += 8;
    }
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 1); off++; /* no proprietary element */
    CHECK(off == 167, "table A.1 demand body is 167 bits");
    tetra_mm_dispatch(bits, off, 5, opts, state);
    CHECK(state->tetra_mm_auth_valid && state->tetra_mm_auth_subtype == 0,
          "authentication demand becomes valid");
    CHECK(state->tetra_mm_auth_random_challenge[0] == 0x10
          && state->tetra_mm_auth_random_challenge[9] == 0x19
          && state->tetra_mm_auth_random_seed[0] == 0xA0
          && state->tetra_mm_auth_random_seed[9] == 0xA9,
          "complete 80-bit challenge and seed retained");

    /* D-AUTHENTICATION RESPONSE with mutual RAND1. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4); off += 4;
    pack_bits(bits, off, 1, 2); off += 2;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x20 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0xDEADBEEF, 32); off += 32;
    pack_bits(bits, off, 1, 1); off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x30 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 200, "table A.3 mutual response body is 200 bits");
    tetra_mm_dispatch(bits, off, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 1
          && state->tetra_mm_auth_response == UINT32_C(0xDEADBEEF)
          && state->tetra_mm_auth_mutual == 1,
          "mutual authentication response retained");
    CHECK(state->tetra_mm_auth_random_seed[9] == 0x29
          && state->tetra_mm_auth_random_challenge[9] == 0x39,
          "response seed and conditional challenge retained");

    /* Removing any part of conditional RAND1 preserves the response snapshot. */
    pack_bits(bits, 86, 0xAAAAAAAA, 32);
    tetra_mm_dispatch(bits, off - 2, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 1
          && state->tetra_mm_auth_response == UINT32_C(0xDEADBEEF),
          "truncated mutual response preserves prior snapshot");

    /* D-AUTHENTICATION RESULT with conditional RES2. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4); off += 4;
    pack_bits(bits, off, 2, 2); off += 2;
    pack_bits(bits, off, 1, 1); off++;       /* authentication success */
    pack_bits(bits, off, 1, 1); off++;       /* mutual */
    pack_bits(bits, off, 0xCAFEBABE, 32); off += 32;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 2
          && state->tetra_mm_auth_result == 1
          && state->tetra_mm_auth_mutual == 1
          && state->tetra_mm_auth_response == UINT32_C(0xCAFEBABE),
          "authentication result and RES2 retained");
    CHECK(state->tetra_mm_auth_random_seed[0] == 0
          && state->tetra_mm_auth_random_challenge[0] == 0,
          "result clears fields absent from its subtype");

    /* D-AUTHENTICATION REJECT permits only reason zero in this revision. */
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_AUTHENTICATION, 4);
    pack_bits(bits, 4, 3, 2);
    pack_bits(bits, 6, 0, 3);
    tetra_mm_dispatch(bits, 9, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 3
          && state->tetra_mm_auth_reject_reason == 0,
          "authentication-not-supported rejection retained");
    pack_bits(bits, 6, 5, 3); /* reserved reason */
    tetra_mm_dispatch(bits, 9, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 3
          && state->tetra_mm_auth_reject_reason == 0,
          "reserved rejection reason preserves prior snapshot");

    /* A bounded proprietary Type 3 value needs its terminal M-bit. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_AUTHENTICATION, 4); off += 4;
    pack_bits(bits, off, 0, 2); off += 2;
    for (int i = 0; i < 20; i++) {
        pack_bits(bits, off, (uint32_t)(0x40 + i), 8); off += 8;
    }
    pack_bits(bits, off, 1, 1); off++;       /* O-bit */
    pack_bits(bits, off, 1, 1); off++;       /* M-bit */
    pack_bits(bits, off, 9, 4); off += 4;
    pack_bits(bits, off, 8, 11); off += 11;
    pack_bits(bits, off, 0x5A, 8); off += 8;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 5, opts, state);
    CHECK(state->tetra_mm_auth_subtype == 0
          && state->tetra_mm_auth_random_challenge[0] == 0x40,
          "authentication demand with proprietary tail accepted");
    pack_bits(bits, 6, 0x60, 8);
    tetra_mm_dispatch(bits, off - 1, 5, opts, state);
    CHECK(state->tetra_mm_auth_random_challenge[0] == 0x40,
          "missing proprietary terminal M-bit preserves snapshot");

    free(state);
    free(opts);
}

/* ETSI EN 300 392-7 V3.5.1 tables A.9 and A.42-A.43. */
static void test_otar_cck_provide(void)
{
    uint8_t bits[640] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Current CCK for an explicit list of two location areas, plus future CCK. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 0, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x10 + i), 8); off += 8;
    }
    pack_bits(bits, off, 1, 2); off += 2;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 0x123, 14); off += 14;
    pack_bits(bits, off, 0x234, 14); off += 14;
    pack_bits(bits, off, 1, 1); off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x80 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 302, "table A.9 list and future-key body is 302 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_valid && state->tetra_mm_otar_subtype == 0
          && state->tetra_mm_otar_cck_provision,
          "CCK Provide becomes valid");
    CHECK(state->tetra_mm_otar_cck_id == 0xCAFE
          && state->tetra_mm_otar_cck_key_type == 0
          && state->tetra_mm_otar_cck_sealed[0] == 0x10
          && state->tetra_mm_otar_cck_sealed[14] == 0x1E,
          "CCK identifier, type and full sealed key retained");
    CHECK(state->tetra_mm_otar_cck_la_type == 1
          && state->tetra_mm_otar_cck_la_count == 2
          && state->tetra_mm_otar_cck_la[0] == 0x123
          && state->tetra_mm_otar_cck_la[1] == 0x234,
          "CCK location-area list retained");
    CHECK(state->tetra_mm_otar_cck_future
          && state->tetra_mm_otar_cck_future_sealed[0] == 0x80
          && state->tetra_mm_otar_cck_future_sealed[14] == 0x8E,
          "future sealed CCK retained");

    pack_bits(bits, 9, 0xBEEF, 16);
    tetra_mm_dispatch(bits, off - 2, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_id == 0xCAFE
          && state->tetra_mm_otar_cck_future_sealed[14] == 0x8E,
          "truncated future CCK preserves prior snapshot");

    /* Mask/selector form clears fields that only occur in other forms. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 0, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x1357, 16); off += 16;
    pack_bits(bits, off, 1, 1); off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x30 + i), 8); off += 8;
    }
    pack_bits(bits, off, 2, 2); off += 2;
    pack_bits(bits, off, 0x3F00, 14); off += 14;
    pack_bits(bits, off, 0x1200, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_key_type == 1
          && state->tetra_mm_otar_cck_la_type == 2
          && state->tetra_mm_otar_cck_la_mask == 0x3F00
          && state->tetra_mm_otar_cck_la_selector == 0x1200,
          "future CCK mask and selector retained");
    CHECK(state->tetra_mm_otar_cck_la_count == 0
          && !state->tetra_mm_otar_cck_future,
          "mask form clears list and absent future-key fields");
    pack_bits(bits, 9, 0x7777, 16);
    pack_bits(bits, 176, 1, 1);               /* forbidden for a future CCK */
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_id == 0x1357,
          "future CCK cannot carry a second future sealed key");

    /* Range form requires the high value to be greater than the low value. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 0, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x2468, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    off += 120;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 100, 14); off += 14;
    pack_bits(bits, off, 200, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_la_type == 3
          && state->tetra_mm_otar_cck_la_low == 100
          && state->tetra_mm_otar_cck_la_high == 200,
          "CCK location-area range retained");
    pack_bits(bits, 162, 50, 14);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_la_high == 200,
          "invalid CCK location-area range preserves snapshot");

    /* Type zero applies the CCK to all location areas. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 0, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x369C, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    off += 120;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_cck_id == 0x369C
          && state->tetra_mm_otar_cck_la_type == 0,
          "CCK applies to all location areas");

    /* No-provision form contains only its generic optional-element bit. */
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_OTAR, 4);
    pack_bits(bits, 4, 0, 4);
    pack_bits(bits, 8, 0, 1);
    pack_bits(bits, 9, 0, 1);
    tetra_mm_dispatch(bits, 10, 2, opts, state);
    CHECK(state->tetra_mm_otar_valid && !state->tetra_mm_otar_cck_provision
          && state->tetra_mm_otar_cck_id == 0
          && state->tetra_mm_otar_cck_la_type == 0,
          "CCK no-provision form clears conditional fields");

    free(state);
    free(opts);
}

/* Tables A.16/A.19 and A.20/A.23: SCK and GSKO transfer paths. */
static void test_otar_sck_and_gsko(void)
{
    uint8_t bits[1400] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Two group-session-encrypted SCKs with an explicit response and MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 45, 16); off += 16;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x2345, 16); off += 16;
    pack_bits(bits, off, 2, 3); off += 3;
    for (int key = 0; key < 2; key++) {
        pack_bits(bits, off, (uint32_t)(3 + key), 5); off += 5;
        pack_bits(bits, off, (uint32_t)(0x1000 + key), 16); off += 16;
        pack_bits(bits, off, (uint32_t)key, 1); off++;
        pack_bits(bits, off, 0, 1); off++;
        for (int i = 0; i < 15; i++) {
            pack_bits(bits, off, (uint32_t)(0x20 + key * 0x20 + i), 8); off += 8;
        }
    }
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 5, 3); off += 3;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 366, "table A.16 two-SCK body is 366 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 2
          && state->tetra_mm_otar_ack
          && state->tetra_mm_otar_explicit_response
          && state->tetra_mm_otar_max_response_timer == 45,
          "SCK Provide response controls retained");
    CHECK(state->tetra_mm_otar_session_key == 1
          && state->tetra_mm_otar_gsko_version == 0x2345
          && state->tetra_mm_otar_key_count == 2,
          "SCK Provide group-session fields retained");
    CHECK(state->tetra_mm_otar_sck_number[0] == 3
          && state->tetra_mm_otar_sck_version[1] == 0x1001
          && state->tetra_mm_otar_sck_use[1] == 1
          && state->tetra_mm_otar_sck_sealed[0][0] == 0x20
          && state->tetra_mm_otar_sck_sealed[1][14] == 0x4E,
          "both complete SCK descriptors retained");
    CHECK(state->tetra_mm_otar_ksg == 2
          && state->tetra_mm_otar_retry_interval == 5
          && state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "SCK Provide KSG, retry and address retained");

    tetra_mm_dispatch(bits, off - 1, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 2
          && state->tetra_mm_otar_sck_version[1] == 0x1001,
          "truncated SCK optional tail preserves snapshot");
    pack_bits(bits, 68, 1, 1);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_sck_sealed[0][0] == 0x20,
          "non-zero SCK reserved bit preserves snapshot");
    pack_bits(bits, 68, 0, 1);
    pack_bits(bits, 332, 5, 4);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_ksg == 2,
          "reserved KSG number preserves SCK snapshot");

    /* Individual-session form carries the 80-bit random seed. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 8, 4); off += 4;     /* proprietary KSG range */
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 118, "table A.16 individual-session zero-key body is 118 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 2
          && !state->tetra_mm_otar_session_key
          && state->tetra_mm_otar_random_seed[0] == 0xA0
          && state->tetra_mm_otar_random_seed[9] == 0xA9
          && state->tetra_mm_otar_key_count == 0
          && state->tetra_mm_otar_ksg == 8,
          "individual-session SCK seed and proprietary KSG retained");

    /* Each rejected SCK has its own valid reject reason. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 3, 4); off += 4;
    pack_bits(bits, off, 2, 3); off += 3;
    pack_bits(bits, off, 1, 3); off += 3;
    pack_bits(bits, off, 7, 5); off += 5;
    pack_bits(bits, off, 3, 3); off += 3;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 6, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 31, "table A.19 two-SCK reject body is 31 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 3
          && state->tetra_mm_otar_key_count == 2
          && state->tetra_mm_otar_reject_reason[0] == 1
          && state->tetra_mm_otar_sck_number[0] == 7
          && state->tetra_mm_otar_reject_reason[1] == 3
          && state->tetra_mm_otar_sck_number[1] == 31
          && state->tetra_mm_otar_retry_interval == 6,
          "SCK Reject list retained");
    pack_bits(bits, 11, 7, 3);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_reject_reason[0] == 1,
          "reserved SCK reject reason preserves snapshot");

    /* GSKO Provide retains the complete seed, key, version and group. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 8, 4); off += 4;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0x60 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0xABCD, 16); off += 16;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x90 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0x654321, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 8
          && state->tetra_mm_otar_random_seed[0] == 0x60
          && state->tetra_mm_otar_random_seed[9] == 0x69
          && state->tetra_mm_otar_gsko_version == 0xABCD
          && state->tetra_mm_otar_gsko_sealed[14] == 0x9E
          && state->tetra_mm_otar_gssi == 0x654321,
          "GSKO Provide snapshot retained");
    CHECK(state->tetra_mm_otar_key_count == 0
          && state->tetra_mm_otar_cck_id == 0,
          "GSKO Provide clears fields from earlier subtypes");

    /* GSKO Reject is the fixed reject reason, group and retry tuple. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 9, 4); off += 4;
    pack_bits(bits, off, 2, 3); off += 3;
    pack_bits(bits, off, 0x123456, 24); off += 24;
    pack_bits(bits, off, 4, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 9
          && state->tetra_mm_otar_reject_reason[0] == 2
          && state->tetra_mm_otar_gssi == 0x123456
          && state->tetra_mm_otar_retry_interval == 4,
          "GSKO Reject snapshot retained");

    free(state);
    free(opts);
}

/* Tables A.12/A.15 and A.62/A.63b: GCK provision and rejection paths. */
static void test_otar_gck(void)
{
    uint8_t bits[1600] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Two group-session-encrypted GCKs associated with one GSSI and MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 4, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 91, 16); off += 16;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x3456, 16); off += 16;
    pack_bits(bits, off, 2, 3); off += 3;
    for (int key = 0; key < 2; key++) {
        pack_bits(bits, off, 0x1200, 16); off += 16;
        pack_bits(bits, off, (uint32_t)(0x4500 + key), 16); off += 16;
        for (int i = 0; i < 15; i++) {
            pack_bits(bits, off, (uint32_t)(0x30 + key * 0x20 + i), 8); off += 8;
        }
    }
    pack_bits(bits, off, 3, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x654321, 24); off += 24;
    pack_bits(bits, off, 5, 3); off += 3;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 409, "table A.12 two-GCK body is 409 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 4
          && state->tetra_mm_otar_ack
          && state->tetra_mm_otar_explicit_response
          && state->tetra_mm_otar_max_response_timer == 91,
          "GCK Provide response controls retained");
    CHECK(state->tetra_mm_otar_session_key
          && state->tetra_mm_otar_gsko_version == 0x3456
          && state->tetra_mm_otar_key_count == 2,
          "GCK Provide group-session fields retained");
    CHECK(state->tetra_mm_otar_gck_number[0] == 0x1200
          && state->tetra_mm_otar_gck_version[1] == 0x4501
          && state->tetra_mm_otar_gck_sealed[0][0] == 0x30
          && state->tetra_mm_otar_gck_sealed[1][14] == 0x5E,
          "both complete GCK descriptors retained");
    CHECK(state->tetra_mm_otar_ksg == 3
          && state->tetra_mm_otar_group_association == 1
          && state->tetra_mm_otar_gssi == 0x654321
          && state->tetra_mm_otar_retry_interval == 5
          && state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "GCK Provide association, retry and address retained");

    tetra_mm_dispatch(bits, off - 1, 3, opts, state);
    CHECK(state->tetra_mm_otar_gck_version[1] == 0x4501,
          "truncated GCK optional tail preserves snapshot");
    pack_bits(bits, 8, 0, 1);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_ack
          && state->tetra_mm_otar_gck_number[0] == 0x1200,
          "non-zero GCK reserved response bit preserves snapshot");
    pack_bits(bits, 8, 1, 1);
    pack_bits(bits, 350, 6, 4);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_ksg == 3,
          "reserved KSG number preserves GCK snapshot");
    pack_bits(bits, 350, 3, 4);
    pack_bits(bits, 198, 0x1201, 16);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_gck_number[1] == 0x1200,
          "GSSI association rejects mixed GCK numbers");
    pack_bits(bits, 198, 0x1200, 16);
    pack_bits(bits, 214, 0x4500, 16);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_gck_version[1] == 0x4501,
          "GSSI association rejects duplicate GCK versions");

    /* Individual-session form permits zero keys and association by GCKN. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 4, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    for (int i = 0; i < 10; i++) {
        pack_bits(bits, off, (uint32_t)(0xA0 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 8, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 119, "table A.12 individual-session zero-key body is 119 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 4
          && !state->tetra_mm_otar_session_key
          && state->tetra_mm_otar_random_seed[0] == 0xA0
          && state->tetra_mm_otar_random_seed[9] == 0xA9
          && state->tetra_mm_otar_key_count == 0
          && state->tetra_mm_otar_ksg == 8
          && !state->tetra_mm_otar_group_association
          && state->tetra_mm_otar_gssi == 0,
          "individual-session zero-key GCK fields retained");

    /* Rejected GCKs may be identified independently by GCKN or GSSI. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 5, 4); off += 4;
    pack_bits(bits, off, 2, 3); off += 3;
    pack_bits(bits, off, 1, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 3, 3); off += 3;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x123456, 24); off += 24;
    pack_bits(bits, off, 6, 3); off += 3;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 63, "table A.15 mixed two-GCK reject body is 63 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 5
          && state->tetra_mm_otar_key_count == 2
          && state->tetra_mm_otar_reject_reason[0] == 1
          && !state->tetra_mm_otar_gck_reject_group_association[0]
          && state->tetra_mm_otar_gck_number[0] == 0xCAFE,
          "GCK Reject entry identified by GCKN retained");
    CHECK(state->tetra_mm_otar_reject_reason[1] == 3
          && state->tetra_mm_otar_gck_reject_group_association[1]
          && state->tetra_mm_otar_gck_reject_gssi[1] == 0x123456
          && state->tetra_mm_otar_retry_interval == 6,
          "GCK Reject entry identified by GSSI retained");

    pack_bits(bits, 11, 7, 3);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_reject_reason[0] == 1,
          "reserved GCK reject reason preserves snapshot");
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_OTAR, 4);
    pack_bits(bits, 4, 5, 4);
    pack_bits(bits, 8, 0, 3);
    tetra_mm_dispatch(bits, 15, 3, opts, state);
    CHECK(state->tetra_mm_otar_key_count == 2
          && state->tetra_mm_otar_gck_number[0] == 0xCAFE,
          "reserved zero GCK reject count preserves snapshot");

    free(state);
    free(opts);
}

/* Table A.24: SCK/GCK association and disassociation for GSSI lists/ranges. */
static void test_otar_key_associate(void)
{
    uint8_t bits[1000] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Associate an SCK subset with three groups in a foreign network. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 6, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 73, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 4, 6); off += 6;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 3, 5); off += 5;
    pack_bits(bits, off, 0x100001, 24); off += 24;
    pack_bits(bits, off, 0x100002, 24); off += 24;
    pack_bits(bits, off, 0x100003, 24); off += 24;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 141, "table A.24 three-group SCK association is 141 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 6
          && state->tetra_mm_otar_ack
          && state->tetra_mm_otar_explicit_response
          && state->tetra_mm_otar_max_response_timer == 73,
          "key-associate response controls retained");
    CHECK(!state->tetra_mm_otar_key_association_type
          && state->tetra_mm_otar_sck_select == 4
          && state->tetra_mm_otar_sck_subset_grouping == 2,
          "SCK selection and subset grouping retained");
    CHECK(state->tetra_mm_otar_group_count == 3
          && !state->tetra_mm_otar_group_is_range
          && state->tetra_mm_otar_group_value_count == 3
          && state->tetra_mm_otar_group_gssi[0] == 0x100001
          && state->tetra_mm_otar_group_gssi[2] == 0x100003,
          "key-associate GSSI list retained");
    CHECK(state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "key-associate address extension retained");

    tetra_mm_dispatch(bits, off - 1, 3, opts, state);
    CHECK(state->tetra_mm_otar_group_gssi[2] == 0x100003,
          "truncated key-associate tail preserves snapshot");
    pack_bits(bits, 8, 0, 1);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_ack,
          "explicit response without acknowledgement preserves snapshot");
    pack_bits(bits, 8, 1, 1);
    pack_bits(bits, 27, 34, 6);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_sck_select == 4,
          "reserved SCK selection preserves snapshot");
    pack_bits(bits, 27, 4, 6);
    pack_bits(bits, 33, 10, 4);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_sck_subset_grouping == 2,
          "non-demand SCK grouping value preserves snapshot");
    pack_bits(bits, 33, 2, 4);
    pack_bits(bits, 37, 0, 5);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_group_count == 3,
          "reserved zero group count preserves snapshot");

    /* Disassociate GCKs from an ordered range, returning the groups to CCK. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 6, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 16); off += 16;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x10000, 17); off += 17;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 1000, 24); off += 24;
    pack_bits(bits, off, 2000, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 98, "table A.24 GCK range disassociation is 98 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 6
          && state->tetra_mm_otar_key_association_type
          && state->tetra_mm_otar_gck_select == 0x10000,
          "GCK disassociation selection retained");
    CHECK(state->tetra_mm_otar_group_count == 31
          && state->tetra_mm_otar_group_is_range
          && state->tetra_mm_otar_group_value_count == 2
          && state->tetra_mm_otar_group_gssi[0] == 1000
          && state->tetra_mm_otar_group_gssi[1] == 2000,
          "ordered GSSI range retained");
    CHECK(state->tetra_mm_otar_sck_select == 0
          && !state->tetra_mm_otar_address_valid,
          "GCK association clears prior subtype-specific fields");

    pack_bits(bits, 27, 0x10001, 17);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_gck_select == 0x10000,
          "reserved GCK selection preserves snapshot");
    pack_bits(bits, 27, 0x10000, 17);
    pack_bits(bits, 73, 999, 24);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_group_gssi[1] == 2000,
          "reversed GSSI range preserves snapshot");

    free(state);
    free(opts);
}

/* Table A.30: DCK forwarding result with optional CCK information. */
static void test_otar_newcell(void)
{
    uint8_t bits[640] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 7, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    for (int i = 0; i < 15; i++) {
        pack_bits(bits, off, (uint32_t)(0x70 + i), 8); off += 8;
    }
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 151, "table A.30 current-CCK NEWCELL is 151 bits");
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 7
          && state->tetra_mm_otar_dck_forwarding_result
          && state->tetra_mm_otar_cck_provision,
          "NEWCELL forwarding and CCK flags retained");
    CHECK(state->tetra_mm_otar_cck_id == 0xCAFE
          && state->tetra_mm_otar_cck_key_type == 0
          && state->tetra_mm_otar_cck_sealed[0] == 0x70
          && state->tetra_mm_otar_cck_sealed[14] == 0x7E
          && state->tetra_mm_otar_cck_la_type == 0,
          "NEWCELL complete CCK information retained");

    tetra_mm_dispatch(bits, off - 1, 4, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 7
          && state->tetra_mm_otar_cck_id == 0xCAFE,
          "truncated NEWCELL preserves snapshot");
    pack_bits(bits, 26, 1, 1);
    pack_bits(bits, 149, 1, 1);
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_otar_cck_key_type == 0
          && !state->tetra_mm_otar_cck_future,
          "future CCK cannot include another future key in NEWCELL");

    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 7, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 11, "table A.30 no-CCK NEWCELL is 11 bits");
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 7
          && !state->tetra_mm_otar_dck_forwarding_result
          && !state->tetra_mm_otar_cck_provision
          && state->tetra_mm_otar_cck_id == 0,
          "NEWCELL without CCK clears conditional fields");

    free(state);
    free(opts);
}

/* Table A.30a: crypto-management group address provision. */
static void test_otar_cmg_gtsi(void)
{
    uint8_t bits[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 12, 4); off += 4;
    pack_bits(bits, off, 0x654321, 24); off += 24;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 59, "table A.30a CMG GTSI with address is 59 bits");
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 12
          && state->tetra_mm_otar_gssi == 0x654321,
          "CMG GTSI retained");
    CHECK(state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "CMG GTSI foreign-network address retained");
    CHECK(!state->tetra_mm_otar_cck_provision
          && state->tetra_mm_otar_group_count == 0,
          "CMG GTSI clears prior subtype-specific fields");

    tetra_mm_dispatch(bits, off - 1, 4, opts, state);
    CHECK(state->tetra_mm_otar_subtype == 12
          && state->tetra_mm_otar_gssi == 0x654321,
          "truncated CMG GTSI tail preserves snapshot");

    free(state);
    free(opts);
}

/* Table A.27f: delete selected, grouped, subset, or complete key classes. */
static void test_otar_key_delete(void)
{
    uint8_t bits[256] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Delete three KAG members in a foreign network. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 10, 4); off += 4;
    pack_bits(bits, off, 1, 3); off += 3;
    pack_bits(bits, off, 3, 5); off += 5;
    pack_bits(bits, off, 0, 5); off += 5;
    pack_bits(bits, off, 15, 5); off += 5;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 58, "table A.27f KAG deletion with address is 58 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 10
          && state->tetra_mm_otar_key_delete_type == 1
          && state->tetra_mm_otar_key_delete_sck_count == 3,
          "KAG deletion type and count retained");
    CHECK(state->tetra_mm_otar_key_delete_sck_number[0] == 0
          && state->tetra_mm_otar_key_delete_sck_number[1] == 15
          && state->tetra_mm_otar_key_delete_sck_number[2] == 31,
          "all KAG SCK identifiers retained");
    CHECK(state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "key-delete address extension retained");

    tetra_mm_dispatch(bits, off - 1, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_sck_count == 3,
          "truncated key-delete optional tail preserves snapshot");

    /* Delete one validated SCK subset. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 10, 4); off += 4;
    pack_bits(bits, off, 2, 3); off += 3;
    pack_bits(bits, off, 8, 4); off += 4;
    pack_bits(bits, off, 15, 5); off += 5;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 21, "table A.27f subset deletion is 21 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_type == 2
          && state->tetra_mm_otar_key_delete_sck_grouping == 8
          && state->tetra_mm_otar_key_delete_sck_subset == 15
          && state->tetra_mm_otar_key_delete_sck_count == 0,
          "SCK subset deletion retained and clears list");

    pack_bits(bits, 15, 16, 5);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_sck_subset == 15,
          "out-of-group delete subset preserves snapshot");
    pack_bits(bits, 15, 15, 5);
    pack_bits(bits, 11, 10, 4);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_sck_grouping == 8,
          "reserved delete grouping preserves snapshot");

    /* Delete two named GCKs. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 10, 4); off += 4;
    pack_bits(bits, off, 4, 3); off += 3;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 0x1234, 16); off += 16;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 48, "table A.27f two-GCK deletion is 48 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_type == 4
          && state->tetra_mm_otar_key_delete_gck_count == 2
          && state->tetra_mm_otar_key_delete_gck_number[0] == 0x1234
          && state->tetra_mm_otar_key_delete_gck_number[1] == 0xCAFE,
          "individual GCK deletion list retained");
    CHECK(state->tetra_mm_otar_key_delete_sck_grouping == 0,
          "GCK deletion clears prior SCK subset fields");

    /* All-SCK, all-GCK, and GSKO deletions have no conditional body. */
    const uint8_t whole_types[] = {3, 5, 6};
    for (unsigned i = 0; i < sizeof(whole_types); i++) {
        memset(bits, 0, sizeof(bits)); off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
        pack_bits(bits, off, 10, 4); off += 4;
        pack_bits(bits, off, whole_types[i], 3); off += 3;
        pack_bits(bits, off, 0, 1); off++;
        CHECK(off == 12, "table A.27f whole-class deletion is 12 bits");
        tetra_mm_dispatch(bits, off, 2, opts, state);
        CHECK(state->tetra_mm_otar_key_delete_type == whole_types[i]
              && state->tetra_mm_otar_key_delete_gck_count == 0,
              "whole key-class deletion retained");
    }

    pack_bits(bits, 8, 7, 3);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_delete_type == 6,
          "downlink key-delete extension type is rejected atomically");

    free(state);
    free(opts);
}

/* Table A.27h: request the status of individual or grouped traffic keys. */
static void test_otar_key_status_demand(void)
{
    uint8_t bits[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Request the last valid subset in grouping 7, with a foreign MNI. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 11, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0x1234, 16); off += 16;
    pack_bits(bits, off, 1, 3); off += 3;
    pack_bits(bits, off, 7, 4); off += 4;
    pack_bits(bits, off, 10, 5); off += 5;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 65, "table A.27h subset request with address is 65 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 11
          && state->tetra_mm_otar_ack
          && state->tetra_mm_otar_explicit_response
          && state->tetra_mm_otar_max_response_timer == 0x1234,
          "key-status response controls retained");
    CHECK(state->tetra_mm_otar_key_status_type == 1
          && state->tetra_mm_otar_key_status_sck_grouping == 7
          && state->tetra_mm_otar_key_status_sck_subset == 10,
          "key-status SCK subset retained");
    CHECK(state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "key-status address extension retained");

    pack_bits(bits, 33, 11, 5);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_sck_subset == 10,
          "out-of-group SCK subset preserves snapshot");
    pack_bits(bits, 33, 10, 5);
    pack_bits(bits, 29, 10, 4);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_sck_grouping == 7,
          "reserved SCK grouping preserves snapshot");
    pack_bits(bits, 29, 7, 4);
    pack_bits(bits, 8, 0, 1);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_ack,
          "key-status explicit response without acknowledgement is invalid");

    /* Individual SCK and GCK identifiers use their complete encoded ranges. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 11, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 16); off += 16;
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 35, "table A.27h individual SCK request is 35 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_type == 0
          && state->tetra_mm_otar_key_status_sck_number == 31
          && !state->tetra_mm_otar_address_valid,
          "individual SCK status request retained");

    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 11, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 9, 16); off += 16;
    pack_bits(bits, off, 3, 3); off += 3;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 46, "table A.27h individual GCK request is 46 bits");
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_type == 3
          && state->tetra_mm_otar_key_status_gck_number == 0xCAFE
          && state->tetra_mm_otar_key_status_sck_number == 0,
          "individual GCK status request retained and clears SCK selection");

    /* Aggregate SCK, aggregate GCK and GSKO requests have no identifier. */
    for (uint8_t type = 2; type <= 5; type += (uint8_t)(type == 2 ? 2 : 1)) {
        memset(bits, 0, sizeof(bits)); off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
        pack_bits(bits, off, 11, 4); off += 4;
        pack_bits(bits, off, 0, 1); off++;
        pack_bits(bits, off, 0, 1); off++;
        pack_bits(bits, off, 0, 16); off += 16;
        pack_bits(bits, off, type, 3); off += 3;
        pack_bits(bits, off, 0, 1); off++;
        CHECK(off == 30, "table A.27h aggregate request is 30 bits");
        tetra_mm_dispatch(bits, off, 2, opts, state);
        CHECK(state->tetra_mm_otar_key_status_type == type
              && state->tetra_mm_otar_key_status_gck_number == 0,
              "aggregate key-status request retained");
    }

    pack_bits(bits, 26, 6, 3);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_type == 5,
          "response-only reject type preserves snapshot");
    pack_bits(bits, 26, 7, 3);
    tetra_mm_dispatch(bits, off, 2, opts, state);
    CHECK(state->tetra_mm_otar_key_status_type == 5,
          "reserved key-status type preserves snapshot");

    free(state);
    free(opts);
}

/* Table A.27d: activate DMO SCK subsets or individual keys for another MNI. */
static void test_otar_dm_sck_activate(void)
{
    uint8_t bits[256] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Activate a complete subset at an absolute IV. */
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 13, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0, 4); off += 4;
    pack_bits(bits, off, 4, 4); off += 4;
    pack_bits(bits, off, 5, 5); off += 5;
    pack_bits(bits, off, 0xBEEF, 16); off += 16;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 17, 5); off += 5;
    pack_bits(bits, off, 45, 6); off += 6;
    pack_bits(bits, off, 0x2468, 16); off += 16;
    pack_bits(bits, off, 460, 10); off += 10;
    pack_bits(bits, off, 4242, 14); off += 14;
    CHECK(off == 93, "table A.27d subset absolute-IV demand is 93 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_valid
          && state->tetra_mm_otar_subtype == 13
          && state->tetra_mm_otar_dm_sck_ack
          && state->tetra_mm_otar_dm_sck_count == 0,
          "DM-SCK subset demand and acknowledgement retained");
    CHECK(state->tetra_mm_otar_dm_sck_grouping == 4
          && state->tetra_mm_otar_dm_sck_subset == 5
          && state->tetra_mm_otar_dm_sck_vn == 0xBEEF,
          "DM-SCK subset identity retained");
    CHECK(state->tetra_mm_otar_dm_sck_time_type == 0
          && state->tetra_mm_otar_dm_sck_slot == 3
          && state->tetra_mm_otar_dm_sck_frame == 17
          && state->tetra_mm_otar_dm_sck_multiframe == 45
          && state->tetra_mm_otar_dm_sck_hyperframe == 0x2468,
          "DM-SCK absolute activation time retained");
    CHECK(state->tetra_mm_otar_address_valid
          && state->tetra_mm_otar_mcc == 460
          && state->tetra_mm_otar_mnc == 4242,
          "mandatory DMO network MNI retained");

    tetra_mm_dispatch(bits, off - 1, 3, opts, state);
    CHECK(state->tetra_mm_otar_dm_sck_hyperframe == 0x2468,
          "truncated mandatory DMO MNI preserves snapshot");
    pack_bits(bits, 17, 6, 5);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_dm_sck_subset == 5,
          "out-of-group DM-SCK subset preserves snapshot");
    pack_bits(bits, 17, 5, 5);
    pack_bits(bits, 13, 10, 4);
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_dm_sck_grouping == 4,
          "reserved DM-SCK grouping preserves snapshot");

    /* Activate two named SCKs at a 48-bit network time. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
    pack_bits(bits, off, 13, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 3, 5); off += 5;
    pack_bits(bits, off, 0x1234, 16); off += 16;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 0xABCD, 16); off += 16;
    pack_bits(bits, off, 1, 2); off += 2;
    pack_bits(bits, off, 0x5678, 16); off += 16;
    pack_bits(bits, off, 0x9ABCDEF0, 32); off += 32;
    pack_bits(bits, off, 302, 10); off += 10;
    pack_bits(bits, off, 100, 14); off += 14;
    CHECK(off == 129, "table A.27d two-SCK network-time demand is 129 bits");
    tetra_mm_dispatch(bits, off, 3, opts, state);
    CHECK(state->tetra_mm_otar_dm_sck_count == 2
          && state->tetra_mm_otar_dm_sck_number[0] == 3
          && state->tetra_mm_otar_dm_sck_version[0] == 0x1234
          && state->tetra_mm_otar_dm_sck_number[1] == 31
          && state->tetra_mm_otar_dm_sck_version[1] == 0xABCD,
          "repeated DM-SCK descriptors retained");
    CHECK(state->tetra_mm_otar_dm_sck_time_type == 1
          && state->tetra_mm_otar_dm_sck_network_time
                 == UINT64_C(0x56789ABCDEF0)
          && state->tetra_mm_otar_mcc == 302
          && state->tetra_mm_otar_mnc == 100,
          "DM-SCK network time and DMO network retained");
    CHECK(state->tetra_mm_otar_dm_sck_grouping == 0
          && state->tetra_mm_otar_dm_sck_vn == 0,
          "individual DM-SCK demand clears prior subset fields");

    /* Immediate and already-active demands carry no additional time value. */
    for (uint8_t time_type = 2; time_type <= 3; time_type++) {
        memset(bits, 0, sizeof(bits)); off = 0;
        pack_bits(bits, off, TETRA_MM_D_OTAR, 4); off += 4;
        pack_bits(bits, off, 13, 4); off += 4;
        pack_bits(bits, off, 0, 1); off++;
        pack_bits(bits, off, 1, 4); off += 4;
        pack_bits(bits, off, 7, 5); off += 5;
        pack_bits(bits, off, 0x7000 + time_type, 16); off += 16;
        pack_bits(bits, off, time_type, 2); off += 2;
        pack_bits(bits, off, 1, 10); off += 10;
        pack_bits(bits, off, 2, 14); off += 14;
        CHECK(off == 60, "table A.27d implicit-time demand is 60 bits");
        tetra_mm_dispatch(bits, off, 3, opts, state);
        CHECK(state->tetra_mm_otar_dm_sck_time_type == time_type
              && state->tetra_mm_otar_dm_sck_network_time == 0,
              "DM-SCK implicit activation time retained");
    }

    free(state);
    free(opts);
}

/* ETSI EN 300 392-7 V3.5.1 table A.26. */
static void test_ck_change_demand(void)
{
    uint8_t bits[320] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* Two individually named TMO SCKs, activated immediately. */
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;       /* acknowledgement */
    pack_bits(bits, off, 3, 2); off += 2;    /* transition to class 3 */
    pack_bits(bits, off, 0, 3); off += 3;    /* SCK */
    pack_bits(bits, off, 0, 1); off++;       /* TMO */
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 3, 5); off += 5;
    pack_bits(bits, off, 0xABCD, 16); off += 16;
    pack_bits(bits, off, 31, 5); off += 5;
    pack_bits(bits, off, 0x1234, 16); off += 16;
    pack_bits(bits, off, 2, 2); off += 2;    /* immediate */
    CHECK(off == 59, "table A.26 two-SCK body is 59 bits");
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_valid && state->tetra_mm_ck_change_ack,
          "SCK change becomes valid and retains acknowledgement");
    CHECK(state->tetra_mm_ck_change_security_class == 3
          && state->tetra_mm_ck_change_key_type == 0
          && state->tetra_mm_ck_change_sck_count == 2,
          "SCK change mandatory fields");
    CHECK(state->tetra_mm_ck_change_sck_number[0] == 3
          && state->tetra_mm_ck_change_sck_version[0] == 0xABCD
          && state->tetra_mm_ck_change_sck_number[1] == 31
          && state->tetra_mm_ck_change_sck_version[1] == 0x1234,
          "both repeated SCK descriptors retained");
    CHECK(state->tetra_mm_ck_change_time_type == 2,
          "immediate SCK activation retained");

    /* The final bit of Time Type is mandatory; truncation is atomic. */
    pack_bits(bits, 15, 7, 5);
    tetra_mm_dispatch(bits, off - 1, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_sck_number[0] == 3
          && state->tetra_mm_ck_change_time_type == 2,
          "truncated SCK demand preserves prior snapshot");

    /* Zero SCK count selects a DMO subset and carries its common version. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 0, 3); off += 3;
    pack_bits(bits, off, 1, 1); off++;       /* DMO */
    pack_bits(bits, off, 0, 4); off += 4;    /* subset */
    pack_bits(bits, off, 9, 4); off += 4;
    pack_bits(bits, off, 17, 5); off += 5;
    pack_bits(bits, off, 0xBEEF, 16); off += 16;
    pack_bits(bits, off, 3, 2); off += 2;    /* currently in use */
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_sck_use == 1
          && state->tetra_mm_ck_change_sck_count == 0
          && state->tetra_mm_ck_change_sck_grouping == 9
          && state->tetra_mm_ck_change_sck_subset == 17
          && state->tetra_mm_ck_change_sck_vn == 0xBEEF,
          "DMO SCK subset fields retained");

    /* Two GCK descriptors followed by a 48-bit network time. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 2, 3); off += 3;    /* GCK */
    pack_bits(bits, off, 2, 4); off += 4;
    pack_bits(bits, off, 0x0102, 16); off += 16;
    pack_bits(bits, off, 0x0304, 16); off += 16;
    pack_bits(bits, off, 0xA0B0, 16); off += 16;
    pack_bits(bits, off, 0xC0D0, 16); off += 16;
    pack_bits(bits, off, 1, 2); off += 2;    /* network time */
    pack_bits(bits, off, 0x1234, 16); off += 16;
    pack_bits(bits, off, 0x56789ABC, 32); off += 32;
    CHECK(off == 128, "table A.26 two-GCK network-time body is 128 bits");
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_key_type == 2
          && state->tetra_mm_ck_change_gck_count == 2,
          "GCK change and repeat count retained");
    CHECK(state->tetra_mm_ck_change_gck_number[0] == 0x0102
          && state->tetra_mm_ck_change_gck_version[0] == 0x0304
          && state->tetra_mm_ck_change_gck_number[1] == 0xA0B0
          && state->tetra_mm_ck_change_gck_version[1] == 0xC0D0,
          "both repeated GCK descriptors retained");
    CHECK(state->tetra_mm_ck_change_network_time == UINT64_C(0x123456789ABC),
          "48-bit key-change network time retained");

    /* Combined Class 3 activation at an absolute IV. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 3, 3); off += 3;
    pack_bits(bits, off, 0xCAFE, 16); off += 16;
    pack_bits(bits, off, 0x1357, 16); off += 16;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 17, 5); off += 5;
    pack_bits(bits, off, 45, 6); off += 6;
    pack_bits(bits, off, 0x2468, 16); off += 16;
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_key_type == 3
          && state->tetra_mm_ck_change_cck_id == 0xCAFE
          && state->tetra_mm_ck_change_gck_vn == 0x1357,
          "combined Class 3 CCK/GCK fields retained");
    CHECK(state->tetra_mm_ck_change_time_type == 0
          && state->tetra_mm_ck_change_slot == 3
          && state->tetra_mm_ck_change_frame == 17
          && state->tetra_mm_ck_change_multiframe == 45
          && state->tetra_mm_ck_change_hyperframe == 0x2468,
          "absolute IV fields retained");

    /* Reserved key-change types and a TMO zero-SCK count are invalid. */
    uint16_t saved_cck = state->tetra_mm_ck_change_cck_id;
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    pack_bits(bits, 7, 6, 3);
    tetra_mm_dispatch(bits, 10, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_cck_id == saved_cck,
          "reserved key-change type preserves prior snapshot");
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_CK_CHANGE_DEMAND, 4);
    pack_bits(bits, 7, 0, 3);
    pack_bits(bits, 10, 0, 1); /* TMO */
    pack_bits(bits, 11, 0, 4); /* invalid subset count */
    pack_bits(bits, 15, 2, 2);
    tetra_mm_dispatch(bits, 17, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_cck_id == saved_cck,
          "TMO zero-SCK count preserves prior snapshot");

    /* Remaining table A.71 key types have their own conditional widths. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 1, 3); off += 3;    /* CCK */
    pack_bits(bits, off, 0x9876, 16); off += 16;
    pack_bits(bits, off, 3, 2); off += 2;
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_key_type == 1
          && state->tetra_mm_ck_change_cck_id == 0x9876,
          "individual CCK change retained");
    CHECK(state->tetra_mm_ck_change_sck_count == 0
          && state->tetra_mm_ck_change_gck_count == 0,
          "CCK change clears prior repeated-key lists");

    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 0, 2); off += 2;
    pack_bits(bits, off, 4, 3); off += 3;    /* all GCKs */
    pack_bits(bits, off, 0x7777, 16); off += 16;
    pack_bits(bits, off, 2, 2); off += 2;
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_key_type == 4
          && state->tetra_mm_ck_change_gck_vn == 0x7777,
          "all-GCK version change retained");

    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_CK_CHANGE_DEMAND, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 1, 2); off += 2;
    pack_bits(bits, off, 5, 3); off += 3;    /* no cipher key */
    pack_bits(bits, off, 3, 2); off += 2;
    tetra_mm_dispatch(bits, off, 4, opts, state);
    CHECK(state->tetra_mm_ck_change_key_type == 5
          && state->tetra_mm_ck_change_security_class == 1
          && state->tetra_mm_ck_change_time_type == 3,
          "no-cipher security-class change retained");

    free(state);
    free(opts);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.11.  The example carries
 * Subscriber Class followed by New Registered Area and Group Identity
 * Location Accept Type 4 elements.  Their nested contents are opaque to the
 * current state model, but every standardized length and terminating M-bit
 * must be present before the visible LU Accept state is replaced. */
static void test_annex_e11_lu_accept_type4_chain(void)
{
    uint8_t bits[256] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, 4); off += 4;
    pack_bits(bits, off, 5, 3); off += 3;       /* accept type: any */
    pack_bits(bits, off, 1, 1); off++;          /* O-bit */
    pack_bits(bits, off, 0, 1); off++;          /* no SSI */
    pack_bits(bits, off, 0, 1); off++;          /* no address extension */
    pack_bits(bits, off, 1, 1); off++;          /* Subscriber Class present */
    pack_bits(bits, off, 0xA55A, 16); off += 16;
    pack_bits(bits, off, 0, 1); off++;          /* no energy saving */
    pack_bits(bits, off, 0, 1); off++;          /* no SCCH information */

    pack_bits(bits, off, 1, 1); off++;          /* first outer M-bit */
    pack_bits(bits, off, 2, 4); off += 4;       /* New Registered Area */
    pack_bits(bits, off, 42, 11); off += 11;
    pack_bits(bits, off, 2, 6); off += 6;       /* two repeated LA entries */
    pack_bits(bits, off, 3, 3); off += 3;       /* first LA timer */
    pack_bits(bits, off, 0x1234, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;          /* first LA O-bit */
    pack_bits(bits, off, 6, 3); off += 3;       /* second LA timer */
    pack_bits(bits, off, 0x2345, 14); off += 14;
    pack_bits(bits, off, 0, 1); off++;          /* second LA O-bit */

    pack_bits(bits, off, 1, 1); off++;          /* another outer element */
    pack_bits(bits, off, 5, 4); off += 4;       /* Group Identity Location Accept */
    pack_bits(bits, off, 55, 11); off += 11;
    pack_bits(bits, off, 1, 1); off++;          /* group identity accepted */
    pack_bits(bits, off, 0, 1); off++;          /* reserved */
    pack_bits(bits, off, 1, 1); off++;          /* nested O-bit */
    pack_bits(bits, off, 1, 1); off++;          /* nested M-bit */
    pack_bits(bits, off, 7, 4); off += 4;       /* Group Identity Downlink */
    pack_bits(bits, off, 35, 11); off += 11;
    pack_bits(bits, off, 1, 6); off += 6;       /* one repeated identity */
    pack_bits(bits, off, 1, 1); off++;          /* detachment */
    pack_bits(bits, off, 2, 2); off += 2;       /* detachment reason: any */
    pack_bits(bits, off, 0, 2); off += 2;       /* address type GSSI */
    pack_bits(bits, off, 0x456789, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;          /* nested terminal M-bit */
    pack_bits(bits, off, 0, 1); off++;          /* outer terminal M-bit */
    CHECK(off == 159, "Annex E.11 MM body is exactly 159 bits");

    tetra_mm_dispatch(bits, off, 6, opts, state);
    CHECK(state->tetra_mm_lu_accept_valid == 1
          && state->tetra_mm_lu_accept_type == 5,
          "Annex E.11 LU Accept accepted");
    CHECK(state->tetra_mm_lu_accept_subscriber_class_valid == 1
          && state->tetra_mm_lu_accept_subscriber_class == 0xA55A,
          "Annex E.11 Subscriber Class published");

    /* Change visible fields, then stop inside the second outer Type 4 value.
     * The previous complete message must remain visible. */
    pack_bits(bits, 4, 2, 3);
    pack_bits(bits, 11, 0xBEEF, 16);
    tetra_mm_dispatch(bits, 157, 6, opts, state);
    CHECK(state->tetra_mm_lu_accept_type == 5
          && state->tetra_mm_lu_accept_subscriber_class == 0xA55A,
          "truncated Annex E.11 nested value preserves LU state");

    /* Both values are complete at bit 158; the outer terminal M-bit remains
     * mandatory for the complete D-LOCATION UPDATE ACCEPT envelope. */
    tetra_mm_dispatch(bits, 158, 6, opts, state);
    CHECK(state->tetra_mm_lu_accept_type == 5
          && state->tetra_mm_lu_accept_subscriber_class == 0xA55A,
          "missing Annex E.11 outer M-bit preserves LU state");

    free(state);
    free(opts);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.3 table E.9. */
static void test_annex_e9_group_identity_downlink(void)
{
    uint8_t bits[256] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(bits, off, TETRA_MM_D_ATTACH_DETACH_GROUP, 4); off += 4;
    pack_bits(bits, off, 0, 1); off++;          /* no group identity report */
    pack_bits(bits, off, 1, 1); off++;          /* acknowledgement requested */
    pack_bits(bits, off, 0, 1); off++;          /* attach/detach mode: any */
    pack_bits(bits, off, 1, 1); off++;          /* O-bit */
    pack_bits(bits, off, 1, 1); off++;          /* M-bit */
    pack_bits(bits, off, 7, 4); off += 4;       /* Group Identity Downlink */
    pack_bits(bits, off, 67, 11); off += 11;
    pack_bits(bits, off, 2, 6); off += 6;       /* two repeated entries */
    pack_bits(bits, off, 0, 1); off++;          /* first: attachment */
    pack_bits(bits, off, 2, 2); off += 2;       /* attachment lifetime */
    pack_bits(bits, off, 5, 3); off += 3;       /* class of usage */
    pack_bits(bits, off, 0, 2); off += 2;       /* GSSI address */
    pack_bits(bits, off, 0x123456, 24); off += 24;
    pack_bits(bits, off, 1, 1); off++;          /* second: detachment */
    pack_bits(bits, off, 3, 2); off += 2;       /* detachment reason */
    pack_bits(bits, off, 0, 2); off += 2;       /* GSSI address */
    pack_bits(bits, off, 0x654321, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;          /* terminal M-bit */
    CHECK(off == 92, "Annex E.9 MM body is exactly 92 bits");

    tetra_mm_dispatch(bits, off, 7, opts, state);
    CHECK(state->tetra_mm_group_identity_valid == 1
          && state->tetra_mm_group_entry_count == 2,
          "Annex E.9 two-entry Group Identity Downlink accepted");
    CHECK(state->tetra_mm_group_entry_action[0] == 0
          && state->tetra_mm_group_entry_attachment_lifetime[0] == 2
          && state->tetra_mm_group_entry_class_of_usage[0] == 5
          && state->tetra_mm_group_entry_gssi[0] == 0x123456,
          "Annex E.9 attachment fields published");
    CHECK(state->tetra_mm_group_entry_action[1] == 1
          && state->tetra_mm_group_entry_detachment_reason[1] == 3
          && state->tetra_mm_group_entry_gssi[1] == 0x654321,
          "Annex E.9 detachment fields published");

    /* A complete 67-bit collection still needs the outer terminal M-bit. */
    pack_bits(bits, 38, 0x234567, 24);
    tetra_mm_dispatch(bits, off - 1, 7, opts, state);
    CHECK(state->tetra_mm_group_entry_count == 2
          && state->tetra_mm_group_entry_gssi[0] == 0x123456,
          "missing Annex E.9 terminal M-bit preserves group snapshot");

    /* Table 16.54 conditional address forms: GTSI, (V)GSSI, and
     * GTSI-(V)GSSI. */
    memset(bits, 0, sizeof(bits)); off = 0;
    pack_bits(bits, off, TETRA_MM_D_ATTACH_DETACH_GROUP, 4); off += 4;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 7, 4); off += 4;
    pack_bits(bits, off, 171, 11); off += 11;
    pack_bits(bits, off, 3, 6); off += 6;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 1, 2); off += 2;
    pack_bits(bits, off, 2, 3); off += 3;
    pack_bits(bits, off, 1, 2); off += 2;
    pack_bits(bits, off, 0x111111, 24); off += 24;
    pack_bits(bits, off, 0x222222, 24); off += 24;
    pack_bits(bits, off, 1, 1); off++;
    pack_bits(bits, off, 2, 2); off += 2;
    pack_bits(bits, off, 2, 2); off += 2;
    pack_bits(bits, off, 0x333333, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 7, 3); off += 3;
    pack_bits(bits, off, 3, 2); off += 2;
    pack_bits(bits, off, 0x444444, 24); off += 24;
    pack_bits(bits, off, 0x555555, 24); off += 24;
    pack_bits(bits, off, 0x666666, 24); off += 24;
    pack_bits(bits, off, 0, 1); off++;
    CHECK(off == 196, "table 16.54 conditional-address body is 196 bits");
    tetra_mm_dispatch(bits, off, 7, opts, state);
    CHECK(state->tetra_mm_group_entry_count == 3,
          "three conditional group-address forms accepted");
    CHECK(state->tetra_mm_group_entry_address_type[0] == 1
          && state->tetra_mm_group_entry_gssi[0] == 0x111111
          && state->tetra_mm_group_entry_extension[0] == 0x222222,
          "GTSI group address retained");
    CHECK(state->tetra_mm_group_entry_address_type[1] == 2
          && state->tetra_mm_group_entry_vgssi[1] == 0x333333,
          "visitor GSSI retained");
    CHECK(state->tetra_mm_group_entry_address_type[2] == 3
          && state->tetra_mm_group_entry_gssi[2] == 0x444444
          && state->tetra_mm_group_entry_extension[2] == 0x555555
          && state->tetra_mm_group_entry_vgssi[2] == 0x666666,
          "GTSI with visitor GSSI retained");

    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0, TETRA_MM_D_ATTACH_DETACH_GROUP, 4);
    pack_bits(bits, 4, 1, 1);
    pack_bits(bits, 5, 0, 1);
    pack_bits(bits, 6, 0, 1);
    pack_bits(bits, 7, 0, 1); /* no optional entries */
    tetra_mm_dispatch(bits, 8, 7, opts, state);
    CHECK(state->tetra_mm_group_entry_count == 0
          && state->tetra_mm_group_entry_gssi[0] == 0,
          "later group message without entries clears the old snapshot");

    free(state);
    free(opts);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
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

    if (g_failures == 0) {
        printf("PASS  tetra_mm_dispatch: all checks passed\n");
        return 0;
    } else {
        printf("FAIL  tetra_mm_dispatch: %d check(s) failed\n", g_failures);
        return 1;
    }
}
