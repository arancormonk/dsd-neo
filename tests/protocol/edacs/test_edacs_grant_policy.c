// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * EDACS grant-policy compatibility profile test.
 *
 * This is a focused evaluator-level regression for the hold/private allow-list
 * settings used by EDACS grant paths. A direct EDACS parser/tuning harness is
 * deferred; this test locks the shared policy contract used by those call sites.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", tag);
        return 1;
    }
    return 0;
}

static int
seed_exact(dsd_state* st, uint32_t id, const char* mode, const char* name) {
    dsd_tg_policy_entry row;
    if (dsd_tg_policy_make_legacy_exact_entry(id, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &row) != 0) {
        return -1;
    }
    return dsd_tg_policy_upsert_legacy_exact(st, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST);
}

int
main(void) {
    int rc = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_decision decision;

    if (!opts || !st) {
        fprintf(stderr, "FAIL: alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !st ? " dsd_state" : "");
        free(st);
        free(opts);
        return 1;
    }

    opts->trunk_use_allow_list = 1;
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;

    rc |= expect_true(
        "group unknown blocked in allow-list",
        dsd_tg_policy_evaluate_group_call(opts, st, 1100U, 2100U, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0
            && decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true("seed group allow", seed_exact(st, 1100U, "A", "ALLOW-GRP") == 0);
    rc |= expect_true(
        "group known allowed",
        dsd_tg_policy_evaluate_group_call(opts, st, 1100U, 2100U, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0
            && decision.tune_allowed == 1);

    rc |= expect_true("seed group block", seed_exact(st, 1200U, "B", "BLOCK-GRP") == 0);
    st->tg_hold = 1200U;
    rc |= expect_true(
        "hold match still blocked by mode in compat profile",
        dsd_tg_policy_evaluate_group_call(opts, st, 1200U, 2200U, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0
            && decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);
    st->tg_hold = 0;

    rc |= expect_true(
        "private unknown blocked in allow-list",
        dsd_tg_policy_evaluate_private_call(opts, st, 9002U, 9001U, 0, 0, DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                            DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                == 0
            && decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true("seed private allow source", seed_exact(st, 9002U, "A", "ALLOW-SRC") == 0);
    rc |= expect_true("private known source allowed",
                      dsd_tg_policy_evaluate_private_call(opts, st, 9002U, 9001U, 0, 0,
                                                          DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                          DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                              == 0
                          && decision.tune_allowed == 1);

    rc |= expect_true("seed private block dst", seed_exact(st, 9001U, "DE", "ENC-LOCKOUT") == 0);
    rc |= expect_true("private explicit mode block",
                      dsd_tg_policy_evaluate_private_call(opts, st, 9002U, 9001U, 0, 0,
                                                          DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                          DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                              == 0
                          && decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);

    if (rc == 0) {
        printf("EDACS_GRANT_POLICY: OK\n");
    }

    free(st);
    free(opts);
    return rc;
}
