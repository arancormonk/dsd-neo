// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 NID parity + NAC decode tests.
 *
 * Focuses on BCH(63,16) decode success for a trivial all-zero codeword and
 * the explicit parity-bit check behavior.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/protocol/p25/p25p1_check_nid.h>

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_eq_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Case 1: All-zero BCH(63,16) codeword (valid). Expect NAC=0, DUID="00", parity=0 accepted.
    char bch_code[63];
    memset(bch_code, 0, sizeof(bch_code));
    int new_nac = -1;
    char new_duid[3] = {0};
    int r1 = check_NID(bch_code, &new_nac, new_duid, /*parity*/ 0);
    rc |= expect_eq_int("decode ok", r1, 1);
    rc |= expect_eq_int("NAC=0", new_nac, 0);
    rc |= expect_eq_str("DUID=00", new_duid, "00");

    // Case 2: Same decoded fields but parity bit mismatched → return -1
    new_nac = -1;
    new_duid[0] = new_duid[1] = '\0';
    int r2 = check_NID(bch_code, &new_nac, new_duid, /*parity*/ 1);
    rc |= expect_eq_int("parity mismatch", r2, -1);
    rc |= expect_eq_int("NAC still 0", new_nac, 0);
    rc |= expect_eq_str("DUID still 00", new_duid, "00");

    return rc;
}
