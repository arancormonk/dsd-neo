// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 FACCH, SACCH and LCCH bursts through the real Reed-Solomon decode, the real MAC CRC and the real XCCH
 * handlers (issue #804): the checksum verdict comes before anything the burst proves, the retry ladder looks past a
 * miscorrection its CRC rejects, its soft erasures stop where the code's check still means something, signalling
 * leaves the service options alone, an LCCH that failed does not turn the next burst into one, a DUID 10 burst is no
 * error, and any MAC whose checksum passed is Signal search evidence for the slot it belongs to. The windows are built as a site sends them (p25p2_burst_builder.h) and dispatched by p25p2_process_duid().
 * Only the VPDU decoder is a stand-in, which records what reached it.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_burst_builder.h"
#include "p25p2_frame_internal.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];

#define TEST_WACN  0xBEE00U
#define TEST_SYSID 0x164U
#define TEST_NAC   0x161U
#define TEST_CODE  ((((uint64_t)TEST_WACN) << 24) | (((uint64_t)TEST_SYSID) << 12) | (uint64_t)TEST_NAC)
#define TEST_T0_NS UINT64_C(1000000000000)
#define TEST_TG    1201
#define TEST_SRC   1202

enum {
    DUID_DATA = 10,
    DUID_SACCH_S = 3,
    DUID_LCCH_S = 4,
    DUID_FACCH_S = 9,
    DUID_LCCH_U = 13,
    MAC_SIGNAL = 0,
    MAC_PTT = 1,
    MAC_END_PTT = 2,
    MAC_IDLE = 3,
    MAC_ACTIVE = 4,
    MAC_HANGTIME = 6,
    STRONG_LLR = 200,
};

static dsd_opts* g_opts;
static dsd_state* g_state;

/* What reached the VPDU decoder, by PDU type, and the CRC verdict of the last MAC_SIGNAL. */
static int g_vpdu_calls[8];
static int g_vpdu_signal_crc_ok = -1;

void
process_MAC_VPDU_crc(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                     unsigned long long int mac[24], int crc_ok) {
    (void)opts;
    (void)state;
    (void)type;
    (void)mac;
    if ((int)pdu_type >= 0 && (int)pdu_type < 8) {
        g_vpdu_calls[pdu_type]++;
    }
    if (pdu_type == P25_MAC_PDU_SIGNAL) {
        g_vpdu_signal_crc_ok = crc_ok;
    }
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                 unsigned long long int mac[24]) {
    process_MAC_VPDU_crc(opts, state, type, pdu_type, mac, 1);
}

/* Signal search evidence the bursts reported: how many, and the last one's protocol, synctype and slot. */
static int g_evidence;
static char g_evidence_protocol[8];
static int g_evidence_sync;
static int g_evidence_slot;

static void
capture_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target, uint32_t source,
                 const char* access) {
    (void)state;
    (void)target;
    (void)source;
    (void)access;
    g_evidence++;
    DSD_SNPRINTF(g_evidence_protocol, sizeof g_evidence_protocol, "%s", protocol ? protocol : "");
    g_evidence_sync = sync;
    g_evidence_slot = slot;
}

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

/* A fresh decoder on the test site, strict CRC (the default) unless @p relaxed. */
static void
fresh_decoder(int relaxed) {
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    dsd_opts_set_relaxed_crc(g_opts, relaxed);
    g_state->p2_wacn = TEST_WACN;
    g_state->p2_sysid = TEST_SYSID;
    g_state->p2_cc = TEST_NAC;
    dsd_decode_clock_use_test(TEST_T0_NS);
    p25_p2_frame_reset();
    DSD_MEMSET(g_vpdu_calls, 0, sizeof(g_vpdu_calls));
    g_vpdu_signal_crc_ok = -1;
    g_evidence = 0;
    g_evidence_protocol[0] = '\0';
    g_evidence_sync = -2;
    g_evidence_slot = -2;
}

static void
mac_header(p25p2_bb_slot* slot, int duid, int opcode) {
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->duid = duid;
    slot->fill_crc = 1;
    slot->mac[0] = (uint8_t)(opcode << 5);
}

/* A MAC_PTT or MAC_END_PTT naming TEST_TG and TEST_SRC (the XCCH reads SRC at octets 13-15, TG at 16-17). */
static void
mac_call(p25p2_bb_slot* slot, int duid, int opcode) {
    mac_header(slot, duid, opcode);
    slot->mac[1] = 0x01U;
    slot->mac[13] = (uint8_t)((TEST_SRC >> 16) & 0xFF);
    slot->mac[14] = (uint8_t)((TEST_SRC >> 8) & 0xFF);
    slot->mac[15] = (uint8_t)(TEST_SRC & 0xFF);
    slot->mac[16] = (uint8_t)((TEST_TG >> 8) & 0xFF);
    slot->mac[17] = (uint8_t)(TEST_TG & 0xFF);
}

/* The payload bits of @p slot as the builder sends them, before RS. */
static void
slot_payload(const p25p2_bb_slot* slot, int* payload, int n_bits) {
    for (int k = 0; k < n_bits; k++) {
        payload[k] = (slot->mac[k / 8] >> (7 - (k % 8))) & 1;
    }
}

/* @p slot with a checksum that fails: its CRC field (FACCH from bit 144, SACCH 168, LCCH 164) sent as given, which
   the caller asserts really fails. */
static int
break_crc(p25p2_bb_slot* slot, int kind_bits, int crc16) {
    const int crc_start = crc16 ? 164 : kind_bits - 12;
    slot->fill_crc = 0;
    slot->mac[(crc_start / 8) + 1] ^= 0x5AU;
    int payload[180];
    slot_payload(slot, payload, kind_bits);
    const int ok = crc16 ? crc16_lb_bridge(payload, 164) == 0 : crc12_xb_bridge(payload, kind_bits - 12) == 0;
    return ok ? 0 : 1;
}

static void
load_window(const uint8_t bits[1440]) {
    for (int i = 0; i < 1440; i++) {
        p2bit[i] = bits[i];
    }
    for (int i = 0; i < 1400; i++) {
        p2llr[i] = bits[i] ? STRONG_LLR : -STRONG_LLR;
    }
}

/* The superframe position of window timeslot 0 is @p t0 (1..12, as an I-ISCH sets it), and its bursts dispatch. */
static void
dispatch_window(int t0) {
    g_state->p2_scramble_offset = t0;
    g_state->currentslot = (t0 % 2) ? 1 : 0;
    p25p2_process_frame_scramble(g_opts, g_state);
    p25p2_process_duid(g_opts, g_state);
}

static void
run_window(int t0, const p25p2_bb_slot slots[4]) {
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, t0, slots, bits);
    load_window(bits);
    dispatch_window(t0);
}

/* Marks RS symbols @p symbols of window timeslot @p ts weakest first (LLR 1, 2, ...). */
static void
weaken_symbols(int ts, int duid, const int* symbols, int count) {
    for (int i = 0; i < count; i++) {
        int offsets[6];
        if (!p25p2_bb_symbol_offsets(duid, symbols[i], offsets)) {
            continue;
        }
        for (int b = 0; b < 6; b++) {
            const int bit = (ts * 360) + offsets[b];
            p2llr[bit] = (int16_t)(p2llr[bit] < 0 ? -(i + 1) : (i + 1));
        }
    }
}

static int
rxid_has_nac(const dsd_state* state) {
    return (state->rx_id.valid & DSD_RXID_ACCESS) != 0U && state->rx_id.access_kind == (uint8_t)DSD_ACCESS_CODE_NAC;
}

static void
seed_call(uint8_t slot) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TEST_TG,
        .policy_target_id = TEST_TG,
        .ota_source_id = TEST_SRC,
        .has_service_metadata = 1U,
    };
    (void)dsd_call_state_observe(g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static int
ptt_seen(int slot) {
    return g_state->p25_p2_last_mac_active_m[slot] > 0.0;
}

/* A scrambled burst whose RS decode passes and whose MAC CRC fails proves no seed, names no NAC and marks no TDMA
   carrier, whether a FACCH, a SACCH or an LCCH (CRC-16); the same window with its checksums right proves all three. */
static int
test_seed_proof_needs_crc(void) {
    static const int duids[3] = {DUID_FACCH_S, DUID_SACCH_S, DUID_LCCH_S};
    static const char* const names[3] = {"FACCH", "SACCH", "LCCH"};
    int rc = 0;
    for (int k = 0; k < 3; k++) {
        p25p2_bb_slot slots[4];
        const int bits = duids[k] == DUID_FACCH_S ? 156 : 180;
        char tag[96];
        for (int ts = 0; ts < 4; ts++) {
            mac_header(&slots[ts], duids[k], MAC_HANGTIME);
            slots[ts].mac[1] = 0x7EU;
            DSD_SNPRINTF(tag, sizeof tag, "%s vector's checksum fails", names[k]);
            rc |= expect(tag, break_crc(&slots[ts], bits, duids[k] == DUID_LCCH_S));
        }
        fresh_decoder(0);
        run_window(2, slots);
        DSD_SNPRINTF(tag, sizeof tag, "%s: RS pass with a failed CRC proves no seed", names[k]);
        rc |= expect(tag, g_state->p2_cc_verified == 0U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: RS pass with a failed CRC names no NAC", names[k]);
        rc |= expect(tag, !rxid_has_nac(g_state));
        DSD_SNPRINTF(tag, sizeof tag, "%s: RS pass with a failed CRC marks no TDMA carrier", names[k]);
        rc |= expect(tag, g_state->tdma_carrier == 0U);

        for (int ts = 0; ts < 4; ts++) {
            slots[ts].fill_crc = 1;
        }
        fresh_decoder(0);
        run_window(2, slots);
        DSD_SNPRINTF(tag, sizeof tag, "%s: a checked burst proves the seed", names[k]);
        rc |= expect(tag, g_state->p2_cc_verified == 1U);
        DSD_SNPRINTF(tag, sizeof tag, "%s: a checked burst names the NAC", names[k]);
        rc |= expect(tag, rxid_has_nac(g_state) && g_state->rx_id.access == TEST_NAC);
        DSD_SNPRINTF(tag, sizeof tag, "%s: a checked burst marks the TDMA carrier", names[k]);
        rc |= expect(tag, g_state->tdma_carrier == 1U);
    }
    return rc;
}

/* The burst that proves the seed may carry the MAC_END_PTT that ends its call: the proof comes after the CRC and before
   the MAC PDU, so the ended call's row keeps the NAC (test_p25_p2_reliability.c's stubbed version, here through the
   real CRC and the real END handler). */
static int
test_seed_proof_before_end_ptt(void) {
    int rc = 0;
    fresh_decoder(0);
    g_state->event_history_s = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (g_state->event_history_s == NULL) {
        return expect("event history allocated", 0);
    }
    seed_call(0U);
    p25p2_bb_slot slots[4];
    mac_call(&slots[0], DUID_FACCH_S, MAC_END_PTT);
    for (int ts = 1; ts < 4; ts++) {
        mac_header(&slots[ts], DUID_DATA, 0);
    }
    run_window(2, slots);
    dsd_call_snapshot call;
    rc |= expect("the END_PTT FACCH proves the seed", g_state->p2_cc_verified == 1U);
    rc |= expect("the END_PTT FACCH ends the call",
                 dsd_call_state_get(g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    const Event_History* row = &g_state->event_history_s[0].Event_History_Items[1];
    rc |= expect("the ended call's row carries the proven NAC",
                 row->access_code_kind == (uint8_t)DSD_ACCESS_CODE_NAC && row->access_code == TEST_NAC);
    return rc;
}

/* A FACCH 11 symbols from what was sent and 9 from another codeword: the fixed-erasure decode miscorrects to that
   codeword, whose CRC fails. Erasing the weakest symbols (which are among the 11) then recovers the burst that was
   sent, and that burst is the one dispatched; it leaned on soft erasures, so it proves nothing. */
static int
test_ladder_continues_past_crc_failure(void) {
    enum { OTHER_SYMBOL = 20 };

    p25p2_bb_slot slots[4];
    mac_call(&slots[0], DUID_FACCH_S, MAC_PTT);
    for (int ts = 1; ts < 4; ts++) {
        mac_header(&slots[ts], DUID_DATA, 0);
    }
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, 2, slots, bits);

    /* A weight-20 codeword: one nonzero payload symbol and its 19 parity symbols (all nonzero, the code is MDS). */
    int w_payload[156] = {0};
    int w_parity[114] = {0};
    for (int b = 0; b < 6; b++) {
        w_payload[((OTHER_SYMBOL - 9) * 6) + b] = (0x2D >> (5 - b)) & 1;
    }
    ez_rs28_facch_encode(w_payload, w_parity);
    int rc = 0;
    int parity_symbols[19];
    int n_parity = 0;
    for (int s = 0; s < 19; s++) {
        int nonzero = 0;
        for (int b = 0; b < 6; b++) {
            nonzero |= w_parity[(s * 6) + b];
        }
        if (nonzero) {
            parity_symbols[n_parity++] = 35 + s;
        }
    }
    rc |= expect("the weight-20 codeword has 19 nonzero parity symbols", n_parity == 19);
    /* Received: the sent burst plus 11 of those parity symbols. */
    for (int i = 0; i < 11; i++) {
        int offsets[6];
        (void)p25p2_bb_symbol_offsets(DUID_FACCH_S, parity_symbols[i], offsets);
        for (int b = 0; b < 6; b++) {
            bits[offsets[b]] ^= (uint8_t)w_parity[((parity_symbols[i] - 35) * 6) + b];
        }
    }
    fresh_decoder(0);
    load_window(bits);
    weaken_symbols(0, DUID_FACCH_S, parity_symbols, 4);
    dispatch_window(2);
    rc |= expect("the burst that was sent is dispatched", ptt_seen(0));
    rc |= expect("its decode used soft erasures", g_state->p25_p2_soft_erasure_ok == 1U);
    rc |= expect("a soft-erasure decode proves no seed", g_state->p2_cc_verified == 0U);
    return rc;
}

/* A burst with @p errors symbol errors, the weakest @p weak of them ranked first: whether its MAC_PTT got through. */
static int
ptt_through_errors(int duid, int errors, int weak) {
    p25p2_bb_slot slots[4];
    mac_call(&slots[0], duid, MAC_PTT);
    for (int ts = 1; ts < 4; ts++) {
        mac_header(&slots[ts], DUID_DATA, 0);
    }
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, 2, slots, bits);
    const int first = duid == DUID_FACCH_S ? 9 : 5;
    int symbols[20];
    for (int i = 0; i < errors; i++) {
        symbols[i] = first + 1 + (2 * i);
        p25p2_bb_flip_symbol(bits, 0, symbols[i]);
    }
    fresh_decoder(0);
    load_window(bits);
    weaken_symbols(0, duid, symbols, weak);
    dispatch_window(2);
    /* A FACCH acts on its own slot (0), a SACCH on the other one. */
    return ptt_seen(duid == DUID_FACCH_S ? 0 : 1);
}

/* The soft-erasure ceilings (4 FACCH, 6 SACCH extra erasures): with the 9 / 6 punctured symbols erased, a FACCH
   decodes e soft erasures deep only if 2 x (errors - e) + 9 + e <= 28, a SACCH if 2 x (errors - e) + 6 + e <= 28. */
static int
test_soft_caps(void) {
    int rc = 0;
    rc |= expect("FACCH: 11 errors decode 3 soft erasures deep", ptt_through_errors(DUID_FACCH_S, 11, 6));
    rc |= expect("FACCH: 12 errors (5 soft erasures) do not decode", !ptt_through_errors(DUID_FACCH_S, 12, 8));
    rc |= expect("SACCH: 14 errors decode 6 soft erasures deep", ptt_through_errors(DUID_SACCH_S, 14, 8));
    rc |= expect("SACCH: 15 errors (8 soft erasures) do not decode", !ptt_through_errors(DUID_SACCH_S, 15, 10));
    return rc;
}

/* Signalling leaves the service options (dmr_so for slot 1, dmr_soR for slot 2) alone: a burst whose RS decode fails,
   one whose CRC fails, and a valid MAC_HANGTIME, which updates none. Both slots, FACCH and SACCH. */
static int
test_dmr_so_untouched(void) {
    static const int duids[2] = {DUID_FACCH_S, DUID_SACCH_S};
    static const char* const cases[3] = {"RS failure", "CRC failure", "valid MAC_HANGTIME"};
    int rc = 0;
    for (int k = 0; k < 2; k++) {
        for (int c = 0; c < 3; c++) {
            p25p2_bb_slot slots[4];
            for (int ts = 0; ts < 4; ts++) {
                mac_header(&slots[ts], duids[k], MAC_HANGTIME);
                slots[ts].mac[1] = 0x7EU;
                if (c == 1) {
                    rc |= expect("broken checksum", break_crc(&slots[ts], duids[k] == DUID_FACCH_S ? 156 : 180, 0));
                }
            }
            uint8_t bits[1440];
            p25p2_bb_window(TEST_CODE, 2, slots, bits);
            if (c == 0) {
                for (int ts = 0; ts < 4; ts++) {
                    for (int s = 0; s < 16; s++) {
                        p25p2_bb_flip_symbol(bits, ts, 10 + (2 * s));
                    }
                }
            }
            fresh_decoder(0);
            g_state->dmr_so = 0x5AU;
            g_state->dmr_soR = 0xA5U;
            load_window(bits);
            dispatch_window(2);
            char tag[96];
            DSD_SNPRINTF(tag, sizeof tag, "%s %s keeps slot 1's service options", k == 0 ? "FACCH" : "SACCH", cases[c]);
            rc |= expect(tag, g_state->dmr_so == 0x5AU);
            DSD_SNPRINTF(tag, sizeof tag, "%s %s keeps slot 2's service options", k == 0 ? "FACCH" : "SACCH", cases[c]);
            rc |= expect(tag, g_state->dmr_soR == 0xA5U);
            if (c == 2) {
                DSD_SNPRINTF(tag, sizeof tag, "%s valid MAC_HANGTIMEs dispatched", k == 0 ? "FACCH" : "SACCH");
                rc |= expect(tag, g_vpdu_calls[P25_MAC_PDU_HANGTIME] == 4);
            }
        }
    }
    return rc;
}

/* An unscrambled LCCH (DUID 13) whose RS decode fails, then a valid burst in the same window: the burst is checked
   with its own CRC-12, not the LCCH's CRC-16, and its MAC_PTT gets through. */
static int
lcch_failure_then(int duid) {
    p25p2_bb_slot slots[4];
    mac_header(&slots[0], DUID_LCCH_U, MAC_SIGNAL);
    mac_call(&slots[1], duid, MAC_PTT);
    mac_header(&slots[2], DUID_DATA, 0);
    mac_header(&slots[3], DUID_DATA, 0);
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, 2, slots, bits);
    for (int s = 0; s < 16; s++) {
        p25p2_bb_flip_symbol(bits, 0, 6 + (3 * s));
    }
    fresh_decoder(0);
    load_window(bits);
    dispatch_window(2);
    /* Window timeslot 1 is superframe slot 3, slot 2: a FACCH acts on slot 2, a SACCH on slot 1. */
    return ptt_seen(duid == DUID_FACCH_S ? 1 : 0) && g_state->p2_is_lcch == 0;
}

static int
test_lcch_flag_does_not_leak_to_facch(void) {
    return expect("a failed LCCH leaves the next FACCH a FACCH", lcch_failure_then(DUID_FACCH_S));
}

static int
test_lcch_flag_does_not_leak_to_sacch(void) {
    return expect("a failed LCCH leaves the next SACCH a SACCH", lcch_failure_then(DUID_SACCH_S));
}

/* What -F (Relaxed CRC) lets through from bursts whose RS decode passes and whose CRC fails, against strict CRC:
   neither proves a seed, names a NAC or marks a carrier, and PTT, END_PTT and ACTIVE never act, as before issue #804. */
static int
relaxed_calls(int relaxed) {
    char tag[128];
    const char* mode = relaxed ? "-F" : "strict";
    int rc = 0;
    p25p2_bb_slot slots[4];
    mac_call(&slots[0], DUID_FACCH_S, MAC_PTT);
    mac_call(&slots[1], DUID_SACCH_S, MAC_PTT);
    mac_call(&slots[2], DUID_SACCH_S, MAC_END_PTT);
    slots[2].mac[1] = 0x00U;
    mac_call(&slots[3], DUID_SACCH_S, MAC_ACTIVE);
    slots[3].mac[1] = 0x00U;
    for (int ts = 0; ts < 4; ts++) {
        rc |= expect("broken checksum", break_crc(&slots[ts], ts == 0 ? 156 : 180, 0));
    }
    fresh_decoder(relaxed);
    seed_call(0U);
    seed_call(1U);
    run_window(2, slots);
    dsd_call_snapshot call0;
    dsd_call_snapshot call1;
    const int both_active = dsd_call_state_get(g_state, 0U, &call0) > 0 && call0.phase == DSD_CALL_PHASE_ACTIVE
                            && dsd_call_state_get(g_state, 1U, &call1) > 0 && call1.phase == DSD_CALL_PHASE_ACTIVE;
    DSD_SNPRINTF(tag, sizeof tag, "%s: failed CRCs prove no seed", mode);
    rc |= expect(tag, g_state->p2_cc_verified == 0U && !rxid_has_nac(g_state) && g_state->tdma_carrier == 0U);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no CRC-failed PTT acts", mode);
    rc |= expect(tag, !ptt_seen(0) && !ptt_seen(1));
    DSD_SNPRINTF(tag, sizeof tag, "%s: no CRC-failed END_PTT acts", mode);
    rc |= expect(tag, g_state->p25_p2_last_end_ptt[0] == 0 && g_state->p25_p2_last_end_ptt[1] == 0 && both_active);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no CRC-failed ACTIVE acts", mode);
    rc |= expect(tag, g_vpdu_calls[P25_MAC_PDU_ACTIVE] == 0);
    return rc;
}

/* The one fallback: a CRC-failed SACCH MAC_SIGNAL whose second octet is zero reaches the VPDU decoder with its failed
   verdict (both modes), and under -F an LCCH MAC_SIGNAL whose second octet is not; a SACCH MAC_SIGNAL whose second
   octet is not never does. None of them proves anything. */
static int
relaxed_signals(int relaxed) {
    char tag[128];
    const char* mode = relaxed ? "-F" : "strict";
    int rc = 0;
    p25p2_bb_slot signals[3];
    mac_header(&signals[0], DUID_SACCH_S, MAC_SIGNAL);
    mac_header(&signals[1], DUID_SACCH_S, MAC_SIGNAL);
    signals[1].mac[1] = 0x7BU;
    mac_header(&signals[2], DUID_LCCH_S, MAC_SIGNAL);
    signals[2].mac[1] = 0x7BU;
    rc |= expect("broken checksum", break_crc(&signals[0], 180, 0));
    rc |= expect("broken checksum", break_crc(&signals[1], 180, 0));
    rc |= expect("broken checksum", break_crc(&signals[2], 180, 1));
    for (int k = 0; k < 3; k++) {
        p25p2_bb_slot slots[4];
        for (int ts = 0; ts < 4; ts++) {
            mac_header(&slots[ts], DUID_DATA, 0);
        }
        slots[1] = signals[k];
        fresh_decoder(relaxed);
        run_window(2, slots);
        const int want = k == 0 || (k == 2 && relaxed);
        DSD_SNPRINTF(tag, sizeof tag, "%s: CRC-failed MAC_SIGNAL %d reaches the VPDU as before", mode, k);
        rc |= expect(tag, g_vpdu_calls[P25_MAC_PDU_SIGNAL] == want);
        if (want) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: CRC-failed MAC_SIGNAL %d carries its failed verdict", mode, k);
            rc |= expect(tag, g_vpdu_signal_crc_ok == 0);
        }
        DSD_SNPRINTF(tag, sizeof tag, "%s: CRC-failed MAC_SIGNAL %d proves nothing", mode, k);
        rc |= expect(tag, g_state->p2_cc_verified == 0U && !rxid_has_nac(g_state) && g_state->tdma_carrier == 0U);
    }
    return rc;
}

static int
test_relaxed_crc_never_proves(void) {
    return relaxed_calls(1) | relaxed_signals(1) | relaxed_calls(0) | relaxed_signals(0);
}

/* A DUID 10 burst (a scrambled data channel burst) is a known kind, not a DUID error: two of them do not abort the
   window, and the SACCH after them dispatches. */
static int
test_duid10_is_not_an_error(void) {
    p25p2_bb_slot slots[4];
    mac_header(&slots[0], DUID_DATA, 0);
    mac_header(&slots[1], DUID_DATA, 0);
    mac_call(&slots[2], DUID_SACCH_S, MAC_PTT);
    mac_header(&slots[3], DUID_SACCH_S, MAC_HANGTIME);
    slots[3].mac[1] = 0x7EU;
    fresh_decoder(0);
    run_window(8, slots);
    int rc = 0;
    /* Window timeslot 2 is superframe slot 10, slot 1: its SACCH acts on slot 2. */
    rc |= expect("the SACCH after two DUID 10 bursts dispatches", ptt_seen(1));
    rc |= expect("the window's last SACCH dispatches too", g_vpdu_calls[P25_MAC_PDU_HANGTIME] == 1);
    return rc;
}

/* One burst of @p duid carrying MAC @p opcode at window timeslot @p ts, the rest DUID 10, in the window at superframe
   slot 2 (timeslot 0 is slot 1's, timeslot 1 slot 2's), read under the current synctype @p sync. */
static void
run_one_burst(int duid, int opcode, int ts, int sync) {
    p25p2_bb_slot slots[4];
    for (int i = 0; i < 4; i++) {
        mac_header(&slots[i], DUID_DATA, 0);
    }
    mac_call(&slots[ts], duid, opcode);
    fresh_decoder(0);
    g_state->synctype = sync;
    run_window(2, slots);
}

/* Any MAC whose real checksum passed is CRC-checked P25 traffic, so a Signal search takes it as evidence (issue #804):
   MAC_HANGTIME, MAC_IDLE and MAC_END_PTT as well as PTT and ACTIVE, once per burst, named by the decoder's current
   Phase 2 synctype and the slot the MAC belongs to. It is traffic, not control channel activity. An LCCH, which is
   control channel signalling, reports itself once, as control channel activity. */
static int
test_hangtime_idle_end_are_evidence(void) {
    static const int duids[2] = {DUID_FACCH_S, DUID_SACCH_S};
    static const char* const kinds[2] = {"FACCH", "SACCH"};
    static const int opcodes[3] = {MAC_HANGTIME, MAC_IDLE, MAC_END_PTT};
    static const char* const macs[3] = {"MAC_HANGTIME", "MAC_IDLE", "MAC_END_PTT"};
    int rc = 0;
    char tag[128];
    for (int k = 0; k < 2; k++) {
        for (int m = 0; m < 3; m++) {
            run_one_burst(duids[k], opcodes[m], 0, DSD_SYNC_P25P2_NEG);
            DSD_SNPRINTF(tag, sizeof tag, "%s %s is evidence once", kinds[k], macs[m]);
            rc |= expect(tag, g_evidence == 1);
            DSD_SNPRINTF(tag, sizeof tag, "%s %s evidence names P25", kinds[k], macs[m]);
            rc |= expect(tag, strcmp(g_evidence_protocol, "P25") == 0);
            DSD_SNPRINTF(tag, sizeof tag, "%s %s evidence names the current synctype", kinds[k], macs[m]);
            rc |= expect(tag, g_evidence_sync == DSD_SYNC_P25P2_NEG);
            /* Timeslot 0 is slot 1's: a FACCH's MAC is its own slot's, a SACCH's the other slot's. */
            DSD_SNPRINTF(tag, sizeof tag, "%s %s evidence names the MAC's slot", kinds[k], macs[m]);
            rc |= expect(tag, g_evidence_slot == (duids[k] == DUID_FACCH_S ? 0 : 1));
            DSD_SNPRINTF(tag, sizeof tag, "%s %s is no control channel activity", kinds[k], macs[m]);
            rc |= expect(tag, g_state->last_cc_sync_time == 0 && g_state->p25_last_cc_msg_time == 0);
        }
    }
    /* Outside a Phase 2 sync (a replayed window, a test) the synctype is Phase 2's positive one. */
    run_one_burst(DUID_SACCH_S, MAC_IDLE, 0, DSD_SYNC_P25P1_POS);
    rc |= expect("a SACCH under another synctype names Phase 2's",
                 g_evidence == 1 && g_evidence_sync == DSD_SYNC_P25P2_POS);

    p25p2_bb_slot slots[4];
    for (int i = 0; i < 4; i++) {
        mac_header(&slots[i], DUID_DATA, 0);
    }
    mac_header(&slots[0], DUID_LCCH_S, MAC_SIGNAL);
    slots[0].mac[1] = 0x7BU;
    fresh_decoder(0);
    run_window(2, slots);
    rc |= expect("a checked LCCH is evidence once, as control channel activity",
                 g_evidence == 1 && g_evidence_sync == -1 && g_state->p25_last_cc_msg_time != 0);
    return rc;
}

/* A burst whose MAC checksum failed is no evidence, strict or under -F, whatever reached the MAC handlers: a FACCH, a
   SACCH and the two MAC_SIGNAL fallbacks (a SACCH whose second octet is zero, and under -F an LCCH); nor is a burst
   whose RS decode failed. */
static int
crc_failed_evidence(int relaxed) {
    const char* mode = relaxed ? "-F" : "strict";
    char tag[128];
    int rc = 0;
    p25p2_bb_slot slots[4];
    mac_call(&slots[0], DUID_FACCH_S, MAC_HANGTIME);
    mac_call(&slots[1], DUID_SACCH_S, MAC_IDLE);
    mac_header(&slots[2], DUID_SACCH_S, MAC_SIGNAL);
    mac_header(&slots[3], DUID_LCCH_S, MAC_SIGNAL);
    slots[3].mac[1] = 0x7BU;
    rc |= expect("broken checksum", break_crc(&slots[0], 156, 0));
    rc |= expect("broken checksum", break_crc(&slots[1], 180, 0));
    rc |= expect("broken checksum", break_crc(&slots[2], 180, 0));
    rc |= expect("broken checksum", break_crc(&slots[3], 180, 1));
    fresh_decoder(relaxed);
    run_window(2, slots);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the MAC_SIGNAL fallbacks reached the VPDU", mode);
    rc |= expect(tag, g_vpdu_calls[P25_MAC_PDU_SIGNAL] == (relaxed ? 2 : 1));
    DSD_SNPRINTF(tag, sizeof tag, "%s: no CRC-failed MAC is evidence", mode);
    rc |= expect(tag, g_evidence == 0);

    for (int ts = 0; ts < 4; ts++) {
        mac_call(&slots[ts], ts % 2 ? DUID_SACCH_S : DUID_FACCH_S, MAC_IDLE);
    }
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, 2, slots, bits);
    for (int ts = 0; ts < 4; ts++) {
        for (int s = 0; s < 16; s++) {
            p25p2_bb_flip_symbol(bits, ts, 10 + (2 * s));
        }
    }
    fresh_decoder(relaxed);
    load_window(bits);
    dispatch_window(2);
    DSD_SNPRINTF(tag, sizeof tag, "%s: the RS decodes failed", mode);
    rc |= expect(tag, g_state->p25_p2_rs_facch_err == 2U && g_state->p25_p2_rs_sacch_err == 2U);
    DSD_SNPRINTF(tag, sizeof tag, "%s: no RS-failed burst is evidence", mode);
    rc |= expect(tag, g_evidence == 0);
    return rc;
}

static int
test_crc_failed_mac_is_not_evidence(void) {
    return crc_failed_evidence(0) | crc_failed_evidence(1);
}

/* One active slot on a two-slot channel: a SACCH sent in one slot's timeslot announces the other slot's voice, so its
   evidence names that other slot whichever timeslot carries it (the receive policy judges that slot, issue #637); a
   FACCH names its own. */
static int
test_sacch_evidence_names_opposite_slot(void) {
    int rc = 0;
    run_one_burst(DUID_SACCH_S, MAC_HANGTIME, 0, DSD_SYNC_P25P2_POS);
    rc |= expect("a SACCH in slot 1's timeslot names slot 2", g_evidence == 1 && g_evidence_slot == 1);
    run_one_burst(DUID_SACCH_S, MAC_HANGTIME, 1, DSD_SYNC_P25P2_POS);
    rc |= expect("a SACCH in slot 2's timeslot names slot 1", g_evidence == 1 && g_evidence_slot == 0);
    run_one_burst(DUID_FACCH_S, MAC_HANGTIME, 1, DSD_SYNC_P25P2_POS);
    rc |= expect("a FACCH in slot 2's timeslot names slot 2", g_evidence == 1 && g_evidence_slot == 1);
    return rc;
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    if (g_opts == NULL || g_state == NULL) {
        return 1;
    }
    dsd_signal_search_evidence_set(capture_evidence);
    int rc = 0;
    rc |= test_seed_proof_needs_crc();
    rc |= test_seed_proof_before_end_ptt();
    rc |= test_ladder_continues_past_crc_failure();
    rc |= test_soft_caps();
    rc |= test_dmr_so_untouched();
    rc |= test_lcch_flag_does_not_leak_to_facch();
    rc |= test_lcch_flag_does_not_leak_to_sacch();
    rc |= test_relaxed_crc_never_proves();
    rc |= test_duid10_is_not_an_error();
    rc |= test_hangtime_idle_end_are_evidence();
    rc |= test_crc_failed_mac_is_not_evidence();
    rc |= test_sacch_evidence_names_opposite_slot();
    dsd_signal_search_evidence_set(NULL);
    dsd_state_ext_free_all(g_state);
    free(g_state->event_history_s);
    free(g_state);
    free(g_opts);
    if (rc == 0) {
        printf("P25_P2_ACCH_VERDICT: OK\n");
    }
    return rc;
}
