// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA DMO (EN 300 396-2/-3) helper tests using vectors decoded from a real
 * 851.0125 MHz direct-mode call (SCH/S + SCH/H of a DSB, DM-OCCUPIED).
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>

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

static void
put_bits(uint8_t* dst, int* at, int nbits, unsigned value) {
    for (int i = nbits - 1; i >= 0; i--) {
        dst[(*at)++] = (uint8_t)((value >> i) & 1u);
    }
}

/* Direct-mode DMAC-SYNC SCH/H up to and including the message type. */
static int
sync_h_header(uint8_t* gh, int msg) {
    int at = 0;
    memset(gh, 0, 124);
    put_bits(gh, &at, 10, 0);
    put_bits(gh, &at, 1, 0);
    put_bits(gh, &at, 1, 0);
    put_bits(gh, &at, 2, 0);
    put_bits(gh, &at, 2, 0);
    put_bits(gh, &at, 24, 1);
    put_bits(gh, &at, 2, 0);
    put_bits(gh, &at, 24, 2);
    put_bits(gh, &at, 24, 0);
    put_bits(gh, &at, 5, (unsigned)msg);
    return at;
}

static void
from_string(const char* s, uint8_t* bits) {
    for (size_t i = 0; s[i]; i++) {
        bits[i] = (uint8_t)(s[i] == '1');
    }
}

/* SCH/S of a DSB: system code 1101, DMAC-SYNC, direct MS-MS, channel A, slot 1, frame 3, no encryption. */
static const char* k_sch_s = "110100000000000001100000000000000000000000000000000000000000";
/* SCH/H of the same call: DM-OCCUPIED, src 100, dst 0x800001, MNI 753/2361. */
static const char* k_sch_h = "00000000001000001000000000000000000000010000000000000000000110010010111100010010010011100"
                             "10110101110000000000000000"
                             "001000100";

int
main(void) {
    uint8_t bits[128];
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;
    static dsd_state state;

    printf("TETRA DMO helpers\n");

    from_string(k_sch_s, bits);
    CHECK(tetra_dmo_system_code_is_dmo(bits) == 1, "system code 1101 is DMO");
    CHECK(tetra_dmo_parse_sync_s(bits, &s) == 1, "SCH/S parses");
    CHECK(s.system_code == 13 && s.sync_pdu_type == 0 && s.comm_type == 0, "system code 13, DMAC-SYNC, direct");
    CHECK(s.ab_usage == 0 && s.slot == 1 && s.frame == 3 && s.enc_state == 0, "channel A, slot 1, frame 3, clear");

    memset(bits, 0, sizeof(bits));
    bits[1] = 1; /* 0100: TMO system code, as in a real BSCH */
    CHECK(tetra_dmo_system_code_is_dmo(bits) == 0, "TMO system code is not DMO");

    from_string(k_sch_h, bits);
    CHECK(tetra_dmo_parse_sync_h(bits, 0, &h) == 1, "SCH/H parses");
    CHECK(h.has_addrs == 1 && h.msg_type == TETRA_DMO_MSG_OCCUPIED, "DM-OCCUPIED");
    CHECK(h.src == 100 && h.dst == 0x800001u, "source 100, destination 0x800001");
    CHECK(((h.mni >> 14) & 0x3FFu) == 753u && (h.mni & 0x3FFFu) == 2361u, "MNI 753/2361");
    CHECK(h.frame_countdown == 0 && h.frag_flag == 0 && h.fill_bit == 1, "countdown 0, no fragmentation, fill bits");
    CHECK((h.elems & TETRA_DMO_ELEMS_CALL) && h.group_call == 1 && h.circuit_mode == 0 && h.priority == 0
              && h.power_class == 4 && h.e2ee == 0,
          "DM-OCCUPIED elements: group speech call, normal priority, power class 4, clear");
    {
        char desc[96];
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "group speech") != NULL,
              "element summary text");
    }
    CHECK(strcmp(tetra_dmo_msg_name(h.msg_type), "DM-OCCUPIED") == 0, "message name");

    /* Colour code: MNI[5:0]=57, source 100. The seed verified on real traffic: SCH/H of NTS2 half slots
     * decode with a valid CRC only with this seed. */
    CHECK(tetra_dmo_dcc_seed(h.mni, h.src) == 0xE4000193u, "DCC seed from MNI/source address");
    CHECK(tetra_dmo_dcc_seed(0, 0) == 3u, "all-zero colour code is the DSB seed 3");

    /* State machine: DM-OCCUPIED learns the colour code, DM-TX CEASED forgets it. */
    from_string(k_sch_s, bits);
    tetra_dmo_parse_sync_s(bits, &s);
    tetra_dmo_apply_sync(&state, &s, &h);
    CHECK(tetra_st(&state)->dmo == 1 && tetra_st(&state)->dmo_dcc_valid == 1, "DSB switches to DMO and learns the DCC");
    CHECK(tetra_st(&state)->dmo_dcc_seed == 0xE4000193u && tetra_st(&state)->dmo_slot == 1
              && tetra_st(&state)->dmo_frame == 3,
          "DMO clock and seed stored");

    h.msg_type = TETRA_DMO_MSG_TX_CEASED;
    tetra_dmo_apply_sync(&state, &s, &h);
    CHECK(tetra_st(&state)->dmo_dcc_valid == 0, "DM-TX CEASED invalidates the colour code");

    h.msg_type = TETRA_DMO_MSG_SETUP;
    s.enc_state = 1;
    tetra_dmo_apply_sync(&state, &s, &h);
    CHECK(tetra_st(&state)->dmo_dcc_valid == 0, "encrypted (DM-2-C) call cannot use a clear colour code");

    /* Fragmented short data: SCH/F fragments use the colour code of the DMAC-SYNC that started them. */
    s.enc_state = 0;
    h.msg_type = TETRA_DMO_MSG_SDS_DATA;
    h.frag_flag = 1;
    tetra_dmo_apply_sync(&state, &s, &h);
    CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == 0xE4000193u,
          "fragmented DM-SDS DATA learns the colour code for its SCH/F");
    h.frag_flag = 0;
    tetra_st(&state)->dmo_dcc_valid = 0;
    tetra_dmo_apply_sync(&state, &s, &h);
    CHECK(tetra_st(&state)->dmo_dcc_valid == 0, "unfragmented DM-SDS does not change the colour code");

    /* DM-REP presence signal: SCH/S layout differs from DMAC-SYNC, and SCH/H
     * carries the repeater address rather than a call. It must not invent a DCC. */
    {
        uint8_t ps[60], ph[124];
        int at = 0;
        tetra_dmo_pres_t pres;
        char desc[120];
        uint32_t kept;
        memset(ps, 0, sizeof(ps));
        memset(ph, 0, sizeof(ph));
#define PUT(dst, nbits, value)                                                                                         \
    do {                                                                                                               \
        for (int _i = (nbits) - 1; _i >= 0; _i--)                                                                      \
            (dst)[at++] = (uint8_t)(((value) >> _i) & 1u);                                                             \
    } while (0)
        PUT(ps, 4, 0xD); /* system code 1101 */
        PUT(ps, 2, 1);   /* DPRES-SYNC */
        PUT(ps, 2, 1);   /* via DM-REP */
        PUT(ps, 1, 0);   /* not managed DMO */
        PUT(ps, 2, 0);
        PUT(ps, 1, 1); /* two-frequency */
        PUT(ps, 2, 0); /* type 1 operating mode */
        PUT(ps, 5, 8); /* 45 MHz */
        PUT(ps, 1, 1); /* uplink = downlink - spacing */
        PUT(ps, 1, 0); /* slave link */
        PUT(ps, 2, 0); /* channel usage */
        PUT(ps, 2, 1); /* occupied */
        PUT(ps, 2, 0); /* slot 1 */
        PUT(ps, 5, 3); /* frame 3 */
        PUT(ps, 3, 4); /* power class 4 */
        PUT(ps, 1, 0);
        PUT(ps, 1, 0);
        PUT(ps, 2, 0); /* countdown */
        PUT(ps, 2, 0); /* priority */
        CHECK(at == 41, "presence SCH/S cursor before the fixed tail");
        at = 0;
        PUT(ph, 10, 17);       /* repeater address */
        PUT(ph, 24, 0xBC4939); /* MNI 753/2361 */
        PUT(ph, 2, 3);         /* validity not restricted */
        PUT(ph, 6, 0);
        PUT(ph, 3, 4);
        PUT(ph, 1, 0);
        PUT(ph, 4, 0); /* open repeater */
#undef PUT
        tetra_dmo_parse_sync_s(ps, &s);
        CHECK(s.sync_pdu_type == 1 && s.comm_type == 1, "presence SCH/S header");
        CHECK(tetra_dmo_parse_presence(ps, ph, &s, &pres) == 1, "repeater presence parses");
        CHECK(s.slot == 1 && s.frame == 3 && pres.ms_link == 0 && pres.channel_state == 1,
              "slave link, occupied, clock");
        CHECK(pres.repeater == 17 && pres.mni == 0xBC4939u && pres.two_freq == 1 && pres.spacing == 8
                  && pres.reverse == 1,
              "repeater address, MNI and 45 MHz reverse duplex");
        kept = tetra_st(&state)->dmo_dcc_seed;
        tetra_st(&state)->dmo_dcc_valid = 1;
        tetra_dmo_apply_sync(&state, &s, NULL);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == kept,
              "presence signal does not replace the call colour code");
        CHECK(tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc)) > 0 && strstr(desc, "uplink=-45.000") != NULL,
              "presence summary names the uplink offset");
    }

    /* Gateway call (EN 300 396-5): the colour code is the low 6 bits of the
     * gateway address plus the source address, not the MNI. The MNI is present
     * here because both addresses are true SSIs. */
    {
        uint8_t gh[124];
        int at = 0;
        uint32_t gw_seed;
#define PUT(dst, nbits, value)                                                                                         \
    do {                                                                                                               \
        for (int _i = (nbits) - 1; _i >= 0; _i--)                                                                      \
            (dst)[at++] = (uint8_t)(((value) >> _i) & 1u);                                                             \
    } while (0)
        memset(gh, 0, sizeof(gh));
        PUT(gh, 10, 0x155); /* gateway address, low 6 bits = 0x15 */
        PUT(gh, 1, 0);      /* fill */
        PUT(gh, 1, 0);      /* no fragmentation */
        PUT(gh, 2, 0);      /* countdown */
        PUT(gh, 2, 0);      /* true destination SSI */
        PUT(gh, 24, 0x800001);
        PUT(gh, 2, 0); /* true source SSI */
        PUT(gh, 24, 100);
        PUT(gh, 24, 0xBC4939); /* MNI, which must NOT enter the colour code */
        PUT(gh, 5, 13);        /* DM-OCCUPIED */
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1, "gateway DMAC-SYNC parses");
        CHECK(h.msg_type == TETRA_DMO_MSG_OCCUPIED && h.has_addrs == 1 && h.relay_addr == 0x155 && h.mni == 0xBC4939u
                  && h.src == 100,
              "gateway OCCUPIED keeps the message type after the MNI");
        s.comm_type = 2;
        s.enc_state = 0;
        gw_seed = tetra_dmo_dcc_seed(0x155u, 100u);
        CHECK(gw_seed == 0x54000193u && gw_seed != 0xE4000193u, "gateway colour code ignores the MNI");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == gw_seed,
              "gateway DM-OCCUPIED learns the gateway colour code");

        /* Both addresses pseudo: the MNI is absent and the message type follows the source. */
        at = 0;
        memset(gh, 0, sizeof(gh));
        PUT(gh, 10, 0x155);
        PUT(gh, 1, 0);
        PUT(gh, 1, 0);
        PUT(gh, 2, 0);
        PUT(gh, 2, 1); /* pseudo destination */
        PUT(gh, 24, 0x800001);
        PUT(gh, 2, 1); /* pseudo source */
        PUT(gh, 24, 100);
        PUT(gh, 5, 13);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.msg_type == TETRA_DMO_MSG_OCCUPIED && h.mni == 0,
              "gateway DMAC-SYNC without a true SSI has no MNI");

        /* Gateway presence, communication type 10, SwMI available, channel occupied. */
        {
            uint8_t ps[60], ph[124];
            tetra_dmo_pres_t pres;
            char desc[160];
            at = 0;
            memset(ps, 0, sizeof(ps));
            memset(ph, 0, sizeof(ph));
            PUT(ps, 4, 0xD);
            PUT(ps, 2, 1); /* DPRES-SYNC */
            PUT(ps, 2, 2); /* DM-GATE */
            PUT(ps, 1, 0); /* not managed DMO */
            PUT(ps, 1, 1); /* SwMI available */
            PUT(ps, 1, 0); /* no DM-REP function */
            PUT(ps, 1, 0); /* reserved in place of the two-frequency flag */
            PUT(ps, 2, 0);
            PUT(ps, 6, 0);
            PUT(ps, 1, 0); /* slave / default link */
            PUT(ps, 2, 0);
            PUT(ps, 2, 1); /* occupied */
            PUT(ps, 2, 0); /* slot 1 */
            PUT(ps, 5, 3);
            PUT(ps, 3, 4);
            PUT(ps, 1, 0);
            PUT(ps, 1, 0);
            PUT(ps, 2, 0);
            PUT(ps, 2, 1); /* priority */
            PUT(ps, 2, 0);
            PUT(ps, 4, 0);
            PUT(ps, 4, 0);
            PUT(ps, 3, 0);
            PUT(ps, 1, 0);
            PUT(ps, 1, 0);
            PUT(ps, 1, 0);
            PUT(ps, 3, 0);
            CHECK(at == 60, "gateway presence SCH/S fills 60 bits");
            at = 0;
            PUT(ph, 10, 17);
            PUT(ph, 24, 0xBC4939);
            PUT(ph, 2, 0);
            PUT(ph, 6, 1);
            PUT(ph, 3, 4);
            PUT(ph, 1, 0);
            PUT(ph, 4, 1);
            tetra_dmo_parse_sync_s(ps, &s);
            CHECK(tetra_dmo_parse_presence(ps, ph, &s, &pres) == 1, "gateway presence parses");
            CHECK(s.slot == 1 && s.frame == 3 && pres.channel_state == 1 && pres.swmi_up == 1 && pres.repeater == 17
                      && pres.mni == 0xBC4939u && pres.priority == 1 && pres.two_freq == 0,
                  "gateway presence clock, SwMI and address");
            CHECK(tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc)) > 0 && strstr(desc, "gw=17") != NULL
                      && strstr(desc, "swmi=up") != NULL && strstr(desc, "occupied") != NULL,
                  "gateway presence summary");
        }
#undef PUT
    }

    /* Presence-check, changeover and pre-emption PDUs (EN 300 396-3 tables 32-34 and 39-44).
     * None of these learn a colour code; DM-DISCONNECT drops the one already held. */
    {
        uint8_t gh[124];
        int at;
        char desc[120];
        uint32_t kept = tetra_st(&state)->dmo_dcc_seed;
        tetra_st(&state)->dmo_dcc_valid = 1;

        at = sync_h_header(gh, TETRA_DMO_MSG_SETUP_PRES);
        put_bits(gh, &at, 3, 0);
        put_bits(gh, &at, 3, 4);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 1);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        s.comm_type = 0;
        s.enc_state = 0;
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && (h.elems & TETRA_DMO_ELEMS_CALL) && h.power_class == 4
                  && h.priority == 1 && h.group_call == 0 && h.preempt_flag == 0 && h.timing_flag == 0,
              "DM-SETUP PRES elements");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "individual speech") != NULL
                  && strstr(desc, "prio=high") != NULL,
              "DM-SETUP PRES summary");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == kept,
              "DM-SETUP PRES does not replace the colour code");

        at = sync_h_header(gh, TETRA_DMO_MSG_CONNECT);
        put_bits(gh, &at, 4, 8);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && (h.elems & TETRA_DMO_ELEMS_CONNECT) && h.circuit_mode == 8,
              "DM-CONNECT circuit mode");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "accept prop-speech") != NULL
                  && strstr(desc, "prio=") == NULL,
              "DM-CONNECT summary");

        at = sync_h_header(gh, TETRA_DMO_MSG_DISCONNECT);
        put_bits(gh, &at, 4, 3);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.disconnect_cause == 3, "DM-DISCONNECT cause");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
                  && strstr(desc, "rejected by called party") != NULL,
              "DM-DISCONNECT summary");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 0, "DM-DISCONNECT invalidates the colour code");

        at = sync_h_header(gh, TETRA_DMO_MSG_TX_REQUEST);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 12, 2);
        put_bits(gh, &at, 2, 3);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.timing_change == 1 && h.timing_adjust == 2 && h.priority == 3,
              "DM-TX REQUEST keeps priority after the timing adjustment");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "prio=emergency") != NULL
                  && strstr(desc, "timing=+10 symbols") != NULL,
              "DM-TX REQUEST summary");

        at = sync_h_header(gh, TETRA_DMO_MSG_TX_ACCEPT);
        put_bits(gh, &at, 1, 0);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && (h.elems & TETRA_DMO_ELEMS_TX_ACCEPT) && h.timing_change == 0,
              "DM-TX ACCEPT without a timing change");

        at = sync_h_header(gh, TETRA_DMO_MSG_PREEMPT);
        put_bits(gh, &at, 2, 1);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 2);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.channel_state == 1 && h.new_call == 1 && h.preempt_type == 0
                  && h.priority == 2,
              "DM-PREEMPT elements");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
                  && strstr(desc, "preempt new-call reservation") != NULL && strstr(desc, "circuit") != NULL,
              "DM-PREEMPT summary");

        at = sync_h_header(gh, TETRA_DMO_MSG_PRE_ACCEPT);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 4, 1);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.new_call == 0 && h.preempt_type == 1,
              "DM-PRE ACCEPT elements");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
                  && strstr(desc, "preempt accepted ongoing short data") != NULL,
              "DM-PRE ACCEPT summary");

        at = sync_h_header(gh, TETRA_DMO_MSG_REJECT);
        put_bits(gh, &at, 4, 2);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.reject_cause == 2, "DM-REJECT cause");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "master will transmit") != NULL,
              "DM-REJECT summary");

        /* No layer-2 addresses, so the 48-bit calling-party TSI fits in the SCH/H block. */
        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 24, 0);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_INFO);
        put_bits(gh, &at, 3, 0);
        put_bits(gh, &at, 24, 0xBC4939);
        put_bits(gh, &at, 24, 100);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.calling_ssi == 100 && h.calling_mni == 0xBC4939u,
              "DM-INFO TSI");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "753/2361") != NULL
                  && strstr(desc, "ssi=100") != NULL,
              "DM-INFO summary");

        at = sync_h_header(gh, TETRA_DMO_MSG_RELEASE);
        put_bits(gh, &at, 4, 15);
        put_bits(gh, &at, 5, 0);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.has_release_ext == 1 && h.release_ext == 0,
              "DM-RELEASE extension");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
                  && strstr(desc, "occupied by other users") != NULL,
              "DM-RELEASE extension summary");
    }

    /* Gateway-specific PDUs (message type 25). A fragmented DM-GSETUP learns the
     * gateway colour code; DM-GRELEASE drops it. An unfragmented one does not. */
    {
        uint8_t gh[124];
        int at;
        char desc[160];
        uint32_t kept;
        s.comm_type = 2;
        s.enc_state = 0;
        tetra_st(&state)->dmo_dcc_valid = 1;
        tetra_st(&state)->dmo_dcc_seed = 1;
        kept = tetra_st(&state)->dmo_dcc_seed;

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0x155);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0); /* not fragmented */
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 1); /* pseudo source, so the MNI is absent */
        put_bits(gh, &at, 24, 100);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_SETUP);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 1);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 4, 7);
        put_bits(gh, &at, 1, 1); /* called party TSI */
        put_bits(gh, &at, 24, 50);
        put_bits(gh, &at, 24, 0xBC4939);
        put_bits(gh, &at, 4, 15);
        put_bits(gh, &at, 1, 0);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.gw_subtype == TETRA_DMO_GW_SETUP && h.called_ssi == 50
                  && h.called_mni == 0xBC4939u && h.request_label == 7 && h.area == 15 && h.group_call == 1,
              "DM-GSETUP called-party TSI");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0
                  && strstr(desc, "DM-GSETUP group speech") != NULL && strstr(desc, "label=7") != NULL
                  && strstr(desc, "MNI=753/2361") != NULL && strstr(desc, "area=all") != NULL,
              "DM-GSETUP summary");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == kept,
              "unfragmented DM-GSETUP does not replace the colour code");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0x155);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 4, 1);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 1);
        put_bits(gh, &at, 24, 100);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_SETUP);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 4, 1);
        put_bits(gh, &at, 1, 0); /* SSI only */
        put_bits(gh, &at, 24, 50);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 1, 0);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.frag_flag == 1 && h.called_tsi == 0 && h.called_ssi == 50,
              "fragmented DM-GSETUP");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == 0x54000193u,
              "fragmented DM-GSETUP learns the gateway colour code");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0x155);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 1);
        put_bits(gh, &at, 24, 100);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_RELEASE);
        put_bits(gh, &at, 4, 1);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1, "DM-GRELEASE parses");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "TMO resource lost") != NULL,
              "DM-GRELEASE summary");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 0, "DM-GRELEASE invalidates the colour code");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_ACK);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 12, 2);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 3, 4);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.timing_adjust == 2 && h.wait_code == 4
                  && h.has_reservation == 0,
              "DM-GACK timing and waiting timer");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "wait=10s") != NULL
                  && strstr(desc, "timing=+10 symbols") != NULL,
              "DM-GACK summary");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_REG_REJECT);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 4, 3);
        put_bits(gh, &at, 4, 5);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.reg_label == 3 && h.reject_cause == 5, "DM-GREGISTER REJECT");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "label=3") != NULL
                  && strstr(desc, "group unsupported") != NULL,
              "DM-GREGISTER REJECT summary");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, 31);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.elems == 0, "unknown gateway subtype is left unparsed");
    }

    /* Short data, an external number and a timing request. None of these replace
     * a colour code unless the short-data burst itself is fragmented. */
    {
        uint8_t gh[124];
        int at;
        char desc[160];
        uint32_t kept;
        tetra_st(&state)->dmo_dcc_valid = 1;
        tetra_st(&state)->dmo_dcc_seed = 0xE4000193u;
        kept = tetra_st(&state)->dmo_dcc_seed;
        s.comm_type = 0;
        s.enc_state = 0;

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_SDS_UDATA);
        put_bits(gh, &at, 4, 3); /* time remaining */
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 1); /* high priority */
        put_bits(gh, &at, 1, 1); /* FCS present */
        put_bits(gh, &at, 1, 1); /* additional addressing */
        put_bits(gh, &at, 4, 4); /* external number only */
        put_bits(gh, &at, 5, 4);
        put_bits(gh, &at, 4, 10); /* * */
        put_bits(gh, &at, 4, 1);
        put_bits(gh, &at, 4, 2);
        put_bits(gh, &at, 4, 3);
        put_bits(gh, &at, 4, 4); /* precoded status */
        put_bits(gh, &at, 16, 0x1234);
        put_bits(gh, &at, 32, 0xA1B2C3D4u);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && h.external_n == 4 && strcmp(h.external, "*123") == 0
                  && h.sdti == 4 && h.sds_value == 0x1234u && h.has_fcs == 1 && h.sds_fcs == 0xA1B2C3D4u,
              "DM-SDS UDATA external number and status");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "status=0x1234") != NULL
                  && strstr(desc, "ext=*123") != NULL && strstr(desc, "fcs=0xa1b2c3d4") != NULL,
              "DM-SDS summary");
        state.tetra_message_sequence = 0;
        tetra_dmo_publish(&state, h.msg_type, h.src, h.dst, desc);
        tetra_dmo_publish(&state, h.msg_type, h.src, h.dst, desc);
        CHECK(state.tetra_message_sequence == 1 && state.tetra_messages[0].category == DSD_TETRA_MESSAGE_SDS
                  && strstr(state.tetra_messages[0].text, "status=0x1234") != NULL
                  && strstr(state.tetra_messages[0].text, "ext=*123") != NULL,
              "DM-SDS is queued once for the terminal");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == kept,
              "unfragmented DM-SDS UDATA keeps the colour code");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 24, 0);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_TIMING_REQ);
        put_bits(gh, &at, 12, 2);
        CHECK(tetra_dmo_parse_sync_h(gh, 0, &h) == 1 && h.timing_adjust == 2, "DM-TIMING REQUEST adjustment");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "timing request") != NULL
                  && strstr(desc, "timing=+10 symbols") != NULL,
              "DM-TIMING REQUEST summary");
        tetra_dmo_apply_sync(&state, &s, &h);
        CHECK(tetra_st(&state)->dmo_dcc_valid == 1 && tetra_st(&state)->dmo_dcc_seed == kept,
              "DM-TIMING REQUEST does not replace the colour code");

        at = 0;
        memset(gh, 0, sizeof(gh));
        put_bits(gh, &at, 10, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 2, 2);
        put_bits(gh, &at, 5, TETRA_DMO_MSG_GATEWAY);
        put_bits(gh, &at, 5, TETRA_DMO_GW_SETUP);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 2, 0);
        put_bits(gh, &at, 4, 1);
        put_bits(gh, &at, 1, 0);
        put_bits(gh, &at, 24, 9);
        put_bits(gh, &at, 4, 0);
        put_bits(gh, &at, 1, 1);
        put_bits(gh, &at, 4, 4);
        put_bits(gh, &at, 5, 4);
        put_bits(gh, &at, 4, 10);
        put_bits(gh, &at, 4, 1);
        put_bits(gh, &at, 4, 2);
        put_bits(gh, &at, 4, 3);
        CHECK(tetra_dmo_parse_sync_h(gh, 2, &h) == 1 && strcmp(h.external, "*123") == 0 && h.sds_truncated == 0,
              "DM-GSETUP external number");
        CHECK(tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc)) > 0 && strstr(desc, "ext=*123") != NULL,
              "DM-GSETUP external-number summary");
    }

    {
        char mode[96];
        static dsd_state mode_state;
        memset(&mode_state, 0, sizeof(mode_state));
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strcmp(mode, "TMO downlink") == 0,
              "default working mode is TMO downlink");
        tetra_st(&mode_state)->net_known = 1;
        tetra_st(&mode_state)->mcc = 86;
        tetra_st(&mode_state)->mnc = 28;
        tetra_st(&mode_state)->colour = 0x19;
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strstr(mode, "TMO downlink") != NULL
                  && strstr(mode, "MCC=86") != NULL && strstr(mode, "CC=0x19") != NULL,
              "TMO downlink names the network");
        tetra_st(&mode_state)->enc_mode = 1;
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strstr(mode, "encrypted") != NULL
                  && strstr(mode, "TEA") == NULL,
              "encrypted trunked call without a KSG stays unnamed");
        tetra_air_ksg_remember(&mode_state, 86, 28, 1);
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strstr(mode, "TEA2") != NULL,
              "remembered KSG is shown beside the working mode");
        tetra_st(&mode_state)->enc_mode = 0;
        tetra_st(&mode_state)->uplink = 1;
        mode_state.tetra_sync.cb = 1;
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0
                  && strstr(mode, "TMO uplink control") != NULL,
              "TMO uplink control working mode");
        tetra_st(&mode_state)->dmo = 1;
        tetra_st(&mode_state)->dmo_comm_type = 0;
        tetra_st(&mode_state)->dmo_ab_usage = 0;
        tetra_st(&mode_state)->dmo_enc_state = 0;
        tetra_st(&mode_state)->dmo_slot = 1;
        tetra_st(&mode_state)->dmo_frame = 3;
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strstr(mode, "DMO direct") != NULL
                  && strstr(mode, "ch A") != NULL && strstr(mode, "clear") != NULL && strstr(mode, "slot 1") != NULL
                  && strstr(mode, "TEA") == NULL,
              "DMO direct working mode");
        tetra_st(&mode_state)->dmo_comm_type = 2;
        tetra_st(&mode_state)->dmo_enc_state = 1;
        CHECK(tetra_dmo_format_mode(&mode_state, mode, (int)sizeof(mode)) > 0 && strstr(mode, "DMO gateway") != NULL
                  && strstr(mode, "encrypted") != NULL,
              "DMO gateway working mode");
    }

    printf("%s\n", g_fail ? "FAILED" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
