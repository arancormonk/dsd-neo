// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Frame-sync Hamming distance helpers.
 *
 * False-alarm probability derivation
 * ----------------------------------
 * Let N be the sync-pattern length in dibits. Under the null hypothesis of
 * uniformly random 4-ary symbols, each received dibit matches the expected
 * pattern position independently with probability 1/4, so the Hamming
 * distance H is Binomial(N, 3/4):
 *
 *   P(H = k) = C(N,k) * (3/4)^k * (1/4)^(N-k)
 *
 * For a single-pattern threshold test "declare sync if H <= t" the per-trial
 * false-alarm probability is the CDF
 *
 *   Pfa_single(N, t) = sum_{k=0..t} P(H = k).
 *
 * Tabulated values (N = 24, the P25/DMR sync length):
 *
 *   t = 0: Pfa ≈ 3.55e-15
 *   t = 2: Pfa ≈ 9.12e-12
 *   t = 4: Pfa ≈ 3.26e-9
 *   t = 6: Pfa ≈ 3.9e-7
 *   t = 8: Pfa ≈ 1.4e-5
 *
 * The `_with_remaps` helper evaluates ten candidate transforms (identity,
 * invert, bit-swap, xor-0x3, 90° rotation — each against the normal and
 * inverted reference pattern) and returns the minimum. Under the Bonferroni
 * union bound this multiplies the effective per-trial false-alarm rate by at
 * most the number of candidates (<= 10), so any application threshold should
 * use the adjusted bound
 *
 *   Pfa_effective ≤ 10 * Pfa_single(N, t).
 *
 * The remap transforms are not independent (e.g. rotation and inversion share
 * orbit structure on Z/4Z), so the actual factor is smaller; 10 is a safe
 * upper bound.
 *
 * Sliding-window detection rate
 * -----------------------------
 * At symbol rate Rs the per-second false-alarm rate across a continuous
 * search is Rs * Pfa_effective. For P25 phase 1 (Rs = 4800 sym/s) and a
 * threshold of t = 4, this gives a bound of ~1.6e-4 false syncs per second,
 * or about one per 1.7 hours of noise. In practice the decoder requires
 * multiple confirming symbols/frames after the initial latch, which drives
 * the end-to-end false-sync rate far below this per-trial bound.
 *
 * Callers choosing thresholds should reason in terms of Pfa_effective and
 * tolerable false-alarm-rate per unit time at the symbol rate of the mode.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/sync_hamming.h>
#include <stdint.h>
#include "sync_hamming_internal.h"

#if (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)         \
    || defined(_MSC_VER)
#define DSD_SYNC_HAMMING_LITTLE_ENDIAN 1
#else
#define DSD_SYNC_HAMMING_LITTLE_ENDIAN 0
#endif

#if defined(_MSC_VER)
#define DSD_SYNC_INLINE   __forceinline
#define DSD_SYNC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define DSD_SYNC_INLINE   inline __attribute__((always_inline))
#define DSD_SYNC_NOINLINE __attribute__((noinline))
#else
#define DSD_SYNC_INLINE inline
#define DSD_SYNC_NOINLINE
#endif

/* The static analyzer reads the portable paths only, as with the other DSP SIMD kernels. */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__clang_analyzer__)
#include <emmintrin.h>
#define DSD_SYNC_HAMMING_HAVE_SSE2 1
#else
#define DSD_SYNC_HAMMING_HAVE_SSE2 0
#endif

#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(__clang_analyzer__)
#include <arm_neon.h>
#define DSD_SYNC_HAMMING_HAVE_NEON 1
#else
#define DSD_SYNC_HAMMING_HAVE_NEON 0
#endif

/*
 * Fast path
 * ---------
 * The hunt calls these on every symbol with ASCII dibits. A window of 8..64 dibits is read in
 * chunks (16 bytes with SSE2, 8-byte words otherwise); a length that is not a multiple of the chunk
 * rereads the last full chunk ending at the window's end, and lanes already seen are dropped when
 * the chunk's bits are placed. Each byte becomes one bit per plane (bit k of `lo`/`hi` is bit 0/1 of
 * dibit k), and mismatches are XOR + popcount. Every remap is a bit-plane identity on dibits 0..3:
 *
 *   invert = d ^ 2, xor3 = d ^ 3, swap = (d0, d1), rotate = swap ^ 1.
 *
 * Other lengths, receive bytes other than 0..3 / '0'..'3', and pattern bytes other than '0'..'3'
 * take the original per-dibit loops, whose handling of such bytes differs between the two entry
 * points and is kept exactly.
 */
enum {
    DSD_SYNC_REMAP_COUNT = 5,
    DSD_SYNC_REFERENCE_COUNT = 2,
    DSD_SYNC_FAST_MIN_LEN = 8,
    DSD_SYNC_FAST_MAX_LEN = 64,
};

typedef struct {
    uint64_t lo;
    uint64_t hi;
} dsd_sync_planes;

static DSD_SYNC_INLINE int
dsd_sync_popcount64(uint64_t v) {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__aarch64__) || defined(__POPCNT__))
    return __builtin_popcountll(v);
#else
    /* POPCNT is not baseline x86-64 (nor available to MSVC without it), so count bits portably. */
    v = v - ((v >> 1) & UINT64_C(0x5555555555555555));
    v = (v & UINT64_C(0x3333333333333333)) + ((v >> 2) & UINT64_C(0x3333333333333333));
    v = (v + (v >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    return (int)((v * UINT64_C(0x0101010101010101)) >> 56);
#endif
}

/* Nonzero if a byte of `w` is out of range: a receive byte (0..3 or 0x30..0x33) needs bits 2, 3, 6, 7
 * clear and bit 4 equal to bit 5; a pattern byte ('0'..'3') needs 0x30 in its top six bits. */
static DSD_SYNC_INLINE uint64_t
dsd_sync_word_bad(uint64_t w, int pattern) {
    if (pattern) {
        return (w & UINT64_C(0xFCFCFCFCFCFCFCFC)) ^ UINT64_C(0x3030303030303030);
    }
    return (w & UINT64_C(0xCCCCCCCCCCCCCCCC)) | ((w ^ (w >> 1)) & UINT64_C(0x1010101010101010));
}

/* Gather bit 0 of each byte of `w` into the low 8 bits; on a little-endian load byte k lands in bit k. */
static DSD_SYNC_INLINE uint64_t
dsd_sync_gather_bit0(uint64_t w) {
    return ((w & UINT64_C(0x0101010101010101)) * UINT64_C(0x0102040810204080)) >> 56;
}

/* Words covering a window (0: take the per-dibit loop). The overlapped last word needs byte k in bit k. */
static DSD_SYNC_INLINE int
dsd_sync_word_count(int len) {
    if (len % 8 == 0) {
        return len / 8;
    }
    return DSD_SYNC_HAMMING_LITTLE_ENDIAN ? len / 8 + 1 : 0;
}

static DSD_SYNC_INLINE uint64_t
dsd_sync_load_word(const char* s, int len, int k) {
    int off = 8 * k;
    if (off + 8 > len) {
        off = len - 8;
    }
    uint64_t w = 0;
    DSD_MEMCPY(&w, s + off, sizeof w);
    return w;
}

/* Moves the 8 lane bits of word k to their window positions, dropping lanes an overlapped word repeats. */
static DSD_SYNC_INLINE uint64_t
dsd_sync_place_word(uint64_t bits, int len, int k) {
    const int i = 8 * k;
    if (i + 8 > len) {
        return (bits >> (i + 8 - len)) << i;
    }
    return bits << i;
}

/* Packs receive and pattern windows; 0 if any byte needs the per-dibit loop. */
static DSD_SYNC_INLINE int
dsd_sync_pack_portable(const char* buf, const char* pat_a, const char* pat_b, int len, dsd_sync_planes* d,
                       dsd_sync_planes* a, dsd_sync_planes* b) {
    const int n = dsd_sync_word_count(len);
    uint64_t bad = n == 0;
    dsd_sync_planes pd = {0, 0};
    dsd_sync_planes pa = {0, 0};
    dsd_sync_planes pb = {0, 0};
    for (int k = 0; k < n; k++) {
        const uint64_t wd = dsd_sync_load_word(buf, len, k);
        const uint64_t wa = dsd_sync_load_word(pat_a, len, k);
        const uint64_t wb = dsd_sync_load_word(pat_b, len, k);
        bad |= dsd_sync_word_bad(wd, 0) | dsd_sync_word_bad(wa, 1) | dsd_sync_word_bad(wb, 1);
        pd.lo |= dsd_sync_place_word(dsd_sync_gather_bit0(wd), len, k);
        pd.hi |= dsd_sync_place_word(dsd_sync_gather_bit0(wd >> 1), len, k);
        pa.lo |= dsd_sync_place_word(dsd_sync_gather_bit0(wa), len, k);
        pa.hi |= dsd_sync_place_word(dsd_sync_gather_bit0(wa >> 1), len, k);
        pb.lo |= dsd_sync_place_word(dsd_sync_gather_bit0(wb), len, k);
        pb.hi |= dsd_sync_place_word(dsd_sync_gather_bit0(wb >> 1), len, k);
    }
    *d = pd;
    *a = pa;
    *b = pb;
    return bad == 0U;
}

#if DSD_SYNC_HAMMING_HAVE_SSE2 || DSD_SYNC_HAMMING_HAVE_NEON
/* 16-byte chunks covering a window; one chunk for 8..15 bytes holds the first and last eight. */
static DSD_SYNC_INLINE int
dsd_sync_chunk_count(int len) {
    return len < 16 ? 1 : (len + 15) / 16;
}
#endif

#if DSD_SYNC_HAMMING_HAVE_SSE2
static DSD_SYNC_INLINE __m128i
dsd_sync_load_chunk(const char* s, int len, int k) {
    if (len < 16) {
        return _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i*)(const void*)s),
                                  _mm_loadl_epi64((const __m128i*)(const void*)(s + len - 8)));
    }
    int off = 16 * k;
    if (off + 16 > len) {
        off = len - 16;
    }
    return _mm_loadu_si128((const __m128i*)(const void*)(s + off));
}

/* Moves a chunk's 16 lane bits to their window positions, dropping lanes the chunk repeats. */
static DSD_SYNC_INLINE uint64_t
dsd_sync_place_chunk(int lanes, int len, int k) {
    const uint64_t m = (uint64_t)(unsigned)lanes;
    if (len < 16) {
        /* Lanes 8..15 hold bytes len - 8 .. len - 1; byte 8 is lane 24 - len. */
        return (m & 0xFFU) | ((m >> (24 - len)) << 8);
    }
    const int i = 16 * k;
    if (i + 16 > len) {
        return (m >> (i + 16 - len)) << i;
    }
    return m << i;
}

static DSD_SYNC_INLINE __m128i
dsd_sync_good_sse2(__m128i v, int pattern) {
    const __m128i t = _mm_and_si128(v, _mm_set1_epi8(-4)); /* 0xFC */
    const __m128i ascii = _mm_cmpeq_epi8(t, _mm_set1_epi8(0x30));
    return pattern ? ascii : _mm_or_si128(ascii, _mm_cmpeq_epi8(t, _mm_setzero_si128()));
}

static DSD_SYNC_INLINE int
dsd_sync_pack(const char* buf, const char* pat_a, const char* pat_b, int len, dsd_sync_planes* d, dsd_sync_planes* a,
              dsd_sync_planes* b) {
    const int n = dsd_sync_chunk_count(len);
    __m128i good = _mm_set1_epi8(-1);
    dsd_sync_planes pd = {0, 0};
    dsd_sync_planes pa = {0, 0};
    dsd_sync_planes pb = {0, 0};
    for (int k = 0; k < n; k++) {
        const __m128i vd = dsd_sync_load_chunk(buf, len, k);
        const __m128i va = dsd_sync_load_chunk(pat_a, len, k);
        const __m128i vb = dsd_sync_load_chunk(pat_b, len, k);
        good = _mm_and_si128(good, _mm_and_si128(dsd_sync_good_sse2(vd, 0),
                                                 _mm_and_si128(dsd_sync_good_sse2(va, 1), dsd_sync_good_sse2(vb, 1))));
        pd.lo |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(vd, 7)), len, k);
        pd.hi |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(vd, 6)), len, k);
        pa.lo |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(va, 7)), len, k);
        pa.hi |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(va, 6)), len, k);
        pb.lo |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(vb, 7)), len, k);
        pb.hi |= dsd_sync_place_chunk(_mm_movemask_epi8(_mm_slli_epi16(vb, 6)), len, k);
    }
    *d = pd;
    *a = pa;
    *b = pb;
    return _mm_movemask_epi8(good) == 0xFFFF;
}

static DSD_SYNC_INLINE int
dsd_sync_distance_simd(const char* buf, const char* pat, int len, int* ham) {
    const int n = dsd_sync_chunk_count(len);
    __m128i good = _mm_set1_epi8(-1);
    uint64_t mism = 0;
    for (int k = 0; k < n; k++) {
        const __m128i vd = dsd_sync_load_chunk(buf, len, k);
        const __m128i vp = dsd_sync_load_chunk(pat, len, k);
        good = _mm_and_si128(good, _mm_and_si128(dsd_sync_good_sse2(vd, 0), dsd_sync_good_sse2(vp, 1)));
        const __m128i same =
            _mm_cmpeq_epi8(_mm_and_si128(_mm_xor_si128(vd, vp), _mm_set1_epi8(3)), _mm_setzero_si128());
        mism |= dsd_sync_place_chunk(~_mm_movemask_epi8(same) & 0xFFFF, len, k);
    }
    *ham = dsd_sync_popcount64(mism);
    return _mm_movemask_epi8(good) == 0xFFFF;
}
#else
static DSD_SYNC_INLINE int
dsd_sync_pack(const char* buf, const char* pat_a, const char* pat_b, int len, dsd_sync_planes* d, dsd_sync_planes* a,
              dsd_sync_planes* b) {
    return dsd_sync_pack_portable(buf, pat_a, pat_b, len, d, a, b);
}
#endif

#if DSD_SYNC_HAMMING_HAVE_NEON
static DSD_SYNC_INLINE uint8x16_t
dsd_sync_load_chunk_neon(const char* s, int len, int k) {
    const uint8_t* u = (const uint8_t*)(const void*)s;
    if (len < 16) {
        return vcombine_u8(vld1_u8(u), vld1_u8(u + len - 8));
    }
    int off = 16 * k;
    if (off + 16 > len) {
        off = len - 16;
    }
    return vld1q_u8(u + off);
}

/* Lanes of chunk k not already counted (see dsd_sync_place_chunk). */
static DSD_SYNC_INLINE uint8x16_t
dsd_sync_new_lanes_neon(int len, int k) {
    static const uint8_t lane_index[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8x16_t lane = vld1q_u8(lane_index);
    if (len < 16) {
        return vorrq_u8(vcltq_u8(lane, vdupq_n_u8(8)), vcgeq_u8(lane, vdupq_n_u8((uint8_t)(24 - len))));
    }
    const int i = 16 * k;
    if (i + 16 > len) {
        return vcgeq_u8(lane, vdupq_n_u8((uint8_t)(i + 16 - len)));
    }
    return vdupq_n_u8(0xFF);
}

static DSD_SYNC_INLINE uint8x16_t
dsd_sync_good_neon(uint8x16_t v, int pattern) {
    const uint8x16_t t = vandq_u8(v, vdupq_n_u8(0xFC));
    const uint8x16_t ascii = vceqq_u8(t, vdupq_n_u8(0x30));
    return pattern ? ascii : vorrq_u8(ascii, vceqq_u8(t, vdupq_n_u8(0)));
}

static DSD_SYNC_INLINE int
dsd_sync_distance_simd(const char* buf, const char* pat, int len, int* ham) {
    const int n = dsd_sync_chunk_count(len);
    uint8x16_t good = vdupq_n_u8(0xFF);
    uint8x16_t count = vdupq_n_u8(0);
    for (int k = 0; k < n; k++) {
        const uint8x16_t vd = dsd_sync_load_chunk_neon(buf, len, k);
        const uint8x16_t vp = dsd_sync_load_chunk_neon(pat, len, k);
        good = vandq_u8(good, vandq_u8(dsd_sync_good_neon(vd, 0), dsd_sync_good_neon(vp, 1)));
        const uint8x16_t mism = vandq_u8(vtstq_u8(veorq_u8(vd, vp), vdupq_n_u8(3)), dsd_sync_new_lanes_neon(len, k));
        count = vsubq_u8(count, mism); /* a mismatch lane is 0xFF, so this adds one */
    }
    /* At most 64 mismatches, so the byte sum cannot wrap. */
    *ham = (int)vaddvq_u8(count);
    return vminvq_u8(good) == 0xFF;
}
#endif

static DSD_SYNC_INLINE uint64_t
dsd_sync_len_mask(int len) {
    return len >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << len) - 1U);
}

static DSD_SYNC_INLINE int
dsd_sync_min_int(int a, int b) {
    return b < a ? b : a;
}

/* min(best, popcount(a), popcount(b)) for masks of `len` bits. */
static DSD_SYNC_INLINE int
dsd_sync_min_pair(int best, uint64_t a, uint64_t b, int len) {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__aarch64__) || defined(__POPCNT__))
    (void)len;
    return dsd_sync_min_int(best, dsd_sync_min_int(dsd_sync_popcount64(a), dsd_sync_popcount64(b)));
#else
    if (len > 32) {
        return dsd_sync_min_int(best, dsd_sync_min_int(dsd_sync_popcount64(a), dsd_sync_popcount64(b)));
    }
    /* Two 32-bit masks in one word halve the popcount work. */
    uint64_t t = a | (b << 32);
    t = t - ((t >> 1) & UINT64_C(0x5555555555555555));
    t = (t & UINT64_C(0x3333333333333333)) + ((t >> 2) & UINT64_C(0x3333333333333333));
    t = (t + (t >> 4)) & UINT64_C(0x0F0F0F0F0F0F0F0F);
    /* Byte 3 sums bytes 0..3 and byte 7 sums bytes 4..7; each sum is at most 32, so no carries. */
    t *= UINT64_C(0x01010101);
    return dsd_sync_min_int(best, dsd_sync_min_int((int)((t >> 24) & 0xFFU), (int)(t >> 56)));
#endif
}

/* Minimum mismatch count over the five remaps of `d` against both references. */
static DSD_SYNC_INLINE int
dsd_sync_qpsk_planes_min(dsd_sync_planes d, dsd_sync_planes pn, dsd_sync_planes pi, int len) {
    const uint64_t m = dsd_sync_len_mask(len);
    const uint64_t nx1 = d.hi ^ pn.hi;
    const uint64_t nx0 = d.lo ^ pn.lo;
    const uint64_t ny1 = d.lo ^ pn.hi;
    const uint64_t ny0 = d.hi ^ pn.lo;
    const uint64_t ix1 = d.hi ^ pi.hi;
    const uint64_t ix0 = d.lo ^ pi.lo;
    const uint64_t iy1 = d.lo ^ pi.hi;
    const uint64_t iy0 = d.hi ^ pi.lo;
    /* identity, invert (d ^ 2), swap bit order, xor 3, rotate (swap ^ 1) */
    int best = len;
    best = dsd_sync_min_pair(best, (nx1 | nx0) & m, (~nx1 | nx0) & m, len);
    best = dsd_sync_min_pair(best, (ny1 | ny0) & m, (~nx1 | ~nx0) & m, len);
    best = dsd_sync_min_pair(best, (ix1 | ix0) & m, (~ix1 | ix0) & m, len);
    best = dsd_sync_min_pair(best, (iy1 | iy0) & m, (~ix1 | ~ix0) & m, len);
    return dsd_sync_min_pair(best, (ny1 | ~ny0) & m, (iy1 | ~iy0) & m, len);
}

static int
dsd_sync_decode_dibit(char dibit_char) {
    int d = (unsigned char)dibit_char;
    if (d >= '0' && d <= '3') {
        d -= '0';
    }
    return d;
}

static int
dsd_sync_invert_dibit(int d) {
    switch (d) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 0;
        default: return 1;
    }
}

static int
dsd_sync_rotate_dibit(int d) {
    /* 90° rotation (cyclic remap 0->1->3->2->0). */
    switch (d & 0x3) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 0;
        default: return 2; /* d==3 */
    }
}

static void
dsd_sync_fill_remaps(int d, int remaps[DSD_SYNC_REMAP_COUNT]) {
    remaps[0] = d;
    remaps[1] = dsd_sync_invert_dibit(d);
    remaps[2] = ((d & 1) << 1) | ((d & 2) >> 1); /* swap bit order */
    remaps[3] = d ^ 0x3;                         /* bitwise not in 2-bit space */
    remaps[4] = dsd_sync_rotate_dibit(d);
}

static int
dsd_sync_min_hamming(int hamming[DSD_SYNC_REMAP_COUNT][DSD_SYNC_REFERENCE_COUNT]) {
    int best = hamming[0][0];
    for (int remap = 0; remap < DSD_SYNC_REMAP_COUNT; remap++) {
        for (int ref = 0; ref < DSD_SYNC_REFERENCE_COUNT; ref++) {
            if (hamming[remap][ref] < best) {
                best = hamming[remap][ref];
            }
        }
    }
    return best;
}

int
dsd_sync_hamming_distance_scalar(const char* buf, const char* pat, int len) {
    int ham = 0;
    for (int i = 0; i < len; i++) {
        int d = dsd_sync_decode_dibit(buf[i]);
        int expect = (unsigned char)pat[i] - '0';
        if (d != expect) {
            ham++;
        }
    }
    return ham;
}

int
dsd_qpsk_sync_hamming_with_remaps_scalar(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    int hamming[DSD_SYNC_REMAP_COUNT][DSD_SYNC_REFERENCE_COUNT] = {{0}};
    for (int k = 0; k < len; k++) {
        int remaps[DSD_SYNC_REMAP_COUNT];
        const int expected[DSD_SYNC_REFERENCE_COUNT] = {pat_norm[k] - '0', pat_inv[k] - '0'};
        int d = dsd_sync_decode_dibit(buf[k]);

        dsd_sync_fill_remaps(d, remaps);
        for (int remap = 0; remap < DSD_SYNC_REMAP_COUNT; remap++) {
            hamming[remap][0] += (remaps[remap] != expected[0]);
            hamming[remap][1] += (remaps[remap] != expected[1]);
        }
    }
    return dsd_sync_min_hamming(hamming);
}

static DSD_SYNC_INLINE int
dsd_sync_fast_len(int len) {
    return len >= DSD_SYNC_FAST_MIN_LEN && len <= DSD_SYNC_FAST_MAX_LEN;
}

int
dsd_qpsk_sync_hamming_with_remaps_portable(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    dsd_sync_planes d;
    dsd_sync_planes pn;
    dsd_sync_planes pi;
    if (!dsd_sync_fast_len(len) || !dsd_sync_pack_portable(buf, pat_norm, pat_inv, len, &d, &pn, &pi)) {
        return dsd_qpsk_sync_hamming_with_remaps_scalar(buf, pat_norm, pat_inv, len);
    }
    return dsd_sync_qpsk_planes_min(d, pn, pi, len);
}

int
dsd_sync_hamming_distance(const char* buf, const char* pat, int len) {
#if DSD_SYNC_HAMMING_HAVE_SSE2 || DSD_SYNC_HAMMING_HAVE_NEON
    /* Compilers vectorize the per-dibit loop well, so only a vector kernel beats it; elsewhere it stays. */
    int ham = 0;
    if (dsd_sync_fast_len(len) && dsd_sync_distance_simd(buf, pat, len, &ham)) {
        return ham;
    }
#endif
    return dsd_sync_hamming_distance_scalar(buf, pat, len);
}

/* Out of line, so the length check in front of the per-dibit loop runs without the kernel's stack frame. */
static DSD_SYNC_NOINLINE int
dsd_sync_qpsk_fast(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    dsd_sync_planes d;
    dsd_sync_planes pn;
    dsd_sync_planes pi;
    if (!dsd_sync_pack(buf, pat_norm, pat_inv, len, &d, &pn, &pi)) {
        return dsd_qpsk_sync_hamming_with_remaps_scalar(buf, pat_norm, pat_inv, len);
    }
    return dsd_sync_qpsk_planes_min(d, pn, pi, len);
}

int
dsd_qpsk_sync_hamming_with_remaps(const char* buf, const char* pat_norm, const char* pat_inv, int len) {
    if (!dsd_sync_fast_len(len)) {
        return dsd_qpsk_sync_hamming_with_remaps_scalar(buf, pat_norm, pat_inv, len);
    }
    return dsd_sync_qpsk_fast(buf, pat_norm, pat_inv, len);
}
