// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Downlink MAC PDUs through their decoders (tetra_mac_pdu.h): the MAC-RESOURCE header with its address element
 * (EN 300 392-2 table 21.55), SYSINFO with and without its D-MLE-SYSINFO (table 21.65, clause 18.4.2.2) and
 * ACCESS-DEFINE (table 21.79); and what the MAC keeps of a SYSINFO and prints for an ACCESS-DEFINE.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_mac_pdu.h"
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

static void
test_mac_resource_header(void) {
    printf("[test_mac_resource_header]\n");

    uint8_t bits[100];
    int n = 0;

    pack_bits(bits, TETRA_MAC_TYPE_RESOURCE, n, 2);
    n += 2; // PDU type
    pack_bits(bits, 1, n, 1);
    n += 1; // fill_bits = 1
    pack_bits(bits, 0, n, 1);
    n += 1; // grant_pos = 0
    pack_bits(bits, TETRA_ENC_MODE_RESERVED, n, 2);
    n += 2; // enc_mode = 1
    pack_bits(bits, 1, n, 1);
    n += 1; // rand_acc = 1
    pack_bits(bits, 42, n, 6);
    n += 6; // len_ind = 42
    pack_bits(bits, TETRA_MAC_ADDR_SSI_EVENT, n, 3);
    n += 3; // addr_type = 5
    pack_bits(bits, 123456, n, 24);
    n += 24; // ssi
    pack_bits(bits, 511, n, 10);
    n += 10; // event_label
    pack_bits(bits, 0, n, 3);
    n += 3; // power control, slot granting and channel allocation flags
    pack_bits(bits, 1, n, 1);
    n += 1; // fill bit

    tetra_mac_resource h;
    CHECK(tetra_mac_resource_decode(bits, n, &h), "MAC-RESOURCE header decodes");
    CHECK(h.fill == 1, "fill bit indication");
    CHECK(h.grant_pos == 0, "position of grant");
    CHECK(h.enc_mode == TETRA_ENC_MODE_RESERVED && h.addr.esi, "encryption mode, with an encrypted short identity");
    CHECK(h.random_access == 1, "random access flag");
    CHECK(h.length_ind == 42, "length indication");
    CHECK(h.addr.type == TETRA_MAC_ADDR_SSI_EVENT && h.addr.ssi == 123456, "address type");
    CHECK(h.addr.event_label == 511, "event label");
}

static void
test_mac_resource_usage_marker(void) {
    printf("[test_mac_resource_usage_marker]\n");

    uint8_t bits[100];
    int n = 0;

    pack_bits(bits, TETRA_MAC_TYPE_RESOURCE, n, 2);
    n += 2; // PDU type
    pack_bits(bits, 0, n, 1);
    n += 1; // fill_bits
    pack_bits(bits, 1, n, 1);
    n += 1; // grant_pos
    pack_bits(bits, 0, n, 2);
    n += 2; // enc_mode
    pack_bits(bits, 0, n, 1);
    n += 1; // rand_acc
    pack_bits(bits, 15, n, 6);
    n += 6; // len_ind
    pack_bits(bits, TETRA_MAC_ADDR_SSI_USAGE, n, 3);
    n += 3; // addr_type = 6
    pack_bits(bits, 654321, n, 24);
    n += 24; // ssi
    pack_bits(bits, 33, n, 6);
    n += 6; // usage_marker
    pack_bits(bits, 0, n, 3);
    n += 3; // power control, slot granting and channel allocation flags

    tetra_mac_resource h;
    CHECK(tetra_mac_resource_decode(bits, n, &h) && h.grant_pos == 1 && h.length_ind == 15,
          "MAC-RESOURCE header decodes");
    CHECK(h.addr.type == TETRA_MAC_ADDR_SSI_USAGE && h.addr.ssi == 654321 && h.addr.usage_marker == 33, "usage marker");
}

static void
test_sysinfo_without_mle(void) {
    printf("[test_sysinfo_without_mle]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    uint8_t bits[200];
    int n = 0;

    pack_bits(bits, TETRA_MAC_TYPE_BROADCAST, n, 2);
    n += 2; // PDU type = 2
    pack_bits(bits, TETRA_MAC_BC_SYSINFO, n, 2);
    n += 2; // broadcast type = 0

    pack_bits(bits, 1000, n, 12);
    n += 12; // main_carrier
    pack_bits(bits, 0, n, 4);
    n += 4; // freq_band
    pack_bits(bits, 0, n, 2);
    n += 2; // freq_offset
    pack_bits(bits, 0, n, 3);
    n += 3; // duplex_spacing
    pack_bits(bits, 1, n, 1);
    n += 1; // rev_op
    pack_bits(bits, 0, n, 2);
    n += 2; // num_csch
    pack_bits(bits, 0, n, 3);
    n += 3; // ms_txpwr
    pack_bits(bits, 0, n, 4);
    n += 4; // rxlev
    pack_bits(bits, 7, n, 4);
    n += 4; // acc_param
    pack_bits(bits, 14, n, 4);
    n += 4; // radio_dl_tmo
    pack_bits(bits, 0, n, 1);
    n += 1; // cck_valid
    pack_bits(bits, 0, n, 16);
    n += 16; // cck_or_hf
    pack_bits(bits, 2, n, 2);
    n += 2; // opt_field_type
    pack_bits(bits, 0xABCDE, n, 20);
    n += 20; // opt_field_data

    tetra_mac_broadcast bc;
    CHECK(tetra_mac_broadcast_decode(bits, n, &bc) && bc.type == TETRA_MAC_BC_SYSINFO && !bc.sysinfo.mle_present,
          "SYSINFO without its D-MLE-SYSINFO decodes");
    CHECK(bc.sysinfo.main_carrier == 1000, "main carrier");
    CHECK(bc.sysinfo.reverse == 1, "reverse operation");
    CHECK(bc.sysinfo.access_parameter == 7, "ACCESS_PARAMETER");
    CHECK(bc.sysinfo.radio_dl_timeout == 14, "RADIO_DOWNLINK_TIMEOUT");
    CHECK(bc.sysinfo.option_flag == 2, "optional field flag");
    CHECK(bc.sysinfo.option == 0xABCDE, "optional field");

    tetra_mac_parse_schd(bits, n, 0, opt, st);
    CHECK(tetra_st(st)->sysinfo_main_carrier == 1000, "the main carrier the frontends show");

    free_state(st);
    free(opt);
}

static void
test_sysinfo_cck_and_mle(void) {
    const int NBITS = 124;
    uint8_t bits[124];
    DSD_MEMSET(bits, 0, sizeof(bits));

    /* PDU type = 2 (BROADCAST), bcast_type = 0 (SYSINFO) */
    pack_bits(bits, 2u, 0, 2);
    pack_bits(bits, 0u, 2, 2);
    /* main_carrier = 100 at offset 4, 12 bits */
    pack_bits(bits, 100u, 4, 12);
    /* freq_band = 0 at offset 16, 4 bits */
    pack_bits(bits, 0u, 16, 4);
    /* freq_offset = 0 at offset 20, 2 bits */
    pack_bits(bits, 0u, 20, 2);
    /* duplex_spacing = 5 at offset 22, 3 bits */
    pack_bits(bits, 5u, 22, 3);
    /* rev_op = 0 at offset 25, 1 bit */
    pack_bits(bits, 0u, 25, 1);
    /* num_csch = 3 at offset 26, 2 bits */
    pack_bits(bits, 3u, 26, 2);
    /* ms_txpwr = 6 at offset 28, 3 bits */
    pack_bits(bits, 6u, 28, 3);
    /* rxlev = 11 at offset 31, 4 bits */
    pack_bits(bits, 11u, 31, 4);
    /* acc_param = 0 at offset 35, 4 bits */
    pack_bits(bits, 0u, 35, 4);
    /* radio_dl_tmo = 0 at offset 39, 4 bits */
    pack_bits(bits, 0u, 39, 4);
    /* cck_valid = 1 at offset 43, 1 bit */
    pack_bits(bits, 1u, 43, 1);
    /* cck_or_hf = 0xABCD at offset 44, 16 bits */
    pack_bits(bits, 0xABCDu, 44, 16);
    /* opt_field_type = 0 at offset 60, 2 bits */
    pack_bits(bits, 0u, 60, 2);
    /* opt_field_data = 0 at offset 62, 20 bits */
    pack_bits(bits, 0u, 62, 20);
    /* MLE: LA=500, subscr_cls=0x1234, bs_svc=0x567 */
    pack_bits(bits, 500u, 82, 14);
    pack_bits(bits, 0x1234u, 96, 16);
    pack_bits(bits, 0x567u, 112, 12);

    tetra_mac_broadcast bc;
    CHECK(tetra_mac_broadcast_decode(bits, NBITS, &bc) && bc.type == TETRA_MAC_BC_SYSINFO, "SYSINFO decodes");
    CHECK(bc.sysinfo.duplex_spacing == 5, "SYSINFO duplex_spacing=5");
    CHECK(bc.sysinfo.num_scch == 3, "SYSINFO num_csch=3");
    CHECK(bc.sysinfo.ms_txpwr_max == 6, "SYSINFO ms_txpwr_max=6");
    CHECK(bc.sysinfo.rxlev_access_min == 11, "SYSINFO rxlev_access_min=11");
    CHECK(bc.sysinfo.cck_flag == 1, "SYSINFO cck_valid=1");
    CHECK(bc.sysinfo.hyperframe_or_cck == 0xABCDu, "SYSINFO cck_id=0xABCD");
    CHECK(bc.sysinfo.mle_present && bc.sysinfo.la == 500 && bc.sysinfo.subscriber_class == 0x1234u
              && bc.sysinfo.bs_service == 0x567u,
          "SYSINFO D-MLE-SYSINFO");
}

static void
test_sysinfo_counted(void) {
    uint8_t bits[124];
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bits(bits, 2u, 0, 2); /* BROADCAST */
    pack_bits(bits, 0u, 2, 2); /* SYSINFO   */
    /* pad to 124 bits */

    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    tetra_mac_parse_schd(bits, 124, 0, opt, st);
    CHECK(tetra_st(st)->frames_sysinfo == 1, "frame_sysinfo incremented");

    free_state(st);
    free(opt);
}

static void
test_access_define_fields(void) {
    printf("[test_access_define_fields]\n");
    dsd_state* st = alloc_state();
    dsd_opts* opt = alloc_opts();

    /* MAC-BROADCAST (2) -> ACCESS-DEFINE (1), EN 300 392-2 table 21.79 */
    uint8_t mac[50];
    DSD_MEMSET(mac, 0, sizeof(mac));
    pack_bits(mac, 2, 0, 2);  /* PDU = BROADCAST */
    pack_bits(mac, 1, 2, 2);  /* BROADCAST type = ACCESS-DEFINE */
    pack_bits(mac, 1, 4, 1);  /* assigned control channel */
    pack_bits(mac, 2, 5, 2);  /* access code C */
    pack_bits(mac, 5, 7, 4);  /* imm = 5 */
    pack_bits(mac, 2, 11, 4); /* wait = 2 */
    pack_bits(mac, 9, 15, 4); /* num_ra = 9 */
    pack_bits(mac, 1, 19, 1); /* frame_len_f = 1 */
    pack_bits(mac, 4, 20, 4); /* ts_ptr = 4 */
    pack_bits(mac, 3, 24, 3); /* min_prio = 3 */
    pack_bits(mac, 0, 27, 2); /* no optional field */
    pack_bits(mac, 4, 29, 3); /* filler bits 100 */

    tetra_mac_broadcast bc;
    CHECK(tetra_mac_broadcast_decode(mac, 32, &bc) && bc.type == TETRA_MAC_BC_ACCESS_DEF
              && bc.access_define.nbits == 32,
          "ACCESS-DEFINE decodes");
    CHECK(bc.access_define.assigned == 1, "Extracted assigned control channel flag");
    CHECK(bc.access_define.access_code == 2, "Extracted access code");
    CHECK(bc.access_define.imm == 5, "Extracted immediate");
    CHECK(bc.access_define.wait_time == 2, "Extracted wait time");
    CHECK(bc.access_define.num_ra == 9, "Extracted num_ra");
    CHECK(bc.access_define.frame_length_factor == 1, "Extracted frame_len_f");
    CHECK(bc.access_define.timeslot_pointer == 4, "Extracted ts_ptr");
    CHECK(bc.access_define.min_priority == 3, "Extracted min_pdu_pri");

    char log[256];
    tetra_test_mac_logged(mac, 32, 0, opt, st, log, sizeof log);
    CHECK(strstr(log, "[TETRA ACCESS-DEFINE] CC=0  assigned  access_code=C  imm=5  wait=2  num_ra=9  frame_len_f=1  "
                      "ts_ptr=4  min_prio=3\n")
              != NULL,
          "ACCESS-DEFINE summary line");

    free_state(st);
    free(opt);
}

static void
test_access_define_assigned_flag(void) {
    printf("[test_access_define_assigned_flag]\n");

    uint8_t bits[100];
    int n = 0;

    pack_bits(bits, TETRA_MAC_TYPE_BROADCAST, n, 2);
    n += 2; // PDU type = 2
    pack_bits(bits, TETRA_MAC_BC_ACCESS_DEF, n, 2);
    n += 2; // broadcast type = 1

    pack_bits(bits, 1, n, 1);
    n += 1; // common_flag = 1
    pack_bits(bits, 0, n, 2);
    n += 2; // access code A
    pack_bits(bits, 0, n, 4);
    n += 4; // immediate
    pack_bits(bits, 0, n, 4);
    n += 4; // wait_time
    pack_bits(bits, 0, n, 4);
    n += 4; // num_ra
    pack_bits(bits, 0, n, 1);
    n += 1; // frame_len_f
    pack_bits(bits, 0, n, 4);
    n += 4; // ts_ptr
    pack_bits(bits, 0, n, 3);
    n += 3; // min_pdu_pri
    pack_bits(bits, 0, n, 2);
    n += 2; // no optional field
    pack_bits(bits, 4, n, 3);
    n += 3; // filler bits 100

    tetra_mac_broadcast bc;
    CHECK(tetra_mac_broadcast_decode(bits, n, &bc) && bc.type == TETRA_MAC_BC_ACCESS_DEF
              && bc.access_define.assigned == 1,
          "common or assigned control channel flag");
}

int
main(void) {
    test_mac_resource_header();
    test_mac_resource_usage_marker();
    test_sysinfo_without_mle();
    test_sysinfo_cck_and_mle();
    test_sysinfo_counted();
    test_access_define_fields();
    test_access_define_assigned_flag();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
