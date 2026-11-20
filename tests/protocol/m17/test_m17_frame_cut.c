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
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "m17_algorithms.h"
#include "m17_internal.h"
#include "scan_activity_capture.h"
#include "test_support.h"

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
/* The read at which the input ends (SIZE_MAX for none): from it on the reader stands in silence, without raising an
   interruption. TEST_END_EXIT: a read that failed and asked for shutdown (an RTL read), which returns its stand-ins
   before counting them, so the frame counts only the symbols read before the end. TEST_END_FALLBACK: a symbol capture
   or PCM file that ended with the fallback to another input asked for, whose stand-ins getSymbol() counts as symbols,
   so the count says the frame was read whole. */
static size_t g_input_end_at = SIZE_MAX;

enum { TEST_END_EXIT = 0, TEST_END_FALLBACK = 1 };

static int g_input_end_mode;
/* The one decoder state and options the newer cases share, on the heap (main()). */
static dsd_opts* g_opts;
static dsd_state* g_state;
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
    const int ended = g_pos >= g_input_end_at;
    if (g_pos == g_input_end_at) {
        if (g_input_end_mode == TEST_END_EXIT) {
            dsd_exitflag_store(1);
        } else {
            state->input_fallback_pending = 1;
        }
    }
    int dibit = g_frame[g_pos];
    *symbol = (float)m17_symbol_from_dibit(g_frame[g_pos]);
    if (state->input_interrupted || ended) {
        dibit = TEST_STOOD_IN_DIBIT;
        *symbol = 0.0F;
    }
    g_pos++;
    /* Every symbol getSymbol() reads counts, an ended input's stand-in too, and neither an erasure an interruption
       stood in nor a failed read's stand-in does (the production reader returns those before counting): the packet's
       frame spacing is read from the count (issue #890). The frame's 8 sync symbols are the hunt's, so frames here end
       184 symbols apart. */
    if (!state->input_interrupted && !(ended && g_input_end_mode == TEST_END_EXIT)) {
        state->symbolcnt++;
    }
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
count_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                      uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
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

/* A link setup from @p src to @p dst with TYPE @p type, its CRC attached. */
static void
build_lsf_bits_with(uint8_t lsf_bits[TEST_M17_LSF_BITS], unsigned long long dst, unsigned long long src,
                    unsigned long long type) {
    uint8_t packed[M17_LSF_BYTES];
    DSD_MEMSET(lsf_bits, 0, TEST_M17_LSF_BITS);
    m17_load_lsf_callsigns(lsf_bits, dst, src);
    write_bits(lsf_bits + 96, type, 16U);
    (void)m17_attach_lsf_crc(lsf_bits, packed);
}

/* A packet transmission's link setup (packet mode, CAN 5) from EF2GH to AB1CD, its CRC attached: what the strict RF
   packet rule needs before any packet frame (issue #890). */
static void
build_packet_lsf_bits(uint8_t lsf_bits[TEST_M17_LSF_BITS]) {
    build_lsf_bits_with(lsf_bits, m17_encode_b40_callsign(0ULL, "AB1CD"), m17_encode_b40_callsign(0ULL, "EF2GH"),
                        m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U));
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
    g_input_end_at = SIZE_MAX;
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

/* A packet frame carrying @p bytes (@p n of them, the rest zero) with the metadata byte @p metadata as sent, valid or
   not. */
static void
load_packet_frame_metadata(const uint8_t* bytes, size_t n, uint8_t metadata) {
    uint8_t chunk_bits[M17_PACKET_CHUNK_BITS];
    DSD_MEMSET(chunk_bits, 0, sizeof(chunk_bits));
    for (size_t i = 0U; i < n * 8U && i < sizeof(chunk_bits); i++) {
        chunk_bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - (i % 8U))) & 1U);
    }
    uint8_t type1[M17_PACKET_TYPE1_FLUSH_BITS];
    uint8_t rnd[M17_PAYLOAD_BITS];
    m17_packet_build_type1_bits(chunk_bits, metadata, type1);
    (void)m17_packet_encode_type1_bits(type1, rnd, NULL);
    load_randomized_frame(rnd);
}

/* A packet frame carrying @p bytes (@p n of them, the rest zero) with metadata (@p eof, @p value). */
static void
load_raw_packet_frame(const uint8_t* bytes, size_t n, uint8_t eof, uint8_t value) {
    uint8_t metadata = 0U;
    assert(m17_packet_metadata_byte(eof, value, &metadata) == 0);
    load_packet_frame_metadata(bytes, n, metadata);
}

/* A packet transmission's checked LSF frame, read as the frame before its packet frames. */
static void
read_packet_lsf_frame(dsd_opts* opts, dsd_state* state) {
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_packet_lsf_bits(lsf_bits);
    state->synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    assert(processM17LSF(opts, state) == 1);
    state->synctype = DSD_SYNC_M17_PKT_POS;
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
    g_input_end_at = SIZE_MAX;
    dsd_exitflag_store(0);
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
   chunk, and a cut last frame neither decodes nor confirms the packet, nor does a frame after it (the cut one is lost);
   the whole packet does. */
static void
test_packet_frame_cut_by_an_input_interruption_decodes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char text[] = "CUT FRAMES DECODE NOTHING";

    prepare(&opts, &state);
    read_packet_lsf_frame(&opts, &state);
    assert(load_packet_frame(text, 0) == 2);
    g_interrupt_at = 170U;
    assert(processM17PKT(&opts, &state) == 0);
    assert(g_pos == M17_PAYLOAD_SYMBOLS);
    assert(state.input_interrupted == 1);
    assert(state.m17_pbc_ct == 0U);
    finish(&state);

    prepare(&opts, &state);
    read_packet_lsf_frame(&opts, &state);
    (void)load_packet_frame(text, 0);
    assert(processM17PKT(&opts, &state) == 1);
    assert(state.m17_pbc_ct == 1U);
    (void)load_packet_frame(text, 1);
    g_interrupt_at = 170U;
    const int evidence = g_search_evidence;
    assert(processM17PKT(&opts, &state) == 0);
    assert(state.input_interrupted == 1);
    assert(g_search_evidence == evidence);
    /* The cut frame drops the whole packet (the strict RF packet rule, issue #890). */
    assert(state.m17_pbc_ct == 0U);

    /* A last frame read whole after the cut one is no packet: it decodes and proves nothing, and still ends the
       transmission. */
    state.input_interrupted = 0;
    (void)load_packet_frame(text, 1);
    assert(processM17PKT(&opts, &state) == 0);
    assert(g_search_evidence == evidence);
    assert(state.m17_pbc_ct == 0U);
    assert(state.m17_tx_ended == 1U);
    finish(&state);

    /* The same packet with no frame lost completes, its CRC the proof. */
    prepare(&opts, &state);
    read_packet_lsf_frame(&opts, &state);
    const int lsf_evidence = g_search_evidence;
    (void)load_packet_frame(text, 0);
    assert(processM17PKT(&opts, &state) == 1);
    (void)load_packet_frame(text, 1);
    assert(processM17PKT(&opts, &state) == 1);
    assert(state.m17_confirmed == 1U);
    assert(g_search_evidence == lsf_evidence + 1);
    assert(state.m17_pbc_ct == 0U);
    finish(&state);
}

/* Issue #890, the strict RF packet rule: a middle frame cut four symbols in is lost, though the symbols counted from the
   frame before to the last frame (stood-in erasures count none) fall inside a frame's spacing: the packet is dropped
   and never printed, under -F too. Without its checked LSF frame a packet is not decoded at all. */
static void
test_packet_cut_early_is_lost(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    static const char text[] = "THIS SMS IS LONG ENOUGH TO TAKE THREE PACKET FRAMES TO SEND";
    char out[8192];

    prepare(opts, state);
    opts->aggressive_framesync = 0;
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "m17_pkt_cut_early") == 0);
    /* The checks run once the capture ends, so a failed one is seen. */
    read_packet_lsf_frame(opts, state);
    const int frames = load_packet_frame(text, 0);
    const int chunk0 = processM17PKT(opts, state);
    (void)load_packet_frame(text, 1);
    g_interrupt_at = 4U;
    (void)processM17PKT(opts, state);
    state->input_interrupted = 0;
    (void)load_packet_frame(text, 2);
    const int eof = processM17PKT(opts, state);
    (void)dsd_test_capture_stderr_end(&cap);
    (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    assert(frames == 3);
    assert(chunk0 == 1);
    assert(eof == 0);
    assert(strstr(out, "SMS") == NULL);
    finish(state);

    /* A whole packet with no LSF frame before it: not decoded. */
    prepare(opts, state);
    state->synctype = DSD_SYNC_M17_PKT_POS;
    (void)load_packet_frame(text, 0);
    assert(processM17PKT(opts, state) == 0);
    assert(state->m17_pbc_ct == 0U);
    finish(state);
}

/* Issue #890: the last frame (EOF) answers for itself through processM17PKT(): an empty packet (two bytes FF FF, the
   CRC of nothing) after a checked LSF frame earns no consumption credit, though the LSF's confirmation stands for the
   EOT marker; so does an EOF rejected for a lost frame before it. */
static void
test_packet_eof_verdict_is_its_own(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    static const uint8_t k_empty[2] = {0xFFU, 0xFFU};

    prepare(opts, state);
    read_packet_lsf_frame(opts, state);
    load_raw_packet_frame(k_empty, sizeof(k_empty), 1U, 2U);
    assert(processM17PKT(opts, state) == 0);
    assert(state->m17_confirmed == 1U);
    assert(state->m17_tx_ended == 1U);
    finish(state);

    static const char text[] = "CUT FRAMES DECODE NOTHING";
    prepare(opts, state);
    read_packet_lsf_frame(opts, state);
    (void)load_packet_frame(text, 0);
    assert(processM17PKT(opts, state) == 1);
    state->symbolcnt += 192U; /* a whole frame lost */
    (void)load_packet_frame(text, 1);
    assert(processM17PKT(opts, state) == 0);
    assert(state->m17_confirmed == 1U);
    finish(state);
}

/* Issue #890: every packet frame answers for itself, apart from the confirmation the transmission's EOT marker needs.
   After a checked LSF frame, an EOF whose byte count (26) no frame can carry, a first chunk numbered 10 and a whole
   packet whose payload CRC fails each earn no consumption credit; the LSF's confirmation stands. The malformed EOF is
   not trusted to end the transmission; the failed packet ends it, its CRC ERR logged, and only -F shows its content.
   The packet is an SMS "HI" whose CRC is wrong. */
static const uint8_t k_bad_crc_sms[6] = {0x05U, 'H', 'I', 0x00U, 0x12U, 0x34U};

static void
test_packet_malformed_eof_earns_no_credit(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;

    prepare(opts, state);
    read_packet_lsf_frame(opts, state);
    const int evidence = g_search_evidence;
    load_packet_frame_metadata(k_bad_crc_sms, sizeof(k_bad_crc_sms), (uint8_t)(0x80U | (26U << 2U)));
    assert(processM17PKT(opts, state) == 0);
    assert(state->m17_confirmed == 1U);
    assert(state->m17_tx_ended == 0U);
    assert(g_search_evidence == evidence);
    finish(state);
}

static void
test_packet_out_of_order_first_chunk_earns_no_credit(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;

    prepare(opts, state);
    read_packet_lsf_frame(opts, state);
    load_raw_packet_frame(k_bad_crc_sms, sizeof(k_bad_crc_sms), 0U, 10U);
    assert(processM17PKT(opts, state) == 0);
    assert(state->m17_confirmed == 1U);
    assert(state->m17_pbc_ct == 0U);
    finish(state);
}

static void
test_packet_crc_failure_earns_no_credit(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    char out[8192];
    for (int relaxed = 0; relaxed <= 1; relaxed++) {
        prepare(opts, state);
        opts->aggressive_framesync = relaxed ? 0 : 1;
        dsd_test_capture_stderr cap;
        assert(dsd_test_capture_stderr_begin(&cap, "m17_pkt_crc_fail") == 0);
        read_packet_lsf_frame(opts, state);
        const int lsf_evidence = g_search_evidence;
        load_raw_packet_frame(k_bad_crc_sms, sizeof(k_bad_crc_sms), 1U, (uint8_t)sizeof(k_bad_crc_sms));
        const int verdict = processM17PKT(opts, state);
        (void)dsd_test_capture_stderr_end(&cap);
        DSD_MEMSET(out, 0, sizeof(out));
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        assert(verdict == 0);
        assert(g_search_evidence == lsf_evidence);
        assert(state->m17_confirmed == 1U);
        assert(state->m17_tx_ended == 1U);
        assert(strstr(out, "CRC ERR") != NULL);
        assert((strstr(out, "HI") != NULL) == relaxed);
        finish(state);
    }
}

/* Issue #890: a packet starts only behind a checked packet-mode LSF frame the decoder applied. One whose source is the
   broadcast address (a destination only), or whose TYPE sets a reserved bit, passes its CRC but is not applied: the
   packet after it is not decoded (its CAN would be the reset one). The same packet behind a usable LSF decodes. */
static void
test_packet_behind_an_unapplied_lsf_is_not_decoded(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    static const char text[] = "HI";
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    const unsigned long long dst = m17_encode_b40_callsign(0ULL, "AB1CD");
    const unsigned long long src = m17_encode_b40_callsign(0ULL, "EF2GH");
    const unsigned long long packet_can5 = m17_compose_frame_info(0U, 0U, 0U, 0U, 5U, 0U, 0U);
    const unsigned long long setups[3][2] = {
        {0xFFFFFFFFFFFFULL, packet_can5}, /* the broadcast address as the source */
        {src, packet_can5 | 0x0002ULL},   /* a reserved TYPE bit */
        {src, packet_can5},               /* usable */
    };
    for (size_t i = 0U; i < 3U; i++) {
        prepare(opts, state);
        build_lsf_bits_with(lsf_bits, dst, setups[i][0], setups[i][1]);
        state->synctype = DSD_SYNC_M17_LSF_POS;
        load_lsf_frame(lsf_bits);
        (void)processM17LSF(opts, state);
        state->synctype = DSD_SYNC_M17_PKT_POS;
        const int evidence = g_search_evidence;
        assert(load_packet_frame(text, 0) == 1);
        const int usable = i == 2U;
        assert(processM17PKT(opts, state) == usable);
        assert(g_search_evidence == evidence + usable);
        finish(state);
    }
}

/* Issue #890: a packet frame the input's end cuts (a read that failed and asked for shutdown, whose last six symbols
   are uncounted stand-ins; a capture or PCM file that ended with the fallback asked for, whose stand-ins are counted,
   so the symbol count says the frame was read whole) is not decoded or credited. So is a packet LSF frame the input's
   end cuts: no packet follows it. */
static void
test_packet_frame_at_the_input_end_decodes_nothing(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    static const char text[] = "HI";
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_packet_lsf_bits(lsf_bits);
    for (int mode = TEST_END_EXIT; mode <= TEST_END_FALLBACK; mode++) {
        prepare(opts, state);
        read_packet_lsf_frame(opts, state);
        const int evidence = g_search_evidence;
        const uint32_t count = state->symbolcnt;
        (void)load_packet_frame(text, 0);
        g_input_end_mode = mode;
        g_input_end_at = 178U;
        assert(processM17PKT(opts, state) == 0);
        /* Marked cut, as an interruption marks a frame: the engine withholds the frame's verdict from the hunt. */
        assert(state->input_interrupted == 1);
        assert(state->symbolcnt - count == (mode == TEST_END_EXIT ? 178U : M17_PAYLOAD_SYMBOLS));
        assert(g_search_evidence == evidence);
        assert(state->m17_pbc_ct == 0U);
        finish(state);

        prepare(opts, state);
        state->synctype = DSD_SYNC_M17_LSF_POS;
        load_lsf_frame(lsf_bits);
        g_input_end_mode = mode;
        g_input_end_at = 178U;
        assert(processM17LSF(opts, state) == 0);
        assert(state->input_interrupted == 1);
        dsd_exitflag_store(0);
        state->input_interrupted = 0;
        state->input_fallback_pending = 0;
        state->synctype = DSD_SYNC_M17_PKT_POS;
        (void)load_packet_frame(text, 0);
        assert(processM17PKT(opts, state) == 0);
        finish(state);
    }
}

/* Issue #890: the spacing between packet frames is counted in one symbol clock epoch. A rewound or switched input
   starts another (dsd_state::symbol_clock_epoch), however many symbols the count says passed: a packet read in it is
   not one the LSF frame read before started. */
static void
test_packet_after_a_symbol_clock_restart_is_not_decoded(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    static const char text[] = "HI";
    for (int restart = 0; restart <= 1; restart++) {
        prepare(opts, state);
        read_packet_lsf_frame(opts, state);
        if (restart) {
            state->symbol_clock_epoch++;
        }
        (void)load_packet_frame(text, 0);
        assert(processM17PKT(opts, state) == !restart);
        finish(state);
    }
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

/* Issue #834: the content notes a scan quiet check hears through the content-evidence hook. */
static int g_quiet_proven;
static int g_quiet_partial;
static uint32_t g_quiet_hold_ms;

static void
record_quiet_note(dsd_state* state, int level, uint32_t hold_ms) {
    (void)state;
    if (level == DSD_CONTENT_EVIDENCE_PROVEN) {
        g_quiet_proven++;
    } else if (level == DSD_CONTENT_EVIDENCE_PARTIAL) {
        g_quiet_partial++;
        g_quiet_hold_ms = hold_ms;
    }
}

static void
quiet_notes_install(int on) {
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.content_evidence = record_quiet_note;
    dsd_frame_sync_hooks_set(on ? &hooks : NULL);
    g_quiet_proven = 0;
    g_quiet_partial = 0;
    g_quiet_hold_ms = 0U;
}

/* Issue #834: a packet frame before the end-of-transmission frame carries no check of its own (the packet's CRC is in
   its last frame), so to a scan quiet check it waits for that verdict: PARTIAL, 80 ms. The last frame's CRC is
   PROVEN, with no PARTIAL; a cut frame notes nothing; nothing with the setting off. */
static void
test_packet_frames_before_their_crc_are_partial(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const char text[] = "A KEY-UP WAITS FOR ITS CRC";
    for (int on = 0; on <= 1; on++) {
        prepare(&opts, &state);
        opts.scan_skip_silent = on;
        read_packet_lsf_frame(&opts, &state);
        quiet_notes_install(1);
        assert(load_packet_frame(text, 0) == 2);
        assert(processM17PKT(&opts, &state) == 1);
        assert(g_quiet_partial == on && g_quiet_proven == 0);
        assert(!on || g_quiet_hold_ms == 80U);
        g_quiet_partial = 0;
        (void)load_packet_frame(text, 1);
        assert(processM17PKT(&opts, &state) == 1);
        assert(state.m17_confirmed == 1U);
        assert(g_quiet_proven == on && g_quiet_partial == 0);
        finish(&state);

        prepare(&opts, &state);
        opts.scan_skip_silent = on;
        read_packet_lsf_frame(&opts, &state);
        quiet_notes_install(1);
        (void)load_packet_frame(text, 0);
        g_interrupt_at = 170U;
        assert(processM17PKT(&opts, &state) == 0);
        assert(g_quiet_partial == 0 && g_quiet_proven == 0);
        quiet_notes_install(0);
        finish(&state);
    }
}

/* Issue #890: what an M17 transmission reports to trunk scan's policy table. A link setup frame whose CRC passed reports
   the checked values it binds (encryption, voice stream); a clean LICH reports once it completes a pair with the frame
   before it, anonymous before any LSF was checked and with the bound values after one; the LSF the six LICH chunks
   rebuild whole reports with them; a packet whose CRC passed reports a data call; a BERT lock (a test) never reports.
   The receive side's CAN filter is kept: a CAN it refuses, and an unknown CAN while it is set, report nothing. */
static void
test_checked_content_reports_to_trunk_scan(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    uint8_t lsf_bits[TEST_M17_LSF_BITS];
    build_lsf_bits(lsf_bits);
    const uint8_t bound = DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED | DSD_SCAN_ACTIVITY_KNOWN_DATA;

    prepare(opts, state);
    dsd_test_scan_activity_install();
    state->synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    assert(processM17LSF(opts, state) == 1);
    assert(g_test_scan_activity.count == 1 && DSD_SYNC_IS_M17(g_test_scan_activity.protocol));
    assert(g_test_scan_activity.last.known == bound && g_test_scan_activity.last.encrypted == 0U
           && g_test_scan_activity.last.data_call == 0U);
    /* Stream frames after it: the first clean LICH starts a pair, the second completes it, with the LSF's values. */
    dsd_test_scan_activity_reset();
    state->synctype = DSD_SYNC_M17_STR_POS;
    load_stream_frame(lsf_bits, 0U, 0U);
    assert(processM17STR(opts, state) == 1);
    const int after_first = g_test_scan_activity.count;
    load_stream_frame(lsf_bits, 1U, 1U);
    assert(processM17STR(opts, state) == 1);
    assert(g_test_scan_activity.count == after_first + 1 && g_test_scan_activity.last.known == bound);
    finish(state);

    /* Late entry: no LSF frame. The pair reports anonymous; the rebuilt LSF, whole and checked, binds the values. */
    prepare(opts, state);
    dsd_test_scan_activity_install();
    state->synctype = DSD_SYNC_M17_STR_POS;
    load_stream_frame(lsf_bits, 0U, 0U);
    (void)processM17STR(opts, state);
    assert(g_test_scan_activity.count == 0);
    load_stream_frame(lsf_bits, 1U, 1U);
    (void)processM17STR(opts, state);
    assert(g_test_scan_activity.count == 1 && g_test_scan_activity.last.known == 0U);
    /* The confirmation the pair made is part of the assembly's binding, so the chunks before it go, and the first chunk
       5 closes an assembly that is not whole: a whole one follows. */
    for (uint16_t fn = 2U; fn < 12U; fn++) {
        load_stream_frame(lsf_bits, (uint8_t)(fn % 6U), fn);
        (void)processM17STR(opts, state);
    }
    assert(state->m17_lsf_checked == 1U && g_test_scan_activity.last.known == bound);
    finish(state);

    /* Under a CAN filter: the LSF's CAN 5 refused by a filter of 3, and the late entry's unknown CAN, report nothing. */
    prepare(opts, state);
    dsd_test_scan_activity_install();
    state->m17_can_en = 3;
    state->synctype = DSD_SYNC_M17_STR_POS;
    load_stream_frame(lsf_bits, 0U, 0U);
    (void)processM17STR(opts, state);
    load_stream_frame(lsf_bits, 1U, 1U);
    (void)processM17STR(opts, state);
    assert(g_test_scan_activity.count == 0);
    state->synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    (void)processM17LSF(opts, state);
    assert(g_test_scan_activity.count == 0);
    finish(state);
    prepare(opts, state);
    dsd_test_scan_activity_install();
    state->m17_can_en = 5;
    state->synctype = DSD_SYNC_M17_LSF_POS;
    load_lsf_frame(lsf_bits);
    (void)processM17LSF(opts, state);
    assert(g_test_scan_activity.count == 1);
    finish(state);

    /* A packet whose CRC passed: a data call. Its own checked packet-mode LSF frame comes first (the strict RF packet
       rule) and reports a data call too. */
    static const char text[] = "A PACKET HOLDS AS DATA";
    prepare(opts, state);
    dsd_test_scan_activity_install();
    read_packet_lsf_frame(opts, state);
    assert(g_test_scan_activity.count == 1 && (g_test_scan_activity.last.known & DSD_SCAN_ACTIVITY_KNOWN_DATA)
           && g_test_scan_activity.last.data_call == 1U);
    dsd_test_scan_activity_reset();
    assert(load_packet_frame(text, 0) == 2);
    (void)processM17PKT(opts, state);
    assert(g_test_scan_activity.count == 0);
    (void)load_packet_frame(text, 1);
    (void)processM17PKT(opts, state);
    assert(g_test_scan_activity.count == 1 && (g_test_scan_activity.last.known & DSD_SCAN_ACTIVITY_KNOWN_DATA)
           && g_test_scan_activity.last.data_call == 1U);
    finish(state);

    /* BERT: confirmed, never a call. */
    prepare(opts, state);
    dsd_test_scan_activity_install();
    state->synctype = DSD_SYNC_M17_BRT_POS;
    uint16_t lfsr = 1U;
    load_bert_frame(&lfsr);
    assert(processM17BRT(opts, state) == 1 && state->m17_confirmed == 1U);
    assert(g_test_scan_activity.count == 0);
    finish(state);
    dsd_trunk_scan_hooks_set(NULL);
}

/* Issue #890 F3, end to end: a BERT lock is PROVEN content to a quiet check on an untyped row, and nothing on a row of
   the m17 class (entered as a -Y row or trunk-scan target enters it), which it confirms all the same. */
static void
test_bert_on_a_typed_row_is_profile_only(void) {
    dsd_opts* opts = g_opts;
    dsd_state* state = g_state;
    for (int typed = 0; typed < 2; typed++) {
        prepare(opts, state);
        if (typed) {
            assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_M17) == 0);
        }
        opts->scan_skip_silent = 1;
        opts->aggressive_framesync = 1;
        state->synctype = DSD_SYNC_M17_BRT_POS;
        quiet_notes_install(1);
        uint16_t lfsr = 1U;
        load_bert_frame(&lfsr);
        assert(processM17BRT(opts, state) == 1 && state->m17_confirmed == 1U);
        assert(g_quiet_proven == (typed ? 0 : 1));
        quiet_notes_install(0);
        finish(state);
    }
}

int
main(void) {
    InitAllFecFunction();
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    assert(g_opts != NULL && g_state != NULL);
    test_stream_frame_cut_by_an_input_interruption_is_dropped();
    test_lsf_cut_by_an_input_interruption_publishes_nothing();
    test_packet_frame_cut_by_an_input_interruption_decodes_nothing();
    test_packet_cut_early_is_lost();
    test_packet_eof_verdict_is_its_own();
    test_packet_malformed_eof_earns_no_credit();
    test_packet_out_of_order_first_chunk_earns_no_credit();
    test_packet_crc_failure_earns_no_credit();
    test_packet_behind_an_unapplied_lsf_is_not_decoded();
    test_packet_frame_at_the_input_end_decodes_nothing();
    test_packet_after_a_symbol_clock_restart_is_not_decoded();
    test_bert_frame_cut_by_an_input_interruption_counts_nothing();
    test_frames_split_by_a_carrier_boundary_are_dropped();
    test_packet_frames_before_their_crc_are_partial();
    test_checked_content_reports_to_trunk_scan();
    test_bert_on_a_typed_row_is_profile_only();
    free(g_state);
    free(g_opts);
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
