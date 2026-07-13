// SPDX-License-Identifier: ISC
#ifndef REEDSOLOMON_HPP_b1405fdab6374ba2a4e65e8d45ec3d80
#define REEDSOLOMON_HPP_b1405fdab6374ba2a4e65e8d45ec3d80

/**
 * @file
 * @brief Reed-Solomon encoder/decoder used across multiple protocols.
 *
 * Code taken and adapted from www.eccpage.com/rs.c (credit: Simon Rockliff).
 * Chosen because it matches P25 expectations where other implementations did not.
 */

/* This program is an encoder/decoder for Reed-Solomon codes. Encoding is in
 systematic form, decoding via the Berlekamp iterative algorithm.
 In the present form , the constants mm, nn, tt, and kk=nn-2tt must be
 specified  (the double letters are used simply to avoid clashes with
 other n,k,t used in other programs into which this was incorporated!)
 Also, the irreducible polynomial used to generate GF(2**mm) must also be
 entered -- these can be found in Lin and Costello, and also Clark and Cain.

 The representation of the elements of GF(2**m) is either in index form,
 where the number is the power of the primitive element alpha, which is
 convenient for multiplication (add the powers modulo 2**m-1) or in
 polynomial form, where the bits represent the coefficients of the
 polynomial representation of the number, which is the most convenient form
 for addition.  The two forms are swapped between via lookup tables.
 This leads to fairly messy looking expressions, but unfortunately, there
 is no easy alternative when working with Galois arithmetic.

 The code is not written in the most elegant way, but to the best
 of my knowledge, (no absolute guarantees!), it works.
 However, when including it into a simulation program, you may want to do
 some conversion of global variables (used here because I am lazy!) to
 local variables where appropriate, and passing parameters (eg array
 addresses) to the functions  may be a sensible move to reduce the number
 of global variables and thus decrease the chance of a bug being introduced.

 This program does not handle erasures at present, but should not be hard
 to adapt to do this, as it is just an adjustment to the Berlekamp-Massey
 algorithm. It also does not attempt to decode past the BCH bound -- see
 Blahut "Theory and practice of error control codes" for how to do this.

 Simon Rockliff, University of Adelaide   21/9/89

 26/6/91 Slight modifications to remove a compiler dependent bug which hadn't
 previously surfaced. A few extra comments added for clarity.
 Appears to all work fine, ready for posting to net!

 Notice
 --------
 This program may be freely modified and/or given to whoever wants it.
 A condition of such distribution is that the author's contribution be
 acknowledged by his name being left in the comments heading the program,
 however no responsibility is accepted for any financial or other loss which
 may result from some unforseen errors or malfunctioning of the program
 during use.
 Simon Rockliff, 26th June 1991
 */

#include <stddef.h>

template <int TT>
class ReedSolomon_63 {
  private:
    static const int MM = 6;  /* RS code over GF(2**mm) */
    static const int NN = 63; /* nn=2**mm -1   length of codeword */
    //int tt;             /* number of errors that can be corrected */
    //int kk;             /* kk = nn-2*tt  */
    static const int KK = NN - 2 * TT;
    // distance = nn-kk+1 = 2*tt+1

    int* alpha_to;
    int* index_of;
    int* gg;

    int
    gf_mul(int a, int b) const {
        if (a == 0 || b == 0) {
            return 0;
        }
        return alpha_to[(index_of[a] + index_of[b]) % NN];
    }

    int
    gf_div(int a, int b) const {
        if (a == 0) {
            return 0;
        }
        if (b == 0) {
            return 0;
        }
        return alpha_to[(index_of[a] - index_of[b] + NN) % NN];
    }

    int
    gf_alpha_pow(int exponent) const {
        int e = exponent % NN;
        if (e < 0) {
            e += NN;
        }
        return alpha_to[e];
    }

    int
    compute_syndromes(const int* word, int* syndromes) const {
        int syn_error = 0;
        syndromes[0] = 0;
        for (int i = 1; i <= NN - KK; i++) {
            int s = 0;
            for (int j = 0; j < NN; j++) {
                if (word[j] != 0) {
                    s ^= gf_mul(word[j], gf_alpha_pow(i * j));
                }
            }
            syndromes[i] = s;
            if (s != 0) {
                syn_error = 1;
            }
        }
        return syn_error;
    }

    int
    solve_gf_linear_system(int matrix[NN - KK][NN - KK + 1], int size, int* solution) const {
        for (int col = 0; col < size; col++) {
            int pivot = -1;
            for (int row = col; row < size; row++) {
                if (matrix[row][col] != 0) {
                    pivot = row;
                    break;
                }
            }
            if (pivot < 0) {
                return 1;
            }
            if (pivot != col) {
                for (int c = col; c <= size; c++) {
                    int tmp = matrix[col][c];
                    matrix[col][c] = matrix[pivot][c];
                    matrix[pivot][c] = tmp;
                }
            }

            int pivot_value = matrix[col][col];
            for (int c = col; c <= size; c++) {
                matrix[col][c] = gf_div(matrix[col][c], pivot_value);
            }

            for (int row = 0; row < size; row++) {
                if (row == col || matrix[row][col] == 0) {
                    continue;
                }
                int factor = matrix[row][col];
                for (int c = col; c <= size; c++) {
                    matrix[row][c] ^= gf_mul(factor, matrix[col][c]);
                }
            }
        }

        for (int i = 0; i < size; i++) {
            solution[i] = matrix[i][size];
        }
        return 0;
    }

    static int
    polynomial_degree(const int* poly, int max_degree) {
        for (int i = max_degree; i >= 0; i--) {
            if (poly[i] != 0) {
                return i;
            }
        }
        return 0;
    }

    void
    build_erasure_locator(const int* erasures, int n_erasures, int* locator) const {
        for (int i = 0; i <= NN - KK; i++) {
            locator[i] = 0;
        }
        locator[0] = 1;

        int degree = 0;
        for (int e = 0; e < n_erasures; e++) {
            int factor = gf_alpha_pow(erasures[e]);
            for (int i = degree; i >= 0; i--) {
                locator[i + 1] ^= gf_mul(locator[i], factor);
            }
            degree++;
        }
    }

    void
    compute_modified_syndromes(const int* syndromes, const int* erasure_locator, int n_erasures, int* modified) const {
        for (int i = 0; i < NN - KK; i++) {
            int value = 0;
            for (int j = 0; j <= n_erasures && j <= i; j++) {
                value ^= gf_mul(erasure_locator[j], syndromes[(i - j) + 1]);
            }
            modified[i] = value;
        }
    }

    int
    berlekamp_massey(const int* syndromes, int n_syndromes, int* locator, int* degree) const {
        int c[NN - KK + 1];
        int b[NN - KK + 1];
        int t[NN - KK + 1];

        for (int i = 0; i <= NN - KK; i++) {
            c[i] = 0;
            b[i] = 0;
            locator[i] = 0;
        }
        c[0] = 1;
        b[0] = 1;

        int l = 0;
        int m = 1;
        int bb = 1;

        for (int n = 0; n < n_syndromes; n++) {
            int discrepancy = syndromes[n];
            for (int i = 1; i <= l; i++) {
                discrepancy ^= gf_mul(c[i], syndromes[n - i]);
            }

            if (discrepancy == 0) {
                m++;
                continue;
            }

            for (int i = 0; i <= NN - KK; i++) {
                t[i] = c[i];
            }

            if (bb == 0) {
                return 1;
            }
            int coefficient = gf_div(discrepancy, bb);
            for (int i = 0; i + m <= NN - KK; i++) {
                if (b[i] != 0) {
                    c[i + m] ^= gf_mul(coefficient, b[i]);
                }
            }

            if ((2 * l) <= n) {
                l = n + 1 - l;
                for (int i = 0; i <= NN - KK; i++) {
                    b[i] = t[i];
                }
                bb = discrepancy;
                m = 1;
            } else {
                m++;
            }
        }

        for (int i = 0; i <= NN - KK; i++) {
            locator[i] = c[i];
        }
        *degree = polynomial_degree(locator, NN - KK);
        return 0;
    }

    int
    multiply_locator_polynomials(const int* lhs, int lhs_degree, const int* rhs, int rhs_degree, int* out) const {
        if (lhs_degree + rhs_degree > NN - KK) {
            return 1;
        }
        for (int i = 0; i <= NN - KK; i++) {
            out[i] = 0;
        }
        for (int i = 0; i <= lhs_degree; i++) {
            for (int j = 0; j <= rhs_degree; j++) {
                out[i + j] ^= gf_mul(lhs[i], rhs[j]);
            }
        }
        return 0;
    }

    int
    find_error_locations(const int* locator, int degree, int* locations) const {
        if (degree == 0) {
            return 0;
        }

        int count = 0;
        for (int pos = 0; pos < NN; pos++) {
            int x = gf_alpha_pow(NN - pos);
            int value = 0;
            int x_power = 1;
            for (int i = 0; i <= degree; i++) {
                value ^= gf_mul(locator[i], x_power);
                x_power = gf_mul(x_power, x);
            }
            if (value == 0) {
                if (count >= NN - KK) {
                    return count + 1;
                }
                locations[count++] = pos;
            }
        }
        return count;
    }

    static int
    location_list_contains(const int* locations, int n_locations, int position) {
        for (int i = 0; i < n_locations; i++) {
            if (locations[i] == position) {
                return 1;
            }
        }
        return 0;
    }

    struct DecodeState {
        int elp[NN - KK + 2][NN - KK];
        int d[NN - KK + 2];
        int l[NN - KK + 2];
        int u_lu[NN - KK + 2];
        int s[NN - KK + 1];
        int root[TT];
        int loc[TT];
        int z[TT + 1];
        int err[NN];
        int reg[TT + 1];
    };

    static void
    copy_codeword(const int* src, int* dst) {
        for (int i = 0; i < NN; i++) {
            dst[i] = src[i];
        }
    }

    void
    convert_poly_to_index_word(const int* input, int* recd) const {
        for (int i = 0; i < NN; i++) {
            recd[i] = index_of[input[i]];
        }
    }

    void
    convert_index_to_poly_word(int* recd) const {
        for (int i = 0; i < NN; i++) {
            if (recd[i] != -1) {
                recd[i] = alpha_to[recd[i]];
            } else {
                recd[i] = 0;
            }
        }
    }

    int
    compute_index_syndromes(const int* recd, int* s) const {
        int syn_error = 0;
        s[0] = 0;
        for (int i = 1; i <= NN - KK; i++) {
            s[i] = 0;
            for (int j = 0; j < NN; j++) {
                if (recd[j] != -1) {
                    s[i] ^= alpha_to[(recd[j] + i * j) % NN];
                }
            }
            if (s[i] != 0) {
                syn_error = 1;
            }
            s[i] = index_of[s[i]];
        }
        return syn_error;
    }

    static void
    initialize_berlekamp_state(DecodeState& state) {
        state.d[0] = 0;
        state.d[1] = state.s[1];
        state.elp[0][0] = 0;
        state.elp[1][0] = 1;
        for (int i = 1; i < NN - KK; i++) {
            state.elp[0][i] = -1;
            state.elp[1][i] = 0;
        }
        state.l[0] = 0;
        state.l[1] = 0;
        state.u_lu[0] = -1;
        state.u_lu[1] = 0;
    }

    void
    advance_berlekamp_without_discrepancy(DecodeState& state, int u) const {
        state.l[u + 1] = state.l[u];
        for (int i = 0; i <= state.l[u]; i++) {
            state.elp[u + 1][i] = state.elp[u][i];
            state.elp[u][i] = index_of[state.elp[u][i]];
        }
    }

    static int
    select_berlekamp_q(const DecodeState& state, int u) {
        int q = u - 1;
        while ((q > 0) && (state.d[q] == -1)) {
            q--;
        }
        if (q > 0) {
            for (int j = q - 1; j > 0; j--) {
                if ((state.d[j] != -1) && (state.u_lu[q] < state.u_lu[j])) {
                    q = j;
                }
            }
        }
        return q;
    }

    void
    update_berlekamp_with_discrepancy(DecodeState& state, int u, int q) const {
        if (state.l[u] > state.l[q] + u - q) {
            state.l[u + 1] = state.l[u];
        } else {
            state.l[u + 1] = state.l[q] + u - q;
        }

        for (int i = 0; i < NN - KK; i++) {
            state.elp[u + 1][i] = 0;
        }
        for (int i = 0; i <= state.l[q]; i++) {
            if (state.elp[q][i] != -1) {
                state.elp[u + 1][i + u - q] = alpha_to[(state.d[u] + NN - state.d[q] + state.elp[q][i]) % NN];
            }
        }
        for (int i = 0; i <= state.l[u]; i++) {
            state.elp[u + 1][i] ^= state.elp[u][i];
            state.elp[u][i] = index_of[state.elp[u][i]];
        }
    }

    void
    compute_next_discrepancy(DecodeState& state, int u) const {
        if (state.s[u + 1] != -1) {
            state.d[u + 1] = alpha_to[state.s[u + 1]];
        } else {
            state.d[u + 1] = 0;
        }
        for (int i = 1; i <= state.l[u + 1]; i++) {
            if ((state.s[u + 1 - i] != -1) && (state.elp[u + 1][i] != 0)) {
                state.d[u + 1] ^= alpha_to[(state.s[u + 1 - i] + index_of[state.elp[u + 1][i]]) % NN];
            }
        }
        state.d[u + 1] = index_of[state.d[u + 1]];
    }

    int
    run_berlekamp_iterations(DecodeState& state) const {
        int u = 0;
        do {
            u++;
            if (state.d[u] == -1) {
                advance_berlekamp_without_discrepancy(state, u);
            } else {
                int q = select_berlekamp_q(state, u);
                update_berlekamp_with_discrepancy(state, u, q);
            }
            state.u_lu[u + 1] = u - state.l[u + 1];
            if (u < NN - KK) {
                compute_next_discrepancy(state, u);
            }
        } while ((u < NN - KK) && (state.l[u + 1] <= TT));
        return u + 1;
    }

    int
    find_decode_roots(const int* locator, int degree, int* reg, int* root, int* loc) const {
        for (int i = 1; i <= degree; i++) {
            reg[i] = locator[i];
        }

        int count = 0;
        for (int i = 1; i <= NN; i++) {
            int q = 1;
            for (int j = 1; j <= degree; j++) {
                if (reg[j] != -1) {
                    reg[j] = (reg[j] + j) % NN;
                    q ^= alpha_to[reg[j]];
                }
            }
            if (!q) {
                root[count] = i;
                loc[count] = NN - i;
                count++;
            }
        }
        return count;
    }

    void
    build_error_evaluator(const int* s, const int* locator, int degree, int* z) const {
        for (int i = 1; i <= degree; i++) {
            if ((s[i] != -1) && (locator[i] != -1)) {
                z[i] = alpha_to[s[i]] ^ alpha_to[locator[i]];
            } else if ((s[i] != -1) && (locator[i] == -1)) {
                z[i] = alpha_to[s[i]];
            } else if ((s[i] == -1) && (locator[i] != -1)) {
                z[i] = alpha_to[locator[i]];
            } else {
                z[i] = 0;
            }
            for (int j = 1; j < i; j++) {
                if ((s[j] != -1) && (locator[i - j] != -1)) {
                    z[i] ^= alpha_to[(locator[i - j] + s[j]) % NN];
                }
            }
            z[i] = index_of[z[i]];
        }
    }

    void
    prepare_decode_corrections(int* recd, int* err) const {
        for (int i = 0; i < NN; i++) {
            err[i] = 0;
            if (recd[i] != -1) {
                recd[i] = alpha_to[recd[i]];
            } else {
                recd[i] = 0;
            }
        }
    }

    int
    compute_error_numerator(int root_value, int degree, const int* z) const {
        int numerator = 1;
        for (int j = 1; j <= degree; j++) {
            if (z[j] != -1) {
                numerator ^= alpha_to[(z[j] + j * root_value) % NN];
            }
        }
        return numerator;
    }

    int
    compute_error_denominator_exponent(int root_index, int degree, const int* root, const int* loc) const {
        int denominator = 0;
        for (int j = 0; j < degree; j++) {
            if (j != root_index) {
                denominator += index_of[1 ^ alpha_to[(loc[j] + root[root_index]) % NN]];
            }
        }
        return denominator % NN;
    }

    void
    apply_decode_corrections(int* recd, int degree, const int* root, const int* loc, const int* z, int* err) const {
        for (int i = 0; i < degree; i++) {
            int numerator = compute_error_numerator(root[i], degree, z);
            err[loc[i]] = numerator;
            if (numerator != 0) {
                err[loc[i]] = index_of[numerator];
                int denominator = compute_error_denominator_exponent(i, degree, root, loc);
                err[loc[i]] = alpha_to[(err[loc[i]] - denominator + NN) % NN];
                recd[loc[i]] ^= err[loc[i]];
            }
        }
    }

    int
    run_decode(int* recd, DecodeState& state) const {
        initialize_berlekamp_state(state);
        int u = run_berlekamp_iterations(state);
        if (state.l[u] > TT) {
            return 1;
        }

        for (int i = 0; i <= state.l[u]; i++) {
            state.elp[u][i] = index_of[state.elp[u][i]];
        }
        int count = find_decode_roots(state.elp[u], state.l[u], state.reg, state.root, state.loc);
        if (count != state.l[u]) {
            return 1;
        }

        build_error_evaluator(state.s, state.elp[u], state.l[u], state.z);
        prepare_decode_corrections(recd, state.err);
        apply_decode_corrections(recd, state.l[u], state.root, state.loc, state.z, state.err);
        return 0;
    }

    static int
    validate_erasures(const int* erasures, int n_erasures) {
        int seen[NN];
        for (int i = 0; i < NN; i++) {
            seen[i] = 0;
        }
        for (int i = 0; i < n_erasures; i++) {
            int pos = erasures[i];
            if ((pos < 0) || (pos >= NN) || seen[pos]) {
                return 1;
            }
            seen[pos] = 1;
        }
        return 0;
    }

    static void
    clear_augmented_matrix(int matrix[NN - KK][NN - KK + 1]) {
        for (int row = 0; row < NN - KK; row++) {
            for (int col = 0; col < NN - KK + 1; col++) {
                matrix[row][col] = 0;
            }
        }
    }

    void
    build_erasures_matrix(const int* locations, int n_locations, const int* syndromes,
                          int matrix[NN - KK][NN - KK + 1]) const {
        for (int row = 0; row < n_locations; row++) {
            int syndrome_order = row + 1;
            for (int col = 0; col < n_locations; col++) {
                matrix[row][col] = gf_alpha_pow(syndrome_order * locations[col]);
            }
            matrix[row][n_locations] = syndromes[syndrome_order];
        }
    }

    int
    run_decode_with_erasures(int* output, const int* erasures, int n_erasures) const {
        int syndromes[NN - KK + 1];
        if (!compute_syndromes(output, syndromes)) {
            return 0;
        }

        int erasure_locator[NN - KK + 1];
        build_erasure_locator(erasures, n_erasures, erasure_locator);

        int modified_syndromes[NN - KK];
        compute_modified_syndromes(syndromes, erasure_locator, n_erasures, modified_syndromes);

        int unknown_locator[NN - KK + 1];
        int unknown_degree = 0;
        if (berlekamp_massey(modified_syndromes + n_erasures, (NN - KK) - n_erasures, unknown_locator, &unknown_degree)
            != 0) {
            return 1;
        }
        if ((2 * unknown_degree) + n_erasures > NN - KK) {
            return 1;
        }

        int combined_locator[NN - KK + 1];
        int erasure_degree = polynomial_degree(erasure_locator, NN - KK);
        if (multiply_locator_polynomials(erasure_locator, erasure_degree, unknown_locator, unknown_degree,
                                         combined_locator)
            != 0) {
            return 1;
        }

        int combined_degree = polynomial_degree(combined_locator, NN - KK);
        int locations[NN - KK];
        int n_locations = find_error_locations(combined_locator, combined_degree, locations);
        if ((n_locations != combined_degree) || (n_locations > NN - KK)) {
            return 1;
        }
        for (int i = 0; i < n_erasures; i++) {
            if (!location_list_contains(locations, n_locations, erasures[i])) {
                return 1;
            }
        }

        int matrix[NN - KK][NN - KK + 1];
        clear_augmented_matrix(matrix);
        build_erasures_matrix(locations, n_locations, syndromes, matrix);

        int corrections[NN - KK];
        for (int i = 0; i < NN - KK; i++) {
            corrections[i] = 0;
        }
        if (solve_gf_linear_system(matrix, n_locations, corrections) != 0) {
            return 1;
        }
        for (int i = 0; i < n_locations; i++) {
            output[locations[i]] ^= corrections[i];
        }

        if (compute_syndromes(output, syndromes)) {
            return 1;
        }
        return 0;
    }

    void
    generate_gf(const int* generator_polinomial)
    /* generate GF(2**mm) from the irreducible polynomial p(X) in pp[0]..pp[mm]
     lookup tables:  index->polynomial form   alpha_to[] contains j=alpha**i;
     polynomial form -> index form  index_of[j=alpha**i] = i
     alpha=2 is the primitive element of GF(2**mm)
    */
    {
        int i, mask;

        mask = 1;
        alpha_to[MM] = 0;
        for (i = 0; i < MM; i++) {
            alpha_to[i] = mask;
            index_of[alpha_to[i]] = i;
            if (generator_polinomial[i] != 0) {
                alpha_to[MM] ^= mask;
            }
            mask <<= 1;
        }
        index_of[alpha_to[MM]] = MM;
        mask >>= 1;
        for (i = MM + 1; i < NN; i++) {
            if (alpha_to[i - 1] >= mask) {
                alpha_to[i] = alpha_to[MM] ^ ((alpha_to[i - 1] ^ mask) << 1);
            } else {
                alpha_to[i] = alpha_to[i - 1] << 1;
            }
            index_of[alpha_to[i]] = i;
        }
        index_of[0] = -1;
    }

    void
    gen_poly()
    /* Obtain the generator polynomial of the tt-error correcting, length
     nn=(2**mm -1) Reed Solomon code  from the product of (X+alpha**i), i=1..2*tt
     */
    {
        int i, j;

        gg[0] = 2; /* primitive element alpha = 2  for GF(2**mm)  */
        gg[1] = 1; /* g(x) = (X+alpha) initially */
        for (i = 2; i <= NN - KK; i++) {
            gg[i] = 1;
            for (j = i - 1; j > 0; j--) {
                if (gg[j] != 0) {
                    gg[j] = gg[j - 1] ^ alpha_to[(index_of[gg[j]] + i) % NN];
                } else {
                    gg[j] = gg[j - 1];
                }
            }
            gg[0] = alpha_to[(index_of[gg[0]] + i) % NN]; /* gg[0] can never be zero */
        }
        /* convert gg[] to index form for quicker encoding */
        for (i = 0; i <= NN - KK; i++) {
            gg[i] = index_of[gg[i]];
        }
    }

  public:
    ReedSolomon_63() {
        alpha_to = new int[NN + 1];
        index_of = new int[NN + 1];
        gg = new int[NN - KK + 1];

        // Polynom used in P25 is alpha**6+alpha+1
        const int generator_polinomial[] = {1, 1, 0, 0, 0, 0, 1}; /* specify irreducible polynomial coeffts */

        generate_gf(generator_polinomial);

        gen_poly();
    }

    // Non-copyable to prevent double-delete of dynamic arrays
    ReedSolomon_63(const ReedSolomon_63&) = delete;
    ReedSolomon_63& operator=(const ReedSolomon_63&) = delete;

    virtual ~ReedSolomon_63() {
        delete[] gg;
        delete[] index_of;
        delete[] alpha_to;
    }

    void
    encode(const int* data, int* bb) const
    /* take the string of symbols in data[i], i=0..(k-1) and encode systematically
     to produce 2*tt parity symbols in bb[0]..bb[2*tt-1]
     data[] is input and bb[] is output in polynomial form.
     Encoding is done by using a feedback shift register with appropriate
     connections specified by the elements of gg[], which was generated above.
     Codeword is   c(X) = data(X)*X**(nn-kk)+ b(X)          */
    {
        int i, j;

        for (i = 0; i < NN - KK; i++) {
            bb[i] = 0;
        }
        for (i = KK - 1; i >= 0; i--) {
            int feedback = index_of[data[i] ^ bb[NN - KK - 1]];
            if (feedback != -1) {
                for (j = NN - KK - 1; j > 0; j--) {
                    if (gg[j] != -1) {
                        bb[j] = bb[j - 1] ^ alpha_to[(gg[j] + feedback) % NN];
                    } else {
                        bb[j] = bb[j - 1];
                    }
                }
                bb[0] = alpha_to[(gg[0] + feedback) % NN];
            } else {
                for (j = NN - KK - 1; j > 0; j--) {
                    bb[j] = bb[j - 1];
                }
                bb[0] = 0;
            }
        }
    }

    int
    decode(const int* input, int* recd) const
    /* assume we have received bits grouped into mm-bit symbols in recd[i],
     i=0..(nn-1),  and recd[i] is polynomial form.
     We first compute the 2*tt syndromes by substituting alpha**i into rec(X) and
     evaluating, storing the syndromes in s[i], i=1..2tt (leave s[0] zero) .
     Then we use the Berlekamp iteration to find the error location polynomial
     elp[i].   If the degree of the elp is >tt, we cannot correct all the errors
     and hence just put out the information symbols uncorrected. If the degree of
     elp is <=tt, we substitute alpha**i , i=1..n into the elp to get the roots,
     hence the inverse roots, the error location numbers. If the number of errors
     located does not equal the degree of the elp, we have more than tt errors
     and cannot correct them.  Otherwise, we then solve for the error value at
     the error location and correct the error.  The procedure is that found in
     Lin and Costello. For the cases where the number of errors is known to be too
     large to correct, the information symbols as received are output (the
     advantage of systematic encoding is that hopefully some of the information
     symbols will be okay and that if we are in luck, the errors are in the
     parity part of the transmitted codeword).  Of course, these insoluble cases
     can be returned as error flags to the calling routine if desired.   */
    {
        DecodeState state;
        convert_poly_to_index_word(input, recd);
        if (!compute_index_syndromes(recd, state.s)) {
            convert_index_to_poly_word(recd);
            return 0;
        }

        int irrecoverable_error = run_decode(recd, state);
        if (irrecoverable_error) {
            convert_index_to_poly_word(recd);
        }
        return irrecoverable_error;
    }

    int
    decode_with_erasures(const int* input, int* output, const int* erasures, int n_erasures) const {
        if (input == NULL || output == NULL || n_erasures < 0 || n_erasures > NN - KK) {
            return 1;
        }
        copy_codeword(input, output);
        if (n_erasures == 0) {
            int syndromes[NN - KK + 1];
            return compute_syndromes(output, syndromes) ? 1 : 0;
        }
        if (erasures == NULL) {
            return 1;
        }
        if (validate_erasures(erasures, n_erasures)) {
            return 1;
        }

        int status = run_decode_with_erasures(output, erasures, n_erasures);
        if (status != 0) {
            copy_codeword(input, output);
        }
        return status;
    }

  protected:
    static int
    bin_to_hex(const char* input) {
        int output = ((input[0] != 0) ? 32 : 0) | ((input[1] != 0) ? 16 : 0) | ((input[2] != 0) ? 8 : 0)
                     | ((input[3] != 0) ? 4 : 0) | ((input[4] != 0) ? 2 : 0) | ((input[5] != 0) ? 1 : 0);

        return output;
    }

    static void
    hex_to_bin(int input, char* output) {
        output[0] = ((input & 32) != 0) ? 1 : 0;
        output[1] = ((input & 16) != 0) ? 1 : 0;
        output[2] = ((input & 8) != 0) ? 1 : 0;
        output[3] = ((input & 4) != 0) ? 1 : 0;
        output[4] = ((input & 2) != 0) ? 1 : 0;
        output[5] = ((input & 1) != 0) ? 1 : 0;
    }
};

/**
 * Convenience class that does a Reed-Solomon (36,20,17) error correction adapting input and output to
 * the DSD data format: hex words packed as char arrays.
 */
class DSDReedSolomon_36_20_17 : public ReedSolomon_63<8> {
  public:
    // tt = (dd-1)/2
    // dd = 17 --> tt = 8
    DSDReedSolomon_36_20_17() : ReedSolomon_63<8>() {
        // Does nothing
    }

    /**
     * Does a Reed-Solomon decode adapting the input and output to the expected DSD data format.
     * \param hex_data Data packed bits, originally a char[20][6], so containing 20 hex works, each char
     *                 is a bit. Bits are corrected in place.
     * \param hex_parity Parity packed bits, originally a char[16][6], 16 hex words.
     * \return 1 if irrecoverable errors have been detected, 0 otherwise.
     */
    int
    decode(char* hex_data, const char* hex_parity) const {
        int input[63];
        int output[63];

        // First put the parity data, 16 hex words
        for (size_t i = 0; i < 16; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }

        // Then the 20 hex words of data
        for (size_t i = 16; i < 16 + 20; i++) {
            input[i] = bin_to_hex(hex_data + (i - 16) * 6);
        }

        // Fill up with zeros to complete the 47 expected hex words of data
        for (size_t i = 16 + 20; i < 63; i++) {
            input[i] = 0;
        }

        // Now we can call decode on the base class
        int irrecoverable_errors = ReedSolomon_63<8>::decode(input, output);

        // Convert it back to binary and put it into hex_data. If decode failed we should have
        // the input unchanged.
        for (size_t i = 16; i < 16 + 20; i++) {
            hex_to_bin(output[i], hex_data + (i - 16) * 6);
        }

        return irrecoverable_errors;
    }

    /**
     * Does a Reed-Solomon decode with soft erasure information.
     * Uses a simple strategy: try hard decode first, and if it fails, try zeroing
     * erased symbols one at a time to help the decoder.
     *
     * \param hex_data Data packed bits, char[20][6], 20 hex words. Corrected in place.
     * \param hex_parity Parity packed bits, char[16][6], 16 hex words.
     * \param erasures Array of erasure positions in RS codeword space (0-15=parity, 16-35=data).
     * \param n_erasures Number of erasure positions (max 16 for t=8).
     * \return 1 if irrecoverable errors, 0 otherwise.
     */
    int
    decode_soft(char* hex_data, const char* hex_parity, const int* erasures, int n_erasures) const {
        // Try hard decode first
        int result = decode(hex_data, hex_parity);
        if (result == 0) {
            return 0; // Success
        }

        if (n_erasures <= 0 || n_erasures > 16 || erasures == NULL) {
            return 1;
        }

        int input[63];
        int output[63];

        for (size_t i = 0; i < 16; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }
        for (size_t i = 16; i < 16 + 20; i++) {
            input[i] = bin_to_hex(hex_data + (i - 16) * 6);
        }
        for (size_t i = 16 + 20; i < 63; i++) {
            input[i] = 0;
        }

        result = ReedSolomon_63<8>::decode_with_erasures(input, output, erasures, n_erasures);
        if (result == 0) {
            for (size_t i = 16; i < 16 + 20; i++) {
                hex_to_bin(output[i], hex_data + (i - 16) * 6);
            }
        }
        return result;
    }

    void
    encode(const char* hex_data, char* out_hex_parity) const {
        int input[47];
        int output[63];

        // Put the 20 hex words of data
        for (size_t i = 0; i < 20; i++) {
            input[i] = bin_to_hex(hex_data + i * 6);
        }

        // Fill up with zeros to complete the 47 expected hex words of data
        for (size_t i = 20; i < 47; i++) {
            input[i] = 0;
        }

        // Now we can call encode on the base class
        ReedSolomon_63<8>::encode(input, output);

        // Convert it back to binary form and put it into the parity
        for (size_t i = 0; i < 16; i++) {
            hex_to_bin(output[i], out_hex_parity + i * 6);
        }
    }
};

/**
 * Convenience class that does a Reed-Solomon (24,12,13) error correction adapting input and output to
 * the DSD data format: hex words packed as char arrays.
 */
class DSDReedSolomon_24_12_13 : public ReedSolomon_63<6> {
  public:
    // tt = (dd-1)/2
    // dd = 13 --> tt = 6
    DSDReedSolomon_24_12_13() : ReedSolomon_63<6>() {
        // Does nothing
    }

    /**
     * Does a Reed-Solomon decode adapting the input and output to the expected DSD data format.
     * \param hex_data Data packed bits, originally a char[12][6], so containing 12 hex works, each char
     *                 is a bit. Bits are corrected in place.
     * \param hex_parity Parity packed bits, originally a char[12][6], 12 hex words.
     * \return 1 if irrecoverable errors have been detected, 0 otherwise.
     */
    int
    decode(char* hex_data, const char* hex_parity) const {
        int input[63];
        int output[63];

        // First put the parity data, 12 hex words
        for (size_t i = 0; i < 12; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }

        // Then the 12 hex words of data
        for (size_t i = 12; i < 12 + 12; i++) {
            input[i] = bin_to_hex(hex_data + (i - 12) * 6);
        }

        // Fill up with zeros to complete the 51 expected hex words of data
        for (size_t i = 12 + 12; i < 63; i++) {
            input[i] = 0;
        }

        // Now we can call decode on the base class
        int irrecoverable_errors = ReedSolomon_63<6>::decode(input, output);

        // Convert it back to binary and put it into hex_data. If decode failed we should have
        // the input unchanged.
        for (size_t i = 12; i < 12 + 12; i++) {
            hex_to_bin(output[i], hex_data + (i - 12) * 6);
        }

        return irrecoverable_errors;
    }

    /**
     * Does a Reed-Solomon decode with soft erasure information.
     *
     * \param hex_data Data packed bits, char[12][6], 12 hex words. Corrected in place.
     * \param hex_parity Parity packed bits, char[12][6], 12 hex words.
     * \param erasures Array of erasure positions in RS codeword space (0-11=parity, 12-23=data).
     * \param n_erasures Number of erasure positions (max 12 for t=6).
     * \return 1 if irrecoverable errors, 0 otherwise.
     */
    int
    decode_soft(char* hex_data, const char* hex_parity, const int* erasures, int n_erasures) const {
        // Try hard decode first
        int result = decode(hex_data, hex_parity);
        if (result == 0) {
            return 0;
        }

        if (n_erasures <= 0 || n_erasures > 12 || erasures == NULL) {
            return 1;
        }

        int input[63];
        int output[63];

        for (size_t i = 0; i < 12; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }
        for (size_t i = 12; i < 12 + 12; i++) {
            input[i] = bin_to_hex(hex_data + (i - 12) * 6);
        }
        for (size_t i = 12 + 12; i < 63; i++) {
            input[i] = 0;
        }

        result = ReedSolomon_63<6>::decode_with_erasures(input, output, erasures, n_erasures);
        if (result == 0) {
            for (size_t i = 12; i < 12 + 12; i++) {
                hex_to_bin(output[i], hex_data + (i - 12) * 6);
            }
        }
        return result;
    }

    void
    encode(const char* hex_data, char* out_hex_parity) const {
        int input[51];
        int output[63];

        // Put the 12 hex words of data
        for (size_t i = 0; i < 12; i++) {
            input[i] = bin_to_hex(hex_data + i * 6);
        }

        // Fill up with zeros to complete the 51 expected hex words of data
        for (size_t i = 12; i < 51; i++) {
            input[i] = 0;
        }

        // Now we can call encode on the base class
        ReedSolomon_63<6>::encode(input, output);

        // Convert it back to binary form and put it into the parity
        for (size_t i = 0; i < 12; i++) {
            hex_to_bin(output[i], out_hex_parity + i * 6);
        }
    }
};

/**
 * Convenience class that does a Reed-Solomon (24,16,9) error correction adapting input and output to
 * the DSD data format: hex words packed as char arrays.
 */
class DSDReedSolomon_24_16_9 : public ReedSolomon_63<4> {
  public:
    // tt = (dd-1)/2
    // dd = 9 --> tt = 4
    DSDReedSolomon_24_16_9() : ReedSolomon_63<4>() {
        // Does nothing
    }

    /**
     * Does a Reed-Solomon decode adapting the input and output to the expected DSD data format.
     * \param hex_data Data packed bits, originally a char[16][6], so containing 16 hex works, each char
     *                 is a bit. Bits are corrected in place.
     * \param hex_parity Parity packed bits, originally a char[8][6], 8 hex words.
     * \return 1 if irrecoverable errors have been detected, 0 otherwise.
     */
    int
    decode(char* hex_data, const char* hex_parity) const {
        int input[63];
        int output[63];

        // First put the parity data, 8 hex words
        for (size_t i = 0; i < 8; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }

        // Then the 16 hex words of data
        for (size_t i = 8; i < 8 + 16; i++) {
            input[i] = bin_to_hex(hex_data + (i - 8) * 6);
        }

        // Fill up with zeros to complete the 55 expected hex words of data
        for (size_t i = 8 + 16; i < 63; i++) {
            input[i] = 0;
        }

        // Now we can call decode on the base class
        int irrecoverable_errors = ReedSolomon_63<4>::decode(input, output);

        // Convert it back to binary and put it into hex_data. If decode failed we should have
        // the input unchanged.
        for (size_t i = 8; i < 8 + 16; i++) {
            hex_to_bin(output[i], hex_data + (i - 8) * 6);
        }

        return irrecoverable_errors;
    }

    /**
     * Does a Reed-Solomon decode with soft erasure information.
     *
     * \param hex_data Data packed bits, char[16][6], 16 hex words. Corrected in place.
     * \param hex_parity Parity packed bits, char[8][6], 8 hex words.
     * \param erasures Array of erasure positions in RS codeword space (0-7=parity, 8-23=data).
     * \param n_erasures Number of erasure positions (max 8 for t=4).
     * \return 1 if irrecoverable errors, 0 otherwise.
     */
    int
    decode_soft(char* hex_data, const char* hex_parity, const int* erasures, int n_erasures) const {
        // Try hard decode first
        int result = decode(hex_data, hex_parity);
        if (result == 0) {
            return 0;
        }

        if (n_erasures <= 0 || n_erasures > 8 || erasures == NULL) {
            return 1;
        }

        int input[63];
        int output[63];

        for (size_t i = 0; i < 8; i++) {
            input[i] = bin_to_hex(hex_parity + i * 6);
        }
        for (size_t i = 8; i < 8 + 16; i++) {
            input[i] = bin_to_hex(hex_data + (i - 8) * 6);
        }
        for (size_t i = 8 + 16; i < 63; i++) {
            input[i] = 0;
        }

        result = ReedSolomon_63<4>::decode_with_erasures(input, output, erasures, n_erasures);
        if (result == 0) {
            for (size_t i = 8; i < 8 + 16; i++) {
                hex_to_bin(output[i], hex_data + (i - 8) * 6);
            }
        }
        return result;
    }

    void
    encode(const char* hex_data, char* out_hex_parity) const {
        int input[55];
        int output[63];

        // Put the 16 hex words of data
        for (size_t i = 0; i < 16; i++) {
            input[i] = bin_to_hex(hex_data + i * 6);
        }

        // Fill up with zeros to complete the 55 expected hex words of data
        for (size_t i = 16; i < 55; i++) {
            input[i] = 0;
        }

        // Now we can call encode on the base class
        ReedSolomon_63<4>::encode(input, output);

        // Convert it back to binary form and put it into the parity
        for (size_t i = 0; i < 8; i++) {
            hex_to_bin(output[i], out_hex_parity + i * 6);
        }
    }
};

#endif // REEDSOLOMON_HPP_b1405fdab6374ba2a4e65e8d45ec3d80
