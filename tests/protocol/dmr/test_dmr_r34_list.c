// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for DMR rate 3/4 list-Viterbi decoding.
 */

#include <assert.h>
#include <dsd-neo/protocol/dmr/r34_viterbi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_r34_internal.h"
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
test_all_decoders_require_zero_flushing_state(void) {
    static const uint8_t expected_hard[18] = {
        0x02U, 0x55U, 0x0BU, 0x35U, 0x0FU, 0x9FU, 0x83U, 0x82U, 0x35U,
        0xDAU, 0x49U, 0xFBU, 0x52U, 0xACU, 0xE4U, 0x64U, 0x5BU, 0xA0U,
    };
    static const uint8_t expected_soft[18] = {
        0x02U, 0x55U, 0x0BU, 0x35U, 0x0FU, 0x9FU, 0x83U, 0x82U, 0x35U,
        0xDAU, 0x49U, 0xFBU, 0x52U, 0xACU, 0xE4U, 0x64U, 0x5AU, 0x28U,
    };
    static const uint8_t expected_list[18] = {
        0x02U, 0x55U, 0x0BU, 0x35U, 0x0FU, 0x9FU, 0x83U, 0x82U, 0x35U,
        0xDAU, 0x49U, 0xFBU, 0x52U, 0xACU, 0xE4U, 0x64U, 0x5AU, 0x0AU,
    };
    const dmr_r34_reference_vector* reference = &k_dmr_r34_reference_vectors[0];
    uint8_t hard_dibits[98];
    uint8_t soft_dibits[98];
    uint8_t reliability[98];
    uint8_t decoded[18];
    dmr_r34_candidate candidates[16];
    int count = 0;

    DSD_MEMCPY(hard_dibits, reference->dibits, sizeof(hard_dibits));
    hard_dibits[73] = 2U; /* Makes an unterminated state-4 path cheaper than the required state-0 path. */
    assert(dmr_r34_viterbi_decode(hard_dibits, decoded) == 0);
    assert(memcmp(decoded, expected_hard, sizeof(decoded)) == 0);

    DSD_MEMCPY(soft_dibits, reference->dibits, sizeof(soft_dibits));
    soft_dibits[48] = 0U; /* Likewise favors state 4 under the weighted branch metric. */
    DSD_MEMSET(reliability, 200, sizeof(reliability));
    assert(dmr_r34_viterbi_decode_soft(soft_dibits, reliability, decoded) == 0);
    assert(memcmp(decoded, expected_soft, sizeof(decoded)) == 0);

    assert(dmr_r34_viterbi_decode_list(soft_dibits, reliability, candidates, 16, &count) == 0);
    assert(count == 16);
    assert(memcmp(candidates[0].bytes18, expected_list, sizeof(expected_list)) == 0);
    for (int i = 0; i < count; i++) {
        int forced_metric = -1;
        assert(dmr_r34_candidate_metric(soft_dibits, reliability, candidates[i].bytes18, &forced_metric) == 0);
        assert(candidates[i].metric == forced_metric);
    }
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
    test_all_decoders_require_zero_flushing_state();
    test_invalid_arguments_are_rejected();
    printf("DMR_R34_LIST: OK\n");
    return 0;
}
