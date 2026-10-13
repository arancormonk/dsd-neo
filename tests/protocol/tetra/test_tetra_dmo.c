// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA direct mode (EN 300 396-2/-3/-4/-5): DSB and DNB parsing, the DM colour code and the protection of the
 * current call. The SCH/S and SCH/H vectors come from a real 851.0125 MHz direct mode call (DSB, DM-OCCUPIED).
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tetra_state.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  PASS: %s\n", msg);                                                                               \
        } else {                                                                                                       \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                           \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

typedef struct {
    uint8_t bits[300];
    int at;
} bitw;

static void
bw_reset(bitw* w) {
    DSD_MEMSET(w->bits, 0, sizeof(w->bits));
    w->at = 0;
}

static void
put(bitw* w, int nbits, uint32_t value) {
    for (int i = nbits - 1; i >= 0; i--) {
        w->bits[w->at++] = (uint8_t)((value >> i) & 1u);
    }
}

static void
from_string(const char* s, uint8_t* bits) {
    for (size_t i = 0; s[i]; i++) {
        bits[i] = (uint8_t)(s[i] == '1');
    }
}

static dsd_state*
new_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static void
free_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

/* Direct DMAC-SYNC SCH/H (EN 300 396-3 table 22) up to and including the message type: dst 1, src 2, MNI 0. */
static void
sync_h_header(bitw* w, int msg) {
    bw_reset(w);
    put(w, 10, 0); /* reserved for direct operation */
    put(w, 1, 0);  /* fill bit indication */
    put(w, 1, 0);  /* not fragmented */
    put(w, 2, 0);  /* frame countdown */
    put(w, 2, 0);
    put(w, 24, 1);
    put(w, 2, 0);
    put(w, 24, 2);
    put(w, 24, 0);
    put(w, 5, (uint32_t)msg);
}

/* DM-SETUP / DM-CONNECT ACK / DM-OCCUPIED elements (tables 31, 35, 36). */
static void
call_elements(bitw* w, uint32_t prio, uint32_t e2ee, uint32_t group) {
    put(w, 3, 0);    /* timing, LCH in frame 3, pre-emption flags */
    put(w, 3, 4);    /* power class 4 */
    put(w, 1, 0);    /* power control */
    put(w, 2, 0);    /* reserved */
    put(w, 2, 0);    /* dual watch, two-frequency */
    put(w, 4, 0);    /* speech */
    put(w, 4, 0);    /* reserved */
    put(w, 2, prio); /* priority */
    put(w, 1, e2ee);
    put(w, 1, group);
    put(w, 1, 0); /* external source */
    put(w, 2, 0);
}

/* DMAC-SYNC SCH/S (table 21), direct, channel A, slot 1, frame 3, with the given air interface encryption state. */
static void
sync_s(bitw* w, uint32_t enc) {
    bw_reset(w);
    put(w, 4, 0xD);
    put(w, 2, 0);
    put(w, 2, 0);
    put(w, 2, 0);
    put(w, 2, 0);
    put(w, 2, 0);
    put(w, 5, 3);
    put(w, 2, enc);
    if (enc != 0u) {
        put(w, 29, 0x1234567u); /* time variant parameter */
        put(w, 1, 0);
        put(w, 4, 1); /* KSG number */
        put(w, 5, 7); /* SCK 8 */
    }
}

/* SCH/S of a DSB: system code 1101, DMAC-SYNC, direct MS-MS, channel A, slot 1, frame 3, no encryption. */
static const char* k_sch_s = "110100000000000001100000000000000000000000000000000000000000";
/* SCH/H of the same call: DM-OCCUPIED, src 100, dst 0x800001, MNI 753/2361. */
static const char* k_sch_h = "00000000001000001000000000000000000000010000000000000000000110010010111100010010010011100"
                             "10110101110000000000000000"
                             "001000100";

static void
test_real_air_call(void) {
    uint8_t bits[128];
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    char desc[96];
    dsd_state* st = new_state();

    printf("[real-air DM-OCCUPIED]\n");
    DSD_MEMSET(bits, 0, sizeof(bits));
    from_string(k_sch_s, bits);
    CHECK(tetra_dmo_system_code_is_dmo(bits) == 1, "system code 1101 is DMO");
    CHECK(tetra_dmo_parse_sync_s(bits, &s) == 1 && s.parsed == 1, "SCH/S parses");
    CHECK(s.system_code == 13 && s.sync_pdu_type == TETRA_DMO_SYNC_DMAC && s.comm_type == 0,
          "system code 13, DMAC-SYNC, direct");
    CHECK(s.ab_usage == 0 && s.slot == 1 && s.frame == 3 && s.enc_state == TETRA_DMO_AIE_DM1,
          "channel A, slot 1, frame 3, DM-1");

    DSD_MEMSET(bits, 0, sizeof(bits));
    bits[1] = 1; /* 0100: TMO system code, as in a real BSCH */
    CHECK(tetra_dmo_system_code_is_dmo(bits) == 0, "TMO system code is not DMO");
    CHECK(tetra_dmo_system_code_is_dmo(NULL) == 0, "no block is not DMO");

    from_string(k_sch_h, bits);
    CHECK(tetra_dmo_parse_sync_h(bits, 0, &h) == 1, "SCH/H parses");
    CHECK(h.has_addrs == 1 && h.msg_type == TETRA_DMO_MSG_OCCUPIED, "DM-OCCUPIED");
    CHECK(h.src == 100 && h.dst == 0x800001u, "source 100, destination 0x800001");
    CHECK(((h.mni >> 14) & 0x3FFu) == 753u && (h.mni & 0x3FFFu) == 2361u, "MNI 753/2361");
    CHECK(h.frame_countdown == 0 && h.frag_flag == 0 && h.fill_bit == 1, "countdown 0, no fragmentation, fill bits");
    CHECK(h.md_ok && h.sdu_state == TETRA_DMO_SDU_CLEAR && h.e[TETRA_DMO_E_CALL_TYPE] == 1
              && h.e[TETRA_DMO_E_CIRCUIT_MODE] == 0 && h.e[TETRA_DMO_E_PRIORITY] == 0
              && h.e[TETRA_DMO_E_POWER_CLASS] == 4 && h.e[TETRA_DMO_E_E2EE] == 0,
          "DM-OCCUPIED elements: group speech call, normal priority, power class 4, clear");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "group speech") != NULL,
          "element summary text");
    CHECK(strcmp(tetra_dmo_msg_name(h.msg_type), "DM-OCCUPIED") == 0, "message name");

    /* Colour code: MNI[5:0]=57, source 100. Verified on real traffic: SCH/H of NTS2 half slots decode with a valid CRC
     * only with this seed. */
    CHECK(tetra_dmo_dcc_seed(h.mni, h.src) == 0xE4000193u, "DCC seed from MNI/source address");
    CHECK(tetra_dmo_dcc_seed(0, 0) == 3u, "all-zero colour code is the DSB seed 3");

    tetra_st(st)->tdma_valid = 1;
    tetra_st(st)->hyperframe_valid = 1;
    from_string(k_sch_s, bits);
    tetra_dmo_parse_sync_s(bits, &s);
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo == 1 && tetra_st(st)->dmo_dcc_valid == 1, "DSB switches to DMO and learns the DCC");
    CHECK(tetra_st(st)->dmo_dcc_seed == 0xE4000193u && tetra_st(st)->dmo_slot == 1 && tetra_st(st)->dmo_frame == 3,
          "DMO clock and seed stored");
    CHECK(tetra_st(st)->tdma_valid == 0 && tetra_st(st)->hyperframe_valid == 0,
          "the TMO clock stops on a direct mode carrier");
    CHECK(tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_CLEAR, "a DM-1 call without end-to-end encryption is clear");

    h.msg_type = TETRA_DMO_MSG_TX_CEASED;
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 0, "DM-TX CEASED invalidates the colour code");
    CHECK(tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_UNKNOWN, "DM-TX CEASED leaves the next call unknown");
    free_state(st);
}

/* DM-2-A, DM-2-B and DM-2-C (EN 300 396-3 clauses 8.5.3 and 9.3.2): what each security class leaves readable, and how
 * the call is classified for the voice path. */
static void
test_security_classes(void) {
    bitw ws;
    bitw wh;
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    char desc[128];
    dsd_state* st = new_state();

    printf("[security classes]\n");
    sync_s(&ws, TETRA_DMO_AIE_DM2A);
    CHECK(ws.at == 60, "encrypted SCH/S fills 60 bits");
    CHECK(tetra_dmo_parse_sync_s(ws.bits, &s) == 1 && s.enc_state == TETRA_DMO_AIE_DM2A && s.tvp == 0x1234567u
              && s.ksg == 1 && s.key_number == 7,
          "DM-2-A SCH/S carries TVP, KSG number and key number");

    /* DM-2-A: addresses and message dependent elements clear, DM-SDU and traffic encrypted. The DM-SDU bits here
     * would read as an end-to-end encrypted group call if taken as clear. */
    sync_h_header(&wh, TETRA_DMO_MSG_OCCUPIED);
    call_elements(&wh, 1, 1, 1);
    tetra_dmo_parse_sync_h(wh.bits, 0, &h);
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(h.enc_state == TETRA_DMO_AIE_DM2A && h.sdu_state == TETRA_DMO_SDU_ENCRYPTED && h.e[TETRA_DMO_E_E2EE] == 0
              && h.e[TETRA_DMO_E_CALL_TYPE] == 0 && h.e[TETRA_DMO_E_PRIORITY] == 1 && h.src == 2 && h.dst == 1,
          "DM-2-A keeps addresses and priority, drops the DM-SDU");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "call speech prio=high") != NULL
              && strstr(desc, "DM-SDU encrypted") != NULL && strstr(desc, "E2EE") == NULL,
          "DM-2-A summary names no DM-SDU element");
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == tetra_dmo_dcc_seed(0, 2),
          "DM-2-A scrambles with the clear source address");
    CHECK(tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_AIR && tetra_st(st)->dmo_security_class == TETRA_DMO_AIE_DM2A,
          "DM-2-A call is air interface encrypted");

    /* DM-2-B: the destination address is encrypted too. */
    sync_s(&ws, TETRA_DMO_AIE_DM2B);
    tetra_dmo_parse_sync_s(ws.bits, &s);
    tetra_dmo_parse_sync_h(wh.bits, 0, &h);
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(h.dst == 0 && h.dst_encrypted == 1 && h.src == 2 && h.has_addrs == 1, "DM-2-B hides the destination only");
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_AIR
              && tetra_st(st)->dmo_security_class == TETRA_DMO_AIE_DM2B,
          "DM-2-B call learns its colour code and is air interface encrypted");

    /* DM-2-C: everything after the address types is encrypted, and the colour code needs the encrypted identity. */
    sync_s(&ws, TETRA_DMO_AIE_DM2C);
    tetra_dmo_parse_sync_s(ws.bits, &s);
    tetra_dmo_parse_sync_h(wh.bits, 0, &h);
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(h.has_addrs == 0 && h.msg_type == 0 && h.src == 0 && h.dst == 0 && h.md_ok == 0
              && h.sdu_state == TETRA_DMO_SDU_ENCRYPTED,
          "DM-2-C leaves no address, message type or element");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) == 0, "DM-2-C has no element summary");
    CHECK(tetra_st(st)->dmo_dcc_valid == 0 && tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_AIR
              && tetra_st(st)->dmo_security_class == TETRA_DMO_AIE_DM2C,
          "DM-2-C call has no colour code and is air interface encrypted");

    /* DM-1 with the end-to-end encryption flag. */
    sync_s(&ws, TETRA_DMO_AIE_DM1);
    tetra_dmo_parse_sync_s(ws.bits, &s);
    sync_h_header(&wh, TETRA_DMO_MSG_SETUP);
    call_elements(&wh, 0, 1, 0);
    tetra_dmo_parse_sync_h(wh.bits, 0, &h);
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_E2E && tetra_st(st)->dmo_dcc_valid == 1,
          "DM-1 call with the E2EE flag is end-to-end encrypted");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "individual speech") != NULL
              && strstr(desc, "E2EE") != NULL,
          "end-to-end encrypted DM-SETUP summary");

    tetra_st(st)->dmo = 0;
    CHECK(tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_UNKNOWN, "outside direct mode the call protection is unknown");
    CHECK(tetra_dmo_call_crypt(NULL) == TETRA_DMO_CRYPT_UNKNOWN, "no state, no call protection");
    free_state(st);
}

/* A DM-REP presence signal has its own SCH/S layout (EN 300 396-4 table 2). Read with the DMAC-SYNC layout, this one
 * gave channel A frequency efficient and air interface encryption state DM-2-A. */
static void
build_rep_presence(bitw* ps, bitw* ph) {
    bw_reset(ps);
    put(ps, 4, 0xD); /* system code 1101 */
    put(ps, 2, 1);   /* DPRES-SYNC */
    put(ps, 2, 1);   /* via DM-REP */
    put(ps, 1, 0);   /* not managed direct mode */
    put(ps, 2, 0);
    put(ps, 1, 1); /* two-frequency */
    put(ps, 2, 0); /* type 1 operating mode */
    put(ps, 5, 8); /* 45 MHz */
    put(ps, 1, 1); /* uplink = downlink - spacing */
    put(ps, 1, 0); /* slave link */
    put(ps, 2, 0); /* channel usage: the carrier */
    put(ps, 2, 1); /* occupied */
    put(ps, 2, 0); /* slot 1 */
    put(ps, 5, 3); /* frame 3 */
    put(ps, 3, 4); /* power class 4 */
    put(ps, 1, 0);
    put(ps, 1, 0);
    put(ps, 2, 0); /* countdown */
    put(ps, 2, 2); /* priority pre-emptive */
    ps->at = 60;
    bw_reset(ph);
    put(ph, 10, 17);       /* repeater address */
    put(ph, 24, 0xBC4939); /* MNI 753/2361 */
    put(ph, 2, 3);         /* validity not restricted */
    put(ph, 6, 0);
    put(ph, 3, 4);
    put(ph, 1, 0);
    put(ph, 4, 0); /* open repeater */
}

static void
test_repeater_presence(void) {
    bitw ps;
    bitw ph;
    tetra_dmo_sync_s_t s;
    tetra_dmo_pres_t pres;
    char desc[128];
    char mode[96];
    dsd_state* st = new_state();

    printf("[DM-REP presence signal]\n");
    tetra_st(st)->dmo = 1;
    tetra_st(st)->dmo_dcc_valid = 1;
    tetra_st(st)->dmo_dcc_seed = 0xE4000193u;
    tetra_st(st)->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    build_rep_presence(&ps, &ph);
    CHECK(tetra_dmo_parse_sync_s(ps.bits, &s) == 1 && s.sync_pdu_type == TETRA_DMO_SYNC_PRES && s.comm_type == 1,
          "presence SCH/S header");
    CHECK(s.slot == 1 && s.frame == 3 && s.ab_usage == 0 && s.enc_state == TETRA_DMO_AIE_DM1 && s.ms_link_flag == 0,
          "presence SCH/S read with its own layout: carrier, slot 1, frame 3, no encryption state");
    tetra_dmo_apply_sync(st, &s, NULL);
    CHECK(tetra_st(st)->dmo_ab_usage == 0 && tetra_dmo_call_crypt(st) == TETRA_DMO_CRYPT_CLEAR,
          "presence signal does not set channel B-style usage or encryption");
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "DMO repeater  ch A") != NULL
              && strstr(mode, "encrypted") == NULL,
          "working mode after a presence signal");
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 0xE4000193u,
          "presence signal does not replace the call colour code");

    CHECK(tetra_dmo_parse_presence(ps.bits, ph.bits, &s, &pres) == 1 && pres.parsed == 1, "repeater presence parses");
    CHECK(pres.ms_link == 0 && pres.channel_state == 1 && pres.priority == 2 && pres.power_class == 4,
          "slave link, occupied, pre-emptive priority");
    CHECK(pres.repeater == 17 && pres.mni == 0xBC4939u && pres.two_freq == 1 && pres.spacing == 8 && pres.reverse == 1,
          "repeater address, MNI and 45 MHz reverse duplex");
    CHECK(tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc)) > 0 && strstr(desc, "rep=17") != NULL
              && strstr(desc, "occupied") != NULL && strstr(desc, "uplink=-45.000") != NULL,
          "presence summary names the uplink offset");
    CHECK(tetra_dmo_describe_presence(&pres, desc, 8) == 7 && strlen(desc) == 7,
          "presence summary is truncated safely");
    CHECK(tetra_dmo_parse_presence(ps.bits, NULL, &s, &pres) == 1 && pres.parsed == 0,
          "presence without SCH/H keeps the SCH/S part");

    /* Managed direct mode presence (communication type 00) is not read. */
    ps.bits[6] = 0;
    ps.bits[7] = 0;
    CHECK(tetra_dmo_parse_sync_s(ps.bits, &s) == 0 && s.parsed == 0, "managed direct mode presence is not parsed");
    CHECK(tetra_dmo_parse_presence(ps.bits, ph.bits, &s, &pres) == 0, "nor is its presence signal");
    free_state(st);
}

static void
test_gateway(void) {
    bitw w;
    bitw ph;
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    tetra_dmo_pres_t pres;
    char desc[160];
    dsd_state* st = new_state();

    printf("[gateway]\n");
    /* The colour code is the low 6 bits of the gateway address plus the source address, not the MNI. The MNI is
     * present here because both addresses are true SSIs. */
    bw_reset(&w);
    put(&w, 10, 0x155);
    put(&w, 4, 0);
    put(&w, 2, 0);
    put(&w, 24, 0x800001);
    put(&w, 2, 0);
    put(&w, 24, 100);
    put(&w, 24, 0xBC4939);
    put(&w, 5, TETRA_DMO_MSG_OCCUPIED);
    call_elements(&w, 0, 0, 1);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.msg_type == TETRA_DMO_MSG_OCCUPIED && h.has_addrs == 1
              && h.relay_addr == 0x155 && h.mni == 0xBC4939u && h.src == 100,
          "gateway DMAC-SYNC keeps the message type after the MNI");
    DSD_MEMSET(&s, 0, sizeof(s));
    s.parsed = 1;
    s.comm_type = 2;
    CHECK(tetra_dmo_dcc_seed(0x155u, 100u) == 0x54000193u, "gateway colour code ignores the MNI");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 0x54000193u,
          "gateway DM-OCCUPIED learns the gateway colour code");

    /* Both addresses pseudo: no MNI, and the message type follows the source. */
    bw_reset(&w);
    put(&w, 10, 0x155);
    put(&w, 4, 0);
    put(&w, 2, 1);
    put(&w, 24, 0x800001);
    put(&w, 2, 1);
    put(&w, 24, 100);
    put(&w, 5, TETRA_DMO_MSG_OCCUPIED);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.msg_type == TETRA_DMO_MSG_OCCUPIED && h.mni == 0,
          "gateway DMAC-SYNC without a true SSI has no MNI");

    /* Gateway presence, communication type 10, SwMI available, channel occupied (EN 300 396-5 tables 5 and 6). */
    bw_reset(&w);
    put(&w, 4, 0xD);
    put(&w, 2, 1);
    put(&w, 2, 2);
    put(&w, 1, 0);
    put(&w, 1, 1); /* SwMI available */
    put(&w, 1, 0); /* no DM-REP function */
    put(&w, 1, 1); /* reserved in place of the two-frequency flag: ignored */
    put(&w, 2, 0);
    put(&w, 6, 0x11);
    put(&w, 1, 0);
    put(&w, 2, 0);
    put(&w, 2, 1); /* occupied */
    put(&w, 2, 0);
    put(&w, 5, 3);
    put(&w, 3, 4);
    put(&w, 1, 0);
    put(&w, 1, 0);
    put(&w, 2, 0);
    put(&w, 2, 1); /* priority */
    put(&w, 2, 0);
    put(&w, 4, 9); /* registration label */
    put(&w, 4, 0);
    put(&w, 3, 0);
    put(&w, 1, 0);
    put(&w, 1, 1); /* gateway encryption on the SwMI */
    put(&w, 1, 0);
    put(&w, 3, 0);
    CHECK(w.at == 60, "gateway presence SCH/S fills 60 bits");
    bw_reset(&ph);
    put(&ph, 10, 17);
    put(&ph, 24, 0xBC4939);
    put(&ph, 2, 0);
    put(&ph, 6, 1);
    put(&ph, 3, 4);
    put(&ph, 1, 0);
    put(&ph, 4, 1);
    tetra_dmo_parse_sync_s(w.bits, &s);
    CHECK(tetra_dmo_parse_presence(w.bits, ph.bits, &s, &pres) == 1, "gateway presence parses");
    CHECK(s.slot == 1 && s.frame == 3 && pres.channel_state == 1 && pres.swmi_up == 1 && pres.dmrep == 0
              && pres.repeater == 17 && pres.mni == 0xBC4939u && pres.priority == 1 && pres.two_freq == 0
              && pres.spacing == 0 && pres.reg_label == 9 && pres.gw_enc == 1 && pres.urt == 1,
          "gateway presence clock, SwMI, label and address");
    CHECK(tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc)) > 0 && strstr(desc, "gw=17") != NULL
              && strstr(desc, "swmi=up") != NULL && strstr(desc, "occupied") != NULL && strstr(desc, "uplink") == NULL,
          "gateway presence summary");
    free_state(st);
}

/* Presence-check, changeover and pre-emption PDUs (EN 300 396-3 tables 32-34 and 39-44). None of these learn a
 * colour code; DM-DISCONNECT drops the one already held. */
static void
test_call_control_elements(void) {
    bitw w;
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    char desc[160];
    dsd_state* st = new_state();

    printf("[call control elements]\n");
    DSD_MEMSET(&s, 0, sizeof(s));
    s.parsed = 1;
    tetra_st(st)->dmo_dcc_valid = 1;
    tetra_st(st)->dmo_dcc_seed = 0x1234u;

    sync_h_header(&w, TETRA_DMO_MSG_SETUP_PRES);
    put(&w, 3, 0);
    put(&w, 3, 4);
    put(&w, 1, 0);
    put(&w, 2, 0);
    put(&w, 2, 0);
    put(&w, 4, 0);
    put(&w, 4, 0);
    put(&w, 2, 1);
    put(&w, 3, 0);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.md_ok && h.e[TETRA_DMO_E_POWER_CLASS] == 4
              && h.e[TETRA_DMO_E_PRIORITY] == 1 && h.e[TETRA_DMO_E_CALL_TYPE] == 0
              && h.e[TETRA_DMO_E_PREEMPT_FLAG] == 0,
          "DM-SETUP PRES elements");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "individual speech") != NULL
              && strstr(desc, "prio=high") != NULL,
          "DM-SETUP PRES summary");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 0x1234u,
          "DM-SETUP PRES does not replace the colour code");

    sync_h_header(&w, TETRA_DMO_MSG_CONNECT);
    put(&w, 4, 8);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.md_ok && h.e[TETRA_DMO_E_CIRCUIT_MODE] == 8,
          "DM-CONNECT circuit mode");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "accept prop-speech") != NULL
              && strstr(desc, "prio=") == NULL,
          "DM-CONNECT summary");

    sync_h_header(&w, TETRA_DMO_MSG_DISCONNECT);
    put(&w, 4, 3);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_DISCONNECT_CAUSE] == 3, "DM-DISCONNECT cause");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "rejected by called party") != NULL,
          "DM-DISCONNECT summary");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 0, "DM-DISCONNECT invalidates the colour code");

    sync_h_header(&w, TETRA_DMO_MSG_TX_REQUEST);
    put(&w, 1, 1);
    put(&w, 12, 2);
    put(&w, 2, 3);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_TIMING_CHANGE] == 1
              && h.e[TETRA_DMO_E_TIMING_ADJUST] == 2 && h.e[TETRA_DMO_E_PRIORITY] == 3,
          "DM-TX REQUEST keeps priority after the timing adjustment");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "prio=emergency") != NULL
              && strstr(desc, "timing=+10 symbols") != NULL,
          "DM-TX REQUEST summary");

    sync_h_header(&w, TETRA_DMO_MSG_TX_ACCEPT);
    put(&w, 1, 0);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.md_ok && h.e[TETRA_DMO_E_TIMING_CHANGE] == 0,
          "DM-TX ACCEPT without a timing change");

    sync_h_header(&w, TETRA_DMO_MSG_PREEMPT);
    put(&w, 2, 1);
    put(&w, 1, 0);
    put(&w, 1, 1);
    put(&w, 4, 0);
    put(&w, 2, 2);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_CHANNEL_STATE] == 1
              && h.e[TETRA_DMO_E_NEW_CALL] == 1 && h.e[TETRA_DMO_E_PREEMPT_TYPE] == 0 && h.e[TETRA_DMO_E_PRIORITY] == 2,
          "DM-PREEMPT elements");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
              && strstr(desc, "preempt new-call reservation circuit") != NULL,
          "DM-PREEMPT summary");

    sync_h_header(&w, TETRA_DMO_MSG_PRE_ACCEPT);
    put(&w, 1, 0);
    put(&w, 1, 0);
    put(&w, 4, 1);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_NEW_CALL] == 0
              && h.e[TETRA_DMO_E_PREEMPT_TYPE] == 1,
          "DM-PRE ACCEPT elements");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
              && strstr(desc, "preempt accepted ongoing short data") != NULL,
          "DM-PRE ACCEPT summary");

    sync_h_header(&w, TETRA_DMO_MSG_REJECT);
    put(&w, 4, 2);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_REJECT_CAUSE] == 2, "DM-REJECT cause");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "master will transmit") != NULL,
          "DM-REJECT summary");

    /* No layer-2 addresses, so the 48-bit calling party TSI fits in the SCH/H block. */
    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 24, 0);
    put(&w, 5, TETRA_DMO_MSG_INFO);
    put(&w, 3, 0);
    put(&w, 24, 0xBC4939);
    put(&w, 24, 100);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_CALLING_SSI] == 100
              && h.e[TETRA_DMO_E_CALLING_MNI] == 0xBC4939u,
          "DM-INFO TSI");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "753/2361") != NULL
              && strstr(desc, "ssi=100") != NULL,
          "DM-INFO summary");

    sync_h_header(&w, TETRA_DMO_MSG_RELEASE);
    put(&w, 4, 15);
    put(&w, 5, 0);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_RELEASE_CAUSE] == 15
              && h.e[TETRA_DMO_E_RELEASE_EXT] == 0 && h.sdu_state == TETRA_DMO_SDU_CLEAR,
          "DM-RELEASE extension");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "occupied by other users") != NULL,
          "DM-RELEASE extension summary");

    /* DM-TX CEASED with both conditional elements does not fit the SCH/H block after a direct address header: the
     * priority is cut, so no summary is made of it. */
    sync_h_header(&w, TETRA_DMO_MSG_TX_CEASED);
    put(&w, 6, 1);    /* reservation time remaining */
    put(&w, 1, 0);    /* timing flag */
    put(&w, 1, 1);    /* requests flag */
    put(&w, 1, 0);    /* changeover requests flag */
    put(&w, 8, 0xFF); /* requests bitmap */
    put(&w, 1, 0);    /* recent user priority flag */
    put(&w, 1, 1);    /* timing change announced: the 12-bit adjustment runs past the block */
    tetra_dmo_parse_sync_h(w.bits, 0, &h);
    CHECK(h.has_addrs == 1 && h.md_ok == 0 && tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) == 0,
          "a cut DM-TX CEASED reports no elements");
    free_state(st);
}

/* Gateway-specific PDUs (message type 25, EN 300 396-5 tables 9 to 22). A fragmented DM-GSETUP learns the gateway
 * colour code; DM-GRELEASE drops it. An unfragmented one does not. */
static void
gw_header(bitw* w, uint32_t frag, uint32_t sub) {
    bw_reset(w);
    put(w, 10, 0x155);
    put(w, 1, 0);
    put(w, 1, frag);
    if (frag) {
        put(w, 4, 1);
    }
    put(w, 2, 0);
    put(w, 2, 2); /* no destination address */
    put(w, 2, 1); /* pseudo source, so the MNI is absent */
    put(w, 24, 100);
    put(w, 5, TETRA_DMO_MSG_GATEWAY);
    put(w, 5, sub);
}

static void
test_gateway_messages(void) {
    bitw w;
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    char desc[160];
    dsd_state* st = new_state();

    printf("[gateway messages]\n");
    DSD_MEMSET(&s, 0, sizeof(s));
    s.parsed = 1;
    s.comm_type = 2;
    tetra_st(st)->dmo_dcc_valid = 1;
    tetra_st(st)->dmo_dcc_seed = 1;

    gw_header(&w, 0, TETRA_DMO_GW_SETUP);
    put(&w, 4, 0);
    put(&w, 4, 0);
    put(&w, 2, 1);
    put(&w, 1, 0);
    put(&w, 1, 1);
    put(&w, 2, 0);
    put(&w, 4, 7);
    put(&w, 1, 1); /* called party TSI */
    put(&w, 24, 50);
    put(&w, 24, 0xBC4939);
    put(&w, 4, 15);
    put(&w, 1, 0);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.e[TETRA_DMO_E_GW_SUBTYPE] == TETRA_DMO_GW_SETUP
              && h.e[TETRA_DMO_E_CALLED_SSI] == 50 && h.e[TETRA_DMO_E_CALLED_MNI] == 0xBC4939u
              && h.e[TETRA_DMO_E_REQUEST_LABEL] == 7 && h.e[TETRA_DMO_E_AREA] == 15 && h.e[TETRA_DMO_E_CALL_TYPE] == 1,
          "DM-GSETUP called party TSI");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "DM-GSETUP group speech") != NULL
              && strstr(desc, "label=7") != NULL && strstr(desc, "MNI=753/2361") != NULL
              && strstr(desc, "area=all") != NULL,
          "DM-GSETUP summary");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 1,
          "unfragmented DM-GSETUP does not replace the colour code");

    gw_header(&w, 1, TETRA_DMO_GW_SETUP);
    put(&w, 10, 0);
    put(&w, 1, 0);
    put(&w, 1, 1);
    put(&w, 2, 0);
    put(&w, 4, 1);
    put(&w, 1, 0); /* SSI only */
    put(&w, 24, 50);
    put(&w, 4, 0);
    put(&w, 1, 0);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.frag_flag == 1 && h.e[TETRA_DMO_E_CALLED_TSI_FLAG] == 0
              && h.e[TETRA_DMO_E_CALLED_SSI] == 50,
          "fragmented DM-GSETUP");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 0x54000193u,
          "fragmented DM-GSETUP learns the gateway colour code");

    gw_header(&w, 0, TETRA_DMO_GW_RELEASE);
    put(&w, 4, 1);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1, "DM-GRELEASE parses");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "TMO resource lost") != NULL,
          "DM-GRELEASE summary");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 0, "DM-GRELEASE invalidates the colour code");

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 5, TETRA_DMO_MSG_GATEWAY);
    put(&w, 5, TETRA_DMO_GW_ACK);
    put(&w, 1, 1);
    put(&w, 12, 2);
    put(&w, 1, 0);
    put(&w, 4, 0);
    put(&w, 2, 0);
    put(&w, 3, 4);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.e[TETRA_DMO_E_TIMING_ADJUST] == 2
              && h.e[TETRA_DMO_E_WAIT_CODE] == 4 && h.e[TETRA_DMO_E_RESERVATION_ANNOUNCED] == 0,
          "DM-GACK timing and waiting timer");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "wait=10s") != NULL
              && strstr(desc, "timing=+10 symbols") != NULL,
          "DM-GACK summary");

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 5, TETRA_DMO_MSG_GATEWAY);
    put(&w, 5, TETRA_DMO_GW_REG_REJECT);
    put(&w, 1, 1);
    put(&w, 2, 0);
    put(&w, 4, 3);
    put(&w, 4, 5);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.e[TETRA_DMO_E_REG_LABEL] == 3
              && h.e[TETRA_DMO_E_REJECT_CAUSE] == 5,
          "DM-GREGISTER REJECT");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "label=3") != NULL
              && strstr(desc, "group unsupported") != NULL && strstr(desc, "unsolicited") != NULL,
          "DM-GREGISTER REJECT summary");

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 5, TETRA_DMO_MSG_GATEWAY);
    put(&w, 5, 31);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.md_ok == 0 && tetra_dmo_describe_elems(&h, desc, 160) == 0,
          "unknown gateway subtype is left unparsed");
    free_state(st);
}

/* Short data, an external number and a timing request. None of these replace a colour code unless the short data
 * burst itself is fragmented. */
static void
test_short_data(void) {
    bitw w;
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    char desc[160];
    dsd_state* st = new_state();

    printf("[short data]\n");
    DSD_MEMSET(&s, 0, sizeof(s));
    s.parsed = 1;
    tetra_st(st)->dmo_dcc_valid = 1;
    tetra_st(st)->dmo_dcc_seed = 0xE4000193u;

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 5, TETRA_DMO_MSG_SDS_UDATA);
    put(&w, 4, 3); /* time remaining */
    put(&w, 1, 0);
    put(&w, 2, 1); /* high priority */
    put(&w, 1, 1); /* FCS present */
    put(&w, 1, 1); /* additional addressing */
    put(&w, 4, 4); /* external number only */
    put(&w, 5, 4);
    put(&w, 4, 10); /* * */
    put(&w, 4, 1);
    put(&w, 4, 2);
    put(&w, 4, 3);
    put(&w, 4, 4); /* pre-coded status */
    put(&w, 16, 0x1234);
    put(&w, 32, 0xA1B2C3D4u);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && h.external_n == 4 && strcmp(h.external, "*123") == 0
              && h.e[TETRA_DMO_E_SDTI] == 4 && h.sds_value == 0x1234u && h.e[TETRA_DMO_E_FCS] == 0xA1B2C3D4u
              && h.sdu_state == TETRA_DMO_SDU_CLEAR,
          "DM-SDS UDATA external number and status");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "status=0x1234") != NULL
              && strstr(desc, "ext=*123") != NULL && strstr(desc, "fcs=0xa1b2c3d4") != NULL,
          "DM-SDS summary");
    st->tetra_message_sequence = 0;
    tetra_dmo_publish(st, h.msg_type, h.src, h.dst, desc);
    tetra_dmo_publish(st, h.msg_type, h.src, h.dst, desc);
    CHECK(st->tetra_message_sequence == 1 && st->tetra_messages[0].category == DSD_TETRA_MESSAGE_SDS
              && strstr(st->tetra_messages[0].text, "status=0x1234") != NULL
              && strstr(st->tetra_messages[0].text, "ext=*123") != NULL,
          "DM-SDS is queued once for the terminal");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 0xE4000193u,
          "unfragmented DM-SDS UDATA keeps the colour code");

    /* A reserved SDTI has no known length: the rest of the DM-SDU cannot be read. */
    w.bits[w.at - 32 - 16 - 1] = 1; /* SDTI 0100 becomes 0101 */
    tetra_dmo_parse_sync_h(w.bits, 2, &h);
    CHECK(h.e[TETRA_DMO_E_SDTI] == 5 && h.sdu_state == TETRA_DMO_SDU_TRUNCATED && h.e[TETRA_DMO_E_FCS] == 0,
          "reserved SDTI stops the DM-SDU");

    /* Fragmented short data: SCH/F fragments use the colour code of the DMAC-SYNC that started them. */
    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 1, 0);
    put(&w, 1, 1); /* start of fragmentation */
    put(&w, 4, 1); /* one SCH/F slot follows */
    put(&w, 2, 0);
    put(&w, 2, 0);
    put(&w, 24, 1);
    put(&w, 2, 0);
    put(&w, 24, 2);
    put(&w, 24, 0);
    put(&w, 5, TETRA_DMO_MSG_SDS_DATA);
    tetra_dmo_parse_sync_h(w.bits, 0, &h);
    tetra_st(st)->dmo_dcc_valid = 0;
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(h.frag_flag == 1 && tetra_st(st)->dmo_dcc_valid == 1, "fragmented DM-SDS DATA learns the colour code");

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 24, 0);
    put(&w, 5, TETRA_DMO_MSG_TIMING_REQ);
    put(&w, 12, 2);
    tetra_st(st)->dmo_dcc_seed = 7;
    CHECK(tetra_dmo_parse_sync_h(w.bits, 0, &h) == 1 && h.e[TETRA_DMO_E_TIMING_ADJUST] == 2,
          "DM-TIMING REQUEST adjustment");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "timing request") != NULL
              && strstr(desc, "timing=+10 symbols") != NULL,
          "DM-TIMING REQUEST summary");
    tetra_dmo_apply_sync(st, &s, &h);
    CHECK(tetra_st(st)->dmo_dcc_valid == 1 && tetra_st(st)->dmo_dcc_seed == 7,
          "DM-TIMING REQUEST does not replace the colour code");

    bw_reset(&w);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 2, 2);
    put(&w, 2, 2);
    put(&w, 5, TETRA_DMO_MSG_GATEWAY);
    put(&w, 5, TETRA_DMO_GW_SETUP);
    put(&w, 10, 0);
    put(&w, 4, 0);
    put(&w, 4, 1);
    put(&w, 1, 0);
    put(&w, 24, 9);
    put(&w, 4, 0);
    put(&w, 1, 1);
    put(&w, 4, 4);
    put(&w, 5, 4);
    put(&w, 4, 10);
    put(&w, 4, 1);
    put(&w, 4, 2);
    put(&w, 4, 3);
    CHECK(tetra_dmo_parse_sync_h(w.bits, 2, &h) == 1 && strcmp(h.external, "*123") == 0
              && h.sdu_state == TETRA_DMO_SDU_CLEAR,
          "DM-GSETUP external number");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "ext=*123") != NULL,
          "DM-GSETUP external-number summary");
    free_state(st);
}

/* DMAC-DATA (EN 300 396-3 table 23) carries its own air interface encryption state. */
static void
test_dmac_data(void) {
    bitw w;
    tetra_dmo_sync_h_t h;
    char desc[160];
    dsd_state* st = new_state();

    printf("[DMAC-DATA]\n");
    bw_reset(&w);
    put(&w, 2, 0); /* DMAC-DATA */
    put(&w, 4, 0); /* fill, second half slot stolen, fragmentation, null PDU */
    put(&w, 2, 1); /* countdown */
    put(&w, 2, TETRA_DMO_AIE_DM1);
    put(&w, 2, 0);
    put(&w, 24, 1);
    put(&w, 2, 0);
    put(&w, 24, 2);
    put(&w, 24, 0xBC4939);
    put(&w, 5, TETRA_DMO_MSG_TX_CEASED);
    put(&w, 6, 2);
    put(&w, 4, 0);
    put(&w, 1, 0);
    put(&w, 2, 1);
    put(&w, 4, 1); /* normal end of transmission */
    CHECK(tetra_dmo_parse_data(w.bits, 124, 0, &h) == 1 && h.has_addrs && h.msg_type == TETRA_DMO_MSG_TX_CEASED
              && h.frame_countdown == 1 && h.src == 2 && h.dst == 1 && h.e[TETRA_DMO_E_RESERVATION] == 2
              && h.e[TETRA_DMO_E_CEASE_CAUSE] == 1,
          "clear DMAC-DATA DM-TX CEASED");
    CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "reserve=12 frames") != NULL
              && strstr(desc, "cause=normal end") != NULL,
          "DM-TX CEASED summary");

    w.bits[8] = 1; /* air interface encryption state DM-2-B */
    w.bits[9] = 1;
    CHECK(tetra_dmo_parse_data(w.bits, 124, 0, &h) == 1 && h.enc_state == TETRA_DMO_AIE_DM2B && h.dst == 0
              && h.dst_encrypted && h.src == 2 && h.md_ok && h.e[TETRA_DMO_E_RESERVATION] == 2
              && h.e[TETRA_DMO_E_CEASE_CAUSE] == 0 && h.sdu_state == TETRA_DMO_SDU_ENCRYPTED,
          "DM-2-B DMAC-DATA: destination and DM-SDU encrypted");

    w.bits[8] = 0; /* DM-2-C */
    CHECK(tetra_dmo_parse_data(w.bits, 124, 0, &h) == 1 && h.enc_state == TETRA_DMO_AIE_DM2C && !h.has_addrs
              && h.src == 0 && h.sdu_state == TETRA_DMO_SDU_ENCRYPTED,
          "DM-2-C DMAC-DATA is encrypted from the destination address type on");

    w.bits[5] = 1; /* null PDU */
    CHECK(tetra_dmo_parse_data(w.bits, 124, 0, &h) == 1 && h.null_pdu && !h.has_addrs, "Null PDU");
    w.bits[1] = 1; /* DMAC-FRAG/END */
    CHECK(tetra_dmo_parse_data(w.bits, 124, 0, &h) == 0, "DMAC-FRAG is not DMAC-DATA");

    CHECK(tetra_dmo_log_dnb_pdu(w.bits, 124, "STCH-1", st) == 1, "DMAC-FRAG/END type from the DNB logger");
    w.bits[0] = 1;
    CHECK(tetra_dmo_log_dnb_pdu(w.bits, 124, "STCH-1", st) == 3, "DMAC-U-SIGNAL type from the DNB logger");
    CHECK(tetra_dmo_log_dnb_pdu(w.bits, 3, "STCH-1", st) == -1, "a block too short for a MAC header");
    CHECK(tetra_dmo_log_dnb_pdu(NULL, 124, "STCH-1", st) == -1, "no block");
    free_state(st);
}

static void
test_names_and_mode(void) {
    char mode[96];
    dsd_state* st = new_state();

    printf("[names and working mode]\n");
    CHECK(strcmp(tetra_dmo_msg_name(TETRA_DMO_MSG_GATEWAY), "GATEWAY-SPECIFIC") == 0, "gateway message name");
    CHECK(strcmp(tetra_dmo_msg_name(4), "reserved") == 0 && strcmp(tetra_dmo_msg_name(30), "proprietary") == 0
              && strcmp(tetra_dmo_msg_name(-1), "reserved") == 0 && strcmp(tetra_dmo_msg_name(99), "reserved") == 0,
          "reserved and proprietary message names");

    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strcmp(mode, "TMO downlink") == 0,
          "default working mode is TMO downlink");
    tetra_st(st)->net_known = 1;
    tetra_st(st)->mcc = 86;
    tetra_st(st)->mnc = 28;
    tetra_st(st)->colour = 0x19;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "TMO downlink") != NULL
              && strstr(mode, "MCC=86") != NULL && strstr(mode, "CC=0x19") != NULL,
          "TMO downlink names the network");
    tetra_air_ksg_remember(st, 86, 28, 1);
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "TEA") == NULL
              && strstr(mode, "encrypted") == NULL,
          "a known KSG is not shown while the call is clear");
    tetra_st(st)->enc_mode = 1;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "encrypted TEA2") != NULL,
          "encrypted trunked call names the network KSG");
    tetra_st(st)->mnc = 29;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "encrypted") != NULL
              && strstr(mode, "TEA") == NULL,
          "encrypted trunked call without a KSG stays unnamed");
    tetra_st(st)->enc_mode = 0;
    tetra_st(st)->uplink = 1;
    st->tetra_sync.cb = 1;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "TMO uplink control") != NULL,
          "TMO uplink control working mode");
    CHECK(tetra_dmo_format_mode(st, mode, 6) == 5 && strcmp(mode, "TMO u") == 0, "working mode is truncated safely");
    CHECK(tetra_dmo_format_mode(st, mode, 1) == 0 && mode[0] == '\0', "one-byte buffer holds an empty line");
    CHECK(tetra_dmo_format_mode(st, NULL, 10) == 0 && tetra_dmo_format_mode(NULL, mode, 10) == 0,
          "no buffer, no state");

    tetra_st(st)->dmo = 1;
    tetra_st(st)->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    tetra_st(st)->dmo_slot = 1;
    tetra_st(st)->dmo_frame = 3;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "DMO direct  ch A  clear") != NULL
              && strstr(mode, "slot 1 frame 3") != NULL && strstr(mode, "TEA") == NULL,
          "DMO direct working mode");
    tetra_st(st)->dmo_comm_type = 2;
    tetra_st(st)->dmo_ab_usage = 2;
    tetra_st(st)->dmo_crypt = TETRA_DMO_CRYPT_AIR;
    tetra_st(st)->dmo_security_class = TETRA_DMO_AIE_DM2C;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "DMO gateway  ch B") != NULL
              && strstr(mode, "DM-2-C encrypted") != NULL,
          "DMO gateway working mode");
    tetra_st(st)->dmo_crypt = TETRA_DMO_CRYPT_E2E;
    CHECK(tetra_dmo_format_mode(st, mode, (int)sizeof(mode)) > 0 && strstr(mode, "end-to-end encrypted") != NULL,
          "DMO end-to-end encrypted working mode");
    free_state(st);
}

int
main(void) {
    printf("TETRA DMO\n");
    test_real_air_call();
    test_security_classes();
    test_repeater_presence();
    test_gateway();
    test_call_control_elements();
    test_gateway_messages();
    test_short_data();
    test_dmac_data();
    test_names_and_mode();
    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
