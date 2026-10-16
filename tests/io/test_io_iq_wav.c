// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * WAV I/Q recordings (issue #701): the parser's container rules, every sample encoding through the replay reader's
 * cf32 conversion, the reader's partial-read contract, seeking, the centre and start time sources, --iq-info, and a
 * sidecar beside a WAV, which the WAV's header overrides however wrong the sidecar is.
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/iq_wav.h>
#include <dsd-neo/platform/file_compat.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "sparse_file.h"
#include "test_support.h"

/* ---- Expectations ---------------------------------------------------------------------------------------------- */

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%" PRIu64 " want=%" PRIu64 "\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* label, const char* got, const char* want) {
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s:\n  got:  %s\n  want: %s\n", label, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_contains(const char* label, const char* text, const char* want) {
    if (!text || !strstr(text, want)) {
        DSD_FPRINTF(stderr, "FAIL: %s: missing \"%s\" in:\n%s\n", label, want, text ? text : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_float(const char* label, size_t index, float got, double want) {
    const double tol = 1e-6 + fabs(want) * 1e-6;
    if (fabs((double)got - want) > tol) {
        DSD_FPRINTF(stderr, "FAIL: %s [%zu]: got=%.9g want=%.9g\n", label, index, (double)got, want);
        return 1;
    }
    return 0;
}

/* @p tmpl with each "@" replaced by @p value. */
static void
fill_template(char* out, size_t out_size, const char* tmpl, const char* value) {
    size_t o = 0;
    const size_t vlen = strlen(value);
    for (const char* t = tmpl; *t && o + 1U < out_size; t++) {
        if (*t == '@') {
            const size_t n = vlen < out_size - 1U - o ? vlen : out_size - 1U - o;
            DSD_MEMCPY(out + o, value, n);
            o += n;
        } else {
            out[o++] = *t;
        }
    }
    out[o] = '\0';
}

/* ---- WAV builder ----------------------------------------------------------------------------------------------- */

typedef struct {
    uint8_t* p;
    size_t len;
    size_t cap;
} wav_bytes;

static void
wb_put(wav_bytes* b, const void* data, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096U;
        while (cap < b->len + n) {
            cap *= 2U;
        }
        uint8_t* np = (uint8_t*)realloc(b->p, cap);
        if (!np) {
            DSD_FPRINTF(stderr, "FAIL: out of memory\n");
            exit(1);
        }
        b->p = np;
        b->cap = cap;
    }
    if (n > 0U) {
        DSD_MEMCPY(b->p + b->len, data, n);
    }
    b->len += n;
}

static void
wb_u8(wav_bytes* b, unsigned v) {
    uint8_t x = (uint8_t)v;
    wb_put(b, &x, 1);
}

static void
wb_u16(wav_bytes* b, unsigned v) {
    wb_u8(b, v & 0xFFU);
    wb_u8(b, (v >> 8) & 0xFFU);
}

static void
wb_u32(wav_bytes* b, uint32_t v) {
    wb_u16(b, v & 0xFFFFU);
    wb_u16(b, (v >> 16) & 0xFFFFU);
}

static void
wb_u64(wav_bytes* b, uint64_t v) {
    wb_u32(b, (uint32_t)(v & 0xFFFFFFFFU));
    wb_u32(b, (uint32_t)(v >> 32));
}

static void
wb_id(wav_bytes* b, const char* id) {
    wb_put(b, id, 4);
}

static void
wb_set_u32(wav_bytes* b, size_t at, uint32_t v) {
    b->p[at] = (uint8_t)(v & 0xFFU);
    b->p[at + 1] = (uint8_t)((v >> 8) & 0xFFU);
    b->p[at + 2] = (uint8_t)((v >> 16) & 0xFFU);
    b->p[at + 3] = (uint8_t)(v >> 24);
}

static void
wb_free(wav_bytes* b) {
    free(b->p);
    DSD_MEMSET(b, 0, sizeof(*b));
}

/* "RIFF" <size, patched by wb_finish_riff()> "WAVE". */
static void
wb_riff_header(wav_bytes* b) {
    wb_id(b, "RIFF");
    wb_u32(b, 0);
    wb_id(b, "WAVE");
}

static void
wb_finish_riff(wav_bytes* b) {
    wb_set_u32(b, 4, (uint32_t)(b->len - 8U));
}

static void
wb_fmt(wav_bytes* b, unsigned tag, unsigned channels, uint32_t rate, unsigned bits) {
    const unsigned block = channels * bits / 8U;
    wb_id(b, "fmt ");
    wb_u32(b, 16);
    wb_u16(b, tag);
    wb_u16(b, channels);
    wb_u32(b, rate);
    wb_u32(b, rate * block);
    wb_u16(b, block);
    wb_u16(b, bits);
}

static const uint8_t kGuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                      0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static void
wb_fmt_ext(wav_bytes* b, unsigned sub, uint32_t rate, unsigned bits, unsigned valid_bits, int bad_guid) {
    const unsigned block = 2U * bits / 8U;
    wb_id(b, "fmt ");
    wb_u32(b, 40);
    wb_u16(b, 0xFFFE);
    wb_u16(b, 2);
    wb_u32(b, rate);
    wb_u32(b, rate * block);
    wb_u16(b, block);
    wb_u16(b, bits);
    wb_u16(b, 22);
    wb_u16(b, valid_bits);
    wb_u32(b, 3); /* front left, front right */
    wb_u16(b, sub);
    uint8_t tail[14];
    DSD_MEMCPY(tail, kGuidTail, sizeof(tail));
    if (bad_guid) {
        tail[13] ^= 0xFFU;
    }
    wb_put(b, tail, sizeof(tail));
}

/* A chunk with @p n body bytes, padded to even size when @p pad. */
static void
wb_chunk(wav_bytes* b, const char* id, const void* body, size_t n, int pad) {
    wb_id(b, id);
    wb_u32(b, (uint32_t)n);
    wb_put(b, body, n);
    if (pad && (n & 1U)) {
        wb_u8(b, 0);
    }
}

static void
wb_data_header(wav_bytes* b, uint32_t size) {
    wb_id(b, "data");
    wb_u32(b, size);
}

/* An auxi chunk in the SDR# layout: start SYSTEMTIME, stop SYSTEMTIME, centre frequency. */
static void
wb_auxi(wav_bytes* b, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second,
        uint32_t center_hz) {
    wav_bytes body;
    DSD_MEMSET(&body, 0, sizeof(body));
    const unsigned st[8] = {year, month, 0, day, hour, minute, second, 0};
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++) {
            wb_u16(&body, st[i]);
        }
    }
    wb_u32(&body, center_hz);
    for (int i = 0; i < 6; i++) {
        wb_u32(&body, 0);
    }
    wb_chunk(b, "auxi", body.p, body.len, 1);
    wb_free(&body);
}

/* ---- Files ------------------------------------------------------------------------------------------------------- */

static char g_dir[DSD_TEST_PATH_MAX];
static char g_made[64][96];
static size_t g_made_count = 0;

/* A path for @p name in the test's directory, recorded for removal. */
static const char*
tpath(const char* name) {
    static char paths[8][DSD_TEST_PATH_MAX];
    static unsigned next = 0;
    char* out = paths[next++ % 8U];
    (void)dsd_test_path_join(out, DSD_TEST_PATH_MAX, g_dir, name);
    for (size_t i = 0; i < g_made_count; i++) {
        if (strcmp(g_made[i], name) == 0) {
            return out;
        }
    }
    if (g_made_count < sizeof(g_made) / sizeof(g_made[0])) {
        DSD_SNPRINTF(g_made[g_made_count], sizeof(g_made[0]), "%s", name);
        g_made_count++;
    }
    return out;
}

static int
write_file(const char* path, const void* data, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        return -1;
    }
    if (len > 0U && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    return fclose(fp);
}

static int
write_wav(const char* name, const wav_bytes* b) {
    return write_file(tpath(name), b->p, b->len);
}

static int
write_text(const char* name, const char* text) {
    return write_file(tpath(name), text, strlen(text));
}

/* Read the whole capture through the replay reader in reads of at most @p max_read bytes. */
static int
read_all(dsd_iq_replay_source* src, uint8_t* out, size_t out_size, size_t max_read, size_t* out_len) {
    size_t total = 0;
    for (;;) {
        size_t want = max_read;
        if (want > out_size - total) {
            want = out_size - total;
        }
        size_t got = 0;
        int rc = dsd_iq_replay_read(src, out + total, want, &got);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        if (got == 0U) {
            break;
        }
        total += got;
        if (total == out_size) {
            uint8_t extra[8];
            rc = dsd_iq_replay_read(src, extra, sizeof(extra), &got);
            if (rc != DSD_IQ_OK || got != 0U) {
                return DSD_IQ_ERR_INVALID_META;
            }
            break;
        }
    }
    *out_len = total;
    return DSD_IQ_OK;
}

static float
f32_at(const uint8_t* bytes, size_t i) {
    float v = 0.0f;
    DSD_MEMCPY(&v, bytes + i * sizeof(float), sizeof(float));
    return v;
}

/* Whether @p bytes hold the bit patterns of the @p count floats at @p want: replay hands samples over unchanged. */
static int
f32_bytes_match(const uint8_t* bytes, const float* want, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint32_t got_bits = 0;
        uint32_t want_bits = 0;
        DSD_MEMCPY(&got_bits, bytes + i * sizeof(float), sizeof(got_bits));
        DSD_MEMCPY(&want_bits, &want[i], sizeof(want_bits));
        if (got_bits != want_bits) {
            return 0;
        }
    }
    return 1;
}

/* Open @p name and read all of it as cf32; *@p n_floats gets the float count. The caller frees the result. */
static float*
replay_floats(const char* name, size_t max_read, size_t* n_floats, dsd_iq_replay_config* cfg_out) {
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    char err[512] = {0};
    if (dsd_iq_replay_open(tpath(name), &cfg, &src, err, sizeof(err)) != DSD_IQ_OK || !src) {
        DSD_FPRINTF(stderr, "FAIL: open %s: %s\n", name, err);
        return NULL;
    }
    const size_t cap = (size_t)cfg.data_bytes;
    uint8_t* bytes = (uint8_t*)malloc(cap ? cap : 1U);
    size_t len = 0;
    int rc = bytes ? read_all(src, bytes, cap, max_read, &len) : DSD_IQ_ERR_ALLOC;
    dsd_iq_replay_close(src);
    if (rc != DSD_IQ_OK || len != cap) {
        DSD_FPRINTF(stderr, "FAIL: read %s: rc=%d len=%zu want=%zu\n", name, rc, len, cap);
        free(bytes);
        dsd_iq_replay_config_clear(&cfg);
        return NULL;
    }
    float* f = (float*)malloc(cap ? cap : 1U);
    for (size_t i = 0; f && i < cap / sizeof(float); i++) {
        f[i] = f32_at(bytes, i);
    }
    free(bytes);
    *n_floats = cap / sizeof(float);
    if (cfg_out) {
        *cfg_out = cfg;
    } else {
        dsd_iq_replay_config_clear(&cfg);
    }
    return f;
}

/* ---- Encodings ------------------------------------------------------------------------------------------------- */

#define ENC_FRAMES 37U

static int32_t
s24_value(size_t i) {
    if (i == 0U) {
        return -8388608;
    }
    if (i == 1U) {
        return 8388607;
    }
    return (int32_t)(i * 200003U % 16000000U) - 8000000;
}

static int32_t
s32_value(size_t i) {
    if (i == 0U) {
        return INT32_MIN;
    }
    if (i == 1U) {
        return INT32_MAX;
    }
    return (int32_t)((int64_t)((uint64_t)i * 50000003U % 4000000000U) - 2000000000);
}

/* Samples of @p bits for sample index i, and the cf32 value each should become. */
static void
put_sample(wav_bytes* b, unsigned tag, unsigned bits, size_t i, double* want) {
    if (tag == 1U && bits == 8U) {
        const unsigned v = (unsigned)((i * 7U) & 0xFFU);
        wb_u8(b, v);
        *want = ((double)v - 127.5) / 127.5;
    } else if (tag == 1U && bits == 16U) {
        const int v = i == 0U ? -32768 : (int)(i * 1031U % 65000U) - 32500;
        wb_u16(b, (unsigned)(uint16_t)(int16_t)v);
        *want = (double)v / 32768.0;
    } else if (tag == 1U && bits == 24U) {
        const int32_t v = s24_value(i);
        const uint32_t u = (uint32_t)v;
        wb_u8(b, u & 0xFFU);
        wb_u8(b, (u >> 8) & 0xFFU);
        wb_u8(b, (u >> 16) & 0xFFU);
        *want = (double)v / 8388608.0;
    } else if (tag == 1U && bits == 32U) {
        const int32_t v = s32_value(i);
        wb_u32(b, (uint32_t)v);
        *want = (double)(float)v / 2147483648.0;
    } else if (tag == 3U && bits == 32U) {
        const float v = ((float)i - 18.0f) * 0.05f;
        uint32_t u = 0;
        DSD_MEMCPY(&u, &v, sizeof(u));
        wb_u32(b, u);
        *want = (double)v;
    } else {
        const double v = ((double)i - 18.0) * 0.0375;
        uint64_t u = 0;
        DSD_MEMCPY(&u, &v, sizeof(u));
        wb_u64(b, u);
        *want = (double)(float)v;
    }
}

static int
check_encoding(const char* name, unsigned tag, unsigned bits, int extensible, dsd_iq_wav_encoding want_enc) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    double want[2U * ENC_FRAMES];
    wb_riff_header(&b);
    if (extensible) {
        wb_fmt_ext(&b, tag, 48000, bits, bits, 0);
    } else {
        wb_fmt(&b, tag, 2, 48000, bits);
    }
    wb_data_header(&b, (uint32_t)(ENC_FRAMES * 2U * bits / 8U));
    for (size_t i = 0; i < (size_t)2U * ENC_FRAMES; i++) {
        put_sample(&b, tag, bits, i, &want[i]);
    }
    wb_finish_riff(&b);
    int rc = expect_int(name, write_wav(name, &b), 0);
    wb_free(&b);

    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("encoding info", dsd_iq_wav_read_info(tpath(name), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("encoding", (int)info.encoding, (int)want_enc);
    rc |= expect_u64("encoding frames", info.frames, ENC_FRAMES);
    rc |= expect_int("encoding block align", info.block_align, (int)(2U * bits / 8U));
    rc |= expect_int("encoding format tag", info.format_tag, extensible ? 0xFFFE : (int)tag);

    size_t n = 0;
    dsd_iq_replay_config cfg;
    float* f = replay_floats(name, 4096U, &n, &cfg);
    if (!f) {
        return 1;
    }
    rc |= expect_u64("encoding floats", n, (uint64_t)2U * ENC_FRAMES);
    rc |= expect_int("encoding replays as cf32", (int)cfg.format, (int)DSD_IQ_FORMAT_CF32);
    rc |= expect_str("encoding stage", cfg.capture_stage, "post_driver_cf32_pre_ring");
    rc |= expect_u64("encoding data bytes", cfg.data_bytes, (uint64_t)ENC_FRAMES * 8U);
    for (size_t i = 0; i < n && i < (size_t)2U * ENC_FRAMES; i++) {
        rc |= expect_float(name, i, f[i], want[i]);
    }
    free(f);
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

static int
test_every_encoding(void) {
    int rc = 0;
    rc |= check_encoding("u8.wav", 1, 8, 0, DSD_IQ_WAV_ENC_U8);
    rc |= check_encoding("s16.wav", 1, 16, 0, DSD_IQ_WAV_ENC_S16);
    rc |= check_encoding("s24.wav", 1, 24, 0, DSD_IQ_WAV_ENC_S24);
    rc |= check_encoding("s32.wav", 1, 32, 0, DSD_IQ_WAV_ENC_S32);
    rc |= check_encoding("f32.wav", 3, 32, 0, DSD_IQ_WAV_ENC_F32);
    rc |= check_encoding("f64.wav", 3, 64, 0, DSD_IQ_WAV_ENC_F64);
    rc |= check_encoding("ext_s24.wav", 1, 24, 1, DSD_IQ_WAV_ENC_S24);
    rc |= check_encoding("ext_f32.wav", 3, 32, 1, DSD_IQ_WAV_ENC_F32);
    rc |= check_encoding("ext_s16.wav", 1, 16, 1, DSD_IQ_WAV_ENC_S16);

    /* u8 is the cu8 widen's (x - 127.5) / 127.5: 127 and 128 sit either side of 0, with no DC line. */
    const uint8_t mid[4] = {127, 128, 0, 255};
    float out[4];
    dsd_iq_wav_frames_to_cf32(DSD_IQ_WAV_ENC_U8, mid, 2, out);
    rc |= expect_float("u8 127", 0, out[0], -0.5 / 127.5);
    rc |= expect_float("u8 128", 1, out[1], 0.5 / 127.5);
    rc |= expect_float("u8 0", 2, out[2], -1.0);
    rc |= expect_float("u8 255", 3, out[3], 1.0);
    return rc;
}

/* f64 is classified before it is narrowed: NaN becomes 0, infinities and anything past 1e6 are clamped. */
static int
test_f64_extremes(void) {
    const double values[8] = {DBL_MAX, -DBL_MAX, INFINITY, -INFINITY, NAN, 1e6, -2e6, 0.25};
    const double want[8] = {1e6, -1e6, 1e6, -1e6, 0.0, 1e6, -1e6, 0.25};
    uint8_t raw[sizeof(values)];
    for (size_t i = 0; i < 8U; i++) {
        uint64_t u = 0;
        DSD_MEMCPY(&u, &values[i], sizeof(u));
        for (int k = 0; k < 8; k++) {
            raw[i * 8U + (size_t)k] = (uint8_t)(u >> (8 * k));
        }
    }
    float out[8];
    dsd_iq_wav_frames_to_cf32(DSD_IQ_WAV_ENC_F64, raw, 4, out);
    int rc = 0;
    for (size_t i = 0; i < 8U; i++) {
        uint32_t bits = 0;
        DSD_MEMCPY(&bits, &out[i], sizeof(bits));
        rc |= expect_true("f64 extreme is finite", (bits & 0x7F800000U) != 0x7F800000U);
        rc |= expect_float("f64 extreme", i, out[i], want[i]);
    }
    return rc;
}

/* ---- Containers -------------------------------------------------------------------------------------------------- */

/* s16 frames whose I is i and Q is -i. */
static void
wb_s16_frames(wav_bytes* b, size_t frames, size_t first) {
    for (size_t i = first; i < first + frames; i++) {
        wb_u16(b, (unsigned)(uint16_t)(int16_t)(int)i);
        wb_u16(b, (unsigned)(uint16_t)(int16_t)-(int)i);
    }
}

static int
check_s16_ramp(const char* label, const float* f, size_t n, size_t first) {
    int rc = 0;
    for (size_t i = 0; i < n / 2U; i++) {
        rc |= expect_float(label, 2U * i, f[2U * i], (double)(i + first) / 32768.0);
        rc |= expect_float(label, 2U * i + 1U, f[2U * i + 1U], -(double)(i + first) / 32768.0);
    }
    return rc;
}

static int
check_rf64(const char* name, const char* magic) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, magic);
    wb_u32(&b, 0xFFFFFFFFU);
    wb_id(&b, "WAVE");
    wb_id(&b, "ds64");
    wb_u32(&b, 28);
    const size_t riff_size_at = b.len;
    wb_u64(&b, 0);  /* riffSize, patched */
    wb_u64(&b, 40); /* dataSize: 10 frames */
    wb_u64(&b, 10); /* sampleCount */
    wb_u32(&b, 0);  /* tableLength */
    wb_fmt(&b, 1, 2, 96000, 16);
    wb_data_header(&b, 0xFFFFFFFFU);
    wb_s16_frames(&b, 10, 0);
    /* A trailing chunk after data stays outside the samples. */
    wb_chunk(&b, "LIST", "INFOjunk", 8, 1);
    const uint64_t riff_size = (uint64_t)b.len - 8U;
    for (int k = 0; k < 8; k++) {
        b.p[riff_size_at + (size_t)k] = (uint8_t)(riff_size >> (8 * k));
    }
    int rc = expect_int(name, write_wav(name, &b), 0);
    wb_free(&b);

    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("rf64 info", dsd_iq_wav_read_info(tpath(name), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("rf64 container", (int)info.container,
                     strcmp(magic, "RF64") == 0 ? DSD_IQ_WAV_CONTAINER_RF64 : DSD_IQ_WAV_CONTAINER_BW64);
    rc |= expect_u64("rf64 declared", info.declared_data_bytes, 40);
    rc |= expect_u64("rf64 frames", info.frames, 10);
    rc |= expect_int("rf64 not truncated", info.truncated, 0);
    size_t n = 0;
    float* f = replay_floats(name, 4096U, &n, NULL);
    if (!f) {
        return 1;
    }
    rc |= expect_u64("rf64 floats", n, 20);
    rc |= check_s16_ramp("rf64 samples", f, n, 0);
    free(f);
    return rc;
}

static int
test_rf64_bw64(void) {
    int rc = check_rf64("rf64.wav", "RF64");
    rc |= check_rf64("bw64.wav", "BW64");
    return rc;
}

/* An unfinalized streaming RIFF: riff_size 0 or 0xFFFFFFFF, data size 0 or 0xFFFFFFFF, data last. */
static int
check_unfinalized(const char* name, uint32_t riff_size, uint32_t data_size) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RIFF");
    wb_u32(&b, riff_size);
    wb_id(&b, "WAVE");
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, data_size);
    wb_s16_frames(&b, 25, 0);
    wb_u8(&b, 0x55); /* a frame cut by the interruption */
    int rc = expect_int(name, write_wav(name, &b), 0);
    wb_free(&b);
    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("unfinalized info", dsd_iq_wav_read_info(tpath(name), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("unfinalized flag", info.unfinalized, 1);
    rc |= expect_u64("unfinalized frames run to EOF", info.frames, 25);
    size_t n = 0;
    float* f = replay_floats(name, 4096U, &n, NULL);
    if (!f) {
        return 1;
    }
    rc |= check_s16_ramp("unfinalized samples", f, n, 0);
    free(f);
    return rc;
}

static int
test_unfinalized_riff(void) {
    int rc = check_unfinalized("unfin_0_0.wav", 0, 0);
    rc |= check_unfinalized("unfin_ff_ff.wav", 0xFFFFFFFFU, 0xFFFFFFFFU);
    rc |= check_unfinalized("unfin_0_ff.wav", 0, 0xFFFFFFFFU);
    rc |= check_unfinalized("unfin_ff_0.wav", 0xFFFFFFFFU, 0);
    return rc;
}

/* The reporter's layout: fmt, a 1016-byte meta chunk, then 24-bit data at 48254 Hz. */
static int
test_reporter_layout(void) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48254, 24);
    uint8_t meta[1016];
    DSD_MEMSET(meta, 'm', sizeof(meta));
    wb_chunk(&b, "meta", meta, sizeof(meta), 1);
    wb_data_header(&b, 6U * 100U);
    for (size_t i = 0; i < 200U; i++) {
        const uint32_t u = (uint32_t)s24_value(i);
        wb_u8(&b, u & 0xFFU);
        wb_u8(&b, (u >> 8) & 0xFFU);
        wb_u8(&b, (u >> 16) & 0xFFU);
    }
    wb_finish_riff(&b);
    int rc = expect_int("reporter write", write_wav("tetra.wav", &b), 0);
    wb_free(&b);
    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("reporter info", dsd_iq_wav_read_info(tpath("tetra.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("reporter data offset", info.data_offset, 1068);
    rc |= expect_u64("reporter rate", info.sample_rate_hz, 48254);
    rc |= expect_u64("reporter frames", info.frames, 100);
    rc |= expect_int("reporter encoding", (int)info.encoding, DSD_IQ_WAV_ENC_S24);
    rc |= expect_int("reporter centre unknown", (int)info.center_source, DSD_IQ_WAV_FROM_NONE);
    rc |= expect_int("reporter start unknown", (int)info.start_source, DSD_IQ_WAV_FROM_NONE);
    return rc;
}

static int
test_pad_bytes(void) {
    int rc = 0;
    char err[512] = {0};
    dsd_iq_wav_info info;

    /* An odd chunk with its pad byte. */
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_chunk(&b, "LIST", "abcde", 5, 1);
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 16);
    wb_s16_frames(&b, 4, 0);
    wb_finish_riff(&b);
    rc |= expect_int("padded write", write_wav("padded.wav", &b), 0);
    wb_free(&b);
    rc |= expect_int("padded info", dsd_iq_wav_read_info(tpath("padded.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("padded data offset", info.data_offset, 12U + 14U + 24U + 8U);

    /* The same chunk with the pad byte left out: the padded position cannot hold a header, the unpadded one can. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_chunk(&b, "LIST", "abcde", 5, 0);
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 16);
    wb_s16_frames(&b, 4, 0);
    wb_finish_riff(&b);
    rc |= expect_int("unpadded write", write_wav("unpadded.wav", &b), 0);
    wb_free(&b);
    rc |= expect_int("unpadded info", dsd_iq_wav_read_info(tpath("unpadded.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("unpadded data offset", info.data_offset, 12U + 13U + 24U + 8U);
    rc |= expect_u64("unpadded frames", info.frames, 4);

    /* Ambiguous: both positions read as a header. The pad byte 'x' makes "xfmt" at the unpadded position, with a size
       of 0x1020 that still fits; the padded position holds the real "fmt ". The padded walk wins. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_chunk(&b, "odd1", "Z", 1, 0);
    wb_u8(&b, 'x');
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 4U * 1100U);
    wb_s16_frames(&b, 1100, 0);
    wb_finish_riff(&b);
    rc |= expect_int("ambiguous write", write_wav("ambiguous.wav", &b), 0);
    wb_free(&b);
    rc |=
        expect_int("ambiguous info", dsd_iq_wav_read_info(tpath("ambiguous.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("ambiguous rate from the padded fmt", info.sample_rate_hz, 48000);
    rc |= expect_u64("ambiguous frames", info.frames, 1100);
    return rc;
}

static int
test_fmt_after_data_and_partial_frame(void) {
    int rc = 0;
    char err[512] = {0};
    dsd_iq_wav_info info;
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_data_header(&b, 4U * 6U + 3U); /* six frames and three bytes of a seventh */
    wb_s16_frames(&b, 6, 10);
    wb_put(&b, "\x01\x02\x03", 3);
    wb_u8(&b, 0); /* pad */
    wb_fmt(&b, 1, 2, 32000, 16);
    wb_finish_riff(&b);
    rc |= expect_int("fmt after data write", write_wav("fmt_last.wav", &b), 0);
    wb_free(&b);
    rc |= expect_int("fmt after data info", dsd_iq_wav_read_info(tpath("fmt_last.wav"), &info, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_u64("fmt after data rate", info.sample_rate_hz, 32000);
    rc |= expect_u64("partial frame dropped", info.frames, 6);
    rc |= expect_u64("partial frame declared", info.declared_data_bytes, 27);
    rc |= expect_u64("partial frame effective", info.effective_data_bytes, 24);
    rc |= expect_int("partial frame not truncated", info.truncated, 0);
    size_t n = 0;
    float* f = replay_floats("fmt_last.wav", 4096U, &n, NULL);
    if (!f) {
        return 1;
    }
    rc |= expect_u64("fmt after data floats", n, 12);
    rc |= check_s16_ramp("fmt after data samples", f, n, 10);
    free(f);
    return rc;
}

static int
expect_refusal(const char* name, const wav_bytes* b, int want_rc, const char* want_msg_fmt) {
    int rc = expect_int(name, write_wav(name, b), 0);
    char want[1024];
    fill_template(want, sizeof(want), want_msg_fmt, tpath(name));
    dsd_iq_wav_info info;
    char err[1024] = {0};
    rc |= expect_int(name, dsd_iq_wav_read_info(tpath(name), &info, err, sizeof(err)), want_rc);
    rc |= expect_str(name, err, want);
    /* The replay open says the same. */
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err2[1024] = {0};
    rc |= expect_int(name, dsd_iq_replay_open(tpath(name), &cfg, NULL, err2, sizeof(err2)), want_rc);
    rc |= expect_str(name, err2, want);
    return rc;
}

static int
test_rejects(void) {
    int rc = 0;
    wav_bytes b;

#define START()                                                                                                        \
    DSD_MEMSET(&b, 0, sizeof(b));                                                                                      \
    wb_riff_header(&b)
#define FINISH_AND_EXPECT(name, want_rc, msg)                                                                          \
    wb_finish_riff(&b);                                                                                                \
    rc |= expect_refusal(name, &b, want_rc, msg);                                                                      \
    wb_free(&b)

    START();
    wb_fmt(&b, 1, 1, 48000, 16);
    wb_data_header(&b, 8);
    wb_s16_frames(&b, 2, 0);
    FINISH_AND_EXPECT("mono.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@' has 1 channel: I/Q replay needs 2 (I, then Q); decode an audio WAV with -i '@'");

    START();
    wb_fmt(&b, 1, 3, 48000, 16);
    wb_data_header(&b, 12);
    wb_s16_frames(&b, 3, 0);
    FINISH_AND_EXPECT("three.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@' has 3 channels: I/Q replay needs exactly 2 (I, then Q)");

    START();
    wb_fmt(&b, 1, 2, 48000, 12);
    wb_data_header(&b, 6);
    wb_put(&b, "\0\0\0\0\0\0", 6);
    FINISH_AND_EXPECT("pcm12.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@': unsupported PCM sample size 12 bits (replay reads 8, 16, 24 and 32)");

    START();
    wb_fmt(&b, 3, 2, 48000, 16);
    wb_data_header(&b, 4);
    wb_put(&b, "\0\0\0\0", 4);
    FINISH_AND_EXPECT("float16.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@': unsupported float sample size 16 bits (replay reads 32 and 64)");

    START();
    wb_fmt(&b, 2, 2, 48000, 4);
    wb_data_header(&b, 4);
    wb_put(&b, "\0\0\0\0", 4);
    FINISH_AND_EXPECT("adpcm.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@': unsupported format tag 0x0002 (replay reads PCM, IEEE float and EXTENSIBLE)");

    START();
    wb_fmt_ext(&b, 1, 48000, 16, 16, 1);
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("badguid.wav", DSD_IQ_ERR_UNSUPPORTED_FMT,
                      "WAV '@': unsupported EXTENSIBLE subformat (replay reads PCM and IEEE float)");

    START();
    wb_fmt_ext(&b, 1, 48000, 16, 20, 0);
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("validbits.wav", DSD_IQ_ERR_INVALID_META, "WAV '@': 20 valid bits in a 16-bit sample");

    START();
    wb_fmt(&b, 1, 2, 48000, 16);
    b.p[b.len - 4] = 6; /* block align 6 for 2 x 16 bits */
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("blockalign.wav", DSD_IQ_ERR_INVALID_META,
                      "WAV '@': block align 6 does not match 2 channels of 16 bits");

    START();
    wb_fmt(&b, 1, 2, 0, 16);
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("rate0.wav", DSD_IQ_ERR_INVALID_META, "WAV '@': the sample rate is 0");

    START();
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("nofmt.wav", DSD_IQ_ERR_INVALID_META, "WAV '@': no fmt chunk");

    START();
    wb_fmt(&b, 1, 2, 48000, 16);
    FINISH_AND_EXPECT("nodata.wav", DSD_IQ_ERR_INVALID_META, "WAV '@': no data chunk");

    START();
    wb_id(&b, "fmt ");
    wb_u32(&b, 12);
    wb_put(&b, "\x01\x00\x02\x00\x80\xbb\x00\x00\x00\x00\x00\x00", 12);
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    FINISH_AND_EXPECT("shortfmt.wav", DSD_IQ_ERR_INVALID_META, "WAV '@': the fmt chunk is too short (12 bytes)");

    /* An empty data chunk, with a chunk after it, in a finalized file. */
    START();
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 0);
    wb_chunk(&b, "LIST", "INFO", 4, 1);
    FINISH_AND_EXPECT("empty.wav", DSD_IQ_ERR_ALIGNMENT, "WAV '@': the data chunk is empty (no I/Q frames)");

    /* An unfinalized header whose size-0 data chunk has another chunk after it is empty too. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RIFF");
    wb_u32(&b, 0);
    wb_id(&b, "WAVE");
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 0);
    wb_chunk(&b, "LIST", "INFO", 4, 1);
    rc |=
        expect_refusal("empty_open.wav", &b, DSD_IQ_ERR_ALIGNMENT, "WAV '@': the data chunk is empty (no I/Q frames)");
    wb_free(&b);

    /* Less than one frame of data. */
    START();
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 3);
    wb_put(&b, "\1\2\3\0", 4);
    FINISH_AND_EXPECT("subframe.wav", DSD_IQ_ERR_ALIGNMENT, "WAV '@': the data chunk holds no whole I/Q frame");

    /* RF64 without ds64 first. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RF64");
    wb_u32(&b, 0xFFFFFFFFU);
    wb_id(&b, "WAVE");
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 4);
    wb_s16_frames(&b, 1, 0);
    rc |= expect_refusal("nods64.wav", &b, DSD_IQ_ERR_INVALID_META,
                         "WAV '@': an RF64/BW64 file must start with a ds64 chunk");
    wb_free(&b);

    /* Big-endian RIFX. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RIFX");
    wb_u32(&b, 4);
    wb_id(&b, "WAVE");
    rc |=
        expect_refusal("rifx.wav", &b, DSD_IQ_ERR_UNSUPPORTED_FMT, "WAV '@': big-endian RIFX files are not supported");
    wb_free(&b);

#undef START
#undef FINISH_AND_EXPECT

    /* A .wav that is not a WAV and has no sidecar. */
    rc |= expect_int("not a wav write", write_text("fake.wav", "this is not a wave file"), 0);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[2048] = {0};
    char want[2048];
    const char* fake = tpath("fake.wav");
    DSD_SNPRINTF(want, sizeof(want),
                 "'%s' is not a WAV file (no RIFF, RF64 or BW64 WAVE header) and has no sidecar (tried '%s.json' and "
                 "'%s.iq.json')",
                 fake, fake, fake);
    rc |= expect_int("not a wav", dsd_iq_replay_read_metadata(fake, &cfg, err, sizeof(err)), DSD_IQ_ERR_INVALID_META);
    rc |= expect_str("not a wav message", err, want);
    return rc;
}

/* ---- Partial reads, truncation, seeking -------------------------------------------------------------------------- */

#define BIG_FRAMES 40000U /* 240000 bytes of s24: larger than any stdio buffer */

static void
wb_s24_ramp(wav_bytes* b, size_t frames) {
    for (size_t i = 0; i < 2U * frames; i++) {
        const uint32_t u = (uint32_t)((int32_t)(i * 37U % 16000000U) - 8000000);
        wb_u8(b, u & 0xFFU);
        wb_u8(b, (u >> 8) & 0xFFU);
        wb_u8(b, (u >> 16) & 0xFFU);
    }
}

static double
s24_ramp_value(size_t i) {
    return (double)((int32_t)(i * 37U % 16000000U) - 8000000) / 8388608.0;
}

static int
test_small_reads_and_seek(void) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48000, 24);
    wb_data_header(&b, 6U * 300U);
    wb_s24_ramp(&b, 300);
    wb_finish_riff(&b);
    int rc = expect_int("small reads write", write_wav("small.wav", &b), 0);
    wb_free(&b);

    /* Any max_bytes works, a read smaller than one cf32 frame included: the stream is the same. */
    size_t n_ref = 0;
    float* ref = replay_floats("small.wav", 65536U, &n_ref, NULL);
    if (!ref) {
        return 1;
    }
    rc |= expect_u64("small reads reference", n_ref, 600);
    for (size_t i = 0; i < n_ref; i++) {
        rc |= expect_float("small reads reference", i, ref[i], s24_ramp_value(i));
    }
    const size_t sizes[] = {1, 3, 5, 7, 8, 13, 4096};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = 0;
        float* f = replay_floats("small.wav", sizes[s], &n, NULL);
        rc |= expect_true("small reads stream", f != NULL && n == n_ref && f32_bytes_match((const uint8_t*)f, ref, n));
        free(f);
    }

    /* Seek and rewind, from the middle of a split frame too. */
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    char err[512] = {0};
    rc |= expect_int("seek open", dsd_iq_replay_open(tpath("small.wav"), &cfg, &src, err, sizeof(err)), DSD_IQ_OK);
    if (!src) {
        free(ref);
        return 1;
    }
    uint8_t buf[64];
    size_t got = 0;
    rc |= expect_int("split read", dsd_iq_replay_read(src, buf, 3, &got), DSD_IQ_OK);
    rc |= expect_u64("split read size", got, 3);
    rc |= expect_int("seek to frame 5", dsd_iq_replay_seek(src, 40), DSD_IQ_OK);
    rc |= expect_int("read at frame 5", dsd_iq_replay_read(src, buf, 16, &got), DSD_IQ_OK);
    rc |= expect_u64("read at frame 5 size", got, 16);
    rc |= expect_true("read at frame 5 bytes", f32_bytes_match(buf, ref + 10, 4));
    rc |= expect_int("unaligned seek rounds down", dsd_iq_replay_seek(src, 8U * 7U + 5U), DSD_IQ_OK);
    rc |= expect_int("read at frame 7", dsd_iq_replay_read(src, buf, 8, &got), DSD_IQ_OK);
    rc |= expect_true("read at frame 7 bytes", got == 8U && f32_bytes_match(buf, ref + 14, 2));
    rc |= expect_int("seek to the end", dsd_iq_replay_seek(src, (uint64_t)8U * 300U), DSD_IQ_OK);
    rc |= expect_int("read at the end", dsd_iq_replay_read(src, buf, 8, &got), DSD_IQ_OK);
    rc |= expect_u64("read at the end size", got, 0);
    rc |= expect_int("seek past the end", dsd_iq_replay_seek(src, (uint64_t)8U * 301U), DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("seek NULL", dsd_iq_replay_seek(NULL, 0), DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("rewind", dsd_iq_replay_rewind(src), DSD_IQ_OK);
    rc |= expect_int("read after rewind", dsd_iq_replay_read(src, buf, 64, &got), DSD_IQ_OK);
    rc |= expect_true("read after rewind bytes", got == 64U && f32_bytes_match(buf, ref, 16));
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    free(ref);
    return rc;
}

/* A sidecar capture seeks too: cu8 rounds down to a whole I/Q pair. */
static int
test_sidecar_seek(void) {
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)i;
    }
    int rc = expect_int("sidecar seek data", write_file(tpath("raw.iq"), payload, sizeof(payload)), 0);
    rc |=
        expect_int("sidecar seek meta",
                   write_text("raw.iq.json",
                              "{\"format\":\"dsd-neo-iq\",\"version\":1,\"sample_format\":\"cu8\",\"iq_order\":\"IQ\","
                              "\"endianness\":\"none\",\"capture_stage\":\"post_mute_pre_widen\","
                              "\"sample_rate_hz\":48000,\"center_frequency_hz\":851000000,"
                              "\"capture_center_frequency_hz\":851000000,\"ppm\":0,\"tuner_gain_tenth_db\":0,"
                              "\"rtl_dsp_bw_khz\":48,\"base_decimation\":1,\"post_downsample\":1,"
                              "\"demod_rate_hz\":48000,\"offset_tuning_enabled\":false,\"fs4_shift_enabled\":false,"
                              "\"combine_rotate_enabled\":true,\"muted_bytes_excluded\":true,"
                              "\"contains_retunes\":false,\"capture_retune_count\":0,\"source_backend\":\"rtl\","
                              "\"source_args\":\"\",\"capture_started_utc\":\"2026-01-01T00:00:00Z\","
                              "\"data_file\":\"raw.iq\",\"data_bytes\":64,\"capture_drops\":0,"
                              "\"capture_drop_blocks\":0,\"input_ring_drops\":0,\"notes\":\"\"}\n"),
                   0);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    char err[512] = {0};
    rc |= expect_int("sidecar seek open", dsd_iq_replay_open(tpath("raw.iq.json"), &cfg, &src, err, sizeof(err)),
                     DSD_IQ_OK);
    if (!src) {
        DSD_FPRINTF(stderr, "  %s\n", err);
        return 1;
    }
    rc |= expect_int("sidecar recorded bandwidth", cfg.recorded_dsp_bw_khz, 48);
    rc |= expect_int("sidecar container", (int)cfg.container, DSD_IQ_REPLAY_CONTAINER_SIDECAR);
    rc |= expect_true("sidecar has no notice", cfg.open_notice[0] == '\0');
    uint8_t buf[8];
    size_t got = 0;
    rc |= expect_int("sidecar seek odd", dsd_iq_replay_seek(src, 11), DSD_IQ_OK);
    rc |= expect_int("sidecar read after seek", dsd_iq_replay_read(src, buf, 4, &got), DSD_IQ_OK);
    rc |= expect_true("sidecar seek lands on a pair", got == 4U && memcmp(buf, payload + 10, 4) == 0);
    rc |= expect_int("sidecar seek past the end", dsd_iq_replay_seek(src, 66), DSD_IQ_ERR_INVALID_ARG);
    rc |= expect_int("sidecar seek to the end", dsd_iq_replay_seek(src, 64), DSD_IQ_OK);
    rc |= expect_int("sidecar read at the end", dsd_iq_replay_read(src, buf, 4, &got), DSD_IQ_OK);
    rc |= expect_u64("sidecar read at the end size", got, 0);
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

/* A header that declares more than the file holds replays what is there; a file cut short under an open source hands
 * over its whole frames, then the next read fails. */
static int
test_truncation(void) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48000, 24);
    wb_data_header(&b, 6U * BIG_FRAMES);
    wb_s24_ramp(&b, BIG_FRAMES);
    wb_finish_riff(&b);
    const size_t header = 12U + 24U + 8U;
    int rc = expect_int("truncation write", write_wav("whole.wav", &b), 0);

    /* Declared but missing: 1000 frames and 2 bytes cut off. */
    rc |= expect_int("truncated write", write_file(tpath("short.wav"), b.p, b.len - (size_t)6U * 1000U - 2U), 0);
    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("truncated info", dsd_iq_wav_read_info(tpath("short.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("truncated flag", info.truncated, 1);
    rc |= expect_u64("truncated frames", info.frames, BIG_FRAMES - 1001U);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    rc |= expect_int("truncated metadata", dsd_iq_replay_read_metadata(tpath("short.wav"), &cfg, err, sizeof(err)),
                     DSD_IQ_OK);
    uint64_t effective = 0;
    int mismatch = 0;
    rc |= expect_int("truncated measure",
                     dsd_iq_replay_measure(&cfg, b.len - (size_t)6U * 1000U - 2U, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("truncated measure bytes", effective, (uint64_t)8U * (BIG_FRAMES - 1001U));
    rc |= expect_int("truncated measure mismatch", mismatch, 1);
    rc |= expect_int("whole measure", dsd_iq_replay_measure(&cfg, b.len, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_int("whole measure no mismatch", mismatch, 0);
    dsd_iq_replay_config_clear(&cfg);

    /* Cut under an open source, 1000 bytes into the second read (2 bytes into a frame). */
    dsd_iq_replay_source* src = NULL;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    rc |= expect_int("cut open", dsd_iq_replay_open(tpath("whole.wav"), &cfg, &src, err, sizeof(err)), DSD_IQ_OK);
    if (!src) {
        wb_free(&b);
        return 1;
    }
    static uint8_t buf[65536];
    size_t got = 0;
    rc |= expect_int("cut first read", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
    rc |= expect_u64("cut first read size", got, sizeof(buf));
    const size_t first_frames = sizeof(buf) / 8U;
    rc |= expect_int("cut", write_file(tpath("whole.wav"), b.p, header + first_frames * 6U + 1000U), 0);
    got = 0;
    rc |= expect_int("cut: the whole frames left", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
    rc |= expect_u64("cut: the whole frames left, size", got, (uint64_t)8U * (1000U / 6U));
    for (size_t i = 0; i < got / sizeof(float); i++) {
        rc |= expect_float("cut: frames", i, f32_at(buf, i), s24_ramp_value(2U * first_frames + i));
    }
    got = 99;
    rc |= expect_int("cut: the next read fails", dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_ERR_IO);
    rc |= expect_u64("cut: the failed read's size", got, 0);
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    wb_free(&b);
    return rc;
}

/* Past 4 GiB: an unfinalized RIFF that streams on past every 32-bit size, read near its end. */
static int
check_large_unfinalized(const char* path, int fd, int* skipped) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RIFF");
    wb_u32(&b, 0xFFFFFFFFU);
    wb_id(&b, "WAVE");
    wb_fmt(&b, 1, 2, 2400000, 16);
    wb_data_header(&b, 0xFFFFFFFFU);
    const uint64_t data_offset = b.len;
    const int64_t size = ((int64_t)5 << 30) + 4103;
    FILE* fp = dsd_test_fdopen(fd, "w+b");
    if (!fp) {
        wb_free(&b);
        (void)dsd_close(fd);
        return 1;
    }
    const int wrote = fwrite(b.p, 1, b.len, fp) == b.len && fflush(fp) == 0;
    wb_free(&b);
    if (!wrote) {
        fclose(fp);
        return 1;
    }
    if (dsd_test_grow_sparse(fd, size) != 0) {
        DSD_FPRINTF(stderr, "SKIP: large unfinalized WAV: cannot make a sparse file here: %s\n", strerror(errno));
        fclose(fp);
        *skipped = 1;
        return 0;
    }
    const uint64_t frames = ((uint64_t)size - data_offset) / 4U;
    /* The last whole frame: I = 1234, Q = -1234. */
    const uint64_t last = data_offset + (frames - 1U) * 4U;
    const uint8_t frame[4] = {0xD2, 0x04, 0x2E, 0xFB};
    if (dsd_fseek64(fp, (int64_t)last, SEEK_SET) != 0 || fwrite(frame, 1, 4, fp) != 4U || fclose(fp) != 0) {
        return 1;
    }

    dsd_iq_wav_info info;
    char err[512] = {0};
    int rc = expect_int("huge info", dsd_iq_wav_read_info(path, &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_int("huge unfinalized", info.unfinalized, 1);
    rc |= expect_u64("huge file size", info.file_size, (uint64_t)size);
    rc |= expect_u64("huge data offset", info.data_offset, data_offset);
    rc |= expect_u64("huge frames", info.frames, frames);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    dsd_iq_replay_source* src = NULL;
    rc |= expect_int("huge open", dsd_iq_replay_open(path, &cfg, &src, err, sizeof(err)), DSD_IQ_OK);
    if (!src) {
        DSD_FPRINTF(stderr, "  %s\n", err);
        return 1;
    }
    rc |= expect_u64("huge data bytes", cfg.data_bytes, frames * 8U);
    rc |= expect_int("huge seek near the end", dsd_iq_replay_seek(src, (frames - 2U) * 8U), DSD_IQ_OK);
    uint8_t out[32];
    size_t got = 0;
    rc |= expect_int("huge read near the end", dsd_iq_replay_read(src, out, sizeof(out), &got), DSD_IQ_OK);
    rc |= expect_u64("huge read size", got, 16);
    rc |= expect_float("huge hole I", 0, f32_at(out, 0), 0.0);
    rc |= expect_float("huge last I", 2, f32_at(out, 2), 1234.0 / 32768.0);
    rc |= expect_float("huge last Q", 3, f32_at(out, 3), -1234.0 / 32768.0);
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

static int
test_large_unfinalized(void) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "dsdneo_iq_wav_huge");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL: mkstemp: %s\n", strerror(errno));
        return 1;
    }
    int skipped = 0;
    int rc = check_large_unfinalized(path, fd, &skipped);
    rc |= expect_int("huge remove", remove(path), 0);
    return rc;
}

/* ---- Centre and start time --------------------------------------------------------------------------------------- */

static int
test_names(void) {
    int rc = 0;
    uint64_t hz = 0;
    char start[DSD_IQ_WAV_START_UTC_SIZE];
    rc |= expect_int("SDR# centre", dsd_iq_wav_center_from_name("/x/SDRSharp_20230105_123456Z_426700000Hz_IQ.wav", &hz),
                     1);
    rc |= expect_u64("SDR# centre value", hz, 426700000U);
    rc |= expect_int(
        "SDR# start",
        dsd_iq_wav_start_from_name("/x/SDRSharp_20230105_123456Z_426700000Hz_IQ.wav", start, sizeof(start)), 1);
    rc |= expect_str("SDR# start value", start, "2023-01-05T12:34:56Z");
    rc |=
        expect_int("SDR++ centre", dsd_iq_wav_center_from_name("baseband_426700000Hz_12-34-56_05-01-2023.wav", &hz), 1);
    rc |= expect_u64("SDR++ centre value", hz, 426700000U);
    rc |=
        expect_int("SDR++ has no UTC start",
                   dsd_iq_wav_start_from_name("baseband_426700000Hz_12-34-56_05-01-2023.wav", start, sizeof(start)), 0);
    rc |= expect_int("SDRuno centre",
                     dsd_iq_wav_center_from_name("C:\\rec\\SDRuno_20200901_181408Z_426700kHz.wav", &hz), 1);
    rc |= expect_u64("SDRuno centre value", hz, 426700000U);
    rc |= expect_int("SDRuno start",
                     dsd_iq_wav_start_from_name("C:\\rec\\SDRuno_20200901_181408Z_426700kHz.wav", start, sizeof(start)),
                     1);
    rc |= expect_str("SDRuno start value", start, "2020-09-01T18:14:08Z");
    rc |= expect_int("MHz with fraction", dsd_iq_wav_center_from_name("cap_426.7125MHz.wav", &hz), 1);
    rc |= expect_u64("MHz with fraction value", hz, 426712500U);
    rc |= expect_int("last token wins", dsd_iq_wav_center_from_name("a_100MHz_b_145.5MHz.WAV", &hz), 1);
    rc |= expect_u64("last token wins value", hz, 145500000U);
    rc |= expect_int("no extension", dsd_iq_wav_center_from_name("a_7.5kHz", &hz), 1);
    rc |= expect_u64("no extension value", hz, 7500U);
    rc |= expect_int("fractional hertz", dsd_iq_wav_center_from_name("a_1.5Hz.wav", &hz), 0);
    rc |= expect_int("above UINT32_MAX", dsd_iq_wav_center_from_name("a_4294967296Hz.wav", &hz), 0);
    rc |= expect_int("zero", dsd_iq_wav_center_from_name("a_0Hz.wav", &hz), 0);
    rc |= expect_int("no unit", dsd_iq_wav_center_from_name("a_426700000.wav", &hz), 0);
    rc |= expect_int("bad unit", dsd_iq_wav_center_from_name("a_426GHz.wav", &hz), 0);
    rc |= expect_int("no name", dsd_iq_wav_center_from_name("tetra.wav", &hz), 0);
    rc |= expect_int("no Z, local time", dsd_iq_wav_start_from_name("x_20230105_123456_1Hz.wav", start, sizeof(start)),
                     0);
    rc |= expect_int("invalid date", dsd_iq_wav_start_from_name("x_20230230_123456Z.wav", start, sizeof(start)), 0);
    return rc;
}

static void
wb_small_s16_with(wav_bytes* b, int auxi, unsigned year, uint32_t center_hz) {
    DSD_MEMSET(b, 0, sizeof(*b));
    wb_riff_header(b);
    wb_fmt(b, 1, 2, 48000, 16);
    if (auxi) {
        wb_auxi(b, year, 3, 14, 15, 9, 26, center_hz);
    }
    wb_data_header(b, 16);
    wb_s16_frames(b, 4, 0);
    wb_finish_riff(b);
}

static int
test_center_and_start_sources(void) {
    int rc = 0;
    wav_bytes b;
    dsd_iq_wav_info info;
    char err[512] = {0};

    /* auxi beats the name. */
    wb_small_s16_with(&b, 1, 2024, 145000000U);
    rc |= expect_int("auxi write", write_wav("SDRSharp_20230105_123456Z_426700000Hz_IQ.wav", &b), 0);
    wb_free(&b);
    rc |=
        expect_int("auxi info",
                   dsd_iq_wav_read_info(tpath("SDRSharp_20230105_123456Z_426700000Hz_IQ.wav"), &info, err, sizeof(err)),
                   DSD_IQ_OK);
    rc |= expect_u64("auxi centre", info.center_frequency_hz, 145000000U);
    rc |= expect_int("auxi centre source", (int)info.center_source, DSD_IQ_WAV_FROM_AUXI);
    rc |= expect_str("auxi start", info.start_utc, "2024-03-14T15:09:26Z");
    rc |= expect_int("auxi start source", (int)info.start_source, DSD_IQ_WAV_FROM_AUXI);

    /* An auxi chunk with no start (year 0) or centre (0) leaves both to the name. */
    wb_small_s16_with(&b, 1, 0, 0);
    rc |= expect_int("empty auxi write", write_wav("SDRuno_20200901_181408Z_426700kHz.wav", &b), 0);
    wb_free(&b);
    rc |= expect_int("empty auxi info",
                     dsd_iq_wav_read_info(tpath("SDRuno_20200901_181408Z_426700kHz.wav"), &info, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_u64("name centre", info.center_frequency_hz, 426700000U);
    rc |= expect_int("name centre source", (int)info.center_source, DSD_IQ_WAV_FROM_FILE_NAME);
    rc |= expect_str("name start", info.start_utc, "2020-09-01T18:14:08Z");
    rc |= expect_int("name start source", (int)info.start_source, DSD_IQ_WAV_FROM_FILE_NAME);

    /* The replay config carries them. */
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    rc |= expect_int(
        "name metadata",
        dsd_iq_replay_read_metadata(tpath("SDRuno_20200901_181408Z_426700kHz.wav"), &cfg, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("config centre", cfg.center_frequency_hz, 426700000U);
    rc |= expect_u64("config capture centre", cfg.capture_center_frequency_hz, 426700000U);
    rc |= expect_str("config start", cfg.capture_started_utc, "2020-09-01T18:14:08Z");
    rc |= expect_str("config backend", cfg.source_backend, "wav");
    rc |= expect_int("config container", (int)cfg.container, DSD_IQ_REPLAY_CONTAINER_WAV);
    rc |= expect_u64("config base", cfg.base_decimation, 1);
    rc |= expect_u64("config post", cfg.post_downsample, 1);
    rc |= expect_u64("config demod", cfg.demod_rate_hz, 48000);
    rc |= expect_u64("config dsp input rate", cfg.dsp_input_rate_hz, 0);
    rc |= expect_int("config bandwidth", cfg.rtl_dsp_bw_khz, 0);
    rc |= expect_int("config recorded bandwidth", cfg.recorded_dsp_bw_khz, 0);
    rc |= expect_true("config no notice", cfg.open_notice[0] == '\0');
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

/* ---- Sidecars beside a WAV --------------------------------------------------------------------------------------- */

static const char* const kBogusSidecar =
    "{\"format\":\"dsd-neo-iq\",\"version\":1,\"sample_format\":\"cf32\",\"iq_order\":\"IQ\",\"endianness\":\"little\","
    "\"capture_stage\":\"post_driver_cf32_pre_ring\",\"sample_rate_hz\":48254,\"center_frequency_hz\":426700000,"
    "\"capture_center_frequency_hz\":426700000,\"ppm\":0,\"tuner_gain_tenth_db\":0,\"rtl_dsp_bw_khz\":48,"
    "\"base_decimation\":1,\"post_downsample\":1,\"demod_rate_hz\":48254,\"offset_tuning_enabled\":false,"
    "\"fs4_shift_enabled\":false,\"combine_rotate_enabled\":true,\"muted_bytes_excluded\":true,"
    "\"contains_retunes\":false,\"capture_retune_count\":0,\"source_backend\":\"rtl\",\"source_args\":\"\","
    "\"capture_started_utc\":\"2026-01-01T00:00:00Z\",\"data_file\":\"@\",\"data_bytes\":0,\"capture_drops\":0,"
    "\"capture_drop_blocks\":0,\"input_ring_drops\":0,\"notes\":\"\"}\n";

/* Semantically invalid sidecars: a rate chain that does not add up, cs16 at the cf32 stage, and events. */
static const char* const kBadChainSidecar =
    "{\"format\":\"dsd-neo-iq\",\"version\":1,\"sample_format\":\"cu8\",\"iq_order\":\"IQ\",\"endianness\":\"none\","
    "\"capture_stage\":\"post_mute_pre_widen\",\"sample_rate_hz\":1536000,\"center_frequency_hz\":1,"
    "\"capture_center_frequency_hz\":1,\"ppm\":0,\"tuner_gain_tenth_db\":0,\"rtl_dsp_bw_khz\":48,"
    "\"base_decimation\":3,\"post_downsample\":1,\"demod_rate_hz\":7,\"offset_tuning_enabled\":false,"
    "\"fs4_shift_enabled\":false,\"combine_rotate_enabled\":true,\"muted_bytes_excluded\":true,"
    "\"contains_retunes\":false,\"capture_retune_count\":0,\"source_backend\":\"rtl\",\"source_args\":\"\","
    "\"capture_started_utc\":\"bogus\",\"data_file\":\"@\",\"data_bytes\":5,\"capture_drops\":0,"
    "\"capture_drop_blocks\":0,\"input_ring_drops\":0,\"notes\":\"\"}\n";

static const char* const kCs16StageSidecar =
    "{\"format\":\"dsd-neo-iq\",\"version\":1,\"sample_format\":\"cs16\",\"iq_order\":\"IQ\",\"endianness\":\"little\","
    "\"capture_stage\":\"post_driver_cf32_pre_ring\",\"sample_rate_hz\":48000,\"center_frequency_hz\":1,"
    "\"capture_center_frequency_hz\":1,\"ppm\":0,\"tuner_gain_tenth_db\":0,\"rtl_dsp_bw_khz\":48,"
    "\"base_decimation\":1,\"post_downsample\":1,\"demod_rate_hz\":48000,\"offset_tuning_enabled\":false,"
    "\"fs4_shift_enabled\":false,\"combine_rotate_enabled\":true,\"muted_bytes_excluded\":true,"
    "\"contains_retunes\":false,\"capture_retune_count\":0,\"source_backend\":\"rtl\",\"source_args\":\"\","
    "\"capture_started_utc\":\"2026-01-01T00:00:00Z\",\"data_file\":\"@\",\"data_bytes\":0,\"capture_drops\":0,"
    "\"capture_drop_blocks\":0,\"input_ring_drops\":0,\"notes\":\"\"}\n";

static const char* const kEventsSidecar =
    "{\"format\":\"dsd-neo-iq\",\"version\":2,\"events\":[{\"kind\":\"retune\",\"byte_offset\":999999999,"
    "\"center_frequency_hz\":5,\"capture_center_frequency_hz\":5,\"sample_rate_hz\":1}],\"data_file\":\"@\","
    "\"sample_format\":\"cu8\"}\n";

static int
check_sidecar_ignored(const char* label, const char* wav_name, const char* sidecar_name, const char* sidecar_fmt) {
    char json[4096];
    fill_template(json, sizeof(json), sidecar_fmt, wav_name);
    int rc = expect_int(label, write_text(sidecar_name, json), 0);
    char want[4096];
    DSD_SNPRINTF(want, sizeof(want),
                 "'%s' is a WAV file: its header gives the format and rate (2-channel PCM 24-bit, 48254 Hz), so the "
                 "sidecar '%s' is ignored",
                 tpath(wav_name), tpath(sidecar_name));
    const char* entries[2] = {tpath(wav_name), tpath(sidecar_name)};
    for (int e = 0; e < 2; e++) {
        char entry[DSD_TEST_PATH_MAX];
        DSD_SNPRINTF(entry, sizeof(entry), "%s", entries[e]);
        dsd_iq_replay_config cfg;
        DSD_MEMSET(&cfg, 0, sizeof(cfg));
        dsd_iq_replay_source* src = NULL;
        char err[1024] = {0};
        const int orc = dsd_iq_replay_open(entry, &cfg, &src, err, sizeof(err));
        rc |= expect_int(label, orc, DSD_IQ_OK);
        if (orc != DSD_IQ_OK) {
            DSD_FPRINTF(stderr, "  entry %s: %s\n", entry, err);
            continue;
        }
        rc |= expect_int(label, (int)cfg.container, DSD_IQ_REPLAY_CONTAINER_WAV);
        rc |= expect_u64(label, cfg.sample_rate_hz, 48254);
        rc |= expect_int(label, (int)cfg.format, DSD_IQ_FORMAT_CF32);
        rc |= expect_u64(label, cfg.event_count, 0);
        rc |= expect_str(label, cfg.open_notice, want);
        rc |= expect_str(label, cfg.data_path, tpath(wav_name));
        rc |= expect_str(label, cfg.metadata_path, entry);
        uint8_t buf[16];
        size_t got = 0;
        rc |= expect_int(label, dsd_iq_replay_read(src, buf, sizeof(buf), &got), DSD_IQ_OK);
        rc |= expect_u64(label, got, sizeof(buf));
        rc |= expect_float(label, 0, f32_at(buf, 0), s24_ramp_value(0));
        dsd_iq_replay_close(src);
        dsd_iq_replay_config_clear(&cfg);
    }
    return rc;
}

static int
test_sidecar_beside_wav(void) {
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48254, 24);
    uint8_t meta[1016];
    DSD_MEMSET(meta, 0, sizeof(meta));
    wb_chunk(&b, "meta", meta, sizeof(meta), 1);
    wb_data_header(&b, 6U * 50U);
    wb_s24_ramp(&b, 50);
    wb_finish_riff(&b);
    int rc = 0;
    const char* const wavs[4] = {"t1.wav", "t2.wav", "t3.wav", "t4.wav"};
    for (int i = 0; i < 4; i++) {
        rc |= expect_int("sidecar wav write", write_wav(wavs[i], &b), 0);
    }
    wb_free(&b);
    rc |= check_sidecar_ignored("bogus sidecar", "t1.wav", "t1.wav.json", kBogusSidecar);
    rc |= check_sidecar_ignored("bad rate chain sidecar", "t2.wav", "t2.wav.json", kBadChainSidecar);
    rc |= check_sidecar_ignored("cs16 at the cf32 stage sidecar", "t3.wav", "t3.wav.iq.json", kCs16StageSidecar);
    rc |= check_sidecar_ignored("events sidecar", "t4.wav", "t4.wav.json", kEventsSidecar);

    /* No sidecar: no notice. */
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[512] = {0};
    rc |= expect_int("lone wav", dsd_iq_replay_read_metadata(tpath("tetra.wav"), &cfg, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_true("lone wav no notice", cfg.open_notice[0] == '\0');
    rc |= expect_str("lone wav metadata path", cfg.metadata_path, tpath("tetra.wav"));
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

/* ---- --iq-info --------------------------------------------------------------------------------------------------- */

/* Read back what was written to @p fp. */
static int
read_stream(FILE* fp, char* out, size_t out_size) {
    out[0] = '\0';
    if (fflush(fp) != 0 || fseek(fp, 0, SEEK_SET) != 0) {
        return -1;
    }
    const size_t n = fread(out, 1, out_size - 1U, fp);
    out[n] = '\0';
    return 0;
}

static char g_info_out[8192];
static char g_info_err[8192];

/* --iq-info for the WAV @p name: its standard output and error text in g_info_out and g_info_err. */
static int
run_iq_info(const char* name, const char* display) {
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[512] = {0};
    g_info_out[0] = '\0';
    g_info_err[0] = '\0';
    int rc = expect_int(name, dsd_iq_replay_read_metadata(tpath(name), &cfg, err, sizeof(err)), DSD_IQ_OK);
    if (rc != 0) {
        return rc;
    }
    dsd_stat_t st;
    rc |= expect_int(name, dsd_stat_path(cfg.data_path, &st), 0);
    FILE* out = tmpfile();
    FILE* errf = tmpfile();
    if (out && errf) {
        rc |= expect_int(name, dsd_iq_info_print(&cfg, display, (uint64_t)st.st_size, out, errf), DSD_IQ_OK);
        rc |= expect_int(name, read_stream(out, g_info_out, sizeof(g_info_out)), 0);
        rc |= expect_int(name, read_stream(errf, g_info_err, sizeof(g_info_err)), 0);
    } else {
        rc |= expect_true("tmpfile", 0);
    }
    if (out) {
        fclose(out);
    }
    if (errf) {
        fclose(errf);
    }
    dsd_iq_replay_config_clear(&cfg);
    return rc;
}

static int
test_iq_info(void) {
    int rc = run_iq_info("t1.wav", "t1.wav");
    const char* text = g_info_out;
    rc |= expect_contains("info", text, "IQ Capture Info: t1.wav\n");
    rc |= expect_contains("info", text, "  Format:              WAV I/Q recording (RIFF container)\n");
    rc |= expect_contains("info", text, "  Sample encoding:     PCM 24-bit, 2 channels: I then Q\n");
    rc |= expect_contains("info", text, "  Sample rate:         48254 Hz\n");
    rc |= expect_contains("info", text, "  Center frequency:    unknown (no auxi chunk, none in the file name)\n");
    rc |= expect_contains("info", text, "  Start time:          unknown (no auxi chunk, none in the file name)\n");
    rc |= expect_contains("info", text, "  Data offset:         1068 bytes\n");
    rc |= expect_contains("info", text, "  Data bytes:          300 (header), 300 (whole frames in the file)\n");
    rc |= expect_contains("info", text, "  Duration:            ~0.00 s (50 frames)\n");
    rc |= expect_contains("info", text,
                          "  Rate chain:          follows the DSP rate the mode runs; at 48 kHz resampled to 48000 Hz, "
                          "decimated by 1, at TETRA's 72 kHz resampled to 72000 Hz, decimated by 1\n");
    rc |= expect_contains("info", text, "  Replay compatible:   yes\n");
    char want[2048];
    DSD_SNPRINTF(want, sizeof(want),
                 "WARNING: '%s' is a WAV file: its header gives the format and rate (2-channel PCM 24-bit, 48254 Hz), "
                 "so the sidecar '%s' is ignored\n",
                 tpath("t1.wav"), tpath("t1.wav.json"));
    rc |= expect_contains("info notice", g_info_err, want);

    /* A rate no plan reaches (under 8 kHz) is not replay compatible: --iq-replay refuses it. */
    wav_bytes slow;
    DSD_MEMSET(&slow, 0, sizeof(slow));
    wb_riff_header(&slow);
    wb_fmt(&slow, 1, 2, 4000, 24);
    wb_data_header(&slow, 6U * 50U);
    wb_s24_ramp(&slow, 50U);
    wb_finish_riff(&slow);
    rc |= expect_int("slow write", write_wav("slow.wav", &slow), 0);
    wb_free(&slow);
    rc |= run_iq_info("slow.wav", NULL);
    rc |= expect_contains("info slow", g_info_out,
                          "  Rate chain:          follows the DSP rate the mode runs; at 48 kHz refused");
    rc |= expect_contains("info slow", g_info_out, "  Replay compatible:   no\n");

    /* A truncated recording with part of a frame at its end. */
    rc |= run_iq_info("short.wav", NULL);
    rc |= expect_contains("info truncated", g_info_err,
                          "WARNING: the WAV header declares 240000 data bytes but the file holds 233998 (a truncated "
                          "recording)\n");
    rc |= expect_contains("info truncated", g_info_err,
                          "WARNING: the data ends inside a frame; its last 4 byte(s) are not replayed\n");

    /* An unfinalized one. */
    rc |= run_iq_info("unfin_0_0.wav", NULL);
    rc |= expect_contains("info unfinalized", g_info_out,
                          "  Data bytes:          unfinalized header, 100 (whole frames to the end of the file)\n");
    rc |=
        expect_contains("info unfinalized", g_info_err,
                        "WARNING: the WAV header was never finalized (an interrupted recording); the data runs to the "
                        "end of the file\n");

    /* A centre and start time from the file name. */
    rc |= run_iq_info("SDRuno_20200901_181408Z_426700kHz.wav", NULL);
    rc |= expect_contains("info sourced", g_info_out, "  Center frequency:    426.700000 MHz (file name)\n");
    rc |= expect_contains("info sourced", g_info_out, "  Start time:          2020-09-01T18:14:08Z (file name)\n");
    return rc;
}

/* ---- Container end ----------------------------------------------------------------------------------------------- */

/* A data chunk that declares more than its container holds: the data stops at the container's end (a writer
 * inconsistency, which --iq-info warns about), not at the end of the file. */
static int
test_container_end(void) {
    /* RIFF ends at byte 48; data starts at 44 and declares 4096 bytes, all of which the file holds. */
    wav_bytes b;
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 4096U);
    wb_s16_frames(&b, 1024U, 0);
    wb_set_u32(&b, 4, 40U);
    int rc = expect_int("riff end write", write_wav("riff_end.wav", &b), 0);
    wb_free(&b);
    dsd_iq_wav_info info;
    char err[512] = {0};
    rc |= expect_int("riff end info", dsd_iq_wav_read_info(tpath("riff_end.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("riff end data offset", info.data_offset, 44U);
    rc |= expect_u64("riff end container end", info.container_end, 48U);
    rc |= expect_u64("riff end frames", info.frames, 1U);
    rc |= expect_int("riff end clamped", info.container_clamped, 1);
    rc |= expect_int("riff end not truncated", info.truncated, 0);
    size_t n = 0;
    dsd_iq_replay_config cfg;
    float* f = replay_floats("riff_end.wav", 4096U, &n, &cfg);
    if (!f) {
        return 1;
    }
    rc |= expect_u64("riff end replayed floats", n, 2U);
    rc |= check_s16_ramp("riff end samples", f, n, 0);
    free(f);
    uint64_t effective = 0;
    int mismatch = 1;
    rc |= expect_int("riff end measure", dsd_iq_replay_measure(&cfg, 44U + 4096U, &effective, &mismatch), DSD_IQ_OK);
    rc |= expect_u64("riff end measure bytes", effective, 8U);
    rc |= expect_int("riff end measure mismatch", mismatch, 0);
    dsd_iq_replay_config_clear(&cfg);
    rc |= run_iq_info("riff_end.wav", NULL);
    rc |= expect_contains("riff end info data", g_info_out,
                          "  Data bytes:          4096 (header), 4 (whole frames in the file)\n");
    rc |= expect_contains("riff end info warn", g_info_err,
                          "WARNING: the WAV data chunk declares 4096 bytes but its RIFF container ends 4 bytes into "
                          "it (a writer inconsistency); the data stops there\n");
    rc |= expect_true("riff end info not truncated", strstr(g_info_err, "truncated") == NULL);

    /* RF64: ds64 says 10 frames of data, its riffSize ends the container 5 frames in. */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_id(&b, "RF64");
    wb_u32(&b, 0xFFFFFFFFU);
    wb_id(&b, "WAVE");
    wb_id(&b, "ds64");
    wb_u32(&b, 28);
    wb_u64(&b, 4U + 36U + 24U + 8U + 20U); /* riffSize: WAVE, ds64, fmt, data header, 5 frames */
    wb_u64(&b, 40);                        /* dataSize: 10 frames */
    wb_u64(&b, 10);
    wb_u32(&b, 0);
    wb_fmt(&b, 1, 2, 96000, 16);
    wb_data_header(&b, 0xFFFFFFFFU);
    wb_s16_frames(&b, 10, 0);
    rc |= expect_int("rf64 end write", write_wav("rf64_end.wav", &b), 0);
    wb_free(&b);
    rc |= expect_int("rf64 end info", dsd_iq_wav_read_info(tpath("rf64_end.wav"), &info, err, sizeof(err)), DSD_IQ_OK);
    rc |= expect_u64("rf64 end declared", info.declared_data_bytes, 40U);
    rc |= expect_u64("rf64 end frames", info.frames, 5U);
    rc |= expect_int("rf64 end clamped", info.container_clamped, 1);
    rc |= expect_int("rf64 end not truncated", info.truncated, 0);
    float* rf64_f = replay_floats("rf64_end.wav", 4096U, &n, NULL);
    if (!rf64_f) {
        return 1;
    }
    rc |= expect_u64("rf64 end replayed floats", n, 10U);
    rc |= check_s16_ramp("rf64 end samples", rf64_f, n, 0);
    free(rf64_f);
    rc |= run_iq_info("rf64_end.wav", NULL);
    rc |= expect_contains("rf64 end info warn", g_info_err,
                          "WARNING: the WAV data chunk declares 40 bytes but its RF64 container ends 20 bytes into it "
                          "(a writer inconsistency); the data stops there\n");

    /* A container past the end of the file clamps nothing: the file ends first (a truncated recording). */
    DSD_MEMSET(&b, 0, sizeof(b));
    wb_riff_header(&b);
    wb_fmt(&b, 1, 2, 48000, 16);
    wb_data_header(&b, 400U);
    wb_s16_frames(&b, 100U, 0);
    wb_finish_riff(&b);
    rc |= expect_int("riff past eof write", write_file(tpath("riff_past_eof.wav"), b.p, b.len - 200U), 0);
    wb_free(&b);
    rc |= expect_int("riff past eof info", dsd_iq_wav_read_info(tpath("riff_past_eof.wav"), &info, err, sizeof(err)),
                     DSD_IQ_OK);
    rc |= expect_u64("riff past eof frames", info.frames, 50U);
    rc |= expect_int("riff past eof not clamped", info.container_clamped, 0);
    rc |= expect_int("riff past eof truncated", info.truncated, 1);
    return rc;
}

static int
test_sniff(void) {
    int rc = 0;
    rc |= expect_int("sniff RIFF", dsd_iq_wav_sniff("RIFF\0\0\0\0WAVE", 12), DSD_IQ_WAV_SNIFF_WAV);
    rc |= expect_int("sniff RF64", dsd_iq_wav_sniff("RF64\0\0\0\0WAVE", 12), DSD_IQ_WAV_SNIFF_WAV);
    rc |= expect_int("sniff BW64", dsd_iq_wav_sniff("BW64\0\0\0\0WAVE", 12), DSD_IQ_WAV_SNIFF_WAV);
    rc |= expect_int("sniff RIFX", dsd_iq_wav_sniff("RIFX\0\0\0\0WAVE", 12), DSD_IQ_WAV_SNIFF_RIFX);
    rc |= expect_int("sniff AVI", dsd_iq_wav_sniff("RIFF\0\0\0\0AVI ", 12), DSD_IQ_WAV_SNIFF_NOT_WAV);
    rc |= expect_int("sniff short", dsd_iq_wav_sniff("RIFF\0\0\0\0WAV", 11), DSD_IQ_WAV_SNIFF_NOT_WAV);
    rc |= expect_int("sniff NULL", dsd_iq_wav_sniff(NULL, 12), DSD_IQ_WAV_SNIFF_NOT_WAV);
    return rc;
}

int
main(void) {
    if (!dsd_test_mkdtemp(g_dir, sizeof(g_dir), "dsdneo_iq_wav")) {
        DSD_FPRINTF(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    int rc = 0;
    rc |= test_sniff();
    rc |= test_every_encoding();
    rc |= test_f64_extremes();
    rc |= test_rf64_bw64();
    rc |= test_unfinalized_riff();
    rc |= test_reporter_layout();
    rc |= test_pad_bytes();
    rc |= test_fmt_after_data_and_partial_frame();
    rc |= test_rejects();
    rc |= test_small_reads_and_seek();
    rc |= test_sidecar_seek();
    rc |= test_truncation();
    rc |= test_names();
    rc |= test_center_and_start_sources();
    rc |= test_sidecar_beside_wav();
    rc |= test_iq_info();
    rc |= test_container_end();
    rc |= test_large_unfinalized();

    const char* files[sizeof(g_made) / sizeof(g_made[0]) + 1U];
    for (size_t i = 0; i < g_made_count; i++) {
        files[i] = g_made[i];
    }
    files[g_made_count] = NULL;
    if (dsd_test_remove_temp_dir(g_dir, files) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove %s: %s\n", g_dir, strerror(errno));
        rc = 1;
    }
    if (rc == 0) {
        DSD_FPRINTF(stdout, "IO_IQ_WAV: OK\n");
    }
    return rc;
}
