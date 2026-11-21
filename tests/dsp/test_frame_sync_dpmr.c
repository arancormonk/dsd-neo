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
/* Each stream symbol arrives at this times its level (1 unless a case fades it, fade()). */
static float g_gain[STREAM_MAX];

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
    float symbol = dibit_level(g_pos < g_len ? g_dibits[g_pos] : filler_dibit(g_pos));
    if (g_pos < STREAM_MAX) {
        symbol *= g_gain[g_pos];
    }
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
    for (int i = 0; i < STREAM_MAX; i++) {
        g_gain[i] = 1.0f;
    }
}

/* Stream symbols [@p from, @p to) arrive at @p gain times their level. */
static void
fade(int from, int to, float gain) {
    assert(from >= 0 && to <= STREAM_MAX);
    for (int i = from; i < to; i++) {
        g_gain[i] = gain;
    }
}

static void
put_dibits(const uint8_t* dibits, int count) {
    assert(g_len + count <= STREAM_MAX);
    DSD_MEMCPY(g_dibits + g_len, dibits, (size_t)count);
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

/* A clean header with channel code bits @p cc. */
static void
put_header_cc(const char* fs1, const uint8_t* info, uint32_t cc_bits) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    uint8_t mi[DPMR_SPEC_MI_WIRE];
    uint8_t cc[24];
    dpmr_spec_encode_mi(info, mi);
    dpmr_spec_put(cc, 0, 24, cc_bits);
    put_dibits(k_preamble, DPMR_SPEC_PREAMBLE_DIBITS);
    put_word(fs1);
    put_bits(mi, DPMR_SPEC_MI_WIRE);
    put_bits(cc, 24);
    put_bits(mi, DPMR_SPEC_MI_WIRE);
}

/* A clean header whose preamble the receiver caught only the last @p preamble dibits of (0 to 36). */
static void
put_header_clipped(const uint8_t* info, int preamble) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    uint8_t mi[DPMR_SPEC_MI_WIRE];
    uint8_t cc[24];
    assert(preamble >= 0 && preamble <= DPMR_SPEC_PREAMBLE_DIBITS);
    dpmr_spec_encode_mi(info, mi);
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U);
    put_dibits(k_preamble + (DPMR_SPEC_PREAMBLE_DIBITS - preamble), preamble);
    put_word(DPMR_FRAME_SYNC_1);
    put_bits(mi, DPMR_SPEC_MI_WIRE);
    put_bits(cc, 24);
    put_bits(mi, DPMR_SPEC_MI_WIRE);
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
    DSD_MEMSET(info17, 0, DPMR_SPEC_END_BITS);
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
        DSD_MEMSET(out, 0xAA, sizeof out);
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
    DSD_MEMCPY(other, info, sizeof other);
    other[71] ^= 1U;
    uint8_t wrong[DPMR_SPEC_MI_WIRE];
    dpmr_spec_encode_mi(other, wrong);
    uint8_t mixed[DPMR_SPEC_MI_WIRE];
    DSD_MEMCPY(mixed, wire, sizeof mixed);
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

    /* Headers of other message types an END follows too, and the hunt looks for it behind every checked header:
       a Status Polling Request (1000, Tables 10.7, 10.8, 10.15, 10.16), a BS_Access poll (1010, MI_TYPE 000,
       10.2.7.1.0 a), an ACK (0011). */
    static const uint32_t k_end_after[] = {0x8U, 0xAU, 0x3U};
    for (size_t m = 0; m < sizeof(k_end_after) / sizeof(k_end_after[0]); m++) {
        dpmr_spec_header poll = k_voice_header;
        poll.mt = k_end_after[m];
        dpmr_spec_header_bits(&poll, info);
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(80);
        put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
        put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
        const int poll_end = g_len;
        put_filler(200);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS3_POS);
        assert(g_pos == poll_end);
        free_state_buffers(g_state);
    }

    /* Table 5.3 reserves message type 1101: a header naming it is no header, CRC passing or not. */
    dpmr_spec_header reserved = k_voice_header;
    reserved.mt = 0xDU;
    dpmr_spec_header_bits(&reserved, info);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_header(DPMR_FRAME_SYNC_1, info, NULL, 0, 0);
    put_filler(200);
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

/* An end frame sent the 11.6 way (each copy from a fresh scrambler), its first copy lost and its second with wire bits 0
   and 14 in error: read from a fresh scrambler it corrects to what was sent (ET 01, status 21); read as the second half
   of one 72-bit run it passes its CRC-7 too, naming End_Type 00 with the reserved bits set, which 11.6 rules out
   ("the 4 reserved bits are set to 0000"). That reading is no END, so the fresh one is the frame's, with its status. */
static void
test_end_reading_with_reserved_fields_is_rejected(void) {
    uint8_t end[DPMR_SPEC_END_BITS];
    uint8_t wire[DPMR_SPEC_END_WIRE];
    uint8_t bad[DPMR_SPEC_END_WIRE];
    uint8_t out[DPMR_SPEC_END_BITS];
    end_info(end);
    dpmr_spec_encode_end(end, 0, wire);
    DSD_MEMCPY(bad, wire, sizeof bad);
    bad[0] ^= 1U;
    bad[14] ^= 1U;
    assert(frame_sync_dpmr_decode_end(bad, DPMR_SPEC_END_WIRE, out) == 0);
    assert(frame_sync_dpmr_decode_end(bad, 0, out) == 1 && memcmp(out, end, sizeof out) == 0);

    uint8_t lost[DPMR_SPEC_END_WIRE];
    DSD_MEMCPY(lost, wire, sizeof lost);
    lost[0] ^= 1U;
    lost[1] ^= 1U; /* a double error in Hamming word 0 */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_fs2_frame(DPMR_FRAME_SYNC_2);
    put_word(DPMR_FRAME_SYNC_3);
    put_bits(lost, DPMR_SPEC_END_WIRE);
    put_bits(bad, DPMR_SPEC_END_WIRE);
    const int end_at = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS2_POS);
    read_fs2_body(g_opts, g_state);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS3_POS);
    assert(g_pos == end_at);
    assert(g_state->dpmr_sync.checked_copies == DSD_DPMR_SYNC_COPY_1);
    assert(checked_bits_equal(g_state, end, DPMR_SPEC_END_BITS));
    free_state_buffers(g_state);

    /* End_Type 10 and ARQ 10 are reserved too (Tables 5.61, 5.48): such a copy is no END, its CRC passing or not. */
    for (int field = 0; field < 2; field++) {
        uint8_t reserved[DPMR_SPEC_END_BITS];
        DSD_MEMCPY(reserved, end, sizeof reserved);
        dpmr_spec_put(reserved, field * 2, 2, 2U);
        dpmr_spec_encode_end(reserved, 0, wire);
        assert(frame_sync_dpmr_decode_end(wire, 0, out) == 0);
    }
}

/* Whether the header just put at @p at (its FS1's first dibit) holds FS1's sign pattern in its payload. */
static int
header_payload_holds_fs1(int at) {
    const char* word = DPMR_FRAME_SYNC_1;
    for (int start = at + 24; start + 24 <= at + 24 + DSD_DPMR_HEADER_DIBITS; start++) {
        int same = 1;
        for (int i = 0; i < 24 && same; i++) {
            const char sign = g_dibits[start + i] <= 1U ? '1' : '3';
            same = sign == word[i];
        }
        if (same) {
            return 1;
        }
    }
    return 0;
}

/* A real header whose payload holds FS1's sign pattern (TS 102 658 6.1.1 gives FS1 no exclusion from the MI, so a
   clean header can carry it): a peer-to-peer Communications_Start from 4279584 to 8402415 (air interface IDs 5F963F
   and BBB522, A.1.2.1.1.6) at colour code 16 does, at payload dibit 40. The later word must not replace the header
   being read: the header passes and is accepted, the word inside it dropped as its payload. */
static void
test_header_payload_holding_fs1_is_kept(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    const dpmr_spec_header header = {
        .mt = 0U,
        .called = 0xBBB522U,
        .calling = 0x5F963FU,
        .mode = 0U,
        .version = 0U,
        .format = 1U,
    };
    dpmr_spec_header_bits(&header, info);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    const int header_at = g_len + DPMR_SPEC_PREAMBLE_DIBITS;
    put_header_cc(DPMR_FRAME_SYNC_1, info, 0x775DD7U);
    assert(header_payload_holds_fs1(header_at));
    const int header_end = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == header_end);
    assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
    free_state_buffers(g_state);

    /* A false FS1 just before it: three words read at once (the false one, the header's, its payload's), none given up
       before its own check; the header is the frame. */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_word(DPMR_FRAME_SYNC_1);
    const int real_at = g_len + DPMR_SPEC_PREAMBLE_DIBITS;
    put_header_cc(DPMR_FRAME_SYNC_1, info, 0x775DD7U);
    assert(header_payload_holds_fs1(real_at));
    const int real_end = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == real_end);
    assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
    free_state_buffers(g_state);
}

/* Every FS1 that can still be read when a header's FS1 comes is kept to its own check (issue #890). FS1 overlaps itself
   at no shift, so its words end at least 24 symbols apart, and one is read for 132 symbols after its end: up to six
   are read at once, the newest included. False words back to back before a header whose preamble was caught late (or
   not at all) leave four or five read when the header's FS1 ends; the header is still checked and accepted.
   A false word that ends 72 symbols before the header's FS1 frames the header's MI0 as its own MI1, which passes: that
   frame, 72 symbols early and with the end of the header's FS1 read as its channel code, is not the header. The header
   is accepted where it is, with its own channel code. */
static void
test_false_fs1_words_before_a_header(void) {
    static const struct {
        int false_words;
        int gap; /* filler symbols between the false words and the header */
        int preamble;
    } k_cases[] = {{4, 0, 32}, {5, 0, 32}, {4, 0, 0}, {5, 0, 0}, {1, 48, 0}, {1, 12, 36}, {2, 24, 0}};

    /* The premise of the bound: no shift of FS1 matches FS1. */
    const char* fs1 = DPMR_FRAME_SYNC_1;
    for (int shift = 1; shift < 24; shift++) {
        assert(strncmp(fs1 + shift, fs1, (size_t)(24 - shift)) != 0);
    }

    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t cc[24];
    dpmr_spec_header_bits(&k_voice_header, info);
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U); /* put_header_clipped()'s channel code */
    int failed = 0;
    for (size_t c = 0; c < sizeof(k_cases) / sizeof(k_cases[0]); c++) {
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(80);
        for (int w = 0; w < k_cases[c].false_words; w++) {
            put_word(DPMR_FRAME_SYNC_1);
        }
        put_filler(k_cases[c].gap);
        put_header_clipped(info, k_cases[c].preamble);
        const int header_end = g_len;
        put_filler(200);
        const int sync = hunt(g_opts, g_state, g_len);
        if (sync != DSD_SYNC_DPMR_FS1_POS || g_pos != header_end
            || !checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS)
            || memcmp(g_state->dpmr_sync.checked_cc, cc, sizeof cc) != 0) {
            DSD_FPRINTF(stderr, "%d false FS1 words, %d filler, %d preamble symbols: sync %d at %d, want %d at %d\n",
                        k_cases[c].false_words, k_cases[c].gap, k_cases[c].preamble, sync, g_pos, DSD_SYNC_DPMR_FS1_POS,
                        header_end);
            failed = 1;
        }
        free_state_buffers(g_state);
    }
    assert(!failed);
}

/* A checked header is left to a later FS1 only when that word can read a frame (issue #890). A header whose MI0 is
   damaged passes on MI1 alone; when the 24 symbols right before its MI1 (MI0's tail and the channel code) carry FS1's
   signs but faded too far to slice a frame on (DSD_WARM_START_MIN_SPAN), the later word's own check would reject it,
   so the header is the frame, accepted at its end with its MI1. */
static void
test_faded_fs1_sign_pattern_does_not_displace_a_header(void) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t mi[DPMR_SPEC_MI_WIRE];
    uint8_t cc[24];
    uint8_t mi0_cc[(DPMR_SPEC_MI_WIRE + 24) / 2];
    uint8_t fs1[24];
    dpmr_spec_header_bits(&k_voice_header, info);
    dpmr_spec_encode_mi(info, mi);
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U);
    dpmr_spec_dibits(mi, DPMR_SPEC_MI_WIRE, mi0_cc);
    dpmr_spec_dibits(cc, 24, mi0_cc + (DPMR_SPEC_MI_WIRE / 2));
    assert(dpmr_spec_word(DPMR_FRAME_SYNC_1, fs1) == 24);
    /* The last 24 symbols before MI1 carry FS1: MI0's tail (so MI0 fails) and the channel code. */
    DSD_MEMCPY(mi0_cc + sizeof(mi0_cc) - 24U, fs1, 24U);

    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_dibits(k_preamble, DPMR_SPEC_PREAMBLE_DIBITS);
    put_word(DPMR_FRAME_SYNC_1);
    put_dibits(mi0_cc, (int)sizeof(mi0_cc));
    fade(g_len - 24, g_len, 0.1f); /* outer levels +-0.3: a span of 0.6 */
    put_bits(mi, DPMR_SPEC_MI_WIRE);
    const int header_end = g_len;
    put_filler(200);
    const int sync = hunt(g_opts, g_state, g_len);
    if (sync != DSD_SYNC_DPMR_FS1_POS || g_pos != header_end) {
        DSD_FPRINTF(stderr, "faded FS1 signs before MI1: sync %d at %d, want %d at %d\n", sync, g_pos,
                    DSD_SYNC_DPMR_FS1_POS, header_end);
    }
    assert(sync == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == header_end);
    assert(g_state->dpmr_sync.checked_copies == DSD_DPMR_SYNC_COPY_1);
    assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
    free_state_buffers(g_state);
}

/* A header that passed on MI1 alone while a later FS1 that can read a frame is pending keeps its END window (issue
   #890): the 24 symbols before a status poll's MI1 carry FS1's signs at half level (MI0's tail, so MI0 fails, and the
   channel code), and its END follows at once. The END, checked where the header puts it, corroborates the header once
   the later candidate fails its two copies: the header is accepted with the END attached, both dispatched together,
   the header timed from its own frame. So is it when the input ends right after the END, before the later candidate
   could be decided: the END is not lost with the cut. */
static void
test_end_corroborates_a_header_a_later_fs1_shadows(void) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t end[DPMR_SPEC_END_BITS];
    uint8_t mi[DPMR_SPEC_MI_WIRE];
    uint8_t cc[24];
    uint8_t mi0_cc[(DPMR_SPEC_MI_WIRE + 24) / 2];
    uint8_t fs1[24];
    dpmr_spec_header poll = k_voice_header;
    poll.mt = 0x8U; /* a Status Polling Request, which an END follows (Tables 10.7, 10.8) */
    dpmr_spec_header_bits(&poll, info);
    end_info(end);
    dpmr_spec_encode_mi(info, mi);
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U);
    dpmr_spec_dibits(mi, DPMR_SPEC_MI_WIRE, mi0_cc);
    dpmr_spec_dibits(cc, 24, mi0_cc + (DPMR_SPEC_MI_WIRE / 2));
    assert(dpmr_spec_word(DPMR_FRAME_SYNC_1, fs1) == 24);
    DSD_MEMCPY(mi0_cc + sizeof(mi0_cc) - 24U, fs1, 24U);

    for (int cut = 0; cut < 2; cut++) {
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(80);
        put_dibits(k_preamble, DPMR_SPEC_PREAMBLE_DIBITS);
        put_word(DPMR_FRAME_SYNC_1);
        put_dibits(mi0_cc, (int)sizeof(mi0_cc));
        fade(g_len - 24, g_len, 0.5f); /* outer levels +-1.5: a span of 3, a word that can read a frame */
        put_bits(mi, DPMR_SPEC_MI_WIRE);
        const int header_end = g_len;
        put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
        const int end_at = g_len;
        put_filler(200);
        if (cut) {
            g_interrupt_at = end_at; /* the read after the END's last symbol: the input ends there */
        }
        const int sync = hunt(g_opts, g_state, g_len);
        const dsd_dpmr_frame_sync* sync_state = &g_state->dpmr_sync;
        if (sync != DSD_SYNC_DPMR_FS1_POS || sync_state->checked_end_attached != 1U) {
            DSD_FPRINTF(stderr, "shadowed header (cut %d): sync %d at %d end %u, want %d with its END (frame end %d)\n",
                        cut, sync, g_pos, (unsigned)sync_state->checked_end_attached, DSD_SYNC_DPMR_FS1_POS,
                        header_end);
        }
        assert(sync == DSD_SYNC_DPMR_FS1_POS);
        assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
        assert(sync_state->checked_copies == DSD_DPMR_SYNC_COPY_1);
        assert(sync_state->checked_frame_lag == (uint32_t)(g_state->symbol_clock - (uint64_t)header_end));
        assert(sync_state->checked_end_attached == 1U);
        assert(memcmp(sync_state->checked_end_bits, end, DPMR_SPEC_END_BITS) == 0);
        assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
        free_state_buffers(g_state);
    }
}

/* An END that overlaps a later header candidate still unresolved is not published before the candidate (issue #890).
   A false FS1 72 symbols before a voice header (called 1, calling 2, mode 0, format 1, TS 102 658 Table 5.4) frames
   the header's MI0 as its own MI1; one sign error in the header's channel code (colour code 28, 7D5FF5, Table 6.1)
   makes it FS3 where the false frame's END would be, and the start of MI1 reads there as an END copy that passes. Both
   of the header's MI copies pass, so the header is the frame: accepted at its end, with no END. */
static void
test_end_inside_a_later_header_waits_for_it(void) {
    static const uint8_t k_preamble[DPMR_SPEC_PREAMBLE_DIBITS] = {
        1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3, 1, 1, 3, 3,
    };
    const dpmr_spec_header voice = {.mt = 0U, .called = 1U, .calling = 2U, .mode = 0U, .version = 0U, .format = 1U};
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t mi[DPMR_SPEC_MI_WIRE];
    uint8_t cc_bits[24];
    uint8_t cc[12];
    dpmr_spec_header_bits(&voice, info);
    dpmr_spec_encode_mi(info, mi);
    dpmr_spec_put(cc_bits, 0, 24, 0x7D5FF5U);
    dpmr_spec_dibits(cc_bits, 24, cc);
    cc[4] ^= 2U; /* one sign error: the channel code now reads FS3 */
    char signs[13];
    for (int i = 0; i < 12; i++) {
        signs[i] = cc[i] <= 1U ? '1' : '3';
    }
    signs[12] = '\0';
    assert(strcmp(signs, DPMR_FRAME_SYNC_3) == 0);

    int failed = 0;
    for (int preamble = 0; preamble <= DPMR_SPEC_PREAMBLE_DIBITS; preamble += DPMR_SPEC_PREAMBLE_DIBITS) {
        setup(g_opts, g_state, MODE_DPMR, 0);
        stream_reset();
        put_filler(80);
        put_word(DPMR_FRAME_SYNC_1);
        put_filler(48 - preamble);
        put_dibits(k_preamble + (DPMR_SPEC_PREAMBLE_DIBITS - preamble), preamble);
        put_word(DPMR_FRAME_SYNC_1);
        put_bits(mi, DPMR_SPEC_MI_WIRE);
        put_dibits(cc, 12);
        put_bits(mi, DPMR_SPEC_MI_WIRE);
        const int header_end = g_len;
        put_filler(300);
        const int sync = hunt(g_opts, g_state, g_len);
        const unsigned copies = g_state->dpmr_sync.checked_copies;
        const unsigned end_attached = g_state->dpmr_sync.checked_end_attached;
        const int at = g_pos;
        const int next = hunt(g_opts, g_state, g_len);
        if (sync != DSD_SYNC_DPMR_FS1_POS || at != header_end || copies != (DSD_DPMR_SYNC_COPY_0 | DSD_DPMR_SYNC_COPY_1)
            || end_attached != 0U || next != DSD_SYNC_NONE) {
            DSD_FPRINTF(stderr,
                        "END inside a later header (%d preamble): sync %d at %d copies %u end %u next %d, want %d at "
                        "%d\n",
                        preamble, sync, at, copies, end_attached, next, DSD_SYNC_DPMR_FS1_POS, header_end);
            failed = 1;
        }
        free_state_buffers(g_state);
    }
    assert(!failed);
}

/* A later FS1 whose levels can read a frame is kept to its own check, MI1 included, before either frame is published
   (issue #890). A false FS1 72 symbols before a header's frames the header's MI0 as its own MI1; with the false word
   and the header's MI0 faded alike, that MI0 passes on the false word's levels but fails on the header's own FS1
   levels. The header still passes on its MI1, so it is the frame, accepted at its end with its own channel code, never
   the false word's frame 72 symbols early. */
static void
test_viable_later_fs1_is_checked_on_both_copies(void) {
    uint8_t info[DPMR_SPEC_MI_BITS];
    uint8_t cc[24];
    dpmr_spec_header_bits(&k_voice_header, info);
    dpmr_spec_put(cc, 0, 24, 0x5D5D55U); /* put_header_clipped()'s channel code */
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    const int false_at = g_len;
    put_word(DPMR_FRAME_SYNC_1);
    fade(false_at, g_len, 0.5f);
    put_filler(12);
    const int header_at = g_len;
    put_header_clipped(info, 36);
    const int header_end = g_len;
    const int mi0_at = header_at + 36 + 24;
    fade(mi0_at, mi0_at + (DPMR_SPEC_MI_WIRE / 2), 0.5f);
    put_filler(200);
    const int sync = hunt(g_opts, g_state, g_len);
    if (sync != DSD_SYNC_DPMR_FS1_POS || g_pos != header_end) {
        DSD_FPRINTF(stderr, "viable later FS1: sync %d at %d, want %d at %d\n", sync, g_pos, DSD_SYNC_DPMR_FS1_POS,
                    header_end);
    }
    assert(sync == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == header_end);
    assert(checked_bits_equal(g_state, info, DPMR_SPEC_MI_BITS));
    assert(memcmp(g_state->dpmr_sync.checked_cc, cc, sizeof cc) == 0);
    free_state_buffers(g_state);
}

/* A Communications_Start copy in the reserved Communications Mode 111 (TS 102 658 Table 5.55) is no copy, whatever its
   CRC: beside a good copy it is no contradiction (the good one is the header), and alone it is no header, so no FS3 is
   looked for behind it. */
static void
test_reserved_start_mode_copy_is_no_copy(void) {
    uint8_t good[DPMR_SPEC_MI_BITS];
    uint8_t reserved[DPMR_SPEC_MI_BITS];
    uint8_t end[DPMR_SPEC_END_BITS];
    dpmr_spec_header bad = k_voice_header;
    bad.mode = 7U;
    dpmr_spec_header_bits(&k_voice_header, good);
    dpmr_spec_header_bits(&bad, reserved);
    end_info(end);
    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_header(DPMR_FRAME_SYNC_1, reserved, good, 0, 0);
    const int header_end = g_len;
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_DPMR_FS1_POS);
    assert(g_pos == header_end);
    assert(g_state->dpmr_sync.checked_copies == DSD_DPMR_SYNC_COPY_1);
    assert(checked_bits_equal(g_state, good, DPMR_SPEC_MI_BITS));
    free_state_buffers(g_state);

    setup(g_opts, g_state, MODE_DPMR, 0);
    stream_reset();
    put_filler(80);
    put_header(DPMR_FRAME_SYNC_1, reserved, NULL, 0, 0);
    put_end(DPMR_FRAME_SYNC_3, end, END_ONE_RUN, 0, 0);
    put_filler(200);
    assert(hunt(g_opts, g_state, g_len) == DSD_SYNC_NONE);
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
    test_end_reading_with_reserved_fields_is_rejected();
    test_header_payload_holding_fs1_is_kept();
    test_false_fs1_words_before_a_header();
    test_faded_fs1_sign_pattern_does_not_displace_a_header();
    test_viable_later_fs1_is_checked_on_both_copies();
    test_end_corroborates_a_header_a_later_fs1_shadows();
    test_end_inside_a_later_header_waits_for_it();
    test_reserved_start_mode_copy_is_no_copy();
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
