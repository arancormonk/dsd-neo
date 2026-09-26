// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 69 test suite — MAC Fragment Reassembly.
 *
 * Tests:
 *  1. Fill indication does not control fragmentation
 *  2. MAC-RESOURCE length indication 111111 seeds the reassembly buffer
 *  3. MAC-FRAG accumulates additional bits into the buffer
 *  4. MAC-END completes the reassembly and dispatches to MLE
 *  5. After MAC-END the buffer is cleared (frag_active=0, frag_nbits=0)
 *  6. A full RESOURCE→FRAG→END sequence produces a correctly decoded CMCE PDU
 */

#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__); \
            g_fail++; \
        } \
    } while (0)

static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

static void pack_bits(uint8_t *out, uint32_t val, int offset, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--)
        out[offset++] = (uint8_t)((val >> i) & 1u);
}

/* -----------------------------------------------------------------------
 * Build a minimal MAC-RESOURCE PDU (NULL address, variable payload).
 *
 * MAC-RESOURCE layout:
 *   [0-1]   pdu_type = 00
 *   [2]     fill_bits indicator
 *   [3]     grant_pos = 0
 *   [4-5]   enc_mode  = 0
 *   [6]     rand_acc  = 0
 *   [7-12]  length indication (63 starts fragmentation)
 *   [13-15] addr_type = 0 (NULL, 0 address bits)
 *   [16-18] optional-element flags = 0
 *   [19+]   TM-SDU payload bits
 * ----------------------------------------------------------------------- */
static int build_mac_resource(uint8_t *out, int out_size,
                               uint8_t fill_bits,
                               uint8_t len_ind,
                               const uint8_t *payload, int payload_nbits)
{
    int total = 19 + payload_nbits;
    if (total > out_size) return -1;
    memset(out, 0, (size_t)total);
    pack_bits(out, 0u,         0,  2); /* PDU type = MAC-RESOURCE */
    pack_bits(out, fill_bits,  2,  1);
    pack_bits(out, 0u,         3,  1); /* grant_pos  */
    pack_bits(out, 0u,         4,  2); /* enc_mode   */
    pack_bits(out, 0u,         6,  1); /* rand_acc   */
    pack_bits(out, len_ind,    7,  6);
    pack_bits(out, 0u,        13,  3); /* addr_type  = NULL */
    pack_bits(out, 0u,        16,  1); /* power control flag */
    pack_bits(out, 0u,        17,  1); /* slot granting flag */
    pack_bits(out, 0u,        18,  1); /* channel allocation flag */
    if (payload_nbits > 0)
        memcpy(out + 19, payload, (size_t)payload_nbits);
    return total;
}

/* Build MAC-FRAG (subtype=0) or MAC-END (subtype=1) */
static int build_mac_frag_end(uint8_t *out, int out_size,
                               uint8_t subtype,
                               const uint8_t *payload, int payload_nbits)
{
    int total = 3 + payload_nbits;
    if (total > out_size) return -1;
    memset(out, 0, (size_t)total);
    pack_bits(out, 1u,      0, 2); /* PDU type = MAC-FRAG/END */
    pack_bits(out, subtype, 2, 1);
    if (payload_nbits > 0)
        memcpy(out + 3, payload, (size_t)payload_nbits);
    return total;
}

/* =======================================================================
 * Test 1+2: MAC-RESOURCE fill_bits=1 dispatches; fill_bits=0 buffers
 * ======================================================================= */
static void test_resource_fill_bits(void)
{
    printf("[test_resource_fill_bits]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /*
     * Build a partial TM-SDU beginning with the three-bit CMCE discriminator.
     * With fill_bits=1 → dispatch immediately → tetra_frag_active stays 0
     */
    uint8_t tmsdu[9];
    memset(tmsdu, 0, sizeof(tmsdu));
    pack_bits(tmsdu, TETRA_MLE_PD_CMCE, 0, 3);

    uint8_t pdu[128];
    int n = build_mac_resource(pdu, sizeof(pdu), 1, 4, tmsdu, 9);
    tetra_mac_parse_schd(pdu, n, 0, opt, st);

    CHECK(st->tetra_frag_active == 0, "fill_bits=1: no fragment started");
    CHECK(st->tetra_frag_nbits  == 0, "fill_bits=1: buffer stays empty");

    /* fill_bits=0 with an ordinary length still dispatches directly. */
    dsd_state *st2 = alloc_state();
    n = build_mac_resource(pdu, sizeof(pdu), 0, 4, tmsdu, 9);
    tetra_mac_parse_schd(pdu, n, 0, opt, st2);

    CHECK(st2->tetra_frag_active == 0, "fill_bits=0: no fragment started");
    CHECK(st2->tetra_frag_nbits  == 0, "fill_bits=0: buffer stays empty");

    dsd_state *st3 = alloc_state();
    n = build_mac_resource(pdu, sizeof(pdu), 0, 63, tmsdu, 9);
    tetra_mac_parse_schd(pdu, n, 0, opt, st3);
    CHECK(st3->tetra_frag_active == 1, "len_ind=63: fragment started");
    CHECK(st3->tetra_frag_nbits == 9, "len_ind=63: 9 bits buffered");

    free(st); free(st2); free(st3); free(opt);
}

/* =======================================================================
 * Test 3: MAC-FRAG appends to buffer
 * ======================================================================= */
static void test_mac_frag_accumulate(void)
{
    printf("[test_mac_frag_accumulate]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Seed with 9 bits via MAC-RESOURCE fill_bits=0 */
    uint8_t tmsdu[9];
    memset(tmsdu, 0, sizeof(tmsdu));
    pack_bits(tmsdu, TETRA_MLE_PD_CMCE, 0, 3);

    uint8_t pdu[128];
    int n = build_mac_resource(pdu, sizeof(pdu), 0, 63, tmsdu, 9);
    tetra_mac_parse_schd(pdu, n, 0, opt, st);
    CHECK(st->tetra_frag_nbits == 9, "after RESOURCE: 9 bits in buffer");

    /* MAC-FRAG with 5 bits */
    uint8_t frag[5];
    memset(frag, 0, sizeof(frag));
    n = build_mac_frag_end(pdu, sizeof(pdu), 0, frag, 5);
    tetra_mac_parse_schd(pdu, n, 0, opt, st);

    CHECK(st->tetra_frag_active == 1,  "after FRAG: still active");
    CHECK(st->tetra_frag_nbits  == 14, "after FRAG: 9+5=14 bits");

    free(st); free(opt);
}

/* =======================================================================
 * Test 4+5+6: Full RESOURCE → FRAG → END sequence dispatches D-ALERT
 *
 * Full TM-SDU (33 bits):
 *   LLC BL-UDATA(4b=2) + PD=CMCE(3b=2) + CMCE D-ALERT(5b=0)
 *   + call_id(14b=5) + setup timeout(3b) + reserved/duplex/queued(3b)
 *   + O-bit(1b=0)
 *
 * Split:
 *   RESOURCE payload: bits 0-3  (4 bits) — LLC header
 *   FRAG    payload:  bits 4-8  (5 bits) — MLE PD + part of CMCE type
 *   END     payload:  bits 9-32 (24 bits) — remaining D-ALERT fields
 * ======================================================================= */
static void test_full_fragment_sequence(void)
{
    printf("[test_full_fragment_sequence]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Build the 33-bit LLC PDU / TM-SDU. */
    uint8_t tmsdu[33];
    memset(tmsdu, 0, sizeof(tmsdu));
    pack_bits(tmsdu, 2, 0, 4); /* BL-UDATA without FCS */
    pack_bits(tmsdu, TETRA_MLE_PD_CMCE, 4, 3);
    pack_bits(tmsdu, 0, 7, 5);
    pack_bits(tmsdu, 5, 12, 14);
    pack_bits(tmsdu, 3, 26, 3);
    pack_bits(tmsdu, 1, 31, 1);
    pack_bits(tmsdu, 0, 32, 1); /* O-bit */

    uint8_t pdu[128];
    int n;

    /* ---- MAC-RESOURCE: discriminator, fill_bits=0 ---- */
    n = build_mac_resource(pdu, sizeof(pdu), 0, 63, tmsdu, 4);
    tetra_mac_parse_schd(pdu, n, 2, opt, st);
    CHECK(st->tetra_frag_nbits  == 4,   "RESOURCE: 4 bits buffered");
    CHECK(st->tetra_frag_active == 1,   "RESOURCE: active=1");
    CHECK(st->tetra_d_alert_valid == 0, "RESOURCE: D-ALERT not yet dispatched");

    /* ---- MAC-FRAG: next 5 bits ---- */
    n = build_mac_frag_end(pdu, sizeof(pdu), 0, tmsdu + 4, 5);
    tetra_mac_parse_schd(pdu, n, 2, opt, st);
    CHECK(st->tetra_frag_nbits  == 9,  "FRAG: 9 bits accumulated");
    CHECK(st->tetra_d_alert_valid == 0, "FRAG: D-ALERT still not dispatched");

    /* ---- MAC-END: final 24 bits ---- */
    n = build_mac_frag_end(pdu, sizeof(pdu), 1, tmsdu + 9, 24);
    tetra_mac_parse_schd(pdu, n, 2, opt, st);

    CHECK(st->tetra_frag_active  == 0,  "END: active cleared");
    CHECK(st->tetra_frag_nbits   == 0,  "END: buffer cleared");
    CHECK(st->tetra_d_alert_valid == 1, "END: D-ALERT decoded after reassembly");
    CHECK(st->tetra_d_alert_call_id == 5, "END: D-ALERT call_id=5");
    CHECK(st->tetra_call_active   == 1, "END: call_active set by D-ALERT");

    free(st); free(opt);
}

/* =======================================================================
 * Test: MAC-END without prior RESOURCE (late join) — must not crash
 * ======================================================================= */
static void test_end_without_start(void)
{
    printf("[test_end_without_start]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    /* Send MAC-END with empty buffer — should be harmless */
    uint8_t pdu[12];
    uint8_t empty[4] = {0, 0, 0, 0};
    int n = build_mac_frag_end(pdu, sizeof(pdu), 1, empty, 4);
    tetra_mac_parse_schd(pdu, n, 0, opt, st);  /* must not crash */

    CHECK(st->tetra_frag_active == 0, "END without start: active stays 0");
    CHECK(st->tetra_frag_nbits  == 0, "END without start: buffer stays 0");

    free(st); free(opt);
}

static void test_fragment_context_guards(void)
{
    printf("[test_fragment_context_guards]\n");
    dsd_state *st = alloc_state();
    dsd_opts *opt = alloc_opts();
    uint8_t pdu[128];
    uint8_t payload[40] = {0};

    pack_bits(payload, TETRA_MLE_PD_CMCE, 0, 3);
    pack_bits(payload, TETRA_CMCE_D_ALERT, 3, 5);

    int n = build_mac_frag_end(pdu, sizeof(pdu), 0, payload, 9);
    tetra_mac_parse_schd(pdu, n, 1, opt, st);
    CHECK(st->tetra_frag_active == 0 && st->tetra_frag_nbits == 0,
          "orphan FRAG cannot create a reassembly context");

    n = build_mac_frag_end(pdu, sizeof(pdu), 1, payload, 34);
    tetra_mac_parse_schd(pdu, n, 1, opt, st);
    CHECK(st->tetra_d_alert_valid == 0,
          "orphan END cannot dispatch a complete-looking TM-SDU");

    n = build_mac_resource(pdu, sizeof(pdu), 0, 63, payload, 9);
    tetra_mac_parse_schd(pdu, n, 1, opt, st);
    n = build_mac_frag_end(pdu, sizeof(pdu), 0, payload + 9, 5);
    tetra_mac_parse_schd(pdu, n, 2, opt, st);
    CHECK(st->tetra_frag_active == 0 && st->tetra_frag_nbits == 0,
          "fragment from a different colour code discards the sequence");

    st->tetra_frag_active = 1;
    st->tetra_frag_cc = 1;
    st->tetra_frag_nbits = 1020;
    n = build_mac_frag_end(pdu, sizeof(pdu), 0, payload, 8);
    tetra_mac_parse_schd(pdu, n, 1, opt, st);
    CHECK(st->tetra_frag_active == 0 && st->tetra_frag_nbits == 0,
          "reassembly overflow discards the complete sequence");

    free(st); free(opt);
}

int main(void)
{
    printf("=== TETRA Phase 69 Test Suite: MAC Fragment Reassembly ===\n\n");

    test_resource_fill_bits();
    test_mac_frag_accumulate();
    test_full_fragment_sequence();
    test_end_without_start();
    test_fragment_context_guards();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
