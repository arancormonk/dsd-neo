// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %d want %d)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_double_eq(const char* label, double got, double want) {
    if (fabs(got - want) > 1e-9) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %.9f want %.9f)\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
add_policy(dsd_state* state, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry entry;
    if (dsd_tg_policy_make_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &entry) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_exact(state, &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

static int
test_unicode_wrappers_and_shared_sync_state(void) {
    int rc = 0;
    rc |= expect_int_eq("unicode wrapper delegates", ui_unicode_supported(), dsd_unicode_supported());
    rc |= expect_int_eq("block glyph wrapper delegates", ui_block_glyphs_supported(),
                        dsd_unicode_block_glyphs_supported());

    rc |= expect_int_eq("initial sync type", ncurses_last_synctype, DSD_SYNC_NONE);
    ncurses_last_synctype = DSD_SYNC_P25P1_POS;
    rc |= expect_int_eq("shared sync type is mutable", ncurses_last_synctype, DSD_SYNC_P25P1_POS);
    ncurses_last_synctype = DSD_SYNC_NONE;
    return rc;
}

static int
test_int_selection_helpers(void) {
    int a = 7;
    int b = -3;
    swap_int_local(&a, &b);

    int rc = 0;
    rc |= expect_int_eq("swap lhs", a, -3);
    rc |= expect_int_eq("swap rhs", b, 7);

    int low = 1;
    int mid = 4;
    int high = 9;
    rc |= expect_true("cmp less", cmp_int_asc(&low, &mid) < 0);
    rc |= expect_true("cmp equal", cmp_int_asc(&mid, &mid) == 0);
    rc |= expect_true("cmp greater", cmp_int_asc(&high, &mid) > 0);

    int vals_min[] = {9, -4, 3, 3, 7, 0, 11};
    int vals_mid[] = {9, -4, 3, 3, 7, 0, 11};
    int vals_max[] = {9, -4, 3, 3, 7, 0, 11};
    rc |= expect_int_eq("select min", select_k_int_local(vals_min, 7, 0), -4);
    rc |= expect_int_eq("select duplicate median", select_k_int_local(vals_mid, 7, 3), 3);
    rc |= expect_int_eq("select max", select_k_int_local(vals_max, 7, 6), 11);
    return rc;
}

static int
test_percentiles_u8(void) {
    uint8_t samples[] = {10, 1, 40, 30, 20};
    double p50 = -1.0;
    double p95 = -1.0;

    int rc = 0;
    rc |= expect_int_eq("null percentile source", compute_percentiles_u8(NULL, 5, &p50, &p95), 0);
    rc |= expect_int_eq("empty percentile source", compute_percentiles_u8(samples, 0, &p50, &p95), 0);
    rc |= expect_int_eq("percentile success", compute_percentiles_u8(samples, 5, &p50, &p95), 1);
    rc |= expect_double_eq("percentile p50", p50, 20.0);
    rc |= expect_double_eq("percentile p95", p95, 40.0);
    rc |= expect_int_eq("percentile accepts null outputs", compute_percentiles_u8(samples, 5, NULL, NULL), 1);
    return rc;
}

static int
test_lockout_label_policy_lookup(void) {
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!state) {
        DSD_FPRINTF(stderr, "FAIL: state alloc\n");
        return 1;
    }

    int rc = 0;
    rc |= expect_true("add B policy", add_policy(state, 123U, "B", "BLOCK") == 0);
    rc |= expect_true("add DE policy", add_policy(state, 456U, "DE", "ENC-BLOCK") == 0);
    rc |= expect_true("add A policy", add_policy(state, 789U, "A", "ALLOW") == 0);
    rc |= expect_true("add SG policy", add_policy(state, 321U, "DE", "SG-ENC-BLOCK") == 0);

    dsd_tg_policy_entry range;
    rc |= expect_true("make range policy",
                      dsd_tg_policy_make_exact_entry(1000U, "B", "RANGE-BLOCK", DSD_TG_POLICY_SOURCE_IMPORTED, &range)
                          == 0);
    range.id_end = 1099U;
    range.is_range = 1U;
    rc |= expect_true("add range policy", dsd_tg_policy_add_range_entry(state, &range) == 0);

    rc |= expect_int_eq("null state not locked", ui_is_locked_from_label(NULL, "TG: 123"), 0);
    rc |= expect_int_eq("null label not locked", ui_is_locked_from_label(state, NULL), 0);
    rc |= expect_int_eq("empty label not locked", ui_is_locked_from_label(state, ""), 0);
    rc |= expect_int_eq("missing target prefix not locked", ui_is_locked_from_label(state, "SRC: 123"), 0);
    rc |= expect_int_eq("invalid target not locked", ui_is_locked_from_label(state, "TG: abc"), 0);
    rc |= expect_int_eq("zero target not locked", ui_is_locked_from_label(state, "TG: 0"), 0);
    rc |= expect_int_eq("overflow target not locked", ui_is_locked_from_label(state, "TG: 4294967296"), 0);
    rc |= expect_int_eq("TG B mode locks", ui_is_locked_from_label(state, "Voice TG: 123 src 4"), 1);
    rc |= expect_int_eq("TGT DE mode locks", ui_is_locked_from_label(state, "Call TGT:456 slot 2"), 1);
    rc |= expect_int_eq("SG DE mode locks", ui_is_locked_from_label(state, "MFID90 GRG Grant: 82F2 SG: 321;"), 1);
    rc |= expect_int_eq("allow mode does not lock", ui_is_locked_from_label(state, "TG: 789"), 0);
    rc |= expect_int_eq("range-only match does not lock label", ui_is_locked_from_label(state, "TG: 1005"), 0);

    const time_t now = time(NULL);
    state->p25_enc_tg_cache_tg[0] = 21001U;
    state->p25_enc_tg_cache_until[0] = now + 10;
    state->p25_enc_tg_cache_is_group[0] = 1U;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    rc |= expect_int_eq("transient cache requires p25 sync",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TG: 21001;"), 0);
    state->synctype = DSD_SYNC_P25P1_POS;
    rc |= expect_int_eq("transient enc cache locks active TG",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TG: 21001;"), 1);
    rc |= expect_int_eq("transient enc cache locks active SG",
                        ui_is_transient_enc_locked_from_label(state, "MFID90 GRG Grant: 82F2 SG: 21001;"), 1);
    rc |= expect_int_eq("group transient cache does not lock same numeric private target",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TGT: 21001;"), 0);
    state->p25_enc_tg_cache_tg[1] = 21001U;
    state->p25_enc_tg_cache_until[1] = now + 10;
    state->p25_enc_tg_cache_is_group[1] = 0U;
    rc |= expect_int_eq("private transient cache locks active TGT",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TGT: 21001;"), 1);
    rc |= expect_int_eq("private voice cache does not color data target",
                        ui_is_transient_enc_locked_from_label(state, "Active Data Ch: 82F2 TGT: 21001;"), 0);
    rc |= expect_int_eq("transient enc cache does not mutate policy lock helper",
                        ui_is_locked_from_label(state, "Active Ch: 82F2 TG: 21001;"), 0);
    state->p25_enc_tg_cache_until[0] = now - 1;
    rc |= expect_int_eq("expired transient enc cache does not lock",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TG: 21001;"), 0);
    state->p25_enc_tg_cache_until[0] = now + 10;
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->lastsynctype = DSD_SYNC_NONE;
    rc |= expect_int_eq("non-p25 transient enc cache does not lock",
                        ui_is_transient_enc_locked_from_label(state, "Active Ch: 82F2 TG: 21001;"), 0);

    dsd_state_ext_free_all(state);
    free(state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_unicode_wrappers_and_shared_sync_state();
    rc |= test_int_selection_helpers();
    rc |= test_percentiles_u8();
    rc |= test_lockout_label_policy_lookup();
    return rc ? 1 : 0;
}
