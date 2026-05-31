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
#include <dsd-neo/protocol/dstar/dstar.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_dstar(int synctype);
void dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state);

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

void
processDSTAR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_calls++;
}

void
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    header_calls++;
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

        dsd_dispatch_handle_dstar(&opts, &state);

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

        dsd_dispatch_handle_dstar(&opts, &state);

        assert(strcmp(state.fsubtype, " DATA         ") == 0);
        assert(open_calls == 0);
        assert(voice_calls == 0);
        assert(header_calls == 1);
    }
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_voice_dispatch();
    test_header_dispatch();
    printf("DSTAR_SYNC_DISPATCH: OK\n");
    return 0;
}
