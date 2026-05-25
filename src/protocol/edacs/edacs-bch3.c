// SPDX-License-Identifier: ISC
/*
 * File:    bch3.h
 * Title:   Encoder/decoder for binary BCH codes in C (Version 3.1)
 * Author:  Robert Morelos-Zaragoza
 * Date:    August 1994-June 13, 1997
 * Mod:     December 14, 2021, for EDACS-FM BCH Polynomial Error Generation and Detection
 * Mod2:    October 2022, for DSD-FME port of EDACS-FM
 * Source:  www.eccpage.com
 *
 * ===============  Encoder/Decoder for binary BCH codes in C =================
 *
 * Version 1:   Original program. The user provides the generator polynomial
 *              of the code (cumbersome!).
 * Version 2:   Computes the generator polynomial of the code.
 * Version 3:   No need to input the coefficients of a primitive polynomial of
 *              degree m, used to construct the Galois Field GF(2**m). The
 *              program now works for any binary BCH code of length such that:
 *              2**(m-1) - 1 < length <= 2**m - 1
 *
 * Note:        You may have to change the size of the arrays to make it work.
 *
 * The encoding and decoding methods used in this program are based on the
 * book "Error Control Coding: Fundamentals and Applications", by Lin and
 * Costello, Prentice Hall, 1983.
 *
 * Thanks to Patrick Boyle (pboyle@era.com) for his observation that 'bch2.c'
 * did not work for lengths other than 2**m-1 which led to this new version.
 * Portions of this program are from 'rs.c', a Reed-Solomon encoder/decoder
 * in C, written by Simon Rockliff (simon@augean.ua.oz.au) on 21/9/89. The
 * previous version of the BCH encoder/decoder in C, 'bch2.c', was written by
 * Robert Morelos-Zaragoza (robert@spectra.eng.hawaii.edu) on 5/19/92.
 *
 * NOTE:
 *          The author is not responsible for any malfunctioning of
 *          this program, nor for any damage caused by it. Please include the
 *          original program along with these comments in any redistribution.
 *
 *  For more information, suggestions, or other ideas on implementing error
 *  correcting codes, please contact me at:
 *
 *                           Robert Morelos-Zaragoza
 *                           5120 Woodway, Suite 7036
 *                           Houston, Texas 77056
 *
 *                    email: r.morelos-zaragoza@ieee.org
 *
 * COPYRIGHT NOTICE: This computer program is free for non-commercial purposes.
 * You may implement this program for any non-commercial application. You may
 * also implement this program for commercial purposes, provided that you
 * obtain my written permission. Any modification of this program is covered
 * by this copyright.
 *
 * == Copyright (c) 1994-7,  Robert Morelos-Zaragoza. All rights reserved.  ==
 *
 * m = order of the Galois field GF(2**m)
 * n = 2**m - 1 = size of the multiplicative group of GF(2**m)
 * length = length of the BCH code
 * t = error correcting capability (max. no. of errors the code corrects)
 * d = 2*t + 1 = designed min. distance = no. of consecutive roots of g(x) + 1
 * k = n - deg(g(x)) = dimension (no. of information bits/codeword) of the code
 * p[] = coefficients of a primitive polynomial used to generate GF(2**m)
 * g[] = coefficients of the generator polynomial, g(x)
 * alpha_to [] = log table of GF(2**m)
 * index_of[] = antilog table of GF(2**m)
 * ddata[] = information bits = coefficients of data polynomial, i(x)
 * bb[] = coefficients of redundancy polynomial x^(length-k) i(x) modulo g(x)
 * numerr = number of errors
 * errpos[] = error positions
 * recd[] = coefficients of the received polynomial
 * decerror = number of decoding errors (in _message_ positions)
 *
 */

#include <dsd-neo/protocol/edacs/edacs_bch.h>

static int m, n, length, k, t, d;
static int p[21];
static int alpha_to[1048576], index_of[1048576], g[548576];
static int recd[1048576], ddata[1048576], bb[548576];

static unsigned int
primitive_poly_mask_for_degree(int degree) {
    static const unsigned int primitive_masks[21] = {
        [2] = (1u << 1),
        [3] = (1u << 1),
        [4] = (1u << 1),
        [5] = (1u << 2),
        [6] = (1u << 1),
        [7] = (1u << 1),
        [8] = (1u << 4) | (1u << 5) | (1u << 6),
        [9] = (1u << 4),
        [10] = (1u << 3),
        [11] = (1u << 2),
        [12] = (1u << 3) | (1u << 4) | (1u << 7),
        [13] = (1u << 1) | (1u << 3) | (1u << 4),
        [14] = (1u << 1) | (1u << 11) | (1u << 12),
        [15] = (1u << 1),
        [16] = (1u << 2) | (1u << 3) | (1u << 5),
        [17] = (1u << 3),
        [18] = (1u << 7),
        [19] = (1u << 1) | (1u << 5) | (1u << 6),
        [20] = (1u << 3),
    };

    if (degree < 0 || degree >= 21) {
        return 0u;
    }
    return primitive_masks[degree];
}

static void
set_primitive_polynomial(int degree) {
    int i;
    unsigned int mask = primitive_poly_mask_for_degree(degree);

    for (i = 1; i < degree; i++) {
        p[i] = 0;
    }
    p[0] = 1;
    p[degree] = 1;

    for (i = 1; i < degree; i++) {
        if ((mask & (1u << i)) != 0u) {
            p[i] = 1;
        }
    }
}

static void
read_p(void)
/*
 *	Read m, the degree of a primitive polynomial p(x) used to compute the
 *	Galois field GF(2**m). Get precomputed coefficients p[] of p(x). Read
 *	the code length.
 */
{
    int ninf;

    do {
        m = 6;
    } while (!(m > 1) || !(m < 21));

    set_primitive_polynomial(m);

    n = (1 << m) - 1;
    ninf = (n + 1) / 2 - 1;

    do {
        length = 40;
    } while (!((length <= n) && (length > ninf)));
}

static void
generate_gf(void)
/*
 * Generate field GF(2**m) from the irreducible polynomial p(X) with
 * coefficients in p[0]..p[m].
 *
 * Lookup tables:
 *   index->polynomial form: alpha_to[] contains j=alpha^i;
 *   polynomial form -> index form:	index_of[j=alpha^i] = i
 *
 * alpha=2 is the primitive element of GF(2**m)
 */
{
    register int i, mask;

    mask = 1;
    alpha_to[m] = 0;
    for (i = 0; i < m; i++) {
        alpha_to[i] = mask;
        index_of[alpha_to[i]] = i;
        if (p[i] != 0) {
            alpha_to[m] ^= mask;
        }
        mask <<= 1;
    }
    index_of[alpha_to[m]] = m;
    mask >>= 1;
    for (i = m + 1; i < n; i++) {
        if (alpha_to[i - 1] >= mask) {
            alpha_to[i] = alpha_to[m] ^ ((alpha_to[i - 1] ^ mask) << 1);
        } else {
            alpha_to[i] = alpha_to[i - 1] << 1;
        }
        index_of[alpha_to[i]] = i;
    }
    index_of[0] = -1;
}

static int
is_value_in_cycle_sets(int cycle[1024][21], int size[1024], int cycle_count, int value) {
    int ii, kaux;

    for (ii = 1; ii <= cycle_count; ii++) {
        for (kaux = 0; kaux < size[ii]; kaux++) {
            if (value == cycle[ii][kaux]) {
                return 1;
            }
        }
    }
    return 0;
}

static void
generate_cycle_set(int cycle[1024][21], int size[1024], int cycle_index) {
    int ii = 0;
    int aux;

    do {
        ii++;
        cycle[cycle_index][ii] = (cycle[cycle_index][ii - 1] * 2) % n;
        size[cycle_index]++;
        aux = (cycle[cycle_index][ii] * 2) % n;
    } while (aux != cycle[cycle_index][0]);
}

static int
find_next_cycle_representative(int cycle[1024][21], int size[1024], int cycle_count, int* found_new) {
    int candidate = 0;
    int seen;

    do {
        candidate++;
        seen = is_value_in_cycle_sets(cycle, size, cycle_count, candidate);
    } while (seen && (candidate < (n - 1)));

    *found_new = !seen;
    return candidate;
}

static int
cycle_contains_root(int cycle[1024][21], int size[1024], int cycle_index) {
    int jj, root;

    for (jj = 0; jj < size[cycle_index]; jj++) {
        for (root = 1; root < d; root++) {
            if (root == cycle[cycle_index][jj]) {
                return 1;
            }
        }
    }
    return 0;
}

static void
generate_cycle_sets(int cycle[1024][21], int size[1024], int* nocycles) {
    int jj = 1;
    int ll;
    int found_new;

    /* Generate cycle sets modulo n, n = 2**m - 1 */
    cycle[0][0] = 0;
    size[0] = 1;
    cycle[1][0] = 1;
    size[1] = 1;

    do {
        generate_cycle_set(cycle, size, jj);
        ll = find_next_cycle_representative(cycle, size, jj, &found_new);
        if (found_new) {
            jj++;
            cycle[jj][0] = ll;
            size[jj] = 1;
        }
    } while (ll < (n - 1));

    *nocycles = jj;
}

static void
select_generator_cycles(int cycle[1024][21], int size[1024], int nocycles, int min[1024], int* noterms, int* rdncy) {
    int ii;
    int min_index = 0;

    *rdncy = 0;
    for (ii = 1; ii <= nocycles; ii++) {
        if (cycle_contains_root(cycle, size, ii)) {
            min[min_index] = ii;
            *rdncy += size[ii];
            min_index++;
        }
    }
    *noterms = min_index;
}

static void
collect_zero_exponents(int cycle[1024][21], int size[1024], int min[1024], int noterms, int zeros[1024]) {
    int ii, jj;
    int zero_index = 1;

    for (ii = 0; ii < noterms; ii++) {
        for (jj = 0; jj < size[min[ii]]; jj++) {
            zeros[zero_index] = cycle[min[ii]][jj];
            zero_index++;
        }
    }
}

static void
build_generator_polynomial(const int zeros[1024], int rdncy) {
    int ii, jj;

    /* g(x) = (X + zeros[1]) initially */
    g[0] = alpha_to[zeros[1]];
    g[1] = 1;
    for (ii = 2; ii <= rdncy; ii++) {
        g[ii] = 1;
        for (jj = ii - 1; jj > 0; jj--) {
            if (g[jj] != 0) {
                g[jj] = g[jj - 1] ^ alpha_to[(index_of[g[jj]] + zeros[ii]) % n];
            } else {
                g[jj] = g[jj - 1];
            }
        }
        g[0] = alpha_to[(index_of[g[0]] + zeros[ii]) % n];
    }
}

static void
gen_poly(void)
/*
 * Compute the generator polynomial of a binary BCH code. Fist generate the
 * cycle sets modulo 2**m - 1, cycle[][] =  (i, 2*i, 4*i, ..., 2^l*i). Then
 * determine those cycle sets that contain integers in the set of (d-1)
 * consecutive integers {1..(d-1)}. The generator polynomial is calculated
 * as the product of linear factors of the form (x+alpha^i), for every i in
 * the above cycle sets.
 */
{
    int nocycles, noterms, rdncy;
    int cycle[1024][21], size[1024], min[1024], zeros[1024];

    generate_cycle_sets(cycle, size, &nocycles);

    t = 2;
    d = 2 * t + 1;

    select_generator_cycles(cycle, size, nocycles, min, &noterms, &rdncy);
    collect_zero_exponents(cycle, size, min, noterms, zeros);

    k = length - rdncy;
    if (k < 0) {
        return;
    }

    build_generator_polynomial(zeros, rdncy);
}

static void
encode_bch(void)
/*
 * Compute redundacy bb[], the coefficients of b(x). The redundancy
 * polynomial b(x) is the remainder after dividing x^(length-k)*ddata(x)
 * by the generator polynomial g(x).
 */
{
    register int i, j;

    if (k <= 0 || length <= k) {
        return; // invalid parameters; avoid out-of-bounds when indexing bb[length-k-1]
    }
    for (i = 0; i < length - k; i++) {
        bb[i] = 0;
    }
    for (i = k - 1; i >= 0; i--) {
        register int feedback = ddata[i] ^ bb[length - k - 1];
        if (feedback != 0) {
            for (j = length - k - 1; j > 0; j--) {
                if (g[j] != 0) {
                    bb[j] = bb[j - 1] ^ feedback;
                } else {
                    bb[j] = bb[j - 1];
                }
            }
            bb[0] = g[0] && feedback;
        } else {
            for (j = length - k - 1; j > 0; j--) {
                bb[j] = bb[j - 1];
            }
            bb[0] = 0;
        }
    }
}

//very simplified version, just to encode, get frame and compare
unsigned long long int
edacs_bch(unsigned long long int message) {
    int i;
    unsigned long long int messagepp = 0ULL;
    //not ideal to run these every frame, will want to eventually run once on start of DSD-neo, and store values
    read_p();      /* Read m */
    generate_gf(); /* Construct the Galois Field GF(2**m) */
    gen_poly();    /* Compute the generator polynomial of BCH code */
    //test shows this works, but output from program is that the poly is backwards, not sure if it corrects okay though
    for (i = 0; i < k; i++) {
        ddata[i] = ((message >> i) & 0x1); //loaded up backwards? or just outputs backwards?
    }

    encode_bch(); /* encode data */

    /*
	 * recd[] are the coefficients of c(x) = x**(length-k)*data(x) + b(x)
	 */
    for (i = 0; i < length - k; i++) {
        recd[i] = bb[i];
    }
    for (i = 0; i < k; i++) {
        recd[i + length - k] = ddata[i];
    }

    for (i = 0; i < length; i++) {
        messagepp = (messagepp << 1) | recd[39 - i]; //get it in the correct direction
    }

    return (messagepp);
}
