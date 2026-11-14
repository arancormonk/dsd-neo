// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A P25 Phase 1 CQPSK carrier whose sync words come through with a few symbols off, as on a simulcast site: the hunt
 * matched them exactly, so each such word lost its whole frame. After a validated frame the hunt now accepts a word
 * within six bit errors where that frame ends, and the frame behind it decodes when its NID passes the strict check.
 *
 * The stream is real-shaped and synthetic: data units laid end to end as on a continuous outbound carrier (frame sync,
 * NID with its status symbol, payload), at the CQPSK demodulator's symbol levels. It runs through the real frame sync,
 * symbol reader and dibit slicer on the RTL CQPSK path, the real Phase 1 dispatcher and the real BCH/soft NID decoder.
 * The frame handlers past the NID are stand-ins that read the rest of their data unit and count it.
 *
 * C++ for the NID codewords, which come from the test-only generator header.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <vector>
#include "p25_nid_generator.hpp"
extern "C" {
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_cqpsk_dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <mbelib-neo/mbelib.h>
#include "frame_sync_state_buffers.h"
}

extern "C" dsd_frame_verdict dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

enum {
    DUID_HDU = 0x0,
    DUID_TDU = 0x3,
    DUID_LDU1 = 0x5,
    DUID_TSDU = 0x7,
    DUID_LDU2 = 0xA,
    SYNC_DIBITS = 24,
    NID_DIBITS = 33, /* 32 NID dibits with the status symbol after the 11th */
    NID_STATUS_DIBIT = 11,
    HEAD_DIBITS = SYNC_DIBITS + NID_DIBITS,
    HDU_DIBITS = 396,
    LDU_DIBITS = 864,
    TDU_DIBITS = 72,
    TSDU_1_DIBITS = 180,
    NAC = 0x1A6,
};

/* The stream the fake radio delivers, as dibits; past its end, pseudo-random noise. */
static std::vector<int> g_stream;
static size_t g_read_index;
static uint32_t g_noise_seed;
static uint32_t g_data_seed;
/* What the fake radio adds to every symbol level: a DC offset, and level noise uniform within +/- g_level_noise. */
static float g_level_offset;
static float g_level_noise;
static uint32_t g_level_seed;

/* Data units the dispatcher handed on past the NID, by kind. */
static int g_hdu;
static int g_ldu1;
static int g_ldu2;
static int g_tdu;
static int g_tsdu;
static int g_other;

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

/* The level the demodulator hands on for @p dibit: its ideal level, the DC offset and the level noise. */
static float
received_level(int dibit) {
    g_level_seed = (g_level_seed * 1103515245U) + 12345U;
    const float uniform = ((float)((g_level_seed >> 8) & 0xFFFFU) / 32767.5f) - 1.0f;
    return level_for_dibit(dibit) + g_level_offset + (g_level_noise * uniform);
}

static void
append_word(const char* word) {
    for (size_t i = 0; word[i] != '\0'; i++) {
        g_stream.push_back(word[i] - '0');
    }
}

/* The P25p1 sync word with the low bit of @p bits of its dibits flipped: one level off, one bit each. */
static void
append_sync(int bits) {
    static const int positions[] = {0, 4, 8, 12, 16, 20, 2, 6, 10};
    char word[SYNC_DIBITS + 1];
    DSD_MEMCPY(word, P25P1_SYNC, sizeof(word));
    for (int i = 0; i < bits; i++) {
        word[positions[i]] = (char)('0' + ((word[positions[i]] - '0') ^ 1));
    }
    append_word(word);
}

/* A NID as sent: the BCH codeword of @p nac and @p duid and the parity bit, as 33 dibits with the status symbol
   @p status after the 11th. */
static void
append_nid(int nac, int duid, int status) {
    char info[16];
    char bits[64];
    for (int i = 0; i < 12; i++) {
        info[i] = (char)((nac >> (11 - i)) & 1);
    }
    for (int i = 0; i < 4; i++) {
        info[12 + i] = (char)((duid >> (3 - i)) & 1);
    }
    p25_test_generate_nid_codeword(info, bits);
    bits[63] = (char)((duid == DUID_LDU1 || duid == DUID_LDU2) ? 1 : 0);
    int bit = 0;
    for (int d = 0; d < NID_DIBITS; d++) {
        if (d == NID_STATUS_DIBIT) {
            g_stream.push_back(status);
            continue;
        }
        g_stream.push_back((bits[bit] << 1) | bits[bit + 1]);
        bit += 2;
    }
}

static void
append_data(int count) {
    for (int i = 0; i < count; i++) {
        g_stream.push_back(next_random_dibit(&g_data_seed));
    }
}

/* One data unit of @p unit dibits naming @p nac and @p duid, its sync word @p sync_bits bits off. */
static void
append_unit(int duid, int unit, int sync_bits) {
    append_sync(sync_bits);
    append_nid(NAC, duid, 1);
    append_data(unit - HEAD_DIBITS);
}

static void
reset_stream(void) {
    g_stream.clear();
    g_read_index = 0;
    g_noise_seed = 0xC0FFEEU;
    g_data_seed = 0x1234567U;
    g_hdu = g_ldu1 = g_ldu2 = g_tdu = g_tsdu = g_other = 0;
    g_level_offset = 0.0f;
    g_level_noise = 0.0f;
    g_level_seed = 0xBADC0DEU;
}

extern "C" {

static int
fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    if (out == NULL || out_got == NULL || count == 0U) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        const int dibit = g_read_index < g_stream.size() ? g_stream[g_read_index] : next_random_dibit(&g_noise_seed);
        out[i] = received_level(dibit);
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
    *out_symbol_rate_hz = 4800;
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

} // extern "C"

/* A Phase 1 decoder on the RTL CQPSK path at 4800/4, the modulation locked (as -f1 -mq runs it), fed g_stream. */
static void
start(dsd_opts* opts, dsd_state* state) {
    static int fake_rtl_context;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    if (!init_state_buffers(state)) {
        abort();
    }
    dsd_frame_sync_reset_mod_state();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->frame_p25p1 = 1;
    opts->mod_cli_lock = 1;
    opts->mod_qpsk = 1;
    opts->msize = 1;
    opts->ssize = 36;
    state->rf_mod = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->rtl_ctx = (struct RtlSdrContext*)&fake_rtl_context;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    dsd_rtl_stream_io_hooks io = {};
    io.read = fake_rtl_read;
    io.return_pwr = fake_rtl_pwr;
    dsd_rtl_stream_io_hooks_set(&io);
    dsd_rtl_stream_metrics_hooks metrics = {};
    metrics.output_kind = fake_output_kind;
    metrics.symbol_profile = fake_symbol_profile;
    metrics.stream_generation = fake_stream_generation;
    metrics.cqpsk_status = fake_cqpsk_status;
    metrics.stream_active = fake_stream_active;
    dsd_rtl_stream_metrics_hooks_set(&metrics);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

/* Hunt and decode until the stream and @p tail dibits of noise after it have gone by. */
static void
run(dsd_opts* opts, dsd_state* state, size_t tail) {
    while (state->symbol_clock < (uint64_t)(g_stream.size() + tail)) {
        const int sync = getFrameSync(opts, state);
        if (DSD_SYNC_IS_P25P1(sync)) {
            state->synctype = sync;
            state->sps_hunt_last_frame_verdict = (int)dsd_dispatch_handle_p25p1(opts, state);
        }
    }
}

static void
stop(dsd_state* state) {
    dsd_rtl_stream_io_hooks_set(nullptr);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    free_state_buffers(state);
}

/* A voice transmission on a continuous carrier: a header, twelve LDUs, a terminator, then noise. Five LDUs' sync words
   are 1 to 6 bits off (two of them back to back), one 7. Matched exactly, each of those six is lost; now the five
   within the tolerance decode, each behind a validated frame, and the one past it is still lost. Nothing decodes from
   the noise after the terminator. The levels come through with @p offset added and noise within +/- @p noise: short
   of a slicer threshold (1.0 from each level) neither may change what decodes, though the decoder's level window
   (use_symbol()) follows the offset and the hunt's does not (the RTL CQPSK path puts back its fixed levels every
   symbol). */
static void
corrupted_syncs_decode(float offset, float noise) {
    static dsd_opts opts;
    static dsd_state state;
    reset_stream();
    g_level_offset = offset;
    g_level_noise = noise;
    append_unit(DUID_HDU, HDU_DIBITS, 0);
    static const int ldu_sync_bits[12] = {0, 2, 0, 6, 0, 1, 3, 0, 7, 0, 5, 0};
    for (int i = 0; i < 12; i++) {
        append_unit((i & 1) ? DUID_LDU2 : DUID_LDU1, LDU_DIBITS, ldu_sync_bits[i]);
    }
    append_unit(DUID_TDU, TDU_DIBITS, 0);
    start(&opts, &state);
    run(&opts, &state, 4000);
    CHECK(g_hdu == 1);
    CHECK(g_ldu1 + g_ldu2 == 11);
    CHECK(g_ldu1 == 5 && g_ldu2 == 6); /* the fifth LDU1, 7 bits off, is the one lost */
    CHECK(g_tdu == 1);
    CHECK(g_tsdu == 0 && g_other == 0);
    CHECK(state.nac == NAC);
    stop(&state);
}

static void
test_corrupted_syncs_decode(void) {
    corrupted_syncs_decode(0.0f, 0.0f);
    corrupted_syncs_decode(0.35f, 0.0f);
    corrupted_syncs_decode(-0.35f, 0.0f);
    corrupted_syncs_decode(0.3f, 0.45f);
    corrupted_syncs_decode(-0.3f, 0.45f);
}

/* The 33 dibits the dispatcher reads as a NID from @p at, as BCH bits and the parity bit (the status symbol skipped). */
static struct p25p1_nid_result
decode_nid_read_at(size_t at, int observed_nac) {
    char bch[63];
    uint8_t rel[63];
    unsigned char parity = 0;
    int bit = 0;
    for (int d = 0; d < NID_DIBITS; d++) {
        if (d == NID_STATUS_DIBIT) {
            continue;
        }
        const int dibit = g_stream[at + (size_t)d];
        if (bit < 62) {
            bch[bit] = (char)((dibit >> 1) & 1);
            bch[bit + 1] = (char)(dibit & 1);
        } else {
            bch[62] = (char)((dibit >> 1) & 1);
            parity = (unsigned char)(dibit & 1);
        }
        bit += 2;
    }
    DSD_MEMSET(rel, 200, sizeof(rel));
    return p25p1_nid_decode(bch, rel, observed_nac, parity, 200);
}

/* Round 3 review #1: a symbol slips into a NAC 555 carrier after a header, and the TSDU's sync word behind it came
   through 3 bits off. The window one symbol early, at the expected position, is 6 bits off and is accepted; the NID it
   reads is one symbol off, and the BCH takes that word for an LDU1 of NAC 555 with 2 corrections and the parity bit
   disagreeing. The strict check drops it: no LDU1 decodes out of the TSDU (whose 864 dibits would also swallow the
   header and the LDU2 behind it), the hunt resumes, and the header and LDU2 that follow decode. (Matched exactly, the
   TSDU is lost too.) */
static void
test_slipped_word_misaligned_nid_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char received[] = "111131133311333313133333";
    reset_stream();
    /* NAC 555 for the whole carrier. */
    append_sync(0);
    append_nid(0x555, DUID_HDU, 1);
    append_data(HDU_DIBITS - HEAD_DIBITS);
    const size_t expected = g_stream.size();
    g_stream.push_back(1); /* the slipped-in symbol */
    append_word(received);
    append_nid(0x555, DUID_TSDU, 1);
    append_data(TSDU_1_DIBITS - HEAD_DIBITS);
    append_sync(0);
    append_nid(0x555, DUID_HDU, 1);
    append_data(HDU_DIBITS - HEAD_DIBITS);
    append_sync(0);
    append_nid(0x555, DUID_LDU2, 1);
    append_data(LDU_DIBITS - HEAD_DIBITS);

    /* What the BCH makes of the NID read one symbol early. */
    const struct p25p1_nid_result misread = decode_nid_read_at(expected + SYNC_DIBITS, 0x555);
    CHECK(misread.status == NID_PARITY_OVERRIDE);
    CHECK(misread.nac == 0x555 && misread.duid == DUID_LDU1 && misread.error_count == 2 && misread.nac_borrowed == 0);

    start(&opts, &state);
    run(&opts, &state, 4000);
    CHECK(g_hdu == 2);
    CHECK(g_ldu2 == 1);
    CHECK(g_ldu1 == 0 && g_tsdu == 0 && g_tdu == 0 && g_other == 0);
    stop(&state);
}

/* One or two symbols inserted after a validated LDU move the next word off the expected position, with the real
   symbols around it. An exact word decodes where it is; a word 2 bits off is lost, as when matched exactly, and nothing
   else decodes in its place. (A lost symbol puts the word before the expected position, where an LDU's decoder, which
   reads its whole 864 dibits, has already read into it: lost either way. FRAME_SYNC_P25_ANCHOR hunts those windows.) */
static void
test_slips(void) {
    static const int slips[] = {1, 2};
    for (size_t s = 0; s < sizeof(slips) / sizeof(slips[0]); s++) {
        for (int corrupted = 0; corrupted <= 1; corrupted++) {
            static dsd_opts opts;
            static dsd_state state;
            reset_stream();
            g_data_seed += (uint32_t)(s * 2U + (size_t)corrupted);
            append_unit(DUID_LDU1, LDU_DIBITS, 0);
            append_data(slips[s]);
            append_unit(DUID_LDU2, LDU_DIBITS, corrupted ? 2 : 0);
            append_unit(DUID_LDU1, LDU_DIBITS, 0);
            start(&opts, &state);
            run(&opts, &state, 4000);
            CHECK(g_ldu1 == 2);
            CHECK(g_ldu2 == (corrupted ? 0 : 1));
            CHECK(g_hdu == 0 && g_tdu == 0 && g_tsdu == 0 && g_other == 0);
            stop(&state);
        }
    }
}

/* After a validated LDU, 12 symbols slip in that read as the word's own first 12, and from there the carrier's dibits
   come through a quarter turn round (the P1200 map). The window at the expected position is then 5 bits off the word
   under the anchor's map. Taken, its NID failed the strict check and the hunt resumed past it, and the exact rotated
   word 12 symbols on, which the exact hunt matches, went with it: the LDU2 behind it was lost. A word a few bits off is
   not taken where it ends in the head of an exact word of any map or polarity that a slip within the no-steal span
   would put there: every LDU decodes, as hunting exactly does. */
static void
test_slip_into_rotated_map(void) {
    static dsd_opts opts;
    static dsd_state state;
    reset_stream();
    append_unit(DUID_LDU1, LDU_DIBITS, 0);
    for (int i = 0; i < 12; i++) {
        g_stream.push_back(P25P1_SYNC[i] - '0');
    }
    const size_t rotated_from = g_stream.size();
    append_unit(DUID_LDU2, LDU_DIBITS, 0);
    append_unit(DUID_LDU1, LDU_DIBITS, 0);
    for (size_t i = rotated_from; i < g_stream.size(); i++) {
        g_stream[i] = dsd_p25_cqpsk_raw_dibit_for_corrected(DSD_P25_CQPSK_DIBIT_MAP_P1200, (uint8_t)g_stream[i]);
    }
    start(&opts, &state);
    run(&opts, &state, 4000);
    CHECK(g_ldu1 == 2);
    CHECK(g_ldu2 == 1);
    CHECK(g_hdu == 0 && g_tdu == 0 && g_tsdu == 0 && g_other == 0);
    CHECK(state.p25_p1_sync_anchor.tolerant_syncs == 0U);
    stop(&state);
}

int
main(void) {
    test_corrupted_syncs_decode();
    test_slipped_word_misaligned_nid_dropped();
    test_slips();
    test_slip_into_rotated_map();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "P25 P1 tolerant sync: OK\n");
    return 0;
}

/* Stand-ins for what the dispatcher calls past the NID: each reads the rest of its data unit off the stream. */
extern "C" {

static void
read_rest(dsd_opts* opts, dsd_state* state, int unit) {
    for (int i = HEAD_DIBITS; i < unit; i++) {
        (void)getDibitSoft(opts, state, NULL);
    }
}

void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    (void)opts;
    (void)state;
    (void)slot_mask;
    (void)synctype;
}

void
resumeScan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
}

void
p25_status_accum_reset(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
}

void
p25_status_accum_classify(dsd_state* state) {
    (void)state;
}

void
processHDU(dsd_opts* opts, dsd_state* state) {
    read_rest(opts, state, HDU_DIBITS);
    g_hdu++;
}

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    read_rest(opts, state, LDU_DIBITS);
    g_ldu1++;
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    read_rest(opts, state, LDU_DIBITS);
    g_ldu2++;
}

void
processTDU(dsd_opts* opts, dsd_state* state) {
    read_rest(opts, state, TDU_DIBITS);
    g_tdu++;
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other++;
}

int
processTSBK(dsd_opts* opts, dsd_state* state) {
    /* A one-block TSDU's decoder stops at the block's end, before the padding. */
    read_rest(opts, state, 158);
    g_tsdu++;
    return 1;
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_other++;
}

} // extern "C"
