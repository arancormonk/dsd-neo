// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for EDACS sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <stdio.h>

int dsd_dispatch_matches_edacs(int synctype);
dsd_frame_verdict dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state);

static int close_calls;
static int close_protocol_synctype = DSD_SYNC_NONE;
static int edacs_calls;
/* edacs() moves the carrier count, as a replay read adopting a recorded retune does (issue #575). */
static int edacs_moves_carrier = 0;
/* A Signal search's yield (or a stalled live read) interrupts the input while edacs() reads the frame. */
static int edacs_interrupts = 0;

static void
reset_calls(void) {
    close_calls = 0;
    close_protocol_synctype = DSD_SYNC_NONE;
    edacs_calls = 0;
    edacs_moves_carrier = 0;
    edacs_interrupts = 0;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    close_calls++;
    opts->mbe_out_f = NULL;
}

/* EDACS closes only the EDACS/ProVoice capture (issue #661); the owner filter is tested in CORE_MBE_FILE_IO. This
   records the protocol the dispatcher names and counts the close as the closer above does. */
void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    close_protocol_synctype = synctype;
    if ((slot_mask & DSD_MBE_OUT_SLOT1) != 0 && opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
}

/* The stubbed BCH verdict dsd_dispatch_handle_edacs() must pass through. */
static int edacs_decoded = 1;

int
edacs(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    edacs_calls++;
    if (edacs_moves_carrier) {
        state->carrier_seq++;
    }
    if (edacs_interrupts) {
        state->input_interrupted = 1;
    }
    return edacs_decoded;
}

void
eot_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(EDACS_SYNC) == 49U, "EDACS_SYNC length");
    _Static_assert(sizeof(INV_EDACS_SYNC) == 49U, "INV_EDACS_SYNC length");
    _Static_assert(sizeof(DOTTING_SEQUENCE_A) == 49U, "DOTTING_SEQUENCE_A length");
    _Static_assert(sizeof(DOTTING_SEQUENCE_B) == 49U, "DOTTING_SEQUENCE_B length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_EDACS_ONLY(DSD_SYNC_EDACS_POS), "EDACS positive is EDACS-only");
    _Static_assert(DSD_SYNC_IS_EDACS_ONLY(DSD_SYNC_EDACS_NEG), "EDACS negative is EDACS-only");
    _Static_assert(DSD_SYNC_IS_EDACS(DSD_SYNC_EDACS_POS), "EDACS positive is EDACS");
    _Static_assert(DSD_SYNC_IS_EDACS(DSD_SYNC_EDACS_NEG), "EDACS negative is EDACS");
    assert(dsd_dispatch_matches_edacs(DSD_SYNC_EDACS_POS));
    assert(dsd_dispatch_matches_edacs(DSD_SYNC_EDACS_NEG));

    _Static_assert(!DSD_SYNC_IS_EDACS_ONLY(DSD_SYNC_PROVOICE_POS), "ProVoice is not EDACS-only");
    _Static_assert(DSD_SYNC_IS_EDACS(DSD_SYNC_PROVOICE_POS), "ProVoice is EDACS-family");
    assert(!dsd_dispatch_matches_edacs(DSD_SYNC_PROVOICE_POS));
    assert(!dsd_dispatch_matches_edacs(DSD_SYNC_DSTAR_VOICE_POS));
}

static void
test_edacs_dispatch_closes_mbe_output(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.mbe_out_f = stdout;
    state.synctype = DSD_SYNC_EDACS_NEG;

    assert(dsd_dispatch_handle_edacs(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

    assert(close_calls == 1);
    assert(DSD_SYNC_IS_EDACS(close_protocol_synctype));
    assert(edacs_calls == 1);
    assert(opts.mbe_out_f == NULL);
}

/* Issue #661: a sync whose BCH fails is not an EDACS control frame, and a frame read across a carrier boundary is none
   of the carrier's: neither ends the ProVoice capture the decoder may be writing. */
static void
test_edacs_dispatch_keeps_capture_on_rejected_frame(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    opts.mbe_out_f = stdout;
    state.synctype = DSD_SYNC_EDACS_POS;
    edacs_decoded = 0;
    assert(dsd_dispatch_handle_edacs(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    edacs_decoded = 1;
    assert(close_calls == 0);
    assert(opts.mbe_out_f == stdout);

    edacs_moves_carrier = 1;
    assert(dsd_dispatch_handle_edacs(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(close_calls == 0);
    assert(opts.mbe_out_f == stdout);

    /* Nor does a frame an input interruption cut (a Signal search's yield, or a stalled live read), whatever the
       decoder made of the erasures standing in for its rest. */
    edacs_moves_carrier = 0;
    edacs_interrupts = 1;
    (void)dsd_dispatch_handle_edacs(&opts, &state);
    assert(close_calls == 0);
    assert(opts.mbe_out_f == stdout);
}

static void
test_edacs_dispatch_without_mbe_output(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    assert(dsd_dispatch_handle_edacs(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

    assert(close_calls == 0);
    assert(edacs_calls == 1);
}

/* #391: edacs() answers zero on the BCH failure and on the trunk_is_tuned early-out alike.
 * Either way the 240 dibits are already gone, so the handler must refuse them the dwell. */
static void
test_failed_bch_reports_unproductive(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    edacs_decoded = 0;

    assert(dsd_dispatch_handle_edacs(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(edacs_calls == 1);
    edacs_decoded = 1;
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_edacs_dispatch_closes_mbe_output();
    test_edacs_dispatch_keeps_capture_on_rejected_frame();
    test_edacs_dispatch_without_mbe_output();
    test_failed_bch_reports_unproductive();
    printf("EDACS_SYNC_DISPATCH: OK\n");
    return 0;
}
