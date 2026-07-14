// SPDX-License-Identifier: ISC
#include <dsd-neo/fec/block_codes.h>

namespace {

struct HammingTables {
    int fixed_values[1024];
    int error_counts[1024];

    HammingTables();
};

static int
bit_parity(unsigned int value) {
    value ^= value >> 16U;
    value ^= value >> 8U;
    value ^= value >> 4U;
    value &= 0xFU;
    return (int)((0x6996U >> value) & 1U);
}

static int
compute_decode_result(int input, int* output) {
    static const int bad_bit_table[16] = {-2, 0, 1, 5, 2, -1, -1, 6, 3, -1, -1, 7, 4, 8, 9, -1};
    const unsigned int value = (unsigned int)input;
    const int s0 = bit_parity(value & 0x398U) << 3; // 1110011000
    const int s1 = bit_parity(value & 0x354U) << 2; // 1101010100
    const int s2 = bit_parity(value & 0x2E2U) << 1; // 1011100010
    const int s3 = bit_parity(value & 0x1E1U);      // 0111100001
    const int syndrome = s0 | s1 | s2 | s3;

    int corrected = input;
    int error_count = 0;
    if (syndrome != 0) {
        const int bad_bit_index = bad_bit_table[syndrome];
        if (bad_bit_index < 0) {
            error_count = 2;
        } else {
            error_count = 1;
            if (bad_bit_index >= 4) {
                corrected ^= 1 << bad_bit_index;
            }
        }
    }

    *output = corrected >> 4;
    return error_count;
}

HammingTables::HammingTables() {
    for (int input = 0; input < 1024; input++) {
        error_counts[input] = compute_decode_result(input, &fixed_values[input]);
    }
}

static const HammingTables&
hamming_tables() {
    static const HammingTables tables;
    return tables;
}

static int
bits_to_int(const char* bits, int count) {
    if (!bits) {
        return -1;
    }

    int value = 0;
    for (int i = 0; i < count; i++) {
        if (bits[i] != 0 && bits[i] != 1) {
            return -1;
        }
        value = (value << 1) | bits[i];
    }
    return value;
}

static void
int_to_six_bits(int value, char* bits) {
    unsigned int remaining = (unsigned int)value;
    for (int i = 5; i >= 0; i--) {
        bits[i] = (char)(remaining & 1U);
        remaining >>= 1U;
    }
}

} // namespace

extern "C" int
hamming_10_6_3_decode(char* data, const char* parity) {
    const int data_value = bits_to_int(data, 6);
    const int check = bits_to_int(parity, 4);
    if (data_value < 0 || check < 0) {
        return 2;
    }

    const int input = (data_value << 4) | check;
    const HammingTables& tables = hamming_tables();
    const int fixed = tables.fixed_values[input];
    const int error_count = tables.error_counts[input];
    if (error_count == 1) {
        int_to_six_bits(fixed, data);
    }
    return error_count;
}
