// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A P25 Phase 2 CQPSK carrier whose sync words come through with a few symbols off, as on a simulcast site: the hunt
 * matched them exactly, so each such word lost the four timeslots behind it (and when the S-ISCH inside the window was
 * hit too, nothing found the window again). After a window its own I-ISCH placed, the hunt now accepts a word within four
 * bit errors where the next window's sync must be, and the window behind it dispatches once its own ISCHs and DUIDs
 * agree with where the anchor says it starts.
 *
 * The stream is real-shaped and synthetic: windows laid end to end as on a continuous carrier, each after its S-ISCH,
 * built as a site sends them (p25p2_burst_builder.h: scrambled FACCH bursts carrying a MAC_IDLE with its CRC, the
 * ISCHs between them), at the CQPSK demodulator's symbol levels. It runs through the real frame sync, symbol reader and
 * dibit slicer on the RTL CQPSK path, the real Phase 2 dispatcher and processP2() with the real Reed-Solomon, CRC and
 * MAC handling. Only the VPDU decoder is a stand-in, which counts the MAC PDUs that reached it.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_state_buffers.h"
#include "p25p2_burst_builder.h"

dsd_frame_verdict dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state);

#define CODE_OF(w, s, n) ((((uint64_t)(w)) << 24) | (((uint64_t)(s)) << 12) | (uint64_t)(n))
#define TEST_CODE        CODE_OF(0xBEE00U, 0x164U, 0x161U)

enum {
    DUID_FACCH_S = 9,
    MAC_IDLE = 3,
    SYNC_DIBITS = 20,
    WINDOW_DIBITS = 700,
    LEAD_DIBITS = 300,
    TAIL_DIBITS = 4000,
    MAX_WINDOWS = 16,
    STREAM_CAP = LEAD_DIBITS + (MAX_WINDOWS * (SYNC_DIBITS + WINDOW_DIBITS)) + 64,
};

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* The stream the fake radio delivers, as dibits; past its end, pseudo-random noise. */
static int g_stream[STREAM_CAP];
static size_t g_stream_len;
static size_t g_read_index;
static uint32_t g_noise_seed;
/* What the fake radio adds to every symbol level: a DC offset. */
static float g_level_offset;
/* The CQPSK dibit map the dibits pushed from now on come through under (the receiver reads them back under it). */
static uint8_t g_push_map;

/* The MAC PDUs that reached the VPDU decoder. */
static int g_vpdu_calls;

void
process_MAC_VPDU_crc(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                     unsigned long long int mac[24], int crc_ok) {
    (void)opts;
    (void)state;
    (void)type;
    (void)pdu_type;
    (void)mac;
    (void)crc_ok;
    g_vpdu_calls++;
}

void
process_MAC_VPDU(dsd_opts* opts, dsd_state* state, int type, p25_mac_pdu_type pdu_type,
                 unsigned long long int mac[24]) {
    process_MAC_VPDU_crc(opts, state, type, pdu_type, mac, 1);
}

static int
next_random_dibit(uint32_t* seed) {
    *seed = (*seed * 1103515245U) + 12345U;
    return (int)((*seed >> 16) & 3U);
}

static float
level_for_dibit(int dibit) {
    switch (dibit & 3) {
        case 0: return 1.0f;
        case 1: return 3.0f;
        case 2: return -1.0f;
        default: return -3.0f;
    }
}

static void
push_dibit(int dibit) {
    if (g_stream_len < STREAM_CAP) {
        g_stream[g_stream_len++] = dsd_p25_cqpsk_raw_dibit_for_corrected(g_push_map, (uint8_t)dibit);
    }
}

/* 40 ISCH bits (bit 39 first) as 20 dibits, the low bit of the dibits at @p bit_errors positions flipped. */
static void
push_isch(uint64_t word, int bit_errors) {
    static const int positions[] = {1, 5, 9, 13, 17, 3, 7, 11, 15, 19};
    int dibits[SYNC_DIBITS];
    for (int d = 0; d < SYNC_DIBITS; d++) {
        dibits[d] = (int)((word >> (38 - (2 * d))) & 3U);
    }
    for (int i = 0; i < bit_errors; i++) {
        dibits[positions[i]] ^= 1;
    }
    for (int d = 0; d < SYNC_DIBITS; d++) {
        push_dibit(dibits[d]);
    }
}

/* The window at superframe timeslot @p t0 after its sync, the sync @p sync_errors bits off and the S-ISCH inside the
   window (after timeslot 0, t0 being 2 mod 4) @p inner_errors bits off: four scrambled FACCH MAC_IDLEs. */
static void
push_window(int t0, int sync_errors, int inner_errors, int salt) {
    p25p2_bb_slot slots[4];
    for (int ts = 0; ts < 4; ts++) {
        DSD_MEMSET(&slots[ts], 0, sizeof(slots[ts]));
        slots[ts].duid = DUID_FACCH_S;
        slots[ts].fill_crc = 1;
        slots[ts].mac[0] = (uint8_t)(MAC_IDLE << 5);
        for (int i = 1; i < 18; i++) {
            slots[ts].mac[i] = (uint8_t)((salt * 41) + (ts * 23) + (i * 11));
        }
    }
    uint8_t bits[1440];
    p25p2_bb_window(TEST_CODE, t0, slots, bits);
    push_isch(0x575D57F7FFULL, sync_errors);
    for (size_t d = 0; d < WINDOW_DIBITS; d++) {
        push_dibit((bits[2U * d] << 1) | bits[(2U * d) + 1U]);
    }
    if (inner_errors > 0) {
        /* The S-ISCH after timeslot 0 occupies dibits 160-179 of the window. */
        static const int inner[] = {161, 165, 169, 173, 177, 163, 167};
        const size_t base = g_stream_len - WINDOW_DIBITS;
        for (int i = 0; i < inner_errors; i++) {
            g_stream[base + (size_t)inner[i]] ^= 1;
        }
    }
}

static void
reset_stream(void) {
    g_stream_len = 0;
    g_read_index = 0;
    g_noise_seed = 0xC0FFEEU;
    g_level_offset = 0.0f;
    g_push_map = DSD_P25_CQPSK_DIBIT_MAP_IDENTITY;
    g_vpdu_calls = 0;
    uint32_t lead = 0x1234567U;
    for (int i = 0; i < LEAD_DIBITS; i++) {
        push_dibit(next_random_dibit(&lead));
    }
}

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count == 0U) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const int dibit = g_read_index < g_stream_len ? g_stream[g_read_index] : next_random_dibit(&g_noise_seed);
        out[i] = level_for_dibit(dibit) + g_level_offset;
        g_read_index++;
    }
    *out_got = (int)count;
    return 0;
}

static double
fake_rtl_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return 1.0;
}

static int
fake_output_kind(void) {
    return RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
}

static int
fake_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    *out_symbol_rate_hz = 6000;
    *out_levels = 4;
    *out_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    return 0;
}

static uint32_t
fake_stream_generation(void) {
    return 1U;
}

static int
fake_stream_active(void) {
    return 1;
}

static int
fake_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    *out_cqpsk_enable = 1;
    *out_cqpsk_timing_active = 1;
    return 0;
}

/* A Phase 2 decoder on the RTL CQPSK path at 6000/4, the modulation locked (as -f2 -m2 runs it), the site's scrambling
   code set, fed g_stream. */
static void
start(dsd_opts* opts, dsd_state* state) {
    static int fake_rtl_context;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    if (!init_state_buffers(state)) {
        abort();
    }
    dsd_frame_sync_reset_mod_state();
    p25_p2_frame_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_p25p2 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_qpsk = 1;
    opts->msize = 1;
    opts->ssize = 36;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    state->rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->p2_wacn = TEST_CODE >> 24;
    state->p2_sysid = (TEST_CODE >> 12) & 0xFFFU;
    state->p2_cc = TEST_CODE & 0xFFFU;
    dsd_rtl_stream_io_hooks_set(&(dsd_rtl_stream_io_hooks){
        .read = fake_rtl_read,
        .return_pwr = fake_rtl_pwr,
    });
    dsd_rtl_stream_metrics_hooks metrics = {
        .output_kind = fake_output_kind,
        .symbol_profile = fake_symbol_profile,
        .stream_generation = fake_stream_generation,
        .cqpsk_status = fake_cqpsk_status,
        .stream_active = fake_stream_active,
    };
    dsd_rtl_stream_metrics_hooks_set(&metrics);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

/* Hunt and decode until the stream and @p tail dibits of noise after it have gone by. */
static void
run(dsd_opts* opts, dsd_state* state, size_t tail) {
    while (state->symbol_clock < (uint64_t)(g_stream_len + tail)) {
        const int sync = getFrameSync(opts, state);
        if (DSD_SYNC_IS_P25P2(sync)) {
            state->synctype = sync;
            state->sps_hunt_last_frame_verdict = (int)dsd_dispatch_handle_p25p2(opts, state);
        }
    }
}

static void
stop(dsd_state* state) {
    dsd_rtl_stream_io_hooks_set(NULL);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    /* The scrambling code recovery's context, which the first window allocated. */
    dsd_state_ext_free_all(state);
    free_state_buffers(state);
}

/* Fifteen windows of a continuous carrier, at superframe timeslots 2, 6 and 10 in turn. Five of their syncs come through
   1 to 5 bits off, each with the S-ISCH inside its window hit as well (a fade over the window's start), so the exact
   hunt finds nothing of those windows: 40 MAC PDUs of 60. Now the four within the tolerance follow a placed window and
   decode (56); the one 5 bits off is lost as before. Nothing decodes from the noise after the carrier. */
static void
corrupted_syncs_decode(float offset) {
    static dsd_opts opts;
    static dsd_state state;
    static const int sync_errors[15] = {0, 0, 2, 0, 4, 1, 0, 3, 0, 5, 0, 0, 0, 0, 0};
    reset_stream();
    g_level_offset = offset;
    for (int w = 0; w < 15; w++) {
        push_window((2 + (4 * w)) % 12, sync_errors[w], sync_errors[w] > 0 ? 3 : 0, w);
    }
    start(&opts, &state);
    run(&opts, &state, TAIL_DIBITS);
    CHECK(g_vpdu_calls == 56);
    stop(&state);
}

static void
test_corrupted_syncs_decode(void) {
    corrupted_syncs_decode(0.0f);
    corrupted_syncs_decode(0.35f);
    corrupted_syncs_decode(-0.35f);
}

/* A clean carrier decodes every window, exactly as before; a carrier whose every sync after the first is 2 bits off
   decodes every window too, each anchored by the one before. */
static void
test_clean_and_all_tolerant(void) {
    static dsd_opts opts;
    static dsd_state state;
    for (int all_off = 0; all_off <= 1; all_off++) {
        reset_stream();
        for (int w = 0; w < 12; w++) {
            push_window((2 + (4 * w)) % 12, (all_off && w > 0) ? 2 : 0, 0, w);
        }
        start(&opts, &state);
        run(&opts, &state, TAIL_DIBITS);
        CHECK(g_vpdu_calls == 48);
        stop(&state);
    }
}

/* One or two symbols inserted after a placed window move the next sync off the expected position, with the real
   symbols around it. An exact word decodes where it is; a word 2 bits off (the S-ISCH inside its window hit too) is
   lost, as when matched exactly, and nothing decodes in its place: the window at the expected position is the word
   shifted, which those errors do not bring within the tolerance. */
static void
test_slips(void) {
    static dsd_opts opts;
    static dsd_state state;
    for (int slip = 1; slip <= 2; slip++) {
        for (int corrupted = 0; corrupted <= 1; corrupted++) {
            reset_stream();
            push_window(2, 0, 0, 0);
            uint32_t seed = 0x5150U + (uint32_t)slip;
            for (int i = 0; i < slip; i++) {
                push_dibit(next_random_dibit(&seed));
            }
            push_window(6, corrupted ? 2 : 0, corrupted ? 3 : 0, 1);
            push_window(10, 0, 0, 2);
            push_window(2, 0, 0, 3);
            start(&opts, &state);
            run(&opts, &state, TAIL_DIBITS);
            CHECK(g_vpdu_calls == (corrupted ? 12 : 16));
            stop(&state);
        }
    }
}

/* After a placed window, 11 symbols slip in that read as the word's own first 11, and from there the carrier's dibits
   come through a quarter turn round (the P1200 map). The window at the expected position is then 3 bits off the word
   under the anchor's map. Taken, it opened a window that failed its verdict, and the exact rotated word 11 symbols on,
   which the exact hunt matches, went with it: 12 MAC PDUs of 16. A word a few bits off is not taken where it ends in
   the head of an exact word of any map or polarity that a slip within the no-steal span would put there: every window
   decodes, as hunting exactly does. */
static void
test_slip_into_rotated_map(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char slipped[] = "11131131111";
    reset_stream();
    push_window(2, 0, 0, 0);
    for (size_t i = 0; slipped[i] != '\0'; i++) {
        push_dibit(slipped[i] - '0');
    }
    g_push_map = DSD_P25_CQPSK_DIBIT_MAP_P1200;
    push_window(6, 0, 0, 1);
    push_window(10, 0, 0, 2);
    push_window(2, 0, 0, 3);
    start(&opts, &state);
    run(&opts, &state, TAIL_DIBITS);
    CHECK(g_vpdu_calls == 16);
    CHECK(state.p25_p2_sync_anchor.tolerant_syncs == 0U);
    CHECK(state.p25_cqpsk_dibit_map_idx == DSD_P25_CQPSK_DIBIT_MAP_P1200);
    stop(&state);
}

int
main(void) {
    test_corrupted_syncs_decode();
    test_clean_and_all_tolerant();
    test_slips();
    test_slip_into_rotated_map();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "P25 P2 tolerant sync: OK\n");
    return 0;
}
