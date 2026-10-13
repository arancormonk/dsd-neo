// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * CMCE call control PDUs (EN 300 392-2 clause 14.7.1) against their tables: D-CONNECT, the floor control PDUs
 * (D-TX GRANTED, D-TX CEASED, D-TX CONTINUE, D-TX WAIT and D-TX INTERRUPT) and D-FACILITY with its independently
 * encoded SS-PDUs (annex E.1.2), checked through the summary line each prints and the call record it updates. A PDU
 * that does not fit, or ends without its O-bit or terminating M-bit, acts on nothing.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_calls.h"
#include "tetra_state.h"
#include "tetra_test_log.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  PASS: %s\n", msg);                                                                               \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                           \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* Write @p nbits bits of @p val, MSB first, at @p offset. */
static void
pack_bits(uint8_t* out, uint32_t val, int offset, int nbits) {
    for (int i = nbits - 1; i >= 0; i--) {
        out[offset++] = (uint8_t)((val >> i) & 1u);
    }
}

static dsd_state*
alloc_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

/* Frees the TETRA state extension with the decoder state. */
static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

static dsd_opts*
alloc_opts(void) {
    return (dsd_opts*)calloc(1, sizeof(dsd_opts));
}

/* A CMCE PDU behind its protocol discriminator (EN 300 392-2 table 18.87): the TM-SDU the MLE routes. */
static void
wrap_mle_cmce(const uint8_t* cmce_body, int cmce_nbits, uint8_t* out, int* out_nbits) {
    DSD_MEMSET(out, 0, (size_t)(3 + cmce_nbits));
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    DSD_MEMCPY(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = 3 + cmce_nbits;
}

/*
 * Build a D-TX-GRANTED CMCE PDU with granted SSI present.
 *
 * CMCE D-TX-GRANTED layout (PDU type=11): mandatory call and floor-control
 * fields, O-bit, notification P-bit, transmitting-party P-bit/TPTI/SSI, and
 * the terminating M-bit. Total: 54 bits.
 */
static void
build_d_tx_granted(uint8_t* cmce, uint32_t granted_ssi) {
    DSD_MEMSET(cmce, 0, 54);
    pack_bits(cmce, 11u, 0, 5);           /* D-TX-GRANTED */
    pack_bits(cmce, 1u, 5, 14);           /* call identifier */
    pack_bits(cmce, 3u, 19, 2);           /* granted to another */
    pack_bits(cmce, 1u, 21, 1);           /* request permission */
    pack_bits(cmce, 0u, 22, 1);           /* encryption */
    pack_bits(cmce, 0u, 23, 1);           /* reserved */
    pack_bits(cmce, 1u, 24, 1);           /* O-bit */
    pack_bits(cmce, 0u, 25, 1);           /* notification absent */
    pack_bits(cmce, 1u, 26, 1);           /* TPTI present */
    pack_bits(cmce, 1u, 27, 2);           /* TPTI = SSI */
    pack_bits(cmce, granted_ssi, 29, 24); /* SSI */
    pack_bits(cmce, 0u, 53, 1);           /* terminating M-bit */
}

/* Call 1 to group @p gssi: a D-SETUP (table 14.15) in the MAC context of a MAC-RESOURCE addressed to the group. */
static void
setup_group_call(dsd_state* st, dsd_opts* opt, uint32_t gssi) {
    uint8_t cmce[41];
    DSD_MEMSET(cmce, 0, sizeof cmce);
    pack_bits(cmce, TETRA_CMCE_D_SETUP, 0, 5);
    pack_bits(cmce, 1u, 5, 14);    /* call identifier */
    pack_bits(cmce, 0x04u, 25, 8); /* basic service: clear speech, point to multipoint */
    uint8_t mle[3 + 41];
    int mle_nbits;
    wrap_mle_cmce(cmce, 41, mle, &mle_nbits);
    tetra_mac_sdu sdu;
    DSD_MEMSET(&sdu, 0, sizeof sdu);
    sdu.addr.type = TETRA_MAC_ADDR_SSI;
    sdu.addr.ssi = gssi;
    sdu.has_addr = 1;
    tetra_mle_dispatch_sdu(mle, mle_nbits, 0, &sdu, opt, st);
}

/* Call @p call_id, set up by a D-SETUP (table 14.15) to a group. */
static void
setup_call(dsd_state* st, dsd_opts* opt, uint16_t call_id) {
    uint8_t mle[44];
    DSD_MEMSET(mle, 0, sizeof mle);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(mle, TETRA_CMCE_D_SETUP, 3, 5);
    pack_bits(mle, call_id, 8, 14);
    pack_bits(mle, 0x04u, 28, 8); /* basic service: clear speech, point to multipoint */
    tetra_mle_dispatch(mle, 44, 0, opt, st);
}

/* D-CONNECT mandatory fields, ETSI EN 300 392-2 table 14.7. */
static void
test_d_connect_parse(void) {
    printf("[test_d_connect_parse]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);
    pack_bits(cmce, 0x2345u, 5, 14);
    pack_bits(cmce, 9u, 19, 4);
    pack_bits(cmce, 1u, 23, 1);
    pack_bits(cmce, 1u, 24, 1);
    pack_bits(cmce, 3u, 25, 2);
    pack_bits(cmce, 1u, 27, 1);
    pack_bits(cmce, 1u, 28, 1);
    pack_bits(cmce, 0u, 29, 1); /* O-bit */

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_call(st, opt, 0x2345);
    char log[256];
    tetra_test_mle_logged(mle, n, 0, opt, st, log, sizeof log);

    const tetra_call_t* const c = tetra_calls_peek(st, 0x2345);
    CHECK(c && c->phase == TETRA_CALL_CONNECTED, "D-CONNECT: the call it names is connected");
    CHECK(c && c->timeout == 9, "D-CONNECT: call timeout");
    CHECK(strstr(log, "[TETRA CMCE D-CONNECT] CC=0 call=9029 timeout=9 grant=3 request=1") != NULL,
          "D-CONNECT: fields after the hook and simplex/duplex bits");

    free_state(st);
    free(opt);
}

/* D-CONNECT connects the call it names, and makes no record for a call no D-SETUP was heard for. */
static void
test_d_connect_call_active(void) {
    printf("[test_d_connect_call_active]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mle_dispatch(mle, n, 0, opt, st);
    CHECK(tetra_calls_peek(st, 0) == NULL, "D-CONNECT of an unknown call: no record");
    setup_call(st, opt, 0);
    tetra_mle_dispatch(mle, n, 0, opt, st);
    CHECK(tetra_calls_peek(st, 0) && tetra_calls_peek(st, 0)->phase == TETRA_CALL_CONNECTED,
          "D-CONNECT: the known call is connected");

    free_state(st);
    free(opt);
}

/* D-CONNECT transmission control fields. */
static void
test_d_connect_transmission_control(void) {
    printf("[test_d_connect_transmission_control]\n");

    uint8_t cmce[30];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);
    pack_bits(cmce, 3u, 25, 2);
    pack_bits(cmce, 1u, 27, 1);
    pack_bits(cmce, 1u, 28, 1);
    pack_bits(cmce, 0u, 29, 1); /* O-bit */

    uint8_t mle[3 + 30];
    int n;
    wrap_mle_cmce(cmce, 30, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    char log[256];
    tetra_test_mle_logged(mle, n, 0, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-CONNECT] CC=0 call=0 timeout=0 grant=3 request=1") != NULL,
          "D-CONNECT: transmission grant and request permission");

    free_state(st);
    free(opt);
}

/* D-CONNECT too-short (only the PDU type). */
static void
test_d_connect_too_short(void) {
    printf("[test_d_connect_too_short]\n");

    uint8_t cmce[5];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 2u, 0, 5);

    uint8_t mle[9 + 5];
    int n;
    wrap_mle_cmce(cmce, 5, mle, &n);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    char log[256];
    tetra_test_mle_logged(mle, n, 0, opt, st, log, sizeof log);

    CHECK(tetra_calls_peek(st, 0) == NULL, "D-CONNECT short: no call");
    CHECK(strstr(log, "[TETRA CMCE D-CONNECT] CC=0 call=") == NULL, "D-CONNECT short: not decoded");

    free_state(st);
    free(opt);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.2 table E.8.  This is the
 * D-CONNECT example carrying a 42-bit SS-AL INVOKE1 ACK Facility as a
 * length-delimited type-3 element.  The SS-AL values come from
 * EN 300 392-9 (SS type 21) and EN 300 392-12-21 (INVOKE1 ACK type 11). */
static void
test_d_connect_annex_e8_ss_al_facility(void) {
    printf("[test_d_connect_annex_e8_ss_al_facility]\n");

    dsd_state* st = alloc_state();
    dsd_opts* op = alloc_opts();
    uint8_t cmce[128];
    uint8_t mle[136];
    int mle_nbits = 0;
    int off = 0;
    DSD_MEMSET(cmce, 0, sizeof(cmce));

    pack_bits(cmce, TETRA_CMCE_D_CONNECT, off, 5);
    off += 5;
    pack_bits(cmce, 0x1234, off, 14);
    off += 14; /* call identifier: any */
    pack_bits(cmce, 9, off, 4);
    off += 4; /* call time-out: any */
    pack_bits(cmce, 0, off, 1);
    off++; /* hook method */
    pack_bits(cmce, 0, off, 1);
    off++; /* simplex */
    pack_bits(cmce, 1, off, 2);
    off += 2; /* transmission present */
    pack_bits(cmce, 0, off, 1);
    off++; /* request permission */
    pack_bits(cmce, 0, off, 1);
    off++; /* call ownership */
    pack_bits(cmce, 1, off, 1);
    off++; /* O-bit */
    pack_bits(cmce, 0, off, 1);
    off++; /* no call priority */
    pack_bits(cmce, 0, off, 1);
    off++; /* no basic service */
    pack_bits(cmce, 0, off, 1);
    off++; /* no temporary address */
    pack_bits(cmce, 0, off, 1);
    off++; /* no notification */
    pack_bits(cmce, 1, off, 1);
    off++; /* M-bit: Facility follows */
    pack_bits(cmce, 3, off, 4);
    off += 4; /* type-3 Facility identifier */
    pack_bits(cmce, 42, off, 11);
    off += 11;
    pack_bits(cmce, 21, off, 6);
    off += 6; /* SS-AL */
    pack_bits(cmce, 11, off, 5);
    off += 5; /* INVOKE1 ACK */
    pack_bits(cmce, 1, off, 2);
    off += 2; /* affected party is SSI */
    pack_bits(cmce, 0x345678, off, 24);
    off += 24;
    pack_bits(cmce, 0, off, 4);
    off += 4; /* invocation accepted */
    pack_bits(cmce, 0, off, 1);
    off++; /* SS-PDU O-bit */
    pack_bits(cmce, 0, off, 1);
    off++; /* D-CONNECT terminal M-bit */
    CHECK(off == 93, "Annex E.8 CMCE body is exactly 93 bits");

    tetra_call_event setup = {.kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    setup.call_id = 0x1234;
    tetra_calls_on_event(op, st, &setup, NULL);
    setup.call_id = 0x2222;
    tetra_calls_on_event(op, st, &setup, NULL);
    char log[256];
    wrap_mle_cmce(cmce, off, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 3, op, st, log, sizeof log);
    CHECK(tetra_calls_peek(st, 0x1234)->phase == TETRA_CALL_CONNECTED, "Annex E.8 D-CONNECT accepted");
    CHECK(tetra_calls_peek(st, 0x1234)->timeout == 9, "Annex E.8 mandatory call fields published");
    CHECK(strstr(log, "[TETRA CMCE D-CONNECT] CC=3 call=4660 timeout=9 grant=1") != NULL,
          "Annex E.8 transmission grant read");

    /* The 42-bit Facility is length-delimited.  Stopping inside it must not
     * replace the last complete call state. */
    pack_bits(cmce, 0x2222, 5, 14);
    wrap_mle_cmce(cmce, 91, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, op, st);
    CHECK(tetra_calls_peek(st, 0x2222)->phase == TETRA_CALL_SETUP, "truncated Annex E.8 Facility preserves call state");

    /* The value is complete at bit 92, but Annex E still requires the
     * terminating outer M-bit at bit 93. */
    wrap_mle_cmce(cmce, 92, mle, &mle_nbits);
    tetra_mle_dispatch(mle, mle_nbits, 3, op, st);
    CHECK(tetra_calls_peek(st, 0x2222)->phase == TETRA_CALL_SETUP,
          "missing Annex E.8 terminal M-bit preserves call state");

    free_state(st);
    free(op);
}

static void
test_tx_granted_identity_set(void) {
    printf("[test_tx_granted_identity_set]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 88888u);

    uint8_t mle[9 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_group_call(st, opt, 44444u);
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    const tetra_call_t* const c = tetra_calls_peek(st, 1);
    CHECK(c && c->talker_ssi == 88888u, "TX-GRANTED: talker = 88888");
    CHECK(c && c->phase == TETRA_CALL_TX_GRANTED, "TX-GRANTED: transmission granted");
    CHECK(c && c->called_ssi == 44444u, "TX-GRANTED: the called group is retained");

    free_state(st);
    free(opt);
}

static void
test_tx_granted_without_channel_assignment(void) {
    printf("[test_tx_granted_without_channel_assignment]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 12345u);

    uint8_t mle[3 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    setup_group_call(st, opt, 44444u);
    tetra_calls_find(st, 1)->alloc.freq_hz = 390000000L;

    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    const tetra_call_t* const c = tetra_calls_peek(st, 1);
    CHECK(c && c->talker_ssi == 12345u, "TX-GRANTED: granted SSI is retained");
    CHECK(c && c->alloc.freq_hz == 390000000L, "TX-GRANTED: absent channel IE does not overwrite the call's channel");

    free_state(st);
    free(opt);
}

static void
test_tx_granted_floor_state(void) {
    printf("[test_tx_granted_floor_state]\n");

    uint8_t cmce[54];
    build_d_tx_granted(cmce, 111u);

    uint8_t mle[3 + 54];
    int mle_nbits;
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    setup_group_call(st, opt, 44444u);
    tetra_mle_dispatch(mle, mle_nbits, 0, opt, st);

    CHECK(tetra_calls_peek(st, 1)->phase == TETRA_CALL_TX_GRANTED, "TX-GRANTED: floor grant marked valid");

    free_state(st);
    free(opt);
}

static void
test_d_tx_granted_request_and_reserved(void) {
    printf("[test_d_tx_granted_request_and_reserved]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    uint8_t cmce[100];
    int n = 0;

    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, n, 5);
    n += 5;
    pack_bits(cmce, 0x123, n, 14);
    n += 14;
    pack_bits(cmce, 3, n, 2);
    n += 2;
    pack_bits(cmce, 1, n, 1);
    n += 1;
    pack_bits(cmce, 0, n, 1);
    n += 1;
    pack_bits(cmce, 1, n, 1);
    n += 1;
    pack_bits(cmce, 0, n, 1);
    n += 1; /* O-bit */

    uint8_t pdu[200];
    int out_n;
    wrap_mle_cmce(cmce, n, pdu, &out_n);
    char log[256];
    tetra_test_mle_logged(pdu, out_n, 0, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-GRANTED] CC=0 call=291 grant=3 request=1 enc=0 reserved=1") != NULL,
          "D-TX GRANTED request permission and reserved bit");

    free_state(st);
    free(opt);
}

static void
test_d_tx_granted_enc_mode(void) {
    /*
     * Minimal Table 14.18 D-TX-GRANTED with encryption control set.
     *
     *   [0-2]  PD = CMCE (2)
     *   [3-7]  CMCE type = 11
     *   [8-21] call identifier
     *   [28-29] transmission grant
     *   [30] request permission, [31] encryption, [32] reserved
     */
    enum { NBITS = 28 };

    uint8_t bits[NBITS];
    DSD_MEMSET(bits, 0, sizeof(bits));

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, 11u, 3, 5); /* D-TX-GRANTED */
    pack_bits(bits, 2u, 8, 14); /* call identifier */
    pack_bits(bits, 3u, 22, 2); /* granted to another user */
    pack_bits(bits, 1u, 24, 1); /* request permission */
    pack_bits(bits, 1u, 25, 1); /* encryption control */
    /* reserved and O-bit remain zero */

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_call_event setup = {.call_id = 2, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(opt, st, &setup, NULL);
    tetra_st(st)->enc_mode = 0; /* start at none */
    tetra_mle_dispatch(bits, NBITS, 0, opt, st);

    CHECK(tetra_calls_peek(st, 2)->e2ee == 1, "D-TX-GRANTED encryption control marks the call end-to-end encrypted");
    CHECK(tetra_st(st)->enc_mode == 0, "D-TX-GRANTED leaves the air interface encryption mode alone");

    free_state(st);
    free(opt);
}

static void
test_d_tx_ceased(void) {
    printf("[test_d_tx_ceased]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* PDU type, 14-bit call identifier, request permission. */
    uint8_t cmce[21];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, 9, 0, 5); /* D-TX-CEASED */
    pack_bits(cmce, 0x1234, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* requests remain permitted */
    pack_bits(cmce, 0, 20, 1); /* O-bit */

    /* Pre-condition: call 0x1234 has a transmission. */
    tetra_call_event setup = {.call_id = 0x1234, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(opt, st, &setup, NULL);
    tetra_calls_find(st, 0x1234)->phase = TETRA_CALL_TX_GRANTED;

    uint8_t pdu[21 + 3];
    char log[256];
    int n;
    wrap_mle_cmce(cmce, 21, pdu, &n);
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-CEASED] CC=0 call=4660 request=1") != NULL,
          "D-TX-CEASED request permission extracted");
    CHECK(tetra_calls_peek(st, 0x1234)->phase == TETRA_CALL_CEASED, "D-TX-CEASED ends the transmission");

    free_state(st);
    free(opt);
}

static void
test_floor_control_events(void) {
    printf("[test_floor_control_events]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();
    uint8_t pdu[9 + 32];
    char log[256];
    int n;

    /* D-TX-CONTINUE: call=0x1234, continue=1, request=1. */
    uint8_t cmce[32];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_CONTINUE, 0, 5);
    pack_bits(cmce, 0x1234, 5, 14);
    pack_bits(cmce, 1, 19, 1);
    pack_bits(cmce, 1, 20, 1);
    pack_bits(cmce, 0, 21, 1); /* O-bit */
    wrap_mle_cmce(cmce, 22, pdu, &n);
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-TX-CONTINUE] CC=0 call=4660 request=1 notif=0 continue=1") != NULL,
          "D-TX-CONTINUE call id, continue value and request permission decoded");

    /* D-TX-INTERRUPT uses the same mandatory grant fields as D-TX-GRANTED. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, 0, 5);
    pack_bits(cmce, 0x22, 5, 14);
    pack_bits(cmce, 3, 19, 2);
    pack_bits(cmce, 1, 21, 1);
    pack_bits(cmce, 1, 22, 1);
    pack_bits(cmce, 0, 24, 1); /* O-bit */
    wrap_mle_cmce(cmce, 25, pdu, &n);
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-TX-INTERRUPT] CC=0 call=34 request=1 notif=0 continue=0 grant=3 enc=1") != NULL,
          "D-TX-INTERRUPT grant and encryption control decoded");

    /* D-TX-WAIT: call id plus request permission. */
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, 0, 5);
    pack_bits(cmce, 0x321, 5, 14);
    pack_bits(cmce, 0, 19, 1);
    pack_bits(cmce, 0, 20, 1); /* O-bit */
    wrap_mle_cmce(cmce, 21, pdu, &n);
    tetra_test_mle_logged(pdu, n, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-TX-WAIT] CC=0 call=801 request=0") != NULL, "D-TX-WAIT call id decoded");

    free_state(st);
    free(opt);
}

static void
test_d_tx_expanded(void) {
    printf("[test_d_tx_expanded]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* Table 14.17: call id, continue, request permission, notification. */
    uint8_t cmce[32];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_CONTINUE, 0, 5);
    pack_bits(cmce, 0x2345, 5, 14);
    pack_bits(cmce, 1, 19, 1); /* continue */
    pack_bits(cmce, 1, 20, 1); /* request permission */
    pack_bits(cmce, 1, 21, 1); /* O-bit */
    pack_bits(cmce, 1, 22, 1); /* notification present */
    pack_bits(cmce, 7, 23, 6); /* notification value */
    pack_bits(cmce, 0, 29, 1); /* terminating M-bit */

    uint8_t mle[64];
    char log[256];
    int mle_nbits;
    wrap_mle_cmce(cmce, 30, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-CONTINUE] CC=7 call=9029 request=1 notif=7 continue=1") != NULL,
          "D-TX-CONTINUE call id, notification and continue value");

    /* D-TX-WAIT: call_id=0, notification=0 */
    dsd_state_ext_free_all(st);
    DSD_MEMSET(st, 0, sizeof(*st));
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, 0, 5);
    pack_bits(cmce, 0, 5, 14);
    pack_bits(cmce, 0, 19, 1);
    pack_bits(cmce, 0, 20, 1); /* O-bit */

    wrap_mle_cmce(cmce, 21, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-WAIT] CC=7 call=0 request=0 notif=0") != NULL,
          "D-TX-WAIT call id 0 without notification");

    /* Table 14.19 D-TX-INTERRUPT. */
    dsd_state_ext_free_all(st);
    DSD_MEMSET(st, 0, sizeof(*st));
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_TX_INTERRUPT, 0, 5);
    pack_bits(cmce, 0x123, 5, 14);
    pack_bits(cmce, 1, 19, 2); /* transmission not granted */
    pack_bits(cmce, 0, 21, 1); /* request permission */
    pack_bits(cmce, 1, 22, 1); /* encryption */
    pack_bits(cmce, 0, 24, 1); /* O-bit */

    wrap_mle_cmce(cmce, 25, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 7, opt, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-TX-INTERRUPT] CC=7 call=291 request=0 notif=0 continue=0 grant=1 enc=1") != NULL,
          "D-TX-INTERRUPT call id and encryption");

    free_state(st);
    free(opt);
}

static void
test_d_facility_annex_e3(void) {
    printf("[test_d_facility_annex_e3]\n");

    dsd_state* st = alloc_state();
    dsd_opts* op = alloc_opts();

    uint8_t cmce[40];
    DSD_MEMSET(cmce, 0, sizeof(cmce));
    pack_bits(cmce, TETRA_CMCE_D_FACILITY, 0, 5);
    pack_bits(cmce, 1, 5, 4);   /* one SS-PDU */
    pack_bits(cmce, 12, 9, 11); /* twelve content bits */
    pack_bits(cmce, 7, 20, 6);  /* SS-Type */
    pack_bits(cmce, 3, 26, 5);  /* SS-PDU type */
    pack_bits(cmce, 0, 31, 1);  /* SS-PDU optional elements absent */
    pack_bits(cmce, 0, 32, 1);  /* D-FACILITY optional elements absent */

    uint8_t mle[48];
    char log[256];
    int mle_nbits = 0;
    wrap_mle_cmce(cmce, 33, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 1, op, st, log, sizeof log);

    CHECK(strstr(log, "[TETRA CMCE D-FACILITY] CC=1 ss_pdus=1 first_ss_type=7") != NULL,
          "D-FACILITY first SS-Type parsed");

    wrap_mle_cmce(cmce, 25, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 1, op, st, log, sizeof log);
    CHECK(strstr(log, "D-FACILITY] CC=1 ss_pdus") == NULL, "truncated D-FACILITY rejected");

    free_state(st);
    free(op);
}

/* ETSI EN 300 392-2 V3.8.1, Annex E.1.2 table E.2. The contents are
 * independent SS-PDUs: each length covers its own complete encoding and only
 * then does the outer D-FACILITY O-bit follow. */
static void
test_d_facility_annex_e2_two_ss_pdus(void) {
    printf("[test_d_facility_annex_e2_two_ss_pdus]\n");

    dsd_state* st = alloc_state();
    dsd_opts* op = alloc_opts();
    uint8_t cmce[80];
    uint8_t mle[88];
    int mle_nbits = 0;
    DSD_MEMSET(cmce, 0, sizeof(cmce));

    pack_bits(cmce, TETRA_CMCE_D_FACILITY, 0, 5);
    pack_bits(cmce, 2, 5, 4); /* Annex E.2: two independent SS-PDUs */
    pack_bits(cmce, 12, 9, 11);
    pack_bits(cmce, 7, 20, 6); /* first SS-Type */
    pack_bits(cmce, 3, 26, 5); /* first SS-PDU type */
    pack_bits(cmce, 0, 31, 1); /* first SS-PDU O-bit */
    pack_bits(cmce, 12, 32, 11);
    pack_bits(cmce, 12, 43, 6); /* second SS-Type */
    pack_bits(cmce, 5, 49, 5);  /* second SS-PDU type */
    pack_bits(cmce, 0, 54, 1);  /* second SS-PDU O-bit */
    pack_bits(cmce, 0, 55, 1);  /* outer D-FACILITY O-bit */

    char log[256];
    wrap_mle_cmce(cmce, 56, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 2, op, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA CMCE D-FACILITY] CC=2 ss_pdus=2 first_ss_type=7") != NULL,
          "Annex E.2 two-SS-PDU collection accepted with its first independent SS-Type");

    /* Stop inside the second independently length-delimited SS-PDU: the PDU is not decoded, not even its first
     * member. */
    wrap_mle_cmce(cmce, 54, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 2, op, st, log, sizeof log);
    CHECK(strstr(log, "D-FACILITY] CC=2 ss_pdus") == NULL, "truncated second SS-PDU rejects the PDU");

    /* Both SS-PDUs are complete, but Annex E requires the outer O-bit. */
    wrap_mle_cmce(cmce, 55, mle, &mle_nbits);
    tetra_test_mle_logged(mle, mle_nbits, 2, op, st, log, sizeof log);
    CHECK(strstr(log, "D-FACILITY] CC=2 ss_pdus") == NULL, "missing outer D-FACILITY O-bit rejects the PDU");

    free_state(st);
    free(op);
}

static void
test_cmce_constants(void) {
    printf("[test_cmce_constants]\n");
    CHECK(TETRA_CMCE_D_ALERT == 0, "D-ALERT type");
    CHECK(TETRA_CMCE_D_INFO == 5, "D-INFO type");
    CHECK(TETRA_CMCE_D_SETUP == 7, "D-SETUP type");
    CHECK(TETRA_CMCE_D_TX_GRANTED == 11, "D-TX-GRANTED type");
    CHECK(TETRA_CMCE_D_CALL_RESTORE == 14, "D-CALL-RESTORE type");
    CHECK(TETRA_CMCE_D_SDS_DATA == 15, "D-SDS-DATA type");
    CHECK(TETRA_CMCE_D_FACILITY == 16, "D-FACILITY type");
    CHECK(TETRA_CMCE_FUNCTION_NOT_SUPPORTED == 31, "FUNCTION-NOT-SUPPORTED type");
}

int
main(void) {
    test_cmce_constants();
    test_d_connect_parse();
    test_d_connect_call_active();
    test_d_connect_transmission_control();
    test_d_connect_too_short();
    test_d_connect_annex_e8_ss_al_facility();
    test_tx_granted_identity_set();
    test_tx_granted_without_channel_assignment();
    test_tx_granted_floor_state();
    test_d_tx_granted_request_and_reserved();
    test_d_tx_granted_enc_mode();
    test_d_tx_ceased();
    test_floor_control_events();
    test_d_tx_expanded();
    test_d_facility_annex_e3();
    test_d_facility_annex_e2_two_ss_pdus();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
