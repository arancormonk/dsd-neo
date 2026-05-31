// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/m17/m17.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "protocol_dispatch_impl.h"

enum { M17_EOT_REMAINING_DIBITS = 184 };

int
dsd_dispatch_matches_m17(int synctype) {
    return DSD_SYNC_IS_M17(synctype);
}

void
dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state) {
    if (state->synctype == DSD_SYNC_M17_PRE_POS || state->synctype == DSD_SYNC_M17_PRE_NEG) {
        skipDibit(opts, state, 8);
        return;
    }

    if (state->synctype == DSD_SYNC_M17_EOT_POS || state->synctype == DSD_SYNC_M17_EOT_NEG) {
        skipDibit(opts, state, M17_EOT_REMAINING_DIBITS);
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
        return;
    }

    if (state->synctype == DSD_SYNC_M17_LSF_POS || state->synctype == DSD_SYNC_M17_LSF_NEG) {
        processM17LSF(opts, state);
        return;
    }

    if (state->synctype == DSD_SYNC_M17_BRT_POS || state->synctype == DSD_SYNC_M17_BRT_NEG) {
        processM17BRT(opts, state);
        return;
    }

    if (state->synctype == DSD_SYNC_M17_PKT_POS || state->synctype == DSD_SYNC_M17_PKT_NEG) {
        processM17PKT(opts, state);
        return;
    }

    processM17STR(opts, state);
}
