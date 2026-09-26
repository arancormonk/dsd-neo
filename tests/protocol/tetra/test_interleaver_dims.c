#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>

int main(void) {
    struct { int bits; int expect; } cases[] = {
        {30, 1},
        {80, 11},
        {120, 11},
        {162, 11},
        {168, 13},
        {216, 101},
        {432, 103},
    };
    int n = sizeof(cases)/sizeof(cases[0]);
    int failed = 0;
    for (int i = 0; i < n; i++) {
        int rows = 0, cols = 0;
        int rc = tetra_get_interleaver_dims(0, cases[i].bits, &rows, &cols);
        if (rc != 0) {
            fprintf(stderr, "FAIL: bits=%d returned rc=%d\n", cases[i].bits, rc);
            failed++;
            continue;
        }
        if (rows != cases[i].expect) {
            fprintf(stderr, "FAIL: bits=%d rows=%d expect=%d\n", cases[i].bits, rows, cases[i].expect);
            failed++;
        } else {
            fprintf(stderr, "OK: bits=%d rows=%d cols=%d\n", cases[i].bits, rows, cols);
        }
    }
    if (failed) {
        fprintf(stderr, "%d tests failed\n", failed);
        return 2;
    }
    fprintf(stderr, "All interleaver-dim tests passed\n");
    return 0;
}
