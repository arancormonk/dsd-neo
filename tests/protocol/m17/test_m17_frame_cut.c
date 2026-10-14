// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * M17 frames an input interruption cut or a carrier boundary split.
 *
 * A Signal search that leaves a frame (its visit ended, or a command came), or a live read that gives up on a stalled
 * input, sets dsd_state::input_interrupted, and getSymbol() stands 0 in for that symbol and every one after it until
 * the engine loop consumes the flag. The frames here are built with the M17 encoder's own helpers and read by the real
 * LSF, stream, packet and BERT readers through GNU ld --wrap seams around the two symbol readers, so the Golay, Viterbi
 * and CRC decisions are the decoder's own: stood-in symbols can pass all of them.
 */

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/protocol/m17/m17_parse.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "m17_algorithms.h"
#include "m17_internal.h"

struct CODEC2;

int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
int __wrap_getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol);
#ifdef USE_CODEC2
void codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits);
void codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]);
int codec2_samples_per_frame(struct CODEC2* codec2_state);
#endif
void LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state);
int Connect(char* hostname, int portno);

enum {
    TEST_M17_LSF_BITS = M17_LSF_BYTES * 8U,
    /* getSymbol()'s stood-in 0 against the thresholds prepare() sets: not above the centre, not under the lower mid. */
    TEST_STOOD_IN_DIBIT = 2,
};

/* The frame body the readers are handed, after its sync word. */
static uint8_t g_frame[M17_PAYLOAD_SYMBOLS];
static size_t g_pos;
/* The read a Signal search's yield (or a stalled live read) interrupts; SIZE_MAX for none. */
static size_t g_interrupt_at = SIZE_MAX;
/* The read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); SIZE_MAX for none. The frame's own dibits follow it, the worst case of
   another carrier's symbols that pass the frame's checks. */
static size_t g_boundary_at = SIZE_MAX;
static int g_codec2_decodes;
static int g_search_evidence;

#ifdef USE_CODEC2
void
codec2_decode(struct CODEC2* codec2_state, short speech[], const unsigned char* bits) {
    (void)codec2_state;
    (void)bits;
    g_codec2_decodes++;
    if (speech != NULL) {
        DSD_MEMSET(speech, 0, 160U * sizeof(short));
    }
}

void
codec2_encode(struct CODEC2* codec2_state, unsigned char* bits, short speech[]) {
    (void)codec2_state;
    (void)bits;
    (void)speech;
}

int
codec2_samples_per_frame(struct CODEC2* codec2_state) {
    (void)codec2_state;
    return 160;
}
#endif

void
LFSRN(const char* BufferIn, char* BufferOut, dsd_state* state) {
    (void)BufferIn;
    (void)BufferOut;
    (void)state;
}

int
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return -1;
}

static int
next_dibit(dsd_state* state, float* symbol) {
    assert(g_pos < M17_PAYLOAD_SYMBOLS);
    if (g_pos == g_boundary_at) {
        state->carrier_seq++;
    }
    if (g_pos == g_interrupt_at) {
        state->input_interrupted = 1;
    }
    int dibit = g_frame[g_pos];
    *symbol = (float)m17_symbol_from_dibit(g_frame[g_pos]);
    if (state->input_interrupted) {
        dibit = TEST_STOOD_IN_DIBIT;
        *symbol = 0.0F;
    }
    g_pos++;
    return dibit;
}

int
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    float symbol = 0.0F;
    const int dibit = next_dibit(state, &symbol);
    if (out_analog_signal != NULL) {
        *out_analog_signal = (int)symbol;
    }
    return dibit;
}

int
__wrap_getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol) {
    (void)opts;
    float symbol = 0.0F;
    const int dibit = next_dibit(state, &symbol);
    if (out_soft_symbol != NULL) {
        *out_soft_symbol = symbol;
    }
    return dibit;
}

static void
count_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                      const char* access) {
    (void)state;
    (void)target;
    (void)source;
    (void)access;
    assert(protocol != NULL && strcmp(protocol, "M17") == 0);
    g_search_evidence++;
}

static void
write_bits(uint8_t* bits, unsigned long long value, size_t n) {
    for (size_t i = 0U; i < n; i++) {
        bits[i] = (uint8_t)((value >> (n - 1U - i)) & 1ULL);
    }
}

/* A voice stream's link setup (3200 bit/s, clear, CAN 5) from EF2GH to AB1CD, its CRC attached. */
static void
build_lsf_bits(uint8_t lsf_bits[TEST_M17_LSF_BITS]) {
    uint8_t packed[M17_LSF_BYTES];
    DSD_MEMSET(lsf_bits, 0, TEST_M17_LSF_BITS);
    m17_load_lsf_callsigns(lsf_bits, m17_encode_b40_callsign(0ULL, "AB1CD"), m17_encode_b40_callsign(0ULL, "EF2GH"));
    write_bits(lsf_bits + 96, m17_compose_frame_info(1U, 2U, 0U, 0U, 5U, 0U, 0U), 16U);
    (void)m17_attach_lsf_crc(lsf_bits, packed);
}

/* Hands the readers the frame next, read whole unless a test then sets a cut or a boundary. */
static void
load_randomized_frame(const uint8_t rnd[M17_PAYLOAD_BITS]) {
    for (size_t i = 0U; i < M17_PAYLOAD_SYMBOLS; i++) {
        g_frame[i] = (uint8_t)(((rnd[i * 2U] & 1U) << 1U) | (rnd[(i * 2U) + 1U] & 1U));
    }
    g_pos = 0U;
    g_interrupt_at = SIZE_MAX;
    g_boundary_at = SIZE_MAX;
}

static void
load_lsf_frame(const uint8_t lsf_bits[TEST_M17_LSF_BITS]) {
    uint8_t type1[M17_LSF_TYPE1_FLUSH_BITS];
    uint8_t rnd[M17_PAYLOAD_BITS];
    DSD_MEMSET(type1, 0, sizeof(type1));
    DSD_MEMCPY(type1, lsf_bits, TEST_M17_LSF_BITS);
    (void)m17_lsf_encode_type1_bits(type1, rnd, NULL);
    load_randomized_frame(rnd);
}

/* Stream frame @p fn (its end flag included) carrying LICH chunk @p lich_cnt of @p lsf_bits. */
static void
load_stream_frame(const uint8_t lsf_bits[TEST_M17_LSF_BITS], uint8_t lich_cnt, uint16_t fn) {
    uint8_t lich_content[M17_LICH_CONTENT_BITS];
    uint8_t encoded_bits[M17_LICH_BITS];
    uint8_t payload[M17_STREAM_PAYLOAD_BITS];
    uint8_t type1[M17_STREAM_TYPE1_FLUSH_BITS];
    uint8_t punctured[M17_STREAM_PUNCTURED_BITS];
    uint8_t combined[M17_PAYLOAD_BITS];
    uint8_t rnd[M17_PAYLOAD_BITS];
    assert(m17_lich_build_content(lsf_bits, lich_cnt, lich_content) == 0);
    m17_lich_encode_bits(lich_content, encoded_bits);
    for (size_t i = 0U; i < M17_STREAM_PAYLOAD_BITS; i++) {
        payload[i] = (uint8_t)((((i * 7U) + 3U) % 5U) == 0U);
    }
    m17_stream_build_type1_bits(fn, payload, type1);
    m17_stream_encode_type1_bits(type1, punctured);
    m17_stream_combine_frame_bits(encoded_bits, punctured, combined);
    m17_payload_encode_bits(combined, rnd);
    load_randomized_frame(rnd);
}

/* Packet frame @p index of the SMS @p text; the last carries the end-of-frame flag. Returns the frame count. */
static int
load_packet_frame(const char* text, int index) {
    static uint8_t packed[M17_PACKET_MAX_TOTAL_BYTES];
    static uint8_t full_bits[M17_PACKET_MAX_FRAMES * M17_PACKET_CHUNK_BITS];
    uint16_t app_len = 0U;
    int frame_count = 0;
    uint8_t last_frame_bytes = 0U;
    uint16_t crc = 0U;
    assert(m17_packet_prepare_sms_payload(text, packed, full_bits, &app_len, &frame_count, &last_frame_bytes, &crc)
           == 0);
    assert(index >= 0 && index < frame_count);
    const int last = index == frame_count - 1;
    uint8_t metadata = 0U;
    assert(m17_packet_metadata_byte(last ? 1U : 0U, last ? last_frame_bytes : (uint8_t)index, &metadata) == 0);
    uint8_t type1[M17_PACKET_TYPE1_FLUSH_BITS];
    uint8_t rnd[M17_PAYLOAD_BITS];
    m17_packet_build_type1_bits(&full_bits[(size_t)index * M17_PACKET_CHUNK_BITS], metadata, type1);
    (void)m17_packet_encode_type1_bits(type1, rnd, NULL);
    load_randomized_frame(rnd);
    return frame_count;
}

/* A BERT frame: the next 197 bits of the PRBS9 sequence @p lfsr steps. */
static void
load_bert_frame(uint16_t* lfsr) {
    uint8_t type1[M17_BERT_TYPE1_FLUSH_BITS];
    uint8_t rnd[M17_PAYLOAD_BITS];
    DSD_MEMSET(type1, 0, sizeof(type1));
    m17_prbs9_fill_bits(lfsr, type1, M17_BERT_PAYLOAD_BITS);
    (void)m17_bert_encode_type1_bits(type1, rnd, NULL);
    load_randomized_frame(rnd);
}

/* A fresh decoder on symbols at +-1 and +-3, its thresholds centred on 0, the search's evidence counted. */
static void
prepare(dsd_opts* opts, dsd_state* state) {
    dsd_state_ext_free_all(state);
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->aggressive_framesync = 1;
    state->min = -4.0F;
    state->lmid = -2.0F;
    state->center = 0.0F;
    state->umid = 2.0F;
    state->max = 4.0F;
    state->m17_can_en = -1;
    state->m17_bert_lfsr = 1U;
    g_interrupt_at = SIZE_MAX;
    g_boundary_at = SIZE_MAX;
    g_codec2_decodes = 0;
    g_search_evidence = 0;
    dsd_signal_search_evidence_set(count_search_evidence);
}

static void
finish(dsd_state* state) {
    dsd_signal_search_evidence_set(NULL);
    dsd_state_ext_free_all(state);
}

/* The stream frame number the decoder last stored, as the AES counter's low bytes. */
static unsigned
stored_frame_number(const dsd_state* state) {
    return ((unsigned)state->m17_meta[14] << 8U) | state->m17_meta[15];
}

static dsd_call_snapshot
m17_call(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    assert(dsd_call_state_get(state, 0U, &call) == 1);
    return call;
}

static int
has_call(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    return dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
}

/* A stream frame's LICH and payload are interleaved over the whole frame. Cut, the rest of it is stood-in erasures,
   which the LICH's Golay blocks can take for codewords and the Viterbi decode for a frame number with its end flag
   set: a frame read entirely of them ends the call as a terminator would, one cut mid-frame stores a frame number the
   transmission never sent, and one cut late still decodes, its voice included. A cut frame is dropped: no frame number,
   no LICH chunk, no voice, no evidence, and the call stays as it was. Decoding resumes with the next whole frame. */
static void
test_stream_frame_cut_by_an_input_interruption_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_lsf_bits(lsf_bits);
    prepare(&opts, &state);

    /* The transmission proves itself with its link setup, and its first stream frame plays. */
    state.synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    assert(processM17LSF(&opts, &state) == 1);
    assert(state.m17_confirmed == 1U);
    state.synctype = DSD_SYNC_M17_STR_POS;
    load_stream_frame(lsf_bits, 0U, 7U);
    assert(processM17STR(&opts, &state) == 1);
    assert(stored_frame_number(&state) == 7U);
    const dsd_call_snapshot before = m17_call(&state);
    assert(before.phase == DSD_CALL_PHASE_ACTIVE);
    assert(before.media_active == 1U);
    assert(strcmp(before.source_text, "EF2GH") == 0);
    const int decodes_before = g_codec2_decodes;
    const int evidence_before = g_search_evidence;
    uint8_t lich_chunks_before[sizeof(state.m17_lsf)];
    DSD_MEMCPY(lich_chunks_before, state.m17_lsf, sizeof(lich_chunks_before));

    static const size_t cuts[3] = {0U, 124U, 170U}; /* the whole frame; mid frame; late, the LICH still clears */
    for (size_t c = 0U; c < 3U; c++) {
        state.input_interrupted = 0;
        load_stream_frame(lsf_bits, 1U, 8U);
        g_interrupt_at = cuts[c];

        assert(processM17STR(&opts, &state) == 0);

        assert(g_pos == M17_PAYLOAD_SYMBOLS);
        assert(state.input_interrupted == 1);
        assert(state.carrier_seq == 0U);
        assert(stored_frame_number(&state) == 7U);
        const dsd_call_snapshot call = m17_call(&state);
        assert(call.phase == DSD_CALL_PHASE_ACTIVE);
        assert(call.epoch == before.epoch);
        assert(g_codec2_decodes == decodes_before);
        assert(g_search_evidence == evidence_before);
        assert(memcmp(state.m17_lsf, lich_chunks_before, sizeof(lich_chunks_before)) == 0);
        assert(state.m17_confirmed == 1U);
    }

    /* The engine loop consumes the interruption, and the next whole frames decode as before, the last ending the call
       as its terminator. */
    state.input_interrupted = 0;
    load_stream_frame(lsf_bits, 2U, 9U);
    assert(processM17STR(&opts, &state) == 1);
    assert(stored_frame_number(&state) == 9U);
    assert(m17_call(&state).epoch == before.epoch);
    load_stream_frame(lsf_bits, 3U, (uint16_t)(M17_STREAM_FRAME_END_MASK | 10U));
    assert(processM17STR(&opts, &state) == 1);
    assert(stored_frame_number(&state) == 10U);
    const dsd_call_snapshot ended = m17_call(&state);
    assert(ended.phase == DSD_CALL_PHASE_ENDED);
    assert(ended.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    finish(&state);
}

/* The LSF's soft Viterbi decode reads the stood-in erasures as symbols, and a link setup cut late in its frame still
   clears its CRC. A cut LSF publishes no call, confirms no transmission and reports no evidence to the search; a whole
   one does all three. */
static void
test_lsf_cut_by_an_input_interruption_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_lsf_bits(lsf_bits);

    static const size_t cuts[3] = {0U, 100U, 170U}; /* the whole frame; mid frame; late enough to clear the CRC */
    for (size_t c = 0U; c < 3U; c++) {
        prepare(&opts, &state);
        state.synctype = DSD_SYNC_M17_LSF_POS;
        load_lsf_frame(lsf_bits);
        g_interrupt_at = cuts[c];

        assert(processM17LSF(&opts, &state) == 0);

        assert(g_pos == M17_PAYLOAD_SYMBOLS);
        assert(state.input_interrupted == 1);
        assert(!has_call(&state));
        assert(state.m17_confirmed == 0U);
        assert(state.m17_confirm_weak_streak == 0U);
        assert(g_search_evidence == 0);
        assert(state.m17_str_dt == 0U);
        assert(state.m17_can == 0U);
        finish(&state);
    }

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    assert(processM17LSF(&opts, &state) == 1);
    assert(state.input_interrupted == 0);
    assert(has_call(&state));
    assert(strcmp(m17_call(&state).source_text, "EF2GH") == 0);
    assert(state.m17_confirmed == 1U);
    assert(g_search_evidence == 1);
    assert(state.m17_can == 5U);
    finish(&state);
}

/* A packet frame cut late in its frame still decodes its chunk and the reassembled packet's CRC. A cut frame adds no
   chunk, and a cut last frame neither decodes nor confirms the packet; the whole packet does. */
static void
test_packet_frame_cut_by_an_input_interruption_decodes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char text[] = "CUT FRAMES DECODE NOTHING";

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_PKT_POS;
    assert(load_packet_frame(text, 0) == 2);
    g_interrupt_at = 170U;
    assert(processM17PKT(&opts, &state) == 0);
    assert(g_pos == M17_PAYLOAD_SYMBOLS);
    assert(state.input_interrupted == 1);
    assert(state.m17_pbc_ct == 0U);
    finish(&state);

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_PKT_POS;
    (void)load_packet_frame(text, 0);
    assert(processM17PKT(&opts, &state) == 1);
    assert(state.m17_pbc_ct == 1U);
    uint8_t chunks_before[sizeof(state.m17_pkt)];
    DSD_MEMCPY(chunks_before, state.m17_pkt, sizeof(chunks_before));
    (void)load_packet_frame(text, 1);
    g_interrupt_at = 170U;
    assert(processM17PKT(&opts, &state) == 0);
    assert(state.input_interrupted == 1);
    assert(state.m17_confirmed == 0U);
    assert(g_search_evidence == 0);
    assert(state.m17_pbc_ct == 1U);
    assert(memcmp(state.m17_pkt, chunks_before, sizeof(chunks_before)) == 0);

    /* The cut frame left the gathered chunk as a sync loss would: the same last frame, whole, completes the packet, its
       CRC the proof. */
    state.input_interrupted = 0;
    (void)load_packet_frame(text, 1);
    assert(processM17PKT(&opts, &state) == 1);
    assert(state.m17_confirmed == 1U);
    assert(g_search_evidence == 1);
    assert(state.m17_pbc_ct == 0U);
    finish(&state);
}

/* A BERT frame cut late in its frame still decodes enough of the PRBS9 sequence to lock. A cut frame counts no bits,
   no errors and no lock, and proves nothing; a whole one locks and proves the transmission. */
static void
test_bert_frame_cut_by_an_input_interruption_counts_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint16_t lfsr = 1U;

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_BRT_POS;
    load_bert_frame(&lfsr);
    g_interrupt_at = 170U;
    assert(processM17BRT(&opts, &state) == 0);
    assert(g_pos == M17_PAYLOAD_SYMBOLS);
    assert(state.input_interrupted == 1);
    assert(state.m17_bert_bits == 0U);
    assert(state.m17_bert_errors == 0U);
    assert(state.m17_bert_locked == 0U);
    assert(state.m17_bert_lfsr == 1U);
    assert(state.m17_confirmed == 0U);
    assert(g_search_evidence == 0);
    finish(&state);

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_BRT_POS;
    lfsr = 1U;
    load_bert_frame(&lfsr);
    assert(processM17BRT(&opts, &state) == 1);
    assert(state.m17_bert_locked == 1U);
    assert(state.m17_bert_bits > 0U);
    assert(state.m17_confirmed == 1U);
    assert(g_search_evidence == 1);
    finish(&state);
}

/* A carrier boundary inside a frame (issue #575) drops it the same way: the rest of it is the next carrier's, and a
   frame that straddles late decodes the carrier left's link setup or stream frame after the boundary ended its call. */
static void
test_frames_split_by_a_carrier_boundary_are_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_lsf_bits(lsf_bits);

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    g_boundary_at = 170U;
    assert(processM17LSF(&opts, &state) == 0);
    assert(state.carrier_seq == 1U);
    assert(state.input_interrupted == 0);
    assert(!has_call(&state));
    assert(state.m17_confirmed == 0U);
    assert(g_search_evidence == 0);
    finish(&state);

    prepare(&opts, &state);
    state.synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    assert(processM17LSF(&opts, &state) == 1);
    state.synctype = DSD_SYNC_M17_STR_POS;
    load_stream_frame(lsf_bits, 0U, 7U);
    assert(processM17STR(&opts, &state) == 1);
    const uint64_t epoch = m17_call(&state).epoch;
    load_stream_frame(lsf_bits, 1U, (uint16_t)(M17_STREAM_FRAME_END_MASK | 8U));
    g_boundary_at = 170U;
    assert(processM17STR(&opts, &state) == 0);
    assert(state.carrier_seq == 1U);
    assert(stored_frame_number(&state) == 7U);
    const dsd_call_snapshot call = m17_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch == epoch);
    finish(&state);
}

int
main(void) {
    InitAllFecFunction();
    test_stream_frame_cut_by_an_input_interruption_is_dropped();
    test_lsf_cut_by_an_input_interruption_publishes_nothing();
    test_packet_frame_cut_by_an_input_interruption_decodes_nothing();
    test_bert_frame_cut_by_an_input_interruption_counts_nothing();
    test_frames_split_by_a_carrier_boundary_are_dropped();
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
