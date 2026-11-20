// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dstar_confirm.h"

/*
 * The voice stream behind a sync (JARL STD 6.0 section 4.1.2): frames of 72 voice symbols and 24 data symbols, one bit
 * a symbol, the bits as the slicer reads them under the sync's polarity (digitize()). Every 21st data frame carries the
 * resynchronization word; after an RF header the stream is voice frame 0 and then that word. The 48-bit end pattern
 * closes the transmission.
 */
enum {
    DSTAR_VOICE_SYMBOLS = 72,
    DSTAR_DATA_SYMBOLS = 24,
    DSTAR_SUPERFRAME_FRAMES = 21, /* voice frames 1-20, each with its data frame, then voice frame 0 of the next */
    DSTAR_END_BITS = 48,
};

/* JARL STD 6.0 section 4.1.2 (d): 10 bits of bit synchronization and the 7-bit M-sequence "1101000" twice. */
#define DSTAR_RESYNC_WORD 0xAAB468U
/* JARL STD 6.0 section 4.1.2 (g)-(h): 32 bits of bit synchronization, "000100110101111" and a 0. DSDcc's termination
   sequence and MMDVM's END_SYNC_DATA are the same 48 bits (55 55 55 55 C8 7A in the bit order of the air). */
#define DSTAR_END_PATTERN UINT64_C(0xAAAAAAAA135E)
#define DSTAR_END_MASK    UINT64_C(0xFFFFFFFFFFFF)

/*
 * The last bits read of the transmission's frames, for the end pattern: newest in bit 0, @c count of them (at most
 * 48) read since the header or the sync the frames follow.
 *
 * The pattern is matched exactly, on the 48 bits that end on a boundary of the frames' 24-symbol grid (a voice frame is
 * three such steps, a data frame one). Transmitters do not agree on where in the grid it starts: JARL puts it on the
 * last data frame (section 4.1.2 (g)), as the capture behind DECODE_IQ_DSTAR has it; MMDVM sends it after the last
 * data frame, where a voice frame would start (DStarTX.cpp); MMDVMHost's end frame ends it with a data frame
 * (DSTAR_END_PATTERN_BYTES). MMDVM's receiver (DStarRX.cpp) and DSDcc (dstar.cpp) look for it at any symbol. Matching
 * it only on the grid, and only whole, keeps a 2^-48 chance per step of reading voice or slow data as an end.
 *
 * A limit: in MMDVMHost's placement the pattern's first half is the last 24 symbols of a voice frame, which is read
 * whole and played before the data frame after it completes the match, so that frame (20 ms of bit synchronization
 * decoded as voice) reaches the vocoder. Every other placement is matched before its voice frame plays.
 */
typedef struct {
    uint64_t bits;
    int count;
} dstar_end_window;

static void
dstar_end_window_push(dstar_end_window* w, int dibit) {
    w->bits = ((w->bits << 1) | (uint64_t)(dibit & 1)) & DSTAR_END_MASK;
    if (w->count < DSTAR_END_BITS) {
        w->count++;
    }
}

/* Whether the last @p len bits read are the end pattern's first @p len (0 < len <= 48). */
static int
dstar_end_window_starts_pattern(const dstar_end_window* w, int len) {
    if (len <= 0 || len > DSTAR_END_BITS || w->count < len) {
        return 0;
    }
    const uint64_t mask = (UINT64_C(1) << (unsigned)len) - 1U;
    return (w->bits & mask) == (DSTAR_END_PATTERN >> (unsigned)(DSTAR_END_BITS - len));
}

static int
dstar_end_window_is_end(const dstar_end_window* w) {
    return dstar_end_window_starts_pattern(w, DSTAR_END_BITS);
}

/* How a run of frames ended. */
typedef enum {
    DSTAR_READ_ON = 0, /* read whole: the stream goes on */
    DSTAR_READ_LOST,   /* a carrier boundary or an input interruption cut it: dropped, as on a sync loss */
    DSTAR_READ_END,    /* the end pattern ended it, and the transmission */
} dstar_read_outcome;

/* One voice frame, 24 symbols at a time: its 72 symbols land on the interleave schedule, each cell with its symbol's
 * bit and the confidence in it (issue #599); the 24 cells the schedule never reaches stay bit 0 at reliability 0,
 * which the decoder never reads. Returns the step (1-3, each 24 symbols) at whose end the 48 bits read are the end
 * pattern, as soon as they are, or 0: the rest of the frame is not read, it is no part of this transmission. A match on
 * step 1 holds the data frame before the voice frame; one on step 2 or 3 holds none. */
static int
dstar_read_voice_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24], dstar_end_window* w) {
    DSD_MEMSET(ambe_fr, 0, sizeof(dsd_vocoder_soft_bit) * 4U * 24U);
    for (int i = 0; i < DSTAR_VOICE_SYMBOLS; i++) {
        float symbol = 0.0f;
        const int dibit = getDibitAndSoftSymbol(opts, state, &symbol);
        dsd_vocoder_soft_bit* cell = &ambe_fr[dstar_interleave_w[i]][dstar_interleave_x[i]];
        cell->bit = (uint8_t)(dibit & 1);
        cell->reliability = dsd_two_level_symbol_reliability(opts, state, symbol);
        dstar_end_window_push(w, dibit);
        if ((i % DSTAR_DATA_SYMBOLS) == DSTAR_DATA_SYMBOLS - 1 && dstar_end_window_is_end(w)) {
            return (i / DSTAR_DATA_SYMBOLS) + 1;
        }
    }
    return 0;
}

/* One data frame's 24 hard symbols into @p out; 1 when the 48 bits ending with it are the end pattern. */
static int
dstar_read_data_frame(dsd_opts* opts, dsd_state* state, uint8_t* out, dstar_end_window* w) {
    for (int i = 0; i < DSTAR_DATA_SYMBOLS; i++) {
        const int dibit = get_dibit_and_analog_signal(opts, state, NULL);
        out[i] = (uint8_t)dibit;
        dstar_end_window_push(w, dibit);
    }
    return dstar_end_window_is_end(w);
}

/* Whether a data frame as read is the resynchronization word. Matched exactly, as the hunt matches it. */
static int
dstar_is_resync_word(const uint8_t* data) {
    uint32_t word = 0U;
    for (int i = 0; i < DSTAR_DATA_SYMBOLS; i++) {
        word = (word << 1) | (uint32_t)(data[i] & 1U);
    }
    return word == DSTAR_RESYNC_WORD;
}

/* An end read whole on the carrier it began on ends the transmission; one the read lost is no end. */
static dstar_read_outcome
dstar_end_or_lost(const dsd_state* state, uint32_t carrier_seq) {
    return dsd_state_frame_read_lost(state, carrier_seq) ? DSTAR_READ_LOST : DSTAR_READ_END;
}

static void
dstar_frame_done(dsd_opts* opts, dsd_state* state) {
    // since we are in a long loop, use this to improve response time in ncurses
    if (dsd_telemetry_is_active()) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    // slot 1
    dsd_event_sync_slot(opts, state, 0);
}

/*
 * The data frame after the superframe's last voice frame: the next resynchronization word, which the hunt matches, or
 * the end pattern, which only the decoder can read. The 48 bits that could make an end there end on that frame
 * (the last voice frame's last 24 bits and it) or 24 symbols after it (it and the next voice frame's first 24). Read
 * one symbol at a time while either can still be the pattern: what was read when neither can goes back to the hunt
 * (dsd_frame_sync_hand_back_symbols()), so a resynchronization word, whose 12th bit is no end pattern's, is matched
 * where it ends, as before, and the symbols after a 24-bit prefix that the rest does not complete are the hunt's too.
 */
static dstar_read_outcome
dstar_read_resync_slot(dsd_opts* opts, dsd_state* state, dstar_end_window* w, uint32_t carrier_seq) {
    for (int n = 1; n <= DSTAR_END_BITS; n++) {
        dstar_end_window_push(w, get_dibit_and_analog_signal(opts, state, NULL));
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            return DSTAR_READ_LOST;
        }
        const int ends_on_slot = n <= DSTAR_DATA_SYMBOLS && dstar_end_window_starts_pattern(w, DSTAR_DATA_SYMBOLS + n);
        const int ends_after_slot = dstar_end_window_starts_pattern(w, n);
        if ((n == DSTAR_DATA_SYMBOLS && ends_on_slot) || (n == DSTAR_END_BITS && ends_after_slot)) {
            return DSTAR_READ_END;
        }
        if (!ends_on_slot && !ends_after_slot) {
            dsd_frame_sync_hand_back_symbols(state, n);
            return DSTAR_READ_ON;
        }
    }
    return DSTAR_READ_ON; /* not reached: at n == 48 one of the two returns above holds */
}

/* The superframe behind its resynchronization word: voice frames 1-20, each followed by its data frame, then voice frame
 * 0 of the next superframe and its data frame, the next word. @p carrier_seq: the carrier count when the read began,
 * the header's behind an RF header (one count for the whole read the header leads). */
static dstar_read_outcome
dstar_read_superframe(dsd_opts* opts, dsd_state* state, dstar_end_window* w, int plays_voice, uint32_t carrier_seq) {
    uint8_t sd[480];
    DSD_MEMSET(sd, 0, sizeof(sd));
    dsd_vocoder_soft_bit ambe_fr[4][24];
    /* The superframe belongs to one carrier (issue #575): its slow data decodes only after the last voice frame, so a
       header, text or position message read whole before the carrier boundary moved the carrier count (a replay read
       adopting a recorded retune) would publish the carrier left's station after the boundary ended its call. From the
       voice frame read across or after the move, the superframe is dropped, slow data and all, as on a sync loss. So it
       is from the voice frame an input interruption cut (a Signal search leaving the superframe): the rest is erasures
       getSymbol() stood in, which the vocoder must not play. */
    for (int j = 0; j < DSTAR_SUPERFRAME_FRAMES; j++) {
        const int end = dstar_read_voice_frame(opts, state, ambe_fr, w);
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            return DSTAR_READ_LOST;
        }
        if (end) {
            /* The end began in voice frame 0 of the next superframe, after the last data frame (MMDVM's placement):
               the slow data is whole, read on this carrier, and decodes (a header, message or position in it is the
               transmission's last). An end that holds a data frame leaves it partial, and it is dropped. */
            if (j == DSTAR_SUPERFRAME_FRAMES - 1 && end >= 2) {
                processDSTAR_SD(opts, state, sd);
            }
            return DSTAR_READ_END;
        }
        if (plays_voice) {
            processMbeFrameSoft(opts, state, DSD_VOICE_PROTOCOL_DSTAR, NULL, ambe_fr, NULL);
            dsd_play_synthesized_voice(opts, state);
        }
        if (j == DSTAR_SUPERFRAME_FRAMES - 1) {
            processDSTAR_SD(opts, state, sd);
            dstar_frame_done(opts, state);
            break;
        }
        if (dstar_read_data_frame(opts, state, sd + ((size_t)j * DSTAR_DATA_SYMBOLS), w)) {
            return dstar_end_or_lost(state, carrier_seq);
        }
        /* A cut inside the data frame drops the superframe here: the next voice frame would be read from the new
           carrier (or from erasures), and a sync in it would be lost to the hunt. */
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            return DSTAR_READ_LOST;
        }
        dstar_frame_done(opts, state);
    }
    return dstar_read_resync_slot(opts, state, w, carrier_seq);
}

/* The end pattern closed the transmission (JARL STD 6.0 section 4.1.2 (g)-(h)): its D-STAR call ends on it, as on a
 * terminator, its capture ends (issue #661), and what slow data it left unfinished goes with it. No CRC covers the
 * pattern, so it is no evidence the transmission was real; the confirmation the transmission earned ends with it
 * (dstar_confirm_reset(), which begins a new transmission): the next must earn its own, so a lone resynchronization word
 * before the carrier drops plays nothing and opens no call. Nothing after it was read: the next transmission's preamble
 * and header are the hunt's. */
static void
dstar_transmission_ended(dsd_opts* opts, dsd_state* state) {
    DSD_FPRINTF(stderr, " END");
    dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_DSTAR_HD_POS);
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 0U, &call) > 0 && DSD_SYNC_IS_DSTAR(call.protocol)
        && dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0) {
        dsd_event_sync_slot(opts, state, 0U);
    }
    dstar_confirm_reset(state);
}

/* A superframe and its accounting (dstar_confirm.h). @p behind_resync: the frames follow an exact resynchronization
 * word, the only thing about them that can be checked from here. @p carrier_seq: as dstar_read_superframe().
 * @p checked (may be NULL): 1 when a CRC in this superframe's slow data proved the transmission (strong evidence: a
 * header rebroadcast or a D-PRS record), never for its resynchronization words alone. Returns whether the transmission
 * stands confirmed as the superframe ends. */
static int
dstar_voice_superframe(dsd_opts* opts, dsd_state* state, dstar_end_window* w, int behind_resync, uint32_t carrier_seq,
                       int* checked) {
    /* A CRC-16 in the slow data below may yet prove the transmission outright. The word on its own is weak, so it has
     * to repeat -- see dstar_confirm.h. */
    dstar_confirm_begin_frame(state);
    if (behind_resync) {
        dstar_confirm_note_evidence(opts, state, DSTAR_EVIDENCE_WEAK);
    }
    /* Its voice reaches the vocoder, the speaker and the call record only from a confirmed transmission: a header whose
       CRC passed, or the superframe before this one (dstar_confirm.h). One sync word on its own is no call: under Auto
       the 4800/2 hunt matched one on receiver noise three times in four hours, and the superframe behind it played
       noise as a call with no callsigns. A late-entry call loses its first superframe (420 ms), as the hunt's credit
       already did. */
    const int plays_voice = dstar_confirm_is_confirmed(state);
    const dstar_read_outcome outcome = dstar_read_superframe(opts, state, w, plays_voice, carrier_seq);

    DSD_FPRINTF(stderr, "\n");

    dstar_confirm_end_frame(opts, state);
    /* The superframe's verdict is the transmission's as it was read, whatever its end clears after. */
    const int confirmed = dstar_confirm_is_confirmed(state);
    if (checked != NULL) {
        *checked = state->dstar_confirm_frame_evidence == (uint8_t)DSTAR_EVIDENCE_STRONG;
    }
    if (outcome == DSTAR_READ_END) {
        dstar_transmission_ended(opts, state);
    }
    return confirmed;
}

//simplified DSTAR
int
processDSTAR(dsd_opts* opts, dsd_state* state) {
    dstar_end_window w;
    DSD_MEMSET(&w, 0, sizeof(w));
    /* The hunt matched the resynchronization word exactly (strcmp, no error budget). */
    return dstar_voice_superframe(opts, state, &w, 1, state->carrier_seq, NULL);
}

/* Voice frame 0 behind an RF header and its data frame, which is the resynchronization word (JARL STD 6.0 section 4.1.2
 * (c)), then the superframe behind it. Returns DSTAR_READ_ON once the word's frame is read whole on the header's carrier
 * (@p carrier_seq, the count when the header's read began). */
static dstar_read_outcome
dstar_read_frame_zero(dsd_opts* opts, dsd_state* state, dstar_end_window* w, uint32_t carrier_seq, int* behind_resync) {
    dsd_vocoder_soft_bit ambe_fr[4][24];
    const int end = dstar_read_voice_frame(opts, state, ambe_fr, w);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        return DSTAR_READ_LOST;
    }
    if (end) {
        return DSTAR_READ_END;
    }
    /* A header whose CRC passed confirmed the transmission: its first 20 ms play. */
    if (dstar_confirm_is_confirmed(state)) {
        processMbeFrameSoft(opts, state, DSD_VOICE_PROTOCOL_DSTAR, NULL, ambe_fr, NULL);
        dsd_play_synthesized_voice(opts, state);
    }
    uint8_t resync[DSTAR_DATA_SYMBOLS];
    if (dstar_read_data_frame(opts, state, resync, w)) {
        return dstar_end_or_lost(state, carrier_seq);
    }
    /* A carrier boundary or an input interruption inside the word's frame: what follows is not the header's
       transmission, and its confirmation must not play or publish it. */
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        return DSTAR_READ_LOST;
    }
    dstar_frame_done(opts, state);
    /* The superframe behind it counts the word only when it was there: the header's own sync proves the header was
       seen, not the word. */
    *behind_resync = dstar_is_resync_word(resync);
    return DSTAR_READ_ON;
}

/* The RF header just checked, to a scan hold (dstar_report_scan_activity()): a data transmission when its DATA flag made
   the D-STAR call it observed a data call (dstar_header_observe()); a header whose observation did not reach the call
   state reports a transmission of unknown kind. */
static void
dstar_report_header_scan_activity(const dsd_opts* opts, const dsd_state* state) {
    dsd_call_snapshot call;
    const int known = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                      && DSD_SYNC_IS_DSTAR(call.protocol);
    dstar_report_scan_activity(opts, state, known, known && call.kind == DSD_CALL_KIND_DATA);
}

int
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {

    int i;
    float soft_symbols[660];
    const uint32_t carrier_seq = state->carrier_seq;

    // Capture soft symbols for soft-decision decoding
    for (i = 0; i < 660; i++) {
        getDibitAndSoftSymbol(opts, state, &soft_symbols[i]);
    }
    /* A header opens another transmission, whether or not it decodes: what slow data the one before it left unfinished
       is not this one's, and whose transmission it is stays unknown (the call it leaves ACTIVE, its terminator missed,
       may be another radio's) until a checked header names a callsign (dstar_header_observe()). */
    dstar_tx_begin(state);
    /* A carrier boundary split the header (issue #575), or an input interruption cut it (a Signal search left it): the
       rest of it belongs to another carrier or is erasures the reader stood in, which the soft Viterbi decode reads as
       confident symbols. It decodes nothing, and the superframe behind it is not read. */
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        return 0;
    }

    const int header_ok = dstar_header_decode_soft(state, soft_symbols);
    if (header_ok) {
        /* A CRC-16/X.25 over 39 octets: proof on its own, and it opens the transmission the
         * voice superframe below belongs to. */
        dstar_confirm_note_evidence(opts, state, DSTAR_EVIDENCE_STRONG);
        /* And a scan hold (issue #890, D4): the header just put its DATA flag on the call as the call's kind. */
        dstar_report_header_scan_activity(opts, state);
        /* The capture of the transmission before it ends here (issue #661). */
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_DSTAR_HD_POS);
    }
    /* The voice frame and the word behind the header are read here, so the superframe that follows starts at voice
       frame 1 with its slow data aligned, as behind a resynchronization word the hunt matched: its first data frame is
       slow data, not the word (a header rebroadcast or a message in it is read whole). */
    dstar_end_window w;
    DSD_MEMSET(&w, 0, sizeof(w));
    int behind_resync = 0;
    const dstar_read_outcome outcome = dstar_read_frame_zero(opts, state, &w, carrier_seq, &behind_resync);
    if (outcome == DSTAR_READ_ON) {
        /* A header that failed its CRC is no verdict on the read when the superframe behind it proved itself: a header
           rebroadcast or D-PRS record whose CRC passed makes it productive, as it would behind a resynchronization word
           the hunt matched. The word alone, or a weak streak, never does. */
        int checked = 0;
        (void)dstar_voice_superframe(opts, state, &w, behind_resync, carrier_seq, &checked);
        return header_ok || checked;
    }
    DSD_FPRINTF(stderr, "\n");
    if (outcome == DSTAR_READ_END) {
        dstar_transmission_ended(opts, state);
    }
    return header_ok;
}
