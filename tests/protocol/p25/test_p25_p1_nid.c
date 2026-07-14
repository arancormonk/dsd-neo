// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 NID parity + NAC decode tests.
 *
 * Focuses on BCH(63,16) decode success for a trivial all-zero codeword and
 * the explicit parity-bit check behavior.
 */

#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Case 1: All-zero BCH(63,16) codeword (valid). Expect NAC=0, DUID="00", parity=0 accepted.
    char bch_code[63];
    DSD_MEMSET(bch_code, 0, sizeof(bch_code));
    struct p25p1_nid_result result = p25p1_nid_decode(bch_code, NULL, 0, /*parity*/ 0, 0);
    rc |= expect_eq_int("decode ok", result.status, NID_OK);
    rc |= expect_eq_int("NAC=0", result.nac, 0);
    rc |= expect_eq_int("DUID=0", result.duid, 0);

    // Case 2: Same decoded fields but parity bit mismatched with 0 errors.
    // The final parity bit is outside the BCH codeword, so successful BCH
    // decode accepts it as NID_PARITY_OVERRIDE (2).
    result = p25p1_nid_decode(bch_code, NULL, 0, /*parity*/ 1, 0);
    rc |= expect_eq_int("parity override (0 errors)", result.status, NID_PARITY_OVERRIDE);
    rc |= expect_eq_int("NAC still 0", result.nac, 0);
    rc |= expect_eq_int("DUID still 0", result.duid, 0);

    // Case 3: Single-bit error in codeword should be corrected by BCH
    DSD_MEMSET(bch_code, 0, sizeof(bch_code));
    bch_code[10] = 1; // flip one bit
    result = p25p1_nid_decode(bch_code, NULL, 0, /*parity*/ 0, 0);
    rc |= expect_eq_int("1-bit correctable", result.status, NID_OK);
    rc |= expect_eq_int("NAC=0 after corr", result.nac, 0);
    rc |= expect_eq_int("DUID=0 after corr", result.duid, 0);

    // Case 4: Un-decodable noise → decode fails (return 0)
    for (int i = 0; i < 63; i++) {
        bch_code[i] = (i & 1);
    }
    result = p25p1_nid_decode(bch_code, NULL, 0, /*parity*/ 0, 0);
    rc |= expect_eq_int("decode failure", result.status, NID_DECODE_FAIL);

    return rc;
}
