// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/protocol/dmr/r34_viterbi.h>

// Forward-declare legacy decoder to avoid including broad dsd.h
uint32_t dmr_34(uint8_t* input, uint8_t treturn[18]);

static void
gen_pattern(uint8_t* dibits, int len, unsigned seed) {
    // Simple LCG to fill 2-bit dibits deterministically
    uint32_t x = seed ? seed : 1u;
    for (int i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        dibits[i] = (uint8_t)((x >> 24) & 0x03u);
    }
}

static void
run_case(unsigned seed) {
    uint8_t in[98];
    uint8_t a[18], b[18];
    gen_pattern(in, 98, seed);
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    // Legacy
    uint8_t in_copy[98];
    for (int i = 0; i < 98; i++) {
        in_copy[i] = in[i];
    }
    dmr_34(in_copy, a);
    // New
    int rc = dmr_r34_viterbi_decode(in, b);
    assert(rc == 0);
    // Compare payloads
    assert(memcmp(a, b, 18) == 0);
}

int
main(void) {
    for (unsigned s = 0; s < 8; s++) {
        run_case(0xC0FFEEu + s);
    }
    printf("DMR R3/4 Viterbi ≡ legacy: OK\n");
    return 0;
}
