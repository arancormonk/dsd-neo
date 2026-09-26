// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MLE dispatch + CMCE D-SETUP / D-RELEASE call-state tests.
 *
 * Verifies that tetra_mle_dispatch() correctly updates dsd_state when it
 * processes TM-SDUs carrying CMCE D-SETUP, D-RELEASE, and
 * D-CONNECT messages.
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Write @nbits bits from @val (MSB-first) into out[offset..]. */
static void pack_bits(uint8_t *out, uint32_t val, int offset, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--)
        out[offset++] = (uint8_t)((val >> i) & 1u);
}

/* Expand a frozen MSB-first hexadecimal PDU into the decoder's one-byte-per-bit
 * representation. Golden vectors use this instead of the field builder above
 * so a shared offset error cannot make both the input and expectation pass. */
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

static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

/* -------------------------------------------------------------------------
 * CMCE test vectors use the table 18.87 three-bit protocol discriminator.
 *
 * TM-SDU starts with the 3-bit CMCE protocol discriminator; the CMCE PDU follows.
 *   [15]    call_timeout = 0
 *   [16-18] call_type  =  2 (010b, acknowledged call)
 *   [19]    duplex     = 1
 *   [20]    notif_ind  = 0
 *   [21]    com_type   = 0
 *   [22]    slots      = 0
 *   [23]    calling_party_present = 1
 *   [24]    calling_party_type    = 0 (SSI)
 *   [25-48] calling_party_ssi     = TEST_CALLING_SSI (24 bits)
 * Expected: tetra_call_active=1, tetra_call_type=2, tetra_calling_ssi=TEST_SSI
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 2: Direct CMCE D-RELEASE clears call_active.
 *   [15-18] cause      = 5 (0101b)
 * Expected: tetra_call_active goes from 1 → 0
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 3: Direct CMCE D-CONNECT sets call_active.
 * Expected: tetra_call_active = 1
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 4: D-SETUP without calling party IE — calling_ssi stays 0.
 *
 * Same as Test 1 but calling_party_present = 0.
 * Expected: tetra_call_active=1, tetra_call_type=1, tetra_calling_ssi=0
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 5: CMCE D-TX-GRANTED with granted party SSI.
 *
 * TM-SDU carries the 3-bit CMCE PD followed by Table 14.18 D-TX-GRANTED.
 *   [28-29] transmission grant = 3 (granted to another user)
 *   [30] request permission, [31] encryption, [32] reserved
 *   [33] notification absent, [34] TPTI present
 *   [35-36] TPTI = 1, [37-60] transmitting SSI
 * Expected: tetra_tx_granted_valid=1, tetra_tx_granted_ssi=TEST_GRANTED_SSI
 * ------------------------------------------------------------------------- */
static int test_d_tx_granted(void)
{
    const uint32_t TEST_GRANTED_SSI = 0x4C1A77u;  /* 5053047 */
    enum { NBITS = 57 };

    uint8_t bits[NBITS];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, 11u, 3, 5);
    pack_bits(bits, 7u, 8, 14);
    pack_bits(bits, 3u, 22, 2);
    pack_bits(bits, 1u, 24, 1);
    pack_bits(bits, 0u, 25, 1);
    pack_bits(bits, 0u, 26, 1);
    pack_bits(bits, 1u, 27, 1); /* O-bit */
    pack_bits(bits, 0u, 28, 1); /* notification absent */
    pack_bits(bits, 1u, 29, 1); /* TPTI present */
    pack_bits(bits, 1u, 30, 2);
    pack_bits(bits, TEST_GRANTED_SSI, 32, 24);
    pack_bits(bits, 0u, 56, 1); /* terminating M-bit */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mle_dispatch(bits, NBITS, 0, opts, state);

    int ok = 1;
    if (!state->tetra_tx_granted_valid) {
        fprintf(stderr, "FAIL(d_tx_granted): tetra_tx_granted_valid not set\n");
        ok = 0;
    }
    if (state->tetra_tx_granted_ssi != TEST_GRANTED_SSI) {
        fprintf(stderr, "FAIL(d_tx_granted): tetra_tx_granted_ssi=0x%06X expect=0x%06X\n",
                (unsigned)state->tetra_tx_granted_ssi,
                (unsigned)TEST_GRANTED_SSI);
        ok = 0;
    }

    free(state);
    free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Test 6: CMCE D-TX-CEASED clears tetra_tx_granted_valid.
 *
 *   Direct CMCE PDU type = 9 (D_TX_CEASED, 01001b)
 * Pre-condition: tetra_tx_granted_valid=1
 * Expected: tetra_tx_granted_valid=0
 * ------------------------------------------------------------------------- */
static int test_d_tx_ceased(void)
{
    enum { NBITS = 24 };

    uint8_t bits[NBITS];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, 9u, 3, 5);
    pack_bits(bits, 42u, 8, 14);
    pack_bits(bits, 1u, 22, 1);
    pack_bits(bits, 0u, 23, 1); /* O-bit: no optionals */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();
    state->tetra_tx_granted_valid = 1;  /* pre-condition: grant was active   */

    tetra_mle_dispatch(bits, NBITS, 0, opts, state);

    int ok = 1;
    if (state->tetra_tx_granted_valid != 0) {
        fprintf(stderr, "FAIL(d_tx_ceased): tetra_tx_granted_valid=%u expect=0\n",
                (unsigned)state->tetra_tx_granted_valid);
        ok = 0;
    }

    free(state);
    free(opts);
    return ok;
}

static int test_d_setup_truncated_optional_rejected(void)
{
    uint8_t cmce[48] = {0};
    uint8_t mle[64] = {0};
    int off = 0;
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();

    pack_bits(cmce, TETRA_CMCE_D_SETUP, off, 5); off += 5;
    pack_bits(cmce, 0x1234, off, 14); off += 14;
    pack_bits(cmce, 9, off, 4); off += 4;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 0x32, off, 8); off += 8;
    pack_bits(cmce, 3, off, 2); off += 2;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 7, off, 4); off += 4;
    pack_bits(cmce, 1, off, 1); off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1); off += 1; /* notification claimed, payload absent */

    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(mle + 3, cmce, (size_t)off);
    state->tetra_call_id = 0x55;
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);

    int ok = state->tetra_call_active == 0 && state->tetra_call_id == 0x55;
    if (!ok)
        fprintf(stderr, "FAIL(d_setup_truncated): malformed optional IE changed call state\n");
    free(state);
    free(opts);
    return ok;
}

static void wrap_cmce(uint8_t *mle, const uint8_t *cmce, int cmce_nbits)
{
    memset(mle, 0, 128);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(mle + 3, cmce, (size_t)cmce_nbits);
}

static int test_floor_control_truncated_optionals_rejected(void)
{
    uint8_t cmce[96] = {0};
    uint8_t mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int ok = 1;
    int off = 0;

    /* D-TX-GRANTED: a present notification must carry all six bits. */
    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, off, 5); off += 5;
    pack_bits(cmce, 7, off, 14); off += 14;
    pack_bits(cmce, 3, off, 2); off += 2;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 0, off, 1); off += 1;
    pack_bits(cmce, 0, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_granted_valid = 0;
    state->tetra_tx_granted_ssi = 0x112233u;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (state->tetra_tx_granted_valid != 0 || state->tetra_tx_granted_ssi != 0x112233u)
        ok = 0;

    /* D-TX-CEASED: malformed notification must not clear an active grant. */
    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_CEASED, off, 5); off += 5;
    pack_bits(cmce, 8, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_granted_valid = 1;
    state->tetra_tx_ceased_tx_perm = 0;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (state->tetra_tx_granted_valid != 1 || state->tetra_tx_ceased_tx_perm != 0)
        ok = 0;

    /* D-TX-WAIT: malformed notification must not publish an event. */
    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, off, 5); off += 5;
    pack_bits(cmce, 9, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1; /* O-bit */
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_wait = 0;
    state->tetra_tx_event_call_id = 0x55;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (state->tetra_tx_wait != 0 || state->tetra_tx_event_call_id != 0x55)
        ok = 0;

    if (!ok)
        fprintf(stderr, "FAIL(floor_control_truncated): malformed optional IE changed state\n");
    free(state);
    free(opts);
    return ok;
}

static int test_release_is_scoped_to_active_call(void)
{
    uint8_t cmce[32] = {0};
    uint8_t mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;
    int ok = 1;

    state->tetra_call_active = 1;
    state->tetra_call_id = 0x1234u;
    state->tetra_cmce_release_cause = 3;
    state->tetra_tx_granted_valid = 1;
    state->tetra_tx_granted_ssi = 0x112233u;
    state->tetra_cmce_tx_granted_party_type_valid = 1;
    state->tetra_tx_event_party_ssi_valid = 1;
    state->tetra_tx_interrupted = 1;

    pack_bits(cmce, TETRA_CMCE_D_RELEASE, off, 5); off += 5;
    pack_bits(cmce, 0x1235u, off, 14); off += 14;
    pack_bits(cmce, 19, off, 5); off += 5;
    pack_bits(cmce, 0, off, 1); off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (!state->tetra_call_active || state->tetra_call_id != 0x1234u ||
        state->tetra_cmce_release_cause != 3 || !state->tetra_tx_granted_valid ||
        state->tetra_tx_granted_ssi != 0x112233u ||
        !state->tetra_cmce_tx_granted_party_type_valid ||
        !state->tetra_tx_event_party_ssi_valid || !state->tetra_tx_interrupted)
        ok = 0;

    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_DISCONNECT, off, 5); off += 5;
    pack_bits(cmce, 0x1234u, off, 14); off += 14;
    pack_bits(cmce, 7, off, 5); off += 5;
    pack_bits(cmce, 0, off, 1); off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 0, opts, state);
    if (state->tetra_call_active || state->tetra_call_id != 0x1234u ||
        state->tetra_cmce_release_cause != 7 || state->tetra_tx_granted_valid ||
        state->tetra_tx_granted_ssi || state->tetra_cmce_tx_granted_party_type_valid ||
        state->tetra_tx_event_party_ssi_valid || state->tetra_tx_interrupted)
        ok = 0;

    if (!ok)
        fprintf(stderr, "FAIL(release_scope): unrelated release changed active call\n");
    free(state);
    free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Test 7: D-SETUP with calling party SSI + called party GSSI.
 *
 * TM-SDU carries the 3-bit CMCE PD followed by D-SETUP.
 *   [15]    call_timeout = 0
 *   [16-18] call_type  =  0  (group)
 *   [19-22] duplex/notif/com_type/slots = 0
 *   [23]    calling_party_present = 1
 *   [24]    calling_party_type    = 0 (SSI)
 *   [25-48] calling_party_ssi     = TEST_CALLING_SSI (24 bits)
 *   [49]    called_party_present  = 1
 *   [50-51] called_party_type     = 1 (GSSI/group)
 *   [52-75] called_party_ssi      = TEST_GSSI (24 bits)
 * Expected: tetra_gssi=TEST_GSSI, tetra_call_id=1 and
 *           tetra_calling_ssi=TEST_CALLING_SSI.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 8: D-SETUP with NO calling party IE but WITH called party GSSI.
 *
 * TM-SDU carries the 3-bit CMCE PD followed by D-SETUP.
 *   [15]    call_timeout = 0
 *   [16-18] call_type  =  0  (group)
 *   [19-22] duplex/notif/com_type/slots = 0
 *   [23]    calling_party_present = 0  (absent → off advances to CMCE bit 15)
 *   [24]    called_party_present  = 1  (CMCE bit 15 → MLE bit 9+15=24)
 *   [25-26] called_party_type     = 1  (GSSI)
 *   [27-50] called_party_ssi      = TEST_GSSI2 (24 bits)
 * Expected: tetra_gssi=TEST_GSSI2 and tetra_calling_ssi=0.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 9: D-ALERT sets tetra_call_active.
 *
 *   Direct CMCE PDU type = 0 (D-ALERT)
 * Expected: tetra_call_active=1
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 10: D-CALL-PROCEEDING sets tetra_call_active.
 *
 *   Direct CMCE PDU type = 1 (D-CALL-PROCEEDING)
 * Expected: tetra_call_active=1
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 11: D-STATUS without calling party — pre-coded status stored.
 *
 * CMCE PDU bit layout (relative to CMCE start after the 3-bit PD):
 *   CMCE[0-4]  = PDU type = 7  (D-STATUS)
 *   CMCE[5-20] = pre_coded_status = TEST_STATUS (16 bits)
 * No calling party IE (buffer ends at CMCE bit 21).
 *
 * Expected: tetra_sds_status=TEST_STATUS, tetra_sds_src=0
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 12: D-STATUS with calling party SSI — src stored.
 *
 * CMCE bit layout:
 *   CMCE[0-4]   = 7  (D-STATUS)
 *   CMCE[5-20]  = TEST_STATUS (16 bits)
 *   CMCE[21]    = calling_party_present = 1
 *   CMCE[22]    = calling_party_type    = 0 (SSI)
 *   CMCE[23-46] = TEST_SRC_SSI (24 bits)
 * Total CMCE bits = 47, MLE total = 9 + 47 = 56 bits.
 *
 * Expected: tetra_sds_status=TEST_STATUS, tetra_sds_src=TEST_SRC_SSI
 * ------------------------------------------------------------------------- */
static int test_standard_setup_release_status(void)
{
    uint8_t cmce[160]={0}, mle[192]; int n=0; int ok=1;
    dsd_state *state=alloc_state(); dsd_opts *opts=alloc_opts();
    /* Table 14.15 mandatory D-SETUP plus absent notification/temp and CPTI=1. */
    int off=0;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5); off+=5;
    pack_bits(cmce,0x1234,off,14); off+=14; pack_bits(cmce,9,off,4); off+=4;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0x32,off,8); off+=8; /* circuit=1, enc=1, com=0, slots=2 */
    pack_bits(cmce,3,off,2); off+=2; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,7,off,4); off+=4;
    pack_bits(cmce,1,off,1); off++; /* O-bit */
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,0,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,1,off,2); off+=2; pack_bits(cmce,0x654321,off,24); off+=24;
    pack_bits(cmce,0,off,1); off++; /* M-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off); n=off+3;
    tetra_mle_dispatch(mle,n,2,opts,state);
    if (!state->tetra_call_active || state->tetra_call_id!=0x1234 || state->tetra_call_timeout!=9 ||
        state->tetra_calling_ssi!=0x654321 || state->tetra_cmce_setup_basic_service!=0x32 ||
        state->tetra_cmce_setup_priority!=7 || !state->tetra_cmce_setup_calling_type_valid ||
        state->tetra_cmce_setup_temporary_address_valid || state->tetra_cmce_setup_notification_valid) ok=0;

    /* Table 14.15 optionals: notification, temporary address and CPTI=2
     * (calling SSI plus calling-party extension), in table order. */
    memset(cmce,0,sizeof(cmce)); off=0;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5); off+=5;
    pack_bits(cmce,0x1235,off,14); off+=14; pack_bits(cmce,6,off,4); off+=4;
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0x20,off,8); off+=8; pack_bits(cmce,1,off,2); off+=2;
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,3,off,4); off+=4;
    pack_bits(cmce,1,off,1); off++; /* O-bit */
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,17,off,6); off+=6;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,0xABCDEF,off,24); off+=24;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,2,off,2); off+=2;
    pack_bits(cmce,0x112233,off,24); off+=24; pack_bits(cmce,0x445566,off,24); off+=24;
    pack_bits(cmce,0,off,1); off++; /* M-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if (state->tetra_call_id!=0x1235 || state->tetra_cmce_notif!=17 ||
        !state->tetra_cmce_setup_notification_valid ||
        !state->tetra_cmce_setup_temporary_address_valid ||
        state->tetra_cmce_setup_temporary_address!=0xABCDEF ||
        !state->tetra_cmce_setup_calling_type_valid || state->tetra_cmce_called_type!=2 ||
        state->tetra_calling_ssi!=0x112233 ||
        !state->tetra_cmce_setup_calling_extension_valid ||
        state->tetra_cmce_setup_calling_extension!=0x445566) ok=0;

    /* A later setup without optionals must not expose stale identities. */
    memset(cmce,0,sizeof(cmce)); off=0;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5); off+=5;
    pack_bits(cmce,0x1236,off,14); off+=14; pack_bits(cmce,5,off,4); off+=4;
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,0,off,1); off++;
    pack_bits(cmce,0x20,off,8); off+=8; pack_bits(cmce,0,off,2); off+=2;
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,2,off,4); off+=4;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if (state->tetra_call_id!=0x1236 || state->tetra_calling_ssi ||
        state->tetra_cmce_setup_notification_valid ||
        state->tetra_cmce_setup_temporary_address_valid ||
        state->tetra_cmce_setup_calling_type_valid ||
        state->tetra_cmce_setup_calling_extension_valid) ok=0;

    /* Table 14.14 D-STATUS with CPTI=1. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_STATUS,off,5); off+=5;
    pack_bits(cmce,1,off,2); off+=2; pack_bits(cmce,0x654321,off,24); off+=24; pack_bits(cmce,0xBEEF,off,16); off+=16;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if (state->tetra_sds_src!=0x654321 || state->tetra_sds_status!=0xBEEF) ok=0;

    /* Table 14.12 D-RELEASE. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_RELEASE,off,5); off+=5;
    pack_bits(cmce,0x1236,off,14); off+=14; pack_bits(cmce,19,off,5); off+=5;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if (state->tetra_call_active || state->tetra_call_id!=0x1236 || state->tetra_cmce_release_cause!=19) ok=0;
    /* Tables 14.4, 14.5, 14.7, 14.8 and 14.11 mandatory fields. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_ALERT,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,6,off,3); off+=3;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(state->tetra_d_alert_call_id!=0x2345 || state->tetra_d_alert_timeout!=6 || !state->tetra_d_alert_queued) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CALL_PROCEEDING,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,5,off,3); off+=3; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,0,off,1); off++;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(state->tetra_d_call_proc_call_id!=0x2345 || state->tetra_d_call_proc_timeout!=5 || !state->tetra_d_call_proc_hook) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CONNECT,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,8,off,4); off+=4; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,3,off,2); off+=2; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(!state->tetra_connect_valid || state->tetra_call_id!=0x2345 || state->tetra_cmce_connect_tx_grant!=3 || !state->tetra_cmce_connect_ownership) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CONNECT_ACK,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,4,off,4); off+=4; pack_bits(cmce,2,off,2); off+=2; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(state->tetra_d_connect_ack_call_id!=0x2345 || state->tetra_d_connect_ack_tx_grant!=2 || !state->tetra_d_connect_ack_tx_permission) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_INFO,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(state->tetra_d_info_call_id!=0x2345 || !state->tetra_d_info_call_timeout || !state->tetra_d_info_notification) ok=0;

    /* Table 14.6 D-CALL-RESTORE. */
    memset(cmce,0,sizeof(cmce));off=0;pack_bits(cmce,TETRA_CMCE_D_CALL_RESTORE,off,5);off+=5;
    pack_bits(cmce,0x3456,off,14);off+=14;pack_bits(cmce,3,off,2);off+=2;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(!state->tetra_call_restore_valid || state->tetra_call_restore_id!=0x3456 || state->tetra_call_restore_grant!=3 || !state->tetra_call_restore_reset)ok=0;

    /* Table 14.33 FUNCTION NOT SUPPORTED with call identifier and pointer=0. */
    memset(cmce,0,sizeof(cmce));off=0;pack_bits(cmce,TETRA_CMCE_FUNCTION_NOT_SUPPORTED,off,5);off+=5;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5);off+=5;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,0x3456,off,14);off+=14;pack_bits(cmce,0,off,8);off+=8;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    memset(mle,0,sizeof(mle)); pack_bits(mle,TETRA_MLE_PD_CMCE,0,3); memcpy(mle+3,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    if(!state->tetra_cmce_fns_valid || state->tetra_cmce_fns_rejected_pdu!=TETRA_CMCE_D_SETUP || state->tetra_cmce_fns_call_id!=0x3456 || state->tetra_cmce_fns_pointer!=0)ok=0;

    free(state); free(opts); return ok;
}

/* Real table 18.87 envelopes: a direct CMCE PDU has only the 3-bit protocol
 * discriminator; an MLE PDU adds the table 18.85 3-bit PDU type. */
static int test_standard_mle_envelopes(void)
{
    uint8_t bits[32] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();

    pack_bits(bits, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(bits, TETRA_CMCE_D_INFO, 3, 5);
    pack_bits(bits, 0x1234, 8, 14);
    pack_bits(bits, 1, 22, 1);
    pack_bits(bits, 1, 23, 1);
    pack_bits(bits, 0, 24, 1); /* O-bit */
    tetra_mle_dispatch(bits, 25, 4, opts, state);
    int ok = state->tetra_d_info_valid
          && state->tetra_d_info_call_id == 0x1234
          && state->tetra_d_info_call_timeout
          && state->tetra_d_info_notification;

    memset(bits, 0, sizeof bits);
    pack_bits(bits, TETRA_MLE_PD_MLE, 0, 3);
    pack_bits(bits, TETRA_MLE_D_RESTORE_FAIL, 3, 3);
    pack_bits(bits, 2, 6, 2);
    pack_bits(bits, 0, 8, 1); /* mandatory O-bit */
    tetra_mle_dispatch(bits, 9, 4, opts, state);
    ok = ok && state->tetra_mle_restore_fail_valid
            && state->tetra_mle_restore_fail_cause == 2;

    free(state); free(opts);
    return ok;
}

/* ETSI EN 300 392-2 V3.8.1 Annex E.19-E.24: downlink cell-reselection
 * responses, including their mandatory outer O-bit and protocol-specific SDU. */
static int test_annex_e19_e24_mle_responses(void)
{
    uint8_t bits[80] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    /* E.19: D-NEW-CELL without an MM SDU. */
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3); off += 3;
    pack_bits(bits, 2, off, 2); off += 2;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 2, opts, state);
    int ok = state->tetra_mle_new_cell_valid
          && state->tetra_mle_new_cell_channel_command == 2;

    /* E.20: D-NEW-CELL carrying a minimal D-LOCATION UPDATE ACCEPT. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3); off += 3;
    pack_bits(bits, 1, off, 2); off += 2;
    pack_bits(bits, 0, off, 1); off++;
    pack_bits(bits, TETRA_MM_D_LOCATION_UPDATING_ACCEPT, off, 4); off += 4;
    pack_bits(bits, 5, off, 3); off += 3;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 2, opts, state);
    ok = ok && state->tetra_mle_new_cell_channel_command == 1
            && state->tetra_mm_lu_accept_valid
            && state->tetra_mm_lu_accept_type == 5;

    /* E.21: D-PREPARE-FAIL without an MM SDU. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_PREPARE_FAIL, off, 3); off += 3;
    pack_bits(bits, 3, off, 2); off += 2;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 3, opts, state);
    ok = ok && state->tetra_mle_prepare_fail_valid
            && state->tetra_mle_prepare_fail_cause == 3;

    /* E.22: D-PREPARE-FAIL carrying D-LOCATION UPDATE REJECT. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_PREPARE_FAIL, off, 3); off += 3;
    pack_bits(bits, 1, off, 2); off += 2;
    pack_bits(bits, 0, off, 1); off++;
    pack_bits(bits, TETRA_MM_D_LOCATION_UPDATING_REJECT, off, 4); off += 4;
    pack_bits(bits, 2, off, 3); off += 3;
    pack_bits(bits, 17, off, 5); off += 5;
    pack_bits(bits, 0, off, 1); off++;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 3, opts, state);
    ok = ok && state->tetra_mle_prepare_fail_cause == 1
            && state->tetra_mm_lu_reject_valid
            && state->tetra_mm_lu_reject_type == 2
            && state->tetra_mm_lu_reject_cause == 17;

    /* E.23: D-RESTORE-ACK carrying CMCE D-CALL-RESTORE. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_RESTORE_ACK, off, 3); off += 3;
    pack_bits(bits, 0, off, 1); off++; /* MLE O-bit */
    pack_bits(bits, TETRA_CMCE_D_CALL_RESTORE, off, 5); off += 5;
    pack_bits(bits, 0x2345, off, 14); off += 14;
    pack_bits(bits, 2, off, 2); off += 2;
    pack_bits(bits, 1, off, 1); off++;
    pack_bits(bits, 1, off, 1); off++;
    pack_bits(bits, 0, off, 1); off++; /* CMCE O-bit */
    tetra_mle_dispatch(bits, off, 4, opts, state);
    ok = ok && state->tetra_restore_ack
            && state->tetra_call_restore_valid
            && state->tetra_call_restore_id == 0x2345
            && state->tetra_call_restore_grant == 2;

    /* The ACK and nested call state are atomic when the CMCE O-bit is absent. */
    state->tetra_restore_ack = 0;
    tetra_mle_dispatch(bits, off - 1, 4, opts, state);
    ok = ok && !state->tetra_restore_ack
            && state->tetra_call_restore_id == 0x2345;

    /* E.24: D-RESTORE-FAIL. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_RESTORE_FAIL, off, 3); off += 3;
    pack_bits(bits, 2, off, 2); off += 2;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 5, opts, state);
    ok = ok && state->tetra_mle_restore_fail_valid
            && state->tetra_mle_restore_fail_cause == 2;

    /* No listed response defines Type 2 elements; O=1 must not replace state. */
    state->tetra_mle_new_cell_channel_command = 3;
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_NEW_CELL, off, 3); off += 3;
    pack_bits(bits, 0, off, 2); off += 2;
    pack_bits(bits, 1, off, 1); off++;
    tetra_mle_dispatch(bits, off, 2, opts, state);
    ok = ok && state->tetra_mle_new_cell_channel_command == 3;

    /* Table 18.10: D-CHANNEL RESPONSE with its reserved optionals absent. */
    memset(bits, 0, sizeof bits); off = 0;
    pack_bits(bits, TETRA_MLE_PD_MLE, off, 3); off += 3;
    pack_bits(bits, TETRA_MLE_D_CHANNEL_RESPONSE, off, 3); off += 3;
    pack_bits(bits, 1, off, 1); off++;
    pack_bits(bits, 5, off, 3); off += 3;
    pack_bits(bits, 9, off, 4); off += 4;
    pack_bits(bits, 0, off, 1); off++;
    tetra_mle_dispatch(bits, off, 2, opts, state);
    ok = ok && state->tetra_mle_channel_response_valid
            && state->tetra_mle_channel_response_type == 1
            && state->tetra_mle_channel_response_reason == 5
            && state->tetra_mle_channel_response_retry_delay == 9;

    bits[off - 1] = 1; /* reserved Type 2 fields claimed */
    state->tetra_mle_channel_response_reason = 7;
    tetra_mle_dispatch(bits, off, 2, opts, state);
    ok = ok && state->tetra_mle_channel_response_reason == 7;

    free(state);
    free(opts);
    return ok;
}

/* Tables 14.4 and 14.5: ordered type-2 fields are committed only when every
 * presence flag and every declared value in the received tail is complete. */
static int test_alert_proceeding_optional_atomicity(void)
{
    uint8_t cmce[80] = {0}, tm_sdu[96] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(cmce, TETRA_CMCE_D_ALERT, off, 5); off += 5;
    pack_bits(cmce, 0x1234, off, 14); off += 14;
    pack_bits(cmce, 5, off, 3); off += 3;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0xA5, off, 8); off += 8;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0x2A, off, 6); off += 6;
    pack_bits(cmce, 0, off, 1); off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    int ok = state->tetra_d_alert_valid
          && state->tetra_d_alert_basic_service == 0xA5
          && state->tetra_d_alert_notification == 0x2A
          && state->tetra_call_type == 5 && state->tetra_enc_mode == 0
          && state->tetra_cmce_com_type == 1 && state->tetra_call_slots == 1;

    /* A replacement declares Basic Service but ends before its eight bits.
     * The last complete D-ALERT must remain untouched. */
    memset(cmce, 0, sizeof cmce); memset(tm_sdu, 0, sizeof tm_sdu); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_ALERT, off, 5); off += 5;
    pack_bits(cmce, 0x2222, off, 14); off += 14;
    pack_bits(cmce, 1, off, 3); off += 3;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 3, off, 2); off += 2;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    ok = ok && state->tetra_d_alert_call_id == 0x1234
            && state->tetra_d_alert_basic_service == 0xA5
            && state->tetra_call_type == 5 && state->tetra_cmce_com_type == 1;

    memset(cmce, 0, sizeof cmce); memset(tm_sdu, 0, sizeof tm_sdu); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_PROCEEDING, off, 5); off += 5;
    pack_bits(cmce, 0x3456, off, 14); off += 14;
    pack_bits(cmce, 6, off, 3); off += 3;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0x53, off, 8); off += 8;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 5, off, 3); off += 3;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 17, off, 6); off += 6;
    pack_bits(cmce, 0, off, 1); off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 3, opts, state);
    ok = ok && state->tetra_d_call_proc_valid
            && state->tetra_d_call_proc_basic_service == 0x53
            && state->tetra_d_call_proc_status == 5
            && state->tetra_d_call_proc_notification == 17
            && state->tetra_call_type == 2 && state->tetra_enc_mode == 1
            && state->tetra_cmce_com_type == 0 && state->tetra_call_slots == 3;

    free(state); free(opts);
    return ok;
}

static int test_call_restore_optionals_and_atomicity(void)
{
    uint8_t cmce[96] = {0}, tm_sdu[112] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(cmce, TETRA_CMCE_D_CALL_RESTORE, off, 5); off += 5;
    pack_bits(cmce, 0x1111, off, 14); off += 14;
    pack_bits(cmce, 3, off, 2); off += 2;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0x2222, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 9, off, 4); off += 4;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 5, off, 3); off += 3;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0x12A, off, 9); off += 9;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 0x2D, off, 6); off += 6;
    pack_bits(cmce, 0, off, 1); off++; /* M-bit */
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 6, opts, state);

    int ok = state->tetra_call_restore_valid
          && state->tetra_call_restore_id == 0x1111
          && state->tetra_call_id == 0x2222
          && state->tetra_call_restore_new_id_valid
          && state->tetra_call_restore_timeout_valid
          && state->tetra_call_restore_timeout == 9
          && state->tetra_call_restore_status_valid
          && state->tetra_call_restore_status == 5
          && state->tetra_call_restore_modify_valid
          && state->tetra_call_restore_modify == 0x12A
          && state->tetra_call_restore_notification_valid
          && state->tetra_call_restore_notification == 0x2D;

    /* New identifier is declared but incomplete: preserve the valid restore. */
    memset(cmce, 0, sizeof cmce); memset(tm_sdu, 0, sizeof tm_sdu); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_CALL_RESTORE, off, 5); off += 5;
    pack_bits(cmce, 0x3333, off, 14); off += 14;
    pack_bits(cmce, 0, off, 2); off += 2;
    pack_bits(cmce, 0, off, 1); off++;
    pack_bits(cmce, 0, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 7, off, 3); off += 3;
    pack_bits(tm_sdu, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(tm_sdu + 3, cmce, (size_t)off);
    tetra_mle_dispatch(tm_sdu, off + 3, 6, opts, state);
    ok = ok && state->tetra_call_restore_id == 0x1111
            && state->tetra_call_id == 0x2222
            && state->tetra_call_restore_modify == 0x12A;

    free(state); free(opts);
    return ok;
}

static int test_d_info_optionals_and_atomicity(void)
{
    uint8_t cmce[160] = {0}, tm_sdu[176] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    int off = 0;

    pack_bits(cmce, TETRA_CMCE_D_INFO, off, 5); off += 5;
    pack_bits(cmce, 0x1010, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++;
    pack_bits(cmce, 1, off, 1); off++; /* O-bit */
#define PUT_DINFO_OPT(value, width) do { pack_bits(cmce,1,off,1); off++; pack_bits(cmce,(value),off,(width)); off+=(width); } while(0)
    PUT_DINFO_OPT(0x2020,14);
    PUT_DINFO_OPT(11,4);
    PUT_DINFO_OPT(6,3);
    PUT_DINFO_OPT(1,1);
    PUT_DINFO_OPT(0x155,9);
    PUT_DINFO_OPT(3,3);
    PUT_DINFO_OPT(0xABCDEF,24);
    PUT_DINFO_OPT(42,6);
    PUT_DINFO_OPT(37,6);
    PUT_DINFO_OPT(12,6);
#undef PUT_DINFO_OPT
    pack_bits(cmce,0,off,1); off++; /* M-bit */
    pack_bits(tm_sdu,TETRA_MLE_PD_CMCE,0,3);
    memcpy(tm_sdu+3,cmce,(size_t)off);
    tetra_mle_dispatch(tm_sdu,off+3,5,opts,state);

    int ok=state->tetra_d_info_valid
        && state->tetra_d_info_new_call_id_valid && state->tetra_d_info_new_call_id==0x2020
        && state->tetra_call_id==0x2020 && state->tetra_call_timeout==11
        && state->tetra_d_info_setup_timeout==6 && state->tetra_d_info_ownership==1
        && state->tetra_d_info_modify==0x155 && state->tetra_d_info_status==3
        && state->tetra_d_info_temporary_address==0xABCDEF
        && state->tetra_d_info_notification_indicator==42
        && state->tetra_d_info_poll_percentage==37 && state->tetra_d_info_poll_number==12;

    /* Declared new call identifier is incomplete; retain every prior value. */
    memset(cmce,0,sizeof cmce); memset(tm_sdu,0,sizeof tm_sdu); off=0;
    pack_bits(cmce,TETRA_CMCE_D_INFO,off,5); off+=5;
    pack_bits(cmce,0x3030,off,14); off+=14;
    pack_bits(cmce,0,off,1); off++;
    pack_bits(cmce,0,off,1); off++;
    pack_bits(cmce,1,off,1); off++; /* O-bit */
    pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,3,off,3); off+=3;
    pack_bits(tm_sdu,TETRA_MLE_PD_CMCE,0,3);
    memcpy(tm_sdu+3,cmce,(size_t)off);
    tetra_mle_dispatch(tm_sdu,off+3,5,opts,state);
    ok=ok && state->tetra_d_info_call_id==0x1010
          && state->tetra_d_info_new_call_id==0x2020
          && state->tetra_d_info_temporary_address==0xABCDEF;

    free(state); free(opts);
    return ok;
}

static int test_connect_optionals_and_atomicity(void)
{
    uint8_t cmce[112]={0},tm_sdu[128]={0};
    dsd_state *state=alloc_state(); dsd_opts *opts=alloc_opts();
    int off=0;
    pack_bits(cmce,TETRA_CMCE_D_CONNECT,off,5);off+=5;
    pack_bits(cmce,0x1111,off,14);off+=14;pack_bits(cmce,8,off,4);off+=4;
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,3,off,2);off+=2;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
#define PUT_CONNECT_OPT(value,width) do{pack_bits(cmce,1,off,1);off++;pack_bits(cmce,(value),off,(width));off+=(width);}while(0)
    PUT_CONNECT_OPT(14,4);PUT_CONNECT_OPT(0xA6,8);PUT_CONNECT_OPT(0x654321,24);PUT_CONNECT_OPT(33,6);
#undef PUT_CONNECT_OPT
    pack_bits(cmce,0,off,1);off++; /* M-bit */
    pack_bits(tm_sdu,TETRA_MLE_PD_CMCE,0,3);memcpy(tm_sdu+3,cmce,(size_t)off);
    tetra_mle_dispatch(tm_sdu,off+3,4,opts,state);
    int ok=state->tetra_connect_valid && state->tetra_call_id==0x1111
        && state->tetra_cmce_connect_priority_valid && state->tetra_cmce_connect_priority==14
        && state->tetra_cmce_connect_basic_service==0xA6
        && state->tetra_connect_call_type==5 && state->tetra_connect_enc_mode==0
        && state->tetra_call_type==5 && state->tetra_enc_mode==0
        && state->tetra_cmce_com_type==1 && state->tetra_call_slots==2
        && state->tetra_cmce_connect_temporary_address==0x654321
        && state->tetra_cmce_connect_notification==33;

    /* Priority is declared but truncated: keep the established call intact. */
    memset(cmce,0,sizeof cmce);memset(tm_sdu,0,sizeof tm_sdu);off=0;
    pack_bits(cmce,TETRA_CMCE_D_CONNECT,off,5);off+=5;pack_bits(cmce,0x2222,off,14);off+=14;
    pack_bits(cmce,1,off,4);off+=4;pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,0,off,2);off+=2;pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;
    pack_bits(tm_sdu,TETRA_MLE_PD_CMCE,0,3);memcpy(tm_sdu+3,cmce,(size_t)off);
    tetra_mle_dispatch(tm_sdu,off+3,4,opts,state);
    ok=ok && state->tetra_call_id==0x1111 && state->tetra_cmce_connect_priority==14
        && state->tetra_call_type==5 && state->tetra_cmce_com_type==1
        && state->tetra_call_slots==2;

    memset(cmce,0,sizeof cmce);memset(tm_sdu,0,sizeof tm_sdu);off=0;
    pack_bits(cmce,TETRA_CMCE_D_CONNECT_ACK,off,5);off+=5;pack_bits(cmce,0x3333,off,14);off+=14;
    pack_bits(cmce,7,off,4);off+=4;pack_bits(cmce,3,off,2);off+=2;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,29,off,6);off+=6;
    pack_bits(cmce,0,off,1);off++; /* M-bit */
    pack_bits(tm_sdu,TETRA_MLE_PD_CMCE,0,3);memcpy(tm_sdu+3,cmce,(size_t)off);
    tetra_mle_dispatch(tm_sdu,off+3,4,opts,state);
    ok=ok && state->tetra_d_connect_ack_valid && state->tetra_d_connect_ack_call_id==0x3333
        && state->tetra_d_connect_ack_notification_valid && state->tetra_d_connect_ack_notification==29;
    free(state);free(opts);return ok;
}

static int test_release_notification_and_atomicity(void)
{
    uint8_t bits[48]={0};
    dsd_state *state=alloc_state(); dsd_opts *opts=alloc_opts();
    state->tetra_call_active=1;state->tetra_call_id=0x1234;state->tetra_tx_granted_valid=1;
    pack_bits(bits,TETRA_MLE_PD_CMCE,0,3);
    pack_bits(bits,TETRA_CMCE_D_RELEASE,3,5);
    pack_bits(bits,0x1234,8,14);pack_bits(bits,9,22,5);
    pack_bits(bits,1,27,1); /* O-bit */
    pack_bits(bits,1,28,1);pack_bits(bits,41,29,6);
    pack_bits(bits,0,35,1); /* M-bit */
    tetra_mle_dispatch(bits,36,2,opts,state);
    int ok=!state->tetra_call_active && !state->tetra_tx_granted_valid
        && state->tetra_cmce_release_notification_valid
        && state->tetra_cmce_release_notification==41
        && !state->tetra_cmce_release_was_disconnect;

    /* A matching D-DISCONNECT whose notification is declared but truncated
     * must not release the call or replace the last complete release fields. */
    memset(bits,0,sizeof bits);state->tetra_call_active=1;state->tetra_tx_granted_valid=1;
    pack_bits(bits,TETRA_MLE_PD_CMCE,0,3);
    pack_bits(bits,TETRA_CMCE_D_DISCONNECT,3,5);
    pack_bits(bits,0x1234,8,14);pack_bits(bits,3,22,5);
    pack_bits(bits,1,27,1); /* O-bit */
    pack_bits(bits,1,28,1);pack_bits(bits,2,29,2);
    tetra_mle_dispatch(bits,31,2,opts,state);
    ok=ok && state->tetra_call_active && state->tetra_tx_granted_valid
        && state->tetra_cmce_release_cause==9
        && state->tetra_cmce_release_notification==41
        && !state->tetra_cmce_release_was_disconnect;
    free(state);free(opts);return ok;
}

static int test_floor_control_scoped_to_active_call(void)
{
    uint8_t cmce[64]={0},mle[128]={0};
    dsd_state *state=alloc_state();dsd_opts *opts=alloc_opts();
    state->tetra_call_active=1;state->tetra_call_id=77;
    state->tetra_tx_granted_valid=1;state->tetra_tx_granted_ssi=0x112233;
    state->tetra_tx_event_call_id=77;
    int off=0;

    pack_bits(cmce,TETRA_CMCE_D_TX_GRANTED,off,5);off+=5;
    pack_bits(cmce,78,off,14);off+=14;pack_bits(cmce,1,off,2);off+=2;
    pack_bits(cmce,0,off,1);off++;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    int ok=state->tetra_tx_granted_valid && state->tetra_tx_granted_ssi==0x112233;

    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_CEASED,off,5);off+=5;
    pack_bits(cmce,78,off,14);off+=14;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && state->tetra_tx_granted_valid;

    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_WAIT,off,5);off+=5;
    pack_bits(cmce,78,off,14);off+=14;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && !state->tetra_tx_wait && state->tetra_tx_event_call_id==77;

    /* The same event for the active call is accepted. */
    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_WAIT,off,5);off+=5;
    pack_bits(cmce,77,off,14);off+=14;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && state->tetra_tx_wait && state->tetra_tx_event_call_id==77;
    free(state);free(opts);return ok;
}

static int test_floor_control_party_optionals_and_clear(void)
{
    uint8_t cmce[128]={0},mle[128]={0};
    dsd_state *state=alloc_state();dsd_opts *opts=alloc_opts();
    int off=0;

    /* Table 14.18: notification followed by TPTI=2, SSI and extension. */
    pack_bits(cmce,TETRA_CMCE_D_TX_GRANTED,off,5);off+=5;
    pack_bits(cmce,77,off,14);off+=14;pack_bits(cmce,3,off,2);off+=2;
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,23,off,6);off+=6;
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,2,off,2);off+=2;
    pack_bits(cmce,0x123456,off,24);off+=24;pack_bits(cmce,0x654321,off,24);off+=24;
    pack_bits(cmce,0,off,1);off++; /* terminating M-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    int ok=state->tetra_tx_granted_valid && state->tetra_tx_granted_ssi==0x123456
        && state->tetra_cmce_tx_granted_notification_valid
        && state->tetra_cmce_tx_granted_notification==23
        && state->tetra_cmce_tx_granted_party_type_valid
        && state->tetra_cmce_tx_granted_party_type==2
        && state->tetra_cmce_tx_granted_party_extension_valid
        && state->tetra_cmce_tx_granted_party_extension==0x654321;

    /* A complete mandatory-only replacement clears absent optional identity. */
    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_GRANTED,off,5);off+=5;
    pack_bits(cmce,77,off,14);off+=14;pack_bits(cmce,0,off,2);off+=2;
    pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && !state->tetra_tx_granted_ssi
        && !state->tetra_cmce_tx_granted_notification_valid
        && !state->tetra_cmce_tx_granted_party_type_valid
        && !state->tetra_cmce_tx_granted_party_extension_valid;

    /* Table 14.19 has the same transmitting-party optional chain. */
    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_INTERRUPT,off,5);off+=5;
    pack_bits(cmce,77,off,14);off+=14;pack_bits(cmce,1,off,2);off+=2;
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,31,off,6);off+=6;
    pack_bits(cmce,1,off,1);off++;pack_bits(cmce,2,off,2);off+=2;
    pack_bits(cmce,0xABCDEF,off,24);off+=24;pack_bits(cmce,0x102030,off,24);off+=24;
    pack_bits(cmce,0,off,1);off++; /* terminating M-bit */
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && state->tetra_tx_interrupted && state->tetra_tx_event_notification_valid
        && state->tetra_tx_event_notification==31
        && state->tetra_tx_event_party_type_valid && state->tetra_tx_event_party_type==2
        && state->tetra_tx_event_party_ssi_valid && state->tetra_tx_event_party_ssi==0xABCDEF
        && state->tetra_tx_event_party_extension_valid
        && state->tetra_tx_event_party_extension==0x102030;

    /* A claimed CPTI=2 extension that is truncated must preserve the event. */
    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_TX_INTERRUPT,off,5);off+=5;
    pack_bits(cmce,77,off,14);off+=14;pack_bits(cmce,2,off,2);off+=2;
    pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;pack_bits(cmce,0,off,1);off++;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,0,off,1);off++;pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,2,off,2);off+=2;pack_bits(cmce,0x111111,off,24);off+=24;
    pack_bits(cmce,0xAA,off,8);off+=8;
    wrap_cmce(mle,cmce,off);tetra_mle_dispatch(mle,off+3,1,opts,state);
    ok=ok && state->tetra_tx_event_notification==31
        && state->tetra_tx_event_party_ssi==0xABCDEF
        && state->tetra_tx_event_party_extension==0x102030;

    free(state);free(opts);return ok;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
static int test_sds_transfer_timestamp_text(void)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 72, 9, 11);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 0xb7, 36, 8);
    pack_bits(cmce, 0x81, 44, 8); /* Timestamp present, Latin-1. */
    pack_bits(cmce, 0x192345, 52, 24);
    pack_bits(cmce, 'O', 76, 8);
    pack_bits(cmce, 'K', 84, 8);
    pack_bits(cmce, 0, 92, 1); /* O-bit */
    wrap_cmce(mle, cmce, 93);
    tetra_mle_dispatch(mle, 96, 3, opts, state);
    int ok = strcmp(state->tetra_sds_text, "OK") == 0
        && state->tetra_sds_msg_ref == 0xb7;
    /* The outer length is complete; the timestamp itself lacks one bit. */
    pack_bits(cmce, 55, 9, 11);
    pack_bits(cmce, 0x11, 36, 8);
    pack_bits(cmce, 0, 75, 1); /* O-bit */
    wrap_cmce(mle, cmce, 76);
    tetra_mle_dispatch(mle, 79, 4, opts, state);
    ok = ok && strcmp(state->tetra_sds_text, "OK") == 0
        && state->tetra_sds_msg_ref == 0xb7 && state->tetra_sds_last_cc == 3;
    free(state); free(opts);
    return ok;
}

static int test_sds_gsm_text(void)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 40, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    /* Packed septets A, ESC, euro: 0x41,0x1b,0x65. */
    pack_bits(cmce, 0xc1, 36, 8);
    pack_bits(cmce, 0x4d, 44, 8);
    pack_bits(cmce, 0x19, 52, 8);
    pack_bits(cmce, 0, 60, 1); /* O-bit */
    wrap_cmce(mle, cmce, 61);
    tetra_mle_dispatch(mle, 64, 3, opts, state);
    int ok = state->tetra_sds_text_len == 4
        && memcmp(state->tetra_sds_text, "A\xe2\x82\xac", 5) == 0;
    /* One packed ESC has no following extension character. */
    pack_bits(cmce, 24, 9, 11);
    pack_bits(cmce, 27, 36, 8);
    pack_bits(cmce, 0, 44, 1); wrap_cmce(mle, cmce, 45);
    tetra_mle_dispatch(mle, 48, 4, opts, state);
    ok = ok && state->tetra_sds_last_cc == 3
        && memcmp(state->tetra_sds_text, "A\xe2\x82\xac", 5) == 0;
    /* TETRA carries packed octets; a seven-bit payload is incomplete. */
    pack_bits(cmce, 23, 9, 11);
    pack_bits(cmce, 0, 43, 1); wrap_cmce(mle, cmce, 44);
    tetra_mle_dispatch(mle, 47, 4, opts, state);
    ok = ok && state->tetra_sds_last_cc == 3;
    /* Undefined extension falls back to the default alphabet character. */
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 0x9b, 36, 8); /* ESC followed by A. */
    pack_bits(cmce, 0x20, 44, 8);
    pack_bits(cmce, 0, 52, 1); wrap_cmce(mle, cmce, 53);
    tetra_mle_dispatch(mle, 56, 5, opts, state);
    ok = ok && strcmp(state->tetra_sds_text, "A") == 0
        && state->tetra_sds_last_cc == 5;
    free(state); free(opts);
    return ok;
}

static int test_sds_utf16_display_bound(void)
{
    uint8_t mle[2048] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(mle, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(mle, TETRA_CMCE_D_SDS_DATA, 3, 5);
    pack_bits(mle, 3, 10, 2);
    pack_bits(mle, 16 + 90 * 16, 12, 11);
    pack_bits(mle, 2, 23, 8);
    pack_bits(mle, 26, 31, 8);
    for (int i = 0; i < 90; i++) pack_bits(mle, 0x4e2d, 39 + i * 16, 16);
    pack_bits(mle, 0, 39 + 90 * 16, 1); /* O-bit */
    tetra_mle_dispatch(mle, 40 + 90 * 16, 3, opts, state);
    int ok = state->tetra_sds_text_len == 255 && state->tetra_sds_text[255] == 0;
    for (int i = 0; i < 85; i++)
        ok = ok && memcmp(state->tetra_sds_text + i * 3, "\xe4\xb8\xad", 3) == 0;
    /* Invalid suffix lies beyond all displayed characters. */
    pack_bits(mle, 0xd800, 39 + 89 * 16, 16);
    tetra_mle_dispatch(mle, 40 + 90 * 16, 4, opts, state);
    ok = ok && state->tetra_sds_last_cc == 3 && state->tetra_sds_text_len == 255;
    free(state); free(opts);
    return ok;
}

static int test_sds_utf16_text(void)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 64, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    pack_bits(cmce, 26, 28, 8);
    pack_bits(cmce, 0x4e2d, 36, 16);
    pack_bits(cmce, 0xd83d, 52, 16);
    pack_bits(cmce, 0xde00, 68, 16);
    pack_bits(cmce, 0, 84, 1); wrap_cmce(mle, cmce, 85);
    tetra_mle_dispatch(mle, 88, 3, opts, state);
    int ok = state->tetra_sds_text_unicode && state->tetra_sds_text_len == 7
        && memcmp(state->tetra_sds_text, "\xe4\xb8\xad\xf0\x9f\x98\x80", 8) == 0;
    pack_bits(cmce, 0x0041, 68, 16); /* High surrogate without low surrogate. */
    wrap_cmce(mle, cmce, 85);
    tetra_mle_dispatch(mle, 88, 4, opts, state);
    ok = ok && state->tetra_sds_text_len == 7 && state->tetra_sds_last_cc == 3;
    free(state); free(opts);
    return ok;
}

static int test_sds_latin1_text(void)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 20, 8);
    pack_bits(cmce, 1, 28, 8);
    pack_bits(cmce, 'A', 36, 8);
    pack_bits(cmce, 0xe9, 44, 8);
    pack_bits(cmce, 0, 52, 1); wrap_cmce(mle, cmce, 53);
    tetra_mle_dispatch(mle, 56, 3, opts, state);
    int ok = state->tetra_sds_text_len == 3
        && memcmp(state->tetra_sds_text, "A\xc3\xa9", 4) == 0;
    pack_bits(cmce, 8, 9, 11); /* PID only: missing the coding-scheme octet. */
    pack_bits(cmce, 0, 28, 1); wrap_cmce(mle, cmce, 29);
    tetra_mle_dispatch(mle, 32, 4, opts, state);
    ok = ok && state->tetra_sds_text_len == 3 && state->tetra_sds_last_cc == 3;
    pack_bits(cmce, 31, 9, 11);
    pack_bits(cmce, 0, 51, 1); wrap_cmce(mle, cmce, 52);
    tetra_mle_dispatch(mle, 55, 4, opts, state);
    ok = ok && state->tetra_sds_text_len == 3 && state->tetra_sds_last_cc == 3;
    free(state); free(opts);
    return ok;
}

static int test_sds_transfer_reference(void)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 56, 9, 11);
    pack_bits(cmce, 131, 20, 8);
    pack_bits(cmce, 1, 35, 1);
    pack_bits(cmce, 0xa9, 36, 8);
    pack_bits(cmce, 17, 44, 5);
    pack_bits(cmce, 1, 49, 3); /* Forward SSI, 24 bits. */
    pack_bits(cmce, 0x123456, 52, 24);
    pack_bits(cmce, 0, 76, 1); wrap_cmce(mle, cmce, 77);
    tetra_mle_dispatch(mle, 80, 3, opts, state);
    int ok = state->tetra_sds_msg_ref == 0xa9
        && state->tetra_sds_storage_forward
        && state->tetra_sds_validity_period == 17
        && state->tetra_sds_forward_valid
        && state->tetra_sds_forward_type == 1
        && state->tetra_sds_forward_ssi == 0x123456;
    pack_bits(cmce, 0x11, 36, 8);
    pack_bits(cmce, 55, 9, 11);
    pack_bits(cmce, 0, 75, 1); wrap_cmce(mle, cmce, 76);
    tetra_mle_dispatch(mle, 79, 4, opts, state);
    ok = ok && state->tetra_sds_msg_ref == 0xa9 && state->tetra_sds_last_cc == 3
        && state->tetra_sds_forward_valid && state->tetra_sds_forward_ssi == 0x123456;
    free(state); free(opts);
    return ok;
}

static int test_sds_report_transport(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 1, 28, 4);
    pack_bits(cmce, 0x4a, 36, 8);
    pack_bits(cmce, 0xd3, 44, 8);
    int ok = 1;
    const int lengths[] = {48,64,88,64,0,0,0,40};
    pack_bits(cmce, 1, 35, 1);
    for (unsigned address = 0; address < 8; address++) {
        memset(cmce + 60, 0, sizeof(cmce) - 60);
        pack_bits(cmce, address, 57, 3);
        switch (address) {
        case 0: pack_bits(cmce, 0xa5, 60, 8); break;
        case 1: pack_bits(cmce, 0x123456, 60, 24); break;
        case 2:
            pack_bits(cmce, 0x234567, 60, 24);
            pack_bits(cmce, 0x345678, 84, 24);
            break;
        case 3:
            pack_bits(cmce, 3, 60, 8);
            pack_bits(cmce, 1, 68, 4);
            pack_bits(cmce, 10, 72, 4);
            pack_bits(cmce, 12, 76, 4);
            /* Bits 80..83 are the mandatory zero dummy digit. */
            break;
        default: break;
        }
        int length = lengths[address] ? lengths[address] : 40;
        pack_bits(cmce, length, 9, 11);
        state->tetra_sds_report_valid = 0;
        pack_bits(cmce,0,20+length,1); /* O-bit */
        wrap_cmce(mle, cmce, 21 + length);
        tetra_mle_dispatch(mle, 24 + length, 2, opts, state);
        ok = ok && state->tetra_sds_report_valid == (lengths[address] != 0);
        if (lengths[address]) {
            ok = ok && state->tetra_sds_report_cause == 0x4a
                && state->tetra_sds_report_msg_ref == 0xd3
                && !state->tetra_sds_report_delivery_ok
                && state->tetra_sds_storage_forward
                && state->tetra_sds_forward_valid
                && state->tetra_sds_forward_type == address;
            if (address == 0) ok = ok && state->tetra_sds_forward_sna == 0xa5;
            if (address == 1) ok = ok && state->tetra_sds_forward_ssi == 0x123456;
            if (address == 2) {
                ok = ok && state->tetra_sds_forward_ssi == 0x234567
                    && state->tetra_sds_forward_extension == 0x345678;
            }
            if (address == 3)
                ok = ok && strcmp(state->tetra_sds_forward_external, "1*+") == 0;
            state->tetra_sds_report_valid = 0;
            pack_bits(cmce, length - 1, 9, 11);
            pack_bits(cmce,0,19+length,1); /* O-bit */
            wrap_cmce(mle, cmce, 20 + length);
            tetra_mle_dispatch(mle, 23 + length, 2, opts, state);
            ok = ok && !state->tetra_sds_report_valid;
        }
    }
    /* Reserved external digits and a non-zero odd-digit pad reject atomically. */
    pack_bits(cmce, 3, 57, 3);
    pack_bits(cmce, 3, 60, 8);
    pack_bits(cmce, 13, 68, 4);
    pack_bits(cmce, 0, 72, 8);
    pack_bits(cmce, 64, 9, 11);
    pack_bits(cmce,0,84,1); wrap_cmce(mle, cmce, 85);
    tetra_mle_dispatch(mle, 88, 6, opts, state);
    ok = ok && state->tetra_sds_forward_type == 7 && state->tetra_sds_last_cc == 2;
    pack_bits(cmce, 1, 68, 4);
    pack_bits(cmce, 2, 72, 4);
    pack_bits(cmce, 3, 76, 4);
    pack_bits(cmce, 1, 80, 4);
    wrap_cmce(mle,cmce,85);
    tetra_mle_dispatch(mle, 88, 6, opts, state);
    ok = ok && state->tetra_sds_forward_type == 7 && state->tetra_sds_last_cc == 2;
    pack_bits(cmce, 0, 35, 1);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 36, 8);
    pack_bits(cmce,0,52,1); wrap_cmce(mle, cmce, 53);
    tetra_mle_dispatch(mle, 56, 2, opts, state);
    ok = ok && state->tetra_sds_report_valid && state->tetra_sds_report_delivery_ok
        && !state->tetra_sds_storage_forward && !state->tetra_sds_forward_valid;
    free(state); free(opts);
    return ok;
}

static int test_sds_ack_transport(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t cmce[128] = {0}, mle[128] = {0};
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, 0, 5);
    pack_bits(cmce, 3, 7, 2);
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 130, 20, 8);
    pack_bits(cmce, 2, 28, 4);
    pack_bits(cmce, 3, 36, 8);
    pack_bits(cmce, 0xe7, 44, 8);
    pack_bits(cmce,0,52,1); wrap_cmce(mle, cmce, 53);
    tetra_mle_dispatch(mle, 56, 2, opts, state);
    int ok = state->tetra_sds_ack_valid && state->tetra_sds_ack_msg_ref == 0xe7
        && state->tetra_sds_ack_delivery_status == 3;
    /* Complete CMCE payload, but truncated inner transport header. */
    pack_bits(cmce, 31, 9, 11);
    pack_bits(cmce, 0x22, 44, 8);
    pack_bits(cmce,0,51,1); wrap_cmce(mle, cmce, 52);
    tetra_mle_dispatch(mle, 55, 7, opts, state);
    ok = ok && state->tetra_sds_ack_msg_ref == 0xe7 && state->tetra_sds_last_cc == 2;
    pack_bits(cmce, 32, 9, 11);
    pack_bits(cmce, 2, 20, 8); /* Simple text PID: no transport ACK. */
    state->tetra_sds_ack_valid = 0;
    pack_bits(cmce,0,52,1); wrap_cmce(mle, cmce, 53);
    tetra_mle_dispatch(mle, 56, 2, opts, state);
    ok = ok && !state->tetra_sds_ack_valid;
    free(state); free(opts);
    return ok;
}

static int test_sds_published_trace(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t bits[88] = {0};

    /* Published tetra-multiframe-sds example trace:
     * https://github.com/itds-consulting/tetra-multiframe-sds#example-output
     *
     * Its independently decoded 64-bit D-SDS-DATA user field is
     * 82:00:20:01:41:68:6F:6A: PID 130, SDS-TRANSFER, message reference 32,
     * no timestamp, Latin-1 coding, text "Ahoj".  The frozen 88-bit value
     * below wraps that field in a minimal CMCE/MLE envelope (CPTI=0, SDTI=3,
     * 64-bit length, no optional tail) without using pack_bits().
     */
    unpack_hex_bits("4F30810400400282D0DED4", bits, 88);
    tetra_mle_dispatch(bits, 88, 4, opts, state);

    int ok = state->tetra_cmce_sds_data_type == 3
        && state->tetra_sds_src == 0
        && state->tetra_sds_msg_ref == 32
        && state->tetra_sds_text_len == 4
        && !state->tetra_sds_text_unicode
        && strcmp(state->tetra_sds_text, "Ahoj") == 0
        && state->tetra_sds_last_cc == 4;
    free(state);
    free(opts);
    return ok;
}

static int test_sds_short_report_status(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t cmce[48] = {0}, mle[128] = {0};
    int ok = 1;
    pack_bits(cmce, TETRA_CMCE_D_STATUS, 0, 5);
    pack_bits(cmce, 1, 5, 2);
    pack_bits(cmce, 0x123456, 7, 24);
    pack_bits(cmce, 0, 47, 1); /* O-bit */
    for (unsigned result = 0; result < 4; result++) {
        pack_bits(cmce, 0x7c00u | (result << 8) | 0xe5u, 31, 16);
        wrap_cmce(mle, cmce, 48);
        tetra_mle_dispatch(mle, 51, 1, opts, state);
        ok = ok && state->tetra_sds_short_report_valid
            && state->tetra_sds_short_report_result == result
            && state->tetra_sds_short_report_msg_ref == 0xe5
            && state->tetra_sds_src == 0x123456;
    }
    /* A truncated replacement must preserve the last complete report. */
    pack_bits(cmce, 0x7c11, 31, 16);
    wrap_cmce(mle, cmce, 48);
    tetra_mle_dispatch(mle, 50, 1, opts, state);
    ok = ok && state->tetra_sds_short_report_result == 3
        && state->tetra_sds_short_report_msg_ref == 0xe5;
    /* Adjacent prefixes are ordinary status, not short reports. */
    for (unsigned status = 0x7800; status <= 0x8000; status += 0x800) {
        state->tetra_sds_short_report_valid = 0;
        pack_bits(cmce, status, 31, 16);
        wrap_cmce(mle, cmce, 48);
        tetra_mle_dispatch(mle, 51, 1, opts, state);
        ok = ok && !state->tetra_sds_short_report_valid;
    }
    free(state);
    free(opts);
    return ok;
}

static void dispatch_concat_fragment(dsd_state *state, dsd_opts *opts,
                                     uint32_t src_ssi, int with_transport,
                                     uint16_t reference, uint8_t total,
                                     uint8_t sequence, int payload_pid,
                                     const uint8_t *data, int data_bits,
                                     uint8_t message_ref)
{
    uint8_t cmce[128] = {0}, mle[128] = {0};
    int off = 0;
    pack_bits(cmce, TETRA_CMCE_D_SDS_DATA, off, 5); off += 5;
    pack_bits(cmce, 1, off, 2); off += 2;
    pack_bits(cmce, src_ssi, off, 24); off += 24;
    pack_bits(cmce, 3, off, 2); off += 2;
    int length_offset = off; off += 11;
    int payload_start = off;
    pack_bits(cmce, with_transport ? 140 : 12, off, 8); off += 8;
    if (with_transport) {
        pack_bits(cmce, 0, off, 4); off += 4; /* SDS-TRANSFER. */
        pack_bits(cmce, 0, off, 4); off += 4; /* Report request + no store/forward. */
        pack_bits(cmce, message_ref, off, 8); off += 8;
    }
    pack_bits(cmce, 0, off, 3); off += 3; /* Concatenation transfer. */
    pack_bits(cmce, reference > 15, off, 1); off++;
    if (reference > 15) {
        pack_bits(cmce, reference >> 4, off, 8); off += 8;
    }
    pack_bits(cmce, reference & 15u, off, 4); off += 4;
    pack_bits(cmce, total, off, 8); off += 8;
    pack_bits(cmce, sequence, off, 8); off += 8;
    if (sequence == 1) {
        pack_bits(cmce, (uint32_t)payload_pid, off, 8); off += 8;
    }
    memcpy(cmce + off, data, (size_t)data_bits); off += data_bits;
    pack_bits(cmce, (uint32_t)(off - payload_start), length_offset, 11);
    pack_bits(cmce,0,off,1); off++; /* O-bit */
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 3, 6, opts, state);
}

static int test_sds_concat_out_of_order_and_duplicates(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t first[24] = {0}, second[16] = {0}, third[8] = {0};
    tetra_sds_concat_reset();
    pack_bits(first, 1, 0, 8); /* No timestamp, Latin-1. */
    pack_bits(first, 'H', 8, 8); pack_bits(first, 'E', 16, 8);
    pack_bits(second, 'L', 0, 8); pack_bits(second, 'L', 8, 8);
    pack_bits(third, 'O', 0, 8);
    strcpy(state->tetra_sds_text, "OLD");
    state->tetra_sds_text_len = 3;

    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 2, -1,
                             second, 16, 0);
    int ok = state->tetra_sds_concat_valid
        && state->tetra_sds_concat_ref == 0xabc
        && state->tetra_sds_concat_received == 1
        && !state->tetra_sds_concat_complete
        && strcmp(state->tetra_sds_text, "OLD") == 0;
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 2, -1,
                             second, 16, 0);
    ok = ok && state->tetra_sds_concat_duplicate
        && state->tetra_sds_concat_received == 1;
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 3, -1,
                             third, 8, 0);
    dispatch_concat_fragment(state, opts, 0x123456, 0, 0xabc, 3, 1, 2,
                             first, 24, 0);
    ok = ok && state->tetra_sds_concat_complete
        && state->tetra_sds_concat_received == 3
        && state->tetra_sds_concat_generation == 1
        && state->tetra_sds_src == 0x123456
        && strcmp(state->tetra_sds_text, "HELLO") == 0;
    free(state); free(opts);
    return ok;
}

static int test_sds_concat_transport_and_conflict(void)
{
    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    uint8_t first[16] = {0}, second[8] = {0}, conflict[8] = {0};
    tetra_sds_concat_reset();
    pack_bits(first, 1, 0, 8); pack_bits(first, 'O', 8, 8);
    pack_bits(second, 'K', 0, 8); pack_bits(conflict, 'X', 0, 8);

    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, second, 8, 0x44);
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, conflict, 8, 0x44);
    int ok = state->tetra_sds_concat_received == 1
        && !state->tetra_sds_concat_complete;
    /* The conflicting duplicate discarded the entire old context. */
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 1, 130, first, 16, 0x44);
    ok = ok && state->tetra_sds_concat_received == 1;
    dispatch_concat_fragment(state, opts, 77, 1, 9, 2, 2, -1, second, 8, 0x44);
    ok = ok && state->tetra_sds_concat_complete
        && state->tetra_sds_concat_generation == 1
        && state->tetra_sds_msg_ref == 0x44
        && strcmp(state->tetra_sds_text, "OK") == 0;

    uint8_t old_received = state->tetra_sds_concat_received;
    dispatch_concat_fragment(state, opts, 88, 0, 3, 1, 1, 2, first, 16, 0);
    ok = ok && state->tetra_sds_concat_received == old_received;
    dispatch_concat_fragment(state, opts, 88, 0, 3, 2, 3, -1, second, 8, 0);
    ok = ok && state->tetra_sds_concat_received == old_received;
    free(state); free(opts);
    tetra_sds_concat_reset();
    return ok;
}

static int test_sds_annex_e_type34_tail(void)
{
    uint8_t cmce[64]={0},mle[128]={0};
    dsd_state *state=alloc_state();dsd_opts *opts=alloc_opts();
    int off=0;

    /* CPTI=0, SDTI=0, followed by one unknown length-delimited Type 3 IE. */
    pack_bits(cmce,TETRA_CMCE_D_SDS_DATA,off,5);off+=5;
    pack_bits(cmce,0,off,2);off+=2;
    pack_bits(cmce,0,off,2);off+=2;
    pack_bits(cmce,0xBEEF,off,16);off+=16;
    pack_bits(cmce,1,off,1);off++; /* O-bit */
    pack_bits(cmce,1,off,1);off++; /* M-bit */
    pack_bits(cmce,4,off,4);off+=4;
    pack_bits(cmce,3,off,11);off+=11;
    pack_bits(cmce,5,off,3);off+=3;
    pack_bits(cmce,0,off,1);off++; /* terminating M-bit */
    wrap_cmce(mle,cmce,off);
    tetra_mle_dispatch(mle,off+3,2,opts,state);
    int ok=state->tetra_sds_short_valid && state->tetra_sds_short_data==0xBEEF;

    /* An incomplete Type 3 value must not publish the replacement payload. */
    memset(cmce,0,sizeof cmce);off=0;
    pack_bits(cmce,TETRA_CMCE_D_SDS_DATA,off,5);off+=5;
    pack_bits(cmce,0,off,2);off+=2;
    pack_bits(cmce,0,off,2);off+=2;
    pack_bits(cmce,0x1234,off,16);off+=16;
    pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,1,off,1);off++;
    pack_bits(cmce,4,off,4);off+=4;
    pack_bits(cmce,4,off,11);off+=11;
    pack_bits(cmce,2,off,2);off+=2;
    wrap_cmce(mle,cmce,off);
    tetra_mle_dispatch(mle,off+3,3,opts,state);
    ok=ok && state->tetra_sds_short_data==0xBEEF && state->tetra_sds_last_cc==2;

    free(state);free(opts);return ok;
}

static int test_function_not_supported_extract_and_o_bit(void)
{
    uint8_t cmce[64]={0},mle[128]={0};
    dsd_state *state=alloc_state();dsd_opts *opts=alloc_opts();
    int off=0;
    pack_bits(cmce,TETRA_CMCE_FUNCTION_NOT_SUPPORTED,off,5);off+=5;
    pack_bits(cmce,TETRA_CMCE_D_INFO,off,5);off+=5;
    pack_bits(cmce,0,off,1);off++; /* no call identifier */
    pack_bits(cmce,3,off,8);off+=8;
    pack_bits(cmce,5,off,8);off+=8;
    pack_bits(cmce,0x15,off,5);off+=5;
    pack_bits(cmce,0,off,1);off++; /* O-bit */
    wrap_cmce(mle,cmce,off);
    tetra_mle_dispatch(mle,off+3,4,opts,state);
    int ok=state->tetra_cmce_fns_valid
        && state->tetra_cmce_fns_rejected_pdu==TETRA_CMCE_D_INFO
        && state->tetra_cmce_fns_pointer==3
        && state->tetra_cmce_fns_extract_bits==5;

    /* The same body without its mandatory O-bit is not a complete PDU. */
    state->tetra_cmce_fns_valid=0;
    tetra_mle_dispatch(mle,off+2,5,opts,state);
    ok=ok && !state->tetra_cmce_fns_valid
        && state->tetra_cmce_fns_pointer==3
        && state->tetra_cmce_fns_extract_bits==5;
    free(state);free(opts);return ok;
}

int main(void)
{
    int passed = 0, total = 0;

#define RUN(fn)                                         \
    do {                                                \
        total++;                                        \
        if (fn()) {                                     \
            fprintf(stderr, "PASS: " #fn "\n");         \
            passed++;                                   \
        } else {                                        \
            fprintf(stderr, "FAIL: " #fn "\n");         \
        }                                               \
    } while (0)

    RUN(test_standard_setup_release_status);
    RUN(test_sds_short_report_status);
    RUN(test_sds_published_trace);
    RUN(test_sds_ack_transport);
    RUN(test_sds_report_transport);
    RUN(test_sds_transfer_reference);
    RUN(test_sds_latin1_text);
    RUN(test_sds_utf16_text);
    RUN(test_sds_utf16_display_bound);
    RUN(test_sds_gsm_text);
    RUN(test_sds_transfer_timestamp_text);
    RUN(test_sds_concat_out_of_order_and_duplicates);
    RUN(test_sds_concat_transport_and_conflict);
    RUN(test_sds_annex_e_type34_tail);
    RUN(test_function_not_supported_extract_and_o_bit);
    RUN(test_standard_mle_envelopes);
    RUN(test_annex_e19_e24_mle_responses);
    RUN(test_alert_proceeding_optional_atomicity);
    RUN(test_call_restore_optionals_and_atomicity);
    RUN(test_d_info_optionals_and_atomicity);
    RUN(test_connect_optionals_and_atomicity);
    RUN(test_release_notification_and_atomicity);
    RUN(test_floor_control_scoped_to_active_call);
    RUN(test_floor_control_party_optionals_and_clear);
    RUN(test_d_tx_granted);
    RUN(test_d_tx_ceased);
    RUN(test_d_setup_truncated_optional_rejected);
    RUN(test_floor_control_truncated_optionals_rejected);
    RUN(test_release_is_scoped_to_active_call);
#undef RUN

    fprintf(stderr, "\n%d / %d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
