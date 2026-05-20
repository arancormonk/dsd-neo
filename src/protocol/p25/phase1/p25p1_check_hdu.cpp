// SPDX-License-Identifier: ISC
#include <dsd-neo/fec/Golay24.hpp>
#include <dsd-neo/fec/ReedSolomon.hpp>
#include <dsd-neo/protocol/p25/p25p1_check_hdu.h>
#include <dsd-neo/protocol/p25/p25p1_soft.h>

#include <stdint.h>
#include <string.h>

// Uncomment for very verbose trace messages
//#define CHECK_HDU_DEBUG

// The following methods are just a C bridge for the C++ implementations of the Golay and ReedSolomon
// algorithms.

static DSDGolay24 golay24;

static DSDReedSolomon_36_20_17 reed_solomon_36_20_17;

int
check_and_fix_golay_24_6(char* hex, char* parity, int* fixed_errors) {
#ifdef CHECK_HDU_DEBUG
    fprintf(stderr, "[");
    for (unsigned int i = 0; i < 6; i++) {
        fprintf(stderr, "%c", (hex[i] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]  [");
    for (unsigned int i = 12; i < 24; i++) {
        fprintf(stderr, "%c", (parity[i - 12] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]");
#endif

    int irrecoverable_errors = golay24.decode_6(hex, parity, fixed_errors);

#ifdef CHECK_HDU_DEBUG
    fprintf(stderr, " -> [");
    for (unsigned int i = 0; i < 6; i++) {
        fprintf(stderr, "%c", (hex[i] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]");
    if (irrecoverable_errors) {
        fprintf(stderr, "  Errors: >4");
    } else {
        fprintf(stderr, "  Errors: %i", *fixed_errors);
    }
    fprintf(stderr, "\n");
#endif

    return irrecoverable_errors;
}

int
check_and_fix_golay_24_12(char* dodeca, char* parity, int* fixed_errors) {
#ifdef CHECK_HDU_DEBUG
    fprintf(stderr, "[");
    for (unsigned int i = 0; i < 12; i++) {
        fprintf(stderr, "%c", (dodeca[i] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]  [");
    for (unsigned int i = 12; i < 24; i++) {
        fprintf(stderr, "%c", (parity[i - 12] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]");
#endif

    int irrecoverable_errors = golay24.decode_12(dodeca, parity, fixed_errors);

#ifdef CHECK_HDU_DEBUG
    fprintf(stderr, " -> [");
    for (unsigned int i = 0; i < 12; i++) {
        fprintf(stderr, "%c", (dodeca[i] != 0) ? 'X' : ' ');
    }
    fprintf(stderr, "]");
    if (irrecoverable_errors) {
        fprintf(stderr, "  Errors: >4");
    } else {
        fprintf(stderr, "  Errors: %i", *fixed_errors);
    }
    fprintf(stderr, "\n");
#endif

    return irrecoverable_errors;
}

void
encode_golay_24_6(char* hex, char* out_parity) {
    golay24.encode_6(hex, out_parity);
}

void
encode_golay_24_12(char* dodeca, char* out_parity) {
    golay24.encode_12(dodeca, out_parity);
}

int
check_and_fix_redsolomon_36_20_17(char* data, char* parity) {
#ifdef CHECK_HDU_DEBUG
    char original[20][6];
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < 6; j++) {
            original[i][j] = data[i * 6 + j];
        }
    }
#endif

    int irrecoverable_errors = reed_solomon_36_20_17.decode(data, parity);

#ifdef CHECK_HDU_DEBUG
    fprintf(stderr, "Results for Reed-Solomon code (36,20,17)\n\n");
    if (irrecoverable_errors == 0) {
        fprintf(stderr, "  i  original fixed\n");
        for (int i = 0; i < 20; i++) {
            fprintf(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                fprintf(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            fprintf(stderr, "] [");
            for (int j = 0; j < 6; j++) {
                fprintf(stderr, "%c", (data[i * 6 + j] == 1) ? 'X' : ' ');
            }
            fprintf(stderr, "]\n");
        }
    } else {
        fprintf(stderr, "Irrecoverable errors found\n");
        fprintf(stderr, "  i  original fixed\n");
        for (int i = 0; i < 20; i++) {
            fprintf(stderr, "%3d  [", i);
            for (int j = 0; j < 6; j++) {
                fprintf(stderr, "%c", (original[i][j] == 1) ? 'X' : ' ');
            }
            fprintf(stderr, "]\n");
        }
    }
    fprintf(stderr, "\n");
#endif

    return irrecoverable_errors;
}

int
check_and_fix_redsolomon_36_20_17_soft(char* data, char* parity, const int* erasures, int n_erasures) {
    return reed_solomon_36_20_17.decode_soft(data, parity, erasures, n_erasures);
}

int
p25p1_rs_36_20_17_soft_reliability(char* data, char* parity, const uint8_t* data_reliab, const uint8_t* parity_reliab) {
    if (data == NULL || parity == NULL || data_reliab == NULL || parity_reliab == NULL) {
        return 1;
    }

    int erasures[16];
    int n_ranked = p25p1_build_rs_ranked_erasures(data_reliab, 20, parity_reliab, 16, 8, erasures, 16);
    char original_data[20 * 6];
    memcpy(original_data, data, sizeof(original_data));

    for (int n = 1; n <= n_ranked; n++) {
        char candidate_data[20 * 6];
        memcpy(candidate_data, original_data, sizeof(candidate_data));
        if (check_and_fix_redsolomon_36_20_17_soft(candidate_data, parity, erasures, n) == 0) {
            memcpy(data, candidate_data, sizeof(candidate_data));
            return 0;
        }
    }
    return 1;
}

void
encode_reedsolomon_36_20_17(char* hex_data, char* fixed_parity) {
    reed_solomon_36_20_17.encode(hex_data, fixed_parity);
}
