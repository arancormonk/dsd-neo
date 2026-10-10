// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p2_frame.c
 * Phase 2 TDMA Frame Processing
 *
 * original copyrights for portions used below (OP25 DUID table, MAC len table)
 *
 * LWVMOBILE
 * 2022-09 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/ez.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_lfsr.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/protocol/p25/p25p2_soft.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "p25p2_frame_internal.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];
extern int16_t p2xllr[1400];
extern int ess_a[2][168];
extern int16_t ess_a_llr[2][168];

/* The voice burst being decoded, for the playout's per-burst verdict, and the policy verdict the decode gate already
   evaluated for it, if any (issue #651): the playout then needs no second policy evaluation. */
static uint32_t s_burst_serial = 0U;

static struct {
    uint32_t serial;
    int slot;
    int valid;
    dsd_p25p2_burst_decision decision;
} s_burst_verdict;

static void
p25p2_note_burst_verdict(int slot, const dsd_p25p2_burst_decision* decision) {
    s_burst_verdict.valid = decision != NULL;
    s_burst_verdict.serial = s_burst_serial;
    s_burst_verdict.slot = slot;
    if (decision) {
        s_burst_verdict.decision = *decision;
    }
}

static const dsd_p25p2_burst_decision*
p25p2_burst_verdict_for(int slot) {
    if (s_burst_verdict.valid && s_burst_verdict.serial == s_burst_serial && s_burst_verdict.slot == slot) {
        return &s_burst_verdict.decision;
    }
    return NULL;
}

// Clear per-slot audio gates, small audio rings, encryption indicators, and
// UI call banners for both logical slots. Intended for use on call teardown
// before returning to the control channel.
void
p25p2_teardown_call(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    // Play what both slots still hold before the gates and crypto go: each queued frame carries the verdict it was
    // decoded with, so the tail of a short call plays in either output format (issue #651).
    if (opts) {
        dsd_p25p2_playout_drain(opts, state);
    }

    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_p2_media_rejected[0] = 0;
    p25_sm_clear_rejected_slot(state, 0);
    state->p25_p2_media_rejected[1] = 0;
    p25_sm_clear_rejected_slot(state, 1);
    p25_crypto_reset_slot(state, 0);
    p25_crypto_reset_slot(state, 1);
    p25_p2_audio_ring_reset(state, -1);
    dsd_p25p2_playout_reset(state, -1);
    state->p25_p2_last_mac_active[0] = 0;
    state->p25_p2_last_mac_active[1] = 0;
    state->p25_p2_last_end_ptt[0] = 0;
    state->p25_p2_last_end_ptt[1] = 0;
    state->dmr_so = 0;
    state->dmr_soR = 0;
    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_miP = 0ULL;
    state->payload_algidR = 0;
    state->payload_keyidR = 0;
    state->payload_miN = 0ULL;
}

//DUID Look Up Table from OP25
static const int16_t duid_lookup[256] = {
    //128 triggers false 4V on bad signal
    0,  0,  0,  -1, 0,  -1, -1, 1,  0,  -1, -1, 4,  -1, 8,  2,  -1, 0,  -1, -1, 1,  -1, 1,  1,  1,  -1, 3,  9,  -1, 5,
    -1, -1, 1,  0,  -1, -1, 10, -1, 6,  2,  -1, -1, 3,  2,  -1, 2,  -1, 2,  2,  -1, 3,  7,  -1, 11, -1, -1, 1,  3,  3,
    -1, 3,  -1, 3,  2,  -1, 0,  -1, -1, 4,  -1, 6,  12, -1, -1, 4,  4,  4,  5,  -1, -1, 4,  -1, 13, 7,  -1, 5,  -1, -1,
    1,  5,  -1, -1, 4,  5,  5,  5,  -1, -1, 6,  7,  -1, 6,  6,  -1, 6,  14, -1, -1, 4,  -1, 6,  2,  -1, 7,  -1, 7,  7,
    -1, 6,  7,  -1, -1, 3,  7,  -1, 5,  -1, -1, 15, -1, -1, -1, 10, -1, 8,  12, -1, -1, 8,  9,  -1, 8,  8,  -1, 8,  -1,
    13, 9,  -1, 11, -1, -1, 1,  9,  -1, 9,  9,  -1, 8,  9,  -1, -1, 10, 10, 10, 11, -1, -1, 10, 14, -1, -1, 10, -1, 8,
    2,  -1, 11, -1, -1, 10, 11, 11, 11, -1, -1, 3,  9,  -1, 11, -1, -1, 15, -1, 13, 12, -1, 12, -1, 12, 12, 14, -1, -1,
    4,  -1, 8,  12, -1, 13, 13, -1, 13, -1, 13, 12, -1, -1, 13, 9,  -1, 5,  -1, -1, 15, 14, -1, -1, 10, -1, 6,  12, -1,
    14, 14, 14, -1, 14, -1, -1, 15, -1, 13, 7,  -1, 11, -1, -1, 15, 14, -1, -1, 15, -1, 15, 15, 15,
};

static const uint8_t duid_canonical[16] = {
    0x00U, 0x17U, 0x2EU, 0x39U, 0x4BU, 0x5CU, 0x65U, 0x72U, 0x8DU, 0x9AU, 0xA3U, 0xB4U, 0xC6U, 0xD1U, 0xE8U, 0xFFU,
};

static uint8_t
p25p2_abs_llr_reliability(int16_t llr) {
    int v = llr < 0 ? -(int)llr : (int)llr;
    if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

static uint8_t
p25p2_reliability_for_abs_bit(int abs_bit) {
    if (abs_bit < 0) {
        return 0;
    }
    if (abs_bit >= 1400) {
        return 0;
    }
    return p25p2_abs_llr_reliability(p2llr[abs_bit]);
}

static int
p25p2_duid_is_exact(uint8_t received, int decoded) {
    return decoded >= 0 && decoded < 16 && received == duid_canonical[decoded];
}

static int
p25p2_duid_080_soft_allowed(const uint8_t reliab8[8], int threshold) {
    if (reliab8 == NULL || (int)reliab8[0] >= threshold) {
        return 0;
    }
    for (int i = 1; i < 8; i++) {
        if ((int)reliab8[i] < threshold) {
            return 0;
        }
    }
    return 1;
}

static int
p25p2_duid_hamming8(uint8_t a, uint8_t b) {
    uint8_t diff = (uint8_t)(a ^ b);
    int count = 0;
    for (int i = 0; i < 8; i++) {
        count += (diff >> i) & 1U;
    }
    return count;
}

static int
p25p2_duid_flip_cost(uint8_t received, uint8_t candidate, const uint8_t reliab8[8], int threshold) {
    int cost = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = (uint8_t)(1U << (7 - i));
        if ((received & mask) != (candidate & mask)) {
            if ((int)reliab8[i] >= threshold) {
                return 999999;
            }
            cost += (int)reliab8[i];
        }
    }
    return cost;
}

int
p25p2_duid_lookup_soft(uint8_t received, const uint8_t* reliab8) {
    int hard = duid_lookup[received];
    if (reliab8 == NULL || p25p2_duid_is_exact(received, hard)) {
        return hard;
    }

    int thresh = p25p2_soft_erasure_threshold();
    if (received == 0x80U && !p25p2_duid_080_soft_allowed(reliab8, thresh)) {
        return hard;
    }

    int best_decoded = hard;
    int best_cost = 999999;
    int tied_best = 0;
    for (int decoded = 0; decoded < 16; decoded++) {
        uint8_t candidate = duid_canonical[decoded];
        int distance = p25p2_duid_hamming8(received, candidate);
        if (distance < 1 || distance > 2) {
            continue;
        }
        if (received == 0x80U && decoded != 0) {
            continue;
        }
        int cost = p25p2_duid_flip_cost(received, candidate, reliab8, thresh);
        if (cost >= 999999) {
            continue;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_decoded = decoded;
            tied_best = 0;
        } else if (cost == best_cost && decoded != best_decoded) {
            tied_best = 1;
        }
    }
    if (tied_best) {
        return hard;
    }
    return best_decoded;
}

//4V and 2V deinterleave schedule
static const int c0[25] = {23, 5, 22, 4, 21, 3, 20, 2, 19, 1, 18, 0, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6};

static const int c1[24] = {10, 9, 8, 7, 6, 5, 22, 4, 21, 3, 20, 2, 19, 1, 18, 0, 17, 16, 15, 14, 13, 12, 11};

static const int c2[12] = {3, 2, 1, 0, 10, 9, 8, 7, 6, 5, 4};

static const int c3[15] = {13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

static const int csubset[73] = {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 3, 0, 0, 1, 3,
                                0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 1, 3, 0, 1, 2, 3,
                                0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};

static const int* w;

static char ambe_fr1[4][24] = {0};
static char ambe_fr2[4][24] = {0};
static char ambe_fr3[4][24] = {0};
static char ambe_fr4[4][24] = {0};

/* The timeslot being dispatched within the four a processP2() call collects (0-3). It is not a superframe position:
   that is (p2_scramble_offset + ts_counter) % 12. */
static int ts_counter = 0;
int p2bit[4320] = {0};             //4320
static uint8_t p2lbit[8640] = {0}; //bits generated by LFSR scrambler, doubling up for offset roll-over
static int p2xbit[4320] = {0};     //bits xored from p2bit and p2lbit

/* The seed p25p2_process_frame_scramble() descrambled the buffered superframe with (issue #575). Its bursts are then
   decoded one after another, and a network status broadcast in an earlier one may replace the seed in force (under -F
   even one that failed its CRC) before a later one decodes. */
static struct {
    uint64_t wacn;
    uint64_t sysid;
    uint64_t nac;
    int valid;
} s_descramble_seed;

/* The carrier count (state->carrier_seq) the last superframe was collected on, once one was (issue #575). */
static struct {
    uint32_t seq;
    int valid;
} s_superframe_carrier;

/* Per-bit soft metrics for captured 700 dibits (1400 bits). */
int16_t p2llr[1400] = {0};  /* bit LLRs before descramble */
int16_t p2xllr[1400] = {0}; /* bit LLRs after descramble */

static int dibit = 0;
static int vc_counter = 0;
static int framing_counter = 0;

static uint64_t isch = 0;
static int isch_decoded = -1;
static uint8_t p2_duid[8] = {0};
static int16_t duid_decoded = -1;

static int ess_b[2][96] = {0}; //96 bits for 4 - 24 bit ESS_B fields starting bit 168 (RS 44,16,29)
int ess_a[2][168] = {0};       //ESS_A 1 (96 bit) and 2 (72 bit) fields, starting at bit 168 and bit 266 (RS Parity)
int16_t ess_a_llr[2][168] = {0};

static int facch[2][156] = {0};
static int facch_rs[2][114] = {0};

static int sacch[2][180] = {0};
static int sacch_rs[2][132] = {0};

static dsd_vocoder_soft_bit
p25p2_soft_bit_from_abs_bit(int abs_bit) {
    if (abs_bit < 0 || abs_bit >= 1400) {
        return dsd_vocoder_soft_bit_from_hard_llr(0, 0);
    }
    return dsd_vocoder_soft_bit_from_hard_llr(p2xbit[abs_bit], p2xllr[abs_bit]);
}

/* The token taken when p25p2_process_duid() began dispatching the collected superframe. */
static p25p2_retune_token s_window_token;

/* Window continuity (issue #651). A window is the four timeslots processP2() collects after a 20-dibit sync; the next
   one starts 4 timeslots later unless a missed sync skipped some, which the symbol count measures (180 dibits a
   timeslot). */
enum {
    P25P2_SYNC_DIBITS = 20,
    P25P2_TIMESLOT_DIBITS = 180,
    P25P2_WINDOW_DIBITS = 700,
};

static struct {
    int valid;
    uint32_t carrier_seq;
    p25p2_retune_token token;
    int start;           /* superframe timeslot of the window's timeslot 0 */
    uint32_t end_symbol; /* dsd_state::symbolcnt after its last dibit */
} s_prev_window;

/* A channel-1 I-ISCH in the window being collected set p2_scramble_offset. */
static int s_isch_located = 0;

static int
p25p2_timeslot_mod12(int timeslot) {
    return ((timeslot % 12) + 12) % 12;
}

// Reset all P25P2 frame processing global state variables.
// This must be called when tuning to a new P25P2 voice channel to clear stale
// data from the previous channel that would otherwise cause decode failures.
// The issue manifests as: first P25P2 tune works, but subsequent voice channel
// grants fail to lock with tanking EVM/SNR until retune to P25P1 control channel.
//
// A tune accepted while a superframe is dispatched runs this from inside the dispatch loop, whose loop variable is
// ts_counter: the token change it makes is what stops that loop (issue #651).
void
p25_p2_frame_reset(void) {
    p25p2_retune_note_frame_reset();
    s_prev_window.valid = 0;
    s_isch_located = 0;
    // Reset counters
    ts_counter = 0;
    vc_counter = 0;
    framing_counter = 0;
    dibit = 0;

    // Reset bit buffers (stale data from previous channel causes decode failures)
    DSD_MEMSET(p2bit, 0, sizeof(p2bit));
    DSD_MEMSET(p2lbit, 0, sizeof(p2lbit));
    DSD_MEMSET(p2xbit, 0, sizeof(p2xbit));

    // Reset soft-decision buffers
    DSD_MEMSET(p2llr, 0, sizeof(p2llr));
    DSD_MEMSET(p2xllr, 0, sizeof(p2xllr));

    // Reset decoded state
    isch = 0;
    isch_decoded = -1;
    DSD_MEMSET(p2_duid, 0, sizeof(p2_duid));
    duid_decoded = -1;

    // Nothing in the buffers was descrambled with any seed now, or collected on any carrier (issue #575).
    DSD_MEMSET(&s_descramble_seed, 0, sizeof(s_descramble_seed));
    DSD_MEMSET(&s_superframe_carrier, 0, sizeof(s_superframe_carrier));

    // Reset ESS buffers (stale ESS_A/ESS_B from previous channel corrupts new channel)
    DSD_MEMSET(ess_a, 0, sizeof(ess_a));
    DSD_MEMSET(ess_b, 0, sizeof(ess_b));
    DSD_MEMSET(ess_a_llr, 0, sizeof(ess_a_llr));

    // Reset FACCH/SACCH buffers
    DSD_MEMSET(facch, 0, sizeof(facch));
    DSD_MEMSET(facch_rs, 0, sizeof(facch_rs));
    DSD_MEMSET(sacch, 0, sizeof(sacch));
    DSD_MEMSET(sacch_rs, 0, sizeof(sacch_rs));

    // Reset AMBE frame buffers
    DSD_MEMSET(ambe_fr1, 0, sizeof(ambe_fr1));
    DSD_MEMSET(ambe_fr2, 0, sizeof(ambe_fr2));
    DSD_MEMSET(ambe_fr3, 0, sizeof(ambe_fr3));
    DSD_MEMSET(ambe_fr4, 0, sizeof(ambe_fr4));
}

//store an entire p2 superframe worth of dibits into a bit buffer
static void
p2_dibit_buffer(dsd_opts* opts, dsd_state* state) {
    for (int i = 0; i < 700; i++) //4 Timeslots minus sync
    {
        dsd_dibit_soft_t soft;

        /* Capture hard dibits and per-bit soft metrics in parallel. */
        dibit = getDibitSoft(opts, state, &soft);

        //dibit inversion is handled internally when the sync type is inverted
        p2bit[((size_t)i * 2)] = (dibit >> 1) & 1;
        p2bit[((size_t)i * 2) + 1] = (dibit & 1);

        /* Store signed reliability for each hard-decision bit. */
        p2llr[(i * 2) + 0] = soft.llr[0];
        p2llr[(i * 2) + 1] = soft.llr[1];
    }
}

/* A burst descrambled with the seed (WACN, SYSID, p2_cc) passed its Reed-Solomon check, which bits descrambled with a
   wrong seed fail: p2_cc is the NAC this carrier runs on (issue #575). It proves the seed the buffer was descrambled
   with, so the seed in force is proven only while it is still that one.

   A call's row reads the live NAC only while the call is active, and the burst that proves the seed may carry the
   MAC_END_PTT that ends it, dispatched right after: the slots are rendered when the proof is first set, before the
   burst's MAC PDU, so the row of the call that burst ends keeps the NAC it proved. */
static void
p25p2_note_seed_proven(dsd_opts* opts, dsd_state* state) {
    if (s_descramble_seed.valid && s_descramble_seed.wacn == state->p2_wacn
        && s_descramble_seed.sysid == state->p2_sysid && s_descramble_seed.nac == state->p2_cc) {
        const int newly_proven = state->p2_cc_verified == 0U;
        state->p2_cc_verified = 1U;
        if (newly_proven) {
            dsd_event_sync_slot(opts, state, 0U);
            dsd_event_sync_slot(opts, state, 1U);
        }
    }
}

void
p25p2_process_frame_scramble(dsd_opts* opts, const dsd_state* state) {
    UNUSED(opts);

    //The bits of the scramble sequence corresponding to signal bits that are not scrambled or not used are discarded.
    //descramble frame scrambled by LFSR of WACN, SysID, and CC(NAC)
    p25p2_generate_scramble_bits(state->p2_wacn, state->p2_sysid, state->p2_cc, p2lbit, 4320U);
    DSD_MEMCPY(p2lbit + 4320, p2lbit, 4320U * sizeof(p2lbit[0]));
    s_descramble_seed.wacn = state->p2_wacn;
    s_descramble_seed.sysid = state->p2_sysid;
    s_descramble_seed.nac = state->p2_cc;
    s_descramble_seed.valid = 1;

    for (int i = 0; i < 4300; i++) {
        //offset by 20 for sync, then 360 for each ts frame off from start of superframe
        p2xbit[i] = p2bit[i] ^ p2lbit[i + 20 + (360 * state->p2_scramble_offset)];
    }

    /* Descrambling preserves confidence magnitude but flips LLR sign when the
       scramble bit inverts the hard bit. Only the captured 1400 bits have
       valid soft metrics. */
    DSD_MEMSET(p2xllr, 0, sizeof(p2xllr));
    for (int i = 0; i < 1400; i++) {
        p2xllr[i] = p2lbit[i + 20 + (360 * state->p2_scramble_offset)] ? (int16_t)-p2llr[i] : p2llr[i];
    }
}

enum {
    P25P2_RS_ERASURE_CAPACITY = 28,
    P25P2_FACCH_FIXED_ERASURES_COUNT = 18,
    P25P2_SACCH_FIXED_ERASURES_COUNT = 11,
};

static const int P25P2_FACCH_FIXED_ERASURES[P25P2_FACCH_FIXED_ERASURES_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 54, 55, 56, 57, 58, 59, 60, 61, 62,
};
static const int P25P2_SACCH_FIXED_ERASURES[P25P2_SACCH_FIXED_ERASURES_COUNT] = {
    0, 1, 2, 3, 4, 57, 58, 59, 60, 61, 62,
};

static int
p25p2_decode_facch_ranked(int payload[156], int parity[114], int scrambled, int* used_dynamic_erasure) {
    int original_payload[156];
    int original_parity[114];
    DSD_MEMCPY(original_payload, payload, sizeof(original_payload));
    DSD_MEMCPY(original_parity, parity, sizeof(original_parity));

    int ec = ez_rs28_facch(payload, parity, P25P2_FACCH_FIXED_ERASURES, P25P2_FACCH_FIXED_ERASURES_COUNT);
    if (ec >= 0) {
        *used_dynamic_erasure = 0;
        return ec;
    }

    int erasures[P25P2_RS_ERASURE_CAPACITY] = {0};
    DSD_MEMCPY(erasures, P25P2_FACCH_FIXED_ERASURES, sizeof(P25P2_FACCH_FIXED_ERASURES));
    int n_erasures = p25p2_facch_soft_erasures(ts_counter, scrambled, erasures, P25P2_FACCH_FIXED_ERASURES_COUNT, 10);
    for (int n = P25P2_FACCH_FIXED_ERASURES_COUNT + 1; n <= n_erasures; n++) {
        DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
        DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
        ec = ez_rs28_facch(payload, parity, erasures, n);
        if (ec >= 0) {
            *used_dynamic_erasure = 1;
            return ec;
        }
    }

    DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
    DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
    *used_dynamic_erasure = 0;
    return ec;
}

static int
p25p2_decode_sacch_ranked(int payload[180], int parity[132], int scrambled, int* used_dynamic_erasure) {
    int original_payload[180];
    int original_parity[132];
    DSD_MEMCPY(original_payload, payload, sizeof(original_payload));
    DSD_MEMCPY(original_parity, parity, sizeof(original_parity));

    int ec = ez_rs28_sacch(payload, parity, P25P2_SACCH_FIXED_ERASURES, P25P2_SACCH_FIXED_ERASURES_COUNT);
    if (ec >= 0) {
        *used_dynamic_erasure = 0;
        return ec;
    }

    int erasures[P25P2_RS_ERASURE_CAPACITY] = {0};
    DSD_MEMCPY(erasures, P25P2_SACCH_FIXED_ERASURES, sizeof(P25P2_SACCH_FIXED_ERASURES));
    int n_erasures = p25p2_sacch_soft_erasures(ts_counter, scrambled, erasures, P25P2_SACCH_FIXED_ERASURES_COUNT, 16);
    for (int n = P25P2_SACCH_FIXED_ERASURES_COUNT + 1; n <= n_erasures; n++) {
        DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
        DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
        ec = ez_rs28_sacch(payload, parity, erasures, n);
        if (ec >= 0) {
            *used_dynamic_erasure = 1;
            return ec;
        }
    }

    DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
    DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
    *used_dynamic_erasure = 0;
    return ec;
}

void
p25p2_process_facchc(dsd_opts* opts, dsd_state* state, int timeslot_index) {
    ts_counter = timeslot_index;
    //gather and process FACCH w/o scrambling (S-OEMI) so we know what to do with the containing data.
    for (int i = 0; i < 72; i++) {
        facch[state->currentslot][i] = p2bit[i + 2 + (ts_counter * 360)];
    }
    //skip DUID 1
    for (int i = 0; i < 62; i++) {
        facch[state->currentslot][i + 72] = p2bit[i + 76 + (ts_counter * 360)];
    }
    //skip sync
    for (int i = 0; i < 22; i++) {
        facch[state->currentslot][i + 134] = p2bit[i + 180 + (ts_counter * 360)];
    }
    //gather FACCH RS parity bits
    for (int i = 0; i < 42; i++) {
        facch_rs[state->currentslot][i] = p2bit[i + 202 + (ts_counter * 360)];
    }
    //skip DUID 3
    for (int i = 0; i < 72; i++) {
        facch_rs[state->currentslot][i + 42] = p2bit[i + 246 + (ts_counter * 360)];
    }

    //send payload and parity for FACCH error correction (RS(63,35), t=14)
    int ec = -2;

    int used_dynamic_erasure = 0;
    ec = p25p2_decode_facch_ranked(facch[state->currentslot], facch_rs[state->currentslot], 0, &used_dynamic_erasure);
    if (used_dynamic_erasure) {
        state->p25_p2_soft_erasure_ok++;
    }

    int opcode = 0;
    opcode =
        (facch[state->currentslot][0] << 2) | (facch[state->currentslot][1] << 1) | (facch[state->currentslot][2] << 0);

    if (state->currentslot == 0) {
        state->dmr_so = opcode;
    } else {
        state->dmr_soR = opcode;
    }

    if (ec >= 0) {
        state->p25_p2_rs_facch_ok++;
        state->p25_p2_rs_facch_corr += (unsigned int)ec;
        /* Feedback: RS OK */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 1, 0, 0, 0, 0);
#endif
        process_FACCH_MAC_PDU(opts, state, facch[state->currentslot]);
    } else {
        state->p25_p2_rs_facch_err++;
        DSD_FPRINTF(stderr, " R-S ERR Fc");
        /* Feedback: RS ERR */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 1, 0, 0, 0);
#endif
    }
}

static void
process_FACCHs(dsd_opts* opts, dsd_state* state) {
    //gather and process FACCH w scrambling (S-OEMI) so we know what to do with the containing data.
    for (int i = 0; i < 72; i++) {
        facch[state->currentslot][i] = p2xbit[i + 2 + (ts_counter * 360)];
    }
    //skip DUID 1
    for (int i = 0; i < 62; i++) {
        facch[state->currentslot][i + 72] = p2xbit[i + 76 + (ts_counter * 360)];
    }
    //skip sync
    for (int i = 0; i < 22; i++) {
        facch[state->currentslot][i + 134] = p2xbit[i + 180 + (ts_counter * 360)];
    }
    //gather FACCh RS parity bits
    for (int i = 0; i < 42; i++) {
        facch_rs[state->currentslot][i] = p2xbit[i + 202 + (ts_counter * 360)];
    }
    //skip DUID 3
    for (int i = 0; i < 72; i++) {
        facch_rs[state->currentslot][i + 42] = p2xbit[i + 246 + (ts_counter * 360)];
    }

    //send payload and parity for FACCH error correction (RS(63,35), t=14)
    int ec = -2;

    int used_dynamic_erasure = 0;
    ec = p25p2_decode_facch_ranked(facch[state->currentslot], facch_rs[state->currentslot], 1, &used_dynamic_erasure);
    if (used_dynamic_erasure) {
        state->p25_p2_soft_erasure_ok++;
    }

    int opcode = 0;
    opcode =
        (facch[state->currentslot][0] << 2) | (facch[state->currentslot][1] << 1) | (facch[state->currentslot][2] << 0);

    if (state->currentslot == 0) {
        state->dmr_so = opcode;
    } else {
        state->dmr_soR = opcode;
    }

    if (ec >= 0) {
        state->p25_p2_rs_facch_ok++;
        state->p25_p2_rs_facch_corr += (unsigned int)ec;
        /* Only a decode that kept its parity check proves the seed: soft erasures can use all of it up (issue #575). */
        if (!used_dynamic_erasure) {
            p25p2_note_seed_proven(opts, state);
        }
        /* Feedback: RS OK */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 1, 0, 0, 0, 0);
#endif
        process_FACCH_MAC_PDU(opts, state, facch[state->currentslot]);
    } else {
        state->p25_p2_rs_facch_err++;
        DSD_FPRINTF(stderr, " R-S ERR Fs");
        /* Feedback: RS ERR */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 1, 0, 0, 0);
#endif
    }
}

void
p25p2_process_sacchc(dsd_opts* opts, dsd_state* state, int timeslot_index) {
    ts_counter = timeslot_index;
    //gather and process SACCH w/o scrambling (I-OEMI) so we know what to do with the containing data.
    for (int i = 0; i < 72; i++) {
        sacch[state->currentslot][i] = p2bit[i + 2 + (ts_counter * 360)];
    }
    //skip DUID 1
    for (int i = 0; i < 108; i++) {
        sacch[state->currentslot][i + 72] = p2bit[i + 76 + (ts_counter * 360)];
    }
    //start collecting parity
    for (int i = 0; i < 60; i++) {
        sacch_rs[state->currentslot][i] = p2bit[i + 184 + (ts_counter * 360)];
    }
    //skip DUID 3
    for (int i = 0; i < 72; i++) {
        sacch_rs[state->currentslot][i + 60] = p2bit[i + 246 + (ts_counter * 360)];
    }

    //send payload and parity for SACCH error correction (RS(63,35), t=14)
    int ec = -2;

    int used_dynamic_erasure = 0;
    ec = p25p2_decode_sacch_ranked(sacch[state->currentslot], sacch_rs[state->currentslot], 0, &used_dynamic_erasure);
    if (used_dynamic_erasure) {
        state->p25_p2_soft_erasure_ok++;
    }

    int opcode = 0;
    opcode =
        (sacch[state->currentslot][0] << 2) | (sacch[state->currentslot][1] << 1) | (sacch[state->currentslot][2] << 0);

    //set inverse true for SACCH
    if (state->currentslot == 0) {
        state->dmr_soR = opcode;
    } else {
        state->dmr_so = opcode;
    }

    if (ec >= 0) {
        state->p25_p2_rs_sacch_ok++;
        state->p25_p2_rs_sacch_corr += (unsigned int)ec;
        /* Feedback: RS OK */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 0, 1, 0, 0);
#endif
        process_SACCH_MAC_PDU(opts, state, sacch[state->currentslot]);
    } else {
        state->p25_p2_rs_sacch_err++;
        DSD_FPRINTF(stderr, " R-S ERR Sc");
        /* Feedback: RS ERR */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 0, 0, 1, 0);
#endif
    }
}

static void
process_SACCHs(dsd_opts* opts, dsd_state* state) {
    //gather and process SACCH w scrambling (I-OEMI) so we know what to do with the containing data.
    for (int i = 0; i < 72; i++) {
        sacch[state->currentslot][i] = p2xbit[i + 2 + (ts_counter * 360)];
    }
    //skip DUID 1
    for (int i = 0; i < 108; i++) {
        sacch[state->currentslot][i + 72] = p2xbit[i + 76 + (ts_counter * 360)];
    }
    //start collecting parity
    for (int i = 0; i < 60; i++) {
        sacch_rs[state->currentslot][i] = p2xbit[i + 184 + (ts_counter * 360)];
    }
    //skip DUID 3
    for (int i = 0; i < 72; i++) {
        sacch_rs[state->currentslot][i + 60] = p2xbit[i + 246 + (ts_counter * 360)];
    }

    //send payload and parity for SACCH error correction (RS(63,35), t=14)
    int ec = -2;

    int used_dynamic_erasure = 0;
    ec = p25p2_decode_sacch_ranked(sacch[state->currentslot], sacch_rs[state->currentslot], 1, &used_dynamic_erasure);
    if (used_dynamic_erasure) {
        state->p25_p2_soft_erasure_ok++;
    }

    int opcode = 0;
    opcode =
        (sacch[state->currentslot][0] << 2) | (sacch[state->currentslot][1] << 1) | (sacch[state->currentslot][2] << 0);

    //set inverse true for SACCH
    if (state->currentslot == 0) {
        state->dmr_soR = opcode;
    } else {
        state->dmr_so = opcode;
    }

    if (ec >= 0) {
        state->p25_p2_rs_sacch_ok++;
        state->p25_p2_rs_sacch_corr += (unsigned int)ec;
        /* Only a decode that kept its parity check proves the seed: soft erasures can use all of it up (issue #575). */
        if (!used_dynamic_erasure) {
            p25p2_note_seed_proven(opts, state);
        }
        /* Feedback: RS OK */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 0, 1, 0, 0);
#endif
        process_SACCH_MAC_PDU(opts, state, sacch[state->currentslot]);
    } else {
        state->p25_p2_rs_sacch_err++;
        DSD_FPRINTF(stderr, " R-S ERR Ss");
        /* Feedback: RS ERR */
#ifdef USE_RADIO
        dsd_rtl_stream_metrics_hook_p25p2_err_update(state->currentslot, 0, 0, 0, 1, 0);
#endif
    }
}

void
p25p2_process_isch(dsd_opts* opts, dsd_state* state, int framing_index) {
    framing_counter = framing_index;
    UNUSED(opts);

    isch = 0;
    uint8_t isch_reliab[40];
    for (int i = 0; i < 40; i++) {
        int abs_bit = i + 320 + (360 * framing_counter);
        isch = isch << 1;
        isch = isch | p2bit[abs_bit];
        isch_reliab[i] = p25p2_reliability_for_abs_bit(abs_bit);
    }

    if (isch != 0x575D57F7FF) {
        isch_decoded = isch_lookup_soft(isch, isch_reliab);

        if (isch_decoded > -1) {
            int uf_count = isch_decoded & 0x3;
            int free = (isch_decoded >> 2) & 0x1;
            int isch_loc = (isch_decoded >> 3) & 0x3;
            int chan_num = (isch_decoded >> 5) & 0x3;
            UNUSED2(uf_count, free);
            state->p2_vch_chan_num = chan_num;

            //relative position to the only chan 1 we should see
            if (chan_num == 1 && isch_loc == 0) {
                state->p2_scramble_offset = 12 - framing_counter;
                s_isch_located = 1;
            } else if (chan_num == 1 && isch_loc == 1) {
                state->p2_scramble_offset = 4 - framing_counter;
                s_isch_located = 1;
            } else if (chan_num == 1 && isch_loc == 2) {
                state->p2_scramble_offset = 8 - framing_counter;
                s_isch_located = 1;
            }

        } else {
            // If -2(no return value) or -1(fec error)
        }
    }

    isch_decoded = -1; //reset to bad value after running
}

static void DSD_ATTR_USED
p25p2_emit_voice_activity(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    int slot = state->currentslot & 1;
    if (opts && opts->trunk_tune_enc_calls == 0 && p25_crypto_companion_suppressed(state, slot)) {
        return;
    }
    p25_sm_emit_active(opts, state, slot);
    state->last_vc_sync_time = dsd_decode_time();
    state->last_vc_sync_time_m = dsd_decode_now_mono_s();
}

static int
p25p2_voice_crypto_is_authoritatively_clear(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }

    const int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;
    if (algid == 0x80) {
        return 1;
    }

    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.kind == DSD_CALL_KIND_PRIVATE_VOICE || call.ota_target_id > INT_MAX) {
        return 0;
    }
    const int talkgroup = (int)call.ota_target_id;
    return p25_patch_tg_key_is_clear(state, talkgroup) || p25_patch_sg_key_is_clear(state, talkgroup);
}

static int
p25p2_active_target(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE
        || call.ota_target_id > INT_MAX) {
        return 0;
    }
    return (int)call.ota_target_id;
}

// Diagnostic trace of decode-side audio gate transitions into the --p25-sm-log
// stream. The gate decides whether a slot's vocoder output lands in the
// playback buffers at all, so a flip between mixer passes is invisible to the
// mixer trace except as silence; this catches it at the decode chokepoints
// with the state that drove it. Logged only on change. Function-local
// statics: single instance, decoder thread only, like the decode path.
static void
p25p2_audio_gate_diag(dsd_opts* opts, const dsd_state* state, const char* at) {
    if (!dsd_p25_sm_log_enabled(opts)) {
        return;
    }
    static int prev[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    const int cur[8] = {state->p25_p2_audio_allowed[0],  state->p25_p2_audio_allowed[1],
                        (int)state->p25_crypto_state[0], (int)state->p25_crypto_state[1],
                        state->p25_p2_media_rejected[0], state->p25_p2_media_rejected[1],
                        (int)state->dmrburstL,           (int)state->dmrburstR};
    int changed = 0;
    for (int i = 0; i < 8; i++) {
        if (prev[i] != cur[i]) {
            changed = 1;
            prev[i] = cur[i];
        }
    }
    if (!changed) {
        return;
    }
    dsd_p25_sm_logf(opts, "event=audio_gate at=%s allowed=%d/%d crypto=%d/%d rejected=%d/%d burst=%d/%d", at, cur[0],
                    cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7]);
}

static void
p25p2_prepare_voice_crypto(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    const int slot = state->currentslot;
    if (slot < 0 || slot > 1) {
        return;
    }
    p25_sm_touch_rejected_slot_skip(opts, state, slot);
    if (state->p25_p2_media_rejected[slot]) {
        state->p25_p2_audio_allowed[slot] = 0;
        p25p2_audio_gate_diag(opts, state, "prepare-rejected");
        return;
    }
    const int svc = (slot == 0) ? state->dmr_so : state->dmr_soR;
    if ((svc & 0x40) != 0 && !p25p2_voice_crypto_is_authoritatively_clear(state, slot)) {
        p25_sm_emit_crypto_pending(opts, state, slot);
    }
    if (p25_crypto_audio_permitted(opts, state, slot)) {
        const int alg = (slot == 0) ? state->payload_algid : state->payload_algidR;
        dsd_p25p2_burst_decision decision;
        int decision_valid = 0;
        state->p25_p2_audio_allowed[slot] =
            dsd_p25p2_decode_audio_allowed_verdict(opts, state, slot, alg, &decision, &decision_valid);
        p25p2_note_burst_verdict(slot, decision_valid ? &decision : NULL);
    }
    p25p2_audio_gate_diag(opts, state, "prepare-voice");
}

static int
p25p2_deinterleave_index(int ww, int* q, int* r, int* s, int* t) {
    if (ww == 0) {
        return c0[(*q)++];
    }
    if (ww == 1) {
        return c1[(*r)++];
    }
    if (ww == 2) {
        return c2[(*s)++];
    }
    if (ww == 3) {
        return c3[(*t)++];
    }
    return -1;
}

static void
p25p2_unpack_voice_frames(int frame_count, dsd_vocoder_soft_bit ambe_soft[4][4][24]) {
    static const int bit_offsets[4] = {2, 76, 172, 246};
    w = csubset;
    int q = 0;
    int r = 0;
    int s = 0;
    int t = 0;
    for (int x = 0; x < 72; x++) {
        int ww = *w++;
        int b = p25p2_deinterleave_index(ww, &q, &r, &s, &t);
        if (ww < 0 || ww >= 4 || b < 0 || b >= 24) {
            continue;
        }
        if (frame_count >= 1) {
            int bit = x + bit_offsets[0] + vc_counter;
            ambe_fr1[ww][b] = p2xbit[bit];
            ambe_soft[0][ww][b] = p25p2_soft_bit_from_abs_bit(bit);
        }
        if (frame_count >= 2) {
            int bit = x + bit_offsets[1] + vc_counter;
            ambe_fr2[ww][b] = p2xbit[bit];
            ambe_soft[1][ww][b] = p25p2_soft_bit_from_abs_bit(bit);
        }
        if (frame_count >= 3) {
            int bit = x + bit_offsets[2] + vc_counter;
            ambe_fr3[ww][b] = p2xbit[bit];
            ambe_soft[2][ww][b] = p25p2_soft_bit_from_abs_bit(bit);
        }
        if (frame_count >= 4) {
            int bit = x + bit_offsets[3] + vc_counter;
            ambe_fr4[ww][b] = p2xbit[bit];
            ambe_soft[3][ww][b] = p25p2_soft_bit_from_abs_bit(bit);
        }
    }
}

static void
p25p2_collect_ess_b_fragment(dsd_state* state) {
    int slot = state->currentslot;
    if (state->fourv_counter[slot] == 0) {
        DSD_MEMSET(state->ess_b[slot], 0, sizeof(state->ess_b[slot]));
        DSD_MEMSET(state->ess_b_llr[slot], 0, sizeof(state->ess_b_llr[slot]));
    }
    state->p25_p2_ess_b_stale[slot] = 0U;
    for (int i = 0; i < 24; i++) {
        int out = i + (state->fourv_counter[slot] * 24);
        int in = i + 148 + vc_counter;
        state->ess_b[slot][out] = p2xbit[in];
        state->ess_b_llr[slot][out] = p2xllr[in];
    }
}

static void
p25p2_increment_fourv_counter(dsd_state* state) {
    int slot = state->currentslot;
    state->fourv_counter[slot]++;
    if (state->fourv_counter[slot] > 3) {
        state->fourv_counter[slot] = 0;
    }
}

/* The superframe pair (0-5) of the timeslot being dispatched: (p2_scramble_offset + ts_counter) % 12 is its
   superframe timeslot, and a pair is the two slots' timeslots 2b and 2b+1. Pairs 0-4 carry voice, pair 5 the SACCH. */
static int
p25p2_pair_index(const dsd_state* state) {
    const int timeslot = (((state->p2_scramble_offset % 12) + 12) + ts_counter) % 12;
    return timeslot / 2;
}

static void
p25p2_open_mbe_for_ready_slot(dsd_opts* opts, dsd_state* state, int slot) {
    if (!opts || !state || slot < 0 || slot > 1 || !p25_crypto_audio_permitted(opts, state, slot)
        || !state->p25_p2_audio_allowed[slot] || opts->mbe_out_dir[0] == 0) {
        return;
    }
    if (slot == 0 && opts->mbe_out_f == NULL) {
        openMbeOutFile(opts, state);
    }
    if (slot == 1 && opts->mbe_out_fR == NULL) {
        openMbeOutFileR(opts, state);
    }
}

// Synthesize one frame of the burst and queue it for the slot's playout, or note the gated frame, which closes the
// slot's stream: a muted slot plays nothing and fills nothing (issue #651).
static void
p25p2_decode_and_store_voice_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_soft[4][24]) {
    int slot = state->currentslot;
    if (slot != 0 && slot != 1) {
        return;
    }
    if (!p25_crypto_audio_permitted(opts, state, slot) || !state->p25_p2_audio_allowed[slot]) {
        dsd_mbe_log_ambe_soft_frame(opts, state, ambe_soft);
        dsd_p25p2_playout_note_muted(state, slot);
        return;
    }
    p25p2_open_mbe_for_ready_slot(opts, state, slot);
    processMbeFrameSoft(opts, state, NULL, ambe_soft, NULL);
    dsd_p25p2_playout_stage_decided(opts, state, slot, p25p2_pair_index(state), s_burst_serial,
                                    p25p2_burst_verdict_for(slot));
}

/* What the timeslot being dispatched carried for its slot, for dsd_p25p2_playout_burst_done(). */
static dsd_p25p2_burst_kind s_ts_kind = DSD_P25P2_BURST_LOST;

static void
process_4V(dsd_opts* opts, dsd_state* state) {
    dsd_vocoder_soft_bit ambe_soft[4][4][24] = {{{{0}}}};

    s_burst_serial++;
    s_ts_kind = DSD_P25P2_BURST_4V;
    p25p2_prepare_voice_crypto(opts, state);
    p25p2_emit_voice_activity(opts, state);
    p25p2_unpack_voice_frames(4, ambe_soft);
    p25p2_collect_ess_b_fragment(state);
    p25p2_increment_fourv_counter(state);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    for (int frame = 0; frame < 4; frame++) {
        p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[frame]);
    }
}

/* The ESS helpers take the burst's slot, which p25p2_process_ess() checked once (0 or 1), and never re-read
   state->currentslot. */
static void
p25p2_ess_load_payload_and_parity(const dsd_state* state, int slot, int payload[96], int parity[168]) {
    for (int i = 0; i < 96; i++) {
        payload[i] = state->ess_b[slot][i];
    }
    for (int i = 0; i < 168; i++) {
        parity[i] = ess_a[slot][i];
    }
}

/* Returns 1 when the ESS decodes, with @p soft_depth the soft erasures the decode needed (0 for a hard decode). */
static int
p25p2_ess_decode_with_soft_erasures(dsd_state* state, int slot, int payload[96], int parity[168], int* ec,
                                    int* soft_depth) {
    *soft_depth = 0;
    *ec = ez_rs28_ess(payload, parity, NULL, 0);
    if (*ec >= 0 && *ec < 15) {
        return 1;
    }

    int original_payload[96];
    int original_parity[168];
    p25p2_ess_load_payload_and_parity(state, slot, payload, parity);
    DSD_MEMCPY(original_payload, payload, sizeof(original_payload));
    DSD_MEMCPY(original_parity, parity, sizeof(original_parity));

    int erasures[44];
    int n_erasures = p25p2_ess_soft_erasures_ranked(state->ess_b_llr[slot], ess_a_llr[slot], erasures, 28);
    for (int n = 1; n <= n_erasures; n++) {
        DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
        DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
        *ec = ez_rs28_ess(payload, parity, erasures, n);
        if (*ec >= 0) {
            *soft_depth = n;
            state->p25_p2_soft_ess_ok++;
            if ((unsigned int)n > state->p25_p2_soft_ess_max_depth) {
                state->p25_p2_soft_ess_max_depth = (unsigned int)n;
            }
            return 1;
        }
    }

    DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
    DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
    return 0;
}

static int
p25p2_ess_algid_from_payload(const int payload[96]) {
    int algid = 0;
    for (short i = 0; i < 8; i++) {
        algid = algid << 1;
        algid = algid | payload[i];
    }
    return algid;
}

static void
p25p2_ess_payload_to_hex(const int payload[96], unsigned long long int* essb_hex1, unsigned long long int* essb_hex2) {
    *essb_hex1 = 0;
    *essb_hex2 = 0;
    for (int i = 0; i < 32; i++) {
        *essb_hex1 = (*essb_hex1 << 1) | (unsigned long long int)payload[i];
    }
    for (int i = 0; i < 64; i++) {
        *essb_hex2 = (*essb_hex2 << 1) | (unsigned long long int)payload[i + 32];
    }
}

typedef struct {
    unsigned long long int essb_hex1;
    unsigned long long int essb_hex2;
    uint64_t mi;
    int algid;
    int keyid;
    int corrections;
    int accepted;
    /* Soft erasures the decode needed; 0 for a hard decode, which checked every parity symbol. */
    int soft_depth;
} p25p2_ess_result;

static p25p2_ess_result
p25p2_ess_decode(dsd_state* state, int slot) {
    int payload[96] = {0};
    int parity[168] = {0};
    p25p2_ess_load_payload_and_parity(state, slot, payload, parity);

    p25p2_ess_result result = {.corrections = 69};
    result.accepted =
        p25p2_ess_decode_with_soft_erasures(state, slot, payload, parity, &result.corrections, &result.soft_depth);
    result.algid = p25p2_ess_algid_from_payload(payload);
    p25p2_ess_payload_to_hex(payload, &result.essb_hex1, &result.essb_hex2);
    result.keyid = (int)((result.essb_hex1 >> 8) & 0xFFFF);
    result.mi = ((result.essb_hex1 & 0xFF) << 56) | ((result.essb_hex2 & 0xFFFFFFFFFFFFFF00) >> 8);
    return result;
}

static double
p25p2_frame_mac_hold_s(double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->p25_mac_hold_is_set) {
        return cfg->p25_mac_hold_s;
    }
    return fallback;
}

static double
p25p2_frame_vc_grace_s(double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->p25_vc_grace_is_set) {
        return cfg->p25_vc_grace_s;
    }
    return fallback;
}

// Re-open a slot's audio gate once its ESS resolves the classification the
// gate was waiting on. The canonical call state -- not the slot's burst hint,
// which only records the last MAC PDU decoded -- decides whether the slot is
// in a call: dsd_p25p2_decode_audio_allowed() requires an ACTIVE epoch, and
// MAC_END/MAC_IDLE run p25_crypto_reset_slot(), which drops the classification
// p25_crypto_audio_permitted() checks just above, so post-transmission states
// stay closed. The media-rejection latch keeps a denied identity closed here.
static void
p25p2_ess_maybe_enable_audio_slot(const dsd_opts* opts, dsd_state* state, int slot, int alg) {
    if (state->p25_p2_media_rejected[slot]) {
        state->p25_p2_audio_allowed[slot] = 0;
        return;
    }
    if (!p25_crypto_audio_permitted(opts, state, slot)) {
        state->p25_p2_audio_allowed[slot] = 0;
        return;
    }
    if (state->p25_p2_audio_allowed[slot] != 0) {
        return;
    }
    // Only a resolved classification re-opens the gate: under follow mode the
    // encrypted-audio unmute override makes p25_crypto_audio_permitted() above
    // answer yes for UNKNOWN/ENCRYPTED_PENDING, but that override exists for
    // audio whose classification is settled, not for an unresolved probe.
    const dsd_p25_crypto_state crypto = state->p25_crypto_state[slot];
    if (crypto == DSD_P25_CRYPTO_UNKNOWN || crypto == DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
        return;
    }
    dsd_p25p2_burst_decision decision;
    int decision_valid = 0;
    if (dsd_p25p2_decode_audio_allowed_verdict(opts, state, slot, alg, &decision, &decision_valid)) {
        state->p25_p2_audio_allowed[slot] = 1;
    }
    if (decision_valid) {
        p25p2_note_burst_verdict(slot, &decision);
    }
}

static void
p25p2_ess_apply_slot0(dsd_opts* opts, dsd_state* state, const p25p2_ess_result* result) {
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE2, 0, result->algid, result->keyid, result->mi,
                             p25p2_active_target(state, 0U));
    p25p2_ess_maybe_enable_audio_slot(opts, state, 0, state->payload_algid);
    p25p2_audio_gate_diag(opts, state, "ess-slot0");

    if (state->payload_algid == 0x80 || state->payload_algid == 0x0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " VCH 1 -");
    DSD_FPRINTF(stderr, " ALG ID: 0x%02X", state->payload_algid);
    DSD_FPRINTF(stderr, " KEY ID: 0x%04X", state->payload_keyid);
    DSD_FPRINTF(stderr, " MI: 0x%016llX", state->payload_miP);
    DSD_FPRINTF(stderr, " ESSB");

    if (dsd_key_scalar_present(state, 0) && state->payload_algid == 0xAA) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 10U, 1));
    }
    if (dsd_key_scalar_present(state, 0) && state->payload_algid == 0x81) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 16U, 1));
    }
    if ((state->payload_algid == 0x84 || state->payload_algid == 0x89) && state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        const unsigned long long segments[4] = {state->A1[0], state->A2[0], state->A3[0], state->A4[0]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, opts->show_keys, segments,
                                                   (state->payload_algid == 0x84) ? 4U : 2U));
    }

    if (state->payload_algid == 0x84 || state->payload_algid == 0x89) {
        p25_lfsr128_slot(state, 0);
    }
}

static void
p25p2_ess_apply_slot1(dsd_opts* opts, dsd_state* state, const p25p2_ess_result* result) {
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE2, 1, result->algid, result->keyid, result->mi,
                             p25p2_active_target(state, 1U));
    p25p2_ess_maybe_enable_audio_slot(opts, state, 1, state->payload_algidR);
    p25p2_audio_gate_diag(opts, state, "ess-slot1");

    if (state->payload_algidR == 0x80 || state->payload_algidR == 0x0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " VCH 2 -");
    DSD_FPRINTF(stderr, " ALG ID: 0x%02X", state->payload_algidR);
    DSD_FPRINTF(stderr, " KEY ID: 0x%04X", state->payload_keyidR);
    DSD_FPRINTF(stderr, " MI: 0x%016llX", state->payload_miN);
    DSD_FPRINTF(stderr, " ESSB");

    if (dsd_key_scalar_present(state, 1) && state->payload_algidR == 0xAA) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->RR, 10U, 1));
    }
    if (dsd_key_scalar_present(state, 1) && state->payload_algidR == 0x81) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->RR, 16U, 1));
    }
    if ((state->payload_algidR == 0x84 || state->payload_algidR == 0x89) && state->aes_key_loaded[1] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        const unsigned long long segments[4] = {state->A1[1], state->A2[1], state->A3[1], state->A4[1]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, opts->show_keys, segments,
                                                   (state->payload_algidR == 0x84) ? 4U : 2U));
    }

    if (state->payload_algidR == 0x84 || state->payload_algidR == 0x89) {
        p25_lfsr128_slot(state, 1);
    }
}

static void
p25p2_ess_handle_decode_failure(dsd_state* state, int slot) {
    state->p25_p2_rs_ess_err++;

    if (slot == 0 && state->payload_algid != 0x80 && state->payload_keyid != 0 && state->payload_miP != 0) {
        LFSRP(state);
    }
    if (slot == 1 && state->payload_algidR != 0x80 && state->payload_keyidR != 0 && state->payload_miN != 0) {
        LFSRP(state);
    }
    if (slot == 0 && (state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        p25_lfsr128_slot(state, 0);
    }
    if (slot == 1 && (state->payload_algidR == 0x84 || state->payload_algidR == 0x89)) {
        p25_lfsr128_slot(state, 1);
    }
}

static int
p25p2_ess_stage_rekey(dsd_state* state, int slot, const p25p2_ess_result* result) {
    if (result->algid == 0 || state->p25_crypto_state[slot] != DSD_P25_CRYPTO_DECRYPTABLE) {
        return 0;
    }

    const int current_algid = slot == 0 ? state->payload_algid : state->payload_algidR;
    const int current_keyid = slot == 0 ? state->payload_keyid : state->payload_keyidR;
    if (current_algid == result->algid && current_keyid == result->keyid) {
        return 0;
    }

    state->p25_p2_rekey[slot].algid = (uint8_t)result->algid;
    state->p25_p2_rekey[slot].keyid = (uint16_t)result->keyid;
    state->p25_p2_rekey[slot].mi = result->mi;
    state->p25_p2_rekey[slot].pending = 1U;
    return 1;
}

static void
p25p2_ess_apply_result(dsd_opts* opts, dsd_state* state, int slot, const p25p2_ess_result* result) {
    if (slot < 0 || slot > 1) {
        return;
    }
    DSD_MEMSET(&state->p25_p2_rekey[slot], 0, sizeof(state->p25_p2_rekey[slot]));
    if (slot == 0) {
        p25p2_ess_apply_slot0(opts, state, result);
    } else {
        p25p2_ess_apply_slot1(opts, state, result);
    }
}

static void
p25p2_commit_deferred_rekeys(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        const dsd_p25_p2_rekey_state rekey = state->p25_p2_rekey[slot];
        if (!rekey.pending) {
            continue;
        }
        const p25p2_ess_result result = {
            .mi = rekey.mi,
            .algid = rekey.algid,
            .keyid = rekey.keyid,
            .accepted = 1,
        };
        DSD_FPRINTF(stderr, "%s", KYEL);
        p25p2_ess_apply_result(opts, state, slot, &result);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static int
p25p2_has_deferred_rekeys(const dsd_state* state) {
    return state && (state->p25_p2_rekey[0].pending || state->p25_p2_rekey[1].pending);
}

/* Timeslots that passed without being dispatched (the rest of a window a second DUID error aborted, or a window a
   missed sync skipped) retire as lost: each slot's open stream fills its missed voice burst in place and every
   completed pair plays, so the two slots stay on the air timeline (issue #651). As after a dispatched pair, a deferred
   rekey is promoted once its pair has played, before the next voice burst decodes. @p first is a superframe
   timeslot. */
static void
p25p2_retire_timeslots(dsd_opts* opts, dsd_state* state, int first, int count) {
    for (int i = 0; i < count; i++) {
        const int timeslot = ((first + i) % 12 + 12) % 12;
        const int slot = timeslot & 1;
        dsd_p25p2_playout_burst_done(opts, state, slot, timeslot / 2, DSD_P25P2_BURST_LOST);
        if (slot == 1) {
            dsd_p25p2_playout_pair_done(opts, state);
            p25p2_commit_deferred_rekeys(opts, state);
        }
    }
}

/* A rekey whose pair will not complete (an abort, or a window that broke continuity) is promoted at once: the pairs
   before the boundary already played or stay queued as PCM, and applying the identity purges the abandoned old-key
   vocoder state before subsequent voice decodes. */
static void
p25p2_resolve_deferred_rekeys(dsd_opts* opts, dsd_state* state) {
    if (!p25p2_has_deferred_rekeys(state)) {
        return;
    }
    p25p2_commit_deferred_rekeys(opts, state);
}

void
p25p2_process_ess(dsd_opts* opts, dsd_state* state, int defer_rekey) {
    /* The burst's slot, checked once: every ESS helper below indexes the slots' arrays with it. A slot out of range
       has no ESS to decode. */
    const int slot = state->currentslot;
    if (slot < 0 || slot > 1) {
        return;
    }
    /* The slot's ESS_B is the carrier left's (p25p2_frame_forget_carrier(), issue #575): nothing to decode until a 4V
       burst on this carrier collects a fragment. */
    if (state->p25_p2_ess_b_stale[slot]) {
        return;
    }
    const p25p2_ess_result result = p25p2_ess_decode(state, slot);

    DSD_FPRINTF(stderr, "%s", KYEL);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " VCH %d - ESS_B %08llX%016llX ERR = %02d", slot + 1, result.essb_hex1, result.essb_hex2,
                    result.corrections);
    }

    if (result.accepted) {
        state->p25_p2_rs_ess_ok++;
        state->p25_p2_rs_ess_corr += (unsigned int)result.corrections;
        /* The ESS is read from the descrambled 4V/2V bits, so a decode that checked its parity proves the seed. One
           that leaned on soft erasures did not check it: erasing all 28 parity symbols always succeeds, whatever seed
           the bits were descrambled with (issue #575). */
        if (result.soft_depth == 0) {
            p25p2_note_seed_proven(opts, state);
        }
        if (!defer_rekey || !p25p2_ess_stage_rekey(state, slot, &result)) {
            p25p2_ess_apply_result(opts, state, slot, &result);
        }
    } else {
        p25p2_ess_handle_decode_failure(state, slot);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    state->fourv_counter[slot] = 0;
}

static void
p25p2_collect_ess_a(const dsd_state* state) {
    for (short i = 0; i < 96; i++) {
        int in = i + 148 + vc_counter;
        ess_a[state->currentslot][i] = p2xbit[in];
        ess_a_llr[state->currentslot][i] = p2xllr[in];
    }
    for (short i = 0; i < 72; i++) {
        int in = i + 246 + vc_counter;
        ess_a[state->currentslot][i + 96] = p2xbit[in];
        ess_a_llr[state->currentslot][i + 96] = p2xllr[in];
    }
}

static void
p25p2_post_2v_reset_crypto_state(dsd_state* state) {
    if (state->currentslot == 0 && state->payload_algid == 0xAA) {
        state->dropL = 256;
    }
    if (state->currentslot == 1 && state->payload_algidR == 0xAA) {
        state->dropR = 256;
    }
    if (state->currentslot == 0
        && (state->payload_algid == 0x81 || state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        state->DMRvcL = 0;
    }
    if (state->currentslot == 1
        && (state->payload_algidR == 0x81 || state->payload_algidR == 0x84 || state->payload_algidR == 0x89)) {
        state->DMRvcR = 0;
    }
}

void
process_2V(dsd_opts* opts, dsd_state* state) {
    dsd_vocoder_soft_bit ambe_soft[4][4][24] = {{{{0}}}};

    s_burst_serial++;
    s_ts_kind = DSD_P25P2_BURST_2V;
    p25p2_prepare_voice_crypto(opts, state);
    p25p2_emit_voice_activity(opts, state);
    p25p2_unpack_voice_frames(2, ambe_soft);
    p25p2_collect_ess_a(state);

    p25p2_process_ess(opts, state, 1);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[0]);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[1]);
    p25p2_post_2v_reset_crypto_state(state);
}

//P2 Data Unit ID
static int
p25p2_duid_has_valid_site(const dsd_state* state) {
    return state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0 && state->p2_wacn != 0xFFFFF
           && state->p2_cc != 0xFFF && state->p2_sysid != 0xFFF;
}

static void
p25p2_duid_collect_and_decode(int timeslot_index) {
    static const int duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    uint8_t p2_duid_reliab[8];
    int p2_duid_complete = 0;
    if (timeslot_index < 0 || timeslot_index >= 4) {
        DSD_MEMSET(p2_duid, 0, sizeof(p2_duid));
        duid_decoded = -1;
        return;
    }
    for (int i = 0; i < 8; i++) {
        int abs_bit = duid_offsets[i] + (timeslot_index * 360);
        p2_duid[i] = p2bit[abs_bit];
        p2_duid_reliab[i] = p25p2_reliability_for_abs_bit(abs_bit);
        p2_duid_complete = (p2_duid_complete << 1) | p2_duid[i];
    }
    duid_decoded = p25p2_duid_lookup_soft((uint8_t)p2_duid_complete, p2_duid_reliab);
}

static void
p25p2_duid_print_frame_header(void) {
    char timestr[9];
    (void)dsd_format_local_datetime(dsd_decode_time(), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    DSD_FPRINTF(stderr, "\n%s        P25p2 ", timestr);
}

static int
p25p2_duid_is_lch_data_unit(void) {
    return duid_decoded != 3 && duid_decoded != 12 && duid_decoded != 13 && duid_decoded != 4;
}

static void
p25p2_duid_maybe_open_mbe(dsd_opts* opts, dsd_state* state, int slot) {
    if (duid_decoded != 0 && duid_decoded != 6) {
        return;
    }

    p25p2_open_mbe_for_ready_slot(opts, state, slot);
}

static int
p25p2_duid_set_channel_label_and_sacch(dsd_opts* opts, dsd_state* state) {
    if (p25p2_duid_is_lch_data_unit()) {
        if (state->currentslot == 0) {
            DSD_FPRINTF(stderr, "LCH 0 ");
            p25p2_duid_maybe_open_mbe(opts, state, 0);
            return 0;
        }
        if (state->currentslot == 1) {
            DSD_FPRINTF(stderr, "LCH 1 ");
            p25p2_duid_maybe_open_mbe(opts, state, 1);
            return 0;
        }
    }

    if (duid_decoded == 13) {
        DSD_FPRINTF(stderr, "LCCH  ");
        if (opts->trunk_is_tuned == 0) {
            rotate_symbol_out_file(opts, state);
        }
        return 0;
    }
    if (duid_decoded == 4) {
        DSD_FPRINTF(stderr, "LCCHs ");
        return 0;
    }
    DSD_FPRINTF(stderr, "SACCH ");
    return 1;
}

// Whether an LCCH burst on a tuned carrier has outlived its voice: the hangtime passed with no voice, the carrier is
// past its tune grace, and (trunking) neither slot's MAC signalling is fresh.
static int
p25p2_duid_lcch_idle_past_hangtime(const dsd_opts* opts, const dsd_state* state, time_t now) {
    if (duid_decoded != 13 || opts->trunk_is_tuned != 1 || ((now - state->last_vc_sync_time) <= opts->trunk_hangtime)) {
        return 0;
    }
    double vc_grace = p25p2_frame_vc_grace_s(0.75);
    double dt_since_tune = (state->p25_last_vc_tune_time != 0) ? (double)(now - state->p25_last_vc_tune_time) : 1e9;
    return dt_since_tune >= vc_grace;
}

// The trunking LCCH release verdict: idle past the hangtime with neither slot's MAC signalling fresh. Pure, so the
// LCCH branch can ask it again once the burst's MAC PDU has run (issue #651).
static int
p25p2_duid_lcch_release_due(const dsd_opts* opts, const dsd_state* state, time_t now) {
    if (opts->trunk_enable != 1 || !p25p2_duid_lcch_idle_past_hangtime(opts, state, now)) {
        return 0;
    }
    double mac_hold = p25p2_frame_mac_hold_s(0.75);
    int left_mac_active = (state->p25_p2_last_mac_active_m[0] > 0.0)
                          && (dsd_decode_now_mono_s() - state->p25_p2_last_mac_active_m[0]) <= mac_hold;
    int right_mac_active = (state->p25_p2_last_mac_active_m[1] > 0.0)
                           && (dsd_decode_now_mono_s() - state->p25_p2_last_mac_active_m[1]) <= mac_hold;
    return !(left_mac_active || right_mac_active);
}

static int
p25p2_duid_compute_pending_release(dsd_opts* opts, dsd_state* state, time_t now) {
    if (!p25p2_duid_lcch_idle_past_hangtime(opts, state, now)) {
        return 0;
    }
    if (opts->trunk_enable == 1) {
        return p25p2_duid_lcch_release_due(opts, state, now);
    }

    const double ended_m = dsd_decode_now_mono_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, ended_m) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    (void)dsd_recent_activity_clear_all(state);
    dsd_p25p2_playout_discard(opts, state);
    opts->trunk_is_tuned = 0;
    return 0;
}

static void
p25p2_duid_clear_idle_state(const dsd_opts* opts, dsd_state* state, time_t now) {
    UNUSED(now);
    if (duid_decoded == 13 && opts->trunk_is_tuned == 0
        && dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS) > 0) {
        dsd_p25p2_playout_reset(state, -1);
    }
}

static void
p25p2_duid_refresh_recent_voice(const dsd_opts* opts, dsd_state* state, time_t now) {
    if (state->p25_p2_audio_allowed[state->currentslot] || opts->trunk_tune_enc_calls == 1) {
        state->last_vc_sync_time = now;
        state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    }
}

/* Whether the window being dispatched knows its superframe position: a channel-1 I-ISCH decoded in it, or its symbol
   distance from the previous window proved where it starts (issue #651). Without it the slot parity and the
   descrambler offset are guesses, and no slot-attributed burst may act. */
static int s_window_proven = 1;

// A voice burst (DUID 0: 4V, 6: 2V), decoded only on a site whose descrambler seed is set.
static void
p25p2_duid_dispatch_voice(dsd_opts* opts, dsd_state* state, time_t now, int valid_site) {
    if (duid_decoded == 0) {
        DSD_FPRINTF(stderr, " 4V %d", state->fourv_counter[state->currentslot] + 1);
    } else {
        DSD_FPRINTF(stderr, " 2V");
    }
    if (!valid_site) {
        return;
    }
    p25p2_duid_refresh_recent_voice(opts, state, now);
    if (duid_decoded == 0) {
        process_4V(opts, state);
    } else {
        process_2V(opts, state);
    }
}

// Return to the control channel, and tear the call down only once the receiver has left the voice channel: a return
// the tuner refuses or defers (or a reacquire the state machine holds the channel for) keeps the channel, and with it
// the slots' gates, crypto and queued voice (issue #651).
static void
p25p2_release_then_teardown(dsd_opts* opts, dsd_state* state, const char* reason) {
    p25_sm_release(NULL, opts, state, reason);
    if (opts->trunk_is_tuned == 1 && p25_sm_get_state(p25_sm_get_ctx()) == P25_SM_TUNED) {
        return;
    }
    p25p2_teardown_call(opts, state);
}

// An LCCH (DUID 13). The verdict computed before the burst is stale once its MAC PDU ran: a retune it accepted moved
// the receiver (the token), and an assignment it accepted on this carrier refreshed the voice and tune times the
// verdict reads. Release only if it still holds (issue #651).
static void
p25p2_duid_dispatch_lcch(dsd_opts* opts, dsd_state* state, time_t now, int p2_pending_release) {
    s_ts_kind = DSD_P25P2_BURST_OTHER;
    state->p2_is_lcch = 1;
    p25p2_process_sacchc(opts, state, ts_counter);
    if (p2_pending_release && !p25p2_retune_token_changed(&s_window_token)
        && p25p2_duid_lcch_release_due(opts, state, now)) {
        p25p2_release_then_teardown(opts, state, "p2-lcch-timeout");
    }
}

static void DSD_ATTR_USED
p25p2_duid_dispatch(dsd_opts* opts, dsd_state* state, time_t now, int p2_pending_release, int* err_counter) {
    int valid_site = p25p2_duid_has_valid_site(state);
    s_ts_kind = DSD_P25P2_BURST_LOST;
    if (!s_window_proven && duid_decoded != 13 && duid_decoded != 4) {
        // Voice, FACCH and SACCH name their slot by the timeslot parity this window cannot prove: a CRC proves a MAC
        // payload, not which slot it belongs to.
        DSD_FPRINTF(stderr, " (unaligned)");
        return;
    }
    switch (duid_decoded) {
        case 0:
        case 6: p25p2_duid_dispatch_voice(opts, state, now, valid_site); break;
        case 3:
            s_ts_kind = DSD_P25P2_BURST_SACCH;
            if (valid_site) {
                process_SACCHs(opts, state);
            }
            break;
        case 12:
            s_ts_kind = DSD_P25P2_BURST_SACCH;
            p25p2_process_sacchc(opts, state, ts_counter);
            break;
        case 15:
            s_ts_kind = DSD_P25P2_BURST_OTHER;
            p25p2_process_facchc(opts, state, ts_counter);
            break;
        case 9:
            s_ts_kind = DSD_P25P2_BURST_OTHER;
            if (valid_site) {
                process_FACCHs(opts, state);
            }
            break;
        case 13: p25p2_duid_dispatch_lcch(opts, state, now, p2_pending_release); break;
        case 4:
            s_ts_kind = DSD_P25P2_BURST_OTHER;
            if (valid_site) {
                state->p2_is_lcch = 1;
                process_SACCHs(opts, state);
            }
            break;
        default:
            DSD_FPRINTF(stderr, " DUID ERR %d", duid_decoded);
            (*err_counter)++;
            break;
    }
}

static int
p25p2_duid_should_abort(dsd_opts* opts, dsd_state* state, int err_counter) {
    if (err_counter <= 1) {
        return 0;
    }

    // The aborting timeslot and the rest of the window pass undispatched: they retire as lost.
    p25p2_retire_timeslots(opts, state, state->p2_scramble_offset + ts_counter, 4 - ts_counter);
    p25p2_resolve_deferred_rekeys(opts, state);
    state->p2_is_lcch = 0;
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    return 1;
}

void
p25p2_duid_post_timeslot(dsd_opts* opts, dsd_state* state, int timeslot_index, int sacch_status) {
    ts_counter = timeslot_index;
    UNUSED(sacch_status);

    dsd_event_sync_slot(opts, state, 0);
    dsd_event_sync_slot(opts, state, 1);

    if (dsd_telemetry_is_active()) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    vc_counter = vc_counter + 360;

    // The playout learns what this timeslot carried for its slot; after slot 2's timeslot the pair is complete and
    // plays (issue #651).
    const int slot = state->currentslot;
    if (slot == 0 || slot == 1) {
        dsd_p25p2_playout_burst_done(opts, state, slot, p25p2_pair_index(state), s_ts_kind);
    }
    if (slot == 1) {
        dsd_p25p2_playout_pair_done(opts, state);
        // The pair's frames have played: promote any ESS identity change only now, so its purge cannot reach the
        // audio decoded under the old one. Every pair commits, whatever the output format or rate.
        p25p2_commit_deferred_rekeys(opts, state);
    }
    s_ts_kind = DSD_P25P2_BURST_LOST;

    if (state->currentslot == 0) {
        state->currentslot = 1;
    } else {
        state->currentslot = 0;
    }
}

// A slot still occupies the carrier when its gate is open, it holds buffered
// audio, or its MAC signaling is fresh inside the hold window. The audio gate
// alone cannot say "idle": an encryption-lockout-suppressed transmission
// keeps its gate closed for its whole life while MAC_PTT/ACTIVE repeats prove
// the site is still transmitting on the slot -- the same signals the LCCH
// pending-release check consumes. Latching p25_sm_force_release on a slot the
// SM is deliberately holding (classification in flight, companion bridging)
// would bypass every guard the SM applies, because the forced-release tick
// runs unguarded.
static int
p25p2_frame_slot_recently_occupied(const dsd_state* state, int slot, double mac_hold_s) {
    if (state->p25_p2_audio_allowed[slot] || state->p25_p2_audio_ring_count[slot] > 0) {
        return 1;
    }
    return (state->p25_p2_last_mac_active_m[slot] > 0.0)
           && (dsd_decode_now_mono_s() - state->p25_p2_last_mac_active_m[slot]) <= mac_hold_s;
}

static void DSD_ATTR_USED
p25p2_duid_fallback_release(dsd_opts* opts, dsd_state* state) {
    if (opts->trunk_enable != 1 || opts->trunk_is_tuned != 1) {
        return;
    }

    time_t now2 = dsd_decode_time();
    int no_recent_voice = (state->last_vc_sync_time != 0) && ((now2 - state->last_vc_sync_time) > opts->trunk_hangtime);
    double mac_hold = p25p2_frame_mac_hold_s(0.75);
    int both_slots_idle = !p25p2_frame_slot_recently_occupied(state, 0, mac_hold)
                          && !p25p2_frame_slot_recently_occupied(state, 1, mac_hold);
    double dt_since_tune = (state->p25_last_vc_tune_time != 0) ? (double)(now2 - state->p25_last_vc_tune_time) : 1e9;
    double vc_grace = p25p2_frame_vc_grace_s(0.75);
    if (no_recent_voice && both_slots_idle && dt_since_tune >= vc_grace) {
        state->p25_sm_force_release = 1;
        p25p2_release_then_teardown(opts, state, "p2-duid-timeout");
    }
}

void
p25p2_process_duid(dsd_opts* opts, dsd_state* state) {
    vc_counter = 0;
    int err_counter = 0;
    const time_t now = dsd_decode_time();
    s_window_token = p25p2_retune_token_now();

    for (ts_counter = 0; ts_counter < 4; ts_counter++) {
        duid_decoded = -2;
        p25p2_duid_collect_and_decode(ts_counter);

        p25p2_duid_print_frame_header();
        int sacch_status = p25p2_duid_set_channel_label_and_sacch(opts, state);
        int p2_pending_release = p25p2_duid_compute_pending_release(opts, state, now);
        p25p2_duid_clear_idle_state(opts, state, now);
        p25p2_duid_dispatch(opts, state, now, p2_pending_release, &err_counter);
        // A retune accepted by this burst (a grant, a return to the control channel) left the channel these bursts
        // were collected on; p25_p2_frame_reset() may also have zeroed the loop variable and the bit buffers, which
        // would otherwise replay the remaining timeslots as phantom 4V bursts (issue #651).
        if (p25p2_retune_token_changed(&s_window_token)) {
            return;
        }
        if (p25p2_duid_should_abort(opts, state, err_counter)) {
            return;
        }

        p25p2_duid_post_timeslot(opts, state, ts_counter, sacch_status);
        // A deferred rekey committed after the timeslot can lock the call out and return to the control channel.
        if (p25p2_retune_token_changed(&s_window_token)) {
            return;
        }
    }

    p25p2_duid_fallback_release(opts, state);
}

void
p25p2_frame_forget_carrier(dsd_state* state) {
    if (!state) {
        return;
    }
    state->p2_is_lcch = 0;
    // What the playout still holds was decoded on the carrier left; the engine's carrier boundary has drained it.
    dsd_p25p2_playout_reset(state, -1);
    for (int slot = 0; slot < 2; slot++) {
        state->fourv_counter[slot] = 0;
        DSD_MEMSET(&state->p25_p2_rekey[slot], 0, sizeof(state->p25_p2_rekey[slot]));
        /* With weak ESS_A symbols a 2V burst's decode erases all of the parity and returns whatever ESS_B it holds, so
           the carrier left's would come back as the new carrier's ALG/KID/MI: it goes, and stays unread until a 4V
           burst on the new carrier collects a fragment. */
        DSD_MEMSET(state->ess_b[slot], 0, sizeof(state->ess_b[slot]));
        DSD_MEMSET(state->ess_b_llr[slot], 0, sizeof(state->ess_b_llr[slot]));
        state->p25_p2_ess_b_stale[slot] = 1U;
    }
}

/* Where the window just collected sits in the superframe (issue #651). Its symbol distance from the previous window
   on this carrier says whether a missed sync skipped timeslots, and how many; a decoded channel-1 I-ISCH says where it
   starts. Returns 1 when the position is proven: by the ISCH, or (no ISCH decoded) by continuity, which then sets the
   descrambler offset the stale ISCH value would otherwise leave 4 timeslots behind. Skipped timeslots to retire come
   back in @p retire_first (superframe timeslot) and @p retire_count. A displacement nothing explains, or an ISCH start
   that contradicts continuity, breaks the playout's timeline (both streams close, so nothing fills across the gap, and
   what follows starts a later pair) and sets @p broken: the pair a deferred rekey waits for will not complete. */
static int
p25p2_window_position(dsd_state* state, uint32_t entry_symbol, int* retire_first, int* retire_count, int* broken) {
    *retire_first = 0;
    *retire_count = 0;
    int expected = -1;
    int close_streams = 0;
    if (s_prev_window.valid && s_prev_window.carrier_seq == state->carrier_seq
        && !p25p2_retune_token_changed(&s_prev_window.token)) {
        const uint32_t delta = entry_symbol - s_prev_window.end_symbol;
        if (delta == P25P2_SYNC_DIBITS) {
            expected = p25p2_timeslot_mod12(s_prev_window.start + 4);
        } else if (delta > P25P2_SYNC_DIBITS && ((delta - P25P2_SYNC_DIBITS) % P25P2_TIMESLOT_DIBITS) == 0U) {
            const uint32_t skipped = (delta - P25P2_SYNC_DIBITS) / P25P2_TIMESLOT_DIBITS;
            expected = p25p2_timeslot_mod12(s_prev_window.start + 4 + (int)(skipped % 12U));
            if (skipped >= 12U) {
                close_streams = 1;
            } else {
                *retire_first = p25p2_timeslot_mod12(s_prev_window.start + 4);
                *retire_count = (int)skipped;
            }
        } else {
            close_streams = 1;
        }
    }
    int proven = s_isch_located;
    if (proven) {
        if (expected >= 0 && p25p2_timeslot_mod12(state->p2_scramble_offset) != expected) {
            close_streams = 1;
            *retire_count = 0;
        }
    } else if (expected >= 0) {
        state->p2_scramble_offset = expected;
        proven = 1;
    }
    if (!proven) {
        close_streams = 1;
        *retire_count = 0;
    }
    if (close_streams) {
        dsd_p25p2_playout_break(state);
    }
    *broken = close_streams;
    return proven;
}

void
processP2(dsd_opts* opts, dsd_state* state) {
    const uint32_t entry_symbol = state->symbolcnt;
    state->dmr_stereo = 1;
    /* A superframe belongs to one carrier (issue #575). What the slots gathered over superframes of a carrier the
       receiver has since left goes before this one is collected; a superframe the carrier boundary split while it was
       collected (a replay read adopting a recorded retune) is dropped whole, as on a sync loss: its bursts read before
       the boundary would otherwise decode after it, reopening the calls it ended and proving the seed on the carrier
       the receiver moved to. */
    if (s_superframe_carrier.valid && s_superframe_carrier.seq != state->carrier_seq) {
        p25p2_frame_forget_carrier(state);
    }
    const uint32_t carrier_seq = state->carrier_seq;
    p2_dibit_buffer(opts, state);
    s_superframe_carrier.seq = state->carrier_seq;
    s_superframe_carrier.valid = 1;
    if (state->carrier_seq != carrier_seq) {
        p25p2_frame_forget_carrier(state);
        state->dmr_stereo = 0;
        DSD_FPRINTF(stderr, "\n");
        return;
    }

    //look at our ISCH values and determine location in superframe before running frame scramble
    s_isch_located = 0;
    for (framing_counter = 0; framing_counter < 4; framing_counter++) {
        //run ISCH in here so we know when to start descramble offset
        p25p2_process_isch(opts, state, framing_counter);
    }
    const p25p2_retune_token window_token = p25p2_retune_token_now();
    int retire_first = 0;
    int retire_count = 0;
    int broken = 0;
    s_window_proven = p25p2_window_position(state, entry_symbol, &retire_first, &retire_count, &broken);
    p25p2_window_set_slot_unproven(!s_window_proven);
    if (!s_window_proven) {
        // A MAC assembly continued from a window whose slot parity is unknown could complete against the wrong
        // slot: none is kept, and the VPDU drops this window's fragments.
        DSD_MEMSET(state->p25_mac_frag, 0, sizeof(state->p25_mac_frag));
    }

    //set initial current slot depending on offset value
    if (state->p2_scramble_offset % 2) {
        state->currentslot = 1;
    } else {
        state->currentslot = 0;
    }

    //frame_scramble runs lfsr and creates an array of unscrambled bits to pull from
    p25p2_process_frame_scramble(opts, state);

    // Timeslots a missed sync skipped play as lost before this window's bursts; after a break in continuity a deferred
    // rekey is promoted before them. A rekey promoted either way can lock the call out and leave the channel, and then
    // nothing of this window dispatches.
    if (broken) {
        p25p2_resolve_deferred_rekeys(opts, state);
    }
    if (retire_count > 0) {
        p25p2_retire_timeslots(opts, state, retire_first, retire_count);
    }

    //process DUID will run through all collected frames and handle them appropriately
    if (!p25p2_retune_token_changed(&window_token)) {
        p25p2_process_duid(opts, state);
    }

    // The next window measures its position from this one, unless this one's position was a guess or a retune
    // moved the receiver while it was dispatched.
    s_prev_window.valid = s_window_proven && !p25p2_retune_token_changed(&window_token);
    s_prev_window.carrier_seq = state->carrier_seq;
    s_prev_window.token = window_token;
    s_prev_window.start = p25p2_timeslot_mod12(state->p2_scramble_offset);
    s_prev_window.end_symbol = entry_symbol + P25P2_WINDOW_DIBITS;
    s_window_proven = 1;
    p25p2_window_set_slot_unproven(0);

    state->dmr_stereo = 0;
    state->p2_is_lcch = 0;

    DSD_FPRINTF(stderr, "\n");
}
