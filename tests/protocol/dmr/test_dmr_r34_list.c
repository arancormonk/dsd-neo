// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for DMR rate 3/4 list-Viterbi decoding.
 */

#include <assert.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_r34_reference_vectors.h"
#include "dsd-neo/core/safe_api.h"

static void
assert_metrics_sorted(const dmr_r34_candidate* candidates, int count) {
    for (int i = 1; i < count; i++) {
        assert(candidates[i - 1].metric <= candidates[i].metric);
    }
}

static int
candidate_contains_payload(const dmr_r34_candidate* candidates, int count, const uint8_t payload[18]) {
    for (int i = 0; i < count; i++) {
        if (memcmp(candidates[i].bytes18, payload, 18) == 0) {
            return 1;
        }
    }
    return 0;
}

static void
test_clean_payload_is_first_candidate(void) {
    const dmr_r34_reference_vector* reference = &k_dmr_r34_reference_vectors[0];
    dmr_r34_candidate candidates[16];
    int count = 0;

    assert(dmr_r34_viterbi_decode_list(reference->dibits, NULL, candidates, 16, &count) == 0);
    assert(count == 16);
    assert_metrics_sorted(candidates, count);
    assert(candidates[0].metric == 0);
    assert(memcmp(candidates[0].bytes18, reference->payload, 18) == 0);
}

static void
test_weighted_clean_payload_is_first_candidate(void) {
    const dmr_r34_reference_vector* reference = &k_dmr_r34_reference_vectors[1];
    uint8_t reliability[98];
    dmr_r34_candidate candidates[16];
    int count = 0;

    DSD_MEMSET(reliability, 200, sizeof(reliability));
    assert(dmr_r34_viterbi_decode_list(reference->dibits, reliability, candidates, 16, &count) == 0);
    assert(count == 16);
    assert_metrics_sorted(candidates, count);
    assert(candidates[0].metric == 0);
    assert(memcmp(candidates[0].bytes18, reference->payload, 18) == 0);
}

static void
test_candidate_list_retains_clean_payload_after_single_dibit_error(void) {
    const dmr_r34_reference_vector* reference = &k_dmr_r34_reference_vectors[2];
    uint8_t dibits[98];
    dmr_r34_candidate candidates[32];
    int count = 0;

    DSD_MEMCPY(dibits, reference->dibits, sizeof(dibits));
    dibits[37] = (uint8_t)((dibits[37] + 1u) & 0x3u);

    assert(dmr_r34_viterbi_decode_list(dibits, NULL, candidates, 32, &count) == 0);
    assert(count == 32);
    assert_metrics_sorted(candidates, count);
    assert(candidate_contains_payload(candidates, count, reference->payload));
}

static void
test_invalid_arguments_are_rejected(void) {
    uint8_t dibits[98] = {0};
    dmr_r34_candidate candidates[2];
    int count = 0;

    assert(dmr_r34_viterbi_decode_list(NULL, NULL, candidates, 2, &count) != 0);
    assert(dmr_r34_viterbi_decode_list(dibits, NULL, NULL, 2, &count) != 0);
    assert(dmr_r34_viterbi_decode_list(dibits, NULL, candidates, 0, &count) != 0);
    assert(dmr_r34_viterbi_decode_list(dibits, NULL, candidates, 2, NULL) != 0);
}

int
main(void) {
    test_clean_payload_is_first_candidate();
    test_weighted_clean_payload_is_first_candidate();
    test_candidate_list_retains_clean_payload_after_single_dibit_error();
    test_invalid_arguments_are_rejected();
    printf("DMR_R34_LIST: OK\n");
    return 0;
}
