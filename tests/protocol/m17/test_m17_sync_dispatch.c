// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for M17 sync constants and dispatch routing.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_m17(int synctype);
void dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state);

static int lsf_calls;
static int pkt_calls;
static int skip_calls;
static int skipped_dibits;
static int str_calls;
static int brt_calls;

static void
reset_calls(void) {
    lsf_calls = 0;
    pkt_calls = 0;
    skip_calls = 0;
    skipped_dibits = 0;
    str_calls = 0;
    brt_calls = 0;
}

void
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    skip_calls++;
    skipped_dibits += count;
}

void
processM17LSF(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    lsf_calls++;
}

void
processM17PKT(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    pkt_calls++;
}

void
processM17STR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    str_calls++;
}

void
processM17BRT(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    brt_calls++;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(M17_LSF) == 9U, "M17_LSF length");
    _Static_assert(sizeof(M17_STR) == 9U, "M17_STR length");
    _Static_assert(sizeof(M17_PRE) == 9U, "M17_PRE length");
    _Static_assert(sizeof(M17_PIV) == 9U, "M17_PIV length");
    _Static_assert(sizeof(M17_BRT) == 9U, "M17_BRT length");
    _Static_assert(sizeof(M17_PKT) == 9U, "M17_PKT length");
    _Static_assert(sizeof(M17_EOT) == 9U, "M17_EOT length");
    _Static_assert(sizeof(M17_EOT_INV) == 9U, "M17_EOT_INV length");
    _Static_assert(sizeof(M17_PRE_LSF) == 17U, "M17_PRE_LSF length");
    _Static_assert(sizeof(M17_PIV_LSF) == 17U, "M17_PIV_LSF length");
    assert(strcmp(M17_STR, "33331131") == 0);
    assert(strcmp(M17_PKT, "13113333") == 0);
}

static void
test_synctype_helpers(void) {
    static const int m17_synctypes[] = {
        DSD_SYNC_M17_STR_POS, DSD_SYNC_M17_STR_NEG, DSD_SYNC_M17_LSF_POS, DSD_SYNC_M17_LSF_NEG,
        DSD_SYNC_M17_BRT_POS, DSD_SYNC_M17_BRT_NEG, DSD_SYNC_M17_PKT_POS, DSD_SYNC_M17_PKT_NEG,
        DSD_SYNC_M17_PRE_POS, DSD_SYNC_M17_PRE_NEG, DSD_SYNC_M17_EOT_POS, DSD_SYNC_M17_EOT_NEG,
    };

    for (size_t i = 0; i < sizeof m17_synctypes / sizeof m17_synctypes[0]; i++) {
        assert(DSD_SYNC_IS_M17(m17_synctypes[i]));
        assert(dsd_dispatch_matches_m17(m17_synctypes[i]));
    }

    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_STR_NEG), "M17 STR negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_LSF_NEG), "M17 LSF negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_BRT_NEG), "M17 BRT negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_PKT_NEG), "M17 PKT negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_PRE_NEG), "M17 PRE negative is inverted");
    _Static_assert(DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_EOT_NEG), "M17 EOT negative is inverted");
    _Static_assert(!DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_STR_POS), "M17 STR positive is not inverted");
    _Static_assert(!DSD_SYNC_IS_INVERTED(DSD_SYNC_M17_LSF_POS), "M17 LSF positive is not inverted");

    _Static_assert(!DSD_SYNC_IS_M17(DSD_SYNC_DSTAR_VOICE_POS), "D-STAR voice is not M17");
    _Static_assert(!DSD_SYNC_IS_M17(DSD_SYNC_YSF_POS), "YSF is not M17");
    assert(!dsd_dispatch_matches_m17(DSD_SYNC_DSTAR_VOICE_POS));
    assert(!dsd_dispatch_matches_m17(DSD_SYNC_YSF_POS));
}

static void
test_synctype_names(void) {
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_STR_POS), "M17") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_LSF_NEG), "M17") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_BRT_POS), "M17 BRT") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_PKT_NEG), "M17 PKT") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_PRE_POS), "M17 PRE") == 0);
    assert(strcmp(dsd_synctype_to_string(DSD_SYNC_M17_EOT_NEG), "M17 EOT") == 0);
}

static int
dispatch_one(int synctype) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_calls();

    state.synctype = synctype;
    dsd_dispatch_handle_m17(&opts, &state);
    return state.lastsynctype;
}

static void
test_preamble_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_PRE_POS);
    assert(skip_calls == 1);
    assert(skipped_dibits == 8);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PRE_NEG);
    assert(skip_calls == 1);
    assert(skipped_dibits == 8);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
}

static void
test_payload_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_LSF_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 1);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_LSF_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 1);
    assert(pkt_calls == 0);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PKT_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 1);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_PKT_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 1);
    assert(str_calls == 0);

    dispatch_one(DSD_SYNC_M17_STR_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 1);

    dispatch_one(DSD_SYNC_M17_STR_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 1);
}

static void
test_eot_dispatch(void) {
    int lastsynctype = dispatch_one(DSD_SYNC_M17_EOT_POS);
    assert(skip_calls == 1);
    assert(skipped_dibits == 184);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(lastsynctype == DSD_SYNC_NONE);

    lastsynctype = dispatch_one(DSD_SYNC_M17_EOT_NEG);
    assert(skip_calls == 1);
    assert(skipped_dibits == 184);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(lastsynctype == DSD_SYNC_NONE);
}

static void
test_bert_dispatch(void) {
    dispatch_one(DSD_SYNC_M17_BRT_POS);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(brt_calls == 1);

    dispatch_one(DSD_SYNC_M17_BRT_NEG);
    assert(skip_calls == 0);
    assert(lsf_calls == 0);
    assert(pkt_calls == 0);
    assert(str_calls == 0);
    assert(brt_calls == 1);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_synctype_names();
    test_preamble_dispatch();
    test_payload_dispatch();
    test_eot_dispatch();
    test_bert_dispatch();
    printf("M17_SYNC_DISPATCH: OK\n");
    return 0;
}
