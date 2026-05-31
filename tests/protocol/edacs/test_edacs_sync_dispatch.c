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
#include <dsd-neo/protocol/edacs/edacs.h>
#include <stdio.h>

int dsd_dispatch_matches_edacs(int synctype);
void dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state);

static int close_calls;
static int edacs_calls;

static void
reset_calls(void) {
    close_calls = 0;
    edacs_calls = 0;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    close_calls++;
    opts->mbe_out_f = NULL;
}

void
edacs(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    edacs_calls++;
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

    dsd_dispatch_handle_edacs(&opts, &state);

    assert(close_calls == 1);
    assert(edacs_calls == 1);
    assert(opts.mbe_out_f == NULL);
}

static void
test_edacs_dispatch_without_mbe_output(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    dsd_dispatch_handle_edacs(&opts, &state);

    assert(close_calls == 0);
    assert(edacs_calls == 1);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_edacs_dispatch_closes_mbe_output();
    test_edacs_dispatch_without_mbe_output();
    printf("EDACS_SYNC_DISPATCH: OK\n");
    return 0;
}
