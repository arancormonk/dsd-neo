// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/protocol/p25/p25_lsd.h>

int
main(void) {
    // bits16: 8 data bits + 8 parity bits; stub treats data==parity as valid.
    uint8_t bits[16];
    memset(bits, 0, sizeof(bits));
    int rc = p25_lsd_fec_16x8(bits);
    if (rc != 1) {
        fprintf(stderr, "p25_lsd_fec_16x8 scaffold expected 1, got %d\n", rc);
        return 1;
    }
    fprintf(stderr, "p25_lsd scaffold OK\n");
    return 0;
}
