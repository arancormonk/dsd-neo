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

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

dsd_frame_verdict
dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state) {
    if (state->synctype == DSD_SYNC_M17_PRE_POS || state->synctype == DSD_SYNC_M17_PRE_NEG) {
        /* 8 dibits is below DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS, which was calibrated against
         * exactly this path (#388): the floor already refuses it credit, so a verdict here
         * would be decoration. */
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
        /* The marker itself is the decode, so the SPS hunt is told the same thing the call
         * state was told above (#391). frame_sync_try_m17_eot() only matches an EOT whose
         * lastsynctype is an M17 LSF, STR, PKT or BRT sync, so it cannot fire on cold noise
         * the way the permissive preamble matcher can, and clearing lastsynctype here means
         * it fires at most once per transmission. The 184 dibits behind it are discarded,
         * but they are the rest of a frame this profile really was carrying. */
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    if (state->synctype == DSD_SYNC_M17_LSF_POS || state->synctype == DSD_SYNC_M17_LSF_NEG) {
        processM17LSF(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    if (state->synctype == DSD_SYNC_M17_BRT_POS || state->synctype == DSD_SYNC_M17_BRT_NEG) {
        processM17BRT(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    if (state->synctype == DSD_SYNC_M17_PKT_POS || state->synctype == DSD_SYNC_M17_PKT_NEG) {
        processM17PKT(opts, state);
        return DSD_FRAME_VERDICT_PRODUCTIVE;
    }

    processM17STR(opts, state);
    return DSD_FRAME_VERDICT_PRODUCTIVE;
}
