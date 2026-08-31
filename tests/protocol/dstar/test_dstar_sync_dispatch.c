// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for D-STAR sync constants and dispatch routing.
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
#include <dsd-neo/protocol/dstar/dstar.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_dstar(int synctype);
dsd_frame_verdict dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state);

static int header_calls;
static int open_calls;
static int voice_calls;

static void
reset_calls(void) {
    header_calls = 0;
    open_calls = 0;
    voice_calls = 0;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    open_calls++;
}

/* The stubbed confirmation verdict dsd_dispatch_handle_dstar() must pass through. */
static int voice_confirm_result = 1;

int
processDSTAR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_calls++;
    return voice_confirm_result;
}

/* The stubbed header CRC verdict dsd_dispatch_handle_dstar() must pass through. */
static int header_decode_result = 1;

int
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    header_calls++;
    return header_decode_result;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(DSTAR_SYNC) == 25U, "DSTAR_SYNC length");
    _Static_assert(sizeof(INV_DSTAR_SYNC) == 25U, "INV_DSTAR_SYNC length");
    _Static_assert(sizeof(DSTAR_HD) == 25U, "DSTAR_HD length");
    _Static_assert(sizeof(INV_DSTAR_HD) == 25U, "INV_DSTAR_HD length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_DSTAR(DSD_SYNC_DSTAR_VOICE_POS), "D-STAR voice positive synctype");
    _Static_assert(DSD_SYNC_IS_DSTAR(DSD_SYNC_DSTAR_VOICE_NEG), "D-STAR voice negative synctype");
    _Static_assert(DSD_SYNC_IS_DSTAR(DSD_SYNC_DSTAR_HD_POS), "D-STAR header positive synctype");
    _Static_assert(DSD_SYNC_IS_DSTAR(DSD_SYNC_DSTAR_HD_NEG), "D-STAR header negative synctype");
    assert(dsd_dispatch_matches_dstar(DSD_SYNC_DSTAR_VOICE_POS));
    assert(dsd_dispatch_matches_dstar(DSD_SYNC_DSTAR_VOICE_NEG));
    assert(dsd_dispatch_matches_dstar(DSD_SYNC_DSTAR_HD_POS));
    assert(dsd_dispatch_matches_dstar(DSD_SYNC_DSTAR_HD_NEG));

    _Static_assert(!DSD_SYNC_IS_DSTAR(DSD_SYNC_P25P1_POS), "P25P1 is not D-STAR");
    _Static_assert(!DSD_SYNC_IS_DSTAR(DSD_SYNC_NXDN_POS), "NXDN is not D-STAR");
    assert(!dsd_dispatch_matches_dstar(DSD_SYNC_P25P1_POS));
    assert(!dsd_dispatch_matches_dstar(DSD_SYNC_NXDN_POS));
}

static void
test_voice_dispatch(void) {
    static const int voice_synctypes[] = {DSD_SYNC_DSTAR_VOICE_POS, DSD_SYNC_DSTAR_VOICE_NEG};

    for (size_t i = 0; i < sizeof voice_synctypes / sizeof voice_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
        state.synctype = voice_synctypes[i];

        /* A confirmed transmission is productive: the superframe decoded content that
         * proved itself, so the 1992 symbols it took are earned (#421). */
        voice_confirm_result = 1;
        assert(dsd_dispatch_handle_dstar(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

        assert(strcmp(state.fsubtype, " VOICE        ") == 0);
        assert(open_calls == 1);
        assert(voice_calls == 1);
        assert(header_calls == 0);
    }
}

static void
test_header_dispatch(void) {
    static const int header_synctypes[] = {DSD_SYNC_DSTAR_HD_POS, DSD_SYNC_DSTAR_HD_NEG};

    for (size_t i = 0; i < sizeof header_synctypes / sizeof header_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        opts.mbe_out_f = stdout;
        DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
        state.synctype = header_synctypes[i];

        header_decode_result = 1;
        assert(dsd_dispatch_handle_dstar(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

        assert(strcmp(state.fsubtype, " DATA         ") == 0);
        assert(open_calls == 0);
        assert(voice_calls == 0);
        assert(header_calls == 1);
    }
}

/* #421: until a D-STAR transmission proves itself -- a CRC-16/X.25 on the RF header or the
 * slow-data header rebroadcast, or a second superframe behind its own exact sync word -- the
 * 1992 symbols a voice superframe consumes validated nothing, and the SPS hunt must not pay
 * for them. This is the hole #419 left open. */
static void
test_unconfirmed_voice_reports_unproductive(void) {
    static const int voice_synctypes[] = {DSD_SYNC_DSTAR_VOICE_POS, DSD_SYNC_DSTAR_VOICE_NEG};

    for (size_t i = 0; i < sizeof voice_synctypes / sizeof voice_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        state.synctype = voice_synctypes[i];
        voice_confirm_result = 0;

        assert(dsd_dispatch_handle_dstar(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
        assert(voice_calls == 1);
        assert(header_calls == 0);
    }
    voice_confirm_result = 1;
}

/* #391: a header whose CRC-16/X.25 failed consumed 2652 symbols and validated none of them,
 * so the handler must say so rather than letting the SPS hunt pay for them. A header opens a
 * transmission, so it reports its own verdict even where the voice frame behind it confirms
 * (#421). */
static void
test_failed_header_reports_unproductive(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    state.synctype = DSD_SYNC_DSTAR_HD_POS;
    header_decode_result = 0;
    voice_confirm_result = 1;

    assert(dsd_dispatch_handle_dstar(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);
    assert(header_calls == 1);
    header_decode_result = 1;
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_voice_dispatch();
    test_unconfirmed_voice_reports_unproductive();
    test_header_dispatch();
    test_failed_header_reports_unproductive();
    printf("DSTAR_SYNC_DISPATCH: OK\n");
    return 0;
}
