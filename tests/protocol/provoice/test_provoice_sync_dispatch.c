// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for ProVoice sync constants and dispatch routing.
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
#include <dsd-neo/protocol/provoice/provoice.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_provoice(int synctype);
dsd_frame_verdict dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state);

static int open_calls;
static int voice_calls;

static void
reset_calls(void) {
    open_calls = 0;
    voice_calls = 0;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    open_calls++;
}

/* The stubbed confirmation verdict dsd_dispatch_handle_provoice() must pass through. */
static int voice_confirm_result = 1;

int
processProVoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_calls++;
    return voice_confirm_result;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(PROVOICE_SYNC) == 33U, "PROVOICE_SYNC length");
    _Static_assert(sizeof(INV_PROVOICE_SYNC) == 33U, "INV_PROVOICE_SYNC length");
    _Static_assert(sizeof(PROVOICE_EA_SYNC) == 33U, "PROVOICE_EA_SYNC length");
    _Static_assert(sizeof(INV_PROVOICE_EA_SYNC) == 33U, "INV_PROVOICE_EA_SYNC length");
    _Static_assert(sizeof(PROVOICE_CONV) == 33U, "PROVOICE_CONV length");
    _Static_assert(sizeof(INV_PROVOICE_CONV) == 33U, "INV_PROVOICE_CONV length");
    _Static_assert(sizeof(PROVOICE_CONV_SHORT) == 17U, "PROVOICE_CONV_SHORT length");
    _Static_assert(sizeof(INV_PROVOICE_CONV_SHORT) == 17U, "INV_PROVOICE_CONV_SHORT length");
    _Static_assert(sizeof(EDACS_SYNC) == 49U, "EDACS_SYNC length");
    _Static_assert(sizeof(INV_EDACS_SYNC) == 49U, "INV_EDACS_SYNC length");
    _Static_assert(sizeof(DOTTING_SEQUENCE_A) == 49U, "DOTTING_SEQUENCE_A length");
    _Static_assert(sizeof(DOTTING_SEQUENCE_B) == 49U, "DOTTING_SEQUENCE_B length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_PROVOICE(DSD_SYNC_PROVOICE_POS), "ProVoice positive synctype");
    _Static_assert(DSD_SYNC_IS_PROVOICE(DSD_SYNC_PROVOICE_NEG), "ProVoice negative synctype");
    assert(dsd_dispatch_matches_provoice(DSD_SYNC_PROVOICE_POS));
    assert(dsd_dispatch_matches_provoice(DSD_SYNC_PROVOICE_NEG));

    _Static_assert(!DSD_SYNC_IS_PROVOICE(DSD_SYNC_EDACS_POS), "EDACS positive is not ProVoice");
    _Static_assert(!DSD_SYNC_IS_PROVOICE(DSD_SYNC_EDACS_NEG), "EDACS negative is not ProVoice");
    _Static_assert(!DSD_SYNC_IS_PROVOICE(DSD_SYNC_DSTAR_VOICE_POS), "D-STAR voice is not ProVoice");
    assert(!dsd_dispatch_matches_provoice(DSD_SYNC_EDACS_POS));
    assert(!dsd_dispatch_matches_provoice(DSD_SYNC_EDACS_NEG));
    assert(!dsd_dispatch_matches_provoice(DSD_SYNC_DSTAR_VOICE_POS));
}

static void
test_voice_dispatch(void) {
    static const int voice_synctypes[] = {DSD_SYNC_PROVOICE_POS, DSD_SYNC_PROVOICE_NEG};

    for (size_t i = 0; i < sizeof voice_synctypes / sizeof voice_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
        state.synctype = voice_synctypes[i];

        /* A confirmed transmission is productive: a second frame arrived behind its own
         * exact 32-symbol sync word, so the 736 dibits are earned (#421). */
        voice_confirm_result = 1;
        assert(dsd_dispatch_handle_provoice(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

        assert(strcmp(state.fsubtype, " VOICE        ") == 0);
        assert(open_calls == 1);
        assert(voice_calls == 1);
    }
}

/* #421: nothing inside a ProVoice frame can fail a check, so a lone frame proves nothing.
 * Until a second one arrives behind its own sync word the 736 dibits validated nothing, and
 * the SPS hunt must not pay for them on the 9600/2 profile EDACS shares. */
static void
test_unconfirmed_voice_reports_unproductive(void) {
    static const int voice_synctypes[] = {DSD_SYNC_PROVOICE_POS, DSD_SYNC_PROVOICE_NEG};

    for (size_t i = 0; i < sizeof voice_synctypes / sizeof voice_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        state.synctype = voice_synctypes[i];
        voice_confirm_result = 0;

        assert(dsd_dispatch_handle_provoice(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
        assert(voice_calls == 1);
    }
    voice_confirm_result = 1;
}

static void
test_voice_dispatch_keeps_open_file(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
    opts.mbe_out_f = stdout;
    state.synctype = DSD_SYNC_PROVOICE_POS;
    voice_confirm_result = 1;

    assert(dsd_dispatch_handle_provoice(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

    assert(strcmp(state.fsubtype, " VOICE        ") == 0);
    assert(open_calls == 0);
    assert(voice_calls == 1);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_voice_dispatch();
    test_unconfirmed_voice_reports_unproductive();
    test_voice_dispatch_keeps_open_file();
    printf("PROVOICE_SYNC_DISPATCH: OK\n");
    return 0;
}
