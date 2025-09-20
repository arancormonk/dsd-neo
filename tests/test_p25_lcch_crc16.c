// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Use existing bridge in p25 CRC module
int crc16_lb_bridge(int payload[190], int len);

int
main(void) {
    int bits[190];
    memset(bits, 0, sizeof(bits));
    // Heuristic call: length 164 bits + 16 CRC bits present in array => -1 for mismatch is acceptable in stub
    int rc = crc16_lb_bridge(bits, 164);
    // Scaffold acceptance: any return value is acceptable here; we only care
    // that the symbol is linked and runnable. Future vectors will assert rc==0.
    fprintf(stderr, "LCCH CRC16 scaffold ran (rc=%d)\n", rc);
    return 0;
}
