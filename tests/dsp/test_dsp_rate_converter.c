// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The streaming converter that takes analog output to the 48 kHz sinks (issue #633): sample counts, pitch, identity at
   48 kHz, block-cut invariance, clears, the ratio approximation and its bounds, the push and pull APIs at the extremes
   of the supported range, and the decimation filter. */

#include <dsd-neo/dsp/rate_converter.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define OUT_HZ 48000

static const double kPi = 3.14159265358979323846;

static uint32_t g_rng = 0x12345678u;

/* Bit-for-bit equality of two sample runs: the converter promises the same bits, so no tolerance applies. */
static int
same_bits(const float* a, const float* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t x = 0U;
        uint32_t y = 0U;
        DSD_MEMCPY(&x, &a[i], sizeof(x));
        DSD_MEMCPY(&y, &b[i], sizeof(y));
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

static uint32_t
rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static float*
make_tone(size_t n, int rate_hz, double freq_hz, double amplitude) {
    float* buf = (float*)malloc(n * sizeof(float));
    if (!buf) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = (float)(amplitude * sin(2.0 * kPi * freq_hz * (double)i / (double)rate_hz));
    }
    return buf;
}

/* Converts @p in through a fresh converter in chunks no longer than @p max_chunk (0: one call); returns the outputs. */
static size_t
convert_chunks(dsd_rate_converter* c, const float* in, size_t n, size_t max_chunk, float* out, size_t out_cap) {
    size_t pos = 0;
    size_t written = 0;
    while (pos < n) {
        size_t chunk = n - pos;
        if (max_chunk > 0) {
            size_t want = 1 + (size_t)(rng_next() % (uint32_t)max_chunk);
            if (want < chunk) {
                chunk = want;
            }
        }
        size_t consumed = 0;
        int got = dsd_rate_converter_process(c, in + pos, chunk, out + written, out_cap - written, &consumed);
        if (got < 0 || consumed == 0) {
            return (size_t)-1;
        }
        pos += consumed;
        written += (size_t)got;
    }
    return written;
}

/* The outputs a stream of n inputs makes from phase 0: floor((n * L + M - 1) / M). */
static size_t
expected_outputs(size_t n, int L, int M) {
    return (size_t)(((uint64_t)n * (uint64_t)L + (uint64_t)M - 1U) / (uint64_t)M);
}

/* Frequency by rising zero crossings over buf[skip..n). */
static double
zero_cross_freq(const float* buf, size_t n, size_t skip, int rate_hz) {
    size_t first = 0;
    size_t last = 0;
    size_t count = 0;
    for (size_t i = skip + 1; i < n; i++) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
            if (count == 0) {
                first = i;
            }
            last = i;
            count++;
        }
    }
    if (count < 2) {
        return 0.0;
    }
    return (double)(count - 1) * (double)rate_hz / (double)(last - first);
}

static double
rms(const float* buf, size_t from, size_t to) {
    double acc = 0.0;
    for (size_t i = from; i < to; i++) {
        acc += (double)buf[i] * (double)buf[i];
    }
    return to > from ? sqrt(acc / (double)(to - from)) : 0.0;
}

static void
test_sample_counts_and_pitch(void) {
    static const int rates[] = {44100, 32000, 22050, 11025, 96000, 192000, 4000, 2500, 1000};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        const int in_hz = rates[r];
        const size_t n = (size_t)in_hz * 10U;
        float* in = make_tone(n, in_hz, in_hz >= 4000 ? 1000.0 : 300.0, 8000.0);
        const size_t out_cap = (size_t)OUT_HZ * 10U + 64U;
        float* out = (float*)malloc(out_cap * sizeof(float));
        CHECK(in && out, "allocation");
        if (!in || !out) {
            free(in);
            free(out);
            return;
        }
        dsd_rate_converter c;
        dsd_rate_converter_init(&c);
        CHECK(dsd_rate_converter_configure(&c, in_hz, OUT_HZ) == DSD_RATE_CONVERTER_CONVERTING, "%d Hz converts",
              in_hz);
        const size_t got = convert_chunks(&c, in, n, 997U, out, out_cap);
        CHECK(got == expected_outputs(n, c.L, c.M), "%d Hz: %zu outputs, want %zu", in_hz, got,
              expected_outputs(n, c.L, c.M));
        /* Ten seconds in are ten seconds out, give or take a sample. */
        CHECK(got + 1U >= (size_t)OUT_HZ * 10U && got <= (size_t)OUT_HZ * 10U + 1U, "%d Hz: %zu outputs for 10 s",
              in_hz, got);
        if (in_hz >= 4000 && got != (size_t)-1) {
            const double f = zero_cross_freq(out, got, (size_t)OUT_HZ / 10U, OUT_HZ);
            CHECK(fabs(f - 1000.0) <= 1.0, "%d Hz: tone reads %.3f Hz, want 1000", in_hz, f);
        }
        dsd_rate_converter_free(&c);
        free(in);
        free(out);
    }
    int L = 0;
    int M = 0;
    CHECK(dsd_rate_converter_ratio(800, OUT_HZ, &L, &M) == DSD_RATE_CONVERTER_UNSUPPORTED && L == 1 && M == 1,
          "800 Hz is unsupported");
}

static void
test_identity_at_48k(void) {
    const size_t n = 4800;
    float* in = make_tone(n, OUT_HZ, 1234.0, 12000.0);
    float out[4800];
    dsd_rate_converter c;
    dsd_rate_converter_init(&c);
    CHECK(dsd_rate_converter_configure(&c, OUT_HZ, OUT_HZ) == DSD_RATE_CONVERTER_IDENTITY, "48k is identity");
    CHECK(dsd_rate_converter_output_hz(&c) == OUT_HZ, "identity output rate");
    CHECK(dsd_rate_converter_max_out_per_input(&c) == 1, "identity makes one output per input");
    CHECK(c.rs.taps == NULL, "identity allocates no filter");
    size_t consumed = 0;
    CHECK(in && dsd_rate_converter_process(&c, in, n, out, n, &consumed) == (int)n && consumed == n, "identity count");
    CHECK(in && same_bits(in, out, n), "identity is bit-identical");
    dsd_rate_converter_free(&c);
    free(in);
}

static void
test_block_cut_invariance(void) {
    static const int rates[] = {44100, 96000, 11025};
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        const int in_hz = rates[r];
        const size_t n = (size_t)in_hz * 2U;
        float* in = make_tone(n, in_hz, 777.0, 9000.0);
        const size_t out_cap = (size_t)OUT_HZ * 2U + 64U;
        float* a = (float*)malloc(out_cap * sizeof(float));
        float* b = (float*)malloc(out_cap * sizeof(float));
        if (!in || !a || !b) {
            CHECK(0, "allocation");
            free(in);
            free(a);
            free(b);
            return;
        }
        dsd_rate_converter c1;
        dsd_rate_converter c2;
        dsd_rate_converter_init(&c1);
        dsd_rate_converter_init(&c2);
        (void)dsd_rate_converter_configure(&c1, in_hz, OUT_HZ);
        (void)dsd_rate_converter_configure(&c2, in_hz, OUT_HZ);
        const size_t na = convert_chunks(&c1, in, n, 0U, a, out_cap);
        const size_t nb = convert_chunks(&c2, in, n, 997U, b, out_cap);
        CHECK(na == nb && na != (size_t)-1 && same_bits(a, b, na), "%d Hz: block cuts change the output", in_hz);
        dsd_rate_converter_free(&c1);
        dsd_rate_converter_free(&c2);
        free(in);
        free(a);
        free(b);
    }
}

static void
test_clear_and_rate_change(void) {
    const size_t n = 4410;
    float* in = make_tone(n, 44100, 500.0, 7000.0);
    float a[8000];
    float b[8000];
    dsd_rate_converter c;
    dsd_rate_converter fresh;
    dsd_rate_converter_init(&c);
    dsd_rate_converter_init(&fresh);
    if (!in) {
        CHECK(0, "allocation");
        return;
    }
    (void)dsd_rate_converter_configure(&c, 44100, OUT_HZ);
    (void)convert_chunks(&c, in, n, 0U, a, 8000U);
    dsd_rate_converter_clear(&c);
    (void)dsd_rate_converter_configure(&fresh, 44100, OUT_HZ);
    size_t na = convert_chunks(&c, in, n, 0U, a, 8000U);
    size_t nb = convert_chunks(&fresh, in, n, 0U, b, 8000U);
    CHECK(na == nb && na != (size_t)-1 && same_bits(a, b, na), "clear() equals a fresh converter");

    /* The same rates keep the converter's state; other rates design it anew from silence. */
    const int phase_before = c.rs.phase;
    const int head_before = c.rs.hist_head;
    const float* taps_before = c.rs.taps;
    CHECK(dsd_rate_converter_configure(&c, 44100, OUT_HZ) == DSD_RATE_CONVERTER_CONVERTING && c.rs.phase == phase_before
              && c.rs.hist_head == head_before && c.rs.taps == taps_before,
          "the same rates keep the state");
    CHECK(dsd_rate_converter_configure(&c, 32000, OUT_HZ) == DSD_RATE_CONVERTER_CONVERTING && c.L == 3 && c.M == 2,
          "32 kHz is 3/2");
    dsd_rate_converter_free(&fresh);
    dsd_rate_converter_init(&fresh);
    (void)dsd_rate_converter_configure(&fresh, 32000, OUT_HZ);
    na = convert_chunks(&c, in, n, 0U, a, 8000U);
    nb = convert_chunks(&fresh, in, n, 0U, b, 8000U);
    CHECK(na == nb && na != (size_t)-1 && same_bits(a, b, na), "a rate change starts from silence");
    dsd_rate_converter_free(&c);
    dsd_rate_converter_free(&fresh);
    free(in);
}

static double
ratio_error_ppm(int in_hz, int L, int M) {
    const double want = (double)OUT_HZ / (double)in_hz;
    const double got = (double)L / (double)M;
    return fabs(got - want) / want * 1e6;
}

static void
test_ratio_approximation(void) {
    struct {
        int in_hz;
        int L;
        int M;
    } exact[] = {{44100, 160, 147}, {22050, 320, 147}, {11025, 640, 147},   {32000, 3, 2}, {96000, 1, 2},
                 {192000, 1, 4},    {1000, 48, 1},     {48024, 2000, 2001}, {2500, 96, 5}, {768000, 1, 16}};

    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        int L = 0;
        int M = 0;
        CHECK(dsd_rate_converter_ratio(exact[i].in_hz, OUT_HZ, &L, &M) == DSD_RATE_CONVERTER_CONVERTING
                  && L == exact[i].L && M == exact[i].M,
              "%d Hz: L/M %d/%d, want %d/%d", exact[i].in_hz, L, M, exact[i].L, exact[i].M);
    }
    static const int approximated[] = {44056, 19531, 48010};
    for (size_t i = 0; i < sizeof(approximated) / sizeof(approximated[0]); i++) {
        int L = 0;
        int M = 0;
        CHECK(dsd_rate_converter_ratio(approximated[i], OUT_HZ, &L, &M) == DSD_RATE_CONVERTER_CONVERTING
                  && L <= DSD_RATE_CONVERTER_MAX_TERM && M <= DSD_RATE_CONVERTER_MAX_TERM
                  && ratio_error_ppm(approximated[i], L, M) <= (double)DSD_RATE_CONVERTER_TOLERANCE_PPM,
              "%d Hz: L/M %d/%d, %.1f ppm", approximated[i], L, M, ratio_error_ppm(approximated[i], L, M));
    }
    int L = 0;
    int M = 0;
    CHECK(dsd_rate_converter_ratio(48001, OUT_HZ, &L, &M) == DSD_RATE_CONVERTER_IDENTITY && L == 1 && M == 1,
          "48001 Hz is within tolerance of 48 kHz: identity");
    CHECK(dsd_rate_converter_ratio(999, OUT_HZ, NULL, NULL) == DSD_RATE_CONVERTER_UNSUPPORTED, "999 Hz unsupported");
    CHECK(dsd_rate_converter_ratio(768001, OUT_HZ, NULL, NULL) == DSD_RATE_CONVERTER_UNSUPPORTED,
          "768001 Hz unsupported");
    CHECK(dsd_rate_converter_ratio(0, OUT_HZ, NULL, NULL) == DSD_RATE_CONVERTER_UNSUPPORTED, "0 Hz unsupported");
    CHECK(dsd_rate_converter_ratio(44100, 0, NULL, NULL) == DSD_RATE_CONVERTER_UNSUPPORTED, "0 Hz out unsupported");

    /* No holes: every rate in range converts within tolerance, on a coarse sweep and at random. */
    int worst_rate = 0;
    double worst_ppm = 0.0;
    for (int64_t step = 0; step < 2000 + (768000 - 1000) / 97 + 1; step++) {
        const int in_hz = step < 2000 ? 1000 + (int)(rng_next() % 767001u) : 1000 + (int)(step - 2000) * 97;
        if (in_hz > 768000) {
            continue;
        }
        const int mode = dsd_rate_converter_ratio(in_hz, OUT_HZ, &L, &M);
        const double ppm = ratio_error_ppm(in_hz, L, M);
        if (mode == DSD_RATE_CONVERTER_UNSUPPORTED || L > DSD_RATE_CONVERTER_MAX_TERM || M > DSD_RATE_CONVERTER_MAX_TERM
            || ppm > (double)DSD_RATE_CONVERTER_TOLERANCE_PPM) {
            CHECK(0, "%d Hz: mode %d L/M %d/%d %.1f ppm", in_hz, mode, L, M, ppm);
            break;
        }
        if (ppm > worst_ppm) {
            worst_ppm = ppm;
            worst_rate = in_hz;
        }
    }
    DSD_FPRINTF(stderr, "ratio sweep: worst %.1f ppm at %d Hz\n", worst_ppm, worst_rate);
}

typedef struct {
    const float* in;
    size_t n;
    size_t pos;
} reader_ctx;

/* The tag read_next() gives input @p pos. */
static uint8_t
input_tag(size_t pos) {
    return (uint8_t)(1U + (pos % 250U));
}

static int
read_next(void* ctx, float* sample, uint8_t* tag) {
    reader_ctx* r = (reader_ctx*)ctx;
    if (r->pos >= r->n) {
        return 0;
    }
    *tag = input_tag(r->pos);
    *sample = r->in[r->pos++];
    return 1;
}

/* fill() in sizes of @p sizes (cycled) equals process() over the same inputs, bit for bit. */
static void
check_fill_matches_process(int in_hz, const size_t* sizes, size_t nsizes, size_t total_out) {
    const size_t n = (size_t)in_hz;
    float* in = make_tone(n, in_hz, in_hz >= 4000 ? 1000.0 : 200.0, 5000.0);
    float* want = (float*)malloc(((size_t)OUT_HZ + 64U) * sizeof(float));
    float* got = (float*)malloc(((size_t)OUT_HZ + 64U) * sizeof(float));
    if (!in || !want || !got) {
        CHECK(0, "allocation");
        free(in);
        free(want);
        free(got);
        return;
    }
    dsd_rate_converter p;
    dsd_rate_converter f;
    dsd_rate_converter_init(&p);
    dsd_rate_converter_init(&f);
    (void)dsd_rate_converter_configure(&p, in_hz, OUT_HZ);
    (void)dsd_rate_converter_configure(&f, in_hz, OUT_HZ);
    const size_t nwant = convert_chunks(&p, in, n, 0U, want, (size_t)OUT_HZ + 64U);
    reader_ctx ctx = {in, n, 0U};
    size_t done = 0;
    size_t k = 0;
    while (done < total_out) {
        size_t size = sizes[k++ % nsizes];
        if (size > total_out - done) {
            size = total_out - done;
        }
        CHECK(dsd_rate_converter_fill(&f, got + done, NULL, size, read_next, &ctx) == (int)size, "%d Hz: fill(%zu)",
              in_hz, size);
        done += size;
    }
    CHECK(nwant >= total_out && same_bits(want, got, total_out), "%d Hz: fill differs from process", in_hz);
    dsd_rate_converter_free(&p);
    dsd_rate_converter_free(&f);
    free(in);
    free(want);
    free(got);
}

/* fill() in sizes of @p sizes (cycled) tags each output with the newest input it was made from: input i's tag for the
   outputs that pushing input i alone makes (process() one input at a time), across every carry boundary. */
static void
check_fill_tags(int in_hz, const size_t* sizes, size_t nsizes, size_t total_out) {
    const size_t n = (size_t)in_hz;
    float* in = make_tone(n, in_hz, in_hz >= 4000 ? 1000.0 : 200.0, 5000.0);
    uint8_t* want = (uint8_t*)malloc((size_t)OUT_HZ + 64U);
    uint8_t* tags = (uint8_t*)malloc((size_t)OUT_HZ + 64U);
    float* got = (float*)malloc(((size_t)OUT_HZ + 64U) * sizeof(float));
    if (!in || !want || !tags || !got) {
        CHECK(0, "allocation");
        free(in);
        free(want);
        free(tags);
        free(got);
        return;
    }
    dsd_rate_converter p;
    dsd_rate_converter f;
    dsd_rate_converter_init(&p);
    dsd_rate_converter_init(&f);
    (void)dsd_rate_converter_configure(&p, in_hz, OUT_HZ);
    (void)dsd_rate_converter_configure(&f, in_hz, OUT_HZ);
    size_t nwant = 0;
    for (size_t i = 0; i < n && nwant < total_out; i++) {
        float one[DSD_RATE_CONVERTER_CARRY];
        size_t consumed = 0;
        const int made = dsd_rate_converter_process(&p, in + i, 1U, one, DSD_RATE_CONVERTER_CARRY, &consumed);
        CHECK(made >= 0 && consumed == 1U, "%d Hz: push input %zu", in_hz, i);
        for (int k = 0; k < made && nwant < (size_t)OUT_HZ + 64U; k++) {
            want[nwant++] = input_tag(i);
        }
    }
    reader_ctx ctx = {in, n, 0U};
    size_t done = 0;
    size_t k = 0;
    while (done < total_out) {
        size_t size = sizes[k++ % nsizes];
        if (size > total_out - done) {
            size = total_out - done;
        }
        CHECK(dsd_rate_converter_fill(&f, got + done, tags + done, size, read_next, &ctx) == (int)size,
              "%d Hz: fill(%zu)", in_hz, size);
        done += size;
    }
    CHECK(nwant >= total_out && memcmp(want, tags, total_out) == 0, "%d Hz: tags differ from the inputs made from",
          in_hz);
    dsd_rate_converter_free(&p);
    dsd_rate_converter_free(&f);
    free(in);
    free(want);
    free(tags);
    free(got);
}

static void
test_fill_tags_follow_their_inputs(void) {
    static const size_t odd[] = {7, 13, 960, 1};
    static const size_t tiny[] = {1, 2, 47};
    check_fill_tags(48000, odd, 4, 40000U);  /* identity: one tag per input */
    check_fill_tags(44100, odd, 4, 40000U);  /* 160/147 */
    check_fill_tags(24000, odd, 4, 40000U);  /* two outputs per input, as EDACS reads a 24 kHz RTL stream */
    check_fill_tags(1000, tiny, 3, 40000U);  /* 48 outputs per input, carried across calls */
    check_fill_tags(192000, odd, 4, 40000U); /* most inputs make none */
}

static void
test_pull_and_push_contracts(void) {
    static const size_t odd[] = {7, 13, 960, 1};
    check_fill_matches_process(44100, odd, 4, 40000U);
    static const size_t tiny[] = {1, 2, 47};
    check_fill_matches_process(1000, tiny, 3, 40000U);

    /* Three triplet blocks of 960 at 44.1 kHz read exactly 2646 inputs. */
    const size_t n = 4000;
    float* in = make_tone(n, 44100, 1000.0, 1000.0);
    float out[960];
    dsd_rate_converter c;
    dsd_rate_converter_init(&c);
    (void)dsd_rate_converter_configure(&c, 44100, OUT_HZ);
    reader_ctx ctx = {in, n, 0U};
    for (int i = 0; i < 3 && in; i++) {
        CHECK(dsd_rate_converter_fill(&c, out, NULL, 960U, read_next, &ctx) == 960, "fill 960");
    }
    CHECK(ctx.pos == 2646U, "three fills of 960 read %zu inputs, want 2646", ctx.pos);
    ctx.n = ctx.pos;
    CHECK(dsd_rate_converter_fill(&c, out, NULL, 960U, read_next, &ctx) == -1, "fill reports the end of input");
    dsd_rate_converter_free(&c);

    /* At 1 kHz one input makes 48 outputs: a push with room for fewer is refused rather than stalling. */
    dsd_rate_converter_init(&c);
    (void)dsd_rate_converter_configure(&c, 1000, OUT_HZ);
    CHECK(dsd_rate_converter_max_out_per_input(&c) == 48, "1 kHz makes 48 outputs per input");
    float big[96];
    size_t consumed = 99;
    CHECK(in && dsd_rate_converter_process(&c, in, 10U, big, 47U, &consumed) == -1 && consumed == 0,
          "a push with 47 outputs of room is refused");
    CHECK(in && dsd_rate_converter_process(&c, in, 10U, big, 48U, &consumed) == 48 && consumed == 1,
          "a push with 48 outputs of room takes one input");
    dsd_rate_converter_free(&c);
    CHECK(dsd_rate_converter_process(&c, in, 10U, big, 96U, &consumed) == -1, "an unconfigured converter refuses");
    free(in);
}

/* A tone above the output's Nyquist leaves @p in_hz at least 40 dB down: the taps scale with the decimation. */
static void
check_decimation_rejects(int in_hz, int want_taps) {
    const size_t n = (size_t)in_hz / 5U;
    float* in = make_tone(n, in_hz, 40000.0, 1.0);
    float* out = (float*)malloc(((size_t)OUT_HZ / 5U + 64U) * sizeof(float));
    if (!in || !out) {
        CHECK(0, "allocation");
        free(in);
        free(out);
        return;
    }
    dsd_rate_converter c;
    dsd_rate_converter_init(&c);
    (void)dsd_rate_converter_configure(&c, in_hz, OUT_HZ);
    CHECK(c.rs.taps_per_phase == want_taps, "%d Hz: %d taps per phase, want %d", in_hz, c.rs.taps_per_phase, want_taps);
    const size_t got = convert_chunks(&c, in, n, 0U, out, (size_t)OUT_HZ / 5U + 64U);
    const double level = rms(out, 1000U, got) / sqrt(0.5);
    const double db = 20.0 * log10(level > 1e-12 ? level : 1e-12);
    CHECK(db <= -40.0, "%d Hz: a 40 kHz tone leaves at %.1f dB", in_hz, db);
    dsd_rate_converter_free(&c);
    free(in);
    free(out);
}

#if defined(DSD_NEO_TEST_ALIGNED_MALLOC_WRAP)
/* GNU ld --wrap seam: fails the filter's allocation on demand. */
static int g_fail_aligned_malloc = 0;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void* __real_dsd_neo_aligned_malloc(size_t size);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void* __wrap_dsd_neo_aligned_malloc(size_t size);

void*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_neo_aligned_malloc(size_t size) {
    if (g_fail_aligned_malloc) {
        return NULL;
    }
    return __real_dsd_neo_aligned_malloc(size);
}

/* A filter that cannot be allocated leaves the converter FAILED, refusing to process rather than pass samples through
   at the wrong rate; the same rates try the allocation again. */
static void
test_design_failure_is_retried(void) {
    dsd_rate_converter c;
    dsd_rate_converter_init(&c);
    g_fail_aligned_malloc = 1;
    CHECK(dsd_rate_converter_configure(&c, 44100, OUT_HZ) == DSD_RATE_CONVERTER_FAILED && c.rs.taps == NULL,
          "a failed allocation leaves the converter FAILED");
    float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float out[16];
    size_t consumed = 0;
    CHECK(dsd_rate_converter_process(&c, in, 4U, out, 16U, &consumed) == -1 && consumed == 0,
          "a failed converter refuses to process");
    g_fail_aligned_malloc = 0;
    CHECK(dsd_rate_converter_configure(&c, 44100, OUT_HZ) == DSD_RATE_CONVERTER_CONVERTING && c.rs.taps != NULL,
          "the same rates try the allocation again");
    dsd_rate_converter_free(&c);
}
#endif

int
main(void) {
    test_sample_counts_and_pitch();
    test_identity_at_48k();
    test_block_cut_invariance();
    test_clear_and_rate_change();
    test_ratio_approximation();
    test_pull_and_push_contracts();
    test_fill_tags_follow_their_inputs();
    check_decimation_rejects(96000, 32);
    check_decimation_rejects(192000, 64);
    check_decimation_rejects(768000, 256);
#if defined(DSD_NEO_TEST_ALIGNED_MALLOC_WRAP)
    test_design_failure_is_retried();
#endif
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
