// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Makes the WAV I/Q recordings the end-to-end WAV replay cases decode (issue #701), in the build tree, from the
 * committed fixtures; nothing it writes is committed.
 *
 *   dsd-neo_test_iq_wav_fixture_tool <tests/fixtures/iq> <out_dir>
 *
 * - scbs_s24/tetra_scbs_50k_s24.wav: tetra_scbs_real_air_50k.cs16 as 24-bit PCM at 50 kHz, each sample shifted left
 *   by 8 bits (exact), with a meta chunk, an odd-sized LIST chunk and its pad byte, and an auxi chunk giving the centre
 *   (467.913 MHz) and a start time (2026-10-15 12:34:56).
 * - call_48254/tetra.wav: tetra_call_real_air_72k.iq resampled to 48254 Hz by this tool's own double-precision
 *   windowed-sinc filter, as 24-bit PCM at half scale, laid out as the issue's recording (a 1016-byte meta chunk, no
 *   auxi, no frequency in the name), with the issue's hand-written cf32 sidecar beside it as tetra.wav.json.
 * - call_known_centre/tetra_call.wav: the same samples with an auxi chunk giving the carrier's centre, 426.7 MHz.
 * - pow2/tetra_bsch_2304k.wav: tetra_bsch_synth_72k.iq upsampled x32 to 2.304 MS/s (the same filter), as 16-bit PCM
 *   at half scale: a rate that reaches 72 kHz through the half-band chain alone.
 * - u8/tetra_bsch_72k_u8.wav: tetra_bsch_synth_72k.iq's cu8 bytes, unchanged, as 8-bit unsigned PCM at 72 kHz.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    uint8_t* p;
    size_t len;
    size_t cap;
    int failed;
} bytes;

static void
b_put(bytes* b, const void* data, size_t n) {
    if (b->failed) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096U;
        while (cap < b->len + n) {
            cap *= 2U;
        }
        uint8_t* p = (uint8_t*)realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    DSD_MEMCPY(b->p + b->len, data, n);
    b->len += n;
}

static void
b_u16(bytes* b, unsigned v) {
    const uint8_t x[2] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU)};
    b_put(b, x, 2U);
}

static void
b_u32(bytes* b, uint32_t v) {
    const uint8_t x[4] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU), (uint8_t)((v >> 16) & 0xFFU),
                          (uint8_t)((v >> 24) & 0xFFU)};
    b_put(b, x, 4U);
}

static void
b_id(bytes* b, const char* id) {
    b_put(b, id, 4U);
}

static void
b_set_u32(bytes* b, size_t at, uint32_t v) {
    b->p[at] = (uint8_t)(v & 0xFFU);
    b->p[at + 1U] = (uint8_t)((v >> 8) & 0xFFU);
    b->p[at + 2U] = (uint8_t)((v >> 16) & 0xFFU);
    b->p[at + 3U] = (uint8_t)((v >> 24) & 0xFFU);
}

/* A chunk, with its pad byte when its size is odd. */
static void
b_chunk(bytes* b, const char* id, const void* body, size_t n) {
    b_id(b, id);
    b_u32(b, (uint32_t)n);
    b_put(b, body, n);
    if (n & 1U) {
        const uint8_t pad = 0;
        b_put(b, &pad, 1U);
    }
}

static void
b_fmt(bytes* b, unsigned tag, uint32_t rate, unsigned bits) {
    b_id(b, "fmt ");
    b_u32(b, 16U);
    b_u16(b, tag);
    b_u16(b, 2U);
    b_u32(b, rate);
    b_u32(b, rate * 2U * (bits / 8U));
    b_u16(b, 2U * (bits / 8U));
    b_u16(b, bits);
}

/* An auxi chunk in the SDR# layout: start SYSTEMTIME, stop SYSTEMTIME, centre frequency. */
static void
b_auxi(bytes* b, const unsigned st[8], uint32_t center_hz) {
    bytes body;
    DSD_MEMSET(&body, 0, sizeof(body));
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++) {
            b_u16(&body, st[i]);
        }
    }
    b_u32(&body, center_hz);
    for (int i = 0; i < 6; i++) {
        b_u32(&body, 0U);
    }
    if (body.failed) {
        b->failed = 1;
    } else {
        b_chunk(b, "auxi", body.p, body.len);
    }
    free(body.p);
}

/* A meta chunk of @p n bytes of printable text. */
static void
b_meta(bytes* b, size_t n) {
    char* text = (char*)malloc(n);
    if (!text) {
        b->failed = 1;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        text[i] = (char)('a' + (int)(i % 26U));
    }
    b_chunk(b, "meta", text, n);
    free(text);
}

static int
write_file(const char* path, const void* data, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: cannot write %s\n", path);
        return -1;
    }
    const int ok = fwrite(data, 1, len, fp) == len;
    const int closed = fclose(fp) == 0;
    if (!ok || !closed) {
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: writing %s failed\n", path);
        return -1;
    }
    return 0;
}

static int
read_file(const char* path, uint8_t** out, size_t* out_len) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: cannot read %s\n", path);
        return -1;
    }
    bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    uint8_t buf[65536];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0U) {
        b_put(&b, buf, got);
    }
    const int err = ferror(fp);
    (void)fclose(fp);
    if (err || b.failed) {
        free(b.p);
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: reading %s failed\n", path);
        return -1;
    }
    *out = b.p;
    *out_len = b.len;
    return 0;
}

static int
make_dir(const char* parent, const char* leaf, char* out, size_t out_size) {
    DSD_SNPRINTF(out, out_size, "%s/%s", parent, leaf);
    if (dsd_mkdir(out, 0755) != 0 && errno != EEXIST) {
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: cannot make %s\n", out);
        return -1;
    }
    return 0;
}

/* ---- Resampling ------------------------------------------------------------------------------------------------- */

static double
bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; k++) {
        const double h = x / (2.0 * (double)k);
        term *= h * h;
        sum += term;
        if (term < 1e-17 * sum) {
            break;
        }
    }
    return sum;
}

static uint64_t
gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0U) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Interleaved complex @p in at @p in_hz, resampled to @p out_hz (L/M exact) with a Kaiser-windowed sinc (beta 9, 48
 * input samples each side) cut off at 0.45 of the lower rate, in double precision. Each of the L phases' taps is
 * computed once. */
static double*
resample_complex(const double* in, size_t n_in, uint32_t in_hz, uint32_t out_hz, size_t* out_n) {
    enum { HALF = 48, TAPS = 2 * HALF };

    const double beta = 9.0;
    const uint64_t g = gcd_u64(in_hz, out_hz);
    const uint64_t up = out_hz / g;
    const uint64_t down = in_hz / g;
    const double cutoff = 0.45 * (double)(in_hz < out_hz ? in_hz : out_hz) / (double)in_hz; /* cycles per input */
    const double i0_beta = bessel_i0(beta);
    double* bank = (double*)malloc((size_t)up * TAPS * sizeof(double));
    const size_t n_out = (size_t)(((uint64_t)n_in * up) / down);
    double* out = (double*)calloc(n_out * 2U, sizeof(double));
    if (!bank || !out) {
        free(bank);
        free(out);
        return NULL;
    }
    for (uint64_t p = 0; p < up; p++) {
        const double frac = (double)p / (double)up;
        for (int j = 0; j < TAPS; j++) {
            /* Tap j multiplies input base - HALF + 1 + j, at x = frac + HALF - 1 - j from the output instant. */
            const double x = frac + (double)(HALF - 1 - j);
            const double r = x / (double)HALF;
            double h = 0.0;
            if (r > -1.0 && r < 1.0) {
                const double w = bessel_i0(beta * sqrt(1.0 - r * r)) / i0_beta;
                const double arg = 2.0 * cutoff * x;
                const double sinc = fabs(arg) < 1e-12 ? 1.0 : sin(M_PI * arg) / (M_PI * arg);
                h = 2.0 * cutoff * sinc * w;
            }
            bank[p * TAPS + (uint64_t)j] = h;
        }
    }
    for (size_t m = 0; m < n_out; m++) {
        const uint64_t pos = (uint64_t)m * down;
        const long base = (long)(pos / up);
        const double* h = bank + (pos % up) * TAPS;
        double acc_i = 0.0;
        double acc_q = 0.0;
        for (int j = 0; j < TAPS; j++) {
            const long k = base - HALF + 1 + j;
            if (k < 0 || (size_t)k >= n_in) {
                continue;
            }
            acc_i += in[2U * (size_t)k] * h[j];
            acc_q += in[2U * (size_t)k + 1U] * h[j];
        }
        out[2U * m] = acc_i;
        out[2U * m + 1U] = acc_q;
    }
    free(bank);
    *out_n = n_out;
    return out;
}

static double*
cu8_to_complex(const uint8_t* cu8, size_t n_bytes, size_t* out_n) {
    const size_t n = n_bytes / 2U;
    double* out = (double*)malloc(n * 2U * sizeof(double));
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < n * 2U; i++) {
        out[i] = ((double)cu8[i] - 127.5) / 127.5;
    }
    *out_n = n;
    return out;
}

static long
clamp_round(double v, long lo, long hi) {
    const double r = floor(v + 0.5);
    if (r < (double)lo) {
        return lo;
    }
    if (r > (double)hi) {
        return hi;
    }
    return (long)r;
}

static void
b_s24(bytes* b, long v) {
    const uint32_t u = (uint32_t)v;
    const uint8_t x[3] = {(uint8_t)(u & 0xFFU), (uint8_t)((u >> 8) & 0xFFU), (uint8_t)((u >> 16) & 0xFFU)};
    b_put(b, x, 3U);
}

/* RIFF header, then the caller's chunks; wav_finish() writes the RIFF size. */
static void
wav_start(bytes* b) {
    b_id(b, "RIFF");
    b_u32(b, 0U);
    b_id(b, "WAVE");
}

static void
wav_finish(bytes* b) {
    if (!b->failed) {
        b_set_u32(b, 4U, (uint32_t)(b->len - 8U));
    }
}

/* A 24-bit PCM WAV of @p iq (n complex samples at full scale 1.0, written at half scale). */
static int
write_call_wav(const char* path, const double* iq, size_t n, uint32_t rate, const unsigned* auxi_st,
               uint32_t auxi_center_hz) {
    bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wav_start(&b);
    b_fmt(&b, 1U, rate, 24U);
    b_meta(&b, 1016U);
    if (auxi_st) {
        b_auxi(&b, auxi_st, auxi_center_hz);
    }
    b_id(&b, "data");
    b_u32(&b, (uint32_t)(n * 6U));
    const double scale = 0.5 * 8388607.0;
    for (size_t i = 0; i < n * 2U; i++) {
        b_s24(&b, clamp_round(iq[i] * scale, -8388608L, 8388607L));
    }
    wav_finish(&b);
    const int rc = b.failed ? -1 : write_file(path, b.p, b.len);
    free(b.p);
    return rc;
}

/* ---- The recordings --------------------------------------------------------------------------------------------- */

static int
make_scbs_s24(const char* fixtures, const char* out_dir) {
    char src[4096];
    char dir[4096];
    char path[4096];
    DSD_SNPRINTF(src, sizeof(src), "%s/tetra_scbs_real_air_50k.cs16", fixtures);
    if (make_dir(out_dir, "scbs_s24", dir, sizeof(dir)) != 0) {
        return -1;
    }
    DSD_SNPRINTF(path, sizeof(path), "%s/tetra_scbs_50k_s24.wav", dir);
    uint8_t* cs16 = NULL;
    size_t len = 0;
    if (read_file(src, &cs16, &len) != 0) {
        return -1;
    }
    bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wav_start(&b);
    b_fmt(&b, 1U, 50000U, 24U);
    b_meta(&b, 64U);
    static const char kList[] = "INFOISFT\x05\x00\x00\x00sdr++"; /* 17 bytes: an odd chunk, padded */
    b_chunk(&b, "LIST", kList, sizeof(kList) - 1U);
    const unsigned st[8] = {2026U, 10U, 4U, 15U, 12U, 34U, 56U, 0U};
    b_auxi(&b, st, 467913000U);
    const size_t samples = len / 2U;
    b_id(&b, "data");
    b_u32(&b, (uint32_t)(samples * 3U));
    for (size_t i = 0; i < samples; i++) {
        const int16_t v = (int16_t)(uint16_t)((unsigned)cs16[2U * i] | ((unsigned)cs16[2U * i + 1U] << 8));
        b_s24(&b, (long)v * 256L);
    }
    wav_finish(&b);
    free(cs16);
    const int rc = b.failed ? -1 : write_file(path, b.p, b.len);
    free(b.p);
    return rc;
}

/* The issue's hand-written sidecar: it calls the WAV cf32 at the driver stage, which the WAV's header overrides. */
static const char kBogusSidecar[] = "{\n"
                                    "  \"format\": \"dsd-neo-iq\",\n"
                                    "  \"version\": 1,\n"
                                    "  \"sample_format\": \"cf32\",\n"
                                    "  \"iq_order\": \"IQ\",\n"
                                    "  \"endianness\": \"little\",\n"
                                    "  \"capture_stage\": \"post_driver_cf32_pre_ring\",\n"
                                    "  \"sample_rate_hz\": 48254,\n"
                                    "  \"center_frequency_hz\": 426700000,\n"
                                    "  \"capture_center_frequency_hz\": 426700000,\n"
                                    "  \"ppm\": 0,\n"
                                    "  \"tuner_gain_tenth_db\": 0,\n"
                                    "  \"rtl_dsp_bw_khz\": 48,\n"
                                    "  \"base_decimation\": 1,\n"
                                    "  \"post_downsample\": 1,\n"
                                    "  \"demod_rate_hz\": 48254,\n"
                                    "  \"offset_tuning_enabled\": false,\n"
                                    "  \"fs4_shift_enabled\": false,\n"
                                    "  \"combine_rotate_enabled\": true,\n"
                                    "  \"muted_bytes_excluded\": true,\n"
                                    "  \"contains_retunes\": false,\n"
                                    "  \"capture_retune_count\": 0,\n"
                                    "  \"source_backend\": \"sdr\",\n"
                                    "  \"source_args\": \"\",\n"
                                    "  \"capture_started_utc\": \"2026-10-01T00:00:00Z\",\n"
                                    "  \"data_file\": \"tetra.wav\",\n"
                                    "  \"data_bytes\": 0,\n"
                                    "  \"capture_drops\": 0,\n"
                                    "  \"capture_drop_blocks\": 0,\n"
                                    "  \"input_ring_drops\": 0,\n"
                                    "  \"notes\": \"\"\n"
                                    "}\n";

static int
make_call_wavs(const char* fixtures, const char* out_dir) {
    char src[4096];
    char dir[4096];
    char path[4096];
    DSD_SNPRINTF(src, sizeof(src), "%s/tetra_call_real_air_72k.iq", fixtures);
    uint8_t* cu8 = NULL;
    size_t len = 0;
    if (read_file(src, &cu8, &len) != 0) {
        return -1;
    }
    size_t n_in = 0;
    double* iq = cu8_to_complex(cu8, len, &n_in);
    free(cu8);
    if (!iq) {
        return -1;
    }
    size_t n_out = 0;
    double* rs = resample_complex(iq, n_in, 72000U, 48254U, &n_out);
    free(iq);
    if (!rs) {
        return -1;
    }
    int rc = make_dir(out_dir, "call_48254", dir, sizeof(dir));
    if (rc == 0) {
        DSD_SNPRINTF(path, sizeof(path), "%s/tetra.wav", dir);
        rc = write_call_wav(path, rs, n_out, 48254U, NULL, 0U);
    }
    if (rc == 0) {
        DSD_SNPRINTF(path, sizeof(path), "%s/tetra.wav.json", dir);
        rc = write_file(path, kBogusSidecar, sizeof(kBogusSidecar) - 1U);
    }
    if (rc == 0) {
        rc = make_dir(out_dir, "call_known_centre", dir, sizeof(dir));
    }
    if (rc == 0) {
        const unsigned st[8] = {2026U, 10U, 4U, 15U, 9U, 0U, 0U, 0U};
        DSD_SNPRINTF(path, sizeof(path), "%s/tetra_call.wav", dir);
        rc = write_call_wav(path, rs, n_out, 48254U, st, 426700000U);
    }
    free(rs);
    return rc;
}

static int
make_bsch_wavs(const char* fixtures, const char* out_dir) {
    char src[4096];
    char dir[4096];
    char path[4096];
    DSD_SNPRINTF(src, sizeof(src), "%s/tetra_bsch_synth_72k.iq", fixtures);
    uint8_t* cu8 = NULL;
    size_t len = 0;
    if (read_file(src, &cu8, &len) != 0) {
        return -1;
    }
    len &= ~(size_t)1U;

    /* u8: the cu8 bytes as they are. */
    int rc = make_dir(out_dir, "u8", dir, sizeof(dir));
    if (rc == 0) {
        bytes b;
        DSD_MEMSET(&b, 0, sizeof(b));
        wav_start(&b);
        b_fmt(&b, 1U, 72000U, 8U);
        b_id(&b, "data");
        b_u32(&b, (uint32_t)len);
        b_put(&b, cu8, len);
        wav_finish(&b);
        DSD_SNPRINTF(path, sizeof(path), "%s/tetra_bsch_72k_u8.wav", dir);
        rc = b.failed ? -1 : write_file(path, b.p, b.len);
        free(b.p);
    }

    /* x32 to 2.304 MS/s, 16-bit. */
    size_t n_in = 0;
    double* iq = rc == 0 ? cu8_to_complex(cu8, len, &n_in) : NULL;
    free(cu8);
    if (rc != 0 || !iq) {
        free(iq);
        return -1;
    }
    size_t n_out = 0;
    double* up = resample_complex(iq, n_in, 72000U, 2304000U, &n_out);
    free(iq);
    if (!up) {
        return -1;
    }
    rc = make_dir(out_dir, "pow2", dir, sizeof(dir));
    if (rc == 0) {
        bytes b;
        DSD_MEMSET(&b, 0, sizeof(b));
        wav_start(&b);
        b_fmt(&b, 1U, 2304000U, 16U);
        b_id(&b, "data");
        b_u32(&b, (uint32_t)(n_out * 4U));
        for (size_t i = 0; i < n_out * 2U; i++) {
            b_u16(&b, (unsigned)(uint16_t)(int16_t)clamp_round(up[i] * 0.5 * 32767.0, -32768L, 32767L));
        }
        wav_finish(&b);
        DSD_SNPRINTF(path, sizeof(path), "%s/tetra_bsch_2304k.wav", dir);
        rc = b.failed ? -1 : write_file(path, b.p, b.len);
        free(b.p);
    }
    free(up);
    return rc;
}

int
main(int argc, char** argv) {
    if (argc != 3) {
        DSD_FPRINTF(stderr, "usage: %s <tests/fixtures/iq> <out_dir>\n", argc > 0 ? argv[0] : "iq_wav_fixture_tool");
        return 2;
    }
    const char* fixtures = argv[1];
    const char* out_dir = argv[2];
    if (dsd_mkdir(out_dir, 0755) != 0 && errno != EEXIST) {
        DSD_FPRINTF(stderr, "iq_wav_fixture_tool: cannot make %s\n", out_dir);
        return 1;
    }
    if (make_scbs_s24(fixtures, out_dir) != 0 || make_call_wavs(fixtures, out_dir) != 0
        || make_bsch_wavs(fixtures, out_dir) != 0) {
        return 1;
    }
    printf("iq_wav_fixture_tool: wrote the WAV fixtures into %s\n", out_dir);
    return 0;
}
