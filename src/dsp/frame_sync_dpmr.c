// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dPMR headers and end frames, read inside the hunt (dsd_dpmr_frame_sync).
 *
 * FS1 (24 symbols) opens a Message_Frame: MI0 (120 bits), the channel code (24) and MI1 (120), 132 dibits (ETSI TS 102
 * 658 V2.6.1 4.2.2.1, Table 5.2). FS3 (12 symbols) opens an End_Frame: two 36-bit END copies (4.2.2.4, Table 5.44).
 * Neither word is enough on its own: FS3 is as short as FS2, which noise and other traffic pass about once in 4096
 * windows, and the 2400/4 profile is shared with NXDN48, whose matcher runs after this one. So a word only marks a
 * candidate. The hunt goes on over the candidate's frame as over any other symbols, every matcher with it, and once the
 * frame's last symbol is in, the frame is read back from the soft symbol history, sliced on the levels of its own
 * sync word, and checked. Only a frame that passes is accepted; one that fails has cost nothing, since no symbol was
 * taken from the hunt (an NXDN48 frame inside it is matched as it would have been). FS3 is looked for only where an
 * end frame can be: one FS2 frame after an accepted FS2, and straight after any checked header (an unsolicited status,
 * 4.1.5.1.2 b and Table 5.3 note 2, a request, a poll: many are a header and an end frame).
 *
 * The wire coding is the specification's, receive direction:
 *  - MI (7.6, 11.5, Figure 11.8): 72 information bits, CRC-8 (X^8 + X^2 + X + 1, 7.1) to 80, ten Hamming(12,8) words
 *    (7.2) to 120, the 12 x 10 MI interleave (7.4, Table 7.4), then the X^9 + X^5 + 1 scrambler from all ones (7.3).
 *    The two copies are the same bits (the scrambled MI is repeated after the channel code), so each is descrambled
 *    from a fresh scrambler, in wire order, before it is deinterleaved.
 *  - END (7.7, 11.6, Figure 11.9): 17 information bits, CRC-7 (X^7 + X^3 + 1) to 24, three Hamming(12,8) words, no
 *    interleave, two copies, scrambled. The specification contradicts itself on how: 7.7 has "the total 72 bits"
 *    scrambled as one run (the second copy takes keystream bits 36 to 71), 11.6 has "for each scrambler block the
 *    scrambler is re-initialized therefore the two scrambled END DATA blocks are bit exact copies". The off-air
 *    sigidwiki recording behind tests/fixtures/iq/dpmr.iq (DPMR.zip, SDRSharp_20160101_231640Z_12kHz_IQ.wav, inverted
 *    polarity) sends one run. The first copy is the same either way; the second is read both ways, each CRC-checked.
 * A copy passes when all its Hamming words are correctable and its CRC matches. One copy passing is enough; two that
 * pass with different contents are no frame. The specification gives the CRC polynomials but not the register's
 * preset or bit order: both CRCs here run MSB first from a zero register, with no final inversion. That recording
 * confirms it: all 18 MI copies of its 9 headers pass the CRC-8 so (a preset of all ones fails every one), and its end
 * frame's first copy passes the CRC-7 so, as do the CCH halves (as also in the DSDcc-derived CCH reference vectors,
 * tests/protocol/dpmr/fixtures).
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"

enum {
    DPMR_MI_WIRE_BITS = 120,
    DPMR_MI_CODED_BITS = 80,
    DPMR_END_WIRE_BITS = 36,
    DPMR_END_CODED_BITS = 24,
    DPMR_FS1_SYMBOLS = 24,
    DPMR_FS3_SYMBOLS = 12,
    /* Where in the header's 264 bits MI0, the channel code and MI1 lie. */
    DPMR_HEADER_CC_AT = DPMR_MI_WIRE_BITS,
    DPMR_HEADER_MI1_AT = DPMR_MI_WIRE_BITS + DSD_DPMR_HEADER_CC_BITS,
    /* How many symbols either side of an FS3 boundary its word may end (DPMR_CONFIRM_FRAME_SYMBOLS_SLIP's one). */
    DPMR_FS3_SLIP = 1,
    /* How many symbols after a header's FS1 its MI1 begins: MI0 (60) and the channel code (12). */
    DPMR_HEADER_MI1_SYMBOLS = DPMR_HEADER_MI1_AT / 2,
};

/* Every FS1 still being read when another comes keeps a slot to its own check: FS1 overlaps itself at no shift, so
   two words end DPMR_FS1_SYMBOLS apart at least, and a word is read for DSD_DPMR_HEADER_DIBITS symbols after its end
   (the one whose frame ends at a symbol is checked before that symbol's word is marked). */
_Static_assert(DSD_DPMR_PENDING_MAX >= ((DSD_DPMR_HEADER_DIBITS - 1) / DPMR_FS1_SYMBOLS) + 1,
               "every FS1 read at once needs its own candidate slot");

/* The Hamming(12,8) parity of each information bit, X7 first: its row of the generator matrix's C3..C0 columns (TS 102
 * 658 Table 7.2), C3 the most significant. A single bit error's syndrome is its column, so these and 8, 4, 2, 1 for
 * the parity bits are the twelve correctable syndromes; 9, 13 and 15 are not correctable. */
static const uint8_t k_dpmr_hamming_parity[8] = {0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3};

void
frame_sync_dpmr_keystream(uint8_t* out, int count) {
    /* X^9 + X^5 + 1 with an initial preset of all ones (7.3): bit i of the register is stage S(i + 1). */
    uint32_t reg = 0x1FFU;
    for (int i = 0; i < count; i++) {
        out[i] = (uint8_t)(reg & 1U);
        const uint32_t feedback = (reg ^ (reg >> 4)) & 1U;
        reg = (reg >> 1) | (feedback << 8);
    }
}

/* CRC of @p count bits MSB first, from a zero register, no final inversion: @p width bits, polynomial @p poly without
 * its X^width term. */
static uint32_t
dpmr_crc(const uint8_t* bits, int count, uint32_t poly, int width) {
    const uint32_t mask = (1U << width) - 1U;
    uint32_t reg = 0U;
    for (int i = 0; i < count; i++) {
        const uint32_t feedback = ((reg >> (width - 1)) ^ bits[i]) & 1U;
        reg = (reg << 1) & mask;
        if (feedback != 0U) {
            reg ^= poly;
        }
    }
    return reg;
}

static uint32_t
dpmr_bits_value(const uint8_t* bits, int count) {
    uint32_t value = 0U;
    for (int i = 0; i < count; i++) {
        value = (value << 1) | (bits[i] & 1U);
    }
    return value;
}

/* Decode the Hamming(12,8) word @p word (X7 first) into @p out (8 bits): 0 when its syndrome is not correctable. */
static int
dpmr_hamming_12_8(const uint8_t* word, uint8_t* out) {
    uint32_t syndrome = dpmr_bits_value(word + 8, 4);
    for (int i = 0; i < 8; i++) {
        if (word[i] != 0U) {
            syndrome ^= k_dpmr_hamming_parity[i];
        }
    }
    DSD_MEMCPY(out, word, 8U);
    if (syndrome == 0U) {
        return 1;
    }
    for (int i = 0; i < 8; i++) {
        if (k_dpmr_hamming_parity[i] == syndrome) {
            out[i] ^= 1U;
            return 1;
        }
    }
    /* A parity bit in error leaves the information bits as they are; anything else is two or more errors. */
    return syndrome == 8U || syndrome == 4U || syndrome == 2U || syndrome == 1U;
}

/* Descramble @p count wire bits into @p out with the scrambler's bits from @p key_offset on, from its preset. */
static void
dpmr_descramble(const uint8_t* wire, uint8_t* out, int count, int key_offset) {
    uint8_t key[DPMR_MI_WIRE_BITS];
    frame_sync_dpmr_keystream(key, key_offset + count);
    for (int i = 0; i < count; i++) {
        out[i] = (uint8_t)((wire[i] ^ key[key_offset + i]) & 1U);
    }
}

int
frame_sync_dpmr_decode_mi(const uint8_t* wire, uint8_t* info) {
    uint8_t clear[DPMR_MI_WIRE_BITS];
    uint8_t coded[DPMR_MI_WIRE_BITS];
    uint8_t data[DPMR_MI_CODED_BITS];
    dpmr_descramble(wire, clear, DPMR_MI_WIRE_BITS, 0);
    /* The transmitter writes the coded bits down the 12 rows column by column and sends the matrix row by row
       (Table 7.4): coded bit 12c + r goes out at 10r + c. */
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 10; c++) {
            coded[(c * 12) + r] = clear[(r * 10) + c];
        }
    }
    int correctable = 1;
    for (int w = 0; w < 10; w++) {
        correctable &= dpmr_hamming_12_8(coded + ((size_t)w * 12U), data + ((size_t)w * 8U));
    }
    if (!correctable
        || dpmr_crc(data, DSD_DPMR_HEADER_MI_BITS, 0x07U, 8)
               != dpmr_bits_value(data + DSD_DPMR_HEADER_MI_BITS, DPMR_MI_CODED_BITS - DSD_DPMR_HEADER_MI_BITS)) {
        return 0;
    }
    /* Table 5.3 reserves message type 1101, and Table 5.55 Communications Mode 111 in the headers whose M field
       describes the payload (Communications_Start, the ACK to a System Request and System Delivery: Tables 5.4, 5.22,
       5.23): a copy naming either is no message, whatever its CRC, so it neither stands for the frame nor contradicts
       the other copy. */
    const uint32_t mt = dpmr_bits_value(data, 4);
    const uint32_t mode = dpmr_bits_value(data + 52, 3);
    if (mt == 0xDU || ((mt == 0x0U || mt == 0x5U || mt == 0x6U) && mode == 7U)) {
        return 0;
    }
    DSD_MEMCPY(info, data, DSD_DPMR_HEADER_MI_BITS);
    return 1;
}

int
frame_sync_dpmr_decode_end(const uint8_t* wire, int key_offset, uint8_t* info) {
    uint8_t coded[DPMR_END_WIRE_BITS];
    uint8_t data[DPMR_END_CODED_BITS];
    if (key_offset < 0 || key_offset > DPMR_END_WIRE_BITS) {
        return 0;
    }
    dpmr_descramble(wire, coded, DPMR_END_WIRE_BITS, key_offset);
    int correctable = 1;
    for (int w = 0; w < 3; w++) {
        correctable &= dpmr_hamming_12_8(coded + ((size_t)w * 12U), data + ((size_t)w * 8U));
    }
    if (!correctable
        || dpmr_crc(data, DSD_DPMR_END_INFO_BITS, 0x09U, 7)
               != dpmr_bits_value(data + DSD_DPMR_END_INFO_BITS, DPMR_END_CODED_BITS - DSD_DPMR_END_INFO_BITS)) {
        return 0;
    }
    /* A reading no transmitter sends is no END, whatever its CRC: End_Type 10 and 11 and ARQ 10 and 11 are reserved
       (Tables 5.61, 5.48), and the four reserved bits are 0000 (11.6). */
    if (dpmr_bits_value(data, 2) > 1U || dpmr_bits_value(data + 2, 2) > 1U || dpmr_bits_value(data + 13, 4) != 0U) {
        return 0;
    }
    DSD_MEMCPY(info, data, DSD_DPMR_END_INFO_BITS);
    return 1;
}

/* Which of two copies of @p len information bits to take: 0 when neither passed or both did with different contents,
 * else the DSD_DPMR_SYNC_COPY_* bits of those that passed, with the bits in @p out. */
static uint8_t
dpmr_pick_copy(int ok0, const uint8_t* info0, int ok1, const uint8_t* info1, int len, uint8_t* out) {
    if (ok0 && ok1 && memcmp(info0, info1, (size_t)len) != 0) {
        return 0U;
    }
    if (!ok0 && !ok1) {
        return 0U;
    }
    DSD_MEMCPY(out, ok0 ? info0 : info1, (size_t)len);
    return (uint8_t)((ok0 ? DSD_DPMR_SYNC_COPY_0 : 0U) | (ok1 ? DSD_DPMR_SYNC_COPY_1 : 0U));
}

static void
dpmr_mark_here(dsd_dpmr_sync_mark* mark, const dsd_state* state, uint64_t clock, uint8_t kind, uint8_t inverted) {
    mark->clock = clock;
    mark->epoch = state->symbol_clock_epoch;
    mark->input = state->symbol_clock_input;
    mark->carrier_seq = state->carrier_seq;
    mark->sps_hunt_idx = state->sps_hunt_idx;
    mark->kind = kind;
    mark->inverted = inverted;
}

/* Whether @p mark was taken on the stream being read, under the polarity configured now. */
static int
dpmr_mark_stream_holds(const dsd_dpmr_sync_mark* mark, const dsd_opts* opts, const dsd_state* state) {
    return mark->epoch == state->symbol_clock_epoch && mark->input == state->symbol_clock_input
           && mark->carrier_seq == state->carrier_seq && mark->sps_hunt_idx == state->sps_hunt_idx
           && mark->inverted == (uint8_t)(opts->inverted_dpmr == 1) && state->input_interrupted == 0;
}

static const char*
dpmr_received_word(uint8_t kind, uint8_t inverted) {
    if (kind == DSD_DPMR_SYNC_FRAME_HEADER) {
        return inverted ? INV_DPMR_FRAME_SYNC_1 : DPMR_FRAME_SYNC_1;
    }
    return inverted ? INV_DPMR_FRAME_SYNC_3 : DPMR_FRAME_SYNC_3;
}

typedef struct {
    float center;
    float umid;
    float lmid;
} dpmr_levels;

/* The slicer levels of a frame read back from the soft symbol history: the outer levels of its sync word, whose
 * @p length symbols end @p back symbols before the newest and are all outer (6.1: FS1 and FS3 are 1s and 3s, '1' the
 * received +3). 0 for a word too flat to read a frame on (DSD_WARM_START_MIN_SPAN). */
static int
dpmr_word_levels(const dsd_state* state, const char* word, int length, int back, dpmr_levels* out) {
    float pos = 0.0f;
    float neg = 0.0f;
    int pos_n = 0;
    int neg_n = 0;
    for (int i = 0; i < length; i++) {
        const float x = dsd_symbol_history_get_back(state, back + (length - 1 - i));
        if (word[i] == '1') {
            pos += x;
            pos_n++;
        } else {
            neg += x;
            neg_n++;
        }
    }
    if (pos_n == 0 || neg_n == 0) {
        return 0;
    }
    pos /= (float)pos_n;
    neg /= (float)neg_n;
    if (!(pos - neg >= DSD_WARM_START_MIN_SPAN)) {
        return 0;
    }
    out->center = (pos + neg) * 0.5f;
    out->umid = out->center + ((pos - out->center) * DSD_WARM_START_MID_FRACTION);
    out->lmid = out->center + ((neg - out->center) * DSD_WARM_START_MID_FRACTION);
    return 1;
}

/* The @p dibits dibits ending on the newest symbol as @p bits (two a dibit, the high bit first), sliced on @p levels:
 * +3 1, +1 0, -1 2, -3 3 as received, the sign bit flipped when the configured polarity is inverted, as the dPMR
 * decoder's reads do (dpmr_read_dibit()). */
static void
dpmr_read_back_bits(const dsd_state* state, const dpmr_levels* levels, uint8_t inverted, int dibits, uint8_t* bits) {
    for (int i = 0; i < dibits; i++) {
        const float x = dsd_symbol_history_get_back(state, dibits - 1 - i);
        /* +3, +1, -1, -3 as received: the sign bit, then the outer bit. */
        uint8_t d = 3U;
        if (x >= levels->umid) {
            d = 1U;
        } else if (x >= levels->center) {
            d = 0U;
        } else if (x >= levels->lmid) {
            d = 2U;
        }
        if (inverted) {
            d ^= 2U;
        }
        bits[(size_t)i * 2U] = (uint8_t)((d >> 1) & 1U);
        bits[((size_t)i * 2U) + 1U] = (uint8_t)(d & 1U);
    }
}

/* Whether another FS1 being read ends where @p header's MI1 begins (@p header's own word end plus 72 symbols) and can
 * read a frame. Its MI0 is then the bits @p header read as MI1: a header behind one or more false FS1 words (the last
 * of them 72 symbols before its own) passes in that false word's frame on its MI0 alone, read as MI1, with the false
 * word's frame misplaced 72 symbols early and the end of the header's FS1 taken as its channel code. A pending word is
 * a sign pattern only, so it stands for the header when its own levels can read a frame (dpmr_word_levels(), as its
 * own check will); its two copies are checked at its own frame's end, MI1 included, which has not been received yet:
 * a reading of its MI0 alone cannot rule it out, as a faded MI0 can pass on @p header's levels and fail on its own.
 * With a word that cannot read a frame, @p header is the frame. */
static int
dpmr_mi1_is_a_later_headers_mi0(const dsd_state* state, const dsd_dpmr_sync_mark* header) {
    const dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    for (int i = 0; i < DSD_DPMR_PENDING_MAX; i++) {
        const dsd_dpmr_sync_mark* other = &sync->pending[i];
        if (other == header || other->kind != DSD_DPMR_SYNC_FRAME_HEADER
            || other->clock != header->clock + DPMR_HEADER_MI1_SYMBOLS || other->epoch != header->epoch
            || other->input != header->input || other->carrier_seq != header->carrier_seq
            || other->sps_hunt_idx != header->sps_hunt_idx || other->inverted != header->inverted
            || state->symbol_clock < other->clock) {
            continue;
        }
        /* This runs on @p header's last symbol, 132 after its word: the later word ends 60 symbols back. */
        dpmr_levels levels;
        if (dpmr_word_levels(state, dpmr_received_word(DSD_DPMR_SYNC_FRAME_HEADER, other->inverted), DPMR_FS1_SYMBOLS,
                             (int)(state->symbol_clock - other->clock), &levels)) {
            return 1;
        }
    }
    return 0;
}

static int
dpmr_check_header(dsd_state* state, const dsd_dpmr_sync_mark* header, const dpmr_levels* levels) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    const uint8_t inverted = header->inverted;
    uint8_t bits[DSD_DPMR_HEADER_DIBITS * 2];
    uint8_t info0[DSD_DPMR_HEADER_MI_BITS];
    uint8_t info1[DSD_DPMR_HEADER_MI_BITS];
    uint8_t info[DSD_DPMR_HEADER_MI_BITS];
    dpmr_read_back_bits(state, levels, inverted, DSD_DPMR_HEADER_DIBITS, bits);
    const int ok0 = frame_sync_dpmr_decode_mi(bits, info0);
    const int ok1 = frame_sync_dpmr_decode_mi(bits + DPMR_HEADER_MI1_AT, info1);
    const uint8_t copies = dpmr_pick_copy(ok0, info0, ok1, info1, DSD_DPMR_HEADER_MI_BITS, info);
    if (copies == 0U) {
        return DSD_SYNC_NONE;
    }
    /* A frame that passed on MI1 alone, where another FS1 that can read a frame ends right before it, is that later
       header's MI0: the later candidate reads it in place, both copies and the channel code where they are, at its own
       frame's end. */
    if (copies == DSD_DPMR_SYNC_COPY_1 && dpmr_mi1_is_a_later_headers_mi0(state, header)) {
        return DSD_SYNC_NONE;
    }
    DSD_MEMCPY(sync->checked_bits, info, DSD_DPMR_HEADER_MI_BITS);
    DSD_MEMCPY(sync->checked_cc, bits + DPMR_HEADER_CC_AT, DSD_DPMR_HEADER_CC_BITS);
    sync->checked_kind = DSD_DPMR_SYNC_FRAME_HEADER;
    sync->checked_copies = copies;
    sync->checked_clock = state->symbol_clock;
    sync->checked_epoch = state->symbol_clock_epoch;
    /* An END frame may follow any header at once, its FS3 twelve symbols from here: Table 5.3 names several (an
       unsolicited status, note 2; Connection_Request, Disconnect_Request, System_Request, Status Polling Response), and
       the procedures send others as header and END pairs too (4.2.3.2 to 4.2.3.5; a status poll, Tables 10.7, 10.8,
       10.15, 10.16; a BS_Access poll, 10.2.7.1.0 a). Looking for one costs a compare at one window, and only an END that
       passes its own check is taken, so every checked header arms it. */
    dpmr_mark_here(&sync->fs3_at, state, state->symbol_clock + DPMR_FS3_SYMBOLS, DSD_DPMR_SYNC_FRAME_END, inverted);
    return inverted ? DSD_SYNC_DPMR_FS1_NEG : DSD_SYNC_DPMR_FS1_POS;
}

/* The second END copy, read both ways: as one 72-bit run's second half (7.7, and the off-air recording) and from a
 * fresh scrambler (11.6). Whichever passes and agrees with a passing first copy @p info0 is taken, so a reading that
 * passes by chance does not turn the frame into two copies that disagree; else the first that passes. */
static int
dpmr_read_second_end(const uint8_t* wire, int ok0, const uint8_t* info0, uint8_t* info1) {
    uint8_t run[DSD_DPMR_END_INFO_BITS];
    uint8_t fresh[DSD_DPMR_END_INFO_BITS];
    const int ok_run = frame_sync_dpmr_decode_end(wire, DPMR_END_WIRE_BITS, run);
    const int ok_fresh = frame_sync_dpmr_decode_end(wire, 0, fresh);
    /* The fresh reading is taken over the run's only when it agrees with the first copy, or the run's does not pass. */
    const int fresh_agrees = ok0 && ok_fresh && memcmp(fresh, info0, sizeof fresh) == 0;
    if (!ok_run && !ok_fresh) {
        return 0;
    }
    const uint8_t* take = (fresh_agrees || !ok_run) ? fresh : run;
    DSD_MEMCPY(info1, take, DSD_DPMR_END_INFO_BITS);
    return 1;
}

static int
dpmr_check_end(dsd_state* state, const dpmr_levels* levels, uint8_t inverted) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    uint8_t bits[DSD_DPMR_END_DIBITS * 2];
    uint8_t info0[DSD_DPMR_END_INFO_BITS];
    uint8_t info1[DSD_DPMR_END_INFO_BITS];
    dpmr_read_back_bits(state, levels, inverted, DSD_DPMR_END_DIBITS, bits);
    const int ok0 = frame_sync_dpmr_decode_end(bits, 0, info0);
    const int ok1 = dpmr_read_second_end(bits + DPMR_END_WIRE_BITS, ok0, info0, info1);
    const uint8_t copies = dpmr_pick_copy(ok0, info0, ok1, info1, DSD_DPMR_END_INFO_BITS, sync->checked_bits);
    if (copies == 0U) {
        return DSD_SYNC_NONE;
    }
    sync->checked_kind = DSD_DPMR_SYNC_FRAME_END;
    sync->checked_copies = copies;
    sync->checked_clock = state->symbol_clock;
    sync->checked_epoch = state->symbol_clock_epoch;
    return inverted ? DSD_SYNC_DPMR_FS3_NEG : DSD_SYNC_DPMR_FS3_POS;
}

static int
dpmr_complete_candidate(const dsd_opts* opts, dsd_state* state, dsd_dpmr_sync_mark* pending) {
    if (pending->kind == DSD_DPMR_SYNC_FRAME_NONE) {
        return DSD_SYNC_NONE;
    }
    const int header = pending->kind == DSD_DPMR_SYNC_FRAME_HEADER;
    const uint64_t frame = header ? DSD_DPMR_HEADER_DIBITS : DSD_DPMR_END_DIBITS;
    const int word = header ? DPMR_FS1_SYMBOLS : DPMR_FS3_SYMBOLS;
    if (!dpmr_mark_stream_holds(pending, opts, state) || state->symbol_clock < pending->clock
        || state->symbol_clock > pending->clock + frame) {
        pending->kind = DSD_DPMR_SYNC_FRAME_NONE;
        return DSD_SYNC_NONE;
    }
    if (state->symbol_clock < pending->clock + frame) {
        return DSD_SYNC_NONE; /* still reading */
    }
    /* The candidate is done whatever its check says; the mark stays readable for it (its kind cleared). */
    const dsd_dpmr_sync_mark done = *pending;
    pending->kind = DSD_DPMR_SYNC_FRAME_NONE;
    /* The frame and its word are the carrier's and still in the soft history. */
    dpmr_levels levels;
    if (dsd_state_carrier_settling(state, frame + (uint64_t)word) || dsd_symbol_history_count(state) < (int)frame + word
        || !dpmr_word_levels(
            state, dpmr_received_word(header ? DSD_DPMR_SYNC_FRAME_HEADER : DSD_DPMR_SYNC_FRAME_END, pending->inverted),
            word, (int)frame, &levels)) {
        return DSD_SYNC_NONE;
    }
    return header ? dpmr_check_header(state, &done, &levels) : dpmr_check_end(state, &levels, done.inverted);
}

int
frame_sync_dpmr_candidate_complete(const dsd_opts* opts, dsd_state* state) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    /* Candidates began at different symbols, so at most one reaches its frame's last symbol here. */
    for (int i = 0; i < DSD_DPMR_PENDING_MAX; i++) {
        const int synctype = dpmr_complete_candidate(opts, state, &sync->pending[i]);
        if (synctype != DSD_SYNC_NONE) {
            /* Every other candidate began inside the frame just accepted (none ends before it and every one began
               before now): its word was that frame's payload. */
            frame_sync_dpmr_forget_candidates(state);
            return synctype;
        }
    }
    return DSD_SYNC_NONE;
}

int
frame_sync_dpmr_candidate_reading(const dsd_state* state) {
    for (int i = 0; i < DSD_DPMR_PENDING_MAX; i++) {
        if (state->dpmr_sync.pending[i].kind != DSD_DPMR_SYNC_FRAME_NONE) {
            return 1;
        }
    }
    return 0;
}

/* A free candidate slot, or NULL when every one is being read. */
static dsd_dpmr_sync_mark*
dpmr_free_candidate(dsd_state* state) {
    for (int i = 0; i < DSD_DPMR_PENDING_MAX; i++) {
        if (state->dpmr_sync.pending[i].kind == DSD_DPMR_SYNC_FRAME_NONE) {
            return &state->dpmr_sync.pending[i];
        }
    }
    return NULL;
}

int
frame_sync_dpmr_arm_header(const dsd_opts* opts, dsd_state* state, const char* window24) {
    const uint8_t inverted = (uint8_t)(opts->inverted_dpmr == 1);
    /* The configured polarity alone: FS4 is FS1's symbol-wise complement (6.1.4 NOTE), so the other polarity's FS1 is
       this one's FS4, a packet data header, which this decoder does not read. */
    if (strcmp(window24, dpmr_received_word(DSD_DPMR_SYNC_FRAME_HEADER, inverted)) != 0) {
        return 0;
    }
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    /* A candidate being read is kept until its own check: a real header's payload can hold FS1's sign pattern (a
       peer-to-peer Communications_Start does) and false FS1 words can come just before a real one, so a later word is
       read beside the others, not instead of any. The one that passes is accepted and the rest, which began inside it,
       dropped. Every word that can be read at once has a slot (DSD_DPMR_PENDING_MAX); were none free, the newest word
       would wait, the older ones being owed their checks. */
    dsd_dpmr_sync_mark* slot = dpmr_free_candidate(state);
    if (slot == NULL) {
        return 0;
    }
    dpmr_mark_here(slot, state, state->symbol_clock, DSD_DPMR_SYNC_FRAME_HEADER, inverted);
    sync->fs3_at.kind = DSD_DPMR_SYNC_FRAME_NONE;
    return 1;
}

int
frame_sync_dpmr_arm_end(const dsd_opts* opts, dsd_state* state, const char* window12) {
    dsd_dpmr_sync_mark* at = &state->dpmr_sync.fs3_at;
    if (at->kind != DSD_DPMR_SYNC_FRAME_END) {
        return 0;
    }
    if (!dpmr_mark_stream_holds(at, opts, state) || state->symbol_clock > at->clock + DPMR_FS3_SLIP) {
        at->kind = DSD_DPMR_SYNC_FRAME_NONE;
        return 0;
    }
    if (state->symbol_clock + DPMR_FS3_SLIP < at->clock
        || strcmp(window12, dpmr_received_word(DSD_DPMR_SYNC_FRAME_END, at->inverted)) != 0) {
        return 0;
    }
    at->kind = DSD_DPMR_SYNC_FRAME_NONE;
    dpmr_mark_here(&state->dpmr_sync.pending[0], state, state->symbol_clock, DSD_DPMR_SYNC_FRAME_END,
                   (uint8_t)(opts->inverted_dpmr == 1));
    return 1;
}

void
frame_sync_dpmr_note_fs2(const dsd_opts* opts, dsd_state* state) {
    /* The next frame's word ends one FS2 frame on: an FS2, or the FS3 of the end frame after the last superframe. */
    dpmr_mark_here(&state->dpmr_sync.fs3_at, state, state->symbol_clock + DSD_FRAME_SYNC_DPMR_FS2_FRAME_SYMBOLS,
                   DSD_DPMR_SYNC_FRAME_END, (uint8_t)(opts->inverted_dpmr == 1));
}

void
frame_sync_dpmr_forget(dsd_state* state) {
    frame_sync_dpmr_forget_candidates(state);
    state->dpmr_sync.fs3_at.kind = DSD_DPMR_SYNC_FRAME_NONE;
}

void
frame_sync_dpmr_forget_candidates(dsd_state* state) {
    for (int i = 0; i < DSD_DPMR_PENDING_MAX; i++) {
        state->dpmr_sync.pending[i].kind = DSD_DPMR_SYNC_FRAME_NONE;
    }
}
