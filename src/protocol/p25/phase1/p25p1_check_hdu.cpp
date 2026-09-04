// SPDX-License-Identifier: ISC
#include <dsd-neo/fec/Golay24.hpp>
#include <dsd-neo/fec/ReedSolomon.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static DSDGolay24 golay24;

namespace {

static DSDReedSolomon_36_20_17*
reed_solomon_36_20_17_instance(void) {
    static DSDReedSolomon_36_20_17* instance = []() noexcept -> DSDReedSolomon_36_20_17* {
        try {
            return new DSDReedSolomon_36_20_17();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

} // namespace

int
check_and_fix_golay_24_6(char* hex, const char* parity, int* fixed_errors) {
    return golay24.decode_6(hex, parity, fixed_errors);
}

int
check_and_fix_golay_24_12(char* dodeca, const char* parity, int* fixed_errors) {
    return golay24.decode_12(dodeca, parity, fixed_errors);
}

int
check_and_fix_redsolomon_36_20_17(char* data, const char* parity) {
    const DSDReedSolomon_36_20_17* rs = reed_solomon_36_20_17_instance();
    if (rs == nullptr) {
        return 1;
    }
    return rs->decode(data, parity);
}

int
check_and_fix_redsolomon_36_20_17_soft(char* data, const char* parity, const int* erasures, int n_erasures) {
    const DSDReedSolomon_36_20_17* rs = reed_solomon_36_20_17_instance();
    if (rs == nullptr) {
        return 1;
    }
    return rs->decode_soft(data, parity, erasures, n_erasures);
}

int
p25p1_rs_36_20_17_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                   const uint8_t* parity_reliab) {
    if (data == NULL || parity == NULL || data_reliab == NULL || parity_reliab == NULL) {
        return 1;
    }

    int erasures[16];
    int n_ranked = p25p1_build_rs_ranked_erasures(data_reliab, 20, parity_reliab, 16, 8, erasures, 16);
    char original_data[20 * 6];
    DSD_MEMCPY(original_data, data, sizeof(original_data));

    for (int n = 1; n <= n_ranked; n++) {
        char candidate_data[20 * 6];
        DSD_MEMCPY(candidate_data, original_data, sizeof(candidate_data));
        if (check_and_fix_redsolomon_36_20_17_soft(candidate_data, parity, erasures, n) == 0) {
            DSD_MEMCPY(data, candidate_data, sizeof(candidate_data));
            return 0;
        }
    }
    return 1;
}
