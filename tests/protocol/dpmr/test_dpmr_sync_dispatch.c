// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for dPMR sync constants and dispatch matching.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_dpmr(int synctype);

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
processdPMRvoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(DPMR_FRAME_SYNC_1) == 25U, "DPMR_FRAME_SYNC_1 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_2) == 13U, "DPMR_FRAME_SYNC_2 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_3) == 13U, "DPMR_FRAME_SYNC_3 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_4) == 25U, "DPMR_FRAME_SYNC_4 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_1) == 25U, "INV_DPMR_FRAME_SYNC_1 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_2) == 13U, "INV_DPMR_FRAME_SYNC_2 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_3) == 13U, "INV_DPMR_FRAME_SYNC_3 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_4) == 25U, "INV_DPMR_FRAME_SYNC_4 length");
}

static void
test_sync_pattern_pairs(void) {
    assert(strcmp(DPMR_FRAME_SYNC_1, INV_DPMR_FRAME_SYNC_4) == 0);
    assert(strcmp(DPMR_FRAME_SYNC_4, INV_DPMR_FRAME_SYNC_1) == 0);
    assert(strcmp(DPMR_FRAME_SYNC_2, INV_DPMR_FRAME_SYNC_2) != 0);
    assert(strcmp(DPMR_FRAME_SYNC_3, INV_DPMR_FRAME_SYNC_3) != 0);
}

static void
test_synctype_helpers(void) {
    for (int synctype = DSD_SYNC_DPMR_FS1_POS; synctype <= DSD_SYNC_DPMR_FS4_NEG; synctype++) {
        assert(DSD_SYNC_IS_DPMR(synctype));
        assert(dsd_dispatch_matches_dpmr(synctype));
    }

    _Static_assert(!DSD_SYNC_IS_DPMR(DSD_SYNC_DSTAR_HD_NEG), "D-STAR header is not dPMR");
    _Static_assert(!DSD_SYNC_IS_DPMR(DSD_SYNC_NXDN_POS), "NXDN is not dPMR");
    assert(!dsd_dispatch_matches_dpmr(DSD_SYNC_DSTAR_HD_NEG));
    assert(!dsd_dispatch_matches_dpmr(DSD_SYNC_NXDN_POS));
}

int
main(void) {
    test_sync_pattern_lengths();
    test_sync_pattern_pairs();
    test_synctype_helpers();
    printf("DPMR_SYNC_DISPATCH: OK\n");
    return 0;
}
