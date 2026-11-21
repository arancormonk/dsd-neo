// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

/* The marker words the hunt did not take: a whole marker less the words it takes the end on. */
enum { M17_EOT_DRAIN_MAX_WORDS = (int)(DSD_FRAME_SYNC_M17_EOT_MARKER_WORDS - DSD_FRAME_SYNC_M17_EOT_WORDS) };

/** @brief Map a frame decoder's "did this validate anything" answer onto the hunt's verdict. */
static dsd_frame_verdict
m17_verdict(int validated) {
    return validated != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

/*
 * The rest of an end-of-transmission marker the hunt took the end on (M17 spec 2.0.11, End of Transmission Marker: 192
 * symbols of 0x555D), read a word at a time up to the first word that is not the marker: that word may be the start of
 * what follows (another transmission's preamble or sync word), so the hunt takes it up as the history of its window
 * (dsd_frame_sync_hand_back_word(), scored where it ends) instead of losing it. A marker whose first words a fade took leaves
 * fewer words than the 184 symbols a fixed skip used to read, and that skip ran into the next frame. A word is judged
 * as the hunt judges one (dsd_frame_sync_m17_eot_word()), on the signs the hunt reads. An input interruption ends the
 * drain with nothing handed back: the next hunt starts afresh.
 */
static void
m17_drain_eot_marker(dsd_opts* opts, dsd_state* state, int inverted) {
    const uint32_t carrier_seq = state->carrier_seq;
    for (int word = 0; word < M17_EOT_DRAIN_MAX_WORDS; word++) {
        char window[DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS + 1U];
        for (int i = 0; i < (int)DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS; i++) {
            (void)get_dibit_and_analog_signal(opts, state, NULL);
        }
        if (dsd_state_frame_read_lost(state, carrier_seq)) {
            return;
        }
        for (int i = 0; i < (int)DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS; i++) {
            window[i] = dsd_frame_sync_symbol_sign_back(state, (int)DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS - 1 - i);
        }
        window[DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS] = '\0';
        if (!dsd_frame_sync_m17_eot_word(window, inverted)) {
            dsd_frame_sync_hand_back_word(state, (int)DSD_FRAME_SYNC_M17_EOT_WORD_SYMBOLS);
            return;
        }
    }
}

dsd_frame_verdict
dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state) {
    if (state->synctype == DSD_SYNC_M17_PRE_POS || state->synctype == DSD_SYNC_M17_PRE_NEG) {
        /* Frame sync no longer returns a bare preamble -- it latches a candidate and waits for
         * the LSF or BRT that must follow (#399) -- so this branch is unreachable from the air.
         * It stays for the symbol classifiers and for anything that replays a recorded synctype.
         * 8 dibits is below DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS, so the floor refuses it credit
         * either way (#388). */
        skipDibit(opts, state, 8);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    if (state->synctype == DSD_SYNC_M17_EOT_POS || state->synctype == DSD_SYNC_M17_EOT_NEG) {
        m17_drain_eot_marker(opts, state, state->synctype == DSD_SYNC_M17_EOT_NEG);
        // An EOT marker decoded over the air is positive evidence the transmission ended, so the
        // event layer can keep an audible epoch whose LSF never reassembled; a plain EXPLICIT end
        // is indistinguishable from an engine retune and would drop that row.
        // Only an M17 call: the marker ends the transmission whose frames led the hunt to it.
        m17_end_call(opts, state, DSD_CALL_END_TERMINATOR, 0);
        /* A signature the transmission advertised and never judged (its last chunk, with the EOS, was lost) is
           reported incomplete before the reset below forgets it. */
        m17_note_transmission_end(state);
        /* What the transmission held (its link setup, LSF bits and LICH chunks, packet, BERT receiver, signature and
           text) goes with its evidence in m17_confirm_reset() below, the one M17 boundary; the hunt's own state goes
           here. */
        state->m17_polarity = 0;
        state->m17_pre_run = 0;
        state->m17_pre_candidate = 0;
        state->m17_pre_candidate_ttl = 0;
        dsd_sync_hit_history_clear(&state->m17_join_hits);
        dsd_nxdn96_sync_clear(&state->nxdn96_sync);
        state->lastsynctype = DSD_SYNC_NONE;
        /* The marker is seven outer symbols of one sign to one of the other, and the levels its words leave behind
         * are skewed with it: the centre sat near half the outer level. The hunt for the next transmission slices and
         * times its preamble against that centre, and with it the timing loop's crossings fell off the zero
         * crossings, the preamble read at the wrong phase and the next LSF was missed or failed its CRC on clean
         * signal. The transmission has ended: put the centre and the extremes back where a lost
         * carrier leaves them (the values noCarrier() sets; nothing else of its reset is done here), and the next
         * preamble sets its own (issue #834). */
        state->max = 15000.0f;
        state->min = -15000.0f;
        state->center = 0.0f;
        /* An EOT can only be reached from an LSF, STR, PKT or BRT sync, so it inherits whatever
         * that chain proved: a transmission that cleared a CRC really did end here, and one that
         * never proved anything ends a chain of false syncs. Report the transmission's own
         * verdict, then forget it -- the next carrier proves itself again (#399). */
        const dsd_frame_verdict eot_verdict =
            m17_confirm_is_confirmed(state) ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
        m17_confirm_reset(state);
        return eot_verdict;
    }

    if (state->synctype == DSD_SYNC_M17_LSF_POS || state->synctype == DSD_SYNC_M17_LSF_NEG) {
        return m17_verdict(processM17LSF(opts, state));
    }

    if (state->synctype == DSD_SYNC_M17_BRT_POS || state->synctype == DSD_SYNC_M17_BRT_NEG) {
        return m17_verdict(processM17BRT(opts, state));
    }

    if (state->synctype == DSD_SYNC_M17_PKT_POS || state->synctype == DSD_SYNC_M17_PKT_NEG) {
        return m17_verdict(processM17PKT(opts, state));
    }

    return m17_verdict(processM17STR(opts, state));
}
