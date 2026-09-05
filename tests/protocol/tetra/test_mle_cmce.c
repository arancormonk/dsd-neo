// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MLE dispatch + CMCE D-SETUP / D-RELEASE call-state tests.
 *
 * Verifies that tetra_mle_dispatch() correctly updates dsd_state when it
 * processes MLE C-PLANE-DATA PDUs carrying CMCE D-SETUP, D-RELEASE, and
 * D-CONNECT messages.
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
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

static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

/* -------------------------------------------------------------------------
 * Test 1: MLE C-PLANE-DATA + CMCE D-SETUP with calling party SSI.
 *
 * TM-SDU bit layout (49 bits total):
 *   [0-4]   MLE type   = 24 (11000b, TETRA_MLE_C_PLANE_DATA)
 *   [5-8]   PD         =  3 (0011b,  TETRA_MLE_PD_CMCE)
 *   [9-13]  CMCE type  =  6 (00110b, TETRA_CMCE_D_SETUP)
 *   [14]    call_id    = 0
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
 * Test 2: MLE C-PLANE-DATA + CMCE D-RELEASE clears call_active.
 *
 *   [0-4]   MLE type   = 24
 *   [5-8]   PD         =  3 (CMCE)
 *   [9-13]  CMCE type  =  5 (00101b, D-RELEASE)
 *   [14]    cause_type = 0 (standard)
 *   [15-18] cause      = 5 (0101b)
 * Expected: tetra_call_active goes from 1 → 0
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 3: MLE C-PLANE-DATA + CMCE D-CONNECT sets call_active.
 *
 *   [0-4]   MLE type   = 24
 *   [5-8]   PD         =  3 (CMCE)
 *   [9-13]  CMCE type  =  2 (00010b, D-CONNECT)
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
 * TM-SDU layout (61 bits), using Table 14.18:
 *   [0-4]   MLE type   = 24  (C_PLANE_DATA)
 *   [5-8]   PD         =  3  (CMCE)
 *   [9-13]  CMCE type  = 10  (D_TX_GRANTED, 01010b)
 *   [14-27] call identifier
 *   [28-29] transmission grant = 3 (granted to another user)
 *   [30] request permission, [31] encryption, [32] reserved
 *   [33] notification absent, [34] TPTI present
 *   [35-36] TPTI = 1, [37-60] transmitting SSI
 * Expected: tetra_tx_granted_valid=1, tetra_tx_granted_ssi=TEST_GRANTED_SSI
 * ------------------------------------------------------------------------- */
static int test_d_tx_granted(void)
{
    const uint32_t TEST_GRANTED_SSI = 0x4C1A77u;  /* 5053047 */
    const int NBITS = 61;

    uint8_t bits[61];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 24u,              0, 5); /* MLE C_PLANE_DATA              */
    pack_bits(bits,  3u,              5, 4); /* PD = CMCE                     */
    pack_bits(bits, 11u,              9, 5); /* CMCE type = D_TX_GRANTED      */
    pack_bits(bits,  7u,             14, 14); /* call identifier              */
    pack_bits(bits,  3u,             28, 2);  /* granted to another user      */
    pack_bits(bits,  1u,             30, 1);  /* request permission           */
    pack_bits(bits,  0u,             31, 1);  /* encryption control           */
    pack_bits(bits,  0u,             32, 1);  /* reserved                     */
    pack_bits(bits,  0u,             33, 1);  /* notification absent          */
    pack_bits(bits,  1u,             34, 1);  /* TPTI present                 */
    pack_bits(bits,  1u,             35, 2);  /* TPTI = SSI                   */
    pack_bits(bits, TEST_GRANTED_SSI, 37, 24); /* transmitting SSI            */

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
 *   [0-4]   MLE type  = 24
 *   [5-8]   PD        =  3 (CMCE)
 *   [9-13]  CMCE type =  8 (D_TX_CEASED, 01000b)
 * Pre-condition: tetra_tx_granted_valid=1
 * Expected: tetra_tx_granted_valid=0
 * ------------------------------------------------------------------------- */
static int test_d_tx_ceased(void)
{
    const int NBITS = 29;

    uint8_t bits[29];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 24u, 0, 5); /* MLE C_PLANE_DATA                         */
    pack_bits(bits,  3u, 5, 4); /* PD = CMCE                                */
    pack_bits(bits,  9u, 9, 5); /* CMCE D_TX_CEASED                         */
    pack_bits(bits, 42u, 14, 14); /* call identifier                         */
    pack_bits(bits,  1u, 28, 1);  /* transmission request permission         */

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
    pack_bits(cmce, 1, off, 1); off += 1; /* notification claimed, payload absent */

    pack_bits(mle, TETRA_MLE_C_PLANE_DATA, 0, 5);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 5, 4);
    memcpy(mle + 9, cmce, (size_t)off);
    state->tetra_call_id = 0x55;
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);

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
    pack_bits(mle, TETRA_MLE_C_PLANE_DATA, 0, 5);
    pack_bits(mle, TETRA_MLE_PD_CMCE, 5, 4);
    memcpy(mle + 9, cmce, (size_t)cmce_nbits);
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
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_granted_valid = 0;
    state->tetra_tx_granted_ssi = 0x112233u;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);
    if (state->tetra_tx_granted_valid != 0 || state->tetra_tx_granted_ssi != 0x112233u)
        ok = 0;

    /* D-TX-CEASED: malformed notification must not clear an active grant. */
    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_CEASED, off, 5); off += 5;
    pack_bits(cmce, 8, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_granted_valid = 1;
    state->tetra_tx_ceased_tx_perm = 0;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);
    if (state->tetra_tx_granted_valid != 1 || state->tetra_tx_ceased_tx_perm != 0)
        ok = 0;

    /* D-TX-WAIT: malformed notification must not publish an event. */
    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_TX_WAIT, off, 5); off += 5;
    pack_bits(cmce, 9, off, 14); off += 14;
    pack_bits(cmce, 1, off, 1); off += 1;
    pack_bits(cmce, 1, off, 1); off += 1; /* notification present, value absent */
    state->tetra_tx_wait = 0;
    state->tetra_tx_event_call_id = 0x55;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);
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

    pack_bits(cmce, TETRA_CMCE_D_RELEASE, off, 5); off += 5;
    pack_bits(cmce, 0x1235u, off, 14); off += 14;
    pack_bits(cmce, 19, off, 5); off += 5;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);
    if (!state->tetra_call_active || state->tetra_call_id != 0x1234u ||
        state->tetra_cmce_release_cause != 3 || !state->tetra_tx_granted_valid)
        ok = 0;

    memset(cmce, 0, sizeof cmce); off = 0;
    pack_bits(cmce, TETRA_CMCE_D_DISCONNECT, off, 5); off += 5;
    pack_bits(cmce, 0x1234u, off, 14); off += 14;
    pack_bits(cmce, 7, off, 5); off += 5;
    wrap_cmce(mle, cmce, off);
    tetra_mle_dispatch(mle, off + 9, 0, opts, state);
    if (state->tetra_call_active || state->tetra_call_id != 0x1234u ||
        state->tetra_cmce_release_cause != 7 || state->tetra_tx_granted_valid)
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
 * MLE bit layout (76 bits):
 *   [0-4]   MLE type   = 24  (C_PLANE_DATA)
 *   [5-8]   PD         =  3  (CMCE)
 *   [9-13]  CMCE type  =  6  (D-SETUP)
 *   [14]    call_id    =  1  (→ tetra_call_id should be 1)
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
 * MLE bit layout (51 bits):
 *   [0-4]   MLE type   = 24
 *   [5-8]   PD         =  3  (CMCE)
 *   [9-13]  CMCE type  =  6  (D-SETUP)
 *   [14]    call_id    =  0
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
 *   [0-4]  MLE type  = 24  (C_PLANE_DATA)
 *   [5-8]  PD        =  3  (CMCE)
 *   [9-13] CMCE type =  0  (D-ALERT)
 * Expected: tetra_call_active=1
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 10: D-CALL-PROCEEDING sets tetra_call_active.
 *
 *   [0-4]  MLE type  = 24
 *   [5-8]  PD        =  3  (CMCE)
 *   [9-13] CMCE type =  1  (D-CALL-PROCEEDING)
 * Expected: tetra_call_active=1
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * Test 11: D-STATUS without calling party — pre-coded status stored.
 *
 * CMCE PDU bit layout (relative to CMCE start, CMCE PDU passed at MLE bit 9):
 *   CMCE[0-4]  = PDU type = 7  (D-STATUS)
 *   CMCE[5-20] = pre_coded_status = TEST_STATUS (16 bits)
 * No calling party IE (buffer ends at CMCE bit 21, i.e. MLE bit 30).
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
    uint8_t cmce[96]={0}, mle[128]; int n=0; int ok=1;
    dsd_state *state=alloc_state(); dsd_opts *opts=alloc_opts();
    /* Table 14.15 mandatory D-SETUP plus absent notification/temp and CPTI=1. */
    int off=0;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5); off+=5;
    pack_bits(cmce,0x1234,off,14); off+=14; pack_bits(cmce,9,off,4); off+=4;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,0x32,off,8); off+=8; /* circuit=1, enc=1, com=0, slots=2 */
    pack_bits(cmce,3,off,2); off+=2; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,7,off,4); off+=4;
    pack_bits(cmce,0,off,1); off++; pack_bits(cmce,0,off,1); off++; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,1,off,2); off+=2; pack_bits(cmce,0x654321,off,24); off+=24;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off); n=off+9;
    tetra_mle_dispatch(mle,n,2,opts,state);
    if (!state->tetra_call_active || state->tetra_call_id!=0x1234 || state->tetra_call_timeout!=9 ||
        state->tetra_calling_ssi!=0x654321 || state->tetra_cmce_setup_basic_service!=0x32 ||
        state->tetra_cmce_setup_priority!=7) ok=0;

    /* Table 14.14 D-STATUS with CPTI=1. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_STATUS,off,5); off+=5;
    pack_bits(cmce,1,off,2); off+=2; pack_bits(cmce,0x654321,off,24); off+=24; pack_bits(cmce,0xBEEF,off,16); off+=16;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if (state->tetra_sds_src!=0x654321 || state->tetra_sds_status!=0xBEEF) ok=0;

    /* Table 14.12 D-RELEASE. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_RELEASE,off,5); off+=5;
    pack_bits(cmce,0x1234,off,14); off+=14; pack_bits(cmce,19,off,5); off+=5;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if (state->tetra_call_active || state->tetra_call_id!=0x1234 || state->tetra_cmce_release_cause!=19) ok=0;
    /* Tables 14.4, 14.5, 14.7, 14.8 and 14.11 mandatory fields. */
    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_ALERT,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,6,off,3); off+=3;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(state->tetra_d_alert_call_id!=0x2345 || state->tetra_d_alert_timeout!=6 || !state->tetra_d_alert_queued) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CALL_PROCEEDING,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,5,off,3); off+=3; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,0,off,1); off++;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(state->tetra_d_call_proc_call_id!=0x2345 || state->tetra_d_call_proc_timeout!=5 || !state->tetra_d_call_proc_hook) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CONNECT,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,8,off,4); off+=4; pack_bits(cmce,1,off,1); off++;
    pack_bits(cmce,1,off,1); off++; pack_bits(cmce,3,off,2); off+=2; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(!state->tetra_connect_valid || state->tetra_call_id!=0x2345 || state->tetra_cmce_connect_tx_grant!=3 || !state->tetra_cmce_connect_ownership) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_CONNECT_ACK,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,4,off,4); off+=4; pack_bits(cmce,2,off,2); off+=2; pack_bits(cmce,1,off,1); off++;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(state->tetra_d_connect_ack_call_id!=0x2345 || state->tetra_d_connect_ack_tx_grant!=2 || !state->tetra_d_connect_ack_tx_permission) ok=0;

    memset(cmce,0,sizeof(cmce)); off=0; pack_bits(cmce,TETRA_CMCE_D_INFO,off,5); off+=5;
    pack_bits(cmce,0x2345,off,14); off+=14; pack_bits(cmce,1,off,1); off++; pack_bits(cmce,1,off,1); off++;
    memset(mle,0,sizeof(mle)); pack_bits(mle,24,0,5); pack_bits(mle,3,5,4); memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(state->tetra_d_info_call_id!=0x2345 || !state->tetra_d_info_call_timeout || !state->tetra_d_info_notification) ok=0;

    /* Table 14.6 D-CALL-RESTORE. */
    memset(cmce,0,sizeof(cmce));off=0;pack_bits(cmce,TETRA_CMCE_D_CALL_RESTORE,off,5);off+=5;
    pack_bits(cmce,0x3456,off,14);off+=14;pack_bits(cmce,3,off,2);off+=2;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,1,off,1);off++;
    memset(mle,0,sizeof(mle));pack_bits(mle,24,0,5);pack_bits(mle,3,5,4);memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(!state->tetra_call_restore_valid || state->tetra_call_restore_id!=0x3456 || state->tetra_call_restore_grant!=3 || !state->tetra_call_restore_reset)ok=0;

    /* Table 14.33 FUNCTION NOT SUPPORTED with call identifier and pointer=0. */
    memset(cmce,0,sizeof(cmce));off=0;pack_bits(cmce,TETRA_CMCE_FUNCTION_NOT_SUPPORTED,off,5);off+=5;
    pack_bits(cmce,TETRA_CMCE_D_SETUP,off,5);off+=5;pack_bits(cmce,1,off,1);off++;pack_bits(cmce,0x3456,off,14);off+=14;pack_bits(cmce,0,off,8);off+=8;
    memset(mle,0,sizeof(mle));pack_bits(mle,24,0,5);pack_bits(mle,3,5,4);memcpy(mle+9,cmce,(size_t)off);
    tetra_mle_dispatch(mle,off+9,2,opts,state);
    if(!state->tetra_cmce_fns_valid || state->tetra_cmce_fns_rejected_pdu!=TETRA_CMCE_D_SETUP || state->tetra_cmce_fns_call_id!=0x3456 || state->tetra_cmce_fns_pointer!=0)ok=0;

    free(state); free(opts); return ok;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
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
    RUN(test_d_tx_granted);
    RUN(test_d_tx_ceased);
    RUN(test_d_setup_truncated_optional_rejected);
    RUN(test_floor_control_truncated_optionals_rejected);
    RUN(test_release_is_scoped_to_active_call);
#undef RUN

    fprintf(stderr, "\n%d / %d passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
