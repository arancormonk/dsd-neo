// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dPMR headers and end frames in the hunt (issue #890: B9, A6, A7, F8; src/dsp/frame_sync_dpmr.c).
 *
 * Before #890 the frame sync matched FS2 alone: FS1 and FS3 were never recognised, so no header ever decoded and no
 * call ended on its end frame. These cases drive getFrameSync() as the engine does, with a symbol source that plays
 * frames built by an encoder written from ETSI TS 102 658 V2.6.1 (tests/protocol/dpmr/dpmr_spec_encoder.h), in both
 * polarities: a header is accepted as FS1 only when an MI copy passed its CRC-8 (one copy is enough; two good copies
 * that disagree are no header), an end frame as FS3 only where one can be and when an END copy passed its CRC-7. A
 * candidate that fails costs nothing: the hunt reads its symbols as any others, so an NXDN48 frame sync inside it, the
 * real header behind a false FS1, and the frame after it are matched as before.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dpmr_spec_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

enum { STREAM_MAX = 40000, FS2_BODY = 372 };

/* The stream, as dibits; levels are +3 for 1, +1 for 0, -1 for 2 and -3 for 3, all negated when g_inverted. */
static uint8_t g_dibits[STREAM_MAX];
static int g_len;
static int g_pos;
static int g_inverted;
/* The read at which getSymbol() records an input interruption (a Signal search leaving, a stream gap); -1 none. */
static int g_interrupt_at = -1;

static float
dibit_level(uint8_t dibit) {
    static const float k_level[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    const float level = k_level[dibit & 3U];
    return g_inverted ? -level : level;
}

/* Filler between frames: 0, 1, 2, 3 over and over, every level, signs 1133 (the preamble's), no sync word of dPMR or
   NXDN in it. */
static uint8_t
filler_dibit(int at) {
    return (uint8_t)(at & 3);
}

/* What dsd_symbol.c's read leaves behind: the soft history, the symbol count and the stream's symbol clock. */
float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;
    const float symbol = dibit_level(g_pos < g_len ? g_dibits[g_pos] : filler_dibit(g_pos));
    if (g_pos == g_interrupt_at) {
        state->input_interrupted = 1;
    }
    g_pos++;
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    state->symbol_clock++;
    return symbol;
}

static void
stream_reset(void) {
    g_len = 0;
    g_pos = 0;
    g_inverted = 0;
    g_interrupt_at = -1;
}

static void
put_dibits(const uint8_t* dibits, int count) {
    assert(g_len + count <= STREAM_MAX);
    memcpy(g_dibits + g_len, dibits, (size_t)count);
    g_len += count;
}

static void
put_filler(int count) {
    for (int i = 0; i < count; i++) {
        uint8_t d = filler_dibit(g_len);
        put_dibits(&d, 1);
    }
}

static void
put_word(const char* word) {
    uint8_t dibits[24];
    const int n = dpmr_spec_word(word, dibits);
    put_dibits(dibits, n);
}

static void
put_bits(const uint8_t* bits, int nbits) {
    uint8_t dibits[DPMR_SPEC_MI_WIRE];
    dpmr_spec_dibits(bits, nbits, dibits);
    put_dibits(dibits, nbits / 2);
}

/* A header: the preamble, FS1 and MI0, the channel code of colour code 12, MI1; @p info1 NULL repeats @p info0, and
   @p flip0/@p flip1 flip that many wire bits of a copy, two to a Hamming word (an uncorrectable double error). */
static void
put_header(const char* fs1, const uint8_t* info0, const uint8_t* info1, int flip0, int flip1) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    uint8_t mi0[DPMR_SPEC_MI_WIRE];
    uint8_t mi1[DPMR_SPEC_MI_WIRE];
    uint8_t cc[24];
    dpmr_spec_encode_mi(info0, mi0);
    dpmr_spec_encode_mi(info1 != NULL ? info1 : info0, mi1);
    /* Wire bit 10r + c is coded bit 12c + r: wire bits 0 and 10 are bits 0 and 1 of Hamming word 0. */
    for (int i = 0; i < flip0; i++) {
        mi0[i * 10] ^= 1U;
    }
    for (int i = 0; i < flip1; i++) {
        mi1[i * 10] ^= 1U;
    }
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U);
    put_dibits(k_preamble, DPMR_SPEC_PREAMBLE_DIBITS);
    put_word(fs1);
    put_bits(mi0, DPMR_SPEC_MI_WIRE);
    put_bits(cc, 24);
    put_bits(mi1, DPMR_SPEC_MI_WIRE);
}

/* An FS2 frame: the word and a body of filler. */
static void
put_fs2_frame(const char* fs2) {
    put_word(fs2);
    put_filler(FS2_BODY);
}

/* The two layouts of an end frame's second copy: one 72-bit scrambler run (7.7, the off-air recording's), or a fresh
   scrambler for each copy (11.6). */
enum { END_ONE_RUN = 0, END_FRESH_COPIES = 1 };

/* An end frame: FS3 and two END copies of @p info17 in @p layout; @p flip0/@p flip1 as for put_header(). */
static void
put_end(const char* fs3, const uint8_t* info17, int layout, int flip0, int flip1) {
    uint8_t end0[DPMR_SPEC_END_WIRE];
    uint8_t end1[DPMR_SPEC_END_WIRE];
    dpmr_spec_encode_end(info17, 0, end0);
    dpmr_spec_encode_end(info17, layout == END_ONE_RUN ? DPMR_SPEC_END_WIRE : 0, end1);
    for (int i = 0; i < flip0; i++) {
        end0[i] ^= 1U;
    }
    for (int i = 0; i < flip1; i++) {
        end1[i] ^= 1U;
    }
    put_word(fs3);
    put_bits(end0, DPMR_SPEC_END_WIRE);
    put_bits(end1, DPMR_SPEC_END_WIRE);
}

static const dpmr_spec_header k_voice_header = {
    .mt = 0U,
    .called = 1464100U,
    .calling = 2928210U,
    .mode = 1U,
    .version = 0U,
    .format = 1U,
};

/* An end frame with status message 21 (11.6: ET 01, no ARQ, no Tx_Wait). */
static void
end_info(uint8_t* info17) {
    memset(info17, 0, DPMR_SPEC_END_BITS);
    dpmr_spec_put(info17, 0, 2, 1U);
    dpmr_spec_put(info17, 8, 5, 21U);
}

/* One decoder for every case, on the heap (AGENTS.md: dsd_state and dsd_opts are heap-allocated), set up afresh by
   setup() and its buffers freed at each case's end. */
static dsd_opts* g_opts;
static dsd_state* g_state;

typedef enum { MODE_DPMR, MODE_DPMR_NXDN48, MODE_AUTO } hunt_mode;

static void
setup(dsd_opts* opts, dsd_state* state, hunt_mode mode, int inverted) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->wav_decimator = 48000;
    opts->frame_dpmr = 1;
    if (mode != MODE_DPMR) {
        opts->frame_nxdn48 = 1;
    }
    if (mode == MODE_AUTO) {
        opts->frame_dstar = 1;
        opts->frame_x2tdma = 1;
        opts->frame_p25p1 = 1;
        opts->frame_p25p2 = 1;
        opts->frame_nxdn96 = 1;
        opts->frame_dmr = 1;
        opts->frame_provoice = 1;
        opts->frame_ysf = 1;
        opts->frame_m17 = 1;
    }
    opts->inverted_dpmr = inverted;
    opts->mod_gfsk = 1;
    opts->mod_cli_lock = 1;
    opts->msize = 1024;
    opts->ssize = 128;
    state->rf_mod = 2;
    state->p25_p2_active_slot = -1;
    state->min = -3.0f;
    state->max = 3.0f;
    state->center = 0.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    state->samplesPerSymbol = 20;
    state->symbolCenter = 10;
    assert(init_state_buffers(state));
    dsd_frame_sync_reset_mod_state();
}

/* getFrameSync() until a sync or @p limit symbols of the stream are read; the sync, or DSD_SYNC_NONE. */
static int
hunt(dsd_opts* opts, dsd_state* state, int limit) {
    while (g_pos < limit) {
        const int sync = getFrameSync(opts, state);
        if (sync > DSD_SYNC_NONE) {
            return sync;
        }
        if (state->input_interrupted) {
            return DSD_SYNC_NONE;
        }
    }
    return DSD_SYNC_NONE;
}

/* What processdPMRvoice() reads behind an FS2. */
static void
read_fs2_body(dsd_opts* opts, dsd_state* state) {
    for (int i = 0; i < FS2_BODY; i++) {
        (void)getSymbol(opts, state, 1);
    }
}

static int
checked_bits_equal(const dsd_state* state, const uint8_t* bits, int count) {
    return memcmp(state->dpmr_sync.checked_bits, bits, (size_t)count) == 0;
}

/* The decoder's own wire codecs against the spec encoder: clean copies, every single-bit error corrected, a double
   error in one word or a CRC that does not match rejected. */
static void
test_wire_codecs(void) {
    uint8_t key[120];
    uint8_t ref[120];
    frame_sync_dpmr_keystream(key, 120);
    dpmr_spec_keystream(ref, 120);
    assert(memcmp(key, ref, sizeof key) == 0);

    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t wire[DPMR_SPEC_MI_WIRE];
    uint8_t out[DPMR_SPEC_MI_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);
    dpmr_spec_encode_mi(info, wire);
    assert(frame_sync_dpmr_decode_mi(wire, out) == 1 && memcmp(out, info, sizeof out) == 0);
    for (int bit = 0; bit < DPMR_SPEC_MI_WIRE; bit++) {
        wire[bit] ^= 1U;
        memset(out, 0xAA, sizeof out);
        assert(frame_sync_dpmr_decode_mi(wire, out) == 1 && memcmp(out, info, sizeof out) == 0);
        wire[bit] ^= 1U;
    }
    wire[0] ^= 1U;
    wire[10] ^= 1U; /* bits 0 and 1 of Hamming word 0 */
    assert(frame_sync_dpmr_decode_mi(wire, out) == 0);
    wire[0] ^= 1U;
    wire[10] ^= 1U;
    /* A CRC-8 that does not match: the information changed, its CRC not. */
    uint8_t other[DPMR_SPEC_MI_BITS];
    memcpy(other, info, sizeof other);
    other[71] ^= 1U;
    uint8_t wrong[DPMR_SPEC_MI_WIRE];
    dpmr_spec_encode_mi(other, wrong);
    uint8_t mixed[DPMR_SPEC_MI_WIRE];
    memcpy(mixed, wire, sizeof mixed);
    /* Hamming word 8 holds information bits 64..71: take it whole from the other copy, the CRC word from this one. */
    for (int r = 0; r < 12; r++) {
        mixed[(r * 10) + 8] = wrong[(r * 10) + 8];
    }
    assert(frame_sync_dpmr_decode_mi(mixed, out) == 0);

    uint8_t end[DPMR_SPEC_END_BITS];
    uint8_t end_wire[DPMR_SPEC_END_WIRE];
    uint8_t end_out[DPMR_SPEC_END_BITS];
    end_info(end);
    dpmr_spec_encode_end(end, 0, end_wire);
    assert(frame_sync_dpmr_decode_end(end_wire, 0, end_out) == 1 && memcmp(end_out, end, sizeof end) == 0);
    for (int bit = 0; bit < DPMR_SPEC_END_WIRE; bit++) {
        end_wire[bit] ^= 1U;
        assert(frame_sync_dpmr_decode_end(end_wire, 0, end_out) == 1 && memcmp(end_out, end, sizeof end) == 0);
        end_wire[bit] ^= 1U;
    }
    end_wire[0] ^= 1U;
    end_wire[1] ^= 1U;
    assert(frame_sync_dpmr_decode_end(end_wire, 0, end_out) == 0);
    /* The second copy of one 72-bit run reads on the run's bits 36 to 71, not on a fresh scrambler's. */
    dpmr_spec_encode_end(end, DPMR_SPEC_END_WIRE, end_wire);
    assert(frame_sync_dpmr_decode_end(end_wire, DPMR_SPEC_END_WIRE, end_out) == 1);
    assert(memcmp(end_out, end, sizeof end) == 0);
    assert(frame_sync_dpmr_decode_end(end_wire, 0, end_out) == 0);
}

/* The CRC conventions, on what the off-air sigidwiki recording behind tests/fixtures/iq/dpmr.iq sent (DPMR.zip,
   SDRSharp_20160101_231640Z_12kHz_IQ.wav): its first header's MI, 057FFFF57FFFF02708 with CRC-8 E6 (all 18 MI copies
   of its 9 headers pass with a zero preset and fail with all ones), and its end frame's END 00200 with CRC-7 16. The
   copies are built with the CRCs the radio sent, not computed ones, so the decoder's CRC registers are checked against
   the air. */
static void
test_crc_conventions_off_air(void) {
    static const uint8_t k_mi_hex[18] = {0, 5, 7, 15, 15, 15, 15, 5, 7, 15, 15, 15, 15, 0, 2, 7, 0, 8};
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t wire[DPMR_SPEC_MI_WIRE];
    uint8_t out[DPMR_SPEC_MI_BITS];
    for (int i = 0; i < 18; i++) {
        dpmr_spec_put(info, i * 4, 4, k_mi_hex[i]);
    }
    assert(dpmr_spec_crc8(info) == 0xE6U);
    dpmr_spec_encode_mi_crc(info, 0xE6U, wire);
    assert(frame_sync_dpmr_decode_mi(wire, out) == 1 && memcmp(out, info, sizeof out) == 0);
    dpmr_spec_encode_mi_crc(info, 0xE9U, wire); /* the CRC an all-ones preset gives */
    assert(frame_sync_dpmr_decode_mi(wire, out) == 0);

    uint8_t end[DPMR_SPEC_END_BITS];
    uint8_t end_wire[DPMR_SPEC_END_WIRE];
    uint8_t end_out[DPMR_SPEC_END_BITS];
    dpmr_spec_put(end, 0, DPMR_SPEC_END_BITS, 0x00200U);
    assert(dpmr_spec_crc7(end) == 0x16U);
    dpmr_spec_encode_end_crc(end, 0x16U, 0, end_wire);
    assert(frame_sync_dpmr_decode_end(end_wire, 0, end_out) == 1 && memcmp(end_out, end, sizeof end) == 0);
}

/* A header is FS1 on its last symbol, in either polarity, read whole and checked: both copies passed. */
static void
test_header_both_polarities(void) {
    for (int inverted = 0; inverted <= 1; inverted++) {
        uint8_t info[DPMR_SPEC_MI_BITS];
        setup(g_opts, g_state, MODE_DPMR, inverted);
        stream_reset();
        g_inverted = inverted;
        dpmr_spec_header_bits(&k_voice_header, info);
        put_filler(100);
        put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
        const int end = g_len;
        put_filler(200);
        const int sync = hunt(g_opts, g_state, g_len);
        assert(sync == (inverted ? DSD_SYNC_DPMR_FS1_NEG : DSD_SYNC_DPMR_FS1_POS));
        assert(g_pos == end);
        assert(g_state->dpmr_sync.checked_kind == DSD_DPMR_SYNC_FRAME_HEADER);
        assert(g_state->dpmr_sync.checked_copies == (DSD_DPMR_SYNC_COPY_0 | DSD_DPMR_SYNC_COPY_1));
        assert(g_state->dpmr_sync.checked_clock == g_state->symbol_clock);
        assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
        uint8_t cc[24];
        dpmr_spec_put(cc, 0, 24, 0x5D5D55U);
        assert(memcmp(g_state->dpmr_sync.checked_cc, cc, sizeof cc) == 0);
        assert(g_state->lastsynctype == sync);
        free_state_buffers(g_state);
    }
}

/* One good copy is enough, whichever it is; two good copies that disagree are no header, and the header after them is
   found. */
static void
test_header_copies(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t other[DPMR_SPEC_MI_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);
    dpmr_spec_header bad = k_voice_header;
    bad.calling = 11206075U;
    dpmr_spec_header_bits(&bad, other);

    for (int which = 0; which <= 1; which++) {
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(60);
        put_header(DPMR_FRAME_SYNC_1, info, NULL, which == 0 ? 2 : 0, which == 1 ? 2 : 0);
        put_filler(100);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
        assert(g_state->dpmr_sync.checked_copies == (which == 0 ? DSD_DPMR_SYNC_COPY_1 : DSD_DPMR_SYNC_COPY_0));
        assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
        free_state_buffers(g_state);
    }

    /* Neither copy good. */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(60);
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 2, 2);
    put_filler(100);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);

    /* Two good copies naming different callers, then a good header. */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(60);
    put_header(DPMR_FRAME_SYNC_1, info, other, 0, 0);
    const int conflict_end = g_len;
    put_filler(40);
    put_header(DPMR_FRAME_SYNC_1, other, NULL, 0, 0);
    const int good_end = g_len;
    put_filler(100);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == good_end && g_pos > conflict_end);
    assert(checked_bits_equal(g_state, other, DPMR_SPEC_MI_BITS));
    free_state_buffers(g_state);
}

/* An input interruption inside the header (a Signal search leaving it, a stream gap) cuts it: the hunt does not take
   it up afterwards, though its symbols go on. */
static void
test_header_cut_by_an_interruption(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(60);
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
    put_filler(200);
    g_interrupt_at = g_len - 200 - 30; /* inside MI1 */
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    assert(g_state->input_interrupted == 1);
    g_state->input_interrupted = 0; /* the engine loop takes it up */
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);
}

/* FS4 is FS1's symbol-wise complement (6.1.4 NOTE): the configured polarity decides which a word is. A header under
   the other polarity, or an FS4 before a header's bits, is no FS1. */
static void
test_fs1_fs4_alias(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);
    for (int configured = 0; configured <= 1; configured++) {
        setup(g_opts, g_state, MODE_DPMR, configured);
        stream_reset();
        g_inverted = !configured; /* the stream in the other polarity */
        put_filler(60);
        put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
        put_filler(100);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
        free_state_buffers(g_state);
    }
    /* FS4 in the configured (positive) polarity, then a header's bits. */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(60);
    put_header(DPMR_FRAME_SYNC_4, info, NULL, 0, 0);
    put_filler(100);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);
}

/* A transmission in both polarities and both end frame layouts: header, a superframe (two FS2 frames), end frame. The
   FS3 one FS2 frame after the last FS2 is the end frame, checked, both copies passing. */
static void
test_transmission_ends_on_its_end_frame(void) {
    for (int run = 0; run < 4; run++) {
        const int inverted = run & 1;
        const int layout = run >> 1;
        uint8_t info[DPMR_SPEC_MI_BITS];
        uint8_t end[DPMR_SPEC_END_BITS];
        setup(g_opts, g_state, MODE_DPMR, inverted);
        stream_reset();
        g_inverted = inverted;
        dpmr_spec_header_bits(&k_voice_header, info);
        end_info(end);
        put_filler(80);
        /* The stream is built in its own polarity: the words as transmitted (positive), negated with the rest. */
        put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
        put_fs2_frame(DPMR_FRAME_SYNC_2);
        put_fs2_frame(DPMR_FRAME_SYNC_2);
        put_end(DPMR_FRAME_SYNC_3, end, layout, 0, 0);
        const int end_at = g_len;
        put_filler(200);
        const int fs1 = inverted ? DSD_SYNC_DPMR_FS1_NEG : DSD_SYNC_DPMR_FS1_POS;
        const int fs2s = inverted ? DSD_SYNC_DPMR_FS2_NEG : DSD_SYNC_DPMR_FS2_POS;
        const int fs3s = inverted ? DSD_SYNC_DPMR_FS3_NEG : DSD_SYNC_DPMR_FS3_POS;
        assert(hunt(g_opts, g_state, g_len) == fs1);
        for (int frame = 0; frame < 2; frame++) {
            assert(hunt(g_opts, g_state, g_len) == fs2s);
            read_fs2_body(g_opts, g_state);
        }
        assert(hunt(g_opts, g_state, g_len) == fs3s);
        assert(g_pos == end_at);
        assert(g_state->dpmr_sync.checked_kind == DSD_DPMR_SYNC_FRAME_END);
        assert(g_state->dpmr_sync.checked_copies == (DSD_DPMR_SYNC_COPY_0 | DSD_DPMR_SYNC_COPY_1));
        assert(checked_bits_equal(g_state, end, DPMR_SPEC_END_BITS));
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
        free_state_buffers(g_state);
    }
}

/* An unsolicited status (4.1.5.1.2 b, Table 5.3 note 2): a Communications_Start header for data straight followed by an
   end frame, its FS3 twelve symbols after the header (F8). One END copy is enough: the second alone, in either layout. */
static void
test_status_header_then_end_frame(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t end[DPMR_SPEC_END_BITS];
    dpmr_spec_header status = k_voice_header;
    status.mode = 2U;
    status.mi_type = 1U;
    dpmr_spec_header_bits(&status, info);
    end_info(end);
    for (int layout = END_ONE_RUN; layout <= END_FRESH_COPIES; layout++) {
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(80);
        put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
        put_end(DPMR_FRAME_SYNC_3, end, layout, 2, 0);
        const int end_at = g_len;
        put_filler(200);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS3_POS);
        assert(g_pos == end_at);
        assert(g_state->dpmr_sync.checked_copies == DSD_DPMR_SYNC_COPY_1);
        assert(checked_bits_equal(g_state, end, DPMR_SPEC_END_BITS));
        free_state_buffers(g_state);
    }

    /* A header whose message type no END follows (an ACK, Table 5.3): no FS3 is looked for behind it. */
    dpmr_spec_header ack = k_voice_header;
    ack.mt = 3U;
    dpmr_spec_header_bits(&ack, info);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
    put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);
}

/* An end frame whose END copies both fail is no FS3, and neither is a good end frame where none can be (no FS2 frame
   or header before it). */
static void
test_unchecked_or_misplaced_end_frames(void) {
    uint8_t end[DPMR_SPEC_END_BITS];
    end_info(end);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 2, 2);
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS2_POS);
    read_fs2_body(g_opts, g_state);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);

    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);
}

/* The off-air recording's key-up (tests/fixtures/iq/dpmr.iq): nine Communications_Start headers back to back, one
   message frame (192 symbols, its preamble included) apart: eight powersave wake-up headers (MI_TYPE 111, WU counting
   8 down to 1, TS 102 658 5.5.19.1 Table 5.71) and the normal header (WU 0, MI_TYPE 000). Each is a header of its
   own, accepted on its last symbol. */
static void
test_powersave_headers_back_to_back(void) {
    uint8_t info[9][DPMR_SPEC_MI_BITS];
    int ends[9];
    setup(g_opts, g_state, MODE_DPMR, 1);
    stream_reset();
    g_inverted = 1;
    put_filler(80);
    for (int h = 0; h < 9; h++) {
        dpmr_spec_header header = {
            .mt = 0U,
            .called = 0x57FFFFU,
            .calling = 0x57FFFFU,
            .mode = 0U,
            .version = 0U,
            .format = 1U,
        };
        header.mi_type = h < 8 ? 7U : 0U;
        header.mi_detail = h < 8 ? (uint32_t)(8 - h) : 0U;
        dpmr_spec_header_bits(&header, info[h]);
        put_header(DPMR_FRAME_SYNC_1, info[h], NULL, 0, 0);
        ends[h] = g_len;
    }
    put_filler(200);
    for (int h = 0; h < 9; h++) {
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_NEG);
        assert(g_pos == ends[h]);
        assert(h == 0 || ends[h] - ends[h - 1] == 192);
        assert(checked_bits_equal(g_state, info[h], DPMR_SPEC_MI_BITS));
    }
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);
}

/* An end frame sent the 11.6 way (each copy from a fresh scrambler) whose second copy took bit errors, one at most in
   each Hamming word, that its fresh reading corrects but that also make it pass, by chance, read as the second half of
   one 72-bit run, with other bits. The codes are linear, so whether that run reading passes depends on the error
   pattern alone (here searched for), not on the END's contents. The reading that agrees with the first copy is the
   frame's: the frame is not two copies that disagree. */
static void
test_end_second_copy_agreeing_reading_wins(void) {
    uint8_t end[DPMR_SPEC_END_BITS];
    uint8_t wire[DPMR_SPEC_END_WIRE];
    uint8_t hit[DPMR_SPEC_END_WIRE];
    uint8_t out[DPMR_SPEC_END_BITS];
    end_info(end);
    dpmr_spec_encode_end(end, 0, wire);
    int found = 0;
    for (int e0 = 0; e0 <= 12 && !found; e0++) {
        for (int e1 = 0; e1 <= 12 && !found; e1++) {
            for (int e2 = 0; e2 <= 12 && !found; e2++) {
                memcpy(hit, wire, sizeof hit);
                const int err[3] = {e0, e1, e2};
                for (int w = 0; w < 3; w++) {
                    if (err[w] > 0) {
                        hit[(w * 12) + err[w] - 1] ^= 1U;
                    }
                }
                found = frame_sync_dpmr_decode_end(hit, DPMR_SPEC_END_WIRE, out) == 1
                        && memcmp(out, end, sizeof out) != 0 && frame_sync_dpmr_decode_end(hit, 0, out) == 1
                        && memcmp(out, end, sizeof out) == 0;
            }
        }
    }
    assert(found);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    put_word(DPMR_FRAME_SYNC_3);
    put_bits(wire, DPMR_SPEC_END_WIRE);
    put_bits(hit, DPMR_SPEC_END_WIRE);
    const int end_at = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS2_POS);
    read_fs2_body(g_opts, g_state);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS3_POS);
    assert(g_pos == end_at);
    assert(g_state->dpmr_sync.checked_copies == (DSD_DPMR_SYNC_COPY_0 | DSD_DPMR_SYNC_COPY_1));
    assert(checked_bits_equal(g_state, end, DPMR_SPEC_END_BITS));
    free_state_buffers(g_state);
}

/* NXDN48's frame sync word as transmitted: the first positive reading of the matcher's. */
static const char k_nxdn_fsw[] = "3131331131";

/* A false FS1 whose 132 symbols hold the second of two NXDN48 frame sync words a frame apart: NXDN48 takes it where it
   is, as it did when FS1 was never matched, under Auto and under dPMR and NXDN48 alone. */
static void
test_nxdn48_inside_a_rejected_candidate(void) {
    for (int m = 0; m <= 1; m++) {
        setup(g_opts, g_state, m == 0 ? MODE_AUTO : MODE_DPMR_NXDN48, 0);
        stream_reset();
        put_filler(100);
        put_word(k_nxdn_fsw);
        put_filler(192 - 10 - 40);
        put_word(DPMR_FRAME_SYNC_1);
        put_filler(40 - 24);
        put_word(k_nxdn_fsw); /* 192 symbols after the first, 16 into the candidate's frame */
        const int nxdn_at = g_len;
        put_filler(300);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NXDN_POS);
        assert(g_pos == nxdn_at);
        free_state_buffers(g_state);
    }
}

/* What follows a rejected candidate is found: a real header whose FS1 lies inside the false one's frame, and an FS2
   frame straight after a false one's. */
static void
test_frames_after_a_rejected_candidate(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);

    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_word(DPMR_FRAME_SYNC_1);
    put_filler(10); /* the real header's preamble starts 10 symbols into the false one's frame */
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
    const int header_end = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == header_end);
    free_state_buffers(g_state);

    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_word(DPMR_FRAME_SYNC_1);
    put_filler(DPMR_SPEC_HEADER_DIBITS - 24);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    const int fs2_at = g_len - FS2_BODY;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS2_POS);
    assert(g_pos == fs2_at);
    free_state_buffers(g_state);
}

/* Receiver noise with FS1 words planted in it every 500 symbols: no header and no end frame is accepted. */
static void
test_noise_accepts_no_header_or_end(void) {
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    uint32_t seed = 0x890DU;
    while (g_len < STREAM_MAX - 600) {
        for (int i = 0; i < 476; i++) {
            seed = (seed * 1103515245U) + 12345U;
            uint8_t d = (uint8_t)((seed >> 16) & 3U);
            put_dibits(&d, 1);
        }
        put_word(DPMR_FRAME_SYNC_1);
    }
    int fs2 = 0;
    while (g_pos < g_len) {
        const int sync = hunt(g_opts, g_state, g_len);
        assert(sync == DSD_SYNC_NONE || sync == DSD_SYNC_DPMR_FS2_POS);
        if (sync == DSD_SYNC_DPMR_FS2_POS) {
            fs2++;
            read_fs2_body(g_opts, g_state);
        }
    }
    (void)fs2;
    free_state_buffers(g_state);
}

/* Run with DSD_NEO_DEBUG_SYMBOL_TIMING=1 (FRAME_SYNC_DPMR_SYMBOL_TIMING): the symbol-timing diagnostic measures each
   accept on the trailing dibits of its sync word. A header and an end frame are accepted on their frame's last symbol,
   132 and 36 symbols past the word, where those dibits are payload: they are not measured; the FS2s between them are. */
static void
test_symbol_timing_skips_delayed_accepts(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t end[DPMR_SPEC_END_BITS];
    dpmr_spec_header_bits(&k_voice_header, info);
    end_info(end);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
    put_filler(200);
    dsd_test_capture_stderr cap;
    char printed[4096];
    assert(dsd_test_capture_stderr_begin(&cap, "dpmr_symbol_timing") == 0);
    const int fs1 = hunt(g_opts, g_state, g_len);
    int fs2 = 0;
    for (int frame = 0; frame < 2; frame++) {
        fs2 += hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS2_POS;
        read_fs2_body(g_opts, g_state);
    }
    const int fs3 = hunt(g_opts, g_state, g_len);
    assert(dsd_test_capture_stderr_end(&cap) == 0);
    assert(dsd_test_capture_stderr_read(&cap, printed, sizeof printed) == 0);
    assert(fs1 == DSD_SYNC_DPMR_FS1_POS && fs2 == 2 && fs3 == DSD_SYNC_DPMR_FS3_POS);
    char want[48];
    DSD_SNPRINTF(want, sizeof want, "SYMTIMING: sync=%d ", DSD_SYNC_DPMR_FS2_POS);
    assert(strstr(printed, want) != NULL);
    DSD_SNPRINTF(want, sizeof want, "SYMTIMING: sync=%d ", DSD_SYNC_DPMR_FS1_POS);
    assert(strstr(printed, want) == NULL);
    DSD_SNPRINTF(want, sizeof want, "SYMTIMING: sync=%d ", DSD_SYNC_DPMR_FS3_POS);
    assert(strstr(printed, want) == NULL);
    free_state_buffers(g_state);
}

int
main(int argc, char** argv) {
    g_opts = (dsd_opts*)calloc(1U, sizeof(*g_opts));
    g_state = (dsd_state*)calloc(1U, sizeof(*g_state));
    assert(g_opts != NULL && g_state != NULL);
    if (argc > 1 && strcmp(argv[1], "symbol-timing") == 0) {
        test_symbol_timing_skips_delayed_accepts();
        free(g_state);
        free(g_opts);
        printf("FRAME_SYNC_DPMR_SYMBOL_TIMING: OK\n");
        return 0;
    }
    test_wire_codecs();
    test_crc_conventions_off_air();
    test_powersave_headers_back_to_back();
    test_end_second_copy_agreeing_reading_wins();
    test_header_both_polarities();
    test_header_copies();
    test_header_cut_by_an_interruption();
    test_fs1_fs4_alias();
    test_transmission_ends_on_its_end_frame();
    test_status_header_then_end_frame();
    test_unchecked_or_misplaced_end_frames();
    test_nxdn48_inside_a_rejected_candidate();
    test_frames_after_a_rejected_candidate();
    test_noise_accepts_no_header_or_end();
    free(g_state);
    free(g_opts);
    printf("FRAME_SYNC_DPMR: OK\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
