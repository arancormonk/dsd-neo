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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
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
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
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

static int
p25_p2_s16_frames_have_audio(short frames[18][160]) {
    for (int j = 0; j < 18; j++) {
        for (int i = 0; i < 160; i++) {
            if (frames[j][i] != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int
p25p2_next_voice_slot(dsd_state* state, int slot) {
    int idx = state->voice_counter[slot] % 18;
    state->voice_counter[slot]++;
    return idx;
}

// Clear per-slot audio gates, small audio rings, encryption indicators, and
// UI call banners for both logical slots. Intended for use on call teardown
// before returning to the control channel.
void
p25p2_teardown_call(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    // Flush any partial superframe worth of decoded audio so short calls
    // (or late-entry captures that end before a full superframe) still
    // produce audible output in int16 mode.
    if (opts && opts->floating_point == 0 && opts->pulse_digi_rate_out == 8000) {
        int has_l = p25_p2_s16_frames_have_audio(state->s_l4);
        int has_r = p25_p2_s16_frames_have_audio(state->s_r4);
        if (has_l || has_r) {
            // At teardown, slot gates may already be cleared by MAC_END/IDLE.
            // The s_l4/s_r4 buffers only contain decoded audio when a slot was
            // allowed at decode time, so use buffer presence as the playback
            // gate here to avoid dropping the tail of short clear calls.
            state->p25_p2_audio_allowed[0] = has_l ? 1 : 0;
            state->p25_p2_audio_allowed[1] = has_r ? 1 : 0;
            playSynthesizedVoiceSS18(opts, state);
        }
        state->voice_counter[0] = 0;
        state->voice_counter[1] = 0;
    }

    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    p25_crypto_reset_slot(state, 0);
    p25_crypto_reset_slot(state, 1);
    p25_p2_audio_ring_reset(state, -1);
    // Clear buffered short audio frames to avoid replaying stale samples on
    // subsequent short calls that never reach the normal SS18 playback path.
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    state->p25_p2_last_mac_active[0] = 0;
    state->p25_p2_last_mac_active[1] = 0;
    state->p25_p2_last_end_ptt[0] = 0;
    state->p25_p2_last_end_ptt[1] = 0;
    state->p25_call_is_packet[0] = 0;
    state->p25_call_is_packet[1] = 0;
    state->p25_call_emergency[0] = 0;
    state->p25_call_emergency[1] = 0;
    state->p25_call_priority[0] = 0;
    state->p25_call_priority[1] = 0;
    state->dmr_so = 0;
    state->dmr_soR = 0;
    state->p25_service_options_valid[0] = 0;
    state->p25_service_options_valid[1] = 0;
    state->payload_algid = 0;
    state->payload_keyid = 0;
    state->payload_miP = 0ULL;
    state->payload_algidR = 0;
    state->payload_keyidR = 0;
    state->payload_miN = 0ULL;
    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", "                     ");
    DSD_SNPRINTF(state->call_string[1], sizeof state->call_string[1], "%s", "                     ");
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

static int ts_counter = 0;         //timeslot counter for time slots 0-11
int p2bit[4320] = {0};             //4320
static uint8_t p2lbit[8640] = {0}; //bits generated by LFSR scrambler, doubling up for offset roll-over
static int p2xbit[4320] = {0};     //bits xored from p2bit and p2lbit

/* Per-bit soft metrics for captured 700 dibits (1400 bits). */
int16_t p2llr[1400] = {0};  /* bit LLRs before descramble */
int16_t p2xllr[1400] = {0}; /* bit LLRs after descramble */

static int dibit = 0;
static int vc_counter = 0;
static int framing_counter = 0;
static int voice = 0; // If voice in vch 0 or vch 1

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

// Reset all P25P2 frame processing global state variables.
// This must be called when tuning to a new P25P2 voice channel to clear stale
// data from the previous channel that would otherwise cause decode failures.
// The issue manifests as: first P25P2 tune works, but subsequent voice channel
// grants fail to lock with tanking EVM/SNR until retune to P25P1 control channel.
void
p25_p2_frame_reset(void) {
    // Reset counters
    ts_counter = 0;
    vc_counter = 0;
    framing_counter = 0;
    voice = 0;
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

static void
process_Frame_Scramble(dsd_opts* opts, const dsd_state* state) {
    UNUSED(opts);

    //The bits of the scramble sequence corresponding to signal bits that are not scrambled or not used are discarded.
    //descramble frame scrambled by LFSR of WACN, SysID, and CC(NAC)
    p25p2_generate_scramble_bits(state->p2_wacn, state->p2_sysid, state->p2_cc, p2lbit, 4320U);
    DSD_MEMCPY(p2lbit + 4320, p2lbit, 4320U * sizeof(p2lbit[0]));

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
        state->p25_service_options_valid[0] = 0;
    } else {
        state->dmr_soR = opcode;
        state->p25_service_options_valid[1] = 0;
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
        state->p25_service_options_valid[0] = 0;
    } else {
        state->dmr_soR = opcode;
        state->p25_service_options_valid[1] = 0;
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
        state->p25_service_options_valid[1] = 0;
    } else {
        state->dmr_so = opcode;
        state->p25_service_options_valid[0] = 0;
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
        state->p25_service_options_valid[1] = 0;
    } else {
        state->dmr_so = opcode;
        state->p25_service_options_valid[0] = 0;
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
            } else if (chan_num == 1 && isch_loc == 1) {
                state->p2_scramble_offset = 4 - framing_counter;
            } else if (chan_num == 1 && isch_loc == 2) {
                state->p2_scramble_offset = 8 - framing_counter;
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
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
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

    if (state->gi[slot] == 1) {
        return 0;
    }
    const int talkgroup = (slot == 0) ? state->lasttg : state->lasttgR;
    return p25_patch_tg_key_is_clear(state, talkgroup) || p25_patch_sg_key_is_clear(state, talkgroup);
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
    const int svc = (slot == 0) ? state->dmr_so : state->dmr_soR;
    if ((svc & 0x40) != 0 && !p25p2_voice_crypto_is_authoritatively_clear(state, slot)) {
        p25_sm_emit_crypto_pending(opts, state, slot);
    }
    if (p25_crypto_audio_permitted(opts, state, slot)) {
        const int alg = (slot == 0) ? state->payload_algid : state->payload_algidR;
        state->p25_p2_audio_allowed[slot] = dsd_p25p2_decode_audio_allowed(opts, state, slot, alg);
    }
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

static void
p25p2_reset_voice_counters_if_needed(dsd_state* state) {
    if (state->voice_counter[0] >= 18) {
        state->voice_counter[0] = 0;
    }
    if (state->voice_counter[1] >= 18) {
        state->voice_counter[1] = 0;
    }
}

static void
p25p2_store_decoded_voice_frame(dsd_state* state, int frame_index, int push_ring) {
    if (state->currentslot == 0) {
        int vc_idx = p25p2_next_voice_slot(state, 0);
        DSD_MEMCPY(state->f_l4[frame_index], state->audio_out_temp_buf, sizeof(state->audio_out_temp_buf));
        DSD_MEMCPY(state->s_l4[vc_idx], state->s_l, sizeof(state->s_l));
        DSD_MEMCPY(state->s_l4u[frame_index], state->s_lu, sizeof(state->s_lu));
        if (push_ring) {
            p25_p2_audio_ring_push(state, 0, state->f_l4[frame_index]);
        }
        return;
    }
    int vc_idx = p25p2_next_voice_slot(state, 1);
    DSD_MEMCPY(state->f_r4[frame_index], state->audio_out_temp_bufR, sizeof(state->audio_out_temp_bufR));
    DSD_MEMCPY(state->s_r4[vc_idx], state->s_r, sizeof(state->s_r));
    DSD_MEMCPY(state->s_r4u[frame_index], state->s_ru, sizeof(state->s_ru));
    if (push_ring) {
        p25_p2_audio_ring_push(state, 1, state->f_r4[frame_index]);
    }
}

static void
p25p2_zero_voice_frame(dsd_state* state, int frame_index) {
    if (state->currentslot == 0) {
        int vc_idx = p25p2_next_voice_slot(state, 0);
        DSD_MEMSET(state->f_l4[frame_index], 0, sizeof(state->f_l4[frame_index]));
        DSD_MEMSET(state->s_l4[vc_idx], 0, sizeof(state->s_l4[0]));
        return;
    }
    int vc_idx = p25p2_next_voice_slot(state, 1);
    DSD_MEMSET(state->f_r4[frame_index], 0, sizeof(state->f_r4[frame_index]));
    DSD_MEMSET(state->s_r4[vc_idx], 0, sizeof(state->s_r4[0]));
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

static void
p25p2_decode_and_store_voice_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_soft[4][24],
                                   int frame_index, int push_ring) {
    int slot = state->currentslot;
    if (slot != 0 && slot != 1) {
        p25p2_zero_voice_frame(state, frame_index);
        return;
    }
    if (!p25_crypto_audio_permitted(opts, state, slot) || !state->p25_p2_audio_allowed[slot]) {
        p25p2_zero_voice_frame(state, frame_index);
        return;
    }
    p25p2_open_mbe_for_ready_slot(opts, state, slot);
    processMbeFrameSoft(opts, state, NULL, ambe_soft, NULL);
    p25p2_store_decoded_voice_frame(state, frame_index, push_ring);
}

static void
process_4V(dsd_opts* opts, dsd_state* state) {
    dsd_vocoder_soft_bit ambe_soft[4][4][24] = {{{{0}}}};

    p25p2_prepare_voice_crypto(opts, state);
    p25p2_emit_voice_activity(opts, state);
    p25p2_unpack_voice_frames(4, ambe_soft);
    p25p2_collect_ess_b_fragment(state);
    p25p2_increment_fourv_counter(state);

    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }
    p25p2_reset_voice_counters_if_needed(state);

    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[0], 0, 1);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[1], 1, 0);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[2], 2, 0);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[3], 3, 0);
}

static void
p25p2_ess_load_payload_and_parity(dsd_state* state, int payload[96], int parity[168]) {
    for (int i = 0; i < 96; i++) {
        payload[i] = state->ess_b[state->currentslot][i];
    }
    for (int i = 0; i < 168; i++) {
        parity[i] = ess_a[state->currentslot][i];
    }
}

static int
p25p2_ess_decode_with_soft_erasures(dsd_state* state, int payload[96], int parity[168], int* ec) {
    *ec = ez_rs28_ess(payload, parity, NULL, 0);
    if (*ec >= 0 && *ec < 15) {
        return 1;
    }

    int original_payload[96];
    int original_parity[168];
    p25p2_ess_load_payload_and_parity(state, payload, parity);
    DSD_MEMCPY(original_payload, payload, sizeof(original_payload));
    DSD_MEMCPY(original_parity, parity, sizeof(original_parity));

    int erasures[44];
    int n_erasures = p25p2_ess_soft_erasures_ranked(state->ess_b_llr[state->currentslot], ess_a_llr[state->currentslot],
                                                    erasures, 28);
    for (int n = 1; n <= n_erasures; n++) {
        DSD_MEMCPY(payload, original_payload, sizeof(original_payload));
        DSD_MEMCPY(parity, original_parity, sizeof(original_parity));
        *ec = ez_rs28_ess(payload, parity, erasures, n);
        if (*ec >= 0) {
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
} p25p2_ess_result;

static p25p2_ess_result
p25p2_ess_decode(dsd_state* state) {
    int payload[96] = {0};
    int parity[168] = {0};
    p25p2_ess_load_payload_and_parity(state, payload, parity);

    p25p2_ess_result result = {.corrections = 69};
    result.accepted = p25p2_ess_decode_with_soft_erasures(state, payload, parity, &result.corrections);
    result.algid = p25p2_ess_algid_from_payload(payload);
    p25p2_ess_payload_to_hex(payload, &result.essb_hex1, &result.essb_hex2);
    result.keyid = (int)((result.essb_hex1 >> 8) & 0xFFFF);
    result.mi = ((result.essb_hex1 & 0xFF) << 56) | ((result.essb_hex2 & 0xFFFFFFFFFFFFFF00) >> 8);
    return result;
}

static double
p25p2_frame_mac_hold_s(const dsd_state* state, double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (state->p25_cfg_mac_hold_s > 0.0) {
        return state->p25_cfg_mac_hold_s;
    }
    if (cfg && cfg->p25_mac_hold_is_set) {
        return cfg->p25_mac_hold_s;
    }
    return fallback;
}

static double
p25p2_frame_vc_grace_s(const dsd_state* state, double fallback) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (state->p25_cfg_vc_grace_s > 0.0) {
        return state->p25_cfg_vc_grace_s;
    }
    if (cfg && cfg->p25_vc_grace_is_set) {
        return cfg->p25_vc_grace_s;
    }
    return fallback;
}

static void
p25p2_ess_maybe_enable_audio_slot(const dsd_opts* opts, dsd_state* state, int slot, int alg, int burst) {
    if (!p25_crypto_audio_permitted(opts, state, slot)) {
        state->p25_p2_audio_allowed[slot] = 0;
        return;
    }
    if (state->p25_p2_audio_allowed[slot] != 0) {
        return;
    }
    int in_call = ((burst >= 20 && burst <= 22) || voice);
    int allow = in_call && dsd_p25p2_decode_audio_allowed(opts, state, slot, alg);
    if (allow) {
        state->p25_p2_audio_allowed[slot] = 1;
    }
}

static void
p25p2_ess_apply_slot0(dsd_opts* opts, dsd_state* state, const p25p2_ess_result* result) {
    (void)p25_crypto_resolve(opts, state, DSD_P25_CRYPTO_PHASE2, 0, result->algid, result->keyid, result->mi,
                             state->lasttg);
    p25p2_ess_maybe_enable_audio_slot(opts, state, 0, state->payload_algid, state->dmrburstL);

    if (state->payload_algid == 0x80 || state->payload_algid == 0x0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " VCH 1 -");
    DSD_FPRINTF(stderr, " ALG ID: 0x%02X", state->payload_algid);
    DSD_FPRINTF(stderr, " KEY ID: 0x%04X", state->payload_keyid);
    DSD_FPRINTF(stderr, " MI: 0x%016llX", state->payload_miP);
    DSD_FPRINTF(stderr, " ESSB");

    if (state->R != 0 && state->payload_algid == 0xAA) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->R, 10U, 1));
    }
    if (state->R != 0 && state->payload_algid == 0x81) {
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
                             state->lasttgR);
    p25p2_ess_maybe_enable_audio_slot(opts, state, 1, state->payload_algidR, state->dmrburstR);

    if (state->payload_algidR == 0x80 || state->payload_algidR == 0x0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " VCH 2 -");
    DSD_FPRINTF(stderr, " ALG ID: 0x%02X", state->payload_algidR);
    DSD_FPRINTF(stderr, " KEY ID: 0x%04X", state->payload_keyidR);
    DSD_FPRINTF(stderr, " MI: 0x%016llX", state->payload_miN);
    DSD_FPRINTF(stderr, " ESSB");

    if (state->RR != 0 && state->payload_algidR == 0xAA) {
        char key_text[19];
        DSD_FPRINTF(stderr, " Key %s",
                    dsd_secret_format_hex(key_text, sizeof key_text, opts->show_keys, state->RR, 10U, 1));
    }
    if (state->RR != 0 && state->payload_algidR == 0x81) {
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
p25p2_ess_handle_decode_failure(dsd_state* state) {
    state->p25_p2_rs_ess_err++;

    if (state->currentslot == 0 && state->payload_algid != 0x80 && state->payload_keyid != 0
        && state->payload_miP != 0) {
        LFSRP(state);
    }
    if (state->currentslot == 1 && state->payload_algidR != 0x80 && state->payload_keyidR != 0
        && state->payload_miN != 0) {
        LFSRP(state);
    }
    if (state->currentslot == 0 && (state->payload_algid == 0x84 || state->payload_algid == 0x89)) {
        p25_lfsr128_slot(state, 0);
    }
    if (state->currentslot == 1 && (state->payload_algidR == 0x84 || state->payload_algidR == 0x89)) {
        p25_lfsr128_slot(state, 1);
    }
}

static int
p25p2_ess_stage_rekey(dsd_state* state, const p25p2_ess_result* result) {
    const int slot = state->currentslot;
    if (slot < 0 || slot > 1 || result->algid == 0 || state->p25_crypto_state[slot] != DSD_P25_CRYPTO_DECRYPTABLE) {
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

static void
p25p2_resolve_deferred_rekeys_on_abort(dsd_opts* opts, dsd_state* state) {
    if (!p25p2_has_deferred_rekeys(state)) {
        return;
    }

    if (opts && opts->pulse_digi_rate_out == 8000) {
        if (opts->floating_point == 1) {
            playSynthesizedVoiceFS4(opts, state);
        } else if (opts->floating_point == 0) {
            playSynthesizedVoiceSS18(opts, state);
            state->voice_counter[0] = 0;
            state->voice_counter[1] = 0;
        }
    }

    // If no output path is active, applying the identity still purges the
    // abandoned old-key buffers before subsequent voice can be decoded.
    p25p2_commit_deferred_rekeys(opts, state);
}

void
p25p2_process_ess(dsd_opts* opts, dsd_state* state, int defer_rekey) {
    const p25p2_ess_result result = p25p2_ess_decode(state);

    DSD_FPRINTF(stderr, "%s", KYEL);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, " VCH %d - ESS_B %08llX%016llX ERR = %02d", state->currentslot + 1, result.essb_hex1,
                    result.essb_hex2, result.corrections);
    }

    if (result.accepted) {
        state->p25_p2_rs_ess_ok++;
        state->p25_p2_rs_ess_corr += (unsigned int)result.corrections;
        if (!defer_rekey || !p25p2_ess_stage_rekey(state, &result)) {
            p25p2_ess_apply_result(opts, state, state->currentslot, &result);
        }
    } else {
        p25p2_ess_handle_decode_failure(state);
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
    if (state->currentslot >= 0 && state->currentslot < 2) {
        state->fourv_counter[state->currentslot] = 0;
    }
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

    p25p2_prepare_voice_crypto(opts, state);
    p25p2_emit_voice_activity(opts, state);
    p25p2_unpack_voice_frames(2, ambe_soft);
    p25p2_collect_ess_a(state);

    p25p2_process_ess(opts, state, 1);
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n");
    }

    p25p2_reset_voice_counters_if_needed(state);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[0], 0, 0);
    p25p2_decode_and_store_voice_frame(opts, state, ambe_soft[1], 1, 1);
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
    (void)dsd_format_local_datetime(time(NULL), DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
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

    voice = 1;
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

static int
p25p2_duid_compute_pending_release(dsd_opts* opts, dsd_state* state, time_t now) {
    if (duid_decoded != 13 || opts->trunk_is_tuned != 1 || ((now - state->last_vc_sync_time) <= opts->trunk_hangtime)) {
        return 0;
    }

    double vc_grace = p25p2_frame_vc_grace_s(state, 0.75);
    double dt_since_tune = (state->p25_last_vc_tune_time != 0) ? (double)(now - state->p25_last_vc_tune_time) : 1e9;
    if (dt_since_tune < vc_grace) {
        return 0;
    }

    double mac_hold = p25p2_frame_mac_hold_s(state, 0.75);
    int left_mac_active = (state->p25_p2_last_mac_active_m[0] > 0.0)
                          && (dsd_time_now_monotonic_s() - state->p25_p2_last_mac_active_m[0]) <= mac_hold;
    int right_mac_active = (state->p25_p2_last_mac_active_m[1] > 0.0)
                           && (dsd_time_now_monotonic_s() - state->p25_p2_last_mac_active_m[1]) <= mac_hold;
    if (opts->trunk_enable == 1) {
        return !(left_mac_active || right_mac_active);
    }

    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    opts->trunk_is_tuned = 0;
    return 0;
}

static void
p25p2_duid_clear_idle_state(const dsd_opts* opts, dsd_state* state, time_t now) {
    if (duid_decoded == 13 && ((now - state->last_active_time) > 2) && opts->trunk_is_tuned == 0) {
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
        state->voice_counter[0] = 0;
        state->voice_counter[1] = 0;
        DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
        DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    }
}

static void
p25p2_duid_refresh_recent_voice(const dsd_opts* opts, dsd_state* state, time_t now) {
    if (state->p25_p2_audio_allowed[state->currentslot] || opts->trunk_tune_enc_calls == 1) {
        state->last_vc_sync_time = now;
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    }
}

static void DSD_ATTR_USED
p25p2_duid_dispatch(dsd_opts* opts, dsd_state* state, time_t now, int p2_pending_release, int* err_counter) {
    int valid_site = p25p2_duid_has_valid_site(state);
    switch (duid_decoded) {
        case 0:
            DSD_FPRINTF(stderr, " 4V %d", state->fourv_counter[state->currentslot] + 1);
            if (valid_site) {
                p25p2_duid_refresh_recent_voice(opts, state, now);
                process_4V(opts, state);
            }
            break;
        case 6:
            DSD_FPRINTF(stderr, " 2V");
            if (valid_site) {
                p25p2_duid_refresh_recent_voice(opts, state, now);
                process_2V(opts, state);
            }
            break;
        case 3:
            if (valid_site) {
                process_SACCHs(opts, state);
            }
            break;
        case 12: p25p2_process_sacchc(opts, state, ts_counter); break;
        case 15: p25p2_process_facchc(opts, state, ts_counter); break;
        case 9:
            if (valid_site) {
                process_FACCHs(opts, state);
            }
            break;
        case 13:
            state->p2_is_lcch = 1;
            p25p2_process_sacchc(opts, state, ts_counter);
            if (p2_pending_release) {
                p25p2_teardown_call(opts, state);
                p25_sm_release(NULL, opts, state, "p2-lcch-timeout");
            }
            break;
        case 4:
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

    p25p2_resolve_deferred_rekeys_on_abort(opts, state);
    state->p2_is_lcch = 0;
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    return 1;
}

static int
p25p2_duid_output_float_pair(dsd_opts* opts, dsd_state* state, int sacch_status, int output_pair) {
    if (!output_pair || opts->floating_point != 1 || opts->pulse_digi_rate_out != 8000
        || (sacch_status != 0 && !p25p2_has_deferred_rekeys(state))) {
        return 0;
    }
    playSynthesizedVoiceFS4(opts, state);
    return 1;
}

static int
p25p2_duid_output_short_pair(dsd_opts* opts, dsd_state* state, int output_pair) {
    const int output_ready =
        state->voice_counter[0] >= 18 || state->voice_counter[1] >= 18 || p25p2_has_deferred_rekeys(state);
    if (!output_pair || !output_ready || opts->floating_point != 0 || opts->pulse_digi_rate_out != 8000) {
        return 0;
    }
    playSynthesizedVoiceSS18(opts, state);
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
    return 1;
}

void
p25p2_duid_post_timeslot(dsd_opts* opts, dsd_state* state, int timeslot_index, int sacch_status) {
    ts_counter = timeslot_index;
    const int output_pair = (ts_counter & 1) != 0;

    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }

    watchdog_event_history(opts, state, 0);
    dsd_p25_optional_hook_watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    dsd_p25_optional_hook_watchdog_event_current(opts, state, 1);

    vc_counter = vc_counter + 360;

    int audio_drained = p25p2_duid_output_float_pair(opts, state, sacch_status, output_pair);
    audio_drained |= p25p2_duid_output_short_pair(opts, state, output_pair);
    if (audio_drained) {
        // Both logical slots have reached the output stage. Promote any ESS
        // identity changes only now so their purge cannot discard or decrypt
        // the completed boundary superframe with the next stream.
        p25p2_commit_deferred_rekeys(opts, state);
    }

    if (state->currentslot == 0) {
        state->currentslot = 1;
    } else {
        state->currentslot = 0;
    }
    if (ts_counter & 1) {
        voice = 0;
    }
}

static void DSD_ATTR_USED
p25p2_duid_fallback_release(dsd_opts* opts, dsd_state* state) {
    if (opts->trunk_enable != 1 || opts->trunk_is_tuned != 1) {
        return;
    }

    time_t now2 = time(NULL);
    int no_recent_voice = (state->last_vc_sync_time != 0) && ((now2 - state->last_vc_sync_time) > opts->trunk_hangtime);
    int both_slots_idle = (state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0);
    double dt_since_tune = (state->p25_last_vc_tune_time != 0) ? (double)(now2 - state->p25_last_vc_tune_time) : 1e9;
    double vc_grace = p25p2_frame_vc_grace_s(state, 0.75);
    if (no_recent_voice && both_slots_idle && dt_since_tune >= vc_grace) {
        state->p25_sm_force_release = 1;
        p25p2_teardown_call(opts, state);
        p25_sm_release(NULL, opts, state, "p2-duid-timeout");
    }
}

void
p25p2_process_duid(dsd_opts* opts, dsd_state* state) {
    vc_counter = 0;
    int err_counter = 0;
    const time_t now = time(NULL);

    for (ts_counter = 0; ts_counter < 4; ts_counter++) {
        duid_decoded = -2;
        p25p2_duid_collect_and_decode(ts_counter);

        p25p2_duid_print_frame_header();
        int sacch_status = p25p2_duid_set_channel_label_and_sacch(opts, state);
        int p2_pending_release = p25p2_duid_compute_pending_release(opts, state, now);
        p25p2_duid_clear_idle_state(opts, state, now);
        p25p2_duid_dispatch(opts, state, now, p2_pending_release, &err_counter);
        if (p25p2_duid_should_abort(opts, state, err_counter)) {
            goto END;
        }

        p25p2_duid_post_timeslot(opts, state, ts_counter, sacch_status);
    }

    p25p2_duid_fallback_release(opts, state);
END:
    voice = 0;
}

void
processP2(dsd_opts* opts, dsd_state* state) {
    state->dmr_stereo = 1;
    p2_dibit_buffer(opts, state);
    voice = 0;

    //look at our ISCH values and determine location in superframe before running frame scramble
    for (framing_counter = 0; framing_counter < 4; framing_counter++) {
        //run ISCH in here so we know when to start descramble offset
        p25p2_process_isch(opts, state, framing_counter);
    }

    //set initial current slot depending on offset value
    if (state->p2_scramble_offset % 2) {
        state->currentslot = 1;
    } else {
        state->currentslot = 0;
    }

    //frame_scramble runs lfsr and creates an array of unscrambled bits to pull from
    process_Frame_Scramble(opts, state);

    //process DUID will run through all collected frames and handle them appropriately
    p25p2_process_duid(opts, state);

    state->dmr_stereo = 0;
    state->p2_is_lcch = 0;

    DSD_FPRINTF(stderr, "\n");
}
