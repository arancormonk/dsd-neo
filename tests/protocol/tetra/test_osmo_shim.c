#include <stdio.h>
#include <stdint.h>
#include "../../../src/protocol/tetra/osmo_sources/osmo_shim_map.h"

int main(void) {
    struct { int8_t in; uint16_t expect; } cases[] = {
        {-127, 0x0000},
        {-100, 0x0000},
        {-64,  0x0000},
        {-63,  0x7FFF},
        {-1,   0x7FFF},
        {0,    0x7FFF},
        {1,    0x7FFF},
        {63,   0x7FFF},
        {64,   0xFFFF},
        {100,  0xFFFF},
        {127,  0xFFFF},
    };
    int n = sizeof(cases)/sizeof(cases[0]);
    int failed = 0;
    for (int i = 0; i < n; i++) {
        uint16_t rc = osmo_soft_to_cost(cases[i].in);
        if (rc != cases[i].expect) {
            fprintf(stderr, "FAIL: in=%d got=0x%04x expect=0x%04x\n", cases[i].in, rc, cases[i].expect);
            failed++;
        } else {
            fprintf(stderr, "OK: in=%4d -> 0x%04x\n", cases[i].in, rc);
        }
    }
    if (failed) return 2;
    fprintf(stderr, "All osmo shim mapping tests passed\n");
    return 0;
}
