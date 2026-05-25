// SPDX-License-Identifier: ISC
#include <dsd-neo/fec/Hamming.hpp>
#include <dsd-neo/fec/ReedSolomon.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_ldu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static Hamming_10_6_3_TableImpl hamming;

namespace {

static DSDReedSolomon_24_12_13*
reed_solomon_24_12_13_instance(void) {
    static DSDReedSolomon_24_12_13* instance = []() noexcept -> DSDReedSolomon_24_12_13* {
        try {
            return new DSDReedSolomon_24_12_13();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

static DSDReedSolomon_24_16_9*
reed_solomon_24_16_9_instance(void) {
    static DSDReedSolomon_24_16_9* instance = []() noexcept -> DSDReedSolomon_24_16_9* {
        try {
            return new DSDReedSolomon_24_16_9();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

} // namespace

int
check_and_fix_hamming_10_6_3(char* hex, char* parity) {
    return hamming.decode(hex, parity);
}

void
encode_hamming_10_6_3(char* hex, char* out_parity) {
    hamming.encode(hex, out_parity);
}

int
check_and_fix_reedsolomon_24_12_13(char* data, const char* parity) {
    const DSDReedSolomon_24_12_13* rs = reed_solomon_24_12_13_instance();
    if (rs == nullptr) {
        return 1;
    }
#ifdef CHECK_LDU_DEBUG
    char original[12][6];
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 6; j++) {
            original[i][j] = data[i * 6 + j];
        }
    }
#endif

    int irrecoverable_error = rs->decode(data, parity);

#ifdef CHECK_LDU_DEBUG
    DSD_FPRINTF(stderr, "Results for Reed-Solomon code (24,12,13)\n\n");
    if (irrecoverable_error == 0) {
        DSD_FPRINTF(stderr, "  i  original fixed\n");
        for (int i = 0; i < 12; i++) {
            DSD_FPRINTF(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "] [");
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (data[i * 6 + j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "]\n");
        }
    } else {
        DSD_FPRINTF(stderr, "Irrecoverable errors found\n");
        DSD_FPRINTF(stderr, "  i  original fixed\n");
        for (int i = 0; i < 12; i++) {
            DSD_FPRINTF(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "]\n");
        }
    }
    DSD_FPRINTF(stderr, "\n");
#endif

    return irrecoverable_error;
}

int
check_and_fix_reedsolomon_24_12_13_soft(char* data, const char* parity, const int* erasures, int n_erasures) {
    const DSDReedSolomon_24_12_13* rs = reed_solomon_24_12_13_instance();
    if (rs == nullptr) {
        return 1;
    }
    return rs->decode_soft(data, parity, erasures, n_erasures);
}

void
encode_reedsolomon_24_12_13(const char* hex_data, char* fixed_parity) {
    const DSDReedSolomon_24_12_13* rs = reed_solomon_24_12_13_instance();
    if (rs == nullptr) {
        return;
    }
    rs->encode(hex_data, fixed_parity);
}

int
check_and_fix_reedsolomon_24_16_9(char* data, const char* parity) {
    const DSDReedSolomon_24_16_9* rs = reed_solomon_24_16_9_instance();
    if (rs == nullptr) {
        return 1;
    }
#ifdef CHECK_LDU_DEBUG
    char original[16][6];
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 6; j++) {
            original[i][j] = data[i * 6 + j];
        }
    }
#endif

    int irrecoverable_error = rs->decode(data, parity);

#ifdef CHECK_LDU_DEBUG
    DSD_FPRINTF(stderr, "Results for Reed-Solomon code (24,16,9)\n\n");
    if (irrecoverable_error == 0) {
        DSD_FPRINTF(stderr, "  i  original fixed\n");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "] [");
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (data[i * 6 + j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "]\n");
        }
    } else {
        DSD_FPRINTF(stderr, "Irrecoverable errors found\n");
        DSD_FPRINTF(stderr, "  i  original fixed\n");
        for (int i = 0; i < 16; i++) {
            DSD_FPRINTF(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                DSD_FPRINTF(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            DSD_FPRINTF(stderr, "]\n");
        }
    }
    DSD_FPRINTF(stderr, "\n");
#endif

    return irrecoverable_error;
}

int
check_and_fix_reedsolomon_24_16_9_soft(char* data, const char* parity, const int* erasures, int n_erasures) {
    const DSDReedSolomon_24_16_9* rs = reed_solomon_24_16_9_instance();
    if (rs == nullptr) {
        return 1;
    }
    return rs->decode_soft(data, parity, erasures, n_erasures);
}

int
p25p1_rs_24_16_9_soft_reliability(char* data, const char* parity, const uint8_t* data_reliab,
                                  const uint8_t* parity_reliab) {
    if (data == NULL || parity == NULL || data_reliab == NULL || parity_reliab == NULL) {
        return 1;
    }

    int erasures[8];
    int n_ranked = p25p1_build_rs_ranked_erasures(data_reliab, 16, parity_reliab, 8, 4, erasures, 8);
    char original_data[16 * 6];
    DSD_MEMCPY(original_data, data, sizeof(original_data));

    for (int n = 1; n <= n_ranked; n++) {
        char candidate_data[16 * 6];
        DSD_MEMCPY(candidate_data, original_data, sizeof(candidate_data));
        if (check_and_fix_reedsolomon_24_16_9_soft(candidate_data, parity, erasures, n) == 0) {
            DSD_MEMCPY(data, candidate_data, sizeof(candidate_data));
            return 0;
        }
    }
    return 1;
}

void
encode_reedsolomon_24_16_9(const char* hex_data, char* fixed_parity) {
    const DSDReedSolomon_24_16_9* rs = reed_solomon_24_16_9_instance();
    if (rs == nullptr) {
        return;
    }
    rs->encode(hex_data, fixed_parity);
}
