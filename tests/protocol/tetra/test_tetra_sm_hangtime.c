// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA trunking SM hangtime getter/setter tests.  Phase 37.
 *
 * Tests:
 *  1.  After init, get_hangtime returns 0 (no override)
 *  2.  set_hangtime(30) → get_hangtime() returns 30
 *  3.  set_hangtime(0)  → get_hangtime() returns 0 (clears override)
 */

#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  PASS: %s\n", msg); \
            g_pass++; \
        } else { \
            printf("  FAIL: %s\n", msg); \
            g_fail++; \
        } \
    } while (0)

int main(void)
{
    printf("[TETRA SM hangtime]\n");

    tetra_sm_init();
    CHECK(tetra_sm_get_hangtime() == 0,
          "get_hangtime() returns 0 after init (no override)");

    tetra_sm_set_hangtime(30);
    CHECK(tetra_sm_get_hangtime() == 30,
          "set_hangtime(30) → get_hangtime() == 30");

    tetra_sm_set_hangtime(0);
    CHECK(tetra_sm_get_hangtime() == 0,
          "set_hangtime(0) clears override → get_hangtime() == 0");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
