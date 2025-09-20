// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Forward declare p25_12 decoder from src/protocol/p25/p25_12.c
int p25_12(uint8_t* input, uint8_t treturn[12]);

int
main(void) {
    uint8_t dibits[98];
    uint8_t out[12];
    memset(dibits, 0, sizeof(dibits));
    memset(out, 0xFF, sizeof(out));
    int rc = p25_12(dibits, out);
    // rc is error tally; we accept any rc>=0 for scaffolding
    if (rc < 0) {
        fprintf(stderr, "p25_12 returned %d\n", rc);
        return 1;
    }
    fprintf(stderr, "p25_12 scaffold OK (rc=%d)\n", rc);
    return 0;
}
