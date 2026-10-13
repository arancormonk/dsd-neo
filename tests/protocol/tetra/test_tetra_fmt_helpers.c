// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA name helpers: the MAC-RESOURCE encryption mode element (EN 300 392-7 clause 6.5.1) and the MM status downlink
 * (EN 300 392-2 clause 16.10.48) names.
 */

#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK_STR(got, exp, msg)                                                                                       \
    do {                                                                                                               \
        if (strcmp((got), (exp)) == 0) {                                                                               \
            printf("  PASS: %s\n", msg);                                                                               \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            printf("  FAIL: %s  (got \"%s\", expected \"%s\")\n", msg, (got), (exp));                                  \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int
main(void) {
    printf("[TETRA name helpers]\n");

    CHECK_STR(tetra_enc_mode_name(0), "none", "enc_mode 0 == none");
    CHECK_STR(tetra_enc_mode_name(1), "reserved", "enc_mode 1 == reserved");
    CHECK_STR(tetra_enc_mode_name(2), "on(even)", "enc_mode 2 == encrypted, even key version");
    CHECK_STR(tetra_enc_mode_name(3), "on(odd)", "enc_mode 3 == encrypted, odd key version");

    CHECK_STR(tetra_mm_status_name(1), "change energy saving request", "status downlink 1");
    CHECK_STR(tetra_mm_status_name(7), "MS frequency bands request", "status downlink 7");
    CHECK_STR(tetra_mm_status_name(15), "reserved", "reserved status downlink");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
