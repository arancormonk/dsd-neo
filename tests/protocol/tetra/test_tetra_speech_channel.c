// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The speech channel decoder against an encoder written straight from EN 300 395-2 clause 5: the CRC of equations 43
 * to 45 (seven bits plus an overall parity bit on the full slot, clause 5.5.1; four bits on the half slot, clause
 * 5.6.1), the rate-1/3 mother code of equation 47, the puncturing of equation 49 restarting at the first class-2 bit,
 * and the full slot's matrix interleaving (equation 52) or the half slot's (216, 101) block interleaving. A clean or
 * lightly corrupted burst decodes to its bits with the CRC passing; a wrong parity bit fails it; random blocks are
 * almost never taken for speech.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

typedef struct {
    int coded;     /* type-3 bits */
    int class0;    /* uncoded type-1 bits */
    int class1;    /* rate 8/12 */
    int class2;    /* class-2 type-1 bits */
    int crc_bits;  /* CRC bits from G(X) */
    int overall;   /* 1: an overall parity bit follows (full slot) */
    uint8_t g_low; /* G(X) below its top term */
    int c2_t;      /* class-2 puncturing: t, Period and P(1..t) */
    int c2_period;
    const uint8_t* c2_p;
} speech_layout;

static const uint8_t k_p_8_12[] = {1, 2, 4};
static const uint8_t k_p_8_18[] = {1, 2, 3, 4, 5, 7, 8, 10, 11};
static const uint8_t k_p_8_17[] = {1, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23};

/* Clause 5.5: G(X) = 1 + X^3 + X^7 and the overall parity bit; class 2 at rate 8/18. */
static const speech_layout k_full = {432, 102, 112, 60, 7, 1, 0x09, 9, 12, k_p_8_18};
/* Clause 5.6: G(X) = 1 + X + X^4; class 2 at rate 8/17. */
static const speech_layout k_half = {216, 51, 56, 30, 4, 0, 0x03, 17, 24, k_p_8_17};

static uint32_t g_rng = 0x2468ACE1u;

static int
next_bit(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (int)(g_rng & 1u);
}

/* F(X) = X^n I(X) mod G(X) by polynomial long division, I(X) = d[0] + d[1] X + ...; bit i of the result is f(i). */
static unsigned
spec_crc(const uint8_t* d, int len, int n, uint8_t g_low) {
    uint8_t rem[128];
    DSD_MEMSET(rem, 0, sizeof(rem));
    for (int i = 0; i < len; i++) {
        rem[n + i] = d[i];
    }
    for (int deg = len + n - 1; deg >= n; deg--) {
        if (rem[deg]) {
            rem[deg] = 0;
            for (int j = 0; j < n; j++) {
                rem[deg - n + j] ^= (uint8_t)((g_low >> j) & 1u);
            }
        }
    }
    unsigned out = 0;
    for (int j = 0; j < n; j++) {
        out |= (unsigned)rem[j] << j;
    }
    return out;
}

/* Equation 47 with G1 = 1 + D + D^2 + D^3 + D^4, G2 = 1 + D + D^3 + D^4, G3 = 1 + D^2 + D^4. */
static void
mother_encode(const uint8_t* c2, int k2, uint8_t* v) {
    static const uint8_t g[3][5] = {{1, 1, 1, 1, 1}, {1, 1, 0, 1, 1}, {1, 0, 1, 0, 1}};
    for (int k = 0; k < k2; k++) {
        for (int i = 0; i < 3; i++) {
            uint8_t sum = 0;
            for (int j = 0; j <= 4; j++) {
                if (k - j >= 0 && g[i][j]) {
                    sum ^= c2[k - j];
                }
            }
            v[(3 * k) + i] = sum;
        }
    }
}

/* Equation 49: C3(j) = V(k), k = Period * ((j - 1) div t) + P(j - t * ((j - 1) div t)). */
static int
puncture(const uint8_t* v, int count, int t, int period, const uint8_t* p, uint8_t* out) {
    for (int j = 1; j <= count; j++) {
        const int block = (j - 1) / t;
        out[j - 1] = v[(period * block) + p[(j - 1) - (t * block)] - 1];
    }
    return count;
}

/* Encode @p type1 (class 0, 1, 2) into the type-4 bits of the layout; @p flip_parity inverts one parity bit. */
static void
encode(const speech_layout* lay, const uint8_t* type1, int flip_parity, uint8_t* type4) {
    uint8_t c2[256];
    uint8_t v[768];
    uint8_t type3[432];
    const int k2 = lay->class1 + lay->class2 + lay->crc_bits + lay->overall + 4;
    const uint8_t* class2 = type1 + lay->class0 + lay->class1;
    DSD_MEMSET(c2, 0, sizeof(c2));
    DSD_MEMSET(v, 0, sizeof(v));
    DSD_MEMSET(type3, 0, sizeof(type3));
    if (k2 > (int)sizeof(c2) || lay->coded > (int)sizeof(type3)) {
        return;
    }

    /* The parity bits follow class 1 and class 2 among the coded type-2 bits. */
    const size_t parity_at = (size_t)lay->class1 + (size_t)lay->class2;
    DSD_MEMCPY(c2, type1 + lay->class0, parity_at);
    const unsigned crc = spec_crc(class2, lay->class2, lay->crc_bits, lay->g_low);
    uint8_t parity = 0;
    for (int i = 0; i < lay->class2; i++) {
        parity ^= class2[i];
    }
    for (unsigned i = 0; i < (unsigned)lay->crc_bits; i++) {
        c2[parity_at + i] = (uint8_t)((crc >> i) & 1u);
        parity ^= (uint8_t)((crc >> i) & 1u);
    }
    if (lay->overall) {
        c2[parity_at + (size_t)lay->crc_bits] = parity;
    }
    if (flip_parity > 0) {
        c2[parity_at + (size_t)flip_parity - 1U] ^= 1u;
    }
    mother_encode(c2, k2, v);

    DSD_MEMCPY(type3, type1, (size_t)lay->class0);
    const int c1_coded = puncture(v, (lay->class1 * 3) / 2, 3, 6, k_p_8_12, type3 + lay->class0);
    (void)puncture(v + ((size_t)lay->class1 * 3U), lay->coded - lay->class0 - c1_coded, lay->c2_t, lay->c2_period,
                   lay->c2_p, type3 + lay->class0 + c1_coded);

    if (lay->coded == 432) {
        for (int i = 0; i < 18; i++) {
            for (int j = 0; j < 24; j++) {
                type4[(i * 24) + j] = type3[(j * 18) + i];
            }
        }
    } else {
        for (int i = 1; i <= lay->coded; i++) {
            type4[(101 * i) % lay->coded] = type3[i - 1];
        }
    }
}

/* Where type-3 bit @p p (0-based) is sent. */
static int
type4_index(const speech_layout* lay, int p) {
    if (lay->coded == 432) {
        return ((p % 18) * 24) + (p / 18);
    }
    return (101 * (p + 1)) % lay->coded;
}

/* Decode type-4 bits as the receiver does, with the type-3 bits at @p flips (0-based) received wrong. */
static int
decode(const speech_layout* lay, const uint8_t* type4, const int* flips, int nflips, uint8_t* out, int* crc_ok,
       int* mismatches) {
    uint16_t soft[432];
    uint16_t deint[432];
    for (int i = 0; i < lay->coded; i++) {
        soft[i] = type4[i] ? 0xFFFFu : 0x0000u;
    }
    for (int i = 0; i < nflips; i++) {
        soft[type4_index(lay, flips[i])] ^= 0xFFFFu;
    }
    if (lay->coded == 432) {
        tetra_speech_deinterleave_soft(soft, deint);
        return tetra_speech_full_decode(deint, out, crc_ok, mismatches);
    }
    tetra_block_deinterleave_soft(soft, deint, lay->coded, 101);
    return tetra_speech_half_decode(deint, out, crc_ok, mismatches);
}

static void
test_layout(const speech_layout* lay, const char* name, int max_mismatch) {
    const int nbits = lay->class0 + lay->class1 + lay->class2;
    uint8_t type1[274];
    uint8_t type4[432];
    uint8_t out[274];
    int crc_ok = 0;
    int mm = -1;
    char label[128];
    for (int i = 0; i < (int)sizeof(type1); i++) {
        type1[i] = i < nbits ? (uint8_t)next_bit() : 0u;
    }

    encode(lay, type1, 0, type4);
    DSD_SNPRINTF(label, sizeof(label), "%s: clean burst decodes, CRC passes, nothing to correct", name);
    expect(decode(lay, type4, NULL, 0, out, &crc_ok, &mm) == nbits && memcmp(out, type1, (size_t)nbits) == 0
               && crc_ok == 1 && mm == 0,
           label);

    /* Spread channel errors among the coded bits are corrected and counted. */
    const int c = lay->class0;
    const int flips[] = {c + 9, c + 45, c + 90, lay->coded - 60, lay->coded - 5};
    DSD_SNPRINTF(label, sizeof(label), "%s: five channel errors corrected", name);
    expect(decode(lay, type4, flips, 5, out, &crc_ok, &mm) == nbits && memcmp(out, type1, (size_t)nbits) == 0
               && crc_ok == 1 && mm == 5,
           label);

    /* A burst whose parity disagrees with its class-2 bits fails, whichever parity bit it is. */
    for (int bit = 1; bit <= lay->crc_bits + lay->overall; bit++) {
        encode(lay, type1, bit, type4);
        DSD_SNPRINTF(label, sizeof(label), "%s: parity bit b%d wrong fails the CRC", name, bit);
        expect(decode(lay, type4, NULL, 0, out, &crc_ok, &mm) == nbits && crc_ok == 0, label);
    }

    int accepted = 0;
    for (int n = 0; n < 400; n++) {
        for (int i = 0; i < lay->coded; i++) {
            type4[i] = (uint8_t)next_bit();
        }
        if (decode(lay, type4, NULL, 0, out, &crc_ok, &mm) == nbits && crc_ok && mm <= max_mismatch) {
            accepted++;
        }
    }
    DSD_SNPRINTF(label, sizeof(label), "%s: %d of 400 random blocks taken for speech", name, accepted);
    expect(accepted <= 2, label);
}

int
main(void) {
    test_layout(&k_full, "full slot", 28);
    test_layout(&k_half, "half slot", 16);
    uint8_t out[274];
    int crc_ok = 1;
    int mm = 0;
    expect(tetra_speech_full_decode(NULL, out, &crc_ok, &mm) == -1 && crc_ok == 0 && mm == -1, "no input fails");
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_speech_channel");
    return 0;
}
