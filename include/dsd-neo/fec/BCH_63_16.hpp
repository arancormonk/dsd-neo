// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef BCH_63_16_HPP_a7b9c2d4e1f83056
#define BCH_63_16_HPP_a7b9c2d4e1f83056

/**
 * @file
 * @brief BCH(63,16,11) encoder/decoder for P25 NID.
 *
 * Implements a binary BCH code that can correct up to 11 bit errors.
 * Used for P25 Phase 1 Network ID (NID) error correction.
 *
 * Based on the algorithms from Simon Rockliff's Reed-Solomon implementation
 * and adapted for binary BCH codes. The BCH code operates over GF(2) but uses
 * GF(2^6) for syndrome calculation and error location.
 *
 * References:
 * - Lin & Costello, "Error Control Coding"
 * - P25 TIA-102.BAAA specification
 */

/**
 * @brief Decode result structure for BCH(63,16,11).
 *
 * Returned by BCH_63_16_11::decode_with_result() to provide both the success/failure
 * indication and the number of bit errors that were corrected. This enables
 * callers (e.g., check_NID) to make confidence-based decisions using the
 * error count as a quality metric.
 */
struct BCH_63_16_Result {
    bool success;    /**< true if decoding succeeded (0-11 errors corrected) */
    int error_count; /**< Number of bit errors corrected (valid only when success=true) */
};

/**
 * BCH(63,16,11) decoder class.
 *
 * Parameters:
 * - n = 63 = 2^6 - 1 (codeword length in bits)
 * - k = 16 (data bits: 12-bit NAC + 4-bit DUID)
 * - t = 11 (error correction capability)
 * - Uses GF(2^6) with primitive polynomial x^6 + x + 1
 */
class BCH_63_16_11 {
  private:
    static const int MM = 6;  // GF(2^6)
    static const int NN = 63; // n = 2^6 - 1
    static const int KK = 16; // k = data bits
    static const int TT = 11; // t = error correction capability

    int alpha_to[NN + 1]; // antilog table: alpha_to[i] = alpha^i
    int index_of[NN + 1]; // log table: index_of[x] = i where alpha^i = x

    void
    generate_gf() {
        // Primitive polynomial: x^6 + x + 1 -> coefficients [1,1,0,0,0,0,1]
        // Same as used in ReedSolomon_63
        const int pp[MM + 1] = {1, 1, 0, 0, 0, 0, 1};

        int mask = 1;
        alpha_to[MM] = 0;
        for (int i = 0; i < MM; i++) {
            alpha_to[i] = mask;
            index_of[alpha_to[i]] = i;
            if (pp[i] != 0) {
                alpha_to[MM] ^= mask;
            }
            mask <<= 1;
        }
        index_of[alpha_to[MM]] = MM;
        mask >>= 1;
        for (int i = MM + 1; i < NN; i++) {
            if (alpha_to[i - 1] >= mask) {
                alpha_to[i] = alpha_to[MM] ^ ((alpha_to[i - 1] ^ mask) << 1);
            } else {
                alpha_to[i] = alpha_to[i - 1] << 1;
            }
            index_of[alpha_to[i]] = i;
        }
        index_of[0] = -1; // log(0) is undefined, use -1 as sentinel
    }

    static void
    reverse_input_bits(const char* input, int recd[NN]) {
        // IT++ maps r[j] = rbin(n - j - 1): input[0] -> recd[62], input[62] -> recd[0]
        for (int i = 0; i < NN; i++) {
            recd[i] = input[NN - 1 - i] ? 1 : 0;
        }
    }

    bool
    compute_syndromes(const int recd[NN], int s[2 * TT + 1]) const {
        bool has_error = false;

        // For BCH, syndrome S_i = sum of r_j * alpha^(i*j) for j=0..n-1
        for (int i = 1; i <= 2 * TT; i++) {
            int syndrome = 0;
            for (int j = 0; j < NN; j++) {
                if (recd[j]) {
                    syndrome ^= alpha_to[(i * j) % NN];
                }
            }
            has_error = has_error || (syndrome != 0);
            s[i] = index_of[syndrome]; // convert to index form
        }
        return has_error;
    }

    static void
    extract_data_bits(const int recd[NN], char* output) {
        // Combined IT++ extraction/reversal effect is output[i] = recd[NN - 1 - i]
        for (int i = 0; i < KK; i++) {
            output[i] = (char)recd[NN - 1 - i];
        }
    }

    static void
    initialize_berlekamp_state(int elp[2 * TT + 2][2 * TT], int d[2 * TT + 2], int l[2 * TT + 2], int u_lu[2 * TT + 2],
                               const int s[2 * TT + 1]) {
        d[0] = 0;    // index form
        d[1] = s[1]; // index form
        elp[0][0] = 0;
        elp[1][0] = 1;
        for (int i = 1; i < 2 * TT; i++) {
            elp[0][i] = -1;
            elp[1][i] = 0;
        }
        l[0] = 0;
        l[1] = 0;
        u_lu[0] = -1;
        u_lu[1] = 0;
    }

    void
    index_elp_row(int row, int degree, int elp[2 * TT + 2][2 * TT]) const {
        for (int i = 0; i <= degree; i++) {
            if (elp[row][i] >= 0) {
                elp[row][i] = index_of[elp[row][i]];
            }
        }
    }

    void
    propagate_elp_when_discrepancy_zero(int u, int elp[2 * TT + 2][2 * TT], int l[2 * TT + 2]) const {
        l[u + 1] = l[u];
        for (int i = 0; i <= l[u]; i++) {
            elp[u + 1][i] = elp[u][i];
        }
        index_elp_row(u, l[u], elp);
    }

    static int
    find_best_discrepancy_index(int u, const int d[2 * TT + 2], const int u_lu[2 * TT + 2]) {
        int q = u - 1;
        while ((q > 0) && (d[q] == -1)) {
            q--;
        }
        if (q <= 0) {
            return q;
        }

        for (int j = q - 1; j > 0; j--) {
            if ((d[j] != -1) && (u_lu[q] < u_lu[j])) {
                q = j;
            }
        }
        return q;
    }

    static int
    next_elp_degree(int u, int q, const int l[2 * TT + 2]) {
        const int candidate = l[q] + u - q;
        return (l[u] > candidate) ? l[u] : candidate;
    }

    void
    form_next_elp(int u, int q, int elp[2 * TT + 2][2 * TT], const int d[2 * TT + 2], const int l[2 * TT + 2]) const {
        for (int i = 0; i < 2 * TT; i++) {
            elp[u + 1][i] = 0;
        }
        for (int i = 0; i <= l[q]; i++) {
            if (elp[q][i] != -1) {
                const int power = (d[u] + NN - d[q] + elp[q][i]) % NN;
                elp[u + 1][i + u - q] = alpha_to[power];
            }
        }
        for (int i = 0; i <= l[u]; i++) {
            elp[u + 1][i] ^= elp[u][i];
        }
        index_elp_row(u, l[u], elp);
    }

    void
    update_discrepancy(int u, const int s[2 * TT + 1], int elp[2 * TT + 2][2 * TT], const int l[2 * TT + 2],
                       int d[2 * TT + 2]) const {
        if (u >= 2 * TT) {
            return;
        }

        int discrepancy = (s[u + 1] != -1) ? alpha_to[s[u + 1]] : 0;
        for (int i = 1; i <= l[u + 1]; i++) {
            if ((s[u + 1 - i] != -1) && (elp[u + 1][i] != 0)) {
                const int power = (s[u + 1 - i] + index_of[elp[u + 1][i]]) % NN;
                discrepancy ^= alpha_to[power];
            }
        }
        d[u + 1] = index_of[discrepancy];
    }

    bool
    run_berlekamp_massey(const int s[2 * TT + 1], int elp[2 * TT + 2][2 * TT], int l[2 * TT + 2], int& u) const {
        int d[2 * TT + 2];
        int u_lu[2 * TT + 2];
        initialize_berlekamp_state(elp, d, l, u_lu, s);

        u = 0;
        do {
            u++;
            if (d[u] == -1) {
                propagate_elp_when_discrepancy_zero(u, elp, l);
            } else {
                const int q = find_best_discrepancy_index(u, d, u_lu);
                l[u + 1] = next_elp_degree(u, q, l);
                form_next_elp(u, q, elp, d, l);
            }
            u_lu[u + 1] = u - l[u + 1];
            update_discrepancy(u, s, elp, l, d);
        } while ((u < 2 * TT) && (l[u + 1] <= TT));

        u++;
        return l[u] <= TT;
    }

    int
    find_error_locations(const int elp_row[2 * TT], int degree, int loc[TT]) const {
        int reg[TT + 1] = {0};
        int count = 0;

        for (int i = 1; i <= degree; i++) {
            reg[i] = elp_row[i];
        }

        for (int i = 1; i <= NN; i++) {
            int q = 1;
            for (int j = 1; j <= degree; j++) {
                if (reg[j] != -1) {
                    reg[j] = (reg[j] + j) % NN;
                    q ^= alpha_to[reg[j]];
                }
            }
            if (q == 0) {
                if (count >= TT) {
                    break; // Safety check
                }
                loc[count] = NN - i;
                count++;
            }
        }

        return count;
    }

    static void
    correct_error_locations(int recd[NN], const int loc[TT], int count) {
        for (int i = 0; i < count; i++) {
            if (loc[i] >= 0 && loc[i] < NN) {
                recd[loc[i]] ^= 1;
            }
        }
    }

    /**
     * Generator polynomial g(x) = 6331 1413 6723 5453 (octal), degree 47.
     *
     * This is the BCH(63,16,23) generator polynomial from TIA-102.BAAA-A section 8.5.2.
     * Stored as 48 binary coefficients (MSB = x^47 coefficient, LSB = x^0 = 1).
     * The polynomial has degree 47, so genpoly[0] is the x^47 coefficient (always 1)
     * and genpoly[47] is the x^0 coefficient (always 1).
     *
     * Octal expansion:
     *   6    3    3    1    1    4    1    3    6    7    2    3    5    4    5    3
     *  110  011  011  001  001  100  001  011  110  111  010  011  101  100  101  011
     */

  public:
    BCH_63_16_11() { generate_gf(); }

    /**
     * @brief Encode 16 information bits into a 63-bit systematic BCH codeword.
     *
     * Uses the generator polynomial g(x) = 6331 1413 6723 5453 (octal) from
     * TIA-102.BAAA-A section 8.5.2. The encoding is systematic: information bits
     * occupy positions 0-15 of the output, parity bits occupy positions 16-62.
     *
     * Implementation uses a feedback shift register (LFSR) division approach:
     * multiply the information polynomial by x^47, divide by g(x), and append
     * the remainder as parity bits.
     *
     * @param input  Array of 16 chars, each 0 or 1 (MSB first: input[0] = bit 15).
     * @param output Array of 63 chars to receive the complete codeword.
     *               output[0..15] = information bits (copied from input).
     *               output[16..62] = parity bits computed from g(x).
     */
    static void
    encode(const char* input, char* output) {
        // Generator polynomial g(x) = 6331 1413 6723 5453 (octal), degree 47.
        // 48 binary coefficients (MSB = x^47, LSB = x^0). Defined as a function-local
        // static to avoid ODR-use linker issues with static constexpr class members in C++14.
        static const char genpoly[48] = {1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1,
                                         1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1};

        // Number of parity bits = n - k = 63 - 16 = 47
        static const int PARITY_BITS = NN - KK; // 47

        // Step 1: Copy 16 information bits to output positions 0-15
        for (int i = 0; i < KK; i++) {
            output[i] = input[i];
        }

        // Step 2: Initialize a 47-bit shift register to zero.
        // The shift register holds the running remainder of the polynomial
        // division of the information polynomial (shifted by x^47) by g(x).
        char sr[47] = {0};

        // Step 3: Feed each information bit through the LFSR.
        // For systematic encoding, we compute:
        //   remainder = (info_polynomial * x^47) mod g(x)
        // using a feedback shift register. The feedback bit is the XOR of the
        // incoming data bit and the MSB of the shift register (the bit about
        // to be shifted out).
        for (int i = 0; i < KK; i++) {
            // Feedback = input bit XOR the MSB of the shift register
            char feedback = input[i] ^ sr[0];

            // Shift the register left by one position, applying feedback
            // at each tap position defined by g(x). The generator polynomial
            // coefficients genpoly[1..47] define where feedback is XORed in.
            for (int j = 0; j < PARITY_BITS - 1; j++) {
                sr[j] = sr[j + 1] ^ (feedback & genpoly[j + 1]);
            }
            // The last position receives only the feedback (genpoly[47] = 1)
            sr[PARITY_BITS - 1] = feedback & genpoly[PARITY_BITS];
        }

        // Step 4: Copy the 47 parity bits from the shift register to output[16..62]
        for (int i = 0; i < PARITY_BITS; i++) {
            output[KK + i] = sr[i];
        }
    }

    /**
     * @brief Decode a BCH(63,16,11) codeword with error count reporting.
     *
     * Computes syndromes, runs Berlekamp-Massey to find the error locator
     * polynomial, then performs Chien search to locate and correct errors.
     * Returns both a success flag and the number of corrected bit errors,
     * enabling callers to assess correction confidence.
     *
     * @param input  Array of 63 chars, each containing a bit (0 or 1).
     *               Bit ordering matches IT++ systematic convention:
     *               data bits in positions 0-15 (MSB first), parity in 16-62.
     * @param output Array of 16 chars to receive corrected data bits.
     * @return BCH_63_16_Result with success flag and error count.
     *         - success=true, error_count=0: no errors detected (all syndromes zero)
     *         - success=true, error_count=N: N errors corrected (1 <= N <= 11)
     *         - success=false, error_count=0: decoding failed (>11 errors or Chien search mismatch)
     */
    BCH_63_16_Result
    decode_with_result(const char* input, char* output) const {
        int recd[NN];      // received codeword (working copy)
        int s[2 * TT + 1]; // syndromes

        reverse_input_bits(input, recd);
        if (!compute_syndromes(recd, s)) {
            // No errors - extract data bits matching IT++ convention
            // Same extraction as the error-corrected case
            extract_data_bits(recd, output);
            return BCH_63_16_Result{true, 0};
        }

        int elp[2 * TT + 2][2 * TT]; // error locator polynomial
        int l[2 * TT + 2];           // degree of each elp row
        int u = 0;
        if (!run_berlekamp_massey(s, elp, l, u)) {
            // Too many errors to correct
            return BCH_63_16_Result{false, 0};
        }

        index_elp_row(u, l[u], elp);

        int loc[TT]; // error locations
        const int count = find_error_locations(elp[u], l[u], loc);

        if (count != l[u]) {
            // Number of roots doesn't match degree - decoding failure
            return BCH_63_16_Result{false, 0};
        }

        correct_error_locations(recd, loc, count);
        extract_data_bits(recd, output);
        return BCH_63_16_Result{true, count};
    }

    /**
     * @brief Decode a BCH(63,16,11) codeword.
     *
     * Thin wrapper around decode_with_result() that discards the error count
     * and preserves the original public bool-returning API.
     *
     * @param input  Array of 63 chars, each containing a bit (0 or 1).
     * @param output Array of 16 chars to receive corrected data bits.
     * @return true if decoding succeeded, false otherwise.
     */
    bool
    decode(const char* input, char* output) const {
        return decode_with_result(input, output).success;
    }
};

#endif // BCH_63_16_HPP_a7b9c2d4e1f83056
