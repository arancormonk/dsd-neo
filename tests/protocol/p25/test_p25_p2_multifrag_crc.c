// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 multi-fragment messages (TIA-102.BBAD-A §3, §4.7): only an LCCH carries them, in consecutive bursts of
 * its logical channel, and a message decodes only when every fragment's MAC PDU passed its CRC-16 and the assembled
 * Multi-Fragment CRC-16 checks. The fragments go through the real VPDU decoder (process_MAC_VPDU_crc()); the
 * discontinuity cases build windows as a site sends them (p25p2_burst_builder.h) and read them through processP2()
 * with a stand-in symbol reader, so the window position, the Reed-Solomon decode, the LCCH CRC-16 and the XCCH
 * handlers all run.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_burst_builder.h"
#include "test_support.h"

#define TEST_T0_NS UINT64_C(1000000000000)

enum {
    DUID_DATA = 10,
    DUID_LCCH_U = 13,
    /* Status Update - Extended LCCH (TIA-102.BBAD-A §3.41, Figures 93-94): 25 octets, the last two the CRC. */
    STATUS_OPCODE = 0xD9,
    STATUS_DATA_LEN = 25,
    BASE_LEN = 18,
    CONT_LEN = 11,
    BASE_OCTETS = BASE_LEN - 2,
    STRONG_LLR = 200,
    WINDOW_DIBITS = 700,
    /* The next window read right behind the previous one (its sync follows), or one window (4 timeslots of 180
       dibits) later: continuity retires the 4 timeslots between as lost. */
    GAP_FIRST = 27,
    GAP_CONTIGUOUS = 20,
    GAP_SKIP_WINDOW = 20 + (4 * 180),
    CAPTURE_BYTES = 16384,
};

static dsd_opts* g_opts;
static dsd_state* g_state;

/* ---- the stand-in symbol reader processP2() reads a built window from ---- */

static uint8_t g_bits[1440];
static int g_feed_pos = -1;

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    int dibit = 0;
    int16_t llr0 = 0;
    int16_t llr1 = 0;
    if (g_feed_pos >= 0 && g_feed_pos < WINDOW_DIBITS) {
        const int bit = g_feed_pos * 2;
        dibit = (g_bits[bit] << 1) | g_bits[bit + 1];
        llr0 = (int16_t)(g_bits[bit] ? STRONG_LLR : -STRONG_LLR);
        llr1 = (int16_t)(g_bits[bit + 1] ? STRONG_LLR : -STRONG_LLR);
        g_feed_pos++;
        if (state) {
            state->symbolcnt++;
        }
    }
    if (out_soft) {
        out_soft->reliability = 128;
        out_soft->llr[0] = llr0;
        out_soft->llr[1] = llr1;
    }
    return dibit;
}

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/* A fresh decoder, strict CRC unless @p relaxed (-F). */
static void
fresh_decoder(int relaxed) {
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    dsd_opts_set_relaxed_crc(g_opts, relaxed);
    dsd_decode_clock_use_test(TEST_T0_NS);
    p25_p2_frame_reset();
}

static const char*
newest_notice(void) {
    static dsd_recent_activity_snapshot recent;
    DSD_MEMSET(&recent, 0, sizeof recent);
    if (dsd_recent_activity_copy_snapshot(g_state, &recent) <= 0) {
        return "";
    }
    return recent.entries[0].notice;
}

static int
decoded(void) {
    return strstr(newest_notice(), "STATUS-L") != NULL;
}

/* The rows of the unit activity log. */
static int
activity_rows(void) {
    dsd_unit_activity rows[4];
    dsd_unit_activity_read_meta meta;
    return dsd_unit_activity_read(g_state, 0U, 0U, rows, 4, &meta);
}

static int
no_assembly(void) {
    return !g_state->p25_mac_frag[0].active && !g_state->p25_mac_frag[1].active;
}

/* ---- the message ---- */

/* The Multi-Fragment CRC-16 (TIA-102.BBAD-A §4.7): G(x) = x^16 + x^12 + x^5 + 1 over the octets MSB first, the
   remainder inverted. Written here from the clause, apart from the decoder's. */
static uint16_t
multifrag_crc16(const uint8_t* octets, size_t count) {
    uint16_t crc = 0U;
    for (size_t i = 0; i < count; i++) {
        for (int b = 7; b >= 0; b--) {
            const unsigned feedback = ((crc >> 15U) & 1U) ^ ((octets[i] >> b) & 1U);
            crc = (uint16_t)(crc << 1U);
            if (feedback) {
                crc ^= 0x1021U;
            }
        }
    }
    return (uint16_t)(crc ^ 0xFFFFU);
}

static void
put_u24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

/* An SUID (Figure 197): WACN 20 bits, System 12, Unit 24. */
static void
put_suid(uint8_t* p, uint32_t wacn, uint32_t sys, uint32_t unit) {
    p[0] = (uint8_t)(wacn >> 12);
    p[1] = (uint8_t)(wacn >> 4);
    p[2] = (uint8_t)(((wacn & 0xFU) << 4) | ((sys >> 8) & 0xFU));
    p[3] = (uint8_t)sys;
    put_u24(p + 4, unit);
}

/* The Status Update's data octets (from Data Length on), its Multi-Fragment CRC-16 in the last two. */
static void
status_update_data(uint8_t data[STATUS_DATA_LEN]) {
    DSD_MEMSET(data, 0, STATUS_DATA_LEN);
    data[0] = STATUS_DATA_LEN;
    data[1] = 0xA1; /* User Status */
    data[2] = 0x0B; /* Unit Status */
    put_u24(data + 3, 1234567U);
    put_suid(data + 6, 0xBEE00U, 0x123U, 7654321U);
    put_u24(data + 13, 7654321U);
    put_suid(data + 16, 0xBEE00U, 0x123U, 1234567U);
    const uint16_t crc = multifrag_crc16(data, STATUS_DATA_LEN - 2);
    data[STATUS_DATA_LEN - 2] = (uint8_t)(crc >> 8);
    data[STATUS_DATA_LEN - 1] = (uint8_t)crc;
}

/* Fragment 1 (B1 B2 MCO, Length 18, then 16 data octets) and fragment 2 (the continuation, MCO %010000, Length 11). */
static void
fragment_macs(const uint8_t data[STATUS_DATA_LEN], unsigned long long int base[24], unsigned long long int cont[24]) {
    DSD_MEMSET(base, 0, sizeof(unsigned long long int) * 24U);
    DSD_MEMSET(cont, 0, sizeof(unsigned long long int) * 24U);
    base[1] = STATUS_OPCODE;
    base[2] = BASE_LEN;
    for (int i = 0; i < BASE_OCTETS; i++) {
        base[3 + i] = data[i];
    }
    cont[1] = 0x10;
    cont[2] = CONT_LEN;
    for (int i = 0; i < STATUS_DATA_LEN - BASE_OCTETS; i++) {
        cont[3 + i] = data[BASE_OCTETS + i];
    }
}

static void
status_update_fragments(unsigned long long int base[24], unsigned long long int cont[24]) {
    uint8_t data[STATUS_DATA_LEN];
    status_update_data(data);
    fragment_macs(data, base, cont);
}

/* ---- the VPDU decoder, as the XCCH hands it a MAC PDU ---- */

/* An LCCH MAC_SIGNAL of logical channel @p lch whose CRC-16 passed (@p crc_ok) or failed (-F lets it through). */
static void
lcch_pdu(int lch, unsigned long long int mac[24], int crc_ok) {
    g_state->p2_is_lcch = 1;
    g_state->currentslot = lch;
    process_MAC_VPDU_crc(g_opts, g_state, 1 /* SACCH layout */, P25_MAC_PDU_SIGNAL, mac, crc_ok);
}

static char g_out[CAPTURE_BYTES];

/* lcch_pdu(), keeping what the decoder prints in g_out. */
static int
lcch_pdu_capturing(int lch, unsigned long long int mac[24], int crc_ok) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p2_multifrag_crc") != 0) {
        return -1;
    }
    lcch_pdu(lch, mac, crc_ok);
    dsd_test_capture_stderr_end(&cap);
    return dsd_test_capture_stderr_read(&cap, g_out, sizeof g_out);
}

static int
test_frag_valid_lcch_completes(void) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    int rc = 0;

    fresh_decoder(0);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    rc |= expect("valid: base alone decodes nothing", !decoded());
    rc |= expect("valid: base starts the LCCH's assembly", g_state->p25_mac_frag[0].active == 1);
    rc |= expect("valid: completes", lcch_pdu_capturing(0, cont, 1) == 0 && decoded());
    rc |= expect("valid: decoded text", strstr(g_out, "Status Update - Extended LCCH Complete") != NULL);
    rc |= expect("valid: user status", strstr(newest_notice(), "User: A1 Unit: 0B") != NULL);
    rc |= expect("valid: target", strstr(newest_notice(), "Target: 1234567") != NULL);
    rc |= expect("valid: source", strstr(newest_notice(), "Source: 7654321") != NULL);
    rc |= expect("valid: assembly done", no_assembly());
    /* A multi-fragment message never makes a unit activity row (plan D9: LCCH stream decoding, issue #874). */
    rc |= expect("valid: no unit activity row", activity_rows() == 0);

    /* A FACCH or SACCH PDU of the other logical channel, between two consecutive bursts of the LCCH's, is no burst of
       the LCCH's stream: its messages leave the LCCH's assembly alone. */
    fresh_decoder(0);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    unsigned long long int other[24] = {0};
    other[1] = 0x30; /* Power Control Signal Quality (5 octets), which only prints */
    other[2] = 0x12;
    other[3] = 0x34;
    other[4] = 0x56;
    other[5] = 0x78;
    g_state->currentslot = 1;
    process_MAC_VPDU(g_opts, g_state, 0 /* FACCH */, P25_MAC_PDU_ACTIVE, other);
    g_state->currentslot = 0;
    process_MAC_VPDU(g_opts, g_state, 1 /* SACCH */, P25_MAC_PDU_ACTIVE, other);
    lcch_pdu(0, cont, 1);
    rc |= expect("valid: other channel's FACCH/SACCH leave the assembly", decoded());

    /* A continuation on the other logical channel joins no assembly of this one (a dual LCCH is not followed). */
    fresh_decoder(0);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    lcch_pdu(1, cont, 1);
    rc |= expect("valid: other channel's continuation joins nothing", !decoded());
    rc |= expect("valid: other channel's continuation leaves this one", g_state->p25_mac_frag[0].active == 1);
    lcch_pdu(0, cont, 1);
    rc |= expect("valid: this channel's continuation completes", decoded());
    return rc;
}

/* A base fragment whose MAC PDU failed its CRC-16 (-F lets the PDU through): nothing is kept, so the good
   continuation after it finds no assembly. */
static int
test_frag_bad_base_good_cont(void) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    int rc = 0;

    fresh_decoder(1);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 0);
    rc |= expect("bad base: not kept", no_assembly());
    lcch_pdu(0, cont, 1);
    rc |= expect("bad base: good continuation decodes nothing", !decoded());
    rc |= expect("bad base: still no assembly", no_assembly());

    /* A good base over a kept one starts again: the failed one never mixes in. */
    fresh_decoder(1);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    lcch_pdu(0, base, 0);
    rc |= expect("bad base: ends the assembly before it", no_assembly());
    lcch_pdu(0, cont, 1);
    rc |= expect("bad base after good: nothing decodes", !decoded());
    return rc;
}

/* A continuation whose MAC PDU failed its CRC-16 ends the assembly: neither it nor a later good copy completes it. */
static int
test_frag_good_base_bad_cont(void) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    int rc = 0;

    fresh_decoder(1);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    lcch_pdu(0, cont, 0);
    rc |= expect("bad continuation: decodes nothing", !decoded());
    rc |= expect("bad continuation: assembly ended", no_assembly());
    lcch_pdu(0, cont, 1);
    rc |= expect("bad continuation: a later good one finds no assembly", !decoded());

    /* Nor does a failed PDU holding any other message keep the assembly going. */
    fresh_decoder(1);
    status_update_fragments(base, cont);
    lcch_pdu(0, base, 1);
    unsigned long long int other[24] = {0};
    other[1] = 0x30;
    other[2] = 0x12;
    lcch_pdu(0, other, 0);
    rc |= expect("failed other PDU: assembly ended", no_assembly());
    lcch_pdu(0, cont, 1);
    rc |= expect("failed other PDU: continuation decodes nothing", !decoded());
    return rc;
}

/* Every fragment's PDU passed, but the assembled message fails its Multi-Fragment CRC-16: nothing past a raw note is
   printed, published or kept. */
static int
test_frag_bad_assembled_crc(void) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    uint8_t data[STATUS_DATA_LEN];
    int rc = 0;

    for (int variant = 0; variant < 3; variant++) {
        fresh_decoder(0);
        status_update_data(data);
        if (variant == 0) {
            data[STATUS_DATA_LEN - 1] ^= 0x01U; /* the CRC itself */
        } else if (variant == 1) {
            data[3] ^= 0x80U; /* a base fragment octet */
        } else {
            data[17] ^= 0x10U; /* a continuation octet */
        }
        fragment_macs(data, base, cont);
        lcch_pdu(0, base, 1);
        rc |= expect("bad assembled CRC: captured", lcch_pdu_capturing(0, cont, 1) == 0);
        rc |= expect("bad assembled CRC: nothing published", !decoded() && newest_notice()[0] == '\0');
        rc |= expect("bad assembled CRC: no decoded text",
                     strstr(g_out, "Status Update - Extended LCCH Complete") == NULL);
        rc |= expect("bad assembled CRC: says so", strstr(g_out, "Multi-Fragment CRC") != NULL);
        rc |= expect("bad assembled CRC: assembly ended", no_assembly());
    }
    return rc;
}

/* Multi-fragment messages are only available on the LCCH (TIA-102.BBAD-A §3): fragments on a FACCH or SACCH, even
   with good CRCs, are dropped. */
static int
test_frag_facch_sacch_dropped(void) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    int rc = 0;

    for (int type = 0; type <= 1; type++) {
        fresh_decoder(0);
        status_update_fragments(base, cont);
        g_state->currentslot = 0;
        process_MAC_VPDU(g_opts, g_state, type, P25_MAC_PDU_ACTIVE, base);
        rc |= expect(type ? "SACCH base not kept" : "FACCH base not kept", no_assembly());
        process_MAC_VPDU(g_opts, g_state, type, P25_MAC_PDU_ACTIVE, cont);
        rc |= expect(type ? "SACCH fragments decode nothing" : "FACCH fragments decode nothing", !decoded());

        /* Nor does one continue an LCCH's assembly. */
        fresh_decoder(0);
        status_update_fragments(base, cont);
        lcch_pdu(0, base, 1);
        g_state->currentslot = type ? 1 : 0; /* a FACCH's MAC is its own slot's, a SACCH's the other's */
        process_MAC_VPDU(g_opts, g_state, type, P25_MAC_PDU_ACTIVE, cont);
        rc |=
            expect(type ? "SACCH continuation completes nothing" : "FACCH continuation completes nothing", !decoded());
    }
    return rc;
}

/* ---- the frame decoder: windows read through processP2() ---- */

static void
lcch_burst(p25p2_bb_slot* slot, const unsigned long long int mac[24], int good_crc) {
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->duid = DUID_LCCH_U;
    slot->fill_crc = good_crc; /* otherwise the CRC-16 bits stay zero, which this content fails */
    slot->mac[0] = 0x00U;      /* MAC_SIGNAL */
    for (int i = 1; i <= 18; i++) {
        slot->mac[i] = (uint8_t)mac[i];
    }
}

static void
data_burst(p25p2_bb_slot* slot) {
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->duid = DUID_DATA;
}

/* A window at superframe slot @p t0 (2 or 6 here: its I-ISCH after slot 4 or 8 locates it) with a single LCCH on
   logical channel 0: window timeslots 0 and 2 carry @p even0 and @p even2 (NULL: a data burst), the odd ones data. */
static void
build_window(int t0, const p25p2_bb_slot* even0, const p25p2_bb_slot* even2) {
    p25p2_bb_slot slots[4];
    data_burst(&slots[1]);
    data_burst(&slots[3]);
    if (even0) {
        slots[0] = *even0;
    } else {
        data_burst(&slots[0]);
    }
    if (even2) {
        slots[2] = *even2;
    } else {
        data_burst(&slots[2]);
    }
    p25p2_bb_window(0U, t0, slots, g_bits);
}

/* processP2() reads the built window @p gap dibits after the previous one ended. */
static void
feed(uint32_t gap) {
    g_state->symbolcnt += gap;
    g_feed_pos = 0;
    processP2(g_opts, g_state);
    g_feed_pos = -1;
}

enum {
    BETWEEN_NOTHING = 0,  /* the continuation is the LCCH's next burst */
    BETWEEN_FAILED_CRC,   /* an LCCH burst whose CRC-16 failed */
    BETWEEN_DATA_BURST,   /* a burst that is no LCCH */
    BETWEEN_LOST_WINDOW,  /* a missed sync: a window's timeslots pass unread */
    BETWEEN_CARRIER_MOVE, /* the receiver changed carrier */
    BETWEEN_FRAME_RESET,  /* the frame decoder was reset (a retune, a symbol-rate change) on the same carrier */
};

/* The base in window timeslot 2 (superframe slot 4) of one window, the continuation in a later window's timeslot 0:
   whether it decoded, with @p between in the LCCH's stream between them. */
static int
across_windows(int relaxed, int between) {
    unsigned long long int base[24];
    unsigned long long int cont[24];
    p25p2_bb_slot b;
    p25p2_bb_slot c;
    p25p2_bb_slot bad;
    fresh_decoder(relaxed);
    status_update_fragments(base, cont);
    lcch_burst(&b, base, 1);
    lcch_burst(&c, cont, 1);
    unsigned long long int other[24] = {0};
    other[1] = 0x30;
    other[2] = 0x12;
    lcch_burst(&bad, other, 0);

    if (between == BETWEEN_FAILED_CRC || between == BETWEEN_DATA_BURST) {
        /* The base one burst earlier (slot 2), then the burst in between at slot 4. */
        build_window(2, &b, between == BETWEEN_FAILED_CRC ? &bad : NULL);
    } else {
        build_window(2, NULL, &b);
    }
    feed(GAP_FIRST);
    uint32_t gap = GAP_CONTIGUOUS;
    int t0 = 6;
    if (between == BETWEEN_LOST_WINDOW) {
        gap = GAP_SKIP_WINDOW;
        t0 = 10;
    } else if (between == BETWEEN_CARRIER_MOVE) {
        g_state->carrier_seq++;
    } else if (between == BETWEEN_FRAME_RESET) {
        p25_p2_frame_reset();
    }
    build_window(t0, &c, NULL);
    feed(gap);
    return decoded();
}

/* Fragments come in consecutive bursts of the LCCH's logical channel (TIA-102.BBAD-A §3), on one carrier: a burst of
   that channel that failed its CRC-16 or was no LCCH, timeslots a missed sync skipped, a carrier change, or a frame
   decoder reset (after which no window says what was missed) between two fragments ends the assembly, strict CRC or
   -F. */
static int
test_frag_discontinuity_resets(void) {
    int rc = 0;
    for (int relaxed = 0; relaxed <= 1; relaxed++) {
        rc |= expect(relaxed ? "-F: next burst completes" : "next burst completes",
                     across_windows(relaxed, BETWEEN_NOTHING));
        rc |= expect(relaxed ? "-F: failed burst between resets" : "failed burst between resets",
                     !across_windows(relaxed, BETWEEN_FAILED_CRC) && no_assembly());
        rc |= expect(relaxed ? "-F: data burst between resets" : "data burst between resets",
                     !across_windows(relaxed, BETWEEN_DATA_BURST) && no_assembly());
        rc |= expect(relaxed ? "-F: lost window between resets" : "lost window between resets",
                     !across_windows(relaxed, BETWEEN_LOST_WINDOW) && no_assembly());
        rc |= expect(relaxed ? "-F: carrier change between resets" : "carrier change between resets",
                     !across_windows(relaxed, BETWEEN_CARRIER_MOVE) && no_assembly());
        rc |= expect(relaxed ? "-F: frame reset between resets" : "frame reset between resets",
                     !across_windows(relaxed, BETWEEN_FRAME_RESET) && no_assembly());
    }

    /* Both fragments in one window, as consecutive LCCH bursts: the frame decoder completes it. */
    unsigned long long int base[24];
    unsigned long long int cont[24];
    p25p2_bb_slot b;
    p25p2_bb_slot c;
    fresh_decoder(0);
    status_update_fragments(base, cont);
    lcch_burst(&b, base, 1);
    lcch_burst(&c, cont, 1);
    build_window(2, &b, &c);
    feed(GAP_FIRST);
    rc |= expect("one window: completes", decoded());
    return rc;
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    if (g_opts == NULL || g_state == NULL) {
        free(g_opts);
        free(g_state);
        return 1;
    }
    int rc = 0;
    rc |= test_frag_valid_lcch_completes();
    rc |= test_frag_bad_base_good_cont();
    rc |= test_frag_good_base_bad_cont();
    rc |= test_frag_bad_assembled_crc();
    rc |= test_frag_facch_sacch_dropped();
    rc |= test_frag_discontinuity_resets();
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    free(g_state);
    free(g_opts);
    if (rc == 0) {
        printf("P25_P2_MULTIFRAG_CRC: OK\n");
    }
    return rc;
}
