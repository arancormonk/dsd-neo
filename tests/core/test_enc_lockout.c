// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for the session-permanent encrypted-target lockout ledger:
 * arming, group/private keying, key-epoch invalidation and re-lock, clear
 * release, purge, LRU eviction, and the opts-gated blocking predicate.
 */

#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s: expectation failed\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    dsd_state* state = (dsd_state*)calloc(1, sizeof(dsd_state));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    if (!state || !opts) {
        free(state);
        free(opts);
        return 1;
    }
    dsd_enc_lockout_init(state);
    opts->trunk_tune_enc_calls = 0;

    // Arming and keyed lookup.
    rc |= expect_true("note rejects target 0", dsd_enc_lockout_note(state, 0U, 1, 0x84, 1) == 0);
    rc |= expect_true("first note newly locks", dsd_enc_lockout_note(state, 100U, 1, 0x84, 0x1234) == 1);
    rc |=
        expect_true("same-epoch refresh is not newly locking", dsd_enc_lockout_note(state, 100U, 1, 0x84, 0x1234) == 0);
    rc |= expect_true("entry active", dsd_enc_lockout_entry_active(state, 100U, 1));
    rc |= expect_true("blocked while lockout enabled", dsd_enc_lockout_is_blocked(opts, state, 100U, 1));
    rc |= expect_true("group entry does not match private key", !dsd_enc_lockout_entry_active(state, 100U, 0));
    rc |= expect_true("unknown target not active", !dsd_enc_lockout_entry_active(state, 101U, 1));
    rc |= expect_true("active count is 1", dsd_enc_lockout_active_count(state) == 1);

    dsd_enc_lockout_entry entry;
    rc |=
        expect_true("lookup copies evidence", dsd_enc_lockout_lookup(state, 100U, 1, &entry) == 1 && entry.algid == 0x84
                                                  && entry.keyid == 0x1234 && entry.hits == 2U && entry.is_group == 1U);

    // Unknown-algid notes keep previously recorded evidence.
    (void)dsd_enc_lockout_note(state, 100U, 1, DSD_ENC_LOCKOUT_ALGID_UNKNOWN, 0);
    rc |= expect_true("unknown algid keeps evidence", dsd_enc_lockout_lookup(state, 100U, 1, &entry) == 1
                                                          && entry.algid == 0x84 && entry.keyid == 0x1234);

    // Follow mode makes the ledger inert without touching entries.
    opts->trunk_tune_enc_calls = 1;
    rc |= expect_true("follow mode is never blocked", !dsd_enc_lockout_is_blocked(opts, state, 100U, 1));
    rc |= expect_true("follow mode retains entry", dsd_enc_lockout_entry_active(state, 100U, 1));
    opts->trunk_tune_enc_calls = 0;

    // Key-epoch invalidation: entry retained, blocking suspended, one
    // re-confirmation re-locks at the new epoch.
    dsd_enc_lockout_bump_key_epoch(state);
    rc |= expect_true("stale epoch stops blocking", !dsd_enc_lockout_is_blocked(opts, state, 100U, 1));
    rc |= expect_true("stale entry retained", dsd_enc_lockout_lookup(state, 100U, 1, NULL) == 1);
    rc |= expect_true("stale entry not counted active", dsd_enc_lockout_active_count(state) == 0);
    rc |= expect_true("re-confirmation is newly locking", dsd_enc_lockout_note(state, 100U, 1, 0x84, 0x1234) == 1);
    rc |= expect_true("re-locked at current epoch", dsd_enc_lockout_is_blocked(opts, state, 100U, 1));

    // Clear evidence releases the entry outright.
    rc |= expect_true("release removes entry", dsd_enc_lockout_release(state, 100U, 1) == 1);
    rc |= expect_true("released entry gone", dsd_enc_lockout_lookup(state, 100U, 1, NULL) == 0);
    rc |= expect_true("double release is a no-op", dsd_enc_lockout_release(state, 100U, 1) == 0);

    // Private and group entries with the same numeric id stay independent.
    (void)dsd_enc_lockout_note(state, 200U, 1, 0x81, 1);
    (void)dsd_enc_lockout_note(state, 200U, 0, 0xAA, 2);
    rc |= expect_true("both same-id entries active", dsd_enc_lockout_active_count(state) == 2);
    (void)dsd_enc_lockout_release(state, 200U, 0);
    rc |= expect_true("private release keeps group entry",
                      dsd_enc_lockout_entry_active(state, 200U, 1) && !dsd_enc_lockout_lookup(state, 200U, 0, NULL));

    // Purge drops everything but preserves the epoch counter.
    const uint64_t epoch_before = state->enc_lockout_key_epoch;
    dsd_enc_lockout_clear_all(state);
    rc |= expect_true("purge drops entries",
                      dsd_enc_lockout_active_count(state) == 0 && dsd_enc_lockout_lookup(state, 200U, 1, NULL) == 0);
    rc |= expect_true("purge preserves epoch", state->enc_lockout_key_epoch == epoch_before);

    // Capacity: filling the table evicts the oldest entry, never wedges.
    for (uint32_t i = 0; i < (uint32_t)DSD_ENC_LOCKOUT_MAX; i++) {
        (void)dsd_enc_lockout_note(state, 1000U + i, 1, 0x84, (int)i);
    }
    rc |= expect_true("table full", dsd_enc_lockout_active_count(state) == DSD_ENC_LOCKOUT_MAX);
    (void)dsd_enc_lockout_note(state, 5000U, 1, 0x84, 99);
    rc |= expect_true("overflow still locks new target", dsd_enc_lockout_entry_active(state, 5000U, 1));
    rc |= expect_true("overflow keeps table at capacity", dsd_enc_lockout_active_count(state) == DSD_ENC_LOCKOUT_MAX);

    free(state);
    free(opts);
    return rc;
}
