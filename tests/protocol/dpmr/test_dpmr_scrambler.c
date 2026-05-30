// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression vector for the dPMR CCH scrambler used before Hamming decode.
 */

#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got 0x%X want 0x%X\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bit(const char* tag, int index, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s[%d]: got %u want %u\n", tag, index, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

int
main(void) {
    static const uint8_t expected[72] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1, 1,
        0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1,
    };

    uint8_t input[72];
    uint8_t output[72];
    DSD_MEMSET(input, 0, sizeof(input));
    DSD_MEMSET(output, 0, sizeof(output));

    uint32_t lfsr = 0x1FFU;
    dpmr_scrambled_pmr_bits(&lfsr, input, output, 72U);

    int rc = expect_u32("dpmr-scrambler-advanced-state", lfsr, 0x1B3U);
    for (int i = 0; i < 72; i++) {
        rc |= expect_bit("dpmr-scrambler-output", i, output[i], expected[i]);
    }

    if (rc == 0) {
        printf("DPMR_SCRAMBLER: OK\n");
    }
    return rc;
}
