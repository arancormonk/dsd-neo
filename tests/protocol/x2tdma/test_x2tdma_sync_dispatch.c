// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for X2-TDMA sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/x2tdma/x2tdma.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_x2tdma(int synctype);
void dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state);

static int close_calls;
static int data_calls;
static int open_calls;
static int print_calls;
static int voice_calls;

static void
reset_calls(void) {
    close_calls = 0;
    data_calls = 0;
    open_calls = 0;
    print_calls = 0;
    voice_calls = 0;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    print_calls++;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    open_calls++;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    close_calls++;
    opts->mbe_out_f = NULL;
}

void
processX2TDMAdata(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    data_calls++;
}

void
processX2TDMAvoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    voice_calls++;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(X2TDMA_BS_DATA_SYNC) == 25U, "X2TDMA_BS_DATA_SYNC length");
    _Static_assert(sizeof(X2TDMA_BS_VOICE_SYNC) == 25U, "X2TDMA_BS_VOICE_SYNC length");
    _Static_assert(sizeof(X2TDMA_MS_DATA_SYNC) == 25U, "X2TDMA_MS_DATA_SYNC length");
    _Static_assert(sizeof(X2TDMA_MS_VOICE_SYNC) == 25U, "X2TDMA_MS_VOICE_SYNC length");
}

static void
test_synctype_helpers(void) {
    for (int synctype = DSD_SYNC_X2TDMA_DATA_POS; synctype <= DSD_SYNC_X2TDMA_DATA_NEG; synctype++) {
        assert(DSD_SYNC_IS_X2TDMA(synctype));
        assert(dsd_dispatch_matches_x2tdma(synctype));
    }

    _Static_assert(!DSD_SYNC_IS_X2TDMA(DSD_SYNC_P25P1_NEG), "P25P1 negative is not X2-TDMA");
    _Static_assert(!DSD_SYNC_IS_X2TDMA(DSD_SYNC_DSTAR_VOICE_POS), "D-STAR voice is not X2-TDMA");
    assert(!dsd_dispatch_matches_x2tdma(DSD_SYNC_P25P1_NEG));
    assert(!dsd_dispatch_matches_x2tdma(DSD_SYNC_DSTAR_VOICE_POS));
}

static void
test_voice_dispatch(void) {
    static const int voice_synctypes[] = {DSD_SYNC_X2TDMA_VOICE_NEG, DSD_SYNC_X2TDMA_VOICE_POS};

    for (size_t i = 0; i < sizeof voice_synctypes / sizeof voice_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        opts.errorbars = 1;
        DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "out");
        state.synctype = voice_synctypes[i];

        dsd_dispatch_handle_x2tdma(&opts, &state);

        assert(state.nac == 0);
        assert(strcmp(state.fsubtype, " VOICE        ") == 0);
        assert(print_calls == 1);
        assert(open_calls == 1);
        assert(close_calls == 0);
        assert(voice_calls == 1);
        assert(data_calls == 0);
    }
}

static void
test_data_dispatch(void) {
    static const int data_synctypes[] = {DSD_SYNC_X2TDMA_DATA_POS, DSD_SYNC_X2TDMA_DATA_NEG};

    for (size_t i = 0; i < sizeof data_synctypes / sizeof data_synctypes[0]; i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_calls();

        opts.errorbars = 1;
        opts.mbe_out_f = stdout;
        state.err_str[0] = 'E';
        state.synctype = data_synctypes[i];

        dsd_dispatch_handle_x2tdma(&opts, &state);

        assert(state.nac == 0);
        assert(state.err_str[0] == '\0');
        assert(print_calls == 1);
        assert(open_calls == 0);
        assert(close_calls == 1);
        assert(voice_calls == 0);
        assert(data_calls == 1);
    }
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_voice_dispatch();
    test_data_dispatch();
    printf("X2TDMA_SYNC_DISPATCH: OK\n");
    return 0;
}
