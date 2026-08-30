// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/m17/m17.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

enum { M17_EOT_REMAINING_DIBITS = 184 };

/** @brief Map a frame decoder's "did this validate anything" answer onto the hunt's verdict. */
static dsd_frame_verdict
m17_verdict(int validated) {
    return validated != 0 ? DSD_FRAME_VERDICT_PRODUCTIVE : DSD_FRAME_VERDICT_UNPRODUCTIVE;
}

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
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
        skipDibit(opts, state, M17_EOT_REMAINING_DIBITS);
        // An EOT marker decoded over the air is positive evidence the transmission ended, so the
        // event layer can keep an audible epoch whose LSF never reassembled; a plain EXPLICIT end
        // is indistinguishable from an engine retune and would drop that row.
        if (dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0) {
            dsd_event_sync_slot(opts, state, 0U);
        }
        DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
        state->m17_pbc_ct = 0;
        state->m17_polarity = 0;
        state->m17_bert_locked = 0;
        state->m17_bert_lfsr = 1;
        state->m17_bert_lock_count = 0;
        state->m17_bert_window_bits = 0;
        state->m17_bert_window_errors = 0;
        state->m17_bert_bits = 0;
        state->m17_bert_errors = 0;
        state->m17_bert_resyncs = 0;
        state->lastsynctype = DSD_SYNC_NONE;
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
