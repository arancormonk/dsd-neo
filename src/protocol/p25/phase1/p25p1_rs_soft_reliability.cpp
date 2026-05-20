// SPDX-License-Identifier: ISC

#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>

#include <stdint.h>
#include <string.h>

int
p25p1_rs_24_12_13_soft_reliability(char* data, char* parity, const uint8_t* data_reliab, const uint8_t* parity_reliab) {
    if (data == NULL || parity == NULL || data_reliab == NULL || parity_reliab == NULL) {
        return 1;
    }

    int erasures[12];
    int n_ranked = p25p1_build_rs_ranked_erasures(data_reliab, 12, parity_reliab, 12, 6, erasures, 12);
    char original_data[12 * 6];
    memcpy(original_data, data, sizeof(original_data));

    for (int n = 1; n <= n_ranked; n++) {
        char candidate_data[12 * 6];
        memcpy(candidate_data, original_data, sizeof(candidate_data));
        if (check_and_fix_reedsolomon_24_12_13_soft(candidate_data, parity, erasures, n) == 0) {
            memcpy(data, candidate_data, sizeof(candidate_data));
            return 0;
        }
    }
    return 1;
}
