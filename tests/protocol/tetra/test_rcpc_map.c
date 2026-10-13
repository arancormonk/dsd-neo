// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * RCPC puncturing schemes against their definitions: every type-3 bit j of every scheme carries mother-code bit
 * k = Period * ((i - 1) div t) + P(i - t * ((i - 1) div t)) (EN 300 392-2 clause 8.2.3.1.2, EN 300 395-2 clause
 * 5.4.3.2), with the P coefficients, t, Period and i(j) written out here from EN 300 392-2 clauses 8.2.3.1.3 to
 * 8.2.3.1.6 and EN 300 395-2 clauses 5.5.2.1, 5.5.2.2 and 5.6.2.1. Within one block no mother bit is sent twice, and
 * tetra_rcpc_puncture_by_id() and tetra_rcpc_depuncture_by_id() follow the same map.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    int id;
    const char* name;
    int t;
    int period;
    uint8_t p[17];
    int skip;  /* i = j + (j - 1) div skip; 0 for i = j */
    int coded; /* type-3 bits in one block */
    int mother;
} scheme;

static const scheme k_schemes[] = {
    {TETRA_RCPC_PUNCT_2_3, "rate 2/3", 3, 8, {1, 2, 5}, 0, 432, 1152},
    {TETRA_RCPC_PUNCT_1_3, "rate 1/3", 6, 8, {1, 2, 3, 5, 6, 7}, 0, 432, 576},
    {TETRA_RCPC_PUNCT_292_432, "rate 292/432", 3, 8, {1, 2, 5}, 65, 432, 1168},
    {TETRA_RCPC_PUNCT_148_432, "rate 148/432", 6, 8, {1, 2, 3, 5, 6, 7}, 35, 432, 592},
    {TETRA_RCPC_PUNCT_SPEECH_8_12, "speech rate 8/12", 3, 6, {1, 2, 4}, 0, 168, 336},
    {TETRA_RCPC_PUNCT_SPEECH_8_18, "speech rate 8/18", 9, 12, {1, 2, 3, 4, 5, 7, 8, 10, 11}, 0, 162, 216},
    {
        TETRA_RCPC_PUNCT_SPEECH_8_17,
        "speech rate 8/17",
        17,
        24,
        {1, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23},
        0,
        81,
        114,
    },
};

static int
expected_k(const scheme* s, int j) {
    const int i = s->skip ? j + ((j - 1) / s->skip) : j;
    const int block = (i - 1) / s->t;
    return (s->period * block) + s->p[(i - 1) - (s->t * block)];
}

static int
check_scheme(const scheme* s) {
    static uint8_t seen[1200];
    static uint8_t mother[1200];
    static uint8_t coded[432];
    static uint16_t costs[432];
    static uint16_t restored[1200];
    DSD_MEMSET(seen, 0, sizeof(seen));
    for (int j = 1; j <= s->coded; j++) {
        const int k = expected_k(s, j);
        const int got = tetra_rcpc_map_j_to_k(s->id, (uint32_t)j);
        if (got != k || k < 1 || k > s->mother || seen[k - 1]) {
            DSD_FPRINTF(stderr, "%s: j=%d maps to k=%d, expected %d within %d, once\n", s->name, j, got, k, s->mother);
            return 1;
        }
        seen[k - 1] = 1;
    }
    for (int k = 0; k < s->mother; k++) {
        mother[k] = (uint8_t)((k * 5 + 3) % 7 < 3);
    }
    if (tetra_rcpc_puncture_by_id(s->id, mother, s->mother, coded, s->coded) != s->coded) {
        DSD_FPRINTF(stderr, "%s: puncture failed\n", s->name);
        return 1;
    }
    for (int j = 1; j <= s->coded; j++) {
        if (coded[j - 1] != mother[expected_k(s, j) - 1]) {
            DSD_FPRINTF(stderr, "%s: punctured bit %d is not mother bit %d\n", s->name, j, expected_k(s, j));
            return 1;
        }
        costs[j - 1] = coded[j - 1] ? 0xFFFFu : 0x0000u;
    }
    if (tetra_rcpc_depuncture_by_id(s->id, costs, s->coded, restored, s->mother) != s->mother) {
        DSD_FPRINTF(stderr, "%s: depuncture failed\n", s->name);
        return 1;
    }
    for (int k = 0; k < s->mother; k++) {
        uint16_t want = TETRA_SOFT_NEUTRAL;
        if (seen[k]) {
            want = mother[k] ? 0xFFFFu : 0x0000u;
        }
        if (restored[k] != want) {
            DSD_FPRINTF(stderr, "%s: mother bit %d restored as 0x%04x\n", s->name, k + 1, restored[k]);
            return 1;
        }
    }
    return 0;
}

int
main(void) {
    int rc = 0;
    for (size_t i = 0; i < sizeof(k_schemes) / sizeof(k_schemes[0]); i++) {
        rc |= check_scheme(&k_schemes[i]);
    }
    if (tetra_rcpc_map_j_to_k(7, 1) != -1 || tetra_rcpc_map_j_to_k(-1, 1) != -1
        || tetra_rcpc_map_j_to_k(TETRA_RCPC_PUNCT_2_3, 0) != -1) {
        DSD_FPRINTF(stderr, "an unknown scheme or bit 0 must map to -1\n");
        rc = 1;
    }
    puts(rc ? "FAILED test_rcpc_map" : "PASS test_rcpc_map");
    return rc;
}
