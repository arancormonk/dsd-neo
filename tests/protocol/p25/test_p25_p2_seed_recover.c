// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 scrambling code recovery (issue #804). Windows are built as a site sends them (p25p2_burst_builder.h)
 * and read by processP2() through a stand-in symbol reader, so the window position, the recovery, the descrambling
 * and the real Reed-Solomon, CRC and XCCH handlers all run. Only the VPDU decoder is a stand-in, which counts what
 * reached it.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/p25/p25.h>
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
#include "p25p2_seed_recover.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];

#define CODE_OF(w, s, n) ((((uint64_t)(w)) << 24) | (((uint64_t)(s)) << 12) | (uint64_t)(n))
#define TEST_CODE        CODE_OF(0xBEE00U, 0x164U, 0x161U)
#define OTHER_CODE       CODE_OF(0x92065U, 0x0D5U, 0x2A7U)
#define TEST_T0_NS       UINT64_C(1000000000000)

enum {
    DUID_SACCH_S = 3,
    DUID_FACCH_S = 9,
    DUID_DATA = 10,
    MAC_IDLE = 3,
    STRONG_LLR = 200,
    WEAK_LLR = 10,
    HIGH_LLR = 250,
    /* Windows read back to back (the next sync follows), or read with a gap no whole number of timeslots explains:
       continuity places the first, only the window's own I-ISCH the second. */
    GAP_CONTIGUOUS = 20,
    GAP_INDEPENDENT = 27,
    WINDOW_DIBITS = 700,
};

static dsd_opts* g_opts;
static dsd_state* g_state;

/* The window the stand-in symbol reader hands processP2(): bits in p2bit layout, and their LLRs. */
static uint8_t g_bits[1440];
static int16_t g_llr[1400];
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
        llr0 = g_llr[bit];
        llr1 = g_llr[bit + 1];
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

/* What reached the VPDU decoder, by PDU type. */
static int g_vpdu_calls[8];

void
process_MAC_VPDU_crc(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                     unsigned long long int mac[24], int crc_ok) {
    (void)opts;
    (void)state;
    (void)type;
    (void)mac;
    (void)crc_ok;
    if ((int)pdu_type >= 0 && (int)pdu_type < 8) {
        g_vpdu_calls[pdu_type]++;
    }
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                 unsigned long long int mac[24]) {
    process_MAC_VPDU_crc(opts, state, type, pdu_type, mac, 1);
}

/* The Signal search side of the hooks: the learned codes it offers, and what it was told. */
static uint64_t g_learned[16];
static size_t g_n_learned;
static int g_learned_queries;
static int g_code_notes;
static uint64_t g_code_noted;
static int g_code_noted_source;
static int g_evidence_notes;
static int g_call_notes;

static size_t
learned_codes(const dsd_state* state, uint64_t* out, size_t cap) {
    (void)state;
    g_learned_queries++;
    size_t n = g_n_learned < cap ? g_n_learned : cap;
    for (size_t i = 0; i < n; i++) {
        out[i] = g_learned[i];
    }
    return n;
}

static void
code_note(const dsd_state* state, uint64_t code, int source) {
    (void)state;
    g_code_notes++;
    g_code_noted = code;
    g_code_noted_source = source;
}

static void
evidence_note(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target, uint32_t source,
              const char* access) {
    (void)state;
    (void)protocol;
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    g_evidence_notes++;
}

static void
call_note(const dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
    g_call_notes++;
}

static int
expect(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static uint64_t g_rng = 0x804804ULL;

static uint32_t
rng32(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

/* A fresh decoder holding no code, strict CRC unless @p relaxed. */
static void
fresh_decoder(int relaxed) {
    dsd_state_ext_free_all(g_state);
    DSD_MEMSET(g_opts, 0, sizeof(*g_opts));
    DSD_MEMSET(g_state, 0, sizeof(*g_state));
    dsd_opts_set_relaxed_crc(g_opts, relaxed);
    dsd_decode_clock_use_test(TEST_T0_NS);
    p25_p2_frame_reset();
    DSD_MEMSET(g_vpdu_calls, 0, sizeof(g_vpdu_calls));
    g_n_learned = 0;
    g_learned_queries = 0;
    g_code_notes = 0;
    g_code_noted = 0;
    g_code_noted_source = 0;
    g_evidence_notes = 0;
    g_call_notes = 0;
}

static void
hold_code(uint64_t code) {
    g_state->p2_wacn = code >> 24;
    g_state->p2_sysid = (code >> 12) & 0xFFFU;
    g_state->p2_cc = code & 0xFFFU;
}

static uint64_t
code_in_force(void) {
    return CODE_OF(g_state->p2_wacn, g_state->p2_sysid, g_state->p2_cc);
}

static uint64_t
pending_code(void) {
    uint64_t code = 0;
    return p25p2_seed_recover_pending(g_state, &code) ? code : 0U;
}

/* A MAC_IDLE whose payload varies by @p salt, CRC filled. */
static void
idle_slot(p25p2_bb_slot* slot, int duid, int salt) {
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->duid = duid;
    slot->fill_crc = 1;
    slot->mac[0] = (uint8_t)(MAC_IDLE << 5);
    for (int i = 1; i < 18; i++) {
        slot->mac[i] = (uint8_t)((salt * 37) + (i * 11));
    }
}

/* Four DATCH bursts (DUID 10), which recovery passes over. */
static void
data_slots(p25p2_bb_slot slots[4]) {
    for (int ts = 0; ts < 4; ts++) {
        DSD_MEMSET(&slots[ts], 0, sizeof(slots[ts]));
        slots[ts].duid = DUID_DATA;
    }
}

/* The window starting at superframe slot @p t0, scrambled with @p code, every bit read with a strong LLR. */
static void
build(uint64_t code, int t0, const p25p2_bb_slot slots[4]) {
    p25p2_bb_window(code, t0, slots, g_bits);
    for (int i = 0; i < 1400; i++) {
        g_llr[i] = (int16_t)(g_bits[i] ? STRONG_LLR : -STRONG_LLR);
    }
}

/* Every bit of RS symbol @p rs_index of window timeslot @p ts read with LLR magnitude @p magnitude. */
static void
symbol_llr(int ts, int duid, int rs_index, int magnitude) {
    int offsets[6];
    if (!p25p2_bb_symbol_offsets(duid, rs_index, offsets)) {
        return;
    }
    for (int b = 0; b < 6; b++) {
        const int bit = (ts * 360) + offsets[b];
        g_llr[bit] = (int16_t)(g_bits[bit] ? magnitude : -magnitude);
    }
}

/* RS symbol @p rs_index of window timeslot @p ts received wrong, with LLR magnitude @p magnitude. */
static void
corrupt(int ts, int duid, int rs_index, int magnitude) {
    p25p2_bb_flip_symbol(g_bits, ts, rs_index);
    symbol_llr(ts, duid, rs_index, magnitude);
}

/* processP2() reads the built window @p gap dibits after the previous one ended. */
static void
feed(uint32_t gap) {
    g_state->symbolcnt += gap;
    g_feed_pos = 0;
    processP2(g_opts, g_state);
    g_feed_pos = -1;
}

/* The ISCH of the window built at @p t0 in place of the built window's own: the window is read as starting there. */
static void
relabel(uint64_t code, int t0, const p25p2_bb_slot slots[4]) {
    uint8_t other[1440];
    p25p2_bb_window(code, t0, slots, other);
    for (int ts = 0; ts < 4; ts++) {
        for (int i = 320; i < 360; i++) {
            g_bits[(ts * 360) + i] = other[(ts * 360) + i];
        }
    }
}

/* One window with one scrambled signalling burst at window timeslot @p ts, the rest DATCH. */
static void
one_burst_window(uint64_t code, int t0, int ts, int duid, int salt) {
    p25p2_bb_slot slots[4];
    data_slots(slots);
    idle_slot(&slots[ts], duid, salt);
    build(code, t0, slots);
}

/* A window start (a superframe slot after an S-ISCH, where a sync starts a window) whose window holds @p t, and the
   timeslot @p t falls in. @p which picks one of the two such windows. */
static void
window_holding(int t, int which, int* t0, int* ts) {
    static const int starts[6] = {2, 3, 6, 7, 10, 11};
    int found = 0;
    for (int i = 0; i < 6; i++) {
        const int at = (t - starts[i] + 12) % 12;
        if (at < 4) {
            if (found == which) {
                *t0 = starts[i];
                *ts = at;
                return;
            }
            found++;
        }
    }
}

static int
parity64(uint64_t v) {
    return dsd_popcount64(v) & 1;
}

/* The tables against the builder, which scrambles with p25p2_generate_scramble_bits() and encodes with the RS
   encoder: the burst bits are the builder's, every check row XORs to 0 over a codeword, and over a scrambled
   all-zero codeword each check row's XOR is the seed row's parity with the code, at every T. */
static int
test_tables_match_direct_encoding(void) {
    const ez_p25p2_seed_tables* tables = ez_p25p2_seed_tables_get();
    int rc = expect("the tables are built", tables != NULL);
    if (tables == NULL) {
        return rc;
    }
    static const int duids[2] = {DUID_FACCH_S, DUID_SACCH_S};
    static const int first_symbol[2] = {9, 5};
    int layout_ok = 1;
    int checks_ok = 1;
    int seeds_ok = 1;
    for (int kind = 0; kind < 2; kind++) {
        const int n_bits = tables->payload_bits[kind] + tables->checks[kind];
        layout_ok &= tables->payload_bits[kind] == (kind ? 180 : 156) && tables->checks[kind] == (kind ? 132 : 114);
        for (int k = 0; k < n_bits; k++) {
            int offsets[6];
            const int rs_index = (k < tables->payload_bits[kind]) ? first_symbol[kind] + (k / 6)
                                                                  : 35 + ((k - tables->payload_bits[kind]) / 6);
            layout_ok &=
                p25p2_bb_symbol_offsets(duids[kind], rs_index, offsets) && offsets[k % 6] == tables->burst_bit[kind][k];
        }
        for (int trial = 0; trial < 8; trial++) {
            const uint64_t code = CODE_OF(1 + (rng32() % 0xFFFFEU), 1 + (rng32() % 0xFFEU), 1 + (rng32() % 0xFFEU));
            for (int t = 0; t < 12; t++) {
                int t0 = 0;
                int ts = 0;
                window_holding(t, trial & 1, &t0, &ts);
                p25p2_bb_slot slots[4];
                data_slots(slots);
                idle_slot(&slots[ts], duids[kind], trial);
                uint8_t clear[1440];
                p25p2_bb_window(0U, t0, slots, clear);
                slots[ts].fill_crc = 0;
                DSD_MEMSET(slots[ts].mac, 0, sizeof(slots[ts].mac));
                uint8_t zero[1440];
                p25p2_bb_window(code, t0, slots, zero);
                for (int i = 0; i < tables->checks[kind]; i++) {
                    int clear_ones = 0;
                    int zero_ones = 0;
                    for (int k = 0; k < n_bits; k++) {
                        if ((tables->check[kind][i][k / 64] >> (k % 64)) & 1U) {
                            clear_ones += clear[(ts * 360) + tables->burst_bit[kind][k]];
                            zero_ones += zero[(ts * 360) + tables->burst_bit[kind][k]];
                        }
                    }
                    checks_ok &= (clear_ones & 1) == 0;
                    seeds_ok &= (zero_ones & 1) == parity64(tables->seed[kind][t][i] & code);
                }
            }
        }
    }
    rc |= expect("the tables' burst bits are the builder's", layout_ok);
    rc |= expect("every check row XORs to 0 over a codeword", checks_ok);
    rc |= expect("every seed row matches the scrambled zero codeword", seeds_ok);
    return rc;
}

/* One clean burst at any superframe slot, FACCH or SACCH, in either window that holds it, gives the code: it waits
   for confirmation and nothing is committed. */
static int
test_solve_every_slot_facch_sacch(void) {
    static const int duids[2] = {DUID_FACCH_S, DUID_SACCH_S};
    int solved = 0;
    int committed = 0;
    for (int kind = 0; kind < 2; kind++) {
        for (int t = 0; t < 12; t++) {
            for (int which = 0; which < 2; which++) {
                int t0 = 0;
                int ts = 0;
                window_holding(t, which, &t0, &ts);
                fresh_decoder(0);
                one_burst_window(TEST_CODE, t0, ts, duids[kind], t);
                feed(GAP_INDEPENDENT);
                solved += pending_code() == TEST_CODE;
                committed += code_in_force() != 0U;
            }
        }
    }
    int rc = expect("every slot, both kinds, both windows: solved", solved == 48);
    rc |= expect("a solved code alone is not committed", committed == 0);
    return rc;
}

/* Two wrong symbols the receiver read with the least confidence: erased, they leave the code. */
static int
test_solve_with_symbol_errors(void) {
    static const int duids[2] = {DUID_FACCH_S, DUID_SACCH_S};
    static const int symbols[2][2] = {{12, 40}, {7, 50}};
    int rc = 0;
    for (int kind = 0; kind < 2; kind++) {
        fresh_decoder(0);
        one_burst_window(TEST_CODE, 10, 1, duids[kind], kind);
        corrupt(1, duids[kind], symbols[kind][0], WEAK_LLR);
        corrupt(1, duids[kind], symbols[kind][1], WEAK_LLR + 1);
        feed(GAP_INDEPENDENT);
        rc |= expect(kind ? "SACCH with two weak wrong symbols solves" : "FACCH with two weak wrong symbols solves",
                     pending_code() == TEST_CODE);
    }
    return rc;
}

/* A wrong symbol read with more confidence than any other (as on air right after the sync): erasure never reaches
   it, so the burst gives no code, and never a wrong one. */
static int
test_high_llr_prefix_error(void) {
    fresh_decoder(0);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 5);
    for (int i = 0; i < 1400; i++) {
        const int magnitude = 100 + (int)(rng32() % 140U);
        g_llr[i] = (int16_t)(g_bits[i] ? magnitude : -magnitude);
    }
    corrupt(0, DUID_SACCH_S, 5, HIGH_LLR);
    feed(GAP_INDEPENDENT);
    const uint64_t pending = pending_code();
    return expect("a high-LLR wrong symbol gives the code or none, never another",
                  pending == 0U || pending == TEST_CODE);
}

/* Random windows with signalling DUIDs and a located position: whatever they give, nothing is committed. */
static int
test_noise_never_accepted(void) {
    enum { NOISE_WINDOWS = 20000 };

    static const int starts[6] = {2, 3, 6, 7, 10, 11};
    static const uint8_t duid_codewords[2] = {0x39U, 0x9AU}; /* DUID 3 and 9 */
    static const int duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    fresh_decoder(0);
    int candidates = 0;
    for (int w = 0; w < NOISE_WINDOWS; w++) {
        for (int i = 0; i < 1400; i++) {
            const uint32_t r = rng32();
            p2bit[i] = (int)(r & 1U);
            p2llr[i] = (int16_t)(p2bit[i] ? 1 + (int)((r >> 1) % 255U) : -1 - (int)((r >> 1) % 255U));
        }
        const int ts = (int)(rng32() % 4U);
        for (int i = 0; i < 8; i++) {
            p2bit[(ts * 360) + duid_offsets[i]] = (duid_codewords[w & 1] >> (7 - i)) & 1;
            p2llr[(ts * 360) + duid_offsets[i]] = (int16_t)(p2bit[(ts * 360) + duid_offsets[i]] ? 255 : -255);
        }
        g_state->p2_scramble_offset = starts[rng32() % 6U];
        const uint64_t before = pending_code();
        p25p2_seed_recover_window(g_opts, g_state, 1, 0);
        const uint64_t after = pending_code();
        candidates += after != 0U && after != before;
    }
    int rc = expect("noise commits no code", code_in_force() == 0U && g_code_notes == 0);
    if (candidates > 0) {
        DSD_FPRINTF(stderr, "note: %d noise windows gave a candidate\n", candidates);
    }
    rc |= expect("noise gives (almost) no candidate", candidates <= 2);
    return rc;
}

/* A window whose I-ISCH misnames its position (here 4 timeslots early) solves to a shifted code that decodes every
   burst mislabelled the same way. A window its own I-ISCH places right cannot confirm it; that window's own code
   replaces it, and a third window confirms the real code. */
static int
test_shifted_seed_rejected(void) {
    fresh_decoder(0);
    p25p2_bb_slot slots[4];
    data_slots(slots);
    idle_slot(&slots[0], DUID_SACCH_S, 1);
    idle_slot(&slots[1], DUID_FACCH_S, 2);
    build(TEST_CODE, 6, slots);
    relabel(TEST_CODE, 2, slots);
    feed(GAP_INDEPENDENT);
    const uint64_t shifted = pending_code();
    int rc = expect("the mislabelled window gives a candidate", shifted != 0U);
    rc |= expect("the candidate is a shifted code", shifted != TEST_CODE);
    rc |= expect("two bursts of the mislabelled window do not confirm it", code_in_force() == 0U);

    one_burst_window(TEST_CODE, 10, 1, DUID_SACCH_S, 3);
    feed(GAP_INDEPENDENT);
    rc |= expect("a rightly placed window does not confirm the shifted code", code_in_force() == 0U);
    rc |= expect("its own code replaces the shifted one", pending_code() == TEST_CODE);

    one_burst_window(TEST_CODE, 6, 0, DUID_FACCH_S, 4);
    feed(GAP_INDEPENDENT);
    rc |= expect("another rightly placed window confirms the real code", code_in_force() == TEST_CODE);
    rc |= expect("the confirming burst proves it", g_state->p2_cc_verified == 1U);
    return rc;
}

/* Two bursts of one window at different slots: the second passes under the first one's code, but one window's
   position is one observation. The next window confirms it, and the window decodes. */
static int
test_same_window_does_not_confirm(void) {
    fresh_decoder(0);
    p25p2_bb_slot slots[4];
    data_slots(slots);
    idle_slot(&slots[0], DUID_SACCH_S, 1);
    idle_slot(&slots[1], DUID_SACCH_S, 2);
    build(TEST_CODE, 10, slots);
    feed(GAP_INDEPENDENT);
    int rc = expect("one window: the code waits", pending_code() == TEST_CODE && code_in_force() == 0U);
    rc |= expect("one window: nothing decoded", g_vpdu_calls[P25_MAC_PDU_IDLE] == 0);

    one_burst_window(TEST_CODE, 6, 2, DUID_FACCH_S, 3);
    feed(GAP_INDEPENDENT);
    rc |= expect("another window confirms it", code_in_force() == TEST_CODE);
    rc |= expect("and is reported as recovered",
                 g_code_notes == 1 && g_code_noted == TEST_CODE && g_code_noted_source == DSD_P25P2_CODE_RECOVERED);
    rc |= expect("the confirming burst proves the code", g_state->p2_cc_verified == 1U);
    rc |= expect("the confirming window's MAC decodes", g_vpdu_calls[P25_MAC_PDU_IDLE] == 1);
    rc |= expect("the received NAC is the recovered one",
                 (g_state->rx_id.valid & DSD_RXID_ACCESS) != 0U && g_state->rx_id.access == 0x161U);
    rc |= expect("nothing waits any more", pending_code() == 0U);
    return rc;
}

/* The SACCH at slot 10 of successive superframes: a wrong code's verdict is the same at one slot, so the second
   window confirms nothing. Slot 11 of a later window does. */
static int
test_same_t_does_not_confirm(void) {
    fresh_decoder(0);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
    feed(GAP_INDEPENDENT);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 2);
    feed(GAP_INDEPENDENT);
    int rc = expect("the same slot twice confirms nothing", code_in_force() == 0U && pending_code() == TEST_CODE);
    one_burst_window(TEST_CODE, 10, 1, DUID_SACCH_S, 3);
    feed(GAP_INDEPENDENT);
    rc |= expect("another slot in another window does", code_in_force() == TEST_CODE);
    return rc;
}

/* Six wrong symbols read with full confidence: past what the erasures can reach, so the burst does not solve, yet
   within what the code corrects. */
static void
six_strong_errors(int ts, int duid) {
    static const int symbols[6] = {11, 15, 20, 26, 33, 40};
    for (int i = 0; i < 6; i++) {
        corrupt(ts, duid, symbols[i], HIGH_LLR);
    }
}

/* The pending code is tried before anything is solved: a burst that does not solve confirms it when it decodes. */
static int
test_pending_confirmed_by_unsolvable_burst(void) {
    fresh_decoder(0);
    one_burst_window(TEST_CODE, 6, 1, DUID_FACCH_S, 1);
    six_strong_errors(1, DUID_FACCH_S);
    feed(GAP_INDEPENDENT);
    int rc = expect("the six-error burst alone does not solve", pending_code() == 0U);

    fresh_decoder(0);
    one_burst_window(TEST_CODE, 10, 1, DUID_SACCH_S, 2);
    feed(GAP_INDEPENDENT);
    rc |= expect("a clean burst gives the candidate", pending_code() == TEST_CODE);
    one_burst_window(TEST_CODE, 6, 1, DUID_FACCH_S, 1);
    six_strong_errors(1, DUID_FACCH_S);
    feed(GAP_INDEPENDENT);
    rc |= expect("the six-error burst confirms it", code_in_force() == TEST_CODE);
    rc |= expect("and proves it as it decodes", g_state->p2_cc_verified == 1U && g_vpdu_calls[P25_MAC_PDU_IDLE] == 1);
    return rc;
}

/* The session's learned codes are tried before a solve: the right one among wrong ones becomes the candidate from a
   burst that does not solve, and another such burst confirms it. The codes are read once per window at most. */
static int
test_learned_code_accepted(void) {
    fresh_decoder(0);
    g_learned[0] = OTHER_CODE;
    g_learned[1] = CODE_OF(0x12345U, 0x678U, 0x9ABU);
    g_learned[2] = TEST_CODE;
    g_learned[3] = CODE_OF(0xBEE00U, 0x164U, 0x162U);
    g_n_learned = 4;
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
    six_strong_errors(0, DUID_SACCH_S);
    feed(GAP_INDEPENDENT);
    int rc = expect("a learned code becomes the candidate", pending_code() == TEST_CODE && code_in_force() == 0U);
    one_burst_window(TEST_CODE, 2, 1, DUID_FACCH_S, 2);
    six_strong_errors(1, DUID_FACCH_S);
    feed(GAP_INDEPENDENT);
    rc |= expect("another window confirms it", code_in_force() == TEST_CODE && g_state->p2_cc_verified == 1U);
    rc |= expect("the learned codes are read at most once a window", g_learned_queries >= 1 && g_learned_queries <= 2);
    return rc;
}

/* Review focus 1: the code in force is right but the bursts are too weak to decode. A wrong code that one burst
   passes waits, a weak burst at another slot does not confirm it, and the right code stays until a clean burst
   proves it. */
static int
test_weak_right_code_is_not_replaced(void) {
    static const int weak_symbols[12] = {10, 13, 16, 19, 22, 25, 28, 31, 37, 43, 49, 52};
    fresh_decoder(0);
    hold_code(TEST_CODE);
    one_burst_window(OTHER_CODE, 10, 0, DUID_SACCH_S, 1);
    feed(GAP_INDEPENDENT);
    int rc = expect("a burst under another code leaves a candidate", pending_code() == OTHER_CODE);
    rc |= expect("but the code in force stays", code_in_force() == TEST_CODE);

    for (int w = 0; w < 3; w++) {
        one_burst_window(TEST_CODE, 6, w % 4, DUID_FACCH_S, w);
        for (int i = 0; i < 12; i++) {
            corrupt(w % 4, DUID_FACCH_S, weak_symbols[i], WEAK_LLR + i);
        }
        feed(GAP_INDEPENDENT);
    }
    rc |= expect("weak bursts replace nothing", code_in_force() == TEST_CODE && g_state->p2_cc_verified == 0U);
    rc |= expect("and are not decoded as the other code", g_code_notes == 0);

    one_burst_window(TEST_CODE, 3, 2, DUID_FACCH_S, 9);
    feed(GAP_INDEPENDENT);
    rc |= expect("a clean burst proves the code in force", code_in_force() == TEST_CODE && g_state->p2_cc_verified);
    rc |= expect("nothing was recovered", g_code_notes == 0);
    return rc;
}

/* The window that gives a candidate changes nothing else: no counter, no service options, no received identity, no
   event, no evidence, no call. */
static int
test_no_side_effects_before_acceptance(void) {
    fresh_decoder(0);
    dsd_signal_search_evidence_set(evidence_note);
    dsd_signal_search_call_set(call_note);
    g_state->dmr_so = 0x40;
    g_state->dmr_soR = 0x41;
    dsd_state* before = (dsd_state*)calloc(1U, sizeof(*before));
    if (before == NULL) {
        return expect("snapshot allocated", 0);
    }
    DSD_MEMCPY(before, g_state, sizeof(*before));
    p25p2_bb_slot slots[4];
    data_slots(slots);
    idle_slot(&slots[0], DUID_SACCH_S, 1);
    idle_slot(&slots[2], DUID_FACCH_S, 2);
    build(TEST_CODE, 10, slots);
    feed(GAP_INDEPENDENT);
    int rc = expect("the window gave a candidate", pending_code() == TEST_CODE);
    rc |= expect("no code", code_in_force() == 0U && g_state->p2_cc_verified == 0U);
    rc |= expect("no RS counter", g_state->p25_p2_rs_facch_ok == before->p25_p2_rs_facch_ok
                                      && g_state->p25_p2_rs_facch_err == before->p25_p2_rs_facch_err
                                      && g_state->p25_p2_rs_sacch_ok == before->p25_p2_rs_sacch_ok
                                      && g_state->p25_p2_rs_sacch_err == before->p25_p2_rs_sacch_err
                                      && g_state->p25_p2_soft_erasure_ok == before->p25_p2_soft_erasure_ok);
    rc |= expect("no service options", g_state->dmr_so == 0x40 && g_state->dmr_soR == 0x41);
    rc |= expect("no received identity", memcmp(&g_state->rx_id, &before->rx_id, sizeof(before->rx_id)) == 0);
    rc |= expect("no TDMA carrier", g_state->tdma_carrier == before->tdma_carrier);
    rc |= expect("no MAC", g_vpdu_calls[P25_MAC_PDU_IDLE] == 0);
    rc |= expect("no evidence, call or code note", g_evidence_notes == 0 && g_call_notes == 0 && g_code_notes == 0);
    free(before);
    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);
    return rc;
}

/* A hand-set code wins, and trunking learns its codes from the control channel: no recovery under either. */
static int
test_hardset_and_trunking_skip(void) {
    int rc = 0;
    for (int mode = 0; mode < 2; mode++) {
        fresh_decoder(0);
        if (mode == 0) {
            hold_code(OTHER_CODE);
            g_state->p2_hardset = 1;
        } else {
            g_opts->trunk_enable = 1;
        }
        one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
        feed(GAP_INDEPENDENT);
        one_burst_window(TEST_CODE, 6, 1, DUID_FACCH_S, 2);
        feed(GAP_INDEPENDENT);
        rc |= expect(mode ? "trunking: nothing waits" : "hand-set: nothing waits", pending_code() == 0U);
        rc |= expect(mode ? "trunking: nothing recovered" : "hand-set: nothing recovered",
                     code_in_force() == (mode ? 0U : OTHER_CODE) && g_code_notes == 0);
    }
    return rc;
}

/* A candidate belongs to its carrier and its alignment: a carrier boundary, the frame's forget and a window whose
   I-ISCH contradicts continuity each drop it, and the window after one confirms nothing. */
static int
test_boundary_drops_pending(void) {
    fresh_decoder(0);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
    feed(GAP_INDEPENDENT);
    int rc = expect("a candidate waits", pending_code() == TEST_CODE);
    g_state->carrier_seq++;
    one_burst_window(TEST_CODE, 6, 1, DUID_FACCH_S, 2);
    feed(GAP_INDEPENDENT);
    rc |= expect("after a carrier boundary it confirms nothing", code_in_force() == 0U);
    rc |= expect("the new carrier's own burst waits instead", pending_code() == TEST_CODE);

    p25p2_frame_forget_carrier(g_state);
    rc |= expect("forgetting the carrier drops it", pending_code() == 0U);

    fresh_decoder(0);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
    feed(GAP_INDEPENDENT);
    /* Read right after it, the next window starts at slot 2; its I-ISCH says 6. */
    one_burst_window(TEST_CODE, 6, 1, DUID_FACCH_S, 2);
    feed(GAP_CONTIGUOUS);
    rc |= expect("a contradicted position drops the candidate", pending_code() == 0U);
    rc |= expect("and confirms nothing", code_in_force() == 0U);
    return rc;
}

/* Whether @p slot's MAC as given (no CRC filled) fails its checksum. */
static int
crc_fails(const p25p2_bb_slot* slot) {
    const p25p2_acch_kind kind = slot->duid == DUID_FACCH_S ? P25P2_ACCH_FACCH : P25P2_ACCH_SACCH;
    const int n_bits = kind == P25P2_ACCH_FACCH ? 156 : 180;
    int payload[180];
    for (int k = 0; k < n_bits; k++) {
        payload[k] = (slot->mac[k / 8] >> (7 - (k % 8))) & 1;
    }
    return !p25p2_acch_checksum_ok(kind, payload);
}

/* No I-ISCH in the built window: it is placed by continuity alone. */
static void
drop_isch(void) {
    for (int ts = 0; ts < 4; ts++) {
        for (int i = 320; i < 360; i++) {
            g_bits[(ts * 360) + i] = 0U;
        }
    }
}

/* Continuity places a window as surely as the window before it was placed, which is no second observation: a window
   without its own I-ISCH neither solves nor confirms. */
static int
test_continuity_alone_neither_solves_nor_confirms(void) {
    fresh_decoder(0);
    p25p2_bb_slot slots[4];
    data_slots(slots);
    build(TEST_CODE, 6, slots);
    feed(GAP_INDEPENDENT);
    one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 1);
    drop_isch();
    feed(GAP_CONTIGUOUS);
    int rc = expect("a window continuity alone places gives no code", pending_code() == 0U);

    fresh_decoder(0);
    one_burst_window(TEST_CODE, 6, 0, DUID_FACCH_S, 2);
    feed(GAP_INDEPENDENT);
    one_burst_window(TEST_CODE, 10, 1, DUID_SACCH_S, 3);
    drop_isch();
    feed(GAP_CONTIGUOUS);
    rc |= expect("nor confirms one", code_in_force() == 0U && pending_code() == TEST_CODE);
    return rc;
}

/* A burst whose RS passes and whose CRC fails gives no code and confirms none, under -F as without it. */
static int
test_relaxed_crc_never_recovers(void) {
    int rc = 0;
    for (int relaxed = 0; relaxed < 2; relaxed++) {
        fresh_decoder(relaxed);
        p25p2_bb_slot slots[4];
        data_slots(slots);
        idle_slot(&slots[0], DUID_SACCH_S, 1);
        slots[0].fill_crc = 0;
        rc |= expect("the SACCH vector's checksum fails", crc_fails(&slots[0]));
        build(TEST_CODE, 10, slots);
        feed(GAP_INDEPENDENT);
        rc |= expect(relaxed ? "-F: a CRC-failed burst gives no code" : "a CRC-failed burst gives no code",
                     pending_code() == 0U);

        one_burst_window(TEST_CODE, 10, 0, DUID_SACCH_S, 2);
        feed(GAP_INDEPENDENT);
        data_slots(slots);
        idle_slot(&slots[1], DUID_FACCH_S, 3);
        slots[1].fill_crc = 0;
        rc |= expect("the FACCH vector's checksum fails", crc_fails(&slots[1]));
        build(TEST_CODE, 6, slots);
        feed(GAP_INDEPENDENT);
        rc |= expect(relaxed ? "-F: a CRC-failed burst confirms nothing" : "a CRC-failed burst confirms nothing",
                     code_in_force() == 0U && pending_code() == TEST_CODE);
    }
    return rc;
}

/* A code with a zero or all-ones part is no site's: never a candidate, solved or learned. */
static int
test_invalid_fields_rejected(void) {
    static const uint64_t invalid[6] = {
        CODE_OF(0xFFFFFU, 0x164U, 0x161U), CODE_OF(0x00001U, 0x000U, 0x161U), CODE_OF(0xBEE00U, 0xFFFU, 0x161U),
        CODE_OF(0xBEE00U, 0x164U, 0x000U), CODE_OF(0xBEE00U, 0x164U, 0xFFFU), CODE_OF(0x00000U, 0x164U, 0x161U),
    };
    int rc = 0;
    int candidates = 0;
    for (int i = 0; i < 6; i++) {
        fresh_decoder(0);
        g_learned[0] = invalid[i];
        g_n_learned = 1;
        one_burst_window(invalid[i], 10, 0, DUID_SACCH_S, i);
        feed(GAP_INDEPENDENT);
        one_burst_window(invalid[i], 6, 1, DUID_FACCH_S, i);
        feed(GAP_INDEPENDENT);
        candidates += pending_code() != 0U || code_in_force() != 0U || g_code_notes != 0;
    }
    rc |= expect("no invalid code is a candidate", candidates == 0);
    return rc;
}

/* The hooks' defaults: no learned codes and no listener, and an answer never longer than asked. */
static int
test_hook_defaults(void) {
    uint64_t out[4] = {0};
    dsd_signal_search_p25p2_codes_set(NULL);
    dsd_signal_search_p25p2_code_set(NULL);
    int rc = expect("no provider: no codes", dsd_signal_search_p25p2_codes(NULL, out, 4) == 0U);
    dsd_signal_search_note_p25p2_code(NULL, TEST_CODE, DSD_P25P2_CODE_RECOVERED);
    dsd_signal_search_p25p2_codes_set(learned_codes);
    dsd_signal_search_p25p2_code_set(code_note);
    g_n_learned = 6;
    rc |= expect("a provider is held to the capacity", dsd_signal_search_p25p2_codes(NULL, out, 4) == 4U);
    rc |= expect("no buffer: no codes", dsd_signal_search_p25p2_codes(NULL, NULL, 4) == 0U);
    g_n_learned = 0;
    dsd_signal_search_p25p2_codes_set(NULL);
    dsd_signal_search_p25p2_code_set(NULL);
    return rc;
}

int
main(void) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    if (g_opts == NULL || g_state == NULL) {
        return 1;
    }
    int rc = test_hook_defaults();
    dsd_signal_search_p25p2_codes_set(learned_codes);
    dsd_signal_search_p25p2_code_set(code_note);
    rc |= test_tables_match_direct_encoding();
    rc |= test_solve_every_slot_facch_sacch();
    rc |= test_solve_with_symbol_errors();
    rc |= test_high_llr_prefix_error();
    rc |= test_noise_never_accepted();
    rc |= test_shifted_seed_rejected();
    rc |= test_same_window_does_not_confirm();
    rc |= test_same_t_does_not_confirm();
    rc |= test_pending_confirmed_by_unsolvable_burst();
    rc |= test_learned_code_accepted();
    rc |= test_weak_right_code_is_not_replaced();
    rc |= test_no_side_effects_before_acceptance();
    rc |= test_hardset_and_trunking_skip();
    rc |= test_boundary_drops_pending();
    rc |= test_continuity_alone_neither_solves_nor_confirms();
    rc |= test_relaxed_crc_never_recovers();
    rc |= test_invalid_fields_rejected();
    dsd_signal_search_p25p2_codes_set(NULL);
    dsd_signal_search_p25p2_code_set(NULL);
    dsd_state_ext_free_all(g_state);
    free(g_state);
    free(g_opts);
    if (rc == 0) {
        printf("P25_P2_SEED_RECOVER: OK\n");
    }
    return rc;
}
