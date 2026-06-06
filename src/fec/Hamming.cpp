// SPDX-License-Identifier: ISC
#include <dsd-neo/fec/Hamming.hpp>

Hamming_10_6_3_data*
Hamming_10_6_3::data() {
    static Hamming_10_6_3_data* instance = []() noexcept -> Hamming_10_6_3_data* {
        try {
            return new Hamming_10_6_3_data();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

Hamming_10_6_3_TableImpl_data*
Hamming_10_6_3_TableImpl::data() {
    static Hamming_10_6_3_TableImpl_data* instance = []() noexcept -> Hamming_10_6_3_TableImpl_data* {
        try {
            return new Hamming_10_6_3_TableImpl_data();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

int
Hamming_10_6_3::decode(std::bitset<10>& input) {
    Hamming_10_6_3_data* state = data();
    if (state == nullptr) {
        return 2;
    }

    int error_count;

    // Compute syndromes
    int s0 = ((state->h0 & input).count() & 1) << 3;
    int s1 = ((state->h1 & input).count() & 1) << 2;
    int s2 = ((state->h2 & input).count() & 1) << 1;
    int s3 = ((state->h3 & input).count() & 1);
    int parity = s0 | s1 | s2 | s3;

    if (parity == 0) {
        error_count = 0;
    } else {
        // Error detected, attempt to fix it
        int bad_bit_index = state->bad_bit_table[parity];

        if (bad_bit_index < 0) {
            error_count = 2;
        } else {
            // Error in a data bit, or more than one error
            // If there is one erroneous bit, the bad one is the one indicated by parity
            if (bad_bit_index < 4) {
                // Error detected in a parity bit
                //std::cout << "Error on parity bit " << bad_bit_index << std::endl;
                error_count = 1;
            } else {
                input.flip(bad_bit_index);
                error_count = 1;
            }
        }
    }

    return error_count;
}

int
Hamming_10_6_3::encode(const std::bitset<6>& input) {
    Hamming_10_6_3_data* state = data();
    if (state == nullptr) {
        return 0;
    }

    // Compute syndromes
    int s0 = ((state->gt0 & input).count() & 1) << 3;
    int s1 = ((state->gt1 & input).count() & 1) << 2;
    int s2 = ((state->gt2 & input).count() & 1) << 1;
    int s3 = ((state->gt3 & input).count() & 1);
    int parity = s0 | s1 | s2 | s3;

    return parity;
}

int
Hamming_10_6_3_TableImpl::decode(int input, int* output) {
    if (!output || input < 0 || input >= 1024) {
        if (output) {
            *output = 0;
        }
        return 2;
    }

    // Making use of a table...
    const Hamming_10_6_3_TableImpl_data* table = data();
    if (table == nullptr) {
        *output = 0;
        return 2;
    }

    *output = table->fixed_values[input];

    return table->error_counts[input];
}

int
Hamming_10_6_3_TableImpl::encode(int input) {
    if (input < 0 || input >= 64) {
        return 0;
    }

    // Making use of a table...
    const Hamming_10_6_3_TableImpl_data* table = data();
    if (table == nullptr) {
        return 0;
    }

    return table->encode_parities[input];
}
