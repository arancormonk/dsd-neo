// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/*
 * dmr_synth: synthetic DMR repeater (BS) dual-slot test signal for the DMR_SYNTH_* end-to-end tests (issue #729).
 *
 * Writes a 48 kHz mono 16-bit WAV of raised-cosine 4FSK discriminator audio carrying a DMR BS outbound channel:
 * CACH/TACT, voice superframes (BS voice sync, EMB, embedded LC, AMBE+2 2450 from mbelib-neo's encoder), data bursts
 * (voice LC header, PI header, terminator with LC, idle), optional DMRA voice encryption (AES-256 0x25, AES-128 0x24,
 * RC4 0x21) with the per-superframe MI advance and late-entry MI fragments, and fault injection.
 *
 * The MI/IV LFSRs, the late-entry fragment layout and the RC4 keystream are implemented here, independently of the
 * decoder; only the AES block/OFB primitive and the FEC decoders (used to search parity) come from the project.
 *
 * The expectation file lists, per transmitted voice frame, the 49 AMBE parameter bits a correct decoder prints after
 * decryption, formatted like dsd-neo's "AMBE <hex> err" / "FRAME AMBE ... data=" (49 bits << 7, 14 hex digits):
 *   slot superframe vc frame hex14
 * preceded by '#' metadata lines: "# alg HH", "# superframes N", "# voice_slot S", "# fault S SF VC MODE",
 * "# le_corrupt S SF_FIRST SF_LAST MODE", "# lost S SF".
 *
 * Late-entry bit stealing: an encrypted call carries its late-entry MI fragments in the first four bits of each
 * frame's unprotected row 3 (ambe_fr[3][0..3]), which are four of the 49 parameter bits. The generator forces those
 * ciphertext bits to the fragment nibble and records the plaintext that decrypts from that ciphertext (the source
 * plaintext with those four bits replaced), so the exact 49-bit comparison stays meaningful.
 *
 * usage: dsd-neo_test_dmr_synth_gen -o out.wav [options]
 *   -o FILE            output WAV (required)
 *   -e FILE            expectation file (default none)
 *   -t SEC             total duration in seconds (default 20)
 *   --ppm X            transmitter symbol-clock offset in ppm (default 0)
 *   --snr DB           SNR in dB against the outer symbol level (default 40)
 *   --cc N             colour code 0..15 (default 1)
 *   --seed N           noise / filler RNG seed (default 1); output is deterministic for a given seed
 *   --s1 voice|idle    slot 1 content (default voice)
 *   --s2 voice|idle    slot 2 content (default idle)
 *   --alg none|aes256|aes128|rc4   voice encryption for every voice slot (default none)
 *   --key HEX          key: 64 hex (aes256), 32 hex (aes128), 10 hex (rc4); default built-in test keys
 *                      (aes: 0123456789ABCDEF FEDCBA9876543210 0F1E2D3C4B5A6978 8796A5B4C3D2E1F0 truncated to the key
 *                      size; rc4: 0123456789)
 *   --kid N            key ID byte (default 1; slot 2 uses kid+1)
 *   --mi HEX           slot 1 initial 32-bit MI (default 12345678); --mi2 HEX for slot 2 (default 9ABCDEF1)
 *   --tg N --src N     slot 1 talkgroup / source ID (defaults 100 / 1001; slot 2 uses tg+1 / src+1)
 *   --hdr-repeat N     voice LC headers (and PI headers) sent before the voice (default 1 each)
 *   --no-term          omit the terminator with LC
 *   --no-le            do not embed late-entry MI fragments (encrypted calls embed them by default)
 *   --le-corrupt S:SF[-SF2|-end][:golay|wrong]
 *                      corrupt slot S's late-entry fragments in superframes SF..SF2: golay = 4 bit errors in the
 *                      first Golay(24,12) codeword (undecodable); wrong = a valid set carrying MI^1 (CRC recomputed)
 *   --corrupt S:SF:VC[:drop|noise|cc|flip]
 *                      corrupt slot S's superframe SF burst VC (1..6) (repeatable):
 *                      drop  (default) copy the previous burst's dibits at the 8 positions the decoder's
 *                            redundant-carrier check compares and fill the sync/EMB field with random dibits: the BS
 *                            frame loop ends exactly at this burst
 *                      noise random CACH and sync/EMB field
 *                      cc    a valid EMB carrying colour code cc^1
 *                      flip  a 2-bit error in the CACH TACT that Hamming(7,4) miscorrects to the other slot (AT
 *                            kept, LCSS may change); the burst itself is intact
 *   --lose-first N     replace the whole of every voice slot's first N superframes with random dibits (a fade
 *                      right after the headers)
 *   --lead SEC         idle bursts on both slots before the calls (default 0.6)
 *   --tail SEC         idle bursts after the calls (default 0.4)
 *
 * Decoder keys: aes256 -> -H '<16hex> <16hex> <16hex> <16hex>', aes128 -> -H '<16hex> <16hex>', rc4 -> -1 <10hex>.
 */
#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/aes.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <math.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dmr_rs_test_encoder.h"

#define SYNTH_PI         3.14159265358979323846
#define MAX_FAULTS       64
#define SYMBOLS_PER_SLOT 144 /* 12 CACH + 132 burst dibits, 30 ms */

static const uint8_t k_cach_interleave[24] = {0, 7,  8, 9,  1,  10, 11, 12, 2,  13, 14, 15,
                                              3, 16, 4, 17, 18, 19, 5,  20, 21, 22, 6,  23};
static const char k_voice_sync[] = "131111333113313313113313";
static const char k_data_sync[] = "313333111331131131331131";
/* Dibit offsets (from the burst start) the decoder's redundant-carrier check compares between consecutive bursts. */
static const int k_redundant_pos[8] = {16, 27, 1, 32, 3, 33, 13, 7};

static void
die(const char* msg) {
    DSD_FPRINTF(stderr, "dmr_synth: %s\n", msg);
    exit(1);
}

/* ---------------------------------------------------------------- RNG (xorshift64, seeded) */
static uint64_t g_rng = 88172645463325252ULL;

static double
urand(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}

static int
rand_dibit(void) {
    int d = (int)(urand() * 4.0);
    return d > 3 ? 3 : d;
}

static double
grand(void) {
    double u1 = urand() + 1e-12;
    double u2 = urand();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * SYNTH_PI * u2);
}

/* ---------------------------------------------------------------- parity search through the project's decoders
 * A word is a codeword when the decoder reports success and corrects nothing (all of these correct in place). */
typedef int (*dec_fn)(unsigned char* w);

static int
d_h74(unsigned char* w) {
    return Hamming_7_4_decode(w) ? 1 : 0;
}

static int
d_qr(unsigned char* w) {
    return QR_16_7_6_decode(w) ? 1 : 0;
}

static int
d_g208(unsigned char* w) {
    return Golay_20_8_decode(w) ? 1 : 0;
}

static int
d_g2412(unsigned char* w) {
    return Golay_24_12_decode(w) ? 1 : 0;
}

static int
d_h1611(unsigned char* w) {
    unsigned char o[11];
    return Hamming_16_11_4_decode(w, o, 1) ? 1 : 0;
}

static void
fill_parity(unsigned char* b, int k, int n, dec_fn dec, const char* name) {
    int np = n - k;
    for (uint32_t p = 0; p < (1u << np); p++) {
        unsigned char t[32];
        unsigned char c[32];
        DSD_MEMCPY(t, b, (size_t)k);
        for (int i = 0; i < np; i++) {
            t[k + i] = (unsigned char)((p >> (np - 1 - i)) & 1u);
        }
        DSD_MEMCPY(c, t, (size_t)n);
        if (dec(c) && memcmp(c, t, (size_t)n) == 0) {
            DSD_MEMCPY(b, t, (size_t)n);
            return;
        }
    }
    DSD_FPRINTF(stderr, "dmr_synth: parity search failed: %s\n", name);
    exit(1);
}

/* ---------------------------------------------------------------- CRCs (ETSI TS 102 361-1 B.3) */
static uint16_t
crc_ccitt80(const uint8_t* bits) {
    uint16_t crc = 0;
    for (int i = 0; i < 80; i++) {
        int fb = ((crc >> 15) & 1) ^ (bits[i] & 1);
        crc = (uint16_t)(crc << 1);
        if (fb) {
            crc ^= 0x1021;
        }
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

static uint8_t
crc4_le(const uint8_t* bits, int len) { /* x^4+x+1, inverted */
    uint8_t w[64] = {0};
    if (len < 0 || len > 60) {
        die("crc4: bad length");
    }
    DSD_MEMCPY(w, bits, (size_t)len);
    for (int i = 0; i < len; i++) {
        if (w[i]) {
            w[i] ^= 1;
            w[i + 3] ^= 1;
            w[i + 4] ^= 1;
        }
    }
    return (uint8_t)(((w[len] << 3) | (w[len + 1] << 2) | (w[len + 2] << 1) | w[len + 3]) ^ 0xF);
}

/* ---------------------------------------------------------------- embedded LC: BPTC(128,72) */
/* 72-bit LC -> 128 transmitted bits (column-major read of the 8x16 matrix). */
static void
bptc128_encode(const uint8_t* lc, uint8_t* out) {
    uint8_t m[8][16];
    DSD_MEMSET(m, 0, sizeof m);
    unsigned sum = 0;
    for (int i = 0; i < 9; i++) {
        unsigned b = 0;
        for (int j = 0; j < 8; j++) {
            b = (b << 1) | lc[8 * i + j];
        }
        sum += b;
    }
    unsigned crc5 = sum % 31;
    int k = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 11; j++) {
            m[i][j] = lc[k++];
        }
    }
    for (int i = 2; i < 7; i++) {
        for (int j = 0; j < 10; j++) {
            m[i][j] = lc[k++];
        }
        m[i][10] = (uint8_t)((crc5 >> (4 - (i - 2))) & 1u);
    }
    for (int i = 0; i < 7; i++) {
        fill_parity(m[i], 11, 16, d_h1611, "hamming(16,11)");
    }
    for (int j = 0; j < 16; j++) {
        int p = 0;
        for (int i = 0; i < 7; i++) {
            p ^= m[i][j];
        }
        m[7][j] = (uint8_t)p;
    }
    for (int n = 0; n < 128; n++) {
        out[n] = m[n % 8][n / 8];
    }
}

/* ---------------------------------------------------------------- MI / IV: own implementation of the DMRA LFSRs
 * AES: the IV is the first 128 bits of the sequence s[0..31] = MI (MSB first), s[t] = s[t-32]^s[t-22]^s[t-2]^s[t-1]
 * (x^32+x^22+x^2+x+1); the next superframe's MI is s[32..63]. RC4: s[t] = s[t-32]^s[t-4]^s[t-2]; next MI s[32..63]. */
static uint32_t
seq_word(const uint8_t* s, int off) {
    uint32_t w = 0;
    for (int i = 0; i < 32; i++) {
        w = (w << 1) | s[off + i];
    }
    return w;
}

static uint32_t
aes_mi_to_iv(uint32_t mi, uint8_t* iv16) {
    uint8_t s[128];
    for (int i = 0; i < 32; i++) {
        s[i] = (uint8_t)((mi >> (31 - i)) & 1u);
    }
    for (int t = 32; t < 128; t++) {
        s[t] = s[t - 32] ^ s[t - 22] ^ s[t - 2] ^ s[t - 1];
    }
    for (int i = 0; i < 16; i++) {
        uint8_t o = 0;
        for (int j = 0; j < 8; j++) {
            o = (uint8_t)((o << 1) | s[8 * i + j]);
        }
        iv16[i] = o;
    }
    return seq_word(s, 32);
}

static uint32_t
rc4_mi_next(uint32_t mi) {
    uint8_t s[64];
    for (int i = 0; i < 32; i++) {
        s[i] = (uint8_t)((mi >> (31 - i)) & 1u);
    }
    for (int t = 32; t < 64; t++) {
        s[t] = s[t - 32] ^ s[t - 4] ^ s[t - 2];
    }
    return seq_word(s, 32);
}

static void
rc4_stream(const uint8_t* key, int klen, uint8_t* out, int n) {
    uint8_t sbox[256];
    for (int i = 0; i < 256; i++) {
        sbox[i] = (uint8_t)i;
    }
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + sbox[i] + key[i % klen]) & 255;
        uint8_t t = sbox[i];
        sbox[i] = sbox[j];
        sbox[j] = t;
    }
    for (int c = 0, i = 0, j = 0; c < n; c++) {
        i = (i + 1) & 255;
        j = (j + sbox[i]) & 255;
        uint8_t t = sbox[i];
        sbox[i] = sbox[j];
        sbox[j] = t;
        out[c] = sbox[(sbox[i] + sbox[j]) & 255];
    }
}

/* ---------------------------------------------------------------- configuration */
enum { ALG_NONE = 0, ALG_AES256 = 0x25, ALG_AES128 = 0x24, ALG_RC4 = 0x21 };

enum { CM_DROP = 0, CM_NOISE, CM_CC, CM_FLIP };

enum { LEC_GOLAY = 0, LEC_WRONG };

static const char* const k_fault_names[] = {"drop", "noise", "cc", "flip"};
static const char* const k_le_fault_names[] = {"golay", "wrong"};

typedef struct {
    int slot, sf, vc, mode;
} fault_t;

typedef struct {
    int slot, sf_first, sf_last, mode;
} le_fault_t;

typedef struct {
    const char* out;
    const char* expect;
    double secs, ppm, snr, lead, tail;
    int cc;
    int voice[2];
    int alg;
    uint8_t key[32];
    int keylen;
    int kid;
    uint32_t mi[2];
    uint32_t tg, src;
    int hdr_repeat, term, le;
    int lose_first;
    uint64_t seed;
    fault_t faults[MAX_FAULTS];
    int nfaults;
    le_fault_t le_faults[MAX_FAULTS];
    int nle_faults;
} synth_cfg;

static synth_cfg cfg;

/* Indices of the four ambe_d bits that land in ambe_fr[3][0..3] (the late-entry nibble), found at start-up by
 * encoding single-bit words: row 3 is unscrambled, so each of its bits follows exactly one parameter bit. */
static int g_le_bit[4];

static void
find_le_bits(void) {
    char d[49];
    char base[4][24];
    char fr[4][24];
    DSD_MEMSET(d, 0, sizeof d);
    if (mbe_encodeAmbe3600x2450Frame(d, base) < 0) {
        die("ambe frame encode");
    }
    for (int i = 0; i < 4; i++) {
        g_le_bit[i] = -1;
    }
    for (int b = 0; b < 49; b++) {
        d[b] = 1;
        if (mbe_encodeAmbe3600x2450Frame(d, fr) < 0) {
            die("ambe frame encode");
        }
        d[b] = 0;
        int changed = -1;
        int nchanged = 0;
        for (int c = 0; c < 24; c++) {
            if (fr[3][c] != base[3][c]) {
                changed = c;
                nchanged++;
            }
        }
        if (nchanged == 1 && changed < 4) {
            g_le_bit[changed] = b;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (g_le_bit[i] < 0) {
            die("cannot locate the late-entry bits of the AMBE frame");
        }
    }
}

static int
is_le_bit(int b) {
    for (int i = 0; i < 4; i++) {
        if (g_le_bit[i] == b) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- voice source */
typedef struct {
    mbe_ambe2450_encoder* enc;
    mbe_parms cur, prev, prev_e;
    double t;
} voice_src;

static void
voice_params(voice_src* v, char* d49) {
    float pcm[160];
    for (int n = 0; n < 160; n++) {
        double tt = v->t + n / 8000.0;
        double f0 = 120.0 + 40.0 * sin(2.0 * SYNTH_PI * 0.7 * tt);
        double env = 0.5 + 0.5 * sin(2.0 * SYNTH_PI * 3.0 * tt);
        double s = 0.0;
        for (int h = 1; h < 25; h++) {
            double fh = f0 * h;
            if (fh > 3500.0) {
                break;
            }
            double form = exp(-pow((fh - 700.0) / 300.0, 2.0)) + 0.6 * exp(-pow((fh - 1200.0) / 400.0, 2.0))
                          + 0.3 * exp(-pow((fh - 2500.0) / 500.0, 2.0));
            s += form * sin(2.0 * SYNTH_PI * fh * tt + h);
        }
        pcm[n] = (float)(0.25 * env * s);
    }
    v->t += 0.02;
    if (mbe_encodeAmbe2450Parms(v->enc, pcm, d49, &v->cur, &v->prev) < 0) {
        die("ambe encode");
    }
    mbe_moveMbeParms(&v->cur, &v->prev);
}

/* The decoder leaves a frame undecrypted when its received 49 bits are the default silence word or have bits 24..43
 * all zero; the generator never transmits such a ciphertext. */
static int
skip_crypto_frame(const char* d49) {
    static const uint64_t k_silence = 0xF801A99F8CE080ULL;
    int is_sil = 1;
    int zero_tail = 1;
    for (int i = 0; i < 49; i++) {
        if ((d49[i] & 1) != (int)((k_silence >> (55 - i)) & 1u)) {
            is_sil = 0;
        }
    }
    for (int i = 24; i < 44; i++) {
        if (d49[i] & 1) {
            zero_tail = 0;
        }
    }
    return is_sil || zero_tail;
}

/* ---------------------------------------------------------------- burst assembly */
static void
make_tact(int slot, unsigned char* tact7) {
    DSD_MEMSET(tact7, 0, 7);
    tact7[0] = 1; /* AT: channel busy */
    tact7[1] = (unsigned char)slot;
    fill_parity(tact7, 4, 7, d_h74, "hamming(7,4)");
}

static void
put_cach_bits(const unsigned char* tact7, int* out) {
    unsigned char cach[24];
    DSD_MEMSET(cach, 0, sizeof cach);
    DSD_MEMCPY(cach, tact7, 7);
    for (int i = 0; i < 12; i++) {
        out[i] = (cach[k_cach_interleave[(size_t)2 * i]] << 1) | cach[k_cach_interleave[((size_t)2 * i) + 1]];
    }
}

static void
put_cach(int slot, int* out) {
    unsigned char tact[7];
    make_tact(slot, tact);
    put_cach_bits(tact, out);
}

/* TACT for @p slot with the first 2-bit error pattern Hamming(7,4) miscorrects to the other slot with AT kept (the
 * code is perfect, so every 2-bit error decodes silently to a codeword at distance 3). */
static void
put_flipped_cach(int slot, int* out) {
    unsigned char tact[7];
    make_tact(slot, tact);
    for (int a = 0; a < 7; a++) {
        for (int b = a + 1; b < 7; b++) {
            unsigned char w[7];
            DSD_MEMCPY(w, tact, 7);
            w[a] ^= 1;
            w[b] ^= 1;
            unsigned char rx[7];
            DSD_MEMCPY(rx, w, 7);
            if (!Hamming_7_4_decode(rx)) {
                continue;
            }
            if (rx[0] == tact[0] && rx[1] != tact[1]) {
                put_cach_bits(w, out);
                return;
            }
        }
    }
    die("no 2-bit TACT error flips the slot");
}

static void
put_frame_dibits(char fr[4][24], int map_off, int n, int* out) {
    for (int i = 0; i < n; i++) {
        const dsd_ambe_2450_dibit_map_entry* m = &dsd_ambe_2450_dibit_map[map_off + i];
        out[i] = ((fr[m->high_row][m->high_col] & 1) << 1) | (fr[m->low_row][m->low_col] & 1);
    }
}

static void
make_emb(int cc, int lcss, const uint8_t* frag32, int* out24) {
    unsigned char emb[16];
    DSD_MEMSET(emb, 0, sizeof emb);
    for (int k = 0; k < 4; k++) {
        emb[k] = (unsigned char)((cc >> (3 - k)) & 1);
    }
    emb[4] = 0; /* PI: the embedded field carries this channel's LC or null */
    emb[5] = (unsigned char)((lcss >> 1) & 1);
    emb[6] = (unsigned char)(lcss & 1);
    fill_parity(emb, 7, 16, d_qr, "qr(16,7)");
    uint8_t bits[48];
    DSD_MEMSET(bits, 0, sizeof bits);
    for (int i = 0; i < 8; i++) {
        bits[i] = emb[i];
        bits[40 + i] = emb[8 + i];
    }
    for (int i = 0; i < 32; i++) {
        bits[8 + i] = frag32 ? frag32[i] : 0;
    }
    for (int i = 0; i < 24; i++) {
        out24[i] = (bits[(size_t)2 * i] << 1) | bits[((size_t)2 * i) + 1];
    }
}

static void
build_voice_burst(int slot, int vc, int cc, char fr[3][4][24], const uint8_t* frag32, int* out) {
    static const int k_lcss[7] = {0, 0, 1, 3, 3, 2, 0};
    put_cach(slot, out);
    put_frame_dibits(fr[0], 0, 36, out + 12);
    put_frame_dibits(fr[1], 0, 18, out + 48);
    if (vc == 1) {
        for (int i = 0; i < 24; i++) {
            out[66 + i] = k_voice_sync[i] == '1' ? 1 : 3;
        }
    } else {
        make_emb(cc, k_lcss[vc], vc == 6 ? NULL : frag32, out + 66);
    }
    put_frame_dibits(fr[1], 18, 18, out + 90);
    put_frame_dibits(fr[2], 0, 36, out + 108);
}

static void
build_data_burst(int slot, int cc, int dtype, const uint8_t* data96, int* out) {
    static const uint8_t k_reserved[3] = {0, 0, 0};
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(data96, k_reserved, info);
    unsigned char st[20];
    DSD_MEMSET(st, 0, sizeof st);
    for (int k = 0; k < 4; k++) {
        st[k] = (unsigned char)((cc >> (3 - k)) & 1);
        st[4 + k] = (unsigned char)((dtype >> (3 - k)) & 1);
    }
    fill_parity(st, 8, 20, d_g208, "golay(20,8)");
    put_cach(slot, out);
    for (int i = 0; i < 49; i++) {
        out[12 + i] = (info[(size_t)2 * i] << 1) | info[((size_t)2 * i) + 1];
    }
    for (int i = 0; i < 5; i++) {
        out[61 + i] = (st[(size_t)2 * i] << 1) | st[((size_t)2 * i) + 1];
    }
    for (int i = 0; i < 24; i++) {
        out[66 + i] = k_data_sync[i] == '1' ? 1 : 3;
    }
    for (int i = 0; i < 5; i++) {
        out[90 + i] = (st[10 + 2 * i] << 1) | st[10 + 2 * i + 1];
    }
    for (int i = 0; i < 49; i++) {
        out[95 + i] = (info[98 + 2 * i] << 1) | info[98 + 2 * i + 1];
    }
}

static void
bytes_to_bits(const uint8_t* b, int nbytes, uint8_t* bits) {
    for (int i = 0; i < nbytes * 8; i++) {
        bits[i] = (uint8_t)((b[i / 8] >> (7 - i % 8)) & 1u);
    }
}

static void
full_lc_bytes(int slot, uint8_t* lc9) {
    uint32_t tg = cfg.tg + (uint32_t)slot;
    uint32_t src = cfg.src + (uint32_t)slot;
    lc9[0] = 0x00; /* PF 0, R 0, FLCO 0: group voice channel user */
    lc9[1] = 0x00; /* FID 0: standard */
    lc9[2] = cfg.alg != ALG_NONE ? 0x40 : 0x00;
    lc9[3] = (uint8_t)(tg >> 16);
    lc9[4] = (uint8_t)(tg >> 8);
    lc9[5] = (uint8_t)tg;
    lc9[6] = (uint8_t)(src >> 16);
    lc9[7] = (uint8_t)(src >> 8);
    lc9[8] = (uint8_t)src;
}

static void
lc_burst_payload(int slot, uint32_t mask, uint8_t* data96) {
    uint8_t cw[12] = {0};
    full_lc_bytes(slot, cw);
    dmr_test_encode_rs_12_9(cw, cw + 9);
    cw[9] ^= (uint8_t)(mask >> 16);
    cw[10] ^= (uint8_t)(mask >> 8);
    cw[11] ^= (uint8_t)mask;
    bytes_to_bits(cw, 12, data96);
}

static void
pi_burst_payload(int slot, uint8_t* data96) {
    uint8_t b[12];
    DSD_MEMSET(b, 0, sizeof b);
    b[0] = (uint8_t)cfg.alg;
    b[1] = 0x10;
    b[2] = (uint8_t)(cfg.kid + slot);
    b[3] = (uint8_t)(cfg.mi[slot] >> 24);
    b[4] = (uint8_t)(cfg.mi[slot] >> 16);
    b[5] = (uint8_t)(cfg.mi[slot] >> 8);
    b[6] = (uint8_t)cfg.mi[slot];
    bytes_to_bits(b, 12, data96);
    uint16_t crc = (uint16_t)(crc_ccitt80(data96) ^ 0x6969);
    b[10] = (uint8_t)(crc >> 8);
    b[11] = (uint8_t)crc;
    bytes_to_bits(b, 12, data96);
}

static void
idle_payload(uint8_t* data96) {
    static const uint8_t k_idle[12] = {0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91};
    bytes_to_bits(k_idle, 12, data96);
}

/* ---------------------------------------------------------------- per-slot voice call state */
typedef struct {
    voice_src vs;
    uint32_t mi_cur;      /* MI of the current superframe */
    uint8_t ks[18][7];    /* per frame: 49 keystream bits packed MSB first */
    uint8_t lefrag[7][3]; /* [vc][frame] 4-bit late-entry nibble */
    uint8_t emb128[128];
} slot_call;

static uint8_t g_key_aes[32];

static void
superframe_keystream(slot_call* c, int alg) {
    DSD_MEMSET(c->ks, 0, sizeof c->ks);
    if (alg == ALG_AES256 || alg == ALG_AES128) {
        uint8_t iv[16];
        uint8_t ofb[160];
        (void)aes_mi_to_iv(c->mi_cur, iv);
        aes_ofb_keystream_output(iv, g_key_aes, ofb, alg == ALG_AES256 ? DSD_AES_KEY_256 : DSD_AES_KEY_128, 10);
        const uint8_t* st = ofb + 16; /* the first OFB block is discarded */
        for (int k = 0; k < 18; k++) {
            for (int b = 0; b < 49; b++) {
                int pos = 56 * k + b;
                if ((st[pos / 8] >> (7 - pos % 8)) & 1u) {
                    c->ks[k][b / 8] |= (uint8_t)(0x80u >> (b % 8));
                }
            }
        }
    } else if (alg == ALG_RC4) {
        uint8_t key[9];
        uint8_t out[256 + 7 * 18];
        DSD_MEMCPY(key, cfg.key, 5);
        key[5] = (uint8_t)(c->mi_cur >> 24);
        key[6] = (uint8_t)(c->mi_cur >> 16);
        key[7] = (uint8_t)(c->mi_cur >> 8);
        key[8] = (uint8_t)c->mi_cur;
        rc4_stream(key, 9, out, (int)sizeof out);
        for (int k = 0; k < 18; k++) {
            DSD_MEMCPY(c->ks[k], out + 256 + ((size_t)7 * k), 7);
            c->ks[k][6] &= 0x80; /* 49 bits */
        }
    }
}

static int
ks_bit(const slot_call* c, int k, int b) {
    return (c->ks[k][b / 8] >> (7 - b % 8)) & 1;
}

static uint32_t
next_mi(uint32_t mi, int alg) {
    if (alg == ALG_RC4) {
        return rc4_mi_next(mi);
    }
    uint8_t iv[16];
    return aes_mi_to_iv(mi, iv);
}

static int
le_fault_mode(int slot, int sf) {
    int mode = -1;
    for (int i = 0; i < cfg.nle_faults; i++) {
        const le_fault_t* f = &cfg.le_faults[i];
        if (f->slot == slot && sf >= f->sf_first && sf <= f->sf_last) {
            mode = f->mode;
        }
    }
    return mode;
}

/* Late-entry fragments of one superframe: the next superframe's MI plus CRC4, as three Golay(24,12) codewords whose
 * data halves ride VC1..VC3 and parity halves VC4..VC6, nibble n of each half in (frame n/3, vc base + n%3). */
static void
superframe_le(slot_call* c, int slot, int sf) {
    uint32_t mi = next_mi(c->mi_cur, cfg.alg);
    int mode = le_fault_mode(slot, sf);
    if (mode == LEC_WRONG) {
        mi ^= 1u;
    }
    uint8_t bits[36];
    for (int i = 0; i < 32; i++) {
        bits[i] = (uint8_t)((mi >> (31 - i)) & 1u);
    }
    uint8_t crc = crc4_le(bits, 32);
    for (int i = 0; i < 4; i++) {
        bits[32 + i] = (uint8_t)((crc >> (3 - i)) & 1u);
    }
    uint8_t dat[36];
    uint8_t par[36];
    for (int t = 0; t < 3; t++) {
        unsigned char cw[24];
        DSD_MEMCPY(cw, bits + ((size_t)12 * t), 12);
        fill_parity(cw, 12, 24, d_g2412, "golay(24,12)");
        DSD_MEMCPY(dat + ((size_t)12 * t), cw, 12);
        DSD_MEMCPY(par + ((size_t)12 * t), cw + 12, 12);
    }
    for (int n = 0; n < 9; n++) {
        int fr = n / 3;
        int r = n % 3;
        const size_t q = (size_t)4 * n;
        uint8_t dn = (uint8_t)((dat[q] << 3) | (dat[q + 1] << 2) | (dat[q + 2] << 1) | dat[q + 3]);
        uint8_t pn = (uint8_t)((par[q] << 3) | (par[q + 1] << 2) | (par[q + 2] << 1) | par[q + 3]);
        c->lefrag[1 + r][fr] = dn;
        c->lefrag[4 + r][fr] = pn;
    }
    if (mode == LEC_GOLAY) {
        c->lefrag[1][0] ^= 0xF; /* 4 bit errors in codeword 0 */
    }
}

/* One voice frame: plaintext from the voice source, ciphertext with the late-entry nibble forced into its row-3 bits,
 * the AMBE frame, and the plaintext a correct decoder recovers from it (returned in @p expect49). */
static void
make_voice_frame(slot_call* c, int vc, int f, char fr[4][24], char* expect49) {
    const int encrypted = cfg.alg != ALG_NONE;
    const int k = 3 * (vc - 1) + f;
    char plain[49];
    char ct[49];
    char rx[49];
    voice_params(&c->vs, plain);
    for (int tries = 0;; tries++) {
        for (int b = 0; b < 49; b++) {
            ct[b] = (char)((plain[b] & 1) ^ (encrypted ? ks_bit(c, k, b) : 0));
        }
        if (encrypted && cfg.le) {
            uint8_t nib = c->lefrag[vc][f];
            for (int i = 0; i < 4; i++) {
                ct[g_le_bit[i]] = (char)((nib >> (3 - i)) & 1);
            }
        }
        if (!encrypted || !skip_crypto_frame(ct)) {
            break;
        }
        if (tries > 40) {
            die("cannot avoid a crypto-skip frame");
        }
        /* perturb one plaintext bit of 24..43 that is not a late-entry bit, and retry */
        int b = 24 + (tries % 20);
        while (is_le_bit(b)) {
            b = 24 + ((b - 24 + 1) % 20);
        }
        plain[b] ^= 1;
    }
    if (mbe_encodeAmbe3600x2450Frame(ct, fr) < 0) {
        die("ambe frame encode");
    }
    if (encrypted && cfg.le) {
        uint8_t nib = c->lefrag[vc][f];
        for (int i = 0; i < 4; i++) {
            if ((fr[3][i] & 1) != ((nib >> (3 - i)) & 1)) {
                die("late-entry nibble did not reach ambe_fr[3][0..3]");
            }
        }
    }
    mbe_process_result res;
    DSD_MEMSET(&res, 0, sizeof res);
    if (mbe_decodeAmbe3600x2450Frame((const char (*)[24])fr, rx, &res) != 0) {
        die("transmitted AMBE frame does not decode clean");
    }
    for (int b = 0; b < 49; b++) {
        if ((rx[b] & 1) != (ct[b] & 1)) {
            die("transmitted AMBE frame does not decode to its ciphertext");
        }
        expect49[b] = (char)((ct[b] & 1) ^ (encrypted ? ks_bit(c, k, b) : 0));
    }
}

/* ---------------------------------------------------------------- schedule */
enum { B_IDLE = 0, B_VLC, B_PI, B_VOICE, B_TLC };

typedef struct {
    uint8_t type, vc;
    int16_t sf;
} burst_desc;

static int
find_fault(int slot, int sf, int vc) {
    for (int i = 0; i < cfg.nfaults; i++) {
        if (cfg.faults[i].slot == slot && cfg.faults[i].sf == sf && cfg.faults[i].vc == vc) {
            return i;
        }
    }
    return -1;
}

static burst_desc*
build_schedule(int slot, int per_slot, int lead_b, int nsf) {
    burst_desc* sched = calloc((size_t)per_slot, sizeof(burst_desc));
    if (!sched) {
        die("out of memory");
    }
    int k = 0;
    for (int i = 0; i < lead_b && k < per_slot; i++) {
        sched[k++].type = B_IDLE;
    }
    if (cfg.voice[slot]) {
        for (int r = 0; r < cfg.hdr_repeat && k < per_slot; r++) {
            sched[k++].type = B_VLC;
        }
        if (cfg.alg != ALG_NONE) {
            for (int r = 0; r < cfg.hdr_repeat && k < per_slot; r++) {
                sched[k++].type = B_PI;
            }
        }
        for (int sf = 0; sf < nsf; sf++) {
            for (int vc = 1; vc <= 6 && k < per_slot; vc++) {
                sched[k].type = B_VOICE;
                sched[k].sf = (int16_t)sf;
                sched[k].vc = (uint8_t)vc;
                k++;
            }
        }
        if (cfg.term && k < per_slot) {
            sched[k++].type = B_TLC;
        }
    }
    for (; k < per_slot; k++) {
        sched[k].type = B_IDLE;
    }
    return sched;
}

static void
write_expect_header(FILE* ef, int nsf) {
    DSD_FPRINTF(ef, "# dmr_synth expectation v1\n# alg %02X\n# superframes %d\n", cfg.alg, nsf);
    for (int s = 0; s < 2; s++) {
        if (cfg.voice[s]) {
            DSD_FPRINTF(ef, "# voice_slot %d\n", s + 1);
        }
    }
    for (int i = 0; i < cfg.nfaults; i++) {
        const fault_t* f = &cfg.faults[i];
        DSD_FPRINTF(ef, "# fault %d %d %d %s\n", f->slot + 1, f->sf, f->vc, k_fault_names[f->mode]);
    }
    for (int i = 0; i < cfg.nle_faults; i++) {
        const le_fault_t* f = &cfg.le_faults[i];
        DSD_FPRINTF(ef, "# le_corrupt %d %d %d %s\n", f->slot + 1, f->sf_first, f->sf_last, k_le_fault_names[f->mode]);
    }
    for (int s = 0; s < 2; s++) {
        for (int sf = 0; cfg.voice[s] && sf < cfg.lose_first && sf < nsf; sf++) {
            DSD_FPRINTF(ef, "# lost %d %d\n", s + 1, sf);
        }
    }
    DSD_FPRINTF(ef, "# slot superframe vc frame expected_hex14\n");
}

/* Corrupts the voice burst at @p out (CACH first); the previous period's burst sits just before it. */
static void
apply_fault(int mode, int slot, const uint8_t* frag, int* out) {
    switch (mode) {
        case CM_DROP: {
            const int* prev = out - SYMBOLS_PER_SLOT;
            for (int i = 0; i < 8; i++) {
                out[12 + k_redundant_pos[i]] = prev[12 + k_redundant_pos[i]];
            }
            for (int i = 66; i < 90; i++) {
                out[i] = rand_dibit();
            }
            break;
        }
        case CM_NOISE:
            for (int i = 0; i < 12; i++) {
                out[i] = rand_dibit();
            }
            for (int i = 66; i < 90; i++) {
                out[i] = rand_dibit();
            }
            break;
        case CM_CC: make_emb(cfg.cc ^ 1, 1, frag, out + 66); break;
        default: put_flipped_cach(slot, out); break;
    }
}

/* Raised-cosine 4FSK at 48 kHz (alpha 0.2, +/-8 symbol taps) at the configured symbol-clock offset. */
static float*
modulate(const int* sym, size_t nsym, size_t* nsamp_out) {
    static const double k_level[4] = {1.0, 3.0, -1.0, -3.0};
    const double alpha = 0.2;
    const double sps = 48000.0 / (4800.0 * (1.0 + cfg.ppm * 1e-6));
    const size_t nsamp = (size_t)((double)nsym * sps);
    float* x = calloc(nsamp, sizeof(float));
    if (!x) {
        die("out of memory");
    }
    for (size_t s = 0; s < nsym; s++) {
        double centre = (double)s * sps;
        long lo = (long)(centre - 8.0 * sps);
        long hi = (long)(centre + 8.0 * sps);
        for (long n = lo < 0 ? 0 : lo; n <= hi && n < (long)nsamp; n++) {
            double t = ((double)n - centre) / sps;
            double h;
            if (fabs(t) < 1e-9) {
                h = 1.0;
            } else {
                double den = 1.0 - pow(2.0 * alpha * t, 2.0);
                double sn = sin(SYNTH_PI * t) / (SYNTH_PI * t);
                h = fabs(den) < 1e-9 ? (SYNTH_PI / 4.0) * sn : sn * cos(SYNTH_PI * alpha * t) / den;
            }
            x[n] += (float)(k_level[sym[s]] * h);
        }
    }
    *nsamp_out = nsamp;
    return x;
}

/* ---------------------------------------------------------------- argument parsing */
static void
usage(void) {
    DSD_FPRINTF(stderr, "usage: dsd-neo_test_dmr_synth_gen -o out.wav [-e expect.txt] [-t sec] "
                        "[--alg none|aes256|aes128|rc4] ... (see the comment at the top of dmr_synth.c)\n");
    exit(2);
}

static int
parse_hex_key(const char* s, uint8_t* out, int maxbytes) {
    int n = 0;
    int nib = 0;
    uint8_t acc = 0;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9') {
            v = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            v = *s - 'a' + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            v = *s - 'A' + 10;
        } else if (*s == ' ' || *s == ':') {
            continue;
        } else {
            return -1;
        }
        acc = (uint8_t)((acc << 4) | v);
        if (++nib == 2) {
            if (n >= maxbytes) {
                return -1;
            }
            out[n++] = acc;
            nib = 0;
            acc = 0;
        }
    }
    return nib ? -1 : n;
}

/* Splits @p buf in place at ':' into at most @p max fields; returns the field count. */
static int
split_fields(char* buf, char** fields, int max) {
    int n = 0;
    char* p = buf;
    while (n < max) {
        fields[n++] = p;
        char* colon = strchr(p, ':');
        if (!colon) {
            break;
        }
        *colon = '\0';
        p = colon + 1;
    }
    return n;
}

static int
parse_int_arg(const char* s, int lo, int hi) {
    int v = 0;
    if (dsd_parse_int_strict(s, 0, lo, hi, &v) != 0) {
        DSD_FPRINTF(stderr, "dmr_synth: bad integer '%s'\n", s);
        usage();
    }
    return v;
}

static double
parse_double_arg(const char* s, double lo, double hi) {
    double v = 0.0;
    if (dsd_parse_double_strict(s, lo, hi, &v) != 0) {
        DSD_FPRINTF(stderr, "dmr_synth: bad number '%s'\n", s);
        usage();
    }
    return v;
}

static uint32_t
parse_u32_arg(const char* s, int base) {
    uint32_t v = 0;
    if (dsd_parse_uint32_strict(s, base, UINT32_MAX, &v) != 0) {
        DSD_FPRINTF(stderr, "dmr_synth: bad value '%s'\n", s);
        usage();
    }
    return v;
}

static int
lookup_name(const char* s, const char* const* names, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    DSD_FPRINTF(stderr, "dmr_synth: unknown mode '%s'\n", s);
    usage();
    return -1;
}

static void
parse_fault(const char* arg) {
    if (cfg.nfaults >= MAX_FAULTS) {
        die("too many --corrupt");
    }
    char buf[64];
    char* fld[4];
    DSD_SNPRINTF(buf, sizeof buf, "%s", arg);
    int n = split_fields(buf, fld, 4);
    if (n < 3) {
        usage();
    }
    fault_t* f = &cfg.faults[cfg.nfaults++];
    f->slot = parse_int_arg(fld[0], 1, 2) - 1;
    f->sf = parse_int_arg(fld[1], 0, 100000);
    f->vc = parse_int_arg(fld[2], 1, 6);
    f->mode = n > 3 ? lookup_name(fld[3], k_fault_names, 4) : CM_DROP;
}

static void
parse_le_fault(const char* arg) {
    if (cfg.nle_faults >= MAX_FAULTS) {
        die("too many --le-corrupt");
    }
    char buf[64];
    char* fld[3];
    DSD_SNPRINTF(buf, sizeof buf, "%s", arg);
    int n = split_fields(buf, fld, 3);
    if (n < 2) {
        usage();
    }
    le_fault_t* f = &cfg.le_faults[cfg.nle_faults++];
    f->slot = parse_int_arg(fld[0], 1, 2) - 1;
    char* dash = strchr(fld[1], '-');
    if (dash) {
        *dash = '\0';
        f->sf_first = parse_int_arg(fld[1], 0, 100000);
        f->sf_last = strcmp(dash + 1, "end") == 0 ? 100000 : parse_int_arg(dash + 1, f->sf_first, 100000);
    } else {
        f->sf_first = parse_int_arg(fld[1], 0, 100000);
        f->sf_last = f->sf_first;
    }
    f->mode = n > 2 ? lookup_name(fld[2], k_le_fault_names, 2) : LEC_GOLAY;
}

static const char*
next_arg(int argc, char** argv, int* i) {
    if (*i + 1 >= argc) {
        DSD_FPRINTF(stderr, "dmr_synth: %s needs a value\n", argv[*i]);
        usage();
    }
    *i += 1;
    return argv[*i];
}

static void
parse_args(int argc, char** argv, int* have_key) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "-o") == 0) {
            cfg.out = next_arg(argc, argv, &i);
        } else if (strcmp(a, "-e") == 0) {
            cfg.expect = next_arg(argc, argv, &i);
        } else if (strcmp(a, "-t") == 0) {
            cfg.secs = parse_double_arg(next_arg(argc, argv, &i), 1.0, 3600.0);
        } else if (strcmp(a, "--ppm") == 0) {
            cfg.ppm = parse_double_arg(next_arg(argc, argv, &i), -10000.0, 10000.0);
        } else if (strcmp(a, "--snr") == 0) {
            cfg.snr = parse_double_arg(next_arg(argc, argv, &i), -20.0, 200.0);
        } else if (strcmp(a, "--cc") == 0) {
            cfg.cc = parse_int_arg(next_arg(argc, argv, &i), 0, 15);
        } else if (strcmp(a, "--seed") == 0) {
            cfg.seed = (uint64_t)parse_u32_arg(next_arg(argc, argv, &i), 0);
        } else if (strcmp(a, "--s1") == 0 || strcmp(a, "--s2") == 0) {
            const char* v = next_arg(argc, argv, &i);
            int s = a[3] == '1' ? 0 : 1;
            if (strcmp(v, "voice") == 0) {
                cfg.voice[s] = 1;
            } else if (strcmp(v, "idle") == 0) {
                cfg.voice[s] = 0;
            } else {
                usage();
            }
        } else if (strcmp(a, "--alg") == 0) {
            const char* v = next_arg(argc, argv, &i);
            if (strcmp(v, "none") == 0) {
                cfg.alg = ALG_NONE;
            } else if (strcmp(v, "aes256") == 0) {
                cfg.alg = ALG_AES256;
            } else if (strcmp(v, "aes128") == 0) {
                cfg.alg = ALG_AES128;
            } else if (strcmp(v, "rc4") == 0) {
                cfg.alg = ALG_RC4;
            } else {
                usage();
            }
        } else if (strcmp(a, "--key") == 0) {
            cfg.keylen = parse_hex_key(next_arg(argc, argv, &i), cfg.key, 32);
            if (cfg.keylen < 0) {
                die("bad --key");
            }
            *have_key = 1;
        } else if (strcmp(a, "--kid") == 0) {
            cfg.kid = parse_int_arg(next_arg(argc, argv, &i), 0, 254);
        } else if (strcmp(a, "--mi") == 0) {
            cfg.mi[0] = parse_u32_arg(next_arg(argc, argv, &i), 16);
        } else if (strcmp(a, "--mi2") == 0) {
            cfg.mi[1] = parse_u32_arg(next_arg(argc, argv, &i), 16);
        } else if (strcmp(a, "--tg") == 0) {
            cfg.tg = parse_u32_arg(next_arg(argc, argv, &i), 0) & 0xFFFFFFu;
        } else if (strcmp(a, "--src") == 0) {
            cfg.src = parse_u32_arg(next_arg(argc, argv, &i), 0) & 0xFFFFFFu;
        } else if (strcmp(a, "--hdr-repeat") == 0) {
            cfg.hdr_repeat = parse_int_arg(next_arg(argc, argv, &i), 1, 8);
        } else if (strcmp(a, "--no-term") == 0) {
            cfg.term = 0;
        } else if (strcmp(a, "--no-le") == 0) {
            cfg.le = 0;
        } else if (strcmp(a, "--le-corrupt") == 0) {
            parse_le_fault(next_arg(argc, argv, &i));
        } else if (strcmp(a, "--corrupt") == 0) {
            parse_fault(next_arg(argc, argv, &i));
        } else if (strcmp(a, "--lose-first") == 0) {
            cfg.lose_first = parse_int_arg(next_arg(argc, argv, &i), 0, 100000);
        } else if (strcmp(a, "--lead") == 0) {
            cfg.lead = parse_double_arg(next_arg(argc, argv, &i), 0.0, 60.0);
        } else if (strcmp(a, "--tail") == 0) {
            cfg.tail = parse_double_arg(next_arg(argc, argv, &i), 0.0, 60.0);
        } else {
            DSD_FPRINTF(stderr, "dmr_synth: unknown option '%s'\n", a);
            usage();
        }
    }
}

/* ---------------------------------------------------------------- output */
static void
put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
write_wav(const char* path, const float* x, size_t nsamp, double noise_std) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        die("cannot open the output WAV");
    }
    uint32_t datalen = (uint32_t)(nsamp * 2);
    uint8_t h[44];
    DSD_MEMSET(h, 0, sizeof h);
    DSD_MEMCPY(h, "RIFF", 4);
    put_le32(h + 4, 36 + datalen);
    DSD_MEMCPY(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    h[20] = 1; /* PCM */
    h[22] = 1; /* mono */
    put_le32(h + 24, 48000);
    put_le32(h + 28, 96000);
    h[32] = 2;
    h[34] = 16;
    DSD_MEMCPY(h + 36, "data", 4);
    put_le32(h + 40, datalen);
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h;
    uint8_t buf[4096];
    size_t fill = 0;
    for (size_t n = 0; n < nsamp && ok; n++) {
        double y = ((double)x[n] + noise_std * grand()) * 3000.0;
        y = fmax(-32767.0, fmin(32767.0, y));
        int16_t v = (int16_t)lrint(y);
        buf[fill++] = (uint8_t)((uint16_t)v & 0xFFu);
        buf[fill++] = (uint8_t)((uint16_t)v >> 8);
        if (fill == sizeof buf || n + 1 == nsamp) {
            ok = fwrite(buf, 1, fill, f) == fill;
            fill = 0;
        }
    }
    if (fclose(f) != 0 || !ok) {
        die("cannot write the output WAV");
    }
}

int
main(int argc, char** argv) {
    cfg.secs = 20.0;
    cfg.snr = 40.0;
    cfg.cc = 1;
    cfg.voice[0] = 1;
    cfg.kid = 1;
    cfg.mi[0] = 0x12345678u;
    cfg.mi[1] = 0x9ABCDEF1u;
    cfg.tg = 100;
    cfg.src = 1001;
    cfg.hdr_repeat = 1;
    cfg.term = 1;
    cfg.le = 1;
    cfg.lead = 0.6;
    cfg.tail = 0.4;
    cfg.seed = 1;
    int have_key = 0;

    parse_args(argc, argv, &have_key);
    if (!cfg.out) {
        usage();
    }
    g_rng ^= cfg.seed * 0x9E3779B97F4A7C15ULL;
    if (g_rng == 0) {
        g_rng = 88172645463325252ULL;
    }

    if (!have_key) {
        static const uint8_t k_test_key[32] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA,
                                               0x98, 0x76, 0x54, 0x32, 0x10, 0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A,
                                               0x69, 0x78, 0x87, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0};
        DSD_MEMCPY(cfg.key, k_test_key, 32);
        cfg.keylen = cfg.alg == ALG_AES256 ? 32 : cfg.alg == ALG_AES128 ? 16 : 5;
    }
    if ((cfg.alg == ALG_AES256 && cfg.keylen != 32) || (cfg.alg == ALG_AES128 && cfg.keylen != 16)
        || (cfg.alg == ALG_RC4 && cfg.keylen != 5)) {
        die("key length does not match --alg");
    }
    DSD_MEMSET(g_key_aes, 0, sizeof g_key_aes);
    DSD_MEMCPY(g_key_aes, cfg.key, (size_t)cfg.keylen);

    Hamming_7_4_init();
    Hamming_16_11_4_init();
    Golay_20_8_init();
    Golay_24_12_init();
    QR_16_7_6_init();
    find_le_bits();

    /* schedule: 30 ms periods alternate slot 1 / slot 2; each slot has its own burst list */
    const int periods = (int)(cfg.secs / 0.030);
    const int per_slot = periods / 2 + 1;
    const int lead_b = (int)(cfg.lead / 0.060);
    const int tail_b = (int)(cfg.tail / 0.060);
    const int hdr_b = cfg.hdr_repeat * (cfg.alg != ALG_NONE ? 2 : 1);
    const int nsf = (per_slot - lead_b - tail_b - hdr_b - (cfg.term ? 1 : 0)) / 6;
    if (nsf < 1) {
        die("duration too short");
    }
    burst_desc* sched[2];
    for (int s = 0; s < 2; s++) {
        sched[s] = build_schedule(s, per_slot, lead_b, nsf);
    }

    slot_call* calls = calloc(2, sizeof(slot_call));
    if (!calls) {
        die("out of memory");
    }
    for (int s = 0; s < 2; s++) {
        calls[s].vs.enc = mbe_ambe2450EncoderAlloc();
        if (!calls[s].vs.enc) {
            die("cannot allocate the AMBE encoder");
        }
        mbe_initMbeParms(&calls[s].vs.cur, &calls[s].vs.prev, &calls[s].vs.prev_e);
        calls[s].vs.t = s * 0.37;
        calls[s].mi_cur = cfg.mi[s];
        uint8_t lc[9];
        uint8_t lcb[72];
        full_lc_bytes(s, lc);
        bytes_to_bits(lc, 9, lcb);
        bptc128_encode(lcb, calls[s].emb128);
    }

    FILE* ef = NULL;
    if (cfg.expect) {
        ef = dsd_fopen_private(cfg.expect, "w");
        if (!ef) {
            die("cannot open the expectation file");
        }
        write_expect_header(ef, nsf);
    }

    const int lead_sym = 960; /* 0.2 s of random symbols */
    const size_t nsym = (size_t)lead_sym + (size_t)periods * SYMBOLS_PER_SLOT + 960;
    int* sym = calloc(nsym, sizeof(int));
    if (!sym) {
        die("out of memory");
    }
    for (int i = 0; i < lead_sym; i++) {
        sym[i] = rand_dibit();
    }
    size_t kpos = (size_t)lead_sym;
    int idx[2] = {0, 0};
    for (int p = 0; p < periods; p++) {
        const int slot = p & 1;
        burst_desc bd = {B_IDLE, 0, 0};
        if (idx[slot] < per_slot) {
            bd = sched[slot][idx[slot]];
        }
        idx[slot]++;
        int* out = &sym[kpos];
        uint8_t d96[96];
        switch (bd.type) {
            case B_VLC:
                lc_burst_payload(slot, 0x969696u, d96);
                build_data_burst(slot, cfg.cc, 1, d96, out);
                break;
            case B_TLC:
                lc_burst_payload(slot, 0x999999u, d96);
                build_data_burst(slot, cfg.cc, 2, d96, out);
                break;
            case B_PI:
                pi_burst_payload(slot, d96);
                build_data_burst(slot, cfg.cc, 0, d96, out);
                break;
            case B_VOICE: {
                slot_call* c = &calls[slot];
                if (bd.vc == 1) {
                    if (bd.sf > 0) {
                        c->mi_cur = next_mi(c->mi_cur, cfg.alg);
                    }
                    if (cfg.alg != ALG_NONE) {
                        superframe_keystream(c, cfg.alg);
                        superframe_le(c, slot, bd.sf);
                    }
                }
                char fr[3][4][24];
                for (int f = 0; f < 3; f++) {
                    char expect49[49];
                    make_voice_frame(c, bd.vc, f, fr[f], expect49);
                    if (ef) {
                        unsigned long long v = 0;
                        for (int b = 0; b < 49; b++) {
                            v = (v << 1) | (unsigned long long)(expect49[b] & 1);
                        }
                        DSD_FPRINTF(ef, "%d %d %d %d %014llX\n", slot + 1, bd.sf, bd.vc, f, v << 7);
                    }
                }
                const uint8_t* frag = (bd.vc >= 2 && bd.vc <= 5) ? &c->emb128[(size_t)32 * (bd.vc - 2)] : NULL;
                build_voice_burst(slot, bd.vc, cfg.cc, fr, frag, out);
                if (bd.sf < cfg.lose_first) {
                    for (int i = 0; i < SYMBOLS_PER_SLOT; i++) {
                        out[i] = rand_dibit();
                    }
                    break;
                }
                int fi = find_fault(slot, bd.sf, bd.vc);
                if (fi < 0) {
                    break;
                }
                if (cfg.faults[fi].mode == CM_DROP && p == 0) {
                    die("cannot drop the first period");
                }
                apply_fault(cfg.faults[fi].mode, slot, frag, out);
                break;
            }
            default:
                idle_payload(d96);
                build_data_burst(slot, cfg.cc, 9, d96, out);
                break;
        }
        kpos += SYMBOLS_PER_SLOT;
    }
    for (; kpos < nsym; kpos++) {
        sym[kpos] = rand_dibit();
    }
    if (ef && fclose(ef) != 0) {
        die("cannot write the expectation file");
    }

    size_t nsamp = 0;
    float* x = modulate(sym, nsym, &nsamp);
    write_wav(cfg.out, x, nsamp, 3.0 / pow(10.0, cfg.snr / 20.0));
    DSD_FPRINTF(stderr, "dmr_synth: wrote %s: %.2f s, %d superframes per voice slot\n", cfg.out,
                (double)nsamp / 48000.0, nsf);

    for (int s = 0; s < 2; s++) {
        mbe_ambe2450EncoderFree(calls[s].vs.enc);
    }
    free(calls);
    free(x);
    free(sym);
    free(sched[0]);
    free(sched[1]);
    return 0;
}
