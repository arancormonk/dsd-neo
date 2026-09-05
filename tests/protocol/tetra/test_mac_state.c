// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MAC PDU parser – dsd_state integration tests.
 *
 * Verifies that tetra_mac_parse_schd() correctly updates dsd_state when it
 * decodes a MAC-BROADCAST/SYSINFO PDU or a MAC-RESOURCE PDU.
 */

#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
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

/* Heap-allocate a zeroed dsd_state.  Caller must free(). */
static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }

/* Heap-allocate a zeroed dsd_opts.  Caller must free(). */
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

static int tune_calls;
static long tuned_freq;
static dsd_trunk_tune_result tune_stub(dsd_opts *opts, dsd_state *state,
                                       long freq, int ted_sps, uint64_t request_id)
{
    (void)opts; (void)state; (void)ted_sps; (void)request_id;
    tune_calls++;
    tuned_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* -------------------------------------------------------------------------
 * Test 1: MAC-BROADCAST/SYSINFO → tetra_sysinfo_known, tetra_la,
 *          tetra_subscr_class, tetra_bs_service_det
 *
 * PDU layout (ETSI EN 300 392-2 §21.5.9 + §21.6.1):
 *   [0-1]   PDU type       = 2 (BROADCAST)       2 bits
 *   [2-3]   Broadcast type = 0 (SYSINFO)          2 bits
 *   [4-15]  Main carrier                         12 bits
 *   [16-19] Freq band                             4 bits
 *   [20-21] Freq offset                           2 bits
 *   [22-24] Duplex spacing                        3 bits
 *   [25]    Reverse op                            1 bit
 *   [26-27] Num SCH                               2 bits
 *   [28-30] MS TX power max                       3 bits
 *   [31-34] RXLEV min                             4 bits
 *   [35-38] Access parameter                      4 bits
 *   [39-42] Radio DL timeout                      4 bits
 *   [43]    CCK valid                             1 bit
 *   [44-59] CCK-ID / HF number                  16 bits
 *   [60-61] Option field type                     2 bits
 *   [62-81] Option field data                    20 bits
 *  MLE:
 *   [82-95] LA (14 bits) = 987
 *   [96-111] Subscr class (16 bits) = 0xBEEF
 *   [112-123] BS service details (12 bits) = 0xA5C
 * Total = 124 bits
 * ------------------------------------------------------------------------- */
static int test_sysinfo(void)
{
    const int NBITS = 124;
    uint8_t bits[124];
    memset(bits, 0, sizeof(bits));

    const uint32_t TEST_LA           = 987u;
    const uint32_t TEST_SUBSCR_CLASS = 0xBEEFu;
    const uint32_t TEST_BS_SVC       = 0xA5Cu;

    /* PDU type = 2 (10b) */
    pack_bits(bits, 2u, 0, 2);
    /* Broadcast type = 0 (00b) */
    pack_bits(bits, 0u, 2, 2);
    /* Remaining 78 bits before MLE can stay 0 */
    /* MLE fields at offset 82 */
    pack_bits(bits, TEST_LA,           82, 14);
    pack_bits(bits, TEST_SUBSCR_CLASS, 96, 16);
    pack_bits(bits, TEST_BS_SVC,      112, 12);

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mac_parse_schd(bits, NBITS, 0, opts, state);

    int ok = 1;

    if (!state->tetra_sysinfo_known) {
        fprintf(stderr, "FAIL(sysinfo): tetra_sysinfo_known not set\n");
        ok = 0;
    }
    if (state->tetra_la != (uint16_t)TEST_LA) {
        fprintf(stderr, "FAIL(sysinfo): tetra_la=%u expect=%u\n",
                (unsigned)state->tetra_la, (unsigned)TEST_LA);
        ok = 0;
    }
    if (state->tetra_subscr_class != (uint16_t)TEST_SUBSCR_CLASS) {
        fprintf(stderr, "FAIL(sysinfo): tetra_subscr_class=0x%04X expect=0x%04X\n",
                (unsigned)state->tetra_subscr_class, (unsigned)TEST_SUBSCR_CLASS);
        ok = 0;
    }
    if (state->tetra_bs_service_det != (uint16_t)TEST_BS_SVC) {
        fprintf(stderr, "FAIL(sysinfo): tetra_bs_service_det=0x%03X expect=0x%03X\n",
                (unsigned)state->tetra_bs_service_det, (unsigned)TEST_BS_SVC);
        ok = 0;
    }

    if (ok) fprintf(stderr, "OK: SYSINFO state update (LA=%u subscr=0x%04X bs=0x%03X)\n",
                    (unsigned)state->tetra_la,
                    (unsigned)state->tetra_subscr_class,
                    (unsigned)state->tetra_bs_service_det);
    free(state); free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Test 2: MAC-RESOURCE (SSI address) → tetra_ssi_valid, tetra_active_ssi,
 *          tetra_enc_mode
 *
 * PDU layout (ETSI EN 300 392-2 §21.4.3.1):
 *   [0-1]   PDU type         = 0 (RESOURCE)   2 bits
 *   [2]     Fill bits ind    = 0               1 bit
 *   [3]     Grant pos        = 0               1 bit
 *   [4-5]   Enc mode         = 1 (on)          2 bits
 *   [6]     Random access    = 0               1 bit
 *   [7-12]  Length indicator = 0               6 bits
 *   [13-15] Address type     = 1 (SSI)         3 bits
 *   [16-39] SSI              = 0x123456       24 bits
 * Total = 40 bits
 * ------------------------------------------------------------------------- */
static int test_mac_resource_ssi(void)
{
    const int NBITS = 40;
    uint8_t bits[40];
    memset(bits, 0, sizeof(bits));

    const uint32_t TEST_SSI      = 0x123456u;
    const uint32_t TEST_ENC_MODE = 1u;  /* TETRA_ENC_MODE_ON */

    /* PDU type = 0 */
    pack_bits(bits, 0u, 0, 2);
    /* fill bits / grant pos already 0 */
    /* enc_mode = 1 at offset 4, 2 bits */
    pack_bits(bits, TEST_ENC_MODE, 4, 2);
    /* len_ind = 0 at offset 7, 6 bits — already 0 */
    /* addr_type = 1 (SSI) at offset 13, 3 bits */
    pack_bits(bits, 1u, 13, 3);
    /* SSI at offset 16, 24 bits */
    pack_bits(bits, TEST_SSI, 16, 24);

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mac_parse_schd(bits, NBITS, 0, opts, state);

    int ok = 1;

    if (!state->tetra_ssi_valid) {
        fprintf(stderr, "FAIL(resource): tetra_ssi_valid not set\n");
        ok = 0;
    }
    if (state->tetra_active_ssi != TEST_SSI) {
        fprintf(stderr, "FAIL(resource): tetra_active_ssi=0x%06X expect=0x%06X\n",
                (unsigned)state->tetra_active_ssi, (unsigned)TEST_SSI);
        ok = 0;
    }
    if (state->tetra_enc_mode != (uint8_t)TEST_ENC_MODE) {
        fprintf(stderr, "FAIL(resource): tetra_enc_mode=%u expect=%u\n",
                (unsigned)state->tetra_enc_mode, (unsigned)TEST_ENC_MODE);
        ok = 0;
    }

    if (ok) fprintf(stderr, "OK: MAC-RESOURCE SSI state update (ssi=0x%06X enc=%u)\n",
                    (unsigned)state->tetra_active_ssi,
                    (unsigned)state->tetra_enc_mode);
    free(state); free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Test 3: NULL state must not crash
 * ------------------------------------------------------------------------- */
static int test_null_state(void)
{
    uint8_t bits[40];
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 0u, 0, 2);   /* MAC-RESOURCE */
    pack_bits(bits, 1u, 13, 3);  /* addr_type = SSI */
    pack_bits(bits, 1u, 16, 24); /* SSI = 1 */

    dsd_opts *opts = alloc_opts();
    tetra_mac_parse_schd(bits, 40, 0, opts, NULL);  /* must not crash */
    fprintf(stderr, "OK: NULL state did not crash\n");
    free(opts);
    return 1;
}

/* -------------------------------------------------------------------------
 * Test 4: SYSINFO too-short PDU must not corrupt state
 * ------------------------------------------------------------------------- */
static int test_sysinfo_tooshort(void)
{
    uint8_t bits[10];
    memset(bits, 0, sizeof(bits));
    pack_bits(bits, 2u, 0, 2);  /* BROADCAST */
    /* only 10 bits — far too short */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();
    tetra_mac_parse_schd(bits, 10, 0, opts, state);

    if (state->tetra_sysinfo_known) {
        fprintf(stderr, "FAIL(tooshort): tetra_sysinfo_known wrongly set\n");
        free(state); free(opts);
        return 0;
    }
    fprintf(stderr, "OK: too-short SYSINFO did not set tetra_sysinfo_known\n");
    free(state); free(opts);
    return 1;
}

/* -------------------------------------------------------------------------
 * Test 5: MAC-BROADCAST type=3 (NWRK-BROADCAST) carrying MLE
 *         D-NWRK-BROADCAST updates tetra_la, tetra_subscr_class,
 *         and sets tetra_nwrk_bcast_known.
 *
 * PDU layout (40 bits total):
 *   [0-1]   MAC type      = 2  (BROADCAST)
 *   [2-3]   bcast_type    = 3  (NWRK_BCAST)
 *   MLE D-NWRK-BROADCAST starts at bit 4:
 *   [4-8]   MLE type      = 0  (D-NWRK-BROADCAST)
 *   [9-22]  LA            = TEST_NWRK_LA  (14 bits)
 *   [23-38] subscr_class  = TEST_NWRK_SC  (16 bits)
 *   [39]    registration  = 0
 * Expected: tetra_la=TEST_NWRK_LA, tetra_subscr_class=TEST_NWRK_SC,
 *           tetra_nwrk_bcast_known=1
 * ------------------------------------------------------------------------- */
static int test_nwrk_broadcast(void)
{
    const uint32_t TEST_NWRK_LA = 4095u;  /* 0x0FFF, fits 14 bits */
    const uint32_t TEST_NWRK_SC = 0xCAFEu;
    const int NBITS = 40;

    uint8_t bits[40];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 2u,          0,  2);  /* MAC type = BROADCAST       */
    pack_bits(bits, 3u,          2,  2);  /* bcast_type = NWRK_BCAST    */
    /* MLE PDU at bit 4 */
    pack_bits(bits, 0u,          4,  5);  /* MLE type = D-NWRK-BROADCAST */
    pack_bits(bits, TEST_NWRK_LA, 9, 14); /* LA (14 bits)               */
    pack_bits(bits, TEST_NWRK_SC, 23, 16); /* subscr_class (16 bits)    */
    /* bit 39 = registration = 0 (already 0)                             */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mac_parse_schd(bits, NBITS, 0, opts, state);

    int ok = 1;
    if (!state->tetra_nwrk_bcast_known) {
        fprintf(stderr, "FAIL(nwrk_bcast): tetra_nwrk_bcast_known not set\n");
        ok = 0;
    }
    if (state->tetra_la != (uint16_t)TEST_NWRK_LA) {
        fprintf(stderr, "FAIL(nwrk_bcast): tetra_la=%u expect=%u\n",
                state->tetra_la, (unsigned)TEST_NWRK_LA);
        ok = 0;
    }
    if (state->tetra_subscr_class != (uint16_t)TEST_NWRK_SC) {
        fprintf(stderr, "FAIL(nwrk_bcast): tetra_subscr_class=0x%04X expect=0x%04X\n",
                state->tetra_subscr_class, (unsigned)TEST_NWRK_SC);
        ok = 0;
    }

    if (ok) fprintf(stderr, "OK: MAC-BROADCAST/NWRK-BCAST la=%u sc=0x%04X\n",
                    state->tetra_la, state->tetra_subscr_class);
    free(state); free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * Test 6: MAC-BROADCAST type=2 (RESTORE) must not crash and must not
 *         set tetra_nwrk_bcast_known.
 *
 * PDU layout (9 bits):
 *   [0-1]  MAC type = 2  (BROADCAST)
 *   [2-3]  bcast_type = 2  (RESTORE)
 *   [4-8]  MLE type = 2  (D-RESTORE-ACK, 5 bits)
 * ------------------------------------------------------------------------- */
static int test_bc_restore_noop(void)
{
    const int NBITS = 9;
    uint8_t bits[9];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 2u, 0, 2);  /* MAC type = BROADCAST      */
    pack_bits(bits, 2u, 2, 2);  /* bcast_type = RESTORE      */
    pack_bits(bits, 2u, 4, 5);  /* MLE type = D-RESTORE-ACK  */

    dsd_state *state = alloc_state();
    dsd_opts  *opts  = alloc_opts();

    tetra_mac_parse_schd(bits, NBITS, 0, opts, state);  /* must not crash */

    int ok = 1;
    if (state->tetra_nwrk_bcast_known) {
        fprintf(stderr, "FAIL(bc_restore): tetra_nwrk_bcast_known wrongly set\n");
        ok = 0;
    }
    if (state->tetra_sysinfo_known) {
        fprintf(stderr, "FAIL(bc_restore): tetra_sysinfo_known wrongly set\n");
        ok = 0;
    }

    if (ok) fprintf(stderr, "OK: MAC-BROADCAST/RESTORE did not corrupt state\n");
    free(state); free(opts);
    return ok;
}

/* A standards-shaped pi/4-DQPSK MAC-RESOURCE header containing a basic
 * Channel Allocation IE. No TM-SDU follows this header. */
static int test_mac_resource_channel_allocation(void)
{
    enum { NBITS = 44 };
    uint8_t bits[NBITS];
    memset(bits, 0, sizeof bits);

    pack_bits(bits, 0u, 0, 2);   /* MAC-RESOURCE */
    pack_bits(bits, 2u, 7, 6);   /* null/header-only PDU length code */
    pack_bits(bits, 0u, 13, 3);  /* null address */
    pack_bits(bits, 0u, 16, 1);  /* no power control */
    pack_bits(bits, 0u, 17, 1);  /* no slot grant */
    pack_bits(bits, 1u, 18, 1);  /* channel allocation present */
    pack_bits(bits, 0u, 19, 2);  /* replace */
    pack_bits(bits, 4u, 21, 4);  /* timeslot bitmap */
    pack_bits(bits, 1u, 25, 2);  /* downlink only */
    pack_bits(bits, 0u, 27, 1);  /* no CLCH permission */
    pack_bits(bits, 0u, 28, 1);  /* no cell change */
    pack_bits(bits, 50u, 29, 12);/* carrier */
    pack_bits(bits, 0u, 41, 1);  /* use SYSINFO band/offset */
    pack_bits(bits, 1u, 42, 2);  /* one monitoring pattern */

    dsd_state *state = alloc_state();
    dsd_opts *opts = alloc_opts();
    state->tetra_freq_band = 4;
    state->tetra_freq_offset = 2; /* -6.25 kHz */
    state->trunk_cc_freq = 460000000L;
    opts->trunk_enable = 1;
    tune_calls = 0;
    tuned_freq = 0;

    tetra_mac_parse_schd(bits, NBITS, 2, opts, state);

    int ok = 1;
    if (!state->tetra_vc_assignment_valid) ok = 0;
    if (state->tetra_vc_assignment_type != 0) ok = 0;
    if (state->tetra_vc_timeslot_bitmap != 4) ok = 0;
    if (state->tetra_vc_uplink_downlink != 1) ok = 0;
    if (state->tetra_vc_carrier != 50) ok = 0;
    if (state->tetra_vc_freq_hz != 461243750L) ok = 0;
    if (tune_calls != 1 || tuned_freq != 461243750L) ok = 0;
    if (!ok) fprintf(stderr, "FAIL(channel allocation): parsed fields differ\n");
    else fprintf(stderr, "OK: MAC-RESOURCE channel allocation freq=%ld slots=0x%X\n",
                 state->tetra_vc_freq_hz, state->tetra_vc_timeslot_bitmap);

    free(state); free(opts);
    return ok;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    int failed = 0;

    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = tune_stub;
    dsd_trunk_tuning_hooks_set(hooks);

    failed += !test_sysinfo();
    failed += !test_mac_resource_ssi();
    failed += !test_null_state();
    failed += !test_sysinfo_tooshort();
    failed += !test_nwrk_broadcast();
    failed += !test_bc_restore_noop();
    failed += !test_mac_resource_channel_allocation();

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});

    if (failed) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failed);
        return 2;
    }
    fprintf(stderr, "\nAll TETRA MAC state tests passed\n");
    return 0;
}
