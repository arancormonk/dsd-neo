// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA Phase 80 test suite.
 *
 * Covers functionality introduced in Phases 77-79:
 *
 * Phase 77: TETRA MAC dropped variables
 * Phase 78: TETRA CMCE/MLE dropped variables
 * Phase 79: TETRA MM dropped variables
 */

#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
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

static void pack_bits(uint8_t *out, uint32_t val, int offset, int nbits)
{
    for (int i = nbits - 1; i >= 0; i--)
        out[offset++] = (uint8_t)((val >> i) & 1u);
}

static dsd_state *alloc_state(void) { return (dsd_state *)calloc(1, sizeof(dsd_state)); }
static dsd_opts  *alloc_opts(void)  { return (dsd_opts  *)calloc(1, sizeof(dsd_opts));  }

static void wrap_mle_cmce(const uint8_t *cmce_body, int cmce_nbits,
                           uint8_t *out, int *out_nbits)
{
    int total = 3 + cmce_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_CMCE, 0, 3);
    memcpy(out + 3, cmce_body, (size_t)cmce_nbits);
    *out_nbits = total;
}

static void wrap_mle_mm(const uint8_t *mm_body, int mm_nbits,
                           uint8_t *out, int *out_nbits)
{
    int total = 3 + mm_nbits;
    memset(out, 0, (size_t)total);
    pack_bits(out, TETRA_MLE_PD_MM, 0, 3);
    memcpy(out + 3, mm_body, (size_t)mm_nbits);
    *out_nbits = total;
}

static void test_phase77_mac_resource(void)
{
    printf("[test_phase77_mac_resource]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t bits[100];
    int n = 0;
    
    pack_bits(bits, TETRA_MAC_TYPE_RESOURCE, n, 2); n+=2; // PDU type
    pack_bits(bits, 1, n, 1); n+=1; // fill_bits = 1
    pack_bits(bits, 0, n, 1); n+=1; // grant_pos = 0
    pack_bits(bits, TETRA_ENC_MODE_ON, n, 2); n+=2; // enc_mode = 1
    pack_bits(bits, 1, n, 1); n+=1; // rand_acc = 1
    pack_bits(bits, 42, n, 6); n+=6; // len_ind = 42
    pack_bits(bits, TETRA_MAC_ADDR_SSI_EVENT, n, 3); n+=3; // addr_type = 5
    pack_bits(bits, 123456, n, 24); n+=24; // ssi
    pack_bits(bits, 511, n, 10); n+=10; // event_label
    
    tetra_mac_parse_schd(bits, n, 0, opt, st);
    
    CHECK(st->tetra_mac_fill_bits == 1, "tetra_mac_fill_bits");
    CHECK(st->tetra_mac_grant_pos == 0, "tetra_mac_grant_pos");
    CHECK(st->tetra_mac_rand_acc == 1, "tetra_mac_rand_acc");
    CHECK(st->tetra_mac_len_ind == 42, "tetra_mac_len_ind");
    CHECK(st->tetra_mac_addr_type == TETRA_MAC_ADDR_SSI_EVENT, "tetra_mac_addr_type");
    CHECK(st->tetra_mac_event_label == 511, "tetra_mac_event_label");
    
    free(st); free(opt);
}

static void test_phase77_mac_resource_usage(void)
{
    printf("[test_phase77_mac_resource_usage]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t bits[100];
    int n = 0;
    
    pack_bits(bits, TETRA_MAC_TYPE_RESOURCE, n, 2); n+=2; // PDU type
    pack_bits(bits, 0, n, 1); n+=1; // fill_bits
    pack_bits(bits, 1, n, 1); n+=1; // grant_pos
    pack_bits(bits, 0, n, 2); n+=2; // enc_mode
    pack_bits(bits, 0, n, 1); n+=1; // rand_acc
    pack_bits(bits, 15, n, 6); n+=6; // len_ind
    pack_bits(bits, TETRA_MAC_ADDR_SSI_USAGE, n, 3); n+=3; // addr_type = 6
    pack_bits(bits, 654321, n, 24); n+=24; // ssi
    pack_bits(bits, 33, n, 6); n+=6; // usage_marker
    
    tetra_mac_parse_schd(bits, n, 0, opt, st);
    
    CHECK(st->tetra_mac_usage_marker == 33, "tetra_mac_usage_marker");
    
    free(st); free(opt);
}

static void test_phase77_sysinfo(void)
{
    printf("[test_phase77_sysinfo]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t bits[200];
    int n = 0;
    
    pack_bits(bits, TETRA_MAC_TYPE_BROADCAST, n, 2); n+=2; // PDU type = 2
    pack_bits(bits, TETRA_MAC_BC_SYSINFO, n, 2); n+=2; // broadcast type = 0
    
    pack_bits(bits, 1000, n, 12); n+=12; // main_carrier
    pack_bits(bits, 0, n, 4); n+=4; // freq_band
    pack_bits(bits, 0, n, 2); n+=2; // freq_offset
    pack_bits(bits, 0, n, 3); n+=3; // duplex_spacing
    pack_bits(bits, 1, n, 1); n+=1; // rev_op
    pack_bits(bits, 0, n, 2); n+=2; // num_csch
    pack_bits(bits, 0, n, 3); n+=3; // ms_txpwr
    pack_bits(bits, 0, n, 4); n+=4; // rxlev
    pack_bits(bits, 7, n, 4); n+=4; // acc_param
    pack_bits(bits, 14, n, 4); n+=4; // radio_dl_tmo
    pack_bits(bits, 0, n, 1); n+=1; // cck_valid
    pack_bits(bits, 0, n, 16); n+=16; // cck_or_hf
    pack_bits(bits, 2, n, 2); n+=2; // opt_field_type
    pack_bits(bits, 0xABCDE, n, 20); n+=20; // opt_field_data

    tetra_mac_parse_schd(bits, n, 0, opt, st);

    CHECK(st->tetra_sysinfo_main_carrier == 1000, "tetra_sysinfo_main_carrier");
    CHECK(st->tetra_sysinfo_rev_op == 1, "tetra_sysinfo_rev_op");
    CHECK(st->tetra_sysinfo_acc_param == 7, "tetra_sysinfo_acc_param");
    CHECK(st->tetra_sysinfo_radio_dl_tmo == 14, "tetra_sysinfo_radio_dl_tmo");
    CHECK(st->tetra_sysinfo_opt_field_type == 2, "tetra_sysinfo_opt_field_type");
    CHECK(st->tetra_sysinfo_opt_field_data == 0xABCDE, "tetra_sysinfo_opt_field_data");

    free(st); free(opt);
}

static void test_phase77_access_define(void)
{
    printf("[test_phase77_access_define]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t bits[100];
    int n = 0;
    
    pack_bits(bits, TETRA_MAC_TYPE_BROADCAST, n, 2); n+=2; // PDU type = 2
    pack_bits(bits, TETRA_MAC_BC_ACCESS_DEF, n, 2); n+=2; // broadcast type = 1
    
    pack_bits(bits, 1, n, 1); n+=1; // common_flag = 1
    pack_bits(bits, 0, n, 4); n+=4; // immediate
    pack_bits(bits, 0, n, 4); n+=4; // wait_time
    pack_bits(bits, 0, n, 4); n+=4; // num_ra
    pack_bits(bits, 0, n, 1); n+=1; // frame_len_f
    pack_bits(bits, 0, n, 4); n+=4; // ts_ptr
    pack_bits(bits, 0, n, 3); n+=3; // min_pdu_pri

    tetra_mac_parse_schd(bits, n, 0, opt, st);

    CHECK(st->tetra_access_common_flag == 1, "tetra_access_common_flag");

    free(st); free(opt);
}

static void test_phase78_cmce_d_tx_granted(void)
{
    printf("[test_phase78_cmce_d_tx_granted]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t cmce[100];
    int n = 0;
    
    pack_bits(cmce, TETRA_CMCE_D_TX_GRANTED, n, 5); n+=5;
    pack_bits(cmce, 0x123, n, 14); n+=14;
    pack_bits(cmce, 3, n, 2); n+=2;
    pack_bits(cmce, 1, n, 1); n+=1;
    pack_bits(cmce, 0, n, 1); n+=1;
    pack_bits(cmce, 1, n, 1); n+=1;
    pack_bits(cmce, 0, n, 1); n+=1; /* O-bit */

    uint8_t pdu[200]; int out_n;
    wrap_mle_cmce(cmce, n, pdu, &out_n);
    tetra_mle_dispatch(pdu, out_n, 0, opt, st);

    CHECK(st->tetra_cmce_tx_granted_perm == 1, "tetra_cmce_tx_granted_perm");
    CHECK(st->tetra_cmce_tx_granted_reserv == 1, "tetra_cmce_tx_granted_reserv");

    free(st); free(opt);
}

static void test_phase78_mle_nwrk_broadcast(void)
{
    printf("[test_phase78_mle_nwrk_broadcast]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t pdu[200];
    int n = 0;

    /* ETSI EN 300 392-2 V3.8.1, Annex E.2.2 table E.16. */
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3); n+=3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3); n+=3;
    pack_bits(pdu, 0x4321, n, 16); n+=16;
    pack_bits(pdu, 2, n, 2); n+=2;
    pack_bits(pdu, 0, n, 1); n+=1;

    tetra_mle_dispatch(pdu, n, 0, opt, st);

    CHECK(st->tetra_mle_cell_reselect_params == 0x4321, "tetra_mle_cell_reselect_params");
    CHECK(st->tetra_mle_cell_load == 2, "tetra_mle_cell_load");
    CHECK(st->tetra_mle_ca_neighbor_count_valid == 0,
          "Annex E.16 has no neighbour count");

    /* Annex E.17 explicitly carries the zero value meaning no neighbour-cell
     * information is available. */
    memset(pdu, 0, sizeof(pdu)); n=0;
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3); n+=3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3); n+=3;
    pack_bits(pdu, 0x1111, n, 16); n+=16;
    pack_bits(pdu, 1, n, 2); n+=2;
    pack_bits(pdu, 1, n, 1); n+=1; /* O-bit */
    pack_bits(pdu, 0, n, 1); n+=1; /* no network time */
    pack_bits(pdu, 1, n, 1); n+=1; /* neighbour count present */
    pack_bits(pdu, 0, n, 3); n+=3;
    tetra_mle_dispatch(pdu, n, 0, opt, st);
    CHECK(st->tetra_mle_ca_neighbor_count_valid == 1
          && st->tetra_mle_ca_neighbor_count == 0,
          "Annex E.17 no-neighbour indication accepted");

    /* Annex E.18: two CA neighbours.  The first has no optional fields; the
     * second carries only its Location Area from the ten standardized P-bits. */
    memset(pdu, 0, sizeof(pdu)); n=0;
    pack_bits(pdu, TETRA_MLE_PD_MLE, n, 3); n+=3;
    pack_bits(pdu, TETRA_MLE_D_NWRK_BROADCAST, n, 3); n+=3;
    pack_bits(pdu, 0x2222, n, 16); n+=16;
    pack_bits(pdu, 3, n, 2); n+=2;
    pack_bits(pdu, 1, n, 1); n+=1; /* O-bit */
    pack_bits(pdu, 0, n, 1); n+=1; /* no network time */
    pack_bits(pdu, 1, n, 1); n+=1; /* neighbour count present */
    pack_bits(pdu, 2, n, 3); n+=3;
    pack_bits(pdu, 1, n, 5); n+=5;
    pack_bits(pdu, 2, n, 2); n+=2;
    pack_bits(pdu, 1, n, 1); n+=1;
    pack_bits(pdu, 1, n, 2); n+=2;
    pack_bits(pdu, 1000, n, 12); n+=12;
    pack_bits(pdu, 0, n, 1); n+=1; /* first neighbour O-bit */
    pack_bits(pdu, 2, n, 5); n+=5;
    pack_bits(pdu, 1, n, 2); n+=2;
    pack_bits(pdu, 0, n, 1); n+=1;
    pack_bits(pdu, 2, n, 2); n+=2;
    pack_bits(pdu, 2000, n, 12); n+=12;
    pack_bits(pdu, 1, n, 1); n+=1; /* second neighbour O-bit */
    pack_bits(pdu, 0, n, 1); n+=1; /* carrier extension */
    pack_bits(pdu, 0, n, 1); n+=1; /* MCC */
    pack_bits(pdu, 0, n, 1); n+=1; /* MNC */
    pack_bits(pdu, 1, n, 1); n+=1; /* LA */
    pack_bits(pdu, 0x2345, n, 14); n+=14;
    pack_bits(pdu, 0, n, 1); n+=1; /* maximum TX power */
    pack_bits(pdu, 0, n, 1); n+=1; /* minimum RX access */
    pack_bits(pdu, 0, n, 1); n+=1; /* subscriber class */
    pack_bits(pdu, 0, n, 1); n+=1; /* BS service details */
    pack_bits(pdu, 0, n, 1); n+=1; /* timeshare/security */
    pack_bits(pdu, 0, n, 1); n+=1; /* TDMA frame offset */
    tetra_mle_dispatch(pdu, n, 0, opt, st);
    CHECK(st->tetra_mle_ca_neighbor_count_valid == 1
          && st->tetra_mle_ca_neighbor_count == 2,
          "Annex E.18 two-neighbour collection accepted");
    CHECK(st->tetra_mle_ca_neighbor_cell_id[0] == 1
          && st->tetra_mle_ca_neighbor_main_carrier[0] == 1000
          && st->tetra_mle_ca_neighbor_cell_id[1] == 2
          && st->tetra_mle_ca_neighbor_main_carrier[1] == 2000,
          "Annex E.18 mandatory neighbour fields retained");

    /* Remove the final Type 2 P-bit after changing the mandatory value.  No
     * part of the incomplete broadcast may replace the previous snapshot. */
    pack_bits(pdu, 0x3333, 6, 16);
    tetra_mle_dispatch(pdu, n - 1, 0, opt, st);
    CHECK(st->tetra_mle_cell_reselect_params == 0x2222
          && st->tetra_mle_ca_neighbor_count == 2,
          "truncated Annex E.18 preserves the previous network broadcast");

    free(st); free(opt);
}

static void test_phase79_mm_attach_detach(void)
{
    printf("[test_phase79_mm_attach_detach]\n");
    dsd_state *st  = alloc_state();
    dsd_opts  *opt = alloc_opts();

    uint8_t mm[100];
    int n = 0;
    
    pack_bits(mm, TETRA_MM_D_ATTACH_DETACH_GROUP, n, 4); n+=4;
    pack_bits(mm, 1, n, 1); n+=1; // group identity report
    pack_bits(mm, 1, n, 1); n+=1; // acknowledgement request
    pack_bits(mm, 1, n, 1); n+=1; // attach/detach mode
    pack_bits(mm, 0, n, 1); n+=1; // no optional elements

    uint8_t pdu[200]; int out_n;
    wrap_mle_mm(mm, n, pdu, &out_n);
    tetra_mle_dispatch(pdu, out_n, 0, opt, st);

    CHECK(st->tetra_mm_detach_flag == 1, "tetra_mm_detach_flag");
    CHECK(st->tetra_mm_class_of_grp == 1, "tetra_mm_class_of_grp");
    CHECK(st->tetra_mm_addr_type == 1, "tetra_mm_addr_type");
    CHECK(st->tetra_mm_group_identity_valid == 1, "tetra_mm_group_identity_valid");
    CHECK(st->tetra_mm_group_identity_report == 1, "tetra_mm_group_identity_report");
    CHECK(st->tetra_mm_group_identity_ack_request == 1, "tetra_mm_group_identity_ack_request");
    CHECK(st->tetra_mm_group_identity_attach_detach_mode == 1,
          "tetra_mm_group_identity_attach_detach_mode");

    free(st); free(opt);
}

int main(void)
{
    printf("TETRA Phase 80 Test Suite (Phases 77-79)\n");

    test_phase77_mac_resource();
    test_phase77_mac_resource_usage();
    test_phase77_sysinfo();
    test_phase77_access_define();

    test_phase78_cmce_d_tx_granted();
    test_phase78_mle_nwrk_broadcast();

    test_phase79_mm_attach_detach();

    if (g_fail > 0) {
        printf("FAILED %d TESTS\n", g_fail);
        return 1;
    }
    printf("ALL %d TESTS PASSED\n", g_pass);
    return 0;
}
